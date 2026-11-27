// Strategy selection (agent/src/hook/hook_plan.h): which hook strategy a function gets, how wide
// the RIP window is, and which functions are refused. Portable (byte vectors only).
#include "hook/hook_plan.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "hook/diag_ids.h"
#include "hook/hook_timing.h"
#include "doctest.h"

using namespace cml::agent::hook;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes concat(const Bytes& a, const Bytes& b) {
    Bytes r = a;
    r.insert(r.end(), b.begin(), b.end());
    return r;
}

Bytes pad(std::size_t n, std::uint8_t value = 0xCC) {
    return Bytes(n, value);
}

HookPlan plan(const Bytes& before, const Bytes& at) {
    // Keep only what plan_hook may look at: the last 8 bytes before and the first 32 from the entry.
    Bytes b = before;
    if (b.size() > kLookBehind) {
        b.erase(b.begin(), b.end() - static_cast<std::ptrdiff_t>(kLookBehind));
    }
    Bytes a = at;
    if (a.size() > kLookAhead) {
        a.resize(kLookAhead);
    }
    return plan_hook(b.data(), b.size(), a.data(), a.size());
}

const Bytes kClDebugEntry = {0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0xE8, 0, 0, 0, 0, 0x5F, 0xC3};
const Bytes kClReleaseEntry = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0, 0, 0, 0, 0x48, 0x83, 0xC4, 0x28, 0xC3};

}  // namespace

TEST_CASE("ADR-0020: padding before the entry selects strategy 1 for every cell's entry bytes") {
    SUBCASE("cl Debug: 7 bytes of CC, entry 40 57 ...") {
        const HookPlan p = plan(pad(7), kClDebugEntry);
        REQUIRE(p.strategy == Strategy::PaddingTwoByte);
        CHECK(p.diag == 0);
        CHECK(p.first_insn_len == 2);  // push rdi with REX
        CHECK(p.entry_write_len == 2);
        CHECK(p.padding_write_len == 5);
        CHECK(p.window_before == 5);
        CHECK(p.window_after == 2);
    }
    SUBCASE("cl Release: 8 bytes of CC, entry 48 83 ec 28") {
        const HookPlan p = plan(pad(8), kClReleaseEntry);
        REQUIRE(p.strategy == Strategy::PaddingTwoByte);
        CHECK(p.first_insn_len == 4);
        CHECK(p.window_before == 5);
        CHECK(p.window_after == 4);
    }
    SUBCASE("clang-cl: 16 bytes of CC") {
        const HookPlan p = plan(pad(16), kClReleaseEntry);
        REQUIRE(p.strategy == Strategy::PaddingTwoByte);
    }
    SUBCASE("exactly 6 bytes of padding is enough, 5 is not") {
        CHECK(plan(pad(6), kClReleaseEntry).strategy == Strategy::PaddingTwoByte);
        const HookPlan five = plan(concat(Bytes{0xC3}, pad(5)), kClReleaseEntry);
        CHECK(five.strategy == Strategy::FiveByte);
    }
    SUBCASE("0x90 padding, and a mix of CC and 90") {
        CHECK(plan(pad(8, 0x90), kClReleaseEntry).strategy == Strategy::PaddingTwoByte);
        CHECK(plan(Bytes{0xCC, 0x90, 0xCC, 0x90, 0xCC, 0x90}, kClReleaseEntry).strategy == Strategy::PaddingTwoByte);
    }
    SUBCASE("a non-padding byte inside the last six disables strategy 1") {
        CHECK(plan(Bytes{0xCC, 0xCC, 0xCC, 0xCC, 0x00, 0xCC, 0xCC, 0xCC}, kClReleaseEntry).strategy == Strategy::FiveByte);
    }
}

TEST_CASE("strategy 1 needs a first instruction of at least two bytes") {
    // push rbx (1 byte) / sub rsp, 20h / ... with padding: strategy 1 refused, strategy 2 used.
    const Bytes one_byte_first = {0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8, 0, 0, 0, 0, 0xC3};
    const HookPlan p = plan(pad(16), one_byte_first);
    REQUIRE(p.strategy == Strategy::FiveByte);
    CHECK(p.first_insn_len == 1);
    CHECK(p.entry_write_len == 5);
    CHECK(p.padding_write_len == 0);
    CHECK(p.window_before == 0);
    CHECK(p.window_after == 5);  // push (1) + sub (4) end exactly on the jump's end
    CHECK_FALSE(p.short_function);
}

TEST_CASE("strategy 2 covers whole instructions: the window ends at the last touched instruction") {
    SUBCASE("sub rsp,28h (4 bytes) then a 5-byte call: window 9, the call ends on the boundary") {
        const HookPlan p = plan({}, kClReleaseEntry);
        REQUIRE(p.strategy == Strategy::FiveByte);
        CHECK(p.window_after == 9);
        CHECK(p.window_before == 0);
    }
    SUBCASE("push rdi (REX) + sub rsp,20h = 6 bytes") {
        const HookPlan p = plan({}, kClDebugEntry);
        REQUIRE(p.strategy == Strategy::FiveByte);
        CHECK(p.window_after == 6);
    }
    SUBCASE("a first instruction of 5 or more bytes alone") {
        const HookPlan p = plan({}, Bytes{0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3});  // lea rax,[rip+0] (7)
        REQUIRE(p.strategy == Strategy::FiveByte);
        CHECK(p.window_after == 7);
    }
    SUBCASE("a 5-byte jmp thunk (incremental linking) is replaced exactly") {
        const Bytes thunk = {0xE9, 0x10, 0x00, 0x00, 0x00, 0xE9, 0x20, 0x00, 0x00, 0x00};
        const HookPlan p = plan({}, thunk);
        REQUIRE(p.strategy == Strategy::FiveByte);
        CHECK(p.window_after == 5);
    }
    SUBCASE("no padding but readable bytes before the entry that are not padding") {
        const HookPlan p = plan(Bytes{0x5D, 0xC3, 0x48, 0x89, 0xE5, 0x90, 0x41, 0x5F}, kClReleaseEntry);
        CHECK(p.strategy == Strategy::FiveByte);
    }
}

TEST_CASE("functions shorter than 5 bytes") {
    const Bytes xor_ret = {0x33, 0xC0, 0xC3};                   // xor eax, eax; ret   (3 bytes)
    const Bytes ret_only = {0xC3};
    const Bytes mov_ret = {0xB0, 0x01, 0xC3};                   // mov al, 1; ret      (3 bytes)

    SUBCASE("with padding before: strategy 1 (needs no trailing bytes)") {
        const HookPlan p = plan(pad(16), concat(xor_ret, pad(13)));
        REQUIRE(p.strategy == Strategy::PaddingTwoByte);
        CHECK(p.window_after == 2);
    }
    SUBCASE("with padding before but a 1-byte function: strategy 2 into the trailing padding") {
        const HookPlan p = plan(pad(16), concat(ret_only, pad(15)));
        REQUIRE(p.strategy == Strategy::FiveByte);
        CHECK(p.short_function);
        CHECK(p.window_after == 5);
    }
    SUBCASE("no padding before, CC padding after: strategy 2 over the function and 2 padding bytes") {
        const HookPlan p = plan({}, concat(xor_ret, pad(13)));
        REQUIRE(p.strategy == Strategy::FiveByte);
        CHECK(p.short_function);
        CHECK(p.window_after == 5);
        CHECK(p.entry_write_len == 5);
    }
    SUBCASE("trailing 0x90 padding counts too") {
        const HookPlan p = plan({}, concat(mov_ret, pad(13, 0x90)));
        REQUIRE(p.strategy == Strategy::FiveByte);
        CHECK(p.short_function);
    }
    SUBCASE("no padding before and no padding after: refused with CML4301") {
        const HookPlan p = plan({}, concat(xor_ret, Bytes{0x48, 0x89, 0x5C, 0x24, 0x08, 0x57}));
        CHECK(p.strategy == Strategy::None);
        CHECK(p.diag == ids::kFunctionTooShort);
    }
    SUBCASE("only some of the missing bytes are padding: refused") {
        // xor/ret (3) + CC + 48 ...: byte 4 is padding, byte 5 is not
        const HookPlan p = plan({}, concat(xor_ret, Bytes{0xCC, 0x48, 0x89, 0x5C, 0x24}));
        CHECK(p.strategy == Strategy::None);
        CHECK(p.diag == ids::kFunctionTooShort);
    }
    SUBCASE("a jmp rel8 stub is a function that ends the flow") {
        const Bytes stub = {0xEB, 0x10};
        CHECK(plan({}, concat(stub, Bytes{0x48, 0x89, 0x5C})).diag == ids::kFunctionTooShort);
        CHECK(plan({}, concat(stub, pad(14))).strategy == Strategy::FiveByte);
    }
    SUBCASE("int3 as the first instruction ends the flow as well") {
        CHECK(plan({}, Bytes{0xCC, 0x48, 0x83, 0xEC, 0x28, 0xC3}).diag == ids::kFunctionTooShort);
    }
    SUBCASE("too few readable bytes after a short function: CML4303, not a guess") {
        const HookPlan p = plan({}, Bytes{0x33, 0xC0, 0xC3, 0xCC});
        CHECK(p.strategy == Strategy::None);
        CHECK(p.diag == ids::kCodeUnreadable);
    }
}

TEST_CASE("a call that ends inside the overwritten range is refused (CML4304)") {
    SUBCASE("call rax (2 bytes) followed by more code, no padding") {
        const HookPlan p = plan({}, Bytes{0xFF, 0xD0, 0x48, 0x83, 0xEC, 0x28, 0xC3});
        CHECK(p.strategy == Strategy::None);
        CHECK(p.diag == ids::kCallInWindow);
        CHECK(p.fail_offset == 2);
    }
    SUBCASE("a call that is the last touched instruction is fine: its return address is outside") {
        const HookPlan p = plan({}, Bytes{0x53, 0xE8, 0, 0, 0, 0, 0xC3});  // push rbx; call rel32
        REQUIRE(p.strategy == Strategy::FiveByte);
        CHECK(p.window_after == 6);
    }
    SUBCASE("call rax as the short function's first instruction, then ret and padding") {
        const HookPlan p = plan({}, Bytes{0xFF, 0xD0, 0xC3, 0xCC, 0xCC, 0xCC});
        CHECK(p.diag == ids::kCallInWindow);
    }
    SUBCASE("strategy 1 may start with a call: the 2 bytes written end the call's encoding") {
        const HookPlan p = plan(pad(8), Bytes{0xFF, 0xD0, 0x48, 0x83, 0xEC, 0x28, 0xC3});
        REQUIRE(p.strategy == Strategy::PaddingTwoByte);
        CHECK(p.window_after == 2);
    }
}

TEST_CASE("unknown instructions fail safe (CML4300) with the offset and bytes") {
    // The unknown instruction is 3DNow! (0F 0F C1 9E, pfadd mm0, mm1): VEX was the example here until WP-38 taught
    // the decoder VEX and EVEX (the test case below).
    SUBCASE("unknown first instruction") {
        const HookPlan p = plan(pad(16), Bytes{0x0F, 0x0F, 0xC1, 0x9E, 0xC3});
        CHECK(p.strategy == Strategy::None);
        CHECK(p.diag == ids::kDecodeUnknown);
        CHECK(p.fail_offset == 0);
        CHECK(p.fail_byte_count == 4);
        CHECK(p.fail_bytes[0] == 0x0F);
    }
    SUBCASE("unknown instruction inside the 5-byte range of strategy 2") {
        const HookPlan p = plan({}, Bytes{0x53, 0x0F, 0x0F, 0xC1, 0x9E, 0xC3});
        CHECK(p.diag == ids::kDecodeUnknown);
        CHECK(p.fail_offset == 1);
    }
    SUBCASE("an unknown instruction AFTER the range is irrelevant") {
        const HookPlan p = plan({}, Bytes{0x48, 0x83, 0xEC, 0x28, 0x90, 0x0F, 0x0F, 0xC1, 0x9E});
        REQUIRE(p.strategy == Strategy::FiveByte);
        CHECK(p.window_after == 5);
    }
    SUBCASE("strategy 1 only needs the first instruction to decode") {
        const HookPlan p = plan(pad(8), Bytes{0x48, 0x83, 0xEC, 0x28, 0x0F, 0x0F});
        CHECK(p.strategy == Strategy::PaddingTwoByte);
    }
    SUBCASE("a VEX instruction with a 66 prefix in front is #UD, not an instruction: refused") {
        const HookPlan p = plan(pad(16), Bytes{0x66, 0xC5, 0xF8, 0x28, 0xC1, 0xC3});
        CHECK(p.strategy == Strategy::None);
        CHECK(p.diag == ids::kDecodeUnknown);
        CHECK(p.fail_bytes[0] == 0x66);
    }
    SUBCASE("a VEX opcode map the decoder does not cover: refused") {
        const HookPlan p = plan({}, Bytes{0xC4, 0xE8, 0x78, 0x85, 0xC1, 0x00, 0xC3});  // map 8
        CHECK(p.strategy == Strategy::None);
        CHECK(p.diag == ids::kDecodeUnknown);
        CHECK(p.fail_bytes[0] == 0xC4);
    }
}

// WP-38 (ADR-0042): a function that begins with a VEX or EVEX instruction is planned like any other. The lengths in
// the comments are worked out by hand (test_length_decoder.cpp explains how).
TEST_CASE("functions that begin with VEX or EVEX instructions get a hook plan") {
    SUBCASE("padding in front: strategy 1, the window is the whole first instruction") {
        // cl /Od /arch:AVX homes a float argument first: vmovss [rsp+8], xmm0 = C5 FA 11 44 24 08 (6 bytes).
        const HookPlan p = plan(pad(8), Bytes{0xC5, 0xFA, 0x11, 0x44, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x10});
        REQUIRE(p.strategy == Strategy::PaddingTwoByte);
        CHECK(p.first_insn_len == 6);
        CHECK(p.entry_write_len == 2);
        CHECK(p.window_before == 5);
        CHECK(p.window_after == 6);
    }
    SUBCASE("padding in front, vzeroupper first (3 bytes, no ModRM): strategy 1") {
        const HookPlan p = plan(pad(16), Bytes{0xC5, 0xF8, 0x77, 0xC3});
        REQUIRE(p.strategy == Strategy::PaddingTwoByte);
        CHECK(p.first_insn_len == 3);
        CHECK(p.window_after == 3);
    }
    SUBCASE("no padding: two 4-byte VEX instructions, the jump ends inside the second") {
        // vmovdqu ymm0, [rcx] ; vmovdqu [rdx], ymm0 ; vzeroupper ; ret   (the MSVC STL's _Load / store helpers)
        const HookPlan p = plan({}, Bytes{0xC5, 0xFE, 0x6F, 0x01, 0xC5, 0xFE, 0x7F, 0x02, 0xC5, 0xF8, 0x77, 0xC3});
        REQUIRE(p.strategy == Strategy::FiveByte);
        CHECK(p.first_insn_len == 4);
        CHECK(p.entry_write_len == 5);
        CHECK(p.window_after == 8);
        CHECK_FALSE(p.short_function);
    }
    SUBCASE("no padding: vzeroupper (3) then sub rsp, 28h (4): window 7") {
        const HookPlan p = plan({}, Bytes{0xC5, 0xF8, 0x77, 0x48, 0x83, 0xEC, 0x28, 0xC3});
        REQUIRE(p.strategy == Strategy::FiveByte);
        CHECK(p.window_after == 7);
    }
    SUBCASE("no padding: a three-byte VEX prefix with an immediate is 6 bytes, one instruction covers the jump") {
        const HookPlan p = plan({}, Bytes{0xC4, 0xE3, 0xFD, 0x00, 0xC1, 0x1B, 0xC3});  // vpermq ymm0, ymm1, 1Bh ; ret
        REQUIRE(p.strategy == Strategy::FiveByte);
        CHECK(p.first_insn_len == 6);
        CHECK(p.window_after == 6);
    }
    SUBCASE("no padding: the 5-byte VEX instruction is exactly the jump") {
        const HookPlan p = plan({}, Bytes{0xC4, 0xE1, 0xF9, 0x6E, 0xC1, 0xC3});  // vmovq xmm0, rcx ; ret
        REQUIRE(p.strategy == Strategy::FiveByte);
        CHECK(p.window_after == 5);
    }
    SUBCASE("EVEX, with and without padding") {
        const Bytes code = {0x62, 0xF1, 0x7C, 0x48, 0x10, 0x41, 0x01, 0xC3};  // vmovups zmm0, [rcx+40h] ; ret
        const HookPlan padded = plan(pad(8), code);
        REQUIRE(padded.strategy == Strategy::PaddingTwoByte);
        CHECK(padded.window_after == 7);
        const HookPlan bare = plan({}, code);
        REQUIRE(bare.strategy == Strategy::FiveByte);
        CHECK(bare.window_after == 7);
    }
    SUBCASE("vzeroupper ; ret is 4 bytes: a short function, hooked only when padding follows") {
        const HookPlan with_tail = plan({}, Bytes{0xC5, 0xF8, 0x77, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC});
        REQUIRE(with_tail.strategy == Strategy::FiveByte);
        CHECK(with_tail.short_function);
        CHECK(with_tail.window_after == 5);
        const HookPlan without = plan({}, Bytes{0xC5, 0xF8, 0x77, 0xC3, 0x48, 0x83, 0xEC, 0x28});
        CHECK(without.strategy == Strategy::None);
        CHECK(without.diag == ids::kFunctionTooShort);
    }
    SUBCASE("a call that ends inside the overwritten bytes is still refused when VEX follows it") {
        const HookPlan p = plan({}, Bytes{0xFF, 0xD0, 0xC5, 0xF8, 0x77, 0xC3});  // call rax ; vzeroupper ; ret
        CHECK(p.strategy == Strategy::None);
        CHECK(p.diag == ids::kCallInWindow);
    }
    SUBCASE("the readable bytes end inside a VEX instruction: CML4303, not a guess") {
        const Bytes cut = {0xC4, 0xE3, 0xFD, 0x00, 0xC1};  // vpermq without its immediate
        CHECK(plan_hook(nullptr, 0, cut.data(), cut.size()).diag == ids::kCodeUnreadable);
    }
}

TEST_CASE("unreadable or truncated code is reported as CML4303") {
    CHECK(plan_hook(nullptr, 0, nullptr, 0).diag == ids::kCodeUnreadable);
    const Bytes one = {0x48};
    CHECK(plan_hook(nullptr, 0, one.data(), one.size()).diag == ids::kCodeUnreadable);
    // The readable range ends inside the second instruction of the 5-byte range.
    const Bytes cut = {0x53, 0x48, 0x83};
    CHECK(plan_hook(nullptr, 0, cut.data(), cut.size()).diag == ids::kCodeUnreadable);
}

TEST_CASE("encode_jmp_rel32 writes E9 and a little-endian displacement relative to the next instruction") {
    std::uint8_t out[5];
    REQUIRE(encode_jmp_rel32(out, 0x140001000ull, 0x140001105ull));
    CHECK(out[0] == 0xE9);
    CHECK(out[1] == 0x00);
    CHECK(out[2] == 0x01);  // 0x140001105 - (0x140001000 + 5) = 0x100
    CHECK(out[3] == 0x00);
    CHECK(out[4] == 0x00);

    REQUIRE(encode_jmp_rel32(out, 0x140001000ull, 0x140000000ull));  // backwards
    std::int32_t rel;
    std::memcpy(&rel, out + 1, 4);
    CHECK(rel == -0x1005);

    // Reach: exactly +-2 GB works, one byte more does not.
    const std::uint64_t site = 0x140000000ull;
    CHECK(encode_jmp_rel32(out, site, site + 5 + 0x7FFFFFFFull));
    CHECK_FALSE(encode_jmp_rel32(out, site, site + 5 + 0x80000000ull));
    CHECK(encode_jmp_rel32(out, site, site + 5 - 0x80000000ull));
    CHECK_FALSE(encode_jmp_rel32(out, site, site + 4 - 0x80000000ull));
    // A distance of several GB is out of reach in either direction.
    CHECK_FALSE(encode_jmp_rel32(out, 0x7FF600000000ull, 0x140000000ull));
    CHECK_FALSE(encode_jmp_rel32(out, 0x140000000ull, 0x7FF600000000ull));
}

TEST_CASE("strategy 1 needs a 2-byte aligned entry and executable padding; otherwise strategy 2") {
    const Bytes before = pad(8);
    PlanEnvironment env;
    SUBCASE("defaults (even entry, executable padding): strategy 1") {
        CHECK(plan_hook(before.data(), before.size(), kClReleaseEntry.data(), kClReleaseEntry.size(), env).strategy ==
              Strategy::PaddingTwoByte);
    }
    SUBCASE("an odd entry falls back to the 5-byte strategy") {
        env.entry_two_byte_aligned = false;
        const HookPlan p = plan_hook(before.data(), before.size(), kClReleaseEntry.data(), kClReleaseEntry.size(), env);
        REQUIRE(p.strategy == Strategy::FiveByte);
        CHECK(p.entry_write_len == 5);
        CHECK(p.padding_write_len == 0);
        CHECK(p.window_before == 0);
    }
    SUBCASE("padding that is not executable falls back to the 5-byte strategy") {
        env.padding_executable = false;
        const HookPlan p = plan_hook(before.data(), before.size(), kClReleaseEntry.data(), kClReleaseEntry.size(), env);
        REQUIRE(p.strategy == Strategy::FiveByte);
        CHECK(p.padding_write_len == 0);
    }
    SUBCASE("the fallback still refuses what strategy 2 refuses") {
        env.entry_two_byte_aligned = false;
        const Bytes call_first = {0xFF, 0xD0, 0x48, 0x83, 0xEC, 0x28, 0xC3};  // call rax ; sub rsp,28h ; ret
        const HookPlan p = plan_hook(before.data(), before.size(), call_first.data(), call_first.size(), env);
        CHECK(p.strategy == Strategy::None);
        CHECK(p.diag == ids::kCallInWindow);
    }
}

TEST_CASE("to_string names the strategies") {
    CHECK(std::string(to_string(Strategy::PaddingTwoByte)) == "padding+2-byte");
    CHECK(std::string(to_string(Strategy::FiveByte)) == "5-byte");
    CHECK(std::string(to_string(Strategy::None)) == "none");
}

TEST_CASE("WP-50: the WP50-HOOKS line carries every phase under a stable key, in order") {
    HooksTimingLine l;
    l.request_id = 17;
    l.batch = 1;
    l.op = "install";
    l.hooks = 275;
    l.threads = 3;
    l.attempts = 1;
    l.batch_timing.installs = 275;
    l.batch_timing.pages = 61;
    l.batch_timing.plan_us = 1200;
    l.batch_timing.write_us = 380000;
    l.batch_timing.write_max_us = 9000;
    l.dispatch.total_us = 395000;
    l.dispatch.install_us = 392000;
    const std::string line = format_hooks_timing(l);
    CHECK(line.rfind("WP50-HOOKS side=agent request=17 batch=1 op=install hooks=275 installed=275 repointed=0 unchanged=0 reverted=0 pages=61 "
                     "threads=3 attempts=1 status=0 total_us=395000 ",
                     0) == 0);
    const char* const keys[] = {"wait_us",      "collect_us", "prepatch_us",   "build_us",     "install_us", "lock_us",     "plan_us",
                                "probe_us",     "query_us", "queries", "protect_us", "stop_us",       "enumerate_us", "suspend_us", "context_us",  "check_us",
                                "write_us",     "write_max_us", "step_us",     "resume_us",    "retry_wait_us", "unprotect_us", "commit_us",
                                "rollback_us",  "postpatch_us", "reply_us"};
    std::size_t at = 0;
    for (const char* key : keys) {
        const std::size_t found = line.find(std::string(" ") + key + "=", at);
        CHECK_MESSAGE(found != std::string::npos, key);
        if (found != std::string::npos) {
            at = found + 1;
        }
    }
    CHECK(line.find(" write_us=380000 write_max_us=9000 ") != std::string::npos);
    CHECK(line.find(" plan_us=1200 ") != std::string::npos);
    CHECK(line.find('\n') == std::string::npos);


    l.status = 4301;
    l.op = "revert";
    const std::string refused = format_hooks_timing(l);
    CHECK(refused.find(" op=revert ") != std::string::npos);
    CHECK(refused.find(" status=CML4301 total_us=") != std::string::npos);
}
