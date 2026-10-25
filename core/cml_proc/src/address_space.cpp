#include "cml/proc/address_space.h"

#include <algorithm>
#include <limits>
#include <string>

#if defined(_WIN32)
#include <windows.h>

#include "cml/proc/loader_diag_ids.h"
#endif

namespace cml::proc {

namespace {

constexpr std::uint64_t kMax64 = std::numeric_limits<std::uint64_t>::max();

constexpr std::uint64_t sat_add(std::uint64_t a, std::uint64_t b) noexcept {
    return a > kMax64 - b ? kMax64 : a + b;
}
constexpr std::uint64_t sat_sub(std::uint64_t a, std::uint64_t b) noexcept {
    return a > b ? a - b : 0;
}

struct Candidate {
    AddressRange range;
    std::uint64_t distance = 0;  // gap between the block and the module
};

bool overlaps(const AddressRange& a, const AddressRange& b) noexcept {
    return a.base < b.end() && b.base < a.end();
}

}  // namespace

ReachWindow compute_reach_window(std::uint64_t module_base, std::uint64_t module_size, std::uint64_t reach,
                                 std::uint64_t min_address, std::uint64_t max_address) noexcept {
    const std::uint64_t module_end = sat_add(module_base, module_size);
    const std::uint64_t lo = std::max({min_address, sat_sub(module_base, reach), sat_sub(module_end, kRel32Reach)});
    const std::uint64_t hi = std::min({max_address, sat_add(module_end, reach), sat_add(module_base, kRel32Reach)});
    ReachWindow window;
    window.lo = lo > kMax64 - (kAllocationGranularity - 1) ? kMax64 : align_up(lo, kAllocationGranularity);
    window.hi = align_down(hi, kAllocationGranularity);
    return window;
}

std::vector<AddressRange> plan_blocks(std::span<const AddressRange> free_regions, const ReachWindow& window,
                                      std::uint64_t block_size, std::size_t max_blocks, std::uint64_t module_base,
                                      std::uint64_t module_size) {
    std::vector<AddressRange> result;
    if (block_size == 0 || max_blocks == 0 || window.empty() || block_size > kMax64 - kAllocationGranularity) {
        return result;
    }
    block_size = align_up(block_size, kAllocationGranularity);
    const std::uint64_t module_end = sat_add(module_base, module_size);

    std::vector<Candidate> candidates;
    // Cuts [lo, hi) (already inside the window) to the part on one side of the module and appends
    // the blocks that fit, packed from the module outwards.
    const auto add_side = [&](std::uint64_t lo, std::uint64_t hi, bool below) {
        lo = lo > kMax64 - (kAllocationGranularity - 1) ? kMax64 : align_up(lo, kAllocationGranularity);
        hi = align_down(hi, kAllocationGranularity);
        if (lo >= hi) {
            return;
        }
        const std::uint64_t fit = (hi - lo) / block_size;
        const std::uint64_t take = std::min<std::uint64_t>(fit, max_blocks);
        for (std::uint64_t i = 0; i < take; ++i) {
            Candidate c;
            if (below) {
                c.range = {hi - (i + 1) * block_size, block_size};
                c.distance = module_base - c.range.end();
            } else {
                c.range = {lo + i * block_size, block_size};
                c.distance = c.range.base - module_end;
            }
            candidates.push_back(c);
        }
    };

    for (const AddressRange& region : free_regions) {
        const std::uint64_t lo = std::max(region.base, window.lo);
        const std::uint64_t hi = std::min(region.end(), window.hi);
        if (lo >= hi) {
            continue;
        }
        // Never overlap the module: keep the part below it and the part above it.
        if (lo < module_base) {
            add_side(lo, std::min(hi, module_base), true);
        }
        if (hi > module_end) {
            add_side(std::max(lo, module_end), hi, false);
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
    for (const Candidate& c : candidates) {
        if (result.size() >= max_blocks) {
            break;
        }
        // Overlapping input regions must not yield overlapping blocks.
        const bool clash = std::any_of(result.begin(), result.end(),
                                       [&](const AddressRange& taken) { return overlaps(taken, c.range); });
        if (!clash) {
            result.push_back(c.range);
        }
    }
    return result;
}

bool rel32_displacement(std::uint64_t site, std::uint32_t n, std::uint64_t target, std::int32_t* out) noexcept {
    const std::uint64_t next = site + 4 + n;  // address of the byte after the instruction
    if (target >= next) {
        const std::uint64_t delta = target - next;
        if (delta > 0x7FFFFFFFull) {
            return false;
        }
        *out = static_cast<std::int32_t>(delta);
        return true;
    }
    const std::uint64_t delta = next - target;
    if (delta > 0x80000000ull) {
        return false;
    }
    // delta == 2^31 maps to INT32_MIN; avoid negating it in int32.
    *out = static_cast<std::int32_t>(-static_cast<std::int64_t>(delta));
    return true;
}

#if defined(_WIN32)

AddressRange user_address_limits() noexcept {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const auto lo = reinterpret_cast<std::uint64_t>(info.lpMinimumApplicationAddress);
    const auto hi = reinterpret_cast<std::uint64_t>(info.lpMaximumApplicationAddress);
    return AddressRange{lo, hi - lo + 1};
}

Result<std::vector<AddressRange>> query_free_regions(std::uint64_t lo, std::uint64_t hi) {
    const AddressRange limits = user_address_limits();
    lo = std::max(lo, limits.base);
    hi = std::min(hi, limits.end());
    std::vector<AddressRange> regions;
    std::uint64_t address = lo;
    while (address < hi) {
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0) {
            const DWORD error = GetLastError();
            if (error == ERROR_INVALID_PARAMETER) {
                break;  // past the end of the user address space
            }
            return Diag::make(ids::kLoaderAddressScanFailed,
                              "VirtualQuery failed at 0x" + std::to_string(address) + " with error " +
                                  std::to_string(error) + " while scanning the address space",
                              "Retry; if it persists the process is in an unusual state - report it with the agent log.");
        }
        const auto region_base = reinterpret_cast<std::uint64_t>(info.BaseAddress);
        const std::uint64_t region_end = region_base + info.RegionSize;
        if (info.State == MEM_FREE) {
            const std::uint64_t from = align_up(std::max(address, region_base), kAllocationGranularity);
            const std::uint64_t to = align_down(std::min(region_end, hi), kAllocationGranularity);
            if (from < to) {
                regions.push_back({from, to - from});
            }
        }
        if (region_end <= address) {
            break;  // defensive: never loop forever
        }
        address = region_end;
    }
    return regions;
}

#endif  // _WIN32

}  // namespace cml::proc
