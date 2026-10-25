// WP-08: the portable address-space planning logic of cml_proc (reach window, block placement,
// REL32 displacement). The Win32 scan is exercised by unit.agent_loader.
#include "cml/proc/address_space.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "doctest.h"

using namespace cml::proc;

namespace {

constexpr std::uint64_t kMiB = 1ull << 20;
constexpr std::uint64_t kGiB = 1ull << 30;
constexpr std::uint64_t kUserMax = 0x7FFF'FFFE'0000ull;

}  // namespace

TEST_CASE("the reach window keeps every REL32 inside int32") {
    const std::uint64_t base = 0x7FF6'0000'0000ull;
    SUBCASE("small module: 1.5 GiB on both sides") {
        const ReachWindow w = compute_reach_window(base, 4 * kMiB, 1536 * kMiB, 0x10000, kUserMax);
        CHECK(w.lo == base - 1536 * kMiB);
        CHECK(w.hi == base + 4 * kMiB + 1536 * kMiB);
        CHECK(w.lo % kAllocationGranularity == 0);
        CHECK(w.hi % kAllocationGranularity == 0);
    }
    SUBCASE("a big module shrinks the window so the far end of the module is still reachable") {
        const ReachWindow w = compute_reach_window(base, 1 * kGiB, 1536 * kMiB, 0x10000, kUserMax);
        // A block at w.lo must be within REL32 reach of the module's last byte, and a block ending at
        // w.hi of its first byte.
        CHECK(base + kGiB - w.lo <= kRel32Reach);
        CHECK(w.hi - base <= kRel32Reach);
        CHECK_FALSE(w.empty());
    }
    SUBCASE("a module of about 4 GiB or more leaves nothing") {
        const ReachWindow w = compute_reach_window(base, 5 * kGiB, 1536 * kMiB, 0x10000, kUserMax);
        CHECK(w.empty());
    }
    SUBCASE("clipped to the address space limits, without wrapping") {
        const ReachWindow w = compute_reach_window(0x40000000ull, 1 * kMiB, 1536 * kMiB, 0x10000, kUserMax);
        CHECK(w.lo == 0x10000);  // base - reach would underflow below the minimum address
        const ReachWindow top = compute_reach_window(kUserMax - 2 * kMiB, 1 * kMiB, 1536 * kMiB, 0x10000, kUserMax);
        CHECK(top.hi <= kUserMax);
        const ReachWindow huge = compute_reach_window(~0ull - 100, 50, ~0ull, 0, ~0ull);
        CHECK(huge.hi >= huge.lo);  // saturating arithmetic, no wrap
    }
}

TEST_CASE("plan_blocks places blocks near the module and respects the window") {
    const std::uint64_t base = 0x7FF6'0000'0000ull;
    const std::uint64_t size = 4 * kMiB;
    const ReachWindow w = compute_reach_window(base, size, 1536 * kMiB, 0x10000, kUserMax);
    const std::vector<AddressRange> free_regions = {
        {w.lo - 100 * kMiB, 500 * kMiB},               // starts before the window, ends inside
        {base - 8 * kMiB, 8 * kMiB - 0x10000},          // directly below the module, with a 64 KiB hole
        {base + size + 64 * kMiB, 300 * kMiB},          // above the module
    };
    const std::vector<AddressRange> blocks = plan_blocks(free_regions, w, 64 * kMiB, 3, base, size);
    REQUIRE(blocks.size() == 3);
    std::uint64_t last_distance = 0;
    for (const AddressRange& b : blocks) {
        CHECK(b.size == 64 * kMiB);
        CHECK(b.base % kAllocationGranularity == 0);
        CHECK(b.base >= w.lo);
        CHECK(b.end() <= w.hi);
        CHECK((b.end() <= base || b.base >= base + size));  // never over the module
        const std::uint64_t distance = b.end() <= base ? base - b.end() : b.base - (base + size);
        CHECK(distance >= last_distance);  // nearest first
        last_distance = distance;
    }
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        for (std::size_t j = i + 1; j < blocks.size(); ++j) {
            CHECK((blocks[i].end() <= blocks[j].base || blocks[j].end() <= blocks[i].base));
        }
    }
    // The small region below the module (8 MiB) cannot hold a 64 MiB block; the nearest block is
    // the one packed against the module from the 500 MiB region... which ends before the window
    // allows? It must still lie inside the window.
}

TEST_CASE("plan_blocks packs from the module outwards and caps the count") {
    const std::uint64_t base = 0x10'0000'0000ull;
    const std::uint64_t size = 1 * kMiB;
    const ReachWindow w = compute_reach_window(base, size, 1536 * kMiB, 0x10000, kUserMax);
    const std::vector<AddressRange> below_only = {{base - 400 * kMiB, 400 * kMiB}};
    const std::vector<AddressRange> blocks = plan_blocks(below_only, w, 64 * kMiB, 4, base, size);
    REQUIRE(blocks.size() == 4);
    CHECK(blocks[0].end() == base);                       // adjacent below the image
    CHECK(blocks[1].end() == blocks[0].base);             // then outwards
    CHECK(blocks[3].base == base - 4 * 64 * kMiB);
    CHECK(plan_blocks(below_only, w, 64 * kMiB, 0, base, size).empty());
    CHECK(plan_blocks(below_only, w, 0, 4, base, size).empty());
    CHECK(plan_blocks(below_only, w, 512 * kMiB, 1, base, size).empty());  // does not fit anywhere
}

TEST_CASE("plan_blocks rounds the block size up and never overlaps the module") {
    const std::uint64_t base = 0x10'0000'0000ull;
    const std::uint64_t size = 16 * kMiB;
    const ReachWindow w = compute_reach_window(base, size, 1536 * kMiB, 0x10000, kUserMax);
    // A free region that (wrongly) contains the module itself.
    const std::vector<AddressRange> overlapping = {{base - 10 * kMiB, 100 * kMiB}};
    const std::vector<AddressRange> blocks = plan_blocks(overlapping, w, 3 * kMiB + 1, 8, base, size);
    REQUIRE_FALSE(blocks.empty());
    for (const AddressRange& b : blocks) {
        CHECK(b.size == 3 * kMiB + 0x10000);  // 3 MiB + 1 rounded up to the next 64 KiB multiple
        CHECK(b.size % kAllocationGranularity == 0);
        CHECK((b.end() <= base || b.base >= base + size));
    }
    // Overlapping free regions must not yield overlapping blocks.
    const std::vector<AddressRange> twice = {{base - 200 * kMiB, 100 * kMiB}, {base - 150 * kMiB, 100 * kMiB}};
    const std::vector<AddressRange> blocks2 = plan_blocks(twice, w, 20 * kMiB, 8, base, size);
    for (std::size_t i = 0; i < blocks2.size(); ++i) {
        for (std::size_t j = i + 1; j < blocks2.size(); ++j) {
            CHECK((blocks2[i].end() <= blocks2[j].base || blocks2[j].end() <= blocks2[i].base));
        }
    }
}

TEST_CASE("rel32_displacement matches the architecture's rules") {
    std::int32_t d = 0;
    // disp = target - (site + 4 + n)
    REQUIRE(rel32_displacement(0x1000, 0, 0x1004, &d));
    CHECK(d == 0);
    REQUIRE(rel32_displacement(0x1000, 4, 0x1008, &d));
    CHECK(d == 0);
    REQUIRE(rel32_displacement(0x1000, 5, 0x1000, &d));
    CHECK(d == -9);
    // Forward limit: INT32_MAX fits, one more does not.
    REQUIRE(rel32_displacement(0x1000, 0, 0x1004 + 0x7FFFFFFFull, &d));
    CHECK(d == 0x7FFFFFFF);
    CHECK_FALSE(rel32_displacement(0x1000, 0, 0x1004 + 0x80000000ull, &d));
    // Backward limit: INT32_MIN fits, one more does not.
    REQUIRE(rel32_displacement(0x1'0000'1000ull, 0, 0x1'0000'1004ull - 0x80000000ull, &d));
    CHECK(d == INT32_MIN);
    CHECK_FALSE(rel32_displacement(0x1'0000'1000ull, 0, 0x1'0000'1003ull - 0x80000000ull, &d));
    // High addresses do not wrap.
    REQUIRE(rel32_displacement(0x7FFF'0000'0000ull, 1, 0x7FFF'0000'0100ull, &d));
    CHECK(d == 0x100 - 5);
    CHECK_FALSE(rel32_displacement(0x1000, 0, 0x7FF6'0000'0000ull, &d));
}

TEST_CASE("alignment helpers") {
    CHECK(align_up(0, 0x10000) == 0);
    CHECK(align_up(1, 0x10000) == 0x10000);
    CHECK(align_up(0x10000, 0x10000) == 0x10000);
    CHECK(align_down(0x1FFFF, 0x10000) == 0x10000);
}
