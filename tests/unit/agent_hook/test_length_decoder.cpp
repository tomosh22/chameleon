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
    expect_len({0x67, 0x8B, 0x00}, 3, kInsnAddr32);       // mov eax, [eax]     (67 prefix: same layout)
    expect_len({0x64, 0x48, 0x8B, 0x04, 0x25, 0x30, 0x00, 0x00, 0x00}, 9, kInsnSegFsGs);  // mov rax, fs:[30h] (SIB no base)
    expect_len({0x65, 0x48, 0x8B, 0x04, 0x25, 0x60, 0x00, 0x00, 0x00}, 9, kInsnSegFsGs);  // mov rax, gs:[60h]
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
    // VEX and EVEX were pinned as unknown here until WP-38 (ADR-0042 reverses that WP-10 decision); their vectors are
    // in the test cases further down. What stays outside:
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
    const Bytes hlt = {0xF4};
    CHECK(decode_instruction(hlt.data(), 1).status == DecodeStatus::Unknown);
    // VEX and EVEX: the prefix payload, the opcode, ModRM, SIB, displacement and immediate can each be missing.
    const Bytes vex2 = {0xC5, 0xF8, 0x28, 0xC1};                         // vmovaps xmm0, xmm1
    const Bytes vex3 = {0xC4, 0xE3, 0xFD, 0x00, 0x44, 0x24, 0x08, 0xD8}; // vpermq ymm0, [rsp+8], 0D8h
    const Bytes evex = {0x62, 0xF1, 0x7C, 0x48, 0x10, 0x44, 0x24, 0x01}; // vmovups zmm0, [rsp+40h]
    for (const Bytes* v : {&vex2, &vex3, &evex}) {
        for (std::size_t k = 1; k < v->size(); ++k) {
            INFO("bytes: " << hex(*v) << " cut to " << k);
            CHECK(decode_instruction(v->data(), k).status == DecodeStatus::Truncated);
        }
        CHECK(decode_instruction(v->data(), v->size()).status == DecodeStatus::Ok);
    }
}

// ADR-0042: where the ModRM displacement sits. The first five vectors are the instructions cl /O2 emitted for
// image-relative array indexing in the orchestrator's spike (run 45402685481): the relocation site of `sym@IMGREL`
// is exactly this displacement.
TEST_CASE("the displacement of a memory operand is located; immediates and branch targets are not displacements") {
    struct Case {
        Bytes bytes;
        unsigned length;
        unsigned disp_offset;
        unsigned disp_size;
        std::uint8_t flags;
    };
    const Case cases[] = {
        {{0xC6, 0x84, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 3, 4, 0},              // mov byte ptr [rbx+rdi+disp32], 0
        {{0x89, 0x9C, 0x87, 0x00, 0x04, 0x00, 0x00}, 7, 3, 4, 0},                    // mov [rdi+rax*4+disp32], ebx
        {{0x48, 0x8B, 0x84, 0xC7, 0x00, 0x00, 0x00, 0x00}, 8, 4, 4, 0},              // mov rax, [rdi+rax*8+disp32]
        {{0xF3, 0x0F, 0x2C, 0x84, 0x87, 0x44, 0x04, 0x00, 0x00}, 9, 5, 4, 0},        // cvttss2si eax, [rdi+rax*4+disp32]
        {{0x8B, 0x84, 0x87, 0x00, 0x00, 0x00, 0x00}, 7, 3, 4, 0},                    // mov eax, [rdi+rax*4+disp32] (jump table)
        {{0x81, 0x84, 0x87, 0x10, 0x00, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12}, 11, 3, 4, 0},  // add [..+disp32], imm32
        {{0x8B, 0x87, 0x00, 0x01, 0x00, 0x00}, 6, 2, 4, 0},                          // mov eax, [rdi+disp32] (no SIB)
        {{0x48, 0x8B, 0x04, 0x25, 0x00, 0x10, 0x00, 0x00}, 8, 4, 4, 0},              // mov rax, [disp32] (SIB, no base)
        {{0x48, 0x8B, 0x04, 0xC5, 0x00, 0x10, 0x00, 0x00}, 8, 4, 4, 0},              // mov rax, [rax*8+disp32]
        {{0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00}, 7, 3, 4, kInsnRipRel},          // lea rax, [rip+disp32]
        {{0x48, 0x89, 0x5C, 0x24, 0x08}, 5, 4, 1, 0},                                // mov [rsp+8], rbx (disp8)
        {{0x8B, 0x45, 0x10}, 3, 2, 1, 0},                                            // mov eax, [rbp+10h] (disp8)
        {{0x8B, 0x04, 0x88}, 3, 0, 0, 0},                                            // mov eax, [rax+rcx*4]
        {{0x48, 0x03, 0xC7}, 3, 0, 0, 0},                                            // add rax, rdi
        {{0xB8, 0x78, 0x56, 0x34, 0x12}, 5, 0, 0, 0},                                // mov eax, imm32
        {{0x05, 0x78, 0x56, 0x34, 0x12}, 5, 0, 0, 0},                                // add eax, imm32
        {{0x68, 0x78, 0x56, 0x34, 0x12}, 5, 0, 0, 0},                                // push imm32
        {{0xE8, 0x00, 0x00, 0x00, 0x00}, 5, 0, 0, kInsnCall},                        // call rel32
        {{0xE9, 0x00, 0x00, 0x00, 0x00}, 5, 0, 0, kInsnJmp},                         // jmp rel32
        {{0x0F, 0x84, 0x00, 0x00, 0x00, 0x00}, 6, 0, 0, kInsnJcc},                   // je rel32
        {{0xC7, 0x05, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}, 10, 2, 4, kInsnRipRel},  // mov [rip+disp32], imm32
        // The same layout, but the address is not a plain 64-bit one: the flags say so.
        {{0x67, 0x8B, 0x84, 0x87, 0x00, 0x00, 0x00, 0x00}, 8, 4, 4, kInsnAddr32},    // mov eax, [edi+eax*4+disp32]
        {{0x65, 0x8B, 0x84, 0x87, 0x00, 0x00, 0x00, 0x00}, 8, 4, 4, kInsnSegFsGs},   // mov eax, gs:[rdi+rax*4+disp32]
        {{0x67, 0x65, 0x8B, 0x87, 0x00, 0x00, 0x00, 0x00}, 8, 4, 4, static_cast<std::uint8_t>(kInsnAddr32 | kInsnSegFsGs)},
        {{0x2E, 0x8B, 0x87, 0x00, 0x00, 0x00, 0x00}, 7, 3, 4, 0},                    // cs override: ignored in 64-bit mode
        // VEX and EVEX (WP-38): the displacement sits behind the longer prefix, and an EVEX disp8 is one byte although
        // the processor scales it.
        {{0xC5, 0xF8, 0x10, 0x84, 0x87, 0x00, 0x00, 0x00, 0x00}, 9, 5, 4, 0},        // vmovups xmm0, [rdi+rax*4+disp32]
        {{0xC5, 0xFA, 0x11, 0x44, 0x24, 0x08}, 6, 5, 1, 0},                          // vmovss [rsp+8], xmm0
        {{0xC5, 0xFD, 0x6F, 0x05, 0x00, 0x00, 0x00, 0x00}, 8, 4, 4, kInsnRipRel},    // vmovdqa ymm0, [rip+disp32]
        {{0xC4, 0xC1, 0x7E, 0x6F, 0x40, 0x20}, 6, 5, 1, 0},                          // vmovdqu ymm0, [r8+20h]
        {{0xC4, 0xE3, 0x7D, 0x06, 0x05, 0x00, 0x00, 0x00, 0x00, 0x31}, 10, 5, 4, kInsnRipRel},  // vperm2f128 ymm0, ymm0, [rip+disp32], 31h
        {{0xC4, 0xE2, 0x6D, 0x92, 0x44, 0x88, 0x10}, 7, 6, 1, 0},                    // vgatherdps ymm0, [rax+ymm1*4+10h], ymm2 (VSIB)
        {{0xC5, 0xF8, 0x28, 0xC1}, 4, 0, 0, 0},                                      // vmovaps xmm0, xmm1
        {{0xC5, 0xF8, 0x77}, 3, 0, 0, 0},                                            // vzeroupper
        {{0x62, 0xF1, 0x7C, 0x48, 0x10, 0x41, 0x01}, 7, 6, 1, 0},                    // vmovups zmm0, [rcx+40h] (disp8*64)
        {{0x62, 0xF1, 0x7C, 0x48, 0x10, 0x81, 0x00, 0x01, 0x00, 0x00}, 10, 6, 4, 0}, // vmovups zmm0, [rcx+100h]
        {{0x62, 0xF1, 0x7C, 0x48, 0x10, 0x05, 0x00, 0x00, 0x00, 0x00}, 10, 6, 4, kInsnRipRel},  // vmovups zmm0, [rip+disp32]
        {{0x62, 0xF3, 0x7D, 0x48, 0x25, 0x44, 0x24, 0x01, 0xFF}, 9, 7, 1, 0},        // vpternlogd zmm0, zmm0, [rsp+40h], 0FFh
        // A segment override or 67 in front of VEX / EVEX is a prefix of the instruction and shows in the flags.
        {{0x65, 0xC5, 0xFA, 0x10, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00}, 10, 6, 4, kInsnSegFsGs},  // vmovss xmm0, gs:[disp32]
        {{0x67, 0xC5, 0xF8, 0x10, 0x40, 0x10}, 6, 5, 1, kInsnAddr32},                // vmovups xmm0, [eax+10h]
        {{0x64, 0x67, 0x62, 0xF1, 0x7C, 0x48, 0x10, 0x41, 0x01}, 9, 8, 1, static_cast<std::uint8_t>(kInsnAddr32 | kInsnSegFsGs)},
        {{0x2E, 0xC5, 0xF8, 0x10, 0x87, 0x00, 0x00, 0x00, 0x00}, 9, 5, 4, 0},        // cs override: ignored
    };
    for (const Case& c : cases) {
        INFO("bytes: " << hex(c.bytes));
        expect_len(c.bytes, c.length, c.flags);
        const DecodeResult r = decode_exact(c.bytes);
        REQUIRE(r.status == DecodeStatus::Ok);
        CHECK(r.disp_offset == c.disp_offset);
        CHECK(r.disp_size == c.disp_size);
        if (r.disp_size != 0) {
            CHECK(r.disp_offset + r.disp_size <= r.length);
        }
    }
    // Not Ok: no displacement is reported. (0F 0F is the 3DNow! escape, which stays outside the covered set.)
    const Bytes unknown_bytes = {0x0F, 0x0F, 0x84, 0x87, 0x00, 0x00, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    const DecodeResult u = decode_instruction(unknown_bytes.data(), unknown_bytes.size());
    CHECK(u.status == DecodeStatus::Unknown);
    CHECK(u.disp_offset == 0);
    CHECK(u.disp_size == 0);
    const Bytes cut = {0x8B, 0x84, 0x87, 0x00};
    const DecodeResult t = decode_instruction(cut.data(), cut.size());
    CHECK(t.status == DecodeStatus::Truncated);
    CHECK(t.disp_offset == 0);
    CHECK(t.disp_size == 0);
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

// ---- VEX and EVEX (WP-38, ADR-0042) ---------------------------------------------------------------------------------
//
// How the lengths below were worked out, by hand, from the Intel SDM volume 2 (2.3 "AVX" and 2.7 "EVEX" for the
// prefixes, the instruction pages for the opcodes; AMD APM volume 4 says the same):
//
//   C5  [R vvvv L pp]                             2 bytes of prefix, opcode map 0F
//   C4  [R X B m-mmmm] [W vvvv L pp]              3 bytes of prefix, map 1 = 0F, 2 = 0F 38, 3 = 0F 3A
//   62  [R X B R' 0 0 m m] [W vvvv 1 pp] [z L'L b V' aaa]   4 bytes of prefix, the same maps
//   then one opcode byte, ModRM (+ SIB + disp8 / disp32 exactly as for a legacy instruction; an EVEX disp8 is scaled by
//   the processor but still one byte), then one immediate byte for every map-3 opcode and for 0F 70..73, C2, C4, C5, C6.
//   VEX 0F 77 (vzeroupper, vzeroall) alone has no ModRM byte.
//
// The R, X, B, vvvv fields are stored inverted, so "no extended register, no second source" is all ones: C5 F8 is
// R=1 vvvv=1111 L=0 pp=00.
TEST_CASE("VEX: two-byte and three-byte prefix, maps 1 to 3") {
    // Map 1 through C5.
    expect_len({0xC5, 0xF8, 0x28, 0xC1}, 4);                     // vmovaps xmm0, xmm1
    expect_len({0xC5, 0xFC, 0x28, 0xC1}, 4);                     // vmovaps ymm0, ymm1          (L = 1)
    expect_len({0xC5, 0x78, 0x29, 0x44, 0x24, 0x20}, 6);         // vmovaps [rsp+20h], xmm8     (R = 0; SIB, disp8)
    expect_len({0xC5, 0xFA, 0x11, 0x44, 0x24, 0x08}, 6);         // vmovss [rsp+8], xmm0        (pp = F3)
    expect_len({0xC5, 0xFB, 0x11, 0x44, 0x24, 0x08}, 6);         // vmovsd [rsp+8], xmm0        (pp = F2)
    expect_len({0xC5, 0xFB, 0x10, 0x05, 0x00, 0x00, 0x00, 0x00}, 8, kInsnRipRel);  // vmovsd xmm0, [rip+disp32]
    expect_len({0xC5, 0xFE, 0x6F, 0x01}, 4);                     // vmovdqu ymm0, [rcx]
    expect_len({0xC5, 0xFE, 0x7F, 0x02}, 4);                     // vmovdqu [rdx], ymm0
    expect_len({0xC5, 0xFD, 0x6F, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00}, 9);  // vmovdqa ymm0, [rsp+80h] (SIB, disp32)
    expect_len({0xC5, 0xF9, 0x6E, 0xC1}, 4);                     // vmovd xmm0, ecx
    expect_len({0xC5, 0xF9, 0x7E, 0xC0}, 4);                     // vmovd eax, xmm0
    expect_len({0xC5, 0xF9, 0xEF, 0xC0}, 4);                     // vpxor xmm0, xmm0, xmm0
    expect_len({0xC5, 0xF1, 0xEF, 0xC9}, 4);                     // vpxor xmm1, xmm1, xmm1      (vvvv = 1110)
    expect_len({0xC5, 0xF8, 0x57, 0xC0}, 4);                     // vxorps xmm0, xmm0, xmm0
    expect_len({0xC5, 0xFA, 0x2A, 0xC1}, 4);                     // vcvtsi2ss xmm0, xmm0, ecx
    expect_len({0xC5, 0xFA, 0x2C, 0xC0}, 4);                     // vcvttss2si eax, xmm0
    expect_len({0xC5, 0xFA, 0x58, 0x05, 0x00, 0x00, 0x00, 0x00}, 8, kInsnRipRel);  // vaddss xmm0, xmm0, [rip+disp32]
    expect_len({0xC5, 0xF8, 0x2E, 0xC1}, 4);                     // vucomiss xmm0, xmm1
    expect_len({0xC5, 0xFD, 0xD7, 0xC0}, 4);                     // vpmovmskb eax, ymm0
    expect_len({0xC5, 0xFD, 0x74, 0x01}, 4);                     // vpcmpeqb ymm0, ymm0, [rcx]
    expect_len({0xC5, 0xF8, 0xAE, 0x14, 0x24}, 5);               // vldmxcsr [rsp]
    expect_len({0xC5, 0xF8, 0xAE, 0x5C, 0x24, 0x08}, 6);         // vstmxcsr [rsp+8]
    // Map 1 through C4 (needed for W = 1 or for the X / B register extensions).
    expect_len({0xC4, 0xE1, 0xF9, 0x6E, 0xC1}, 5);               // vmovq xmm0, rcx             (W = 1)
    expect_len({0xC4, 0xE1, 0xF9, 0x7E, 0xC0}, 5);               // vmovq rax, xmm0
    expect_len({0xC4, 0xC1, 0x7E, 0x6F, 0x00}, 5);               // vmovdqu ymm0, [r8]          (B = 0)
    expect_len({0xC4, 0xC1, 0x7D, 0x6F, 0x00}, 5);               // vmovdqa ymm0, [r8]
    expect_len({0xC4, 0xC1, 0x7C, 0x10, 0x00}, 5);               // vmovups ymm0, [r8]
    expect_len({0xC4, 0xC1, 0x7E, 0x7F, 0x04, 0x01}, 6);         // vmovdqu [r9+rax], ymm0      (SIB)
    expect_len({0xC4, 0xA1, 0x7E, 0x6F, 0x04, 0x01}, 6);         // vmovdqu ymm0, [rcx+r8]      (X = 0)
    expect_len({0xC4, 0xE1, 0x78, 0x28, 0xC1}, 5);               // vmovaps xmm0, xmm1 in the long form
    // Map 2 (0F 38): ModRM, never an immediate.
    expect_len({0xC4, 0xE2, 0x7D, 0x18, 0xC0}, 5);               // vbroadcastss ymm0, xmm0
    expect_len({0xC4, 0xE2, 0x7D, 0x18, 0x05, 0x00, 0x00, 0x00, 0x00}, 9, kInsnRipRel);  // vbroadcastss ymm0, [rip+disp32]
    expect_len({0xC4, 0xE2, 0x7D, 0x78, 0xC0}, 5);               // vpbroadcastb ymm0, xmm0
    expect_len({0xC4, 0xE2, 0x7D, 0x58, 0xC1}, 5);               // vpbroadcastd ymm0, xmm1
    expect_len({0xC4, 0xC2, 0x7D, 0x59, 0x00}, 5);               // vpbroadcastq ymm0, [r8]
    expect_len({0xC4, 0xE2, 0x79, 0x00, 0xC1}, 5);               // vpshufb xmm0, xmm0, xmm1
    expect_len({0xC4, 0xE2, 0x7D, 0x17, 0xC0}, 5);               // vptest ymm0, ymm0
    expect_len({0xC4, 0xE2, 0x71, 0xA9, 0xC2}, 5);               // vfmadd213ss xmm0, xmm1, xmm2
    expect_len({0xC4, 0xE2, 0xF1, 0xB8, 0x01}, 5);               // vfmadd231pd xmm0, xmm1, [rcx]
    expect_len({0xC4, 0xE2, 0x7D, 0x13, 0xC0}, 5);               // vcvtph2ps ymm0, xmm0
    expect_len({0xC4, 0xE2, 0x78, 0xF2, 0xC1}, 5);               // andn eax, eax, ecx
    expect_len({0xC4, 0xE2, 0x78, 0xF3, 0xD9}, 5);               // blsi eax, ecx               (group 17, /3)
    expect_len({0xC4, 0xE2, 0x7A, 0xF7, 0xC1}, 5);               // sarx eax, ecx, eax
    expect_len({0xC4, 0xE2, 0x7B, 0xF6, 0xC1}, 5);               // mulx eax, eax, ecx
    expect_len({0xC4, 0xE2, 0xFB, 0xF5, 0x44, 0x24, 0x10}, 7);   // pdep rax, rax, [rsp+10h]
    expect_len({0xC4, 0xE2, 0x6D, 0x92, 0x04, 0x88}, 6);         // vgatherdps ymm0, [rax+ymm1*4], ymm2  (VSIB)
    expect_len({0xC4, 0xE2, 0x79, 0xDC, 0xC1}, 5);               // vaesenc xmm0, xmm0, xmm1
    // Map 3 (0F 3A): ModRM and always one immediate byte.
    expect_len({0xC4, 0xE3, 0xFD, 0x00, 0xC0, 0xD8}, 6);         // vpermq ymm0, ymm0, 0D8h
    expect_len({0xC4, 0xE3, 0xFD, 0x00, 0x01, 0x1B}, 6);         // vpermq ymm0, [rcx], 1Bh
    expect_len({0xC4, 0xE3, 0x7D, 0x19, 0xC1, 0x01}, 6);         // vextractf128 xmm1, ymm0, 1
    expect_len({0xC4, 0xE3, 0x7D, 0x39, 0x44, 0x24, 0x20, 0x01}, 8);  // vextracti128 [rsp+20h], ymm0, 1
    expect_len({0xC4, 0xE3, 0x7D, 0x18, 0xC1, 0x01}, 6);         // vinsertf128 ymm0, ymm0, xmm1, 1
    expect_len({0xC4, 0xE3, 0x79, 0x16, 0xC0, 0x01}, 6);         // vpextrd eax, xmm0, 1
    expect_len({0xC4, 0xE3, 0x79, 0x0F, 0xC1, 0x08}, 6);         // vpalignr xmm0, xmm0, xmm1, 8
    expect_len({0xC4, 0xE3, 0x7D, 0x1D, 0xC0, 0x04}, 6);         // vcvtps2ph xmm0, ymm0, 4
    expect_len({0xC4, 0xE3, 0x79, 0x63, 0xC1, 0x0C}, 6);         // vpcmpistri xmm0, xmm1, 0Ch
    expect_len({0xC4, 0xE3, 0x7B, 0xF0, 0xC1, 0x07}, 6);         // rorx eax, ecx, 7
    // The /is4 forms: the fourth register travels in the immediate byte, which is still exactly one byte.
    expect_len({0xC4, 0xE3, 0x75, 0x4A, 0xC2, 0x30}, 6);         // vblendvps ymm0, ymm1, ymm2, ymm3
    expect_len({0xC4, 0xE3, 0x75, 0x4C, 0x01, 0x30}, 6);         // vpblendvb ymm0, ymm1, [rcx], ymm3
}

TEST_CASE("VEX: the opcodes with and without ModRM and immediate that are easy to get wrong") {
    // 0F 77 is the only VEX opcode without a ModRM byte: the byte behind it belongs to the next instruction.
    expect_len({0xC5, 0xF8, 0x77}, 3);                           // vzeroupper
    expect_len({0xC5, 0xFC, 0x77}, 3);                           // vzeroall
    expect_len({0xC4, 0xE1, 0x78, 0x77}, 4);                     // vzeroupper in the long form
    {
        const Bytes code = {0xC5, 0xF8, 0x77, 0xC3};             // vzeroupper ; ret
        const DecodeResult first = decode_instruction(code.data(), code.size());
        REQUIRE(first.status == DecodeStatus::Ok);
        CHECK(first.length == 3);
        const DecodeResult second = decode_instruction(code.data() + 3, 1);
        REQUIRE(second.status == DecodeStatus::Ok);
        CHECK(second.flags == kInsnRet);
    }
    // Map-1 opcodes with an immediate byte: 70, 71, 72, 73, C2, C4, C5, C6.
    expect_len({0xC5, 0xFD, 0x70, 0xC0, 0x1B}, 5);               // vpshufd ymm0, ymm0, 1Bh
    expect_len({0xC5, 0xFA, 0x70, 0xC1, 0x00}, 5);               // vpshufhw xmm0, xmm1, 0
    expect_len({0xC5, 0xFB, 0x70, 0x01, 0x00}, 5);               // vpshuflw xmm0, [rcx], 0
    expect_len({0xC5, 0xF9, 0x71, 0xD0, 0x04}, 5);               // vpsrlw xmm0, xmm0, 4        (group 12, /2)
    expect_len({0xC5, 0xF1, 0x72, 0xF0, 0x04}, 5);               // vpslld xmm1, xmm0, 4        (group 13, /6)
    expect_len({0xC5, 0xF9, 0x73, 0xD8, 0x08}, 5);               // vpsrldq xmm0, xmm0, 8       (group 14, /3)
    expect_len({0xC5, 0xF8, 0xC2, 0xC1, 0x00}, 5);               // vcmpeqps xmm0, xmm0, xmm1
    expect_len({0xC5, 0xFB, 0xC2, 0x44, 0x24, 0x08, 0x01}, 7);   // vcmpltsd xmm0, xmm0, [rsp+8]
    expect_len({0xC5, 0xF9, 0xC4, 0xC1, 0x01}, 5);               // vpinsrw xmm0, xmm0, ecx, 1
    expect_len({0xC5, 0xF9, 0xC5, 0xC0, 0x02}, 5);               // vpextrw eax, xmm0, 2
    expect_len({0xC5, 0xF8, 0xC6, 0xC1, 0x44}, 5);               // vshufps xmm0, xmm0, xmm1, 44h
    // Their neighbours have none.
    expect_len({0xC5, 0xF9, 0x74, 0xC1}, 4);                     // vpcmpeqb
    expect_len({0xC5, 0xF9, 0x6F, 0xC1}, 4);                     // vmovdqa
    expect_len({0xC5, 0xFA, 0x7E, 0xC1}, 4);                     // vmovq xmm0, xmm1
    expect_len({0xC5, 0xF8, 0xC6, 0x05, 0, 0, 0, 0, 0x44}, 9, kInsnRipRel);  // vshufps with [rip+disp32]: disp32 then imm8
    // The opmask (k-register) instructions of AVX-512 are VEX-encoded.
    expect_len({0xC5, 0xF8, 0x90, 0xC1}, 4);                     // kmovw k0, k1
    expect_len({0xC5, 0xFB, 0x92, 0xC8}, 4);                     // kmovd k1, eax
    expect_len({0xC5, 0xF4, 0x41, 0xCA}, 4);                     // kandw k1, k1, k2
    expect_len({0xC5, 0xF8, 0x98, 0xC1}, 4);                     // kortestw k0, k1
    expect_len({0xC4, 0xE3, 0x79, 0x32, 0xC9, 0x04}, 6);         // kshiftlb k1, k1, 4
}

TEST_CASE("EVEX: four-byte prefix, maps 1 to 3, compressed displacement, masking, broadcast, rounding") {
    expect_len({0x62, 0xF1, 0x7C, 0x48, 0x28, 0xC1}, 6);         // vmovaps zmm0, zmm1
    expect_len({0x62, 0xF1, 0x7C, 0x08, 0x28, 0xC1}, 6);         // vmovaps xmm0, xmm1 (EVEX.128)
    expect_len({0x62, 0xE1, 0x7C, 0x08, 0x28, 0xC1}, 6);         // vmovaps xmm16, xmm1         (R' = 0)
    expect_len({0x62, 0xF1, 0x7C, 0x48, 0x10, 0x01}, 6);         // vmovups zmm0, [rcx]
    expect_len({0x62, 0xF1, 0x7C, 0x48, 0x10, 0x41, 0x01}, 7);   // vmovups zmm0, [rcx+40h]     (disp8*64 is one byte)
    expect_len({0x62, 0xF1, 0x7C, 0x48, 0x10, 0x81, 0x00, 0x01, 0x00, 0x00}, 10);  // vmovups zmm0, [rcx+100h]
    expect_len({0x62, 0xF1, 0x7C, 0x48, 0x10, 0x05, 0x00, 0x00, 0x00, 0x00}, 10, kInsnRipRel);
    expect_len({0x62, 0xF1, 0xFE, 0x48, 0x6F, 0x44, 0x24, 0x01}, 8);  // vmovdqu64 zmm0, [rsp+40h] (SIB, disp8)
    expect_len({0x62, 0xF1, 0xFE, 0x48, 0x7F, 0x01}, 6);         // vmovdqu64 [rcx], zmm0
    expect_len({0x62, 0xF1, 0x7F, 0x48, 0x6F, 0x01}, 6);         // vmovdqu8 zmm0, [rcx]        (pp = F2)
    expect_len({0x62, 0xF1, 0x7C, 0xC9, 0x28, 0xC2}, 6);         // vmovaps zmm0{k1}{z}, zmm2
    expect_len({0x62, 0xF1, 0x7C, 0x58, 0x58, 0x01}, 6);         // vaddps zmm0, zmm0, [rcx]{1to16}
    expect_len({0x62, 0xF1, 0x7C, 0x18, 0x58, 0xC1}, 6);         // vaddps zmm0, zmm0, zmm1, {rn-sae}
    expect_len({0x62, 0xF1, 0x7C, 0x78, 0x58, 0xC1}, 6);         // vaddps zmm0, zmm0, zmm1, {rz-sae} (L'L = 3 as rounding)
    expect_len({0x62, 0xF1, 0x7D, 0x48, 0xEF, 0xC0}, 6);         // vpxord zmm0, zmm0, zmm0
    expect_len({0x62, 0xF1, 0x7D, 0x48, 0x74, 0xC1}, 6);         // vpcmpeqb k0, zmm0, zmm1
    expect_len({0x62, 0xF1, 0x7F, 0x08, 0x7B, 0xC1}, 6);         // vcvtusi2sd xmm0, xmm0, ecx
    // Map 1 opcodes with an immediate.
    expect_len({0x62, 0xF1, 0x7C, 0x48, 0xC2, 0xC1, 0x00}, 7);   // vcmpeqps k0, zmm0, zmm1
    expect_len({0x62, 0xF1, 0x7D, 0x48, 0x72, 0xF0, 0x04}, 7);   // vpslld zmm0, zmm0, 4
    expect_len({0x62, 0xF1, 0x7D, 0x48, 0x72, 0xC8, 0x04}, 7);   // vprold zmm0, zmm0, 4        (/1: EVEX only)
    expect_len({0x62, 0xF1, 0x7D, 0x48, 0x70, 0x41, 0x01, 0x1B}, 8);  // vpshufd zmm0, [rcx+40h], 1Bh
    // Map 2.
    expect_len({0x62, 0xF2, 0x7D, 0x48, 0x7C, 0xC0}, 6);         // vpbroadcastd zmm0, eax
    expect_len({0x62, 0xF2, 0xFD, 0x48, 0x59, 0x01}, 6);         // vpbroadcastq zmm0, [rcx]
    expect_len({0x62, 0xF2, 0x7E, 0x48, 0x28, 0xC1}, 6);         // vpmovm2b zmm0, k1           (pp = F3)
    expect_len({0x62, 0xF2, 0x7E, 0x48, 0x31, 0x01}, 6);         // vpmovdb [rcx], zmm0
    expect_len({0x62, 0xF2, 0x75, 0x48, 0xB8, 0xC2}, 6);         // vfmadd231ps zmm0, zmm1, zmm2
    expect_len({0x62, 0xF2, 0xFD, 0x49, 0xA0, 0x04, 0x88}, 7);   // vpscatterdq [rax+ymm1*4]{k1}, zmm0 (VSIB)
    expect_len({0x62, 0xF2, 0x7D, 0x49, 0x92, 0x44, 0x88, 0x04}, 8);  // vgatherdps zmm0{k1}, [rax+zmm1*4+10h] (disp8*4)
    // Map 3: always an immediate byte.
    expect_len({0x62, 0xF3, 0x7D, 0x48, 0x25, 0xC1, 0xFF}, 7);   // vpternlogd zmm0, zmm0, zmm1, 0FFh
    expect_len({0x62, 0xF3, 0x7D, 0x48, 0x3F, 0xC1, 0x02}, 7);   // vpcmpb k0, zmm0, zmm1, 2
    expect_len({0x62, 0xF3, 0xFD, 0x48, 0x1B, 0xC1, 0x01}, 7);   // vextractf64x4 ymm1, zmm0, 1
    expect_len({0x62, 0xF3, 0xFD, 0x48, 0x00, 0x44, 0x24, 0x01, 0xD8}, 9);  // vpermq zmm0, [rsp+40h], 0D8h
}

TEST_CASE("VEX / EVEX: what is refused") {
    // 66, F2, F3, lock or REX in front of a VEX or EVEX prefix is #UD (SDM volume 2, 2.3.2 and 2.7.1).
    for (const std::uint8_t prefix : std::initializer_list<std::uint8_t>{0x66, 0xF2, 0xF3, 0xF0, 0x40, 0x48, 0x4F}) {
        INFO("prefix " << static_cast<unsigned>(prefix));
        expect_unknown({prefix, 0xC5, 0xF8, 0x28, 0xC1});
        expect_unknown({prefix, 0xC4, 0xE2, 0x7D, 0x18, 0xC0});
        expect_unknown({prefix, 0x62, 0xF1, 0x7C, 0x48, 0x28, 0xC1});
    }
    expect_unknown({0x66, 0x2E, 0xC5, 0xF8, 0x28, 0xC1});        // ... also with an allowed prefix in between
    expect_unknown({0x48, 0x2E, 0xC5, 0xF8, 0x28, 0xC1});
    expect_unknown({0x65, 0xF3, 0xC5, 0xF8, 0x28, 0xC1});
    // In 64-bit mode C4, C5 and 62 are never LES, LDS or BOUND: a ModRM-like second byte with mod != 3 does not make
    // them something else. C5 00 ...: R = 0, vvvv = 0000, L = 0, pp = 00, then opcode 28.
    expect_len({0xC5, 0x00, 0x28, 0xC1}, 4);                     // vmovaps xmm8, xmm1 with vvvv = 0 (#UD, same length)
    // Maps other than 1, 2, 3.
    expect_unknown({0xC4, 0xE0, 0x78, 0x28, 0xC1});              // map 0
    expect_unknown({0xC4, 0xE4, 0x78, 0x28, 0xC1});              // map 4
    expect_unknown({0xC4, 0xE7, 0x78, 0x28, 0xC1});              // map 7
    expect_unknown({0xC4, 0xE8, 0x78, 0x85, 0xC1, 0x00});        // map 8 (the XOP numbering)
    expect_unknown({0xC4, 0xFF, 0x78, 0x28, 0xC1});              // map 31
    expect_unknown({0x62, 0xF0, 0x7C, 0x48, 0x28, 0xC1});        // EVEX map 0
    expect_unknown({0x62, 0xF4, 0x7C, 0x08, 0x01, 0xC1});        // EVEX map 4 (APX)
    expect_unknown({0x62, 0xF5, 0x7C, 0x48, 0x58, 0xC1});        // EVEX map 5 (AVX512-FP16: vaddph)
    expect_unknown({0x62, 0xF6, 0x7D, 0x48, 0x98, 0xC1});        // EVEX map 6
    expect_unknown({0x62, 0xF7, 0x7C, 0x48, 0x28, 0xC1});        // EVEX map 7
    expect_unknown({0x62, 0xF9, 0x7C, 0x48, 0x28, 0xC1});        // the reserved bit 3 of the first payload byte
    expect_unknown({0x62, 0xF1, 0x78, 0x48, 0x28, 0xC1});        // the fixed 1 bit of the second payload byte is 0
    expect_unknown({0x62, 0xF1, 0x7C, 0xC8, 0x28, 0xC1});        // zeroing ({z}) without a mask register
    expect_unknown({0x62, 0xF1, 0x7C, 0x68, 0x28, 0xC1});        // L'L = 3 without EVEX.b
    expect_unknown({0x62, 0xF1, 0x7C, 0x78, 0x58, 0x01});        // L'L = 3 with a memory operand
    // Opcodes that exist in the legacy map but not under VEX: sharing the map byte does not make them instructions.
    expect_unknown({0xC5, 0xF8, 0x05});                          // 0F 05 syscall
    expect_unknown({0xC5, 0xF8, 0x0B});                          // 0F 0B ud2
    expect_unknown({0xC5, 0xF8, 0x1F, 0x00});                    // 0F 1F nop
    expect_unknown({0xC5, 0xF8, 0xA2});                          // 0F A2 cpuid
    expect_unknown({0xC5, 0xF8, 0xAF, 0xC1});                    // 0F AF imul
    expect_unknown({0xC5, 0xF8, 0xB6, 0xC1});                    // 0F B6 movzx
    expect_unknown({0xC5, 0xF8, 0x80, 0x00, 0x00, 0x00, 0x00});  // 0F 80 jo rel32
    expect_unknown({0xC5, 0xF8, 0x40, 0xC1});                    // 0F 40 cmovo
    expect_unknown({0xC5, 0xF8, 0xC3, 0x01});                    // 0F C3 movnti
    expect_unknown({0xC5, 0xF8, 0xC7, 0x09});                    // 0F C7 /1 cmpxchg8b
    expect_unknown({0xC5, 0xF8, 0x6F, 0xC1});                    // 0F 6F without a prefix is MMX movq: no VEX form
    expect_unknown({0xC5, 0xF8, 0xD4, 0xC1});                    // MMX paddq
    expect_unknown({0xC5, 0xFA, 0x28, 0xC1});                    // F3 0F 28 is nothing
    expect_unknown({0xC5, 0xFB, 0x6F, 0xC1});                    // F2 0F 6F is EVEX only (vmovdqu8)
    expect_unknown({0xC5, 0xF9, 0x77});                          // 66 0F 77 is nothing
    expect_unknown({0xC5, 0xF8, 0xFF, 0xC1});                    // 0F FF
    expect_unknown({0xC4, 0xE2, 0x78, 0xF0, 0x01});              // 0F 38 F0 movbe
    expect_unknown({0xC4, 0xE2, 0x7B, 0xF0, 0xC1});              // F2 0F 38 F0 crc32
    expect_unknown({0xC4, 0xE2, 0x78, 0xC8, 0xC1});              // 0F 38 C8 sha1nexte
    expect_unknown({0xC4, 0xE2, 0x79, 0x80, 0x01});              // 66 0F 38 80 invept
    expect_unknown({0xC4, 0xE2, 0x79, 0x10, 0xC1});              // 66 0F 38 10 pblendvb (the VEX form is 0F 3A 4C)
    expect_unknown({0xC4, 0xE3, 0x78, 0xCC, 0xC1, 0x00});        // 0F 3A CC sha1rnds4
    expect_unknown({0xC4, 0xE3, 0x78, 0x0F, 0xC1, 0x00});        // 0F 3A 0F without 66 (MMX palignr)
    // VEX instructions that have no EVEX form.
    expect_unknown({0x62, 0xF1, 0x7C, 0x48, 0x77});              // vzeroupper
    expect_unknown({0x62, 0xF1, 0x7C, 0x48, 0x50, 0xC1});        // vmovmskps
    expect_unknown({0x62, 0xF1, 0x7D, 0x48, 0xD7, 0xC1});        // vpmovmskb
    expect_unknown({0x62, 0xF1, 0x7F, 0x48, 0xF0, 0x01});        // vlddqu
    expect_unknown({0x62, 0xF1, 0x7C, 0x48, 0xAE, 0x10});        // vldmxcsr
    expect_unknown({0x62, 0xF2, 0x7D, 0x48, 0x17, 0xC1});        // vptest
    expect_unknown({0x62, 0xF2, 0x78, 0x48, 0xF2, 0xC1});        // andn
    expect_unknown({0x62, 0xF3, 0x7D, 0x48, 0x4A, 0xC1, 0x00});  // vblendvps
    // Sets this decoder leaves out (docs/wp/WP-38.md): FMA4, AMX.
    expect_unknown({0xC4, 0xE3, 0xF9, 0x69, 0xC2, 0x10});        // vfmaddpd xmm0, xmm0, xmm1, xmm2 (FMA4)
    expect_unknown({0xC4, 0xE2, 0x78, 0x49, 0xC0});              // tilerelease
}

TEST_CASE("legacy maps: forms that are not instructions are refused (found by the reference comparison, WP-38)") {
    // A lock prefix is #UD except on a read-modify-write instruction with a memory destination.
    expect_len({0xF0, 0xFF, 0x01}, 3);                           // lock inc dword [rcx]
    expect_len({0xF0, 0x83, 0x09, 0x01}, 4);                     // lock or dword [rcx], 1
    expect_len({0xF0, 0x0F, 0xBA, 0x29, 0x03}, 5);               // lock bts dword [rcx], 3
    expect_len({0xF0, 0x48, 0x0F, 0xC7, 0x0E}, 5);               // lock cmpxchg16b [rsi]
    expect_len({0xF0, 0x87, 0x01}, 3);                           // lock xchg [rcx], eax
    expect_len({0xF0, 0xF7, 0x19}, 3);                           // lock neg dword [rcx]
    expect_unknown({0xF0, 0xFF, 0xC0});                          // lock inc eax (register destination)
    expect_unknown({0xF0, 0x8B, 0x01});                          // lock mov
    expect_unknown({0xF0, 0x03, 0x01});                          // lock add eax, [rcx] (memory is the source)
    expect_unknown({0xF0, 0x39, 0x01});                          // lock cmp
    expect_unknown({0xF0, 0x83, 0x39, 0x01});                    // lock cmp dword [rcx], 1
    expect_unknown({0xF0, 0x0F, 0xBA, 0x21, 0x03});              // lock bt
    expect_unknown({0xF0, 0x90});                                // lock nop
    expect_unknown({0xF0, 0x0F, 0xAF, 0x01});                    // lock imul
    // Operand forms an opcode does not have.
    expect_unknown({0x8D, 0xC1});                                // lea with a register operand
    expect_unknown({0x8C, 0xF0});                                // mov eax, segment register 6
    expect_unknown({0x8E, 0xC8});                                // mov cs, eax
    expect_len({0x8C, 0xE0}, 2);                                 // mov eax, fs
    expect_len({0x8E, 0xD8}, 2);                                 // mov ds, eax
    expect_unknown({0x0F, 0x50, 0x01});                          // movmskps from memory
    expect_unknown({0x0F, 0x2B, 0xC1});                          // movntps to a register
    expect_unknown({0x0F, 0xC3, 0xC1});                          // movnti to a register
    expect_unknown({0x66, 0x0F, 0x73, 0x19, 0x08});              // psrldq with a memory operand
    expect_unknown({0x0F, 0x73, 0xD9, 0x08});                    // psrldq without 66
    expect_unknown({0x66, 0x0F, 0x71, 0xC8, 0x08});              // group 12 /1
    // Mandatory prefixes: an SSE opcode exists only in the forms the tables name.
    expect_unknown({0x0F, 0x6C, 0xC1});                          // punpcklqdq needs 66
    expect_unknown({0xF3, 0x0F, 0x28, 0xC1});                    // F3 on movaps
    expect_unknown({0x0F, 0xB8, 0xC1});                          // popcnt needs F3
    expect_unknown({0x0F, 0xF0, 0x01});                          // lddqu needs F2
    expect_unknown({0x0F, 0x38, 0x17, 0xC1});                    // ptest needs 66
    expect_unknown({0xF3, 0x0F, 0x38, 0x00, 0xC1});              // F3 on pshufb
    expect_unknown({0x0F, 0x38, 0x50, 0xC1});                    // nothing at 0F 38 50
    expect_unknown({0x0F, 0x3A, 0x0A, 0xC1, 0x00});              // roundss needs 66
    expect_unknown({0x0F, 0x3A, 0x00, 0xC1, 0x00});              // nothing at 0F 3A 00 without VEX
    expect_len({0x66, 0x0F, 0x3A, 0x0A, 0xC1, 0x00}, 6);         // roundss
    expect_len({0xF2, 0x0F, 0x38, 0xF1, 0xC1}, 5);               // crc32 eax, ecx
    expect_len({0x0F, 0x38, 0xF0, 0x01}, 4);                     // movbe eax, [rcx]
    expect_unknown({0x0F, 0x38, 0xF0, 0xC1});                    // movbe with a register operand
    // The last F2 / F3 prefix selects the form, 66 only when there is neither.
    expect_len({0x66, 0xF2, 0x0F, 0x58, 0xC1}, 5);               // addsd with a stray 66
    expect_unknown({0x66, 0xF3, 0x0F, 0x6C, 0xC1});              // F3 wins over 66: punpcklqdq has no F3 form
    // x87: the slots the SDM leaves undefined.
    expect_unknown({0xD9, 0x09});                                // D9 /1 with memory
    expect_unknown({0xD9, 0xD1});                                // D9 D1
    expect_unknown({0xDB, 0x21});                                // DB /4 with memory
    expect_unknown({0xDD, 0x29});                                // DD /5 with memory
    expect_unknown({0xDF, 0xE1});                                // DF E1
    expect_len({0xD9, 0xD0}, 2);                                 // fnop
    expect_len({0xDB, 0xE3}, 2);                                 // fninit
    expect_len({0xDE, 0xD9}, 2);                                 // fcompp
    // Groups 7, 9 and 15.
    expect_len({0x0F, 0x01, 0xF9}, 3);                           // rdtscp
    expect_len({0x0F, 0x01, 0xD5}, 3);                           // xend
    expect_unknown({0x0F, 0x01, 0xC7});                          // nothing
    expect_unknown({0x0F, 0x01, 0x28});                          // 0F 01 /5 with memory, no prefix
    expect_len({0x0F, 0xC7, 0xF0}, 3);                           // rdrand eax
    expect_len({0xF3, 0x0F, 0xC7, 0xF8}, 4);                     // rdpid rax
    expect_unknown({0x0F, 0xC7, 0xC8});                          // cmpxchg8b with a register operand
    expect_unknown({0x0F, 0xC7, 0x01});                          // 0F C7 /0
    expect_len({0x0F, 0xAE, 0xE8}, 3);                           // lfence
    expect_len({0xF3, 0x0F, 0xAE, 0xC0}, 4);                     // rdfsbase eax
    expect_len({0x66, 0x0F, 0xAE, 0x39}, 4);                     // clflushopt [rcx]
    expect_unknown({0x0F, 0xAE, 0xC0});                          // 0F AE /0 with a register, no prefix
}
