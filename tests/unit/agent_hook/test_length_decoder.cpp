// The x64 instruction-length decoder (agent/src/hook/length_decoder.h). Portable: byte vectors only.
// Every length below was worked out by hand from the Intel SDM opcode maps (not produced by running
// the decoder), so a table typo shows up as a failing vector.
#include "hook/length_decoder.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "doctest.h"

using namespace cml::agent::hook;

namespace {

using Bytes = std::vector<std::uint8_t>;

std::string hex(const Bytes& b) {
    static const char kDigits[] = "0123456789ABCDEF";
    std::string s;
    for (std::uint8_t v : b) {
        s += kDigits[v >> 4];
        s += kDigits[v & 15];
        s += ' ';
    }
    return s;
}

// Decodes exactly `bytes` (the vector is the whole readable range, so over-reads would be caught
// by sanitizers) and requires one instruction of `expected` bytes.
DecodeResult decode_exact(const Bytes& bytes) {
    return decode_instruction(bytes.data(), bytes.size());
}

void expect_len(const Bytes& bytes, unsigned expected, std::uint8_t flags = 0) {
    INFO("bytes: " << hex(bytes));
    const DecodeResult r = decode_exact(bytes);
    REQUIRE(r.status == DecodeStatus::Ok);
    CHECK(r.length == expected);
    CHECK(r.flags == flags);
    // The length is independent of what follows the instruction.
    Bytes longer = bytes;
    longer.resize(bytes.size() + 16, 0xCC);
    const DecodeResult r2 = decode_instruction(longer.data(), longer.size());
    REQUIRE(r2.status == DecodeStatus::Ok);
    CHECK(r2.length == expected);
    // And a window that ends one byte early is reported as truncated, never as a shorter instruction.
    const DecodeResult r3 = decode_instruction(bytes.data(), bytes.size() - 1);
    CHECK(r3.status == DecodeStatus::Truncated);
}

void expect_unknown(const Bytes& bytes) {
    INFO("bytes: " << hex(bytes));
    // Pad so that the verdict cannot be "truncated".
    Bytes padded = bytes;
    padded.resize(bytes.size() + 16, 0x00);
    const DecodeResult r = decode_instruction(padded.data(), padded.size());
    CHECK(r.status == DecodeStatus::Unknown);
    CHECK(r.length == 0);
}

struct Case {
    Bytes bytes;
    unsigned length;
    std::uint8_t flags;
};

}  // namespace

TEST_CASE("ADR-0020 entry sequences decode to the expected boundaries") {
    SUBCASE("cl Debug: 40 57 48 83 ec 20 (push rdi with REX, sub rsp, 20h)") {
        const Bytes code = {0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9};
        const DecodeResult first = decode_instruction(code.data(), code.size());
        REQUIRE(first.status == DecodeStatus::Ok);
        CHECK(first.length == 2);  // the REX-prefixed push is two bytes: strategy 1 is allowed
        const DecodeResult second = decode_instruction(code.data() + 2, code.size() - 2);
        REQUIRE(second.status == DecodeStatus::Ok);
        CHECK(second.length == 4);
    }
    SUBCASE("cl Release, clang-cl: 48 83 ec 28 (sub rsp, 28h)") {
        const Bytes code = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0x00, 0x00, 0x00, 0x00};
        const DecodeResult first = decode_instruction(code.data(), code.size());
        REQUIRE(first.status == DecodeStatus::Ok);
        CHECK(first.length == 4);  // a 5-byte jump would split this instruction
    }
}

TEST_CASE("push and pop, with and without REX") {
    expect_len({0x50}, 1);
    expect_len({0x53}, 1);
    expect_len({0x55}, 1);
    expect_len({0x57}, 1);
    expect_len({0x5D}, 1);
    expect_len({0x40, 0x57}, 2);
    expect_len({0x41, 0x57}, 2);
    expect_len({0x41, 0x5F}, 2);
    expect_len({0x48, 0x55}, 2);
    expect_len({0x6A, 0x00}, 2);
    expect_len({0x68, 0x00, 0x01, 0x00, 0x00}, 5);
    expect_len({0x66, 0x68, 0x01, 0x00}, 4);  // push imm16
    expect_len({0xFF, 0x35, 0x00, 0x00, 0x00, 0x00}, 6, kInsnRipRel);  // push [rip+0]
    expect_len({0xFF, 0x74, 0x24, 0x08}, 4);
    expect_len({0x8F, 0xC0}, 2);  // pop rax, long form
}

TEST_CASE("mov and lea with ModRM, SIB and displacements") {
    expect_len({0x4C, 0x8B, 0xDC}, 3);                    // mov r11, rsp
    expect_len({0x48, 0x8B, 0xEC}, 3);                    // mov rbp, rsp
    expect_len({0x48, 0x89, 0x5C, 0x24, 0x08}, 5);        // mov [rsp+8], rbx   (SIB, disp8)
    expect_len({0x48, 0x89, 0x74, 0x24, 0x10}, 5);        // mov [rsp+10h], rsi
    expect_len({0x4C, 0x89, 0x44, 0x24, 0x18}, 5);        // mov [rsp+18h], r8
    expect_len({0x89, 0x4C, 0x24, 0x08}, 4);              // mov [rsp+8], ecx
    expect_len({0x89, 0x4D, 0xFC}, 3);                    // mov [rbp-4], ecx   (disp8, no SIB)
    expect_len({0x8B, 0x45, 0xFC}, 3);
    expect_len({0x48, 0x8B, 0x01}, 3);                    // mov rax, [rcx]     (mod 0)
    expect_len({0x48, 0x8B, 0x04, 0x24}, 4);              // mov rax, [rsp]     (mod 0 + SIB)
    expect_len({0x8B, 0x04, 0x8D, 0x00, 0x00, 0x00, 0x00}, 7);  // mov eax, [rcx*4+0] (SIB base=5, mod 0: disp32)
    expect_len({0x8B, 0x45, 0x00}, 3);                    // mov eax, [rbp+0]   (mod 1, disp8)
    expect_len({0x8B, 0x85, 0x00, 0x01, 0x00, 0x00}, 6);  // mov eax, [rbp+100h] (mod 2, disp32)
    expect_len({0x48, 0x8B, 0x84, 0x24, 0x00, 0x01, 0x00, 0x00}, 8);  // mov rax, [rsp+100h]
    expect_len({0x48, 0x8D, 0x05, 0x10, 0x00, 0x00, 0x00}, 7, kInsnRipRel);  // lea rax, [rip+10h]
    expect_len({0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00}, 7, kInsnRipRel);  // mov rax, [rip+0]
    expect_len({0x8B, 0x05, 0x11, 0x22, 0x33, 0x44}, 6, kInsnRipRel);
    expect_len({0x4C, 0x8D, 0x1D, 0x00, 0x00, 0x00, 0x00}, 7, kInsnRipRel);  // lea r11, [rip+0]
    expect_len({0x48, 0x8D, 0x4C, 0x24, 0x20}, 5);        // lea rcx, [rsp+20h]
    expect_len({0x48, 0x8D, 0x54, 0x24, 0x28}, 5);
    expect_len({0x67, 0x8B, 0x00}, 3);                    // mov eax, [eax]     (67 prefix: same layout)
    expect_len({0x64, 0x48, 0x8B, 0x04, 0x25, 0x30, 0x00, 0x00, 0x00}, 9);  // mov rax, fs/gs:[30h] (SIB no base)
    expect_len({0x65, 0x48, 0x8B, 0x04, 0x25, 0x60, 0x00, 0x00, 0x00}, 9);  // mov rax, gs:[60h]
    expect_len({0x48, 0x63, 0xD1}, 3);                    // movsxd rdx, ecx
    expect_len({0x0F, 0xB6, 0xC1}, 3);                    // movzx eax, cl
    expect_len({0x0F, 0xB7, 0x01}, 3);
    expect_len({0x48, 0x0F, 0xBE, 0xC1}, 4);              // movsx rax, cl
    expect_len({0x87, 0x01}, 2);                          // xchg
    expect_len({0xF0, 0x0F, 0xB1, 0x0A}, 4);              // lock cmpxchg [rdx], ecx
    expect_len({0xF0, 0x48, 0x0F, 0xC1, 0x01}, 5);        // lock xadd [rcx], rax
}

TEST_CASE("mov with immediates") {
    expect_len({0xB8, 0x01, 0x00, 0x00, 0x00}, 5);        // mov eax, 1
    expect_len({0xB9, 0x01, 0x00, 0x00, 0x00}, 5);
    expect_len({0x41, 0xB8, 0x01, 0x00, 0x00, 0x00}, 6);  // mov r8d, 1
    expect_len({0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8}, 10); // movabs rax, imm64
    expect_len({0x49, 0xBA, 1, 2, 3, 4, 5, 6, 7, 8}, 10);
    expect_len({0x66, 0xB8, 0x01, 0x00}, 4);              // mov ax, 1
    expect_len({0xB0, 0x01}, 2);                          // mov al, 1
    expect_len({0xB7, 0x01}, 2);
    expect_len({0xC7, 0x05, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}, 10, kInsnRipRel);  // mov dword [rip+0], 1
    expect_len({0xC7, 0x44, 0x24, 0x08, 0x00, 0x00, 0x00, 0x00}, 8);   // mov dword [rsp+8], 0
    expect_len({0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00}, 7);          // mov rax, 1 (sign-extended imm32)
    expect_len({0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF}, 9);  // mov qword [rsp+20h], -2
    expect_len({0xC6, 0x44, 0x24, 0x08, 0x00}, 5);        // mov byte [rsp+8], 0
    expect_len({0xC6, 0x00, 0x01}, 3);
    expect_len({0x66, 0xC7, 0x00, 0x01, 0x00}, 5);        // mov word [rax], 1
}

TEST_CASE("ALU instructions: sub/add rsp, and/or/xor/cmp/test") {
    expect_len({0x48, 0x83, 0xEC, 0x28}, 4);              // sub rsp, 28h
    expect_len({0x48, 0x83, 0xEC, 0x20}, 4);
    expect_len({0x48, 0x81, 0xEC, 0x38, 0x01, 0x00, 0x00}, 7);  // sub rsp, 138h
    expect_len({0x48, 0x83, 0xC4, 0x28}, 4);              // add rsp, 28h
    expect_len({0x48, 0x81, 0xC4, 0x38, 0x01, 0x00, 0x00}, 7);
    expect_len({0x48, 0x83, 0xE4, 0xF0}, 4);              // and rsp, -16
    expect_len({0x83, 0xE0, 0x0F}, 3);                    // and eax, 0Fh
    expect_len({0x25, 0xFF, 0x00, 0x00, 0x00}, 5);        // and eax, 0FFh
    expect_len({0x0D, 0x00, 0x01, 0x00, 0x00}, 5);        // or eax, 100h
    expect_len({0x0C, 0x01}, 2);                          // or al, 1
    expect_len({0x34, 0x01}, 2);                          // xor al, 1
    expect_len({0x33, 0xC0}, 2);                          // xor eax, eax
    expect_len({0x31, 0xC9}, 2);
    expect_len({0x48, 0x33, 0xC0}, 3);
    expect_len({0x0B, 0xC1}, 2);                          // or eax, ecx
    expect_len({0x23, 0xC1}, 2);
    expect_len({0x3B, 0xC1}, 2);                          // cmp eax, ecx
    expect_len({0x39, 0x0D, 0x00, 0x00, 0x00, 0x00}, 6, kInsnRipRel);
    expect_len({0x3B, 0x0D, 0x00, 0x00, 0x00, 0x00}, 6, kInsnRipRel);
    expect_len({0x83, 0x3D, 0x00, 0x00, 0x00, 0x00, 0x00}, 7, kInsnRipRel);  // cmp dword [rip+0], 0 (ADR-0020 item 2)
    expect_len({0x83, 0xF9, 0x01}, 3);                    // cmp ecx, 1
    expect_len({0x83, 0x7C, 0x24, 0x08, 0x00}, 5);        // cmp dword [rsp+8], 0
    expect_len({0x48, 0x83, 0x7C, 0x24, 0x08, 0x00}, 6);
    expect_len({0x80, 0x3D, 0x00, 0x00, 0x00, 0x00, 0x00}, 7, kInsnRipRel);  // cmp byte [rip+0], 0
    expect_len({0x3D, 0x00, 0x01, 0x00, 0x00}, 5);        // cmp eax, 100h
    expect_len({0x3C, 0x01}, 2);                          // cmp al, 1
    expect_len({0x66, 0x3D, 0x01, 0x00}, 4);              // cmp ax, 1   (Iz is 16 bit with 66)
    expect_len({0x66, 0x83, 0xF8, 0x01}, 4);
    expect_len({0x66, 0x81, 0xF9, 0x01, 0x02}, 5);
    expect_len({0x48, 0x3D, 0x00, 0x01, 0x00, 0x00}, 6);  // cmp rax, 100h (REX.W keeps imm32)
    expect_len({0x66, 0x48, 0x3D, 0x00, 0x01, 0x00, 0x00}, 7);  // REX.W wins over 66
    expect_len({0x85, 0xC9}, 2);                          // test ecx, ecx
    expect_len({0x48, 0x85, 0xC9}, 3);
    expect_len({0x84, 0xC0}, 2);
    expect_len({0xA8, 0x01}, 2);                          // test al, 1
    expect_len({0xA9, 0x00, 0x01, 0x00, 0x00}, 5);        // test eax, 100h
    expect_len({0xF6, 0xC1, 0x01}, 3);                    // test cl, 1
    expect_len({0xF7, 0xC1, 0x01, 0x00, 0x00, 0x00}, 6);  // test ecx, 1
    expect_len({0xF6, 0x41, 0x08, 0x01}, 4);              // test byte [rcx+8], 1
    expect_len({0xF7, 0xD8}, 2);                          // neg eax (no immediate: group slot 3)
    expect_len({0xF7, 0xD0}, 2);                          // not eax
    expect_len({0xF7, 0xF1}, 2);                          // div ecx
    expect_len({0xF6, 0xD0}, 2);
    expect_len({0x48, 0xF7, 0xE1}, 3);                    // mul rcx
    expect_len({0x69, 0xC0, 0xE8, 0x03, 0x00, 0x00}, 6);  // imul eax, eax, 1000
    expect_len({0x6B, 0xC0, 0x05}, 3);
    expect_len({0x0F, 0xAF, 0xC1}, 3);                    // imul eax, ecx
    expect_len({0x48, 0x0F, 0xAF, 0xC1}, 4);
    expect_len({0xFF, 0xC0}, 2);                          // inc eax
    expect_len({0xFF, 0xC8}, 2);                          // dec eax
    expect_len({0xFE, 0xC0}, 2);                          // inc al
    expect_len({0xFE, 0xC8}, 2);                          // dec al
    expect_len({0xD1, 0xE8}, 2);                          // shr eax, 1
    expect_len({0xC1, 0xE0, 0x04}, 3);                    // shl eax, 4
    expect_len({0x48, 0xC1, 0xE8, 0x3F}, 4);
    expect_len({0xD3, 0xE0}, 2);                          // shl eax, cl
    expect_len({0xD0, 0xE8}, 2);
    expect_len({0xC0, 0xE8, 0x02}, 3);
    expect_len({0x0F, 0xBA, 0xE0, 0x05}, 4);              // bt eax, 5
    expect_len({0x0F, 0xA3, 0xC8}, 3);                    // bt eax, ecx
    expect_len({0x0F, 0xA4, 0xC2, 0x04}, 4);              // shld edx, eax, 4
    expect_len({0x0F, 0xBC, 0xC1}, 3);                    // bsf
    expect_len({0xF3, 0x0F, 0xB8, 0xC1}, 4);              // popcnt eax, ecx
    expect_len({0x0F, 0x94, 0xC0}, 3);                    // sete al
    expect_len({0x0F, 0x44, 0xC1}, 3);                    // cmove eax, ecx
    expect_len({0x0F, 0xC8}, 2);                          // bswap eax
    expect_len({0x48, 0x0F, 0xC8}, 3);
    expect_len({0x98}, 1);                                // cwde
    expect_len({0x48, 0x99}, 2);                          // cqo
    expect_len({0x91}, 1);                                // xchg eax, ecx
    expect_len({0xC9}, 1);                                // leave
}

TEST_CASE("control flow: call, jmp, jcc, ret, int3, ud2") {
    expect_len({0xE8, 0x00, 0x00, 0x00, 0x00}, 5, kInsnCall);
    expect_len({0xE9, 0x00, 0x00, 0x00, 0x00}, 5, kInsnJmp);
    expect_len({0xEB, 0x05}, 2, kInsnJmp);
    expect_len({0x74, 0x05}, 2, kInsnJcc);
    expect_len({0x7F, 0xFE}, 2, kInsnJcc);
    expect_len({0x0F, 0x84, 0x00, 0x00, 0x00, 0x00}, 6, kInsnJcc);
    expect_len({0x0F, 0x8F, 0x10, 0x00, 0x00, 0x00}, 6, kInsnJcc);
    expect_len({0xE3, 0x02}, 2, kInsnJcc);                // jrcxz
    expect_len({0xE2, 0xFE}, 2, kInsnJcc);                // loop
    expect_len({0xFF, 0x15, 0x00, 0x00, 0x00, 0x00}, 6, kInsnCall | kInsnRipRel);   // call [rip+0]
    expect_len({0xFF, 0xD0}, 2, kInsnCall);               // call rax
    expect_len({0x41, 0xFF, 0xD3}, 3, kInsnCall);         // call r11
    expect_len({0xFF, 0x50, 0x08}, 3, kInsnCall);         // call [rax+8]
    expect_len({0xFF, 0x14, 0xC8}, 3, kInsnCall);         // call [rax+rcx*8]
    expect_len({0xFF, 0xE0}, 2, kInsnJmp);                // jmp rax
    expect_len({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}, 6, kInsnJmp | kInsnRipRel);    // jmp [rip+0]
    expect_len({0x48, 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}, 7, kInsnJmp | kInsnRipRel);  // rex.w jmp [rip+0] (import thunk)
    expect_len({0xC3}, 1, kInsnRet);
    expect_len({0xF3, 0xC3}, 2, kInsnRet);                // repz ret
    expect_len({0xC2, 0x08, 0x00}, 3, kInsnRet);
    expect_len({0xCC}, 1, kInsnTrap);
    expect_len({0xCD, 0x29}, 2, kInsnTrap);               // int 29h (__fastfail)
    expect_len({0x0F, 0x0B}, 2, kInsnTrap);               // ud2
    CHECK(ends_flow(kInsnRet));
    CHECK(ends_flow(kInsnJmp));
    CHECK(ends_flow(kInsnTrap));
    CHECK_FALSE(ends_flow(kInsnJcc));
    CHECK_FALSE(ends_flow(kInsnCall));
    CHECK_FALSE(ends_flow(0));
}

TEST_CASE("nop forms") {
    expect_len({0x90}, 1);
    expect_len({0x66, 0x90}, 2);
    expect_len({0xF3, 0x90}, 2);                          // pause
    expect_len({0x41, 0x90}, 2);                          // xchg r8, rax
    expect_len({0x0F, 0x1F, 0x00}, 3);
    expect_len({0x0F, 0x1F, 0x40, 0x00}, 4);
    expect_len({0x0F, 0x1F, 0x44, 0x00, 0x00}, 5);
    expect_len({0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00}, 6);
    expect_len({0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00}, 7);
    expect_len({0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, 8);
    expect_len({0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, 9);
    expect_len({0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, 10);
    expect_len({0x66, 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, 11);
    expect_len({0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, 10);
    expect_len({0x0F, 0x1F, 0x44, 0x00, 0x00}, 5);
    expect_len({0x48, 0x8D, 0x49, 0x00}, 4);              // lea rcx, [rcx+0] (nop idiom)
    expect_len({0x8D, 0x64, 0x24, 0x00}, 4);
}

TEST_CASE("endbr64 and CET") {
    expect_len({0xF3, 0x0F, 0x1E, 0xFA}, 4);              // endbr64
    expect_len({0xF3, 0x0F, 0x1E, 0xFB}, 4);              // endbr32
    expect_len({0xF3, 0x48, 0x0F, 0x1E, 0xC8}, 5);        // rdsspq rax
}

TEST_CASE("SSE moves: movaps, movdqa, movups, movdqu, movss, movsd, movq") {
    expect_len({0x0F, 0x28, 0xC1}, 3);                    // movaps xmm0, xmm1
    expect_len({0x0F, 0x29, 0x74, 0x24, 0x20}, 5);        // movaps [rsp+20h], xmm6
    expect_len({0x0F, 0x29, 0xB4, 0x24, 0x80, 0x00, 0x00, 0x00}, 8);  // movaps [rsp+80h], xmm6
    expect_len({0x44, 0x0F, 0x29, 0x44, 0x24, 0x40}, 6);  // movaps [rsp+40h], xmm8
    expect_len({0x0F, 0x28, 0x05, 0x00, 0x00, 0x00, 0x00}, 7, kInsnRipRel);
    expect_len({0x0F, 0x10, 0x01}, 3);                    // movups xmm0, [rcx]
    expect_len({0x0F, 0x11, 0x41, 0x10}, 4);
    expect_len({0x66, 0x0F, 0x6F, 0x05, 0x00, 0x00, 0x00, 0x00}, 8, kInsnRipRel);   // movdqa xmm0, [rip+0]
    expect_len({0x66, 0x0F, 0x7F, 0x01}, 4);              // movdqa [rcx], xmm0
    expect_len({0xF3, 0x0F, 0x6F, 0x01}, 4);              // movdqu xmm0, [rcx]
    expect_len({0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20}, 6);  // movdqu [rsp+20h], xmm0
    expect_len({0xF3, 0x0F, 0x10, 0x01}, 4);              // movss
    expect_len({0xF2, 0x0F, 0x10, 0x01}, 4);              // movsd
    expect_len({0xF2, 0x0F, 0x11, 0x44, 0x24, 0x08}, 6);
    expect_len({0xF3, 0x0F, 0x7E, 0x01}, 4);              // movq xmm0, [rcx]
    expect_len({0x66, 0x0F, 0xD6, 0x01}, 4);              // movq [rcx], xmm0
    expect_len({0x66, 0x48, 0x0F, 0x6E, 0xC1}, 5);        // movq xmm0, rcx
    expect_len({0x66, 0x0F, 0x6E, 0xC1}, 4);              // movd xmm0, ecx
    expect_len({0x66, 0x0F, 0xEF, 0xC0}, 4);              // pxor xmm0, xmm0
    expect_len({0x0F, 0x57, 0xC0}, 3);                    // xorps
    expect_len({0xF2, 0x0F, 0x58, 0xC1}, 4);              // addsd
    expect_len({0xF3, 0x0F, 0x5A, 0xC0}, 4);              // cvtss2sd
    expect_len({0xF2, 0x48, 0x0F, 0x2A, 0xC1}, 5);        // cvtsi2sd xmm0, rcx
    expect_len({0x0F, 0x2E, 0xC1}, 3);                    // ucomiss
    expect_len({0x66, 0x0F, 0x2F, 0xC1}, 4);              // comisd
    expect_len({0x66, 0x0F, 0x70, 0xC0, 0x00}, 5);        // pshufd xmm0, xmm0, 0
    expect_len({0x66, 0x0F, 0x73, 0xD8, 0x08}, 5);        // psrldq xmm0, 8
    expect_len({0x0F, 0xC6, 0xC1, 0x00}, 4);              // shufps
    expect_len({0x66, 0x0F, 0xC4, 0xC1, 0x01}, 5);        // pinsrw
    expect_len({0x66, 0x0F, 0xD7, 0xC0}, 4);              // pmovmskb eax, xmm0
    expect_len({0x66, 0x0F, 0x74, 0xC1}, 4);              // pcmpeqb
    expect_len({0x66, 0x0F, 0x38, 0x00, 0xC1}, 5);        // pshufb
    expect_len({0x66, 0x0F, 0x3A, 0x0F, 0xC1, 0x08}, 6);  // palignr
    expect_len({0x66, 0x0F, 0x3A, 0x63, 0xC1, 0x0C}, 6);  // pcmpistri
    expect_len({0x0F, 0xAE, 0xF0}, 3);                    // mfence
    expect_len({0x0F, 0xAE, 0x14, 0x24}, 4);              // ldmxcsr [rsp]
    expect_len({0x0F, 0x01, 0xD0}, 3);                    // xgetbv
    expect_len({0x0F, 0xA2}, 2);                          // cpuid
    expect_len({0x0F, 0x31}, 2);                          // rdtsc
    expect_len({0x0F, 0x05}, 2);                          // syscall
    expect_len({0x0F, 0x18, 0x09}, 3);                    // prefetcht0 [rcx]
    expect_len({0x0F, 0x0D, 0x09}, 3);                    // prefetchw [rcx]
    expect_len({0x0F, 0xC7, 0x0E}, 3);                    // cmpxchg8b [rsi]
    expect_len({0x48, 0x0F, 0xC7, 0x0E}, 4);              // cmpxchg16b [rsi]
}

TEST_CASE("string instructions, with and without rep") {
    expect_len({0xA4}, 1);
    expect_len({0xF3, 0xA4}, 2);                          // rep movsb
    expect_len({0xF3, 0x48, 0xA5}, 3);                    // rep movsq
    expect_len({0xF3, 0x48, 0xAB}, 3);                    // rep stosq
    expect_len({0xF3, 0xAA}, 2);                          // rep stosb
    expect_len({0xF2, 0xAE}, 2);                          // repne scasb
    expect_len({0xAC}, 1);                                // lodsb
    expect_len({0xA6}, 1);
}

TEST_CASE("x87 escapes always carry a ModRM byte") {
    expect_len({0xD9, 0x45, 0x08}, 3);                    // fld dword [rbp+8]
    expect_len({0xDD, 0x04, 0x24}, 3);                    // fld qword [rsp]
    expect_len({0xDD, 0xD8}, 2);                          // fstp st(0)
    expect_len({0xD9, 0xE8}, 2);                          // fld1
    expect_len({0xDB, 0x2D, 0x00, 0x00, 0x00, 0x00}, 6, kInsnRipRel);  // fld tbyte [rip+0]
    expect_len({0xDF, 0xE0}, 2);                          // fnstsw ax
}

TEST_CASE("prefix handling") {
    // A legacy prefix after REX makes the REX ineffective; the length is the same either way, but
    // the immediate size follows REX.W only when REX is last.
    expect_len({0x48, 0x66, 0xB8, 0x01, 0x00}, 5);        // REX dropped by the 66: mov ax, 1
    expect_len({0x66, 0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8}, 11);  // REX last: movabs, imm64
    expect_len({0x48, 0x48, 0x83, 0xEC, 0x28}, 5);        // two REX bytes: the last counts, both are length
    // 14 prefixes + 1 opcode byte is exactly the 15-byte limit.
    Bytes fourteen(14, 0x66);
    fourteen.push_back(0x90);
    expect_len(fourteen, 15);
    Bytes fifteen(15, 0x66);
    fifteen.push_back(0x90);
    expect_unknown(fifteen);
    // An instruction that would be longer than 15 bytes is invalid.
    Bytes too_long(13, 0x66);
    for (const std::uint8_t b : std::initializer_list<std::uint8_t>{0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}) {
        too_long.push_back(b);
    }
    expect_unknown(too_long);
}

TEST_CASE("everything outside the covered set is unknown, never guessed") {
    expect_unknown({0xC4, 0xE2, 0x7D, 0x18, 0xC0});       // VEX3
    expect_unknown({0xC5, 0xF8, 0x28, 0xC1});             // VEX2: vmovaps
    expect_unknown({0x62, 0xF1, 0x7C, 0x48, 0x28, 0xC1}); // EVEX
    expect_unknown({0x8F, 0xE9, 0x78, 0xC2, 0xC1});       // XOP (8F with reg != 0)
    expect_unknown({0x0F, 0x0F, 0xC1, 0x9E});             // 3DNow!
    expect_unknown({0xA0, 0, 0, 0, 0, 0, 0, 0, 0});       // mov al, moffs64
    expect_unknown({0xA1, 0, 0, 0, 0, 0, 0, 0, 0});
    expect_unknown({0xE4, 0x10});                         // in
    expect_unknown({0xEC});
    expect_unknown({0xF4});                               // hlt
    expect_unknown({0x06});                               // invalid in 64-bit mode
    expect_unknown({0x27});
    expect_unknown({0x60});
    expect_unknown({0x9A, 0, 0, 0, 0, 0, 0});
    expect_unknown({0xC8, 0x10, 0x00, 0x00});             // enter
    expect_unknown({0xCB});                               // retf
    expect_unknown({0xCF});                               // iret
    expect_unknown({0xD6});
    expect_unknown({0xEA, 0, 0, 0, 0, 0, 0});
    expect_unknown({0xFF, 0xF8});                         // FF /7
    expect_unknown({0xFF, 0x18});                         // call far [rax]
    expect_unknown({0xFF, 0x28});                         // jmp far [rax]
    expect_unknown({0xFE, 0xD0});                         // FE /2
    expect_unknown({0xC6, 0xF8, 0x00});                   // xabort
    expect_unknown({0xC7, 0xF8, 0x00, 0x00, 0x00, 0x00}); // xbegin
    expect_unknown({0x0F, 0xBA, 0xC0, 0x01});             // 0F BA /0 is invalid
    expect_unknown({0x66, 0xE8, 0x00, 0x00, 0x00, 0x00}); // 66 call: rel16 or rel32 depending on vendor
    expect_unknown({0x66, 0xE9, 0x00, 0x00, 0x00, 0x00});
    expect_unknown({0x66, 0x0F, 0x84, 0x00, 0x00, 0x00, 0x00});
    expect_unknown({0x0F, 0x04});
    expect_unknown({0x0F, 0x78, 0xC1});                   // vmread
    expect_unknown({0x0F, 0x20, 0xC0});                   // mov cr
    expect_unknown({0x0F, 0x30});                         // wrmsr
}

TEST_CASE("incomplete instructions are Truncated, and nothing past `available` is read") {
    CHECK(decode_instruction(nullptr, 4).status == DecodeStatus::Truncated);
    const Bytes one = {0xE8};
    CHECK(decode_instruction(one.data(), 0).status == DecodeStatus::Truncated);
    const Bytes rex_only = {0x48};
    CHECK(decode_instruction(rex_only.data(), 1).status == DecodeStatus::Truncated);
    const Bytes escape = {0x0F};
    CHECK(decode_instruction(escape.data(), 1).status == DecodeStatus::Truncated);
    const Bytes three = {0x0F, 0x38};
    CHECK(decode_instruction(three.data(), 2).status == DecodeStatus::Truncated);
    const Bytes modrm_missing = {0x48, 0x83};
    CHECK(decode_instruction(modrm_missing.data(), 2).status == DecodeStatus::Truncated);
    const Bytes sib_missing = {0x48, 0x89, 0x5C};
    CHECK(decode_instruction(sib_missing.data(), 3).status == DecodeStatus::Truncated);
    const Bytes disp_missing = {0x48, 0x8B, 0x05, 0x00, 0x00};
    CHECK(decode_instruction(disp_missing.data(), 5).status == DecodeStatus::Truncated);
    const Bytes imm_missing = {0x48, 0x83, 0xEC};
    CHECK(decode_instruction(imm_missing.data(), 3).status == DecodeStatus::Truncated);
    // An unknown opcode that is complete is Unknown even when nothing follows it.
    const Bytes vex = {0xC5};
    CHECK(decode_instruction(vex.data(), 1).status == DecodeStatus::Unknown);
}

TEST_CASE("a linear sweep over a realistic function prologue finds every boundary") {
    // cl RelWithDebInfo style: sub rsp, 38h / mov [rsp+40h], rcx / lea rax,[rip+..] / call / add rsp / ret
    const Bytes code = {
        0x48, 0x83, 0xEC, 0x38,                          // sub rsp, 38h
        0x48, 0x89, 0x4C, 0x24, 0x40,                    // mov [rsp+40h], rcx
        0x48, 0x8D, 0x05, 0x11, 0x22, 0x33, 0x44,        // lea rax, [rip+44332211h]
        0x48, 0x89, 0x44, 0x24, 0x20,                    // mov [rsp+20h], rax
        0xE8, 0x01, 0x02, 0x03, 0x04,                    // call rel32
        0x85, 0xC0,                                      // test eax, eax
        0x74, 0x05,                                      // je +5
        0xB8, 0x01, 0x00, 0x00, 0x00,                    // mov eax, 1
        0x48, 0x83, 0xC4, 0x38,                          // add rsp, 38h
        0xC3,                                            // ret
    };
    const unsigned expected[] = {4, 5, 7, 5, 5, 2, 2, 5, 4, 1};
    std::size_t at = 0;
    for (unsigned want : expected) {
        const DecodeResult r = decode_instruction(code.data() + at, code.size() - at);
        REQUIRE(r.status == DecodeStatus::Ok);
        CHECK(r.length == want);
        at += r.length;
    }
    CHECK(at == code.size());
}
