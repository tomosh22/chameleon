#pragma once

// A small x64 instruction-length decoder (plan section 4.8, WP-10).
//
// The hook installer must know where instructions start and end at the top of a function: a
// 5-byte `jmp rel32` written over the entry may split an instruction (ADR-0020 item 4), which is
// only safe if the whole range up to the end of the last touched instruction is treated as
// overwritten. This decoder gives the lengths. It does not give mnemonics, registers or targets.
// The one operand fact it reports is where the displacement of a memory operand sits: the broker's
// bind planner compiles this same file in and uses that to prove that a relocation site is a
// sign-extended displacement (ADR-0042).
//
// Scope: the integer, x87 and SSE/SSE2..SSE4/AES/SHA instructions that cl and clang-cl emit, and (WP-38,
// ADR-0042, which reverses WP-10's "no VEX, no EVEX") the VEX (C4, C5) and EVEX (62) encoded ones of opcode
// maps 1 to 3: AVX, AVX2, FMA, BMI, the opmask instructions, AVX-512. Written from the Intel SDM (volume 2:
// 2.3 and 2.7 for the prefixes, appendix A and the instruction pages for the opcodes). Anything else (XOP,
// 3DNow!, other VEX / EVEX maps, a legacy prefix or REX in front of VEX / EVEX, an opcode or operand form the
// tables do not name) is reported as Unknown and the caller must fail safe: a wrong length would make the
// installer overwrite half of an instruction that can still be reached, so "unknown" is always preferred over
// "probably".
//
// The lengths are checked against a reference disassembler (Zydis) by the fuzz target and the differential
// sweep of tests/fuzz (lde_reference.h has the rule); the decoder itself contains no third-party code.
//
// The decoder is table driven (the tables are built at compile time from range rules), portable
// (no OS dependency), allocation-free and never reads beyond `available` bytes.

#include <cstddef>
#include <cstdint>

namespace cml::agent::hook {

enum class DecodeStatus : std::uint8_t {
    Ok,         // length and flags are valid
    Unknown,    // an opcode, prefix combination or ModRM form this decoder does not cover (or is invalid)
    Truncated,  // the instruction is understood but runs past `available` bytes
};

// Bits of DecodeResult::flags.
inline constexpr std::uint8_t kInsnCall = 0x01;    // call rel32, call r/m (the return address is the next instruction)
inline constexpr std::uint8_t kInsnJmp = 0x02;     // unconditional jump (rel8, rel32, r/m)
inline constexpr std::uint8_t kInsnJcc = 0x04;     // conditional jump, loop, jrcxz
inline constexpr std::uint8_t kInsnRet = 0x08;     // ret, ret imm16
inline constexpr std::uint8_t kInsnTrap = 0x10;    // int3, int imm8, ud2: control does not continue
inline constexpr std::uint8_t kInsnRipRel = 0x20;  // has a RIP-relative memory operand
inline constexpr std::uint8_t kInsnAddr32 = 0x40;  // has a 67 prefix: a memory operand's address is truncated to 32 bits
inline constexpr std::uint8_t kInsnSegFsGs = 0x80; // has an fs or gs override: a memory operand is relative to that base

struct DecodeResult {
    DecodeStatus status = DecodeStatus::Unknown;
    std::uint8_t length = 0;  // 1..15 when status == Ok, else 0
    std::uint8_t flags = 0;   // kInsn* bits, only when status == Ok
    // The displacement of the ModRM memory operand, only when status == Ok (ADR-0042): `disp_size` is 0 (no memory
    // operand, or one without a displacement), 1 or 4, and `disp_offset` is where it starts, counted from the first
    // byte of the instruction (0 when disp_size == 0). The processor sign-extends a displacement to the address size,
    // which an immediate of the same width is not guaranteed to be. The address computed from it is a plain 64-bit
    // address only when none of kInsnRipRel, kInsnAddr32 and kInsnSegFsGs is set. Branch targets (rel8/rel32) are
    // immediates here, never displacements; the moffs forms (A0..A3) are not covered at all (Unknown).
    // One exception to "sign-extends" (WP-38): the 1-byte displacement of an EVEX-encoded instruction (62 ...) is a
    // compressed displacement, which the processor multiplies by a factor N that depends on the instruction (disp8*N,
    // SDM volume 2, 2.7.5) before it sign-extends it. `disp_size == 1` then says only where the byte is and that it is
    // one byte; no flag marks it, so a user that needs the value must look at the first non-prefix byte for 62 or
    // restrict itself to `disp_size == 4`, which is the literal sign-extended displacement under every encoding.
    std::uint8_t disp_offset = 0;
    std::uint8_t disp_size = 0;
};

// Length of the one instruction at `code`, looking at no more than `available` bytes.
DecodeResult decode_instruction(const std::uint8_t* code, std::size_t available) noexcept;

// True for instructions after which execution does not fall through to the next one
// (ret, unconditional jump, int3/ud2): the end of a function's straight-line code.
inline constexpr bool ends_flow(std::uint8_t flags) noexcept {
    return (flags & (kInsnRet | kInsnJmp | kInsnTrap)) != 0;
}

// The bytes functions are padded with (int3 and single-byte nop).
inline constexpr bool is_padding_byte(std::uint8_t b) noexcept {
    return b == 0xCC || b == 0x90;
}

}  // namespace cml::agent::hook
