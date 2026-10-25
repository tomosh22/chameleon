// The hook installer on real code (Windows): hand-assembled functions placed with exact padding in
// executable memory, and functions compiled by this cell's compiler. Covers both strategies, the
// short-function rules, re-pointing, revert, atomicity of a batch, every refusal, page protection,
// the RIP-in-window retry path, thread convergence and the stopped-world discipline.
#if defined(_WIN32)

#include <windows.h>

#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "crash_report.h"
#include "doctest.h"
#include "heap.h"
#include "hook/diag_ids.h"
#include "hook/installer.h"
#include "hook_test_support.h"

using namespace cml::agent::hook;
using namespace hooktest;
namespace agent_heap = cml::agent::heap;

namespace {

// Functions compiled by this cell's compiler (noinline, called through volatile pointers).
volatile int g_bias = 1000;
HOOKTEST_NOINLINE int natural_orig(int x) { return x + g_bias; }
HOOKTEST_NOINLINE int natural_new_1(int x) { return x + g_bias + 1; }
HOOKTEST_NOINLINE int natural_new_2(int x) { return x + g_bias + 2; }

using NaturalFn = int (*)(int);

template <class Function>
const void* address_of(Function function) {
    return reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(function));
}

std::string describe(const BatchResult& r, const std::vector<HookOutcome>& out) {
    std::string s = r.diag.format();
    for (const HookOutcome& o : out) {
        s += " | " + std::to_string(static_cast<int>(o.status)) + " " + o.diag.format();
    }
    return s;
}

std::int32_t rel32_at(const std::uint8_t* p) {
    std::int32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

std::int64_t distance(const void* to, const void* from_next) {
    return static_cast<const std::uint8_t*>(to) - static_cast<const std::uint8_t*>(from_next);
}

BatchResult install_one(Installer& installer, const void* target, const void* replacement, const char* name,
                        HookOutcome* outcome) {
    std::vector<HookOutcome> out;
    const HookRequest request{target, replacement, name};
    BatchResult r = installer.install(&request, 1, out);
    if (outcome != nullptr) {
        *outcome = out.empty() ? HookOutcome{} : out[0];
    }
    return r;
}

BatchResult revert_one(Installer& installer, const void* target, const char* name, HookOutcome* outcome) {
    std::vector<HookOutcome> out;
    const HookRequest request{target, nullptr, name};
    BatchResult r = installer.revert(&request, 1, out);
    if (outcome != nullptr) {
        *outcome = out.empty() ? HookOutcome{} : out[0];
    }
    return r;
}

// Memory around a function, to check that nothing outside the intended bytes changes.
constexpr std::size_t kBefore = 16;
constexpr std::size_t kAfter = 48;
Bytes around(const Placed& p) {
    return snapshot(p.entry - kBefore, kBefore + kAfter);
}

}  // namespace

TEST_CASE("strategy 1 on the entry shapes of cl Debug, cl Release and clang-cl (ADR-0020)") {
    struct Shape {
        const char* name;
        Bytes code;
        std::size_t padding;
        std::uint32_t first_len;
    };
    const Shape shapes[] = {
        {"cl_debug (7 CC, 40 57 48 83 ec 20)", cl_debug(0), 7, 2},
        {"cl_release (8 CC, 48 83 ec 28)", cl_release(0), 8, 4},
        {"clang_cl (16 CC, 48 83 ec 28)", cl_release(0), 16, 4},
    };
    for (const Shape& shape : shapes) {
        INFO(shape.name);
        Arena arena;
        REQUIRE(arena.ok());
        const Placed old_fn = arena.place_padded(shape.code, shape.padding, 16);
        const Placed new_fn = arena.place_padded(lea_ret(1000), 8, 8);
        arena.seal();
        const Bytes before = around(old_fn);
        REQUIRE(old_fn.as_int()(5) == 5);

        Installer installer;
        REQUIRE(installer.ready());
        HookOutcome outcome;
        const BatchResult r = install_one(installer, old_fn.entry, new_fn.entry, "old_fn", &outcome);
        REQUIRE_MESSAGE(r.ok(), r.diag.format());
        CHECK(outcome.status == HookStatus::Installed);
        CHECK(outcome.strategy == Strategy::PaddingTwoByte);
        CHECK(r.stats.attempts == 1);

        // The padding holds `jmp rel32` to the new code; the entry holds `jmp -7` (EB F9).
        CHECK(old_fn.entry[-5] == 0xE9);
        CHECK(rel32_at(old_fn.entry - 4) == distance(new_fn.entry, old_fn.entry));
        CHECK(old_fn.entry[0] == 0xEB);
        CHECK(old_fn.entry[1] == 0xF9);
        // Nothing else moved: bytes before the padding jump, and everything after the first instruction.
        const Bytes after = around(old_fn);
        for (std::size_t i = 0; i < before.size(); ++i) {
            const bool written = (i >= kBefore - 5 && i < kBefore + 2);
            if (!written) {
                CHECK_MESSAGE(before[i] == after[i], "offset " << i);
            }
        }
        CHECK(old_fn.as_int()(5) == 1005);

        // The record keeps the original bytes of the first hook.
        HookInfo info;
        REQUIRE(installer.find(old_fn.entry, &info));
        CHECK(info.strategy == Strategy::PaddingTwoByte);
        CHECK(info.entry_length == 2);
        CHECK(std::memcmp(info.original_entry, before.data() + kBefore, 2) == 0);
        CHECK(std::memcmp(info.original_padding, before.data() + kBefore - 5, 5) == 0);

        // Revert puts every byte back.
        HookOutcome reverted;
        const BatchResult rr = revert_one(installer, old_fn.entry, "old_fn", &reverted);
        REQUIRE_MESSAGE(rr.ok(), rr.diag.format());
        CHECK(reverted.status == HookStatus::Reverted);
        CHECK(around(old_fn) == before);
        CHECK(old_fn.as_int()(5) == 5);
        CHECK(installer.hook_count() == 0);
        CHECK(protection_of(old_fn.entry) == PAGE_EXECUTE_READ);  // protection restored
    }
}

TEST_CASE("strategy 2: a 5-byte jump at the entry, at every alignment") {
    for (std::size_t misalign : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 11u, 13u, 15u}) {
        INFO("entry misalignment " << misalign);
        Arena arena;
        const Placed old_fn = arena.place_padded(cl_release(0), 0, 0, 0xCC, 0xCC, misalign);
        const Placed new_fn = arena.place_padded(lea_ret(2000), 0, 0);
        arena.seal();
        const Bytes before = around(old_fn);
        // No padding in front: the bytes before are 0x00.
        REQUIRE(old_fn.entry[-1] == 0x00);

        Installer installer;
        HookOutcome outcome;
        const BatchResult r = install_one(installer, old_fn.entry, new_fn.entry, "no_padding", &outcome);
        REQUIRE_MESSAGE(r.ok(), r.diag.format());
        CHECK(outcome.strategy == Strategy::FiveByte);
        CHECK(old_fn.entry[0] == 0xE9);
        CHECK(rel32_at(old_fn.entry + 1) == distance(new_fn.entry, old_fn.entry + 5));
        // cl_release is sub rsp,28h (4 bytes) + lea (6 bytes): the jump covers both, the window is 10.
        const Bytes after = around(old_fn);
        for (std::size_t i = 0; i < before.size(); ++i) {
            if (i < kBefore || i >= kBefore + 5) {
                CHECK_MESSAGE(before[i] == after[i], "offset " << i);  // the stray tail bytes are untouched too
            }
        }
        CHECK(old_fn.as_int()(7) == 2007);
        REQUIRE(revert_one(installer, old_fn.entry, "no_padding", nullptr).ok());
        CHECK(around(old_fn) == before);
        CHECK(old_fn.as_int()(7) == 7);
    }
}

TEST_CASE("a first instruction of one byte refuses strategy 1 and falls back to strategy 2") {
    Arena arena;
    const Placed old_fn = arena.place_padded(push_first(0), 16, 16);  // plenty of CC in front
    const Placed new_fn = arena.place_padded(lea_ret(3000), 8, 8);
    arena.seal();
    const Bytes before = around(old_fn);
    Installer installer;
    HookOutcome outcome;
    const BatchResult r = install_one(installer, old_fn.entry, new_fn.entry, "push_first", &outcome);
    REQUIRE_MESSAGE(r.ok(), r.diag.format());
    CHECK(outcome.strategy == Strategy::FiveByte);
    CHECK(old_fn.entry[0] == 0xE9);
    CHECK(old_fn.entry[-1] == 0xCC);  // the padding was not used
    CHECK(old_fn.as_int()(1) == 3001);
    REQUIRE(revert_one(installer, old_fn.entry, "push_first", nullptr).ok());
    CHECK(around(old_fn) == before);
}

TEST_CASE("functions shorter than 5 bytes, with and without padding") {
    SUBCASE("3 bytes, CC padding in front: strategy 1") {
        Arena arena;
        const Placed old_fn = arena.place_padded(xor_ret(), 16, 13);  // xor eax,eax; ret
        const Placed new_fn = arena.place_padded(lea_ret(4000), 8, 8);
        arena.seal();
        const Bytes before = around(old_fn);
        Installer installer;
        HookOutcome outcome;
        REQUIRE(install_one(installer, old_fn.entry, new_fn.entry, "xor_ret", &outcome).ok());
        CHECK(outcome.strategy == Strategy::PaddingTwoByte);
        CHECK(old_fn.as_int()(1) == 4001);
        REQUIRE(revert_one(installer, old_fn.entry, "xor_ret", nullptr).ok());
        CHECK(around(old_fn) == before);
        CHECK(old_fn.as_int()(1) == 0);
    }
    SUBCASE("4 bytes, no padding in front, CC after: strategy 2 over the function and one padding byte") {
        Arena arena;
        const Placed old_fn = arena.place_padded(short_lea(0), 0, 12);
        const Placed new_fn = arena.place_padded(lea_ret(5000), 8, 8);
        arena.seal();
        const Bytes before = around(old_fn);
        Installer installer;
        HookOutcome outcome;
        REQUIRE_MESSAGE(install_one(installer, old_fn.entry, new_fn.entry, "short_lea", &outcome).ok(), "");
        CHECK(outcome.strategy == Strategy::FiveByte);
        CHECK(old_fn.entry[0] == 0xE9);
        CHECK(old_fn.as_int()(2) == 5002);
        REQUIRE(revert_one(installer, old_fn.entry, "short_lea", nullptr).ok());
        CHECK(around(old_fn) == before);
        CHECK(old_fn.as_int()(2) == 2);
    }
    SUBCASE("1 byte (ret), 0x90 padding after: strategy 2 into the padding") {
        Arena arena;
        const Placed old_fn = arena.place_padded(ret_only(), 0, 15, 0xCC, 0x90);
        const Placed new_fn = arena.place_padded(lea_ret(6000), 8, 8);
        arena.seal();
        const Bytes before = around(old_fn);
        Installer installer;
        HookOutcome outcome;
        REQUIRE(install_one(installer, old_fn.entry, new_fn.entry, "ret_only", &outcome).ok());
        CHECK(outcome.strategy == Strategy::FiveByte);
        CHECK(old_fn.as_int()(3) == 6003);
        REQUIRE(revert_one(installer, old_fn.entry, "ret_only", nullptr).ok());
        CHECK(around(old_fn) == before);
    }
    SUBCASE("3 bytes with no padding on either side: refused with CML4301, nothing written") {
        Arena arena;
        const Placed old_fn = arena.place_padded(xor_ret(), 0, 0);  // 0x00 all around
        const Placed new_fn = arena.place_padded(lea_ret(7000), 8, 8);
        arena.seal();
        const Bytes before = around(old_fn);
        Installer installer;
        HookOutcome outcome;
        const BatchResult r = install_one(installer, old_fn.entry, new_fn.entry, "tiny_fn", &outcome);
        CHECK_FALSE(r.ok());
        CHECK(r.diag.code == ids::kFunctionTooShort);
        CHECK(outcome.status == HookStatus::Failed);
        CHECK(outcome.diag.message.find("tiny_fn") != std::string::npos);
        CHECK_FALSE(outcome.diag.what_to_do.empty());
        CHECK(around(old_fn) == before);
        CHECK(installer.hook_count() == 0);
        CHECK(old_fn.as_int()(1) == 0);
    }
}

TEST_CASE("fail-safe refusals name the function and change nothing") {
    Arena arena;
    const Placed new_fn = arena.place_padded(lea_ret(1), 8, 8);
    SUBCASE("an instruction the decoder does not know (VEX): CML4300") {
        const Placed old_fn = arena.place_padded(vex_first(), 0, 0);
        arena.seal();
        const Bytes before = around(old_fn);
        Installer installer;
        HookOutcome outcome;
        const BatchResult r = install_one(installer, old_fn.entry, new_fn.entry, "vex_fn", &outcome);
        CHECK(r.diag.code == ids::kDecodeUnknown);
        CHECK(outcome.diag.message.find("vex_fn") != std::string::npos);
        CHECK(outcome.diag.message.find("c5 f8") != std::string::npos);  // the bytes are quoted
        CHECK(around(old_fn) == before);
    }
    SUBCASE("a call that ends inside the overwritten bytes: CML4304") {
        const Placed old_fn = arena.place_padded(call_first(), 0, 0);
        arena.seal();
        const Bytes before = around(old_fn);
        Installer installer;
        const BatchResult r = install_one(installer, old_fn.entry, new_fn.entry, "call_fn", nullptr);
        CHECK(r.diag.code == ids::kCallInWindow);
        CHECK(around(old_fn) == before);
    }
    SUBCASE("padding in front makes the same function hookable with strategy 1") {
        const Placed old_fn = arena.place_padded(call_first(), 8, 0);
        arena.seal();
        Installer installer;
        HookOutcome outcome;
        REQUIRE(install_one(installer, old_fn.entry, new_fn.entry, "call_fn", &outcome).ok());
        CHECK(outcome.strategy == Strategy::PaddingTwoByte);
        REQUIRE(revert_one(installer, old_fn.entry, "call_fn", nullptr).ok());
    }
    SUBCASE("a target that is not executable code: CML4303") {
        arena.seal();
        Arena data;
        data.seal(PAGE_READWRITE);
        Installer installer;
        const BatchResult r = install_one(installer, data.base() + 0x800, new_fn.entry, "data_fn", nullptr);
        CHECK(r.diag.code == ids::kCodeUnreadable);
    }
    SUBCASE("a replacement that is not executable code: CML4310") {
        const Placed old_fn = arena.place_padded(cl_release(0), 8, 8);
        arena.seal();
        std::vector<std::uint8_t> data(64);
        Installer installer;
        const BatchResult r = install_one(installer, old_fn.entry, data.data(), "bad_repl", nullptr);
        CHECK(r.diag.code == ids::kBadArgument);
    }
    SUBCASE("code that ends right before an unreadable page is not guessed at: CML4303") {
        // 53 48 83 at the very end of a page, the next page PAGE_NOACCESS: the instruction is cut.
        arena.seal();
        auto* two = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        REQUIRE(two != nullptr);
        std::uint8_t* entry = two + 0x1000 - 3;
        entry[0] = 0x53;
        entry[1] = 0x48;
        entry[2] = 0x83;
        DWORD old = 0;
        VirtualProtect(two, 0x1000, PAGE_EXECUTE_READ, &old);
        VirtualProtect(two + 0x1000, 0x1000, PAGE_NOACCESS, &old);
        Installer installer;
        const BatchResult r = install_one(installer, entry, new_fn.entry, "cut_fn", nullptr);
        CHECK(r.diag.code == ids::kCodeUnreadable);
        VirtualFree(two, 0, MEM_RELEASE);
    }
}

TEST_CASE("re-pointing rewrites only the jump target and keeps the first hook's original bytes") {
    for (const bool padded : {true, false}) {
        INFO((padded ? "strategy 1" : "strategy 2"));
        Arena arena;
        const Placed old_fn = arena.place_padded(cl_release(0), padded ? 8 : 0, 16);
        const Placed new_1 = arena.place_padded(lea_ret(100), 8, 8);
        const Placed new_2 = arena.place_padded(lea_ret(200), 8, 8);
        arena.seal();
        const Bytes original = around(old_fn);
        Installer installer;
        HookOutcome outcome;
        REQUIRE(install_one(installer, old_fn.entry, new_1.entry, "fn", &outcome).ok());
        const Strategy strategy = outcome.strategy;
        CHECK(strategy == (padded ? Strategy::PaddingTwoByte : Strategy::FiveByte));
        CHECK(old_fn.as_int()(1) == 101);
        const Bytes first = around(old_fn);

        // Same replacement again: nothing is written.
        REQUIRE(install_one(installer, old_fn.entry, new_1.entry, "fn", &outcome).ok());
        CHECK(outcome.status == HookStatus::Unchanged);
        CHECK(around(old_fn) == first);

        // Another replacement: only the 4 displacement bytes change.
        const BatchResult r = install_one(installer, old_fn.entry, new_2.entry, "fn", &outcome);
        REQUIRE_MESSAGE(r.ok(), r.diag.format());
        CHECK(outcome.status == HookStatus::Repointed);
        CHECK(outcome.strategy == strategy);
        const Bytes second = around(old_fn);
        std::size_t changed = 0;
        for (std::size_t i = 0; i < first.size(); ++i) {
            changed += first[i] != second[i];
        }
        CHECK(changed <= 4);
        CHECK(changed >= 1);
        if (padded) {
            CHECK(rel32_at(old_fn.entry - 4) == distance(new_2.entry, old_fn.entry));
            CHECK(old_fn.entry[0] == 0xEB);
        } else {
            CHECK(rel32_at(old_fn.entry + 1) == distance(new_2.entry, old_fn.entry + 5));
        }
        CHECK(old_fn.as_int()(1) == 201);

        // The original bytes recorded are those of the first hook, so revert restores the real original.
        HookInfo info;
        REQUIRE(installer.find(old_fn.entry, &info));
        CHECK(info.replacement == new_2.entry);
        REQUIRE(revert_one(installer, old_fn.entry, "fn", nullptr).ok());
        CHECK(around(old_fn) == original);
        CHECK(old_fn.as_int()(1) == 1);

        // And it can be hooked again from scratch.
        REQUIRE(install_one(installer, old_fn.entry, new_1.entry, "fn", &outcome).ok());
        CHECK(outcome.status == HookStatus::Installed);
        CHECK(old_fn.as_int()(1) == 101);
        REQUIRE(revert_one(installer, old_fn.entry, "fn", nullptr).ok());
    }
}

TEST_CASE("a batch is all or nothing") {
    Arena arena;
    const Placed good_1 = arena.place_padded(cl_release(0), 8, 8);
    const Placed good_2 = arena.place_padded(cl_debug(0), 7, 8);
    const Placed tiny = arena.place_padded(xor_ret(), 0, 0);  // cannot be hooked
    const Placed repl = arena.place_padded(lea_ret(9), 8, 8);
    arena.seal();
    const Bytes snap_1 = around(good_1);
    const Bytes snap_2 = around(good_2);

    Installer installer;
    const HookRequest requests[] = {
        {good_1.entry, repl.entry, "good_1"},
        {tiny.entry, repl.entry, "tiny"},
        {good_2.entry, repl.entry, "good_2"},
    };
    std::vector<HookOutcome> out;
    const BatchResult r = installer.install(requests, 3, out);
    CHECK_FALSE(r.ok());
    CHECK(r.diag.code == ids::kFunctionTooShort);
    REQUIRE(out.size() == 3);
    CHECK(out[0].status == HookStatus::NotAttempted);
    CHECK(out[0].diag.code == ids::kBatchAborted);
    CHECK(out[1].status == HookStatus::Failed);
    CHECK(out[1].diag.code == ids::kFunctionTooShort);
    CHECK(out[2].status == HookStatus::NotAttempted);
    CHECK(around(good_1) == snap_1);
    CHECK(around(good_2) == snap_2);
    CHECK(installer.hook_count() == 0);
    CHECK(r.stats.attempts == 0);  // the world was never stopped for a request that could not succeed

    // Without the bad member the same request works, in ONE stop of the world.
    const HookRequest only_good[] = {requests[0], requests[2]};
    const BatchResult ok = installer.install(only_good, 2, out);
    REQUIRE_MESSAGE(ok.ok(), describe(ok, out));
    CHECK(ok.stats.attempts == 1);
    CHECK(out[0].status == HookStatus::Installed);
    CHECK(out[1].status == HookStatus::Installed);
    CHECK(good_1.as_int()(1) == 10);
    CHECK(good_2.as_int()(1) == 10);
    CHECK(installer.hook_count() == 2);

    // Reverting a batch that contains a function without a hook changes nothing.
    const HookRequest mixed[] = {{good_1.entry, nullptr, "good_1"}, {tiny.entry, nullptr, "tiny"}};
    const BatchResult bad_revert = installer.revert(mixed, 2, out);
    CHECK(bad_revert.diag.code == ids::kNotInstalled);
    CHECK(good_1.as_int()(1) == 10);
    CHECK(installer.hook_count() == 2);

    const HookRequest both[] = {{good_1.entry, nullptr, "good_1"}, {good_2.entry, nullptr, "good_2"}};
    REQUIRE(installer.revert(both, 2, out).ok());
    CHECK(out[0].status == HookStatus::Reverted);
    CHECK(around(good_1) == snap_1);
    CHECK(around(good_2) == snap_2);
}

TEST_CASE("malformed requests: duplicates, nulls, the function as its own replacement, overlap, unknown revert") {
    Arena arena;
    const Placed a = arena.place_padded(cl_release(0), 8, 8);
    // Two 5-byte `mov eax, imm32; ret` functions back to back (no padding): for the overlap case.
    const Placed two = arena.place_padded(cat({const_ret(1), const_ret(2)}), 0, 0);
    const Placed repl = arena.place_padded(lea_ret(9), 8, 8);
    arena.seal();
    Installer installer;
    std::vector<HookOutcome> out;

    SUBCASE("listed twice") {
        const HookRequest requests[] = {{a.entry, repl.entry, "a"}, {a.entry, repl.entry, "a_again"}};
        const BatchResult r = installer.install(requests, 2, out);
        CHECK(r.diag.code == ids::kBadArgument);
        CHECK(out[1].status == HookStatus::Failed);
        CHECK(a.as_int()(1) == 1);
    }
    SUBCASE("null target or replacement") {
        const HookRequest no_target[] = {{nullptr, repl.entry, "x"}};
        CHECK(installer.install(no_target, 1, out).diag.code == ids::kBadArgument);
        const HookRequest no_repl[] = {{a.entry, nullptr, "x"}};
        CHECK(installer.install(no_repl, 1, out).diag.code == ids::kBadArgument);
    }
    SUBCASE("replacement equal to the function") {
        const HookRequest same[] = {{a.entry, a.entry, "a"}};
        CHECK(installer.install(same, 1, out).diag.code == ids::kBadArgument);
    }
    SUBCASE("hooks whose overwritten bytes overlap: CML4316") {
        // A second function entry inside the first one's jump.
        const HookRequest requests[] = {{two.entry, repl.entry, "first"}, {two.entry + 1, repl.entry, "inside"}};
        const BatchResult r = installer.install(requests, 2, out);
        CHECK(r.diag.code == ids::kOverlap);
        CHECK(installer.hook_count() == 0);
    }
    SUBCASE("revert of a function without a hook: CML4311") {
        const HookRequest request[] = {{a.entry, nullptr, "a"}};
        CHECK(installer.revert(request, 1, out).diag.code == ids::kNotInstalled);
    }
    SUBCASE("someone else changed the bytes: CML4312, hook kept") {
        REQUIRE(install_one(installer, a.entry, repl.entry, "a", nullptr).ok());
        DWORD old = 0;
        VirtualProtect(a.entry, 16, PAGE_EXECUTE_READWRITE, &old);
        a.entry[0] = 0xEB;  // an instrumentation tool rewrote the entry
        a.entry[1] = 0x10;
        VirtualProtect(a.entry, 16, old, &old);
        const Bytes tampered = around(a);
        CHECK(revert_one(installer, a.entry, "a", nullptr).diag.code == ids::kCodeChanged);
        CHECK(around(a) == tampered);
        CHECK(installer.hook_count() == 1);
    }
    SUBCASE("an empty request is a no-op") {
        CHECK(installer.install(nullptr, 0, out).ok());
        CHECK(out.empty());
    }
}

TEST_CASE("the jump target must be within 2 GB (CML4302)") {
    Arena arena;
    const Placed old_fn = arena.place_padded(cl_release(0), 8, 8);
    arena.seal();
    // Find executable memory more than 2 GB away.
    std::uint8_t* far_block = nullptr;
    for (std::uint64_t k = 0; k < 64 && far_block == nullptr; ++k) {
        const std::uint64_t base = reinterpret_cast<std::uint64_t>(old_fn.entry);
        const std::uint64_t wanted = (base & ~0xFFFFull) + 0xA0000000ull + k * 0x10000000ull;
        far_block = static_cast<std::uint8_t*>(
            VirtualAlloc(reinterpret_cast<void*>(wanted), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    }
    if (far_block == nullptr) {
        MESSAGE("could not reserve memory 2.5 GB away; the out-of-range check is covered by test_plan only");
        return;
    }
    far_block[0] = 0xC3;
    Installer installer;
    const Bytes before = around(old_fn);
    const BatchResult r = install_one(installer, old_fn.entry, far_block, "far_fn", nullptr);
    CHECK(r.diag.code == ids::kOutOfRange);
    CHECK(around(old_fn) == before);
    VirtualFree(far_block, 0, MEM_RELEASE);
}

TEST_CASE("a write that straddles a page boundary, and protection is restored on both pages") {
    Arena arena(4);
    // Entry 3 bytes before the end of page 1: the 5-byte jump spans pages 1 and 2.
    const Placed old_fn = arena.place_at(0x2000 - 3, Bytes(), cl_release(0), Bytes(16, 0xCC));
    const Placed new_fn = arena.place_padded(lea_ret(77), 8, 8);
    arena.seal();
    const Bytes before = around(old_fn);
    // Different protections on the two pages: the installer must restore each page's own.
    DWORD ignored = 0;
    VirtualProtect(arena.base() + 0x2000, 0x1000, PAGE_EXECUTE_READWRITE, &ignored);
    REQUIRE(protection_of(old_fn.entry) == PAGE_EXECUTE_READ);
    REQUIRE(protection_of(old_fn.entry + 3) == PAGE_EXECUTE_READWRITE);

    Installer installer;
    HookOutcome outcome;
    const BatchResult r = install_one(installer, old_fn.entry, new_fn.entry, "straddle", &outcome);
    REQUIRE_MESSAGE(r.ok(), r.diag.format());
    CHECK(outcome.strategy == Strategy::FiveByte);
    CHECK(old_fn.as_int()(1) == 78);
    CHECK(protection_of(old_fn.entry) == PAGE_EXECUTE_READ);
    CHECK(protection_of(old_fn.entry + 3) == PAGE_EXECUTE_READWRITE);
    REQUIRE(revert_one(installer, old_fn.entry, "straddle", nullptr).ok());
    CHECK(around(old_fn) == before);
    CHECK(protection_of(old_fn.entry) == PAGE_EXECUTE_READ);
    CHECK(protection_of(old_fn.entry + 3) == PAGE_EXECUTE_READWRITE);
}

TEST_CASE("functions compiled by this cell's compiler: hook, re-point, revert") {
    NaturalFn volatile call = natural_orig;
    const std::uint8_t* const entry = static_cast<const std::uint8_t*>(address_of(&natural_orig));
    const Bytes before = snapshot(entry - kBefore, kBefore + kAfter);
    REQUIRE(call(5) == 1005);

    Installer installer;
    HookOutcome outcome;
    const BatchResult r =
        install_one(installer, address_of(&natural_orig), address_of(&natural_new_1), "natural_orig", &outcome);
    REQUIRE_MESSAGE(r.ok(), r.diag.format());
    INFO("strategy " << to_string(outcome.strategy) << "; entry bytes " << std::hex << int(entry[0]) << " " << int(entry[1]));
    CHECK((outcome.strategy == Strategy::PaddingTwoByte || outcome.strategy == Strategy::FiveByte));
    CHECK(call(5) == 1006);

    REQUIRE(install_one(installer, address_of(&natural_orig), address_of(&natural_new_2), "natural_orig", &outcome).ok());
    CHECK(outcome.status == HookStatus::Repointed);
    CHECK(call(5) == 1007);

    REQUIRE(revert_one(installer, address_of(&natural_orig), "natural_orig", nullptr).ok());
    CHECK(snapshot(entry - kBefore, kBefore + kAfter) == before);
    CHECK(call(5) == 1005);
    CHECK(protection_of(entry) == PAGE_EXECUTE_READ);
}

TEST_CASE("300 hooks are applied in a single stop of the world") {
    Arena arena(16);
    std::vector<Placed> olds;
    std::vector<Bytes> originals;
    const Placed repl = arena.place_padded(lea_ret(1000), 8, 8);
    for (int i = 0; i < 300; ++i) {
        // Mix of shapes so that both strategies are in one request.
        const Bytes code = (i % 3 == 0) ? cl_release(i) : (i % 3 == 1) ? cl_debug(i) : push_first(i);
        const std::size_t padding = (i % 2 == 0) ? 8 : 0;
        olds.push_back(arena.place_padded(code, padding, 8));
        originals.push_back(around(olds.back()));
    }
    arena.seal();
    std::vector<HookRequest> requests;
    for (std::size_t i = 0; i < olds.size(); ++i) {
        requests.push_back({olds[i].entry, repl.entry, "fn"});
    }
    Installer installer;
    std::vector<HookOutcome> out;
    const BatchResult r = installer.install(requests.data(), requests.size(), out);
    REQUIRE_MESSAGE(r.ok(), describe(r, out));
    CHECK(r.stats.attempts == 1);
    CHECK(installer.hook_count() == 300);
    std::size_t strategy_1 = 0;
    for (std::size_t i = 0; i < olds.size(); ++i) {
        CHECK(out[i].status == HookStatus::Installed);
        strategy_1 += out[i].strategy == Strategy::PaddingTwoByte;
        CHECK(olds[i].as_int()(1) == 1001);
    }
    CHECK(strategy_1 > 0);
    CHECK(strategy_1 < 300);
    REQUIRE(installer.revert(requests.data(), requests.size(), out).ok());
    for (std::size_t i = 0; i < olds.size(); ++i) {
        CHECK(around(olds[i]) == originals[i]);
    }
    CHECK(installer.hook_count() == 0);
}

TEST_CASE("two threads installing at once are serialised") {
    Arena arena;
    const Placed a = arena.place_padded(cl_release(0), 8, 8);
    const Placed b = arena.place_padded(cl_debug(0), 7, 8);
    const Placed repl = arena.place_padded(lea_ret(5), 8, 8);
    arena.seal();
    Installer installer;
    std::atomic<int> failures{0};
    auto work = [&](const Placed& target) {
        for (int i = 0; i < 20; ++i) {
            if (!install_one(installer, target.entry, repl.entry, "t", nullptr).ok()) {
                ++failures;
            }
            if (!revert_one(installer, target.entry, "t", nullptr).ok()) {
                ++failures;
            }
        }
    };
    std::thread t1(work, std::cref(a));
    std::thread t2(work, std::cref(b));
    t1.join();
    t2.join();
    CHECK(failures.load() == 0);
    CHECK(installer.hook_count() == 0);
}

// ---- the stopped world ---------------------------------------------------------------------------------

namespace {

struct Workers {
    std::atomic<bool> stop{false};
    std::atomic<long long> counter{0};
    std::vector<std::thread> threads;
    explicit Workers(int count) {
        for (int i = 0; i < count; ++i) {
            threads.emplace_back([this] {
                while (!stop.load(std::memory_order_relaxed)) {
                    counter.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
    }
    ~Workers() {
        stop.store(true);
        for (std::thread& t : threads) {
            t.join();
        }
    }
};

struct WindowProbe {
    std::atomic<long long>* counter = nullptr;
    long long before = -1;
    long long after = -2;
    bool forbidden = false;
    int calls = 0;
};

void probe_window(void* context) {
    auto* probe = static_cast<WindowProbe*>(context);
    ++probe->calls;
    probe->forbidden = agent_heap::alloc_forbidden();
    probe->before = probe->counter->load();
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    do {
        QueryPerformanceCounter(&now);
    } while ((now.QuadPart - start.QuadPart) * 1000 < frequency.QuadPart * 5);  // 5 ms
    probe->after = probe->counter->load();
}

}  // namespace

TEST_CASE("while the bytes are written every other thread is stopped and allocation is forbidden") {
    Arena arena;
    const Placed old_fn = arena.place_padded(cl_release(0), 8, 8);
    const Placed new_fn = arena.place_padded(lea_ret(5), 8, 8);
    arena.seal();

    Workers workers(6);
    while (workers.counter.load() < 1000) {
        std::this_thread::yield();
    }
    WindowProbe probe;
    probe.counter = &workers.counter;
    InstallerOptions options;
    options.test_in_window = &probe_window;
    options.test_context = &probe;
    Installer installer(options);

    REQUIRE(install_one(installer, old_fn.entry, new_fn.entry, "fn", nullptr).ok());
    CHECK(probe.calls == 1);
    CHECK(probe.forbidden);
    CHECK_MESSAGE(probe.before == probe.after, "counter advanced by " << (probe.after - probe.before) << " while stopped");
    CHECK_FALSE(agent_heap::alloc_forbidden());  // and it is lifted afterwards

    // After the install, the workers run again.
    const long long mark = workers.counter.load();
    Sleep(30);
    CHECK(workers.counter.load() > mark);

    // The same holds for revert.
    probe = WindowProbe{};
    probe.counter = &workers.counter;
    REQUIRE(revert_one(installer, old_fn.entry, "fn", nullptr).ok());
    CHECK(probe.calls == 1);
    CHECK(probe.before == probe.after);
}

TEST_CASE("a thread inside the first bytes of the function: retry until it leaves, then install") {
    Arena arena;
    // spin_until() runs entirely inside its first 5 bytes; no padding, so strategy 2 with a 5-byte window.
    const Placed spin = arena.place_padded(spin_until(), 0, 0);
    const Placed repl = arena.place_padded(ret_only(), 0, 0);
    arena.seal();
    const Bytes before = around(spin);

    volatile char flag = 0;
    std::atomic<DWORD> spinner_id{0};
    std::thread spinner([&] {
        spinner_id = GetCurrentThreadId();
        spin.as_void()(&flag);
    });
    while (spinner_id.load() == 0) {
        std::this_thread::yield();
    }
    Sleep(20);  // let it reach the loop

    InstallerOptions options;
    options.max_attempts = 400;  // far more than the release below needs
    Installer installer(options);

    SUBCASE("it leaves in time") {
        std::thread releaser([&] {
            Sleep(60);
            flag = 1;
        });
        HookOutcome outcome;
        const BatchResult r = install_one(installer, spin.entry, repl.entry, "spin_until", &outcome);
        releaser.join();
        spinner.join();
        REQUIRE_MESSAGE(r.ok(), r.diag.format());
        CHECK(outcome.status == HookStatus::Installed);
        CHECK(r.stats.retries >= 1);  // the RIP-in-window path ran
        CHECK(r.stats.attempts >= 2);
        CHECK(spin.entry[0] == 0xE9);
        REQUIRE(revert_one(installer, spin.entry, "spin_until", nullptr).ok());
        CHECK(around(spin) == before);
    }
    SUBCASE("it never leaves: CML4305 names the thread and the function, nothing is written") {
        options.max_attempts = 50;  // the plan's limit
        installer.set_test_options(options);
        const auto start = std::chrono::steady_clock::now();
        HookOutcome outcome;
        const BatchResult r = install_one(installer, spin.entry, repl.entry, "spin_until", &outcome);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        CHECK(r.diag.code == ids::kThreadInWindow);
        CHECK(outcome.status == HookStatus::Failed);
        CHECK(r.diag.message.find("spin_until") != std::string::npos);
        CHECK(r.diag.message.find("thread " + std::to_string(spinner_id.load())) != std::string::npos);
        CHECK(r.stats.attempts == 50);
        CHECK(r.stats.retries >= 50);
        CHECK(elapsed < std::chrono::seconds(30));  // bounded
        CHECK(around(spin) == before);
        CHECK(installer.hook_count() == 0);
        CHECK(protection_of(spin.entry) == PAGE_EXECUTE_READ);
        flag = 1;
        spinner.join();  // it was resumed and is still alive: it exits now
    }
    if (spinner.joinable()) {
        flag = 1;
        spinner.join();
    }
}

TEST_CASE("a thread inside the strategy-1 window is found too (padding written, entry not yet)") {
    Arena arena;
    // Same loop but with padding in front and a longer first instruction: the first instruction is
    // `cmp byte [rcx], 0` (3 bytes), so strategy 1's window is [entry-5, entry+3) and RIP alternates
    // between inside (entry) and outside (the je at entry+3). The install must still succeed.
    const Placed spin = arena.place_padded(spin_until(), 8, 8);
    const Placed repl = arena.place_padded(ret_only(), 8, 8);
    arena.seal();
    volatile char flag = 0;
    std::atomic<bool> started{false};
    std::thread spinner([&] {
        started = true;
        spin.as_void()(&flag);
    });
    while (!started.load()) {
        std::this_thread::yield();
    }
    Sleep(10);
    Installer installer;
    HookOutcome outcome;
    const BatchResult r = install_one(installer, spin.entry, repl.entry, "spin_until", &outcome);
    REQUIRE_MESSAGE(r.ok(), r.diag.format());
    CHECK(outcome.strategy == Strategy::PaddingTwoByte);
    flag = 1;
    spinner.join();
    REQUIRE(revert_one(installer, spin.entry, "spin_until", nullptr).ok());
}

TEST_CASE("thread enumeration converges, or fails cleanly when threads keep appearing") {
    Arena arena;
    const Placed old_fn = arena.place_padded(cl_release(0), 8, 8);
    const Placed new_fn = arena.place_padded(lea_ret(5), 8, 8);
    arena.seal();
    Workers workers(4);

    SUBCASE("a new thread in the first pass needs a second pass") {
        InstallerOptions options;
        options.test_fake_new_thread_passes = 1;
        Installer installer(options);
        const BatchResult r = install_one(installer, old_fn.entry, new_fn.entry, "fn", nullptr);
        REQUIRE_MESSAGE(r.ok(), r.diag.format());
        CHECK(r.stats.passes == 2);
        REQUIRE(revert_one(installer, old_fn.entry, "fn", nullptr).ok());
    }
    SUBCASE("threads that never stop appearing: CML4307 within the pass limit, threads running again") {
        InstallerOptions options;
        options.test_fake_new_thread_passes = 1000;
        options.max_passes = 8;
        Installer installer(options);
        const Bytes before = around(old_fn);
        const auto start = std::chrono::steady_clock::now();
        HookOutcome outcome;
        const BatchResult r = install_one(installer, old_fn.entry, new_fn.entry, "fn", &outcome);
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(30));
        CHECK(r.diag.code == ids::kThreadsNotConverging);
        CHECK(outcome.status == HookStatus::NotAttempted);
        CHECK(outcome.diag.code == ids::kThreadsNotConverging);
        CHECK(r.stats.passes == 8);
        CHECK(around(old_fn) == before);
        const long long mark = workers.counter.load();
        Sleep(30);
        CHECK(workers.counter.load() > mark);  // everything was resumed
        CHECK_FALSE(agent_heap::alloc_forbidden());
    }
    SUBCASE("threads stopped for longer than the budget: CML4309, and they run again") {
        InstallerOptions options;
        options.test_fake_new_thread_passes = 1000000;  // never converges ...
        options.max_passes = 1000000;                   // ... and the pass limit is far away,
        options.window_budget_ms = 50;                  // so the budget is what ends it
        Installer installer(options);
        const Bytes before = around(old_fn);
        const auto start = std::chrono::steady_clock::now();
        const BatchResult r = install_one(installer, old_fn.entry, new_fn.entry, "fn", nullptr);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        CHECK(r.diag.code == ids::kWindowTimeout);
        CHECK(elapsed < std::chrono::seconds(20));
        CHECK(r.stats.window_ms >= 30);  // GetTickCount64 ticks every ~15 ms
        CHECK(around(old_fn) == before);
        const long long mark = workers.counter.load();
        Sleep(30);
        CHECK(workers.counter.load() > mark);
    }
    SUBCASE("a pre-sized table smaller than the thread count is refused, not grown: CML4306") {
        InstallerOptions options;
        options.max_threads = 2;  // four workers + the test runner exist
        Installer installer(options);
        const Bytes before = around(old_fn);
        const BatchResult r = install_one(installer, old_fn.entry, new_fn.entry, "fn", nullptr);
        CHECK(r.diag.code == ids::kThreadEnumFailed);
        CHECK(around(old_fn) == before);
        const long long mark = workers.counter.load();
        Sleep(30);
        CHECK(workers.counter.load() > mark);
    }
}

TEST_CASE("threads created continuously while hooks are installed and reverted") {
    Arena arena;
    const Placed old_fn = arena.place_padded(cl_release(0), 8, 8);
    const Placed new_fn = arena.place_padded(lea_ret(5), 8, 8);
    arena.seal();

    std::atomic<bool> stop{false};
    std::atomic<long long> created{0};
    std::atomic<int> alive{0};  // children that have not finished: none may outlive this test
    // Creates short-lived threads as fast as it can. It is itself stopped by the installer, perhaps
    // inside CreateThread while holding the loader lock or the heap lock: the installer must not
    // need either.
    std::thread creator([&] {
        while (!stop.load()) {
            ++alive;
            HANDLE h = CreateThread(
                nullptr, 0,
                [](LPVOID counter) -> DWORD {
                    for (int i = 0; i < 2000; ++i) {
                        YieldProcessor();
                    }
                    static_cast<std::atomic<int>*>(counter)->fetch_sub(1);
                    return 0;
                },
                &alive, 0, nullptr);
            if (h != nullptr) {
                CloseHandle(h);
                ++created;
            } else {
                --alive;
            }
        }
    });
    while (created.load() < 100) {
        std::this_thread::yield();
    }

    Installer installer;
    int succeeded = 0;
    int refused = 0;
    long long slowest_ms = 0;
    auto timed = [&](auto&& operation) {
        const auto start = std::chrono::steady_clock::now();
        BatchResult r = operation();
        slowest_ms = std::max<long long>(
            slowest_ms,
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        return r;
    };
    const auto loop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    int rounds = 0;
    for (int i = 0; i < 40 && std::chrono::steady_clock::now() < loop_deadline; ++i) {
        ++rounds;
        const BatchResult install = timed([&] { return install_one(installer, old_fn.entry, new_fn.entry, "fn", nullptr); });
        if (install.ok()) {
            ++succeeded;
        } else {
            ++refused;
            // A refusal is a clean one: a CML43xx diagnostic, nothing written, threads running.
            CHECK_MESSAGE((install.diag.code >= 4300 && install.diag.code <= 4399), install.diag.format());
        }
        // Whatever state the hook is in, take it back out; a revert may be refused for the same reason
        // an install can, so it is tried again (a bounded number of times).
        for (int tries = 0; tries < 50 && installer.hook_count() > 0; ++tries) {
            const BatchResult revert = timed([&] { return revert_one(installer, old_fn.entry, "fn", nullptr); });
            if (!revert.ok()) {
                ++refused;
                CHECK_MESSAGE((revert.diag.code >= 4300 && revert.diag.code <= 4399), revert.diag.format());
            }
        }
    }
    stop = true;
    creator.join();
    // Let every child finish (a thread still starting up when the process exits has crashed the
    // Debug runtime's checks in the past), then give the last ones time to leave the kernel.
    for (int waited = 0; alive.load() > 0 && waited < 1000; ++waited) {
        Sleep(10);
    }
    Sleep(100);
    CHECK(alive.load() == 0);
    if (installer.hook_count() > 0) {
        REQUIRE(revert_one(installer, old_fn.entry, "fn", nullptr).ok());  // nothing creates threads any more
    }
    MESSAGE("threads created=" << created.load() << " installs ok=" << succeeded << " refused=" << refused
                                << " slowest=" << slowest_ms << " ms");
    CHECK(rounds >= 1);
    CHECK(succeeded + refused >= rounds);
    CHECK(succeeded >= 1);
    CHECK(slowest_ms < 10000);
    CHECK(installer.hook_count() == 0);
    CHECK(old_fn.as_int()(1) == 1);
}

TEST_CASE("which threads exist after the tests above (information for the exit crash hunt)") {
    // Not an assertion on the exact number: Windows may keep thread-pool threads. The start address of
    // every thread other than this one is printed so that a thread of ours that was left behind shows up.
    Sleep(200);
    using QueryFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<QueryFn>(
        reinterpret_cast<void (*)()>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    REQUIRE(snapshot != INVALID_HANDLE_VALUE);
    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    int threads = 0;
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId()) {
            continue;
        }
        ++threads;
        std::uintptr_t start = 0;
        if (HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID)) {
            if (query != nullptr) {
                query(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), nullptr);
            }
            CloseHandle(h);
        }
        MESSAGE("thread " << entry.th32ThreadID << (entry.th32ThreadID == GetCurrentThreadId() ? " (this one)" : "")
                          << " starts at " << hooktest::symbolize(start));
    }
    CloseHandle(snapshot);
    CHECK(threads < 16);
}

#endif  // _WIN32
