#include "image_writer.h"

#if defined(_WIN32)

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "cml/proc/address_space.h"
#include "cml/proc/loader_diag_ids.h"
#include "cml/util/log.h"

namespace cml::agent::loader {

namespace {

constexpr DWORD kWritableMask = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
constexpr DWORD kExecutableMask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

std::string hex(std::uint64_t value) {
    static const char kDigits[] = "0123456789abcdef";
    std::string s;
    do {
        s.insert(s.begin(), kDigits[value & 0xF]);
        value >>= 4;
    } while (value != 0);
    return "0x" + s;
}

Diag invalid(std::uint32_t id, std::string message) {
    return Diag::make(id, std::move(message),
                      "Report this with the agent log; the patch is unloaded and the program keeps running the old code.");
}

// One VirtualQuery result, cached so that consecutive fix-ups in the same section cost nothing.
struct Region {
    std::uint64_t base = 0;
    std::uint64_t end = 0;
    DWORD protect = 0;
    DWORD state = 0;
    DWORD type = 0;
    bool valid = false;

    bool contains(std::uint64_t address) const noexcept { return valid && address >= base && address < end; }
    bool usable() const noexcept {
        return valid && state == MEM_COMMIT && type == MEM_IMAGE && protect != 0 &&
               (protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
    }
};

bool query_region(std::uint64_t address, Region* out) noexcept {
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0) {
        out->valid = false;
        return false;
    }
    out->base = reinterpret_cast<std::uint64_t>(info.BaseAddress);
    out->end = out->base + info.RegionSize;
    out->protect = info.Protect;
    out->state = info.State;
    out->type = info.Type;
    out->valid = true;
    return true;
}

class RegionCache {
public:
    const Region& find(std::uint64_t address) noexcept {
        if (!current_.contains(address)) {
            query_region(address, &current_);
        }
        return current_;
    }

private:
    Region current_;
};

std::uint32_t site_size(const ipc::Fixup& fixup) noexcept {
    return fixup.kind == ipc::FixupKind::Rel32 ? 4u : 8u;
}

// A site inside the image, in one committed image region.
Result<void> check_site(RegionCache& cache, const ImageRef& image, std::size_t index, const ipc::Fixup& fixup) {
    const std::uint32_t length = site_size(fixup);
    if (fixup.rva > image.size || length > image.size - fixup.rva) {
        return invalid(ids::kLoaderFixupInvalid, "fix-up " + std::to_string(index) + " at RVA " + hex(fixup.rva) +
                                                     " lies outside the patch image (size " + hex(image.size) + ")");
    }
    if (fixup.kind == ipc::FixupKind::SlotWrite && fixup.rva % 8 != 0) {
        return invalid(ids::kLoaderFixupInvalid, "thunk slot " + std::to_string(index) + " at RVA " + hex(fixup.rva) +
                                                     " is not 8-byte aligned");
    }
    const std::uint64_t site = image.base + fixup.rva;
    const Region& region = cache.find(site);
    if (!region.usable() || site + length > region.end) {
        return invalid(ids::kLoaderFixupInvalid, "fix-up " + std::to_string(index) + " at RVA " + hex(fixup.rva) +
                                                     " is not inside one committed, accessible image section");
    }
    return {};
}

// Value to store for a fix-up; false (and a diagnostic) when a REL32 does not fit.
Result<void> compute(const ImageRef& image, std::size_t index, const ipc::Fixup& fixup, std::uint64_t* out) {
    if (fixup.kind != ipc::FixupKind::Rel32) {
        *out = fixup.value;
        return {};
    }
    const std::uint64_t site = image.base + fixup.rva;
    std::int32_t displacement = 0;
    if (!proc::rel32_displacement(site, fixup.n, fixup.value, &displacement)) {
        const std::uint64_t next = site + 4 + fixup.n;
        const std::uint64_t distance = fixup.value >= next ? fixup.value - next : next - fixup.value;
        return Diag::make(ids::kLoaderRel32OutOfRange,
                          "REL32 fix-up " + std::to_string(index) + " at RVA " + hex(fixup.rva) + " (site " + hex(site) +
                              ") cannot reach target " + hex(fixup.value) + ": distance " + hex(distance) +
                              " does not fit 32 bits",
                          "The patch was placed too far from the code it refers to; reserve address space nearer the module "
                          "(or reload) so the patch lands within 2 GiB.");
    }
    *out = static_cast<std::uint64_t>(static_cast<std::uint32_t>(displacement));
    return {};
}

// Writes bytes into image memory, lifting the protection of one region at a time and restoring it.
class ProtectedWriter {
public:
    ProtectedWriter() = default;
    ~ProtectedWriter() { static_cast<void>(finish()); }
    ProtectedWriter(const ProtectedWriter&) = delete;
    ProtectedWriter& operator=(const ProtectedWriter&) = delete;

    Result<void> write(std::uint64_t address, const void* data, std::uint32_t length, bool atomic_slot) {
        if (!(open_ && address >= lo_ && address + length <= hi_)) {
            Result<void> closed = finish();
            if (!closed) {
                return closed;
            }
            Region region;
            if (!query_region(address, &region) || !region.usable() || address + length > region.end) {
                return invalid(ids::kLoaderFixupInvalid, "cannot write at " + hex(address) + ": memory is not accessible");
            }
            if ((region.protect & kWritableMask) != 0) {
                store(address, data, length, atomic_slot);
                if ((region.protect & kExecutableMask) != 0) {
                    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), length);
                }
                return {};
            }
            const DWORD modifiers = region.protect & ~static_cast<DWORD>(0xFF);
            const DWORD executable = region.protect & kExecutableMask;
            const DWORD wanted = (executable != 0 ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE) | modifiers;
            DWORD previous = 0;
            if (!VirtualProtect(reinterpret_cast<LPVOID>(region.base), static_cast<SIZE_T>(region.end - region.base),
                                wanted, &previous)) {
                return invalid(ids::kLoaderFixupInvalid, "VirtualProtect failed for " + hex(region.base) + " (error " +
                                                             std::to_string(GetLastError()) + ")");
            }
            open_ = true;
            lo_ = region.base;
            hi_ = region.end;
            previous_ = previous;
        }
        store(address, data, length, atomic_slot);
        return {};
    }

    // Restores the protection of the open region (and flushes the instruction cache if it was code).
    Result<void> finish() {
        if (!open_) {
            return {};
        }
        open_ = false;
        DWORD ignored = 0;
        const bool restored = VirtualProtect(reinterpret_cast<LPVOID>(lo_), static_cast<SIZE_T>(hi_ - lo_), previous_,
                                             &ignored) != 0;
        const DWORD error = restored ? 0 : GetLastError();
        if ((previous_ & kExecutableMask) != 0) {
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(lo_), static_cast<SIZE_T>(hi_ - lo_));
        }
        if (!restored) {
            return invalid(ids::kLoaderFixupInvalid, "could not restore the page protection of " + hex(lo_) +
                                                         " (error " + std::to_string(error) + ")");
        }
        return {};
    }

private:
    static void store(std::uint64_t address, const void* data, std::uint32_t length, bool atomic_slot) noexcept {
        if (atomic_slot) {
            std::uint64_t value = 0;
            std::memcpy(&value, data, sizeof(value));
            InterlockedExchange64(reinterpret_cast<volatile LONG64*>(address), static_cast<LONG64>(value));
        } else {
            std::memcpy(reinterpret_cast<void*>(address), data, length);
        }
    }

    bool open_ = false;
    std::uint64_t lo_ = 0;
    std::uint64_t hi_ = 0;
    DWORD previous_ = 0;
};

Result<std::uint32_t> write_pass(const ImageRef& image, std::span<const ipc::Fixup> fixups, bool slots) {
    ProtectedWriter writer;
    std::uint32_t written = 0;
    for (std::size_t i = 0; i < fixups.size(); ++i) {
        const ipc::Fixup& fixup = fixups[i];
        if ((fixup.kind == ipc::FixupKind::SlotWrite) != slots) {
            continue;
        }
        std::uint64_t value = 0;
        Result<void> computed = compute(image, i, fixup, &value);
        if (!computed) {
            return computed.error();  // cannot happen after validation; kept for safety
        }
        Result<void> done = fixup.kind == ipc::FixupKind::Rel32
                                ? writer.write(image.base + fixup.rva, &value, 4, false)
                                : writer.write(image.base + fixup.rva, &value, 8, slots);
        if (!done) {
            static_cast<void>(writer.finish());
            return done.error();
        }
        ++written;
    }
    Result<void> closed = writer.finish();
    if (!closed) {
        return closed.error();
    }
    return written;
}

#if defined(_MSC_VER) || defined(__clang__)
// Runs one initializer; returns 0, or the SEH exception code if it faulted. Separate function: a
// function with __try may not hold objects that need unwinding.
DWORD call_guarded(void (*function)()) {
    DWORD code = 0;
    __try {
        function();
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return code == 0 ? 0xFFFFFFFFu : code;
    }
    return 0;
}
#endif

}  // namespace

Result<std::uint32_t> apply_fixups(const ImageRef& image, std::span<const ipc::Fixup> fixups) {
    if (image.base == 0 || image.size == 0) {
        return invalid(ids::kLoaderFixupInvalid, "fix-ups were requested for an empty image");
    }
    // Pass 0: validate everything; nothing is written unless every entry is good.
    {
        RegionCache cache;
        for (std::size_t i = 0; i < fixups.size(); ++i) {
            Result<void> site = check_site(cache, image, i, fixups[i]);
            if (!site) {
                return site.error();
            }
            std::uint64_t ignored = 0;
            Result<void> value = compute(image, i, fixups[i], &ignored);
            if (!value) {
                return value.error();
            }
        }
    }
    Result<std::uint32_t> first = write_pass(image, fixups, false);  // REL32 + ADDR64
    if (!first) {
        return first;
    }
    Result<std::uint32_t> second = write_pass(image, fixups, true);  // thunk slots
    if (!second) {
        return second;
    }
    CML_LOG(Trace, "applied %u fix-ups and %u slots at image 0x%llx", *first, *second,
            static_cast<unsigned long long>(image.base));
    return *first + *second;
}

Result<std::uint32_t> run_initializers(const ImageRef& image, std::span<const std::uint32_t> rvas) {
    if (rvas.empty()) {
        return std::uint32_t{0};
    }
    if (image.base == 0 || image.size == 0) {
        return invalid(ids::kLoaderInitializerFailed, "initializers were requested for an empty image");
    }
    // Resolve every entry first so that a bad table is refused before anything runs.
    std::vector<void (*)()> entries;
    entries.reserve(rvas.size());
    RegionCache cache;
    for (std::size_t i = 0; i < rvas.size(); ++i) {
        const std::uint32_t rva = rvas[i];
        if (rva > image.size || image.size - rva < sizeof(std::uint64_t) || rva % sizeof(std::uint64_t) != 0) {
            return invalid(ids::kLoaderInitializerFailed, "initializer " + std::to_string(i) + " at RVA " + hex(rva) +
                                                              " is outside the image or not 8-byte aligned");
        }
        const std::uint64_t entry_address = image.base + rva;
        const Region& table = cache.find(entry_address);
        if (!table.usable() || entry_address + sizeof(std::uint64_t) > table.end) {
            return invalid(ids::kLoaderInitializerFailed, "initializer table entry " + std::to_string(i) + " at RVA " +
                                                              hex(rva) + " is not readable image memory");
        }
        std::uint64_t target = 0;
        std::memcpy(&target, reinterpret_cast<const void*>(entry_address), sizeof(target));
        if (target != 0) {
            Region code;
            if (target < image.base || target - image.base >= image.size || !query_region(target, &code) ||
                !code.usable() || (code.protect & kExecutableMask) == 0) {
                return invalid(ids::kLoaderInitializerFailed,
                               "initializer " + std::to_string(i) + " (table entry at RVA " + hex(rva) + ") points to " +
                                   hex(target) + ", which is not executable code inside the patch image");
            }
        }
        entries.push_back(reinterpret_cast<void (*)()>(target));
    }
    std::uint32_t ran = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i] == nullptr) {
            continue;  // the CRT skips null entries too
        }
#if defined(_MSC_VER) || defined(__clang__)
        const DWORD fault = call_guarded(entries[i]);
#else
        entries[i]();
        const DWORD fault = 0;
#endif
        if (fault != 0) {
            return Diag::make(ids::kLoaderInitializerFailed,
                              "initializer " + std::to_string(i) + " (table entry at RVA " + hex(rvas[i]) +
                                  ") raised exception 0x" + hex(fault).substr(2) + " after " + std::to_string(ran) +
                                  " initializer(s) had run",
                              "Fix the crash in the new global's constructor; the patch is unloaded and the old code keeps running.");
        }
        ++ran;
    }
    return ran;
}

}  // namespace cml::agent::loader

#endif  // _WIN32
