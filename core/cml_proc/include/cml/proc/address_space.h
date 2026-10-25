#pragma once

// Address-space helpers shared by the agent and the broker (plan section 4.7).
//
// Patch DLLs are linked `/FIXED` at a base inside a block of address space that the agent reserved
// near the module being patched, so that every REL32 between the patch and the module fits in 32
// bits. This header holds
//   * the pure planning logic (portable: reach window, choosing blocks among free regions), which
//     is unit-tested on every host, and
//   * the Win32 scan (Windows only): the free regions of the process within a window.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "cml/util/result.h"

namespace cml::proc {

// Windows reserves address space in units of 64 KiB and maps images at such addresses.
inline constexpr std::uint64_t kAllocationGranularity = 0x10000;

// Largest distance between a REL32 site and its target that still fits an int32, minus a margin
// for the instruction length (4 + N <= 9) and one granule of slack. A block [b, b + size) can be
// reached from every byte of a module [m, m_end) and vice versa iff
//   b + size <= m + kRel32Reach   and   b >= m_end - kRel32Reach.
inline constexpr std::uint64_t kRel32Reach = 0x7FFF0000ull;

struct AddressRange {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::uint64_t end() const noexcept { return base + size; }
    bool contains(std::uint64_t address) const noexcept { return address >= base && address - base < size; }
};

// Where a block may lie. Empty when lo >= hi.
struct ReachWindow {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    bool empty() const noexcept { return lo >= hi; }
};

// The window for blocks around [module_base, module_base + module_size): within `reach` of the
// module on both sides (default 1.5 GiB in the plan) AND within REL32 range of every part of the
// module, clipped to [min_address, max_address). All arithmetic saturates; never wraps.
ReachWindow compute_reach_window(std::uint64_t module_base, std::uint64_t module_size, std::uint64_t reach,
                                 std::uint64_t min_address, std::uint64_t max_address) noexcept;

// Picks block placements. `free_regions` are free address ranges (any order, may overlap the
// window partially). Returns at most `max_blocks` non-overlapping blocks of `block_size` bytes,
// 64 KiB aligned, fully inside `window`, never overlapping the module, ordered nearest to the
// module first. Within a free region blocks are packed from the module side outwards.
// `block_size` is rounded up to the allocation granularity by the caller; 0 yields nothing.
std::vector<AddressRange> plan_blocks(std::span<const AddressRange> free_regions, const ReachWindow& window,
                                      std::uint64_t block_size, std::size_t max_blocks, std::uint64_t module_base,
                                      std::uint64_t module_size);

// True when a REL32 displacement from `site` (the address of the 4 displacement bytes) with `n`
// immediate bytes after it to `target` fits an int32; stores the displacement in `*out` then.
bool rel32_displacement(std::uint64_t site, std::uint32_t n, std::uint64_t target, std::int32_t* out) noexcept;

inline constexpr std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) noexcept {
    return (value + alignment - 1) / alignment * alignment;
}
inline constexpr std::uint64_t align_down(std::uint64_t value, std::uint64_t alignment) noexcept {
    return value / alignment * alignment;
}

#if defined(_WIN32)

// Lowest and highest usable user-mode addresses of this process ([min, max)), from GetSystemInfo.
AddressRange user_address_limits() noexcept;

// The free (MEM_FREE) regions of the calling process that intersect [lo, hi), clipped to it and
// shrunk to 64 KiB alignment inwards. Allocation-heavy (a std::vector): do not call while threads
// are suspended. CML4214 when VirtualQuery fails unexpectedly.
Result<std::vector<AddressRange>> query_free_regions(std::uint64_t lo, std::uint64_t hi);

#endif  // _WIN32

}  // namespace cml::proc
