#include "patch_loader.h"

#if defined(_WIN32)

#include <algorithm>
#include <limits>
#include <utility>

#include "cml/proc/diag_ids.h"
#include "cml/proc/loader_diag_ids.h"
#include "cml/proc/proc.h"
#include "cml/util/log.h"
#include "image_writer.h"

namespace cml::agent::loader {

namespace {

using proc::AddressRange;

constexpr std::uint64_t kGranule = proc::kAllocationGranularity;
constexpr std::uint64_t kMaxBlockSize = 1ull << 30;
constexpr std::uint64_t kMaxImageSize = 1ull << 30;
constexpr std::uint64_t kDefaultReach = 1536ull << 20;
constexpr std::size_t kMaxAllocationsPerRange = 8;
constexpr std::size_t kMaxRegionsPerAllocation = 4096;

std::string hex(std::uint64_t value) {
    static const char kDigits[] = "0123456789abcdef";
    std::string s;
    do {
        s.insert(s.begin(), kDigits[value & 0xF]);
        value >>= 4;
    } while (value != 0);
    return "0x" + s;
}

unsigned long long ull(std::uint64_t v) {
    return static_cast<unsigned long long>(v);
}

std::string failure_text(const Diag& diag) {
    return diag.message + " What to do: " + diag.what_to_do;
}

bool reserve_at(std::uint64_t base, std::uint64_t size) noexcept {
    void* p = VirtualAlloc(reinterpret_cast<LPVOID>(base), static_cast<SIZE_T>(size), MEM_RESERVE, PAGE_NOACCESS);
    return p == reinterpret_cast<LPVOID>(base);
}

bool query(std::uint64_t address, MEMORY_BASIC_INFORMATION* info) noexcept {
    return VirtualQuery(reinterpret_cast<LPCVOID>(address), info, sizeof(*info)) != 0;
}

// True when every page of `range` is free (MEM_FREE) right now.
bool range_is_free(const AddressRange& range) noexcept {
    std::uint64_t address = range.base;
    while (address < range.end()) {
        MEMORY_BASIC_INFORMATION info;
        if (!query(address, &info) || info.State != MEM_FREE) {
            return false;
        }
        const std::uint64_t next = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
        if (next <= address) {
            return false;
        }
        address = next;
    }
    return true;
}

Diag make(std::uint32_t id, std::string message, std::string advice) {
    return Diag::make(id, std::move(message), std::move(advice));
}

bool image_header_ok(std::uint64_t base, std::uint32_t expected_size, std::string* why) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) {
        *why = "no valid DOS header";
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<std::uint64_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        *why = "not an x64 PE32+ image";
        return false;
    }
    if (nt->OptionalHeader.SizeOfImage != expected_size) {
        *why = "SizeOfImage is " + hex(nt->OptionalHeader.SizeOfImage) + ", the request said " + hex(expected_size);
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// owned address space
// ---------------------------------------------------------------------------------------------

bool PatchLoader::owned_covers(const AddressRange& range) const {
    return std::any_of(owned_.begin(), owned_.end(), [&](const AddressRange& o) {
        return o.base <= range.base && range.end() <= o.end();
    });
}

void PatchLoader::owned_add(const AddressRange& range) {
    if (range.size == 0) {
        return;
    }
    owned_.push_back(range);
    std::sort(owned_.begin(), owned_.end(), [](const AddressRange& a, const AddressRange& b) { return a.base < b.base; });
    std::vector<AddressRange> merged;
    merged.reserve(owned_.size());
    for (const AddressRange& r : owned_) {
        if (!merged.empty() && r.base <= merged.back().end()) {
            merged.back().size = std::max(merged.back().end(), r.end()) - merged.back().base;
        } else {
            merged.push_back(r);
        }
    }
    owned_ = std::move(merged);
}

void PatchLoader::owned_remove(const AddressRange& range) {
    std::vector<AddressRange> kept;
    kept.reserve(owned_.size() + 1);
    for (const AddressRange& o : owned_) {
        if (o.end() <= range.base || range.end() <= o.base) {
            kept.push_back(o);
            continue;
        }
        if (o.base < range.base) {
            kept.push_back({o.base, range.base - o.base});
        }
        if (range.end() < o.end()) {
            kept.push_back({range.end(), o.end() - range.end()});
        }
    }
    owned_ = std::move(kept);
}

// Makes exactly `range` free in the OS and no longer owned. The allocations that contain it are
// released as a whole (VirtualFree cannot release part of a reservation); the parts of those
// allocations outside `range` are re-reserved straight away. A part that cannot be re-reserved
// (another thread was faster) is dropped from the owned set.
Result<void> PatchLoader::detach_range(const AddressRange& range) {
    struct Allocation {
        std::uint64_t base;
        std::uint64_t end;
    };
    Allocation allocations[kMaxAllocationsPerRange];
    std::size_t count = 0;

    std::uint64_t address = range.base;
    while (address < range.end()) {
        MEMORY_BASIC_INFORMATION info;
        if (!query(address, &info)) {
            return make(ids::kLoaderReleaseFailed, "VirtualQuery failed at " + hex(address) + " (error " +
                                                       std::to_string(GetLastError()) + ")",
                        "Report this with the agent log.");
        }
        if (info.State != MEM_RESERVE || info.Type != MEM_PRIVATE) {
            return make(ids::kLoaderRangeNotReserved,
                        "address " + hex(address) + " is not reserved address space (state " + hex(info.State) +
                            ", type " + hex(info.Type) + ")",
                        "Reserve a block with ReserveRange first and place the patch inside it.");
        }
        const auto allocation_base = reinterpret_cast<std::uint64_t>(info.AllocationBase);
        std::uint64_t allocation_end = allocation_base;
        std::size_t regions = 0;
        for (;;) {
            MEMORY_BASIC_INFORMATION part;
            if (!query(allocation_end, &part) || reinterpret_cast<std::uint64_t>(part.AllocationBase) != allocation_base) {
                break;
            }
            allocation_end = reinterpret_cast<std::uint64_t>(part.BaseAddress) + part.RegionSize;
            if (++regions > kMaxRegionsPerAllocation) {
                return make(ids::kLoaderReleaseFailed, "the reservation at " + hex(allocation_base) + " is too fragmented",
                            "Report this with the agent log.");
            }
        }
        if (allocation_end <= address || count == kMaxAllocationsPerRange ||
            !owned_covers({allocation_base, allocation_end - allocation_base})) {
            return make(ids::kLoaderRangeNotReserved,
                        "the reservation containing " + hex(address) + " is not (entirely) owned by the agent",
                        "Only blocks reserved through ReserveRange can be released for a patch.");
        }
        allocations[count++] = {allocation_base, allocation_end};
        address = allocation_end;
    }

    // Release. If one release fails, put back what was already released.
    for (std::size_t i = 0; i < count; ++i) {
        if (!VirtualFree(reinterpret_cast<LPVOID>(allocations[i].base), 0, MEM_RELEASE)) {
            const DWORD error = GetLastError();
            for (std::size_t j = 0; j < i; ++j) {
                const AddressRange whole{allocations[j].base, allocations[j].end - allocations[j].base};
                owned_remove(whole);
                restore_reservation(whole);
            }
            return make(ids::kLoaderReleaseFailed,
                        "VirtualFree of the reservation at " + hex(allocations[i].base) + " failed (error " +
                            std::to_string(error) + ")",
                        "Report this with the agent log.");
        }
    }
    // Re-reserve everything that is not `range`: the race window is exactly `range`. A part that
    // another thread already touched is re-reserved in the free fragments that remain.
    owned_remove(range);
    for (std::size_t i = 0; i < count; ++i) {
        if (allocations[i].base < range.base) {
            const AddressRange head{allocations[i].base, range.base - allocations[i].base};
            owned_remove(head);
            restore_reservation(head);
            if (!owned_covers(head)) {
                CML_LOG(Warn, "CML%u: part of %llx+%llx next to a released range was taken by another thread",
                        ids::kLoaderRangeTaken, ull(head.base), ull(head.size));
            }
        }
        if (allocations[i].end > range.end()) {
            const AddressRange tail{range.end(), allocations[i].end - range.end()};
            owned_remove(tail);
            restore_reservation(tail);
            if (!owned_covers(tail)) {
                CML_LOG(Warn, "CML%u: part of %llx+%llx next to a released range was taken by another thread",
                        ids::kLoaderRangeTaken, ull(tail.base), ull(tail.size));
            }
        }
    }
    return {};
}

// After a failed or undone load: reserve again whatever part of `range` is free and own it.
void PatchLoader::restore_reservation(const AddressRange& range) {
    std::uint64_t address = range.base;
    while (address < range.end()) {
        MEMORY_BASIC_INFORMATION info;
        if (!query(address, &info)) {
            return;
        }
        const std::uint64_t region_end = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
        if (region_end <= address) {
            return;
        }
        if (info.State == MEM_FREE) {
            const std::uint64_t from = proc::align_up(address, kGranule);
            const std::uint64_t to = proc::align_down(std::min(region_end, range.end()), kGranule);
            if (from < to && reserve_at(from, to - from)) {
                owned_add({from, to - from});
            }
        }
        address = region_end;
    }
}

PatchLoader::~PatchLoader() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!owned_.empty()) {
        const AddressRange range = owned_.back();
        if (!detach_range(range)) {
            owned_remove(range);  // not releasable (already gone); never loop
        }
    }
}

PatchLoader& PatchLoader::shared() {
    static PatchLoader* const instance = new PatchLoader();
    return *instance;
}

void PatchLoader::set_test_hooks(const TestHooks& hooks) {
    std::lock_guard<std::mutex> lock(mutex_);
    hooks_ = hooks;
}

std::size_t PatchLoader::patch_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return patches_.size();
}

std::vector<AddressRange> PatchLoader::owned_ranges() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return owned_;
}

bool PatchLoader::is_loaded(std::uint64_t image_base) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(patches_.begin(), patches_.end(),
                       [&](const Patch& p) { return p.range.base == image_base; });
}

// ---------------------------------------------------------------------------------------------
// reservation
// ---------------------------------------------------------------------------------------------

Result<std::vector<AddressRange>> PatchLoader::reserve_ranges(const ReserveParams& params) {
    if (params.module_base == 0 || params.module_size == 0 ||
        params.module_base > std::numeric_limits<std::uint64_t>::max() - params.module_size) {
        return make(ids::kLoaderInvalidRequest, "ReserveRange needs a module with a non-zero base and size",
                    "Pass the base and size of the module being enabled.");
    }
    if (params.block_size == 0 || params.block_size > kMaxBlockSize || params.block_count == 0 ||
        params.block_count > ipc::kMaxReservedBlocks) {
        return make(ids::kLoaderInvalidRequest,
                    "ReserveRange block size " + hex(params.block_size) + " / count " + std::to_string(params.block_count) +
                        " is outside 1 byte..1 GiB / 1..64",
                    "Use the defaults (4 blocks of 64 MiB) or a smaller block size.");
    }
    const std::uint64_t block_size = proc::align_up(params.block_size, kGranule);
    const std::uint64_t reach = params.reach == 0 ? kDefaultReach : params.reach;
    const proc::AddressRange limits = proc::user_address_limits();
    const proc::ReachWindow window =
        proc::compute_reach_window(params.module_base, params.module_size, reach, limits.base, limits.end());
    if (window.empty()) {
        return make(ids::kLoaderNoAddressWindow,
                    "module " + hex(params.module_base) + "+" + hex(params.module_size) +
                        " leaves no address window that every REL32 can reach",
                    "The module is too large for patching with 32-bit relative calls; see KNOWN_LIMITATIONS.");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AddressRange> reserved;
    // The scan is racy by nature (another thread may allocate between scan and reservation), so
    // plan with spare candidates and rescan a few times.
    for (int round = 0; round < 3 && reserved.size() < params.block_count; ++round) {
        Result<std::vector<AddressRange>> free_regions = proc::query_free_regions(window.lo, window.hi);
        if (!free_regions) {
            return free_regions.error();
        }
        const std::size_t wanted = params.block_count - reserved.size();
        const std::vector<AddressRange> plan = proc::plan_blocks(*free_regions, window, block_size, wanted + 8,
                                                                 params.module_base, params.module_size);
        for (const AddressRange& block : plan) {
            if (reserved.size() >= params.block_count) {
                break;
            }
            if (reserve_at(block.base, block.size)) {
                owned_add(block);
                reserved.push_back(block);
            }
        }
        if (plan.empty()) {
            break;
        }
    }
    if (reserved.empty()) {
        return make(ids::kLoaderNoAddressWindow,
                    "no free " + hex(block_size) + "-byte block within reach of module " + hex(params.module_base) + "+" +
                        hex(params.module_size),
                    "Free address space near the module (fewer or smaller blocks) or reduce the block size.");
    }
    if (reserved.size() < params.block_count) {
        CML_LOG(Warn, "reserved %u of %u requested blocks near module 0x%llx", static_cast<unsigned>(reserved.size()),
                params.block_count,
                ull(params.module_base));
    } else {
        CML_LOG(Info, "reserved %u block(s) of 0x%llx bytes near module 0x%llx (first at 0x%llx)",
                static_cast<unsigned>(reserved.size()),
                ull(block_size), ull(params.module_base), ull(reserved.front().base));
    }
    return reserved;
}

Result<void> PatchLoader::reserve_exact(std::uint64_t base, std::uint64_t size) {
    if (size == 0 || base % kGranule != 0 || size % kGranule != 0 ||
        base > std::numeric_limits<std::uint64_t>::max() - size) {
        return make(ids::kLoaderInvalidRequest, "reserve_exact needs a 64 KiB aligned, non-empty range",
                    "Align the base and size to 64 KiB.");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!reserve_at(base, size)) {
        return make(ids::kLoaderNoAddressWindow, "cannot reserve " + hex(base) + "+" + hex(size) + " (already in use)",
                    "Choose another range.");
    }
    owned_add({base, size});
    return {};
}

Result<void> PatchLoader::release_range(std::uint64_t base, std::uint64_t size) {
    if (size == 0 || base % kGranule != 0 || size % kGranule != 0 ||
        base > std::numeric_limits<std::uint64_t>::max() - size) {
        return make(ids::kLoaderInvalidRequest, "ReleaseRange needs a 64 KiB aligned, non-empty range",
                    "Pass a range returned by ReserveRange.");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const AddressRange range{base, size};
    if (!owned_covers(range)) {
        return make(ids::kLoaderReleaseFailed,
                    "range " + hex(base) + "+" + hex(size) + " is not (entirely) reserved and unused by the agent",
                    "Only unused parts of blocks from ReserveRange can be released; unload patches first.");
    }
    return detach_range(range);
}

// ---------------------------------------------------------------------------------------------
// loading
// ---------------------------------------------------------------------------------------------

Result<void> PatchLoader::try_load_candidate(const ipc::PatchCandidate& candidate, std::uint32_t attempt,
                                             const std::wstring& wide_path, Patch* out) {
    const AddressRange range{candidate.image_base, proc::align_up(candidate.image_size, kGranule)};
    if (!owned_covers(range)) {
        return make(ids::kLoaderRangeNotReserved,
                    "image range " + hex(range.base) + "+" + hex(range.size) + " of " + candidate.path +
                        " is not inside address space reserved by the agent",
                    "Reserve a block with ReserveRange and link the patch at a base inside it.");
    }
    // 1. Release exactly the image range. From here until LoadLibraryExW returns, other threads of
    //    the process may take it.
    Result<void> detached = detach_range(range);
    if (!detached) {
        return detached.error();
    }
    if (hooks_.after_release != nullptr) {
        hooks_.after_release(hooks_.context, range.base, range.size, attempt);
    }
    // 2. Load.
    HMODULE module = LoadLibraryExW(wide_path.c_str(), nullptr, 0);
    if (module == nullptr) {
        const DWORD error = GetLastError();
        const bool taken = !range_is_free(range);  // probe before restore_reservation changes the picture
        restore_reservation(range);
        if (taken || error == ERROR_INVALID_ADDRESS || error == ERROR_ILLEGAL_DLL_RELOCATION ||
            error == ERROR_NOT_ENOUGH_MEMORY) {
            return make(ids::kLoaderRangeTaken,
                        "attempt " + std::to_string(attempt) + ": " + candidate.path + " could not be mapped at " +
                            hex(candidate.image_base) + " (OS error " + std::to_string(error) +
                            (taken ? ", the range was taken by another allocation" : "") + ")",
                        "The agent retries with the next reserved block; if every attempt fails, retry the reload.");
        }
        return make(ids::kLoaderLoadFailed,
                    "LoadLibraryExW(" + candidate.path + ") failed with OS error " + std::to_string(error),
                    "Check that the patch DLL exists, is a valid x64 DLL and that its dependencies can be found.");
    }
    // 3. It must sit where it was linked to sit.
    const auto loaded_at = reinterpret_cast<std::uint64_t>(module);
    if (loaded_at != candidate.image_base) {
        if (!FreeLibrary(module)) {
            CML_LOG(Warn, "FreeLibrary failed after a base mismatch (error %lu)", GetLastError());
        }
        restore_reservation(range);
        return make(ids::kLoaderBaseMismatch,
                    candidate.path + " loaded at " + hex(loaded_at) + " instead of " + hex(candidate.image_base),
                    "The patch must be linked with /FIXED /BASE:<the reserved address>; relink it.");
    }
    std::string why;
    if (!image_header_ok(loaded_at, candidate.image_size, &why)) {
        if (!FreeLibrary(module)) {
            CML_LOG(Warn, "FreeLibrary failed after an image mismatch (error %lu)", GetLastError());
        }
        restore_reservation(range);
        return make(ids::kLoaderImageMismatch, candidate.path + ": " + why,
                    "Send the SizeOfImage of the DLL that was actually linked.");
    }
    out->range = range;
    out->image_size = candidate.image_size;
    out->module = module;
    out->busy = true;  // the caller still has to apply fix-ups and run initializers
    return {};
}

PatchLoader::Patch* PatchLoader::find_patch(std::uint64_t image_base) {
    for (Patch& p : patches_) {
        if (p.range.base == image_base) {
            return &p;
        }
    }
    return nullptr;
}

// Unloads patches_[index] and returns its range to the reserved state. Requires mutex_.
Result<void> PatchLoader::unload_locked(std::size_t index) {
    const Patch patch = patches_[index];
    if (!FreeLibrary(patch.module)) {
        const DWORD error = GetLastError();
        patches_[index].busy = false;
        return make(ids::kLoaderUnloadFailed,
                    "FreeLibrary of the patch at " + hex(patch.range.base) + " failed (OS error " + std::to_string(error) + ")",
                    "The patch stays mapped and its address range is lost; report this with the agent log.");
    }
    patches_.erase(patches_.begin() + static_cast<std::ptrdiff_t>(index));
    restore_reservation(patch.range);
    CML_LOG(Debug, "unloaded patch at 0x%llx", ull(patch.range.base));
    return {};
}

void PatchLoader::unload_after_failure(std::uint64_t image_base) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < patches_.size(); ++i) {
        if (patches_[i].range.base == image_base) {
            Result<void> unloaded = unload_locked(i);
            if (!unloaded) {
                CML_LOG(Warn, "%s", unloaded.error().format().c_str());
            }
            return;
        }
    }
}

Result<void> PatchLoader::lock_patch(std::uint64_t image_base, std::uint64_t* image_size_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    Patch* patch = find_patch(image_base);
    if (patch == nullptr) {
        return make(ids::kLoaderUnknownPatch, "no patch is loaded at " + hex(image_base),
                    "Send LoadPatch first; ApplyFixups and RunInitializers address a patch by its load base.");
    }
    if (patch->busy) {
        return make(ids::kLoaderPatchBusy, "the patch at " + hex(image_base) + " is being used by another request",
                    "Wait for the previous request on this patch to be answered.");
    }
    patch->busy = true;
    *image_size_out = patch->image_size;
    return {};
}

void PatchLoader::unlock_patch(std::uint64_t image_base) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Patch* patch = find_patch(image_base)) {
        patch->busy = false;
    }
}

ipc::PatchLoaded PatchLoader::load_patch(const ipc::LoadPatch& request) {
    ipc::PatchLoaded answer;
    answer.request_id = request.request_id;
    answer.patch_id = request.patch_id;
    const auto fail = [&answer](const Diag& diag) {
        answer.status = diag.code;
        answer.message = failure_text(diag);
        answer.image_base = 0;
        answer.image_size = 0;
        return answer;
    };

    if (request.candidates.empty() || request.candidates.size() > ipc::kMaxLoadCandidates) {
        return fail(make(ids::kLoaderInvalidRequest, "LoadPatch needs 1 to 5 candidates", "Send at least one candidate."));
    }
    std::vector<std::wstring> wide_paths;
    wide_paths.reserve(request.candidates.size());
    for (const ipc::PatchCandidate& c : request.candidates) {
        if (c.path.empty() || c.image_size == 0 || c.image_size > kMaxImageSize || c.image_base == 0 ||
            c.image_base % kGranule != 0 ||
            c.image_base > std::numeric_limits<std::uint64_t>::max() - proc::align_up(c.image_size, kGranule)) {
            return fail(make(ids::kLoaderInvalidRequest,
                             "candidate " + c.path + " has a bad base " + hex(c.image_base) + " or size " + hex(c.image_size),
                             "The base must be 64 KiB aligned and the size the SizeOfImage of the linked DLL."));
        }
        wide_paths.push_back(proc::wide_from_utf8(c.path));
        if (wide_paths.back().empty()) {
            return fail(make(ids::kLoaderInvalidRequest, "candidate path is not convertible: " + c.path,
                             "Use an absolute path."));
        }
    }

    // Steps 1-3 for each candidate in turn, until one is placed.
    std::uint32_t attempts = 0;
    std::uint32_t collisions = 0;
    void (*busy_hook)(void*, std::uint64_t) = nullptr;
    void* busy_context = nullptr;
    std::size_t placed = request.candidates.size();
    Diag last;
    bool fatal = false;
    for (std::size_t i = 0; i < request.candidates.size(); ++i) {
        Patch patch;
        patch.patch_id = request.patch_id;
        Result<void> result = [&]() -> Result<void> {
            std::lock_guard<std::mutex> lock(mutex_);
            ++attempts;
            Result<void> loaded = try_load_candidate(request.candidates[i], attempts, wide_paths[i], &patch);
            if (loaded) {
                patches_.push_back(patch);
                busy_hook = hooks_.while_busy;
                busy_context = hooks_.context;
            }
            return loaded;
        }();
        if (result) {
            placed = i;
            break;
        }
        last = result.error();
        CML_LOG(Debug, "%s", last.format().c_str());
        if (last.code == ids::kLoaderRangeTaken) {
            ++collisions;
        } else if (last.code != ids::kLoaderRangeNotReserved) {
            fatal = true;
            break;
        }
    }
    if (placed == request.candidates.size()) {
        if (!fatal && collisions > 0) {
            last = make(ids::kLoaderRangeTaken,
                        "the address range of every one of " + std::to_string(attempts) +
                            " load attempt(s) was taken by another allocation before LoadLibrary could map the patch (last: " +
                            last.message + ")",
                        "Retry the reload; if it keeps failing something in the process allocates aggressively near the module.");
        }
        answer.attempts = attempts;
        return fail(last);
    }

    // Steps 4-6 on the placed patch (the patch is marked busy, so nobody else touches it).
    const ipc::PatchCandidate& chosen = request.candidates[placed];
    const ImageRef image{chosen.image_base, chosen.image_size};
    answer.attempts = attempts;
    answer.candidate_index = static_cast<std::uint32_t>(placed);
    if (busy_hook != nullptr) {
        busy_hook(busy_context, chosen.image_base);
    }

    Result<std::uint32_t> fixed = loader::apply_fixups(image, chosen.fixups);
    if (!fixed) {
        unload_after_failure(chosen.image_base);
        return fail(fixed.error());
    }
    Result<std::uint32_t> ran = loader::run_initializers(image, request.initializer_rvas);
    if (!ran) {
        unload_after_failure(chosen.image_base);
        return fail(ran.error());
    }
    unlock_patch(chosen.image_base);

    answer.status = 0;
    answer.image_base = chosen.image_base;
    answer.image_size = chosen.image_size;
    answer.fixups_applied = *fixed;
    answer.initializers_run = *ran;
    answer.message = "loaded";
    CML_LOG(Debug, "patch %llu loaded at 0x%llx (candidate %u, %u attempt(s), %u fix-ups, %u initializers)",
            ull(request.patch_id), ull(chosen.image_base), static_cast<unsigned>(placed), attempts, *fixed, *ran);
    return answer;
}

Result<std::uint32_t> PatchLoader::apply_fixups(std::uint64_t image_base, std::span<const ipc::Fixup> fixups) {
    std::uint64_t size = 0;
    Result<void> locked = lock_patch(image_base, &size);
    if (!locked) {
        return locked.error();
    }
    Result<std::uint32_t> applied = loader::apply_fixups({image_base, static_cast<std::uint32_t>(size)}, fixups);
    if (!applied) {
        unload_after_failure(image_base);
        return applied;
    }
    unlock_patch(image_base);
    return applied;
}

Result<std::uint32_t> PatchLoader::run_initializers(std::uint64_t image_base, std::span<const std::uint32_t> rvas) {
    std::uint64_t size = 0;
    Result<void> locked = lock_patch(image_base, &size);
    if (!locked) {
        return locked.error();
    }
    Result<std::uint32_t> ran = loader::run_initializers({image_base, static_cast<std::uint32_t>(size)}, rvas);
    if (!ran) {
        unload_after_failure(image_base);
        return ran;
    }
    unlock_patch(image_base);
    return ran;
}

Result<void> PatchLoader::unload_patch(std::uint64_t image_base) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < patches_.size(); ++i) {
        if (patches_[i].range.base == image_base) {
            if (patches_[i].busy) {
                return make(ids::kLoaderPatchBusy, "the patch at " + hex(image_base) + " is being used by another request",
                            "Wait for the previous request on this patch to be answered.");
            }
            return unload_locked(i);
        }
    }
    return make(ids::kLoaderUnknownPatch, "no patch is loaded at " + hex(image_base),
                "Only patches loaded by LoadPatch can be unloaded this way.");
}

// ---------------------------------------------------------------------------------------------
// broker requests
// ---------------------------------------------------------------------------------------------

bool PatchLoader::is_loader_request(const ipc::Message& message) noexcept {
    if (const auto* reserve = std::get_if<ipc::ReserveRange>(&message)) {
        return !reserve->answer;
    }
    return std::holds_alternative<ipc::ReleaseRange>(message) || std::holds_alternative<ipc::LoadPatch>(message) ||
           std::holds_alternative<ipc::ApplyFixups>(message) || std::holds_alternative<ipc::RunInitializers>(message);
}

ipc::Message PatchLoader::handle_request(const ipc::Message& request) {
    const auto reply = [](std::uint64_t id, const Diag* diag, const std::string& ok_text) {
        ipc::Reply r;
        r.request_id = id;
        if (diag != nullptr) {
            r.status = diag->code;
            r.message = failure_text(*diag);
        } else {
            r.message = ok_text;
        }
        return r;
    };

    if (const auto* reserve = std::get_if<ipc::ReserveRange>(&request); reserve != nullptr && !reserve->answer) {
        ReserveParams params;
        params.module_base = reserve->module_base;
        params.module_size = reserve->module_size;
        params.block_size = reserve->block_size;
        params.reach = reserve->reach;
        params.block_count = reserve->block_count;
        ipc::ReserveRange answer;
        answer.request_id = reserve->request_id;
        answer.answer = true;
        Result<std::vector<AddressRange>> blocks = reserve_ranges(params);
        if (!blocks) {
            answer.status = blocks.error().code;
            answer.message = failure_text(blocks.error());
        } else {
            answer.message = "reserved " + std::to_string(blocks->size()) + " block(s)";
            for (const AddressRange& b : *blocks) {
                answer.blocks.push_back({b.base, b.size});
            }
        }
        return answer;
    }
    if (const auto* release = std::get_if<ipc::ReleaseRange>(&request)) {
        Result<void> done = release->mode == ipc::ReleaseMode::UnloadPatch ? unload_patch(release->base)
                                                                            : release_range(release->base, release->size);
        return done ? reply(release->request_id, nullptr, "released") : reply(release->request_id, &done.error(), "");
    }
    if (const auto* load = std::get_if<ipc::LoadPatch>(&request)) {
        return load_patch(*load);
    }
    if (const auto* fixups = std::get_if<ipc::ApplyFixups>(&request)) {
        Result<std::uint32_t> done = apply_fixups(fixups->image_base, fixups->fixups);
        return done ? reply(fixups->request_id, nullptr, "applied " + std::to_string(*done) + " fix-up(s)")
                    : reply(fixups->request_id, &done.error(), "");
    }
    if (const auto* inits = std::get_if<ipc::RunInitializers>(&request)) {
        Result<std::uint32_t> done = run_initializers(inits->image_base, inits->rvas);
        return done ? reply(inits->request_id, nullptr, "ran " + std::to_string(*done) + " initializer(s)")
                    : reply(inits->request_id, &done.error(), "");
    }
    const Diag unsupported = make(ids::kMessageTypeUnsupported,
                                  std::string(ipc::to_string(ipc::type_of(request))) + " is not a loader request",
                                  "Use an agent and a broker from the same build.");
    return reply(0, &unsupported, "");
}

}  // namespace cml::agent::loader

#endif  // _WIN32
