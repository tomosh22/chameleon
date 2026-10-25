#pragma once

// A small x64 instruction-length decoder (plan section 4.8, WP-10).
//
// The hook installer must know where instructions start and end at the top of a function: a
// 5-byte `jmp rel32` written over the entry may split an instruction (ADR-0020 item 4), which is
// only safe if the whole range up to the end of the last touched instruction is treated as
// overwritten. This decoder gives the lengths. It does not give operands, mnemonics or targets.
//
// Scope: the integer, x87 and legacy-prefix SSE/SSE2..SSE4/AES (no VEX, no EVEX, no XOP) instructions
// that cl and clang-cl emit in prologues and early function bodies, written from the Intel SDM
// opcode maps (volume 2, appendix A). Anything else is reported as Unknown and the caller must fail
// safe: a wrong length would make the installer overwrite half of an instruction that can still be
// reached, so "unknown" is always preferred over "probably".
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

struct DecodeResult {
    DecodeStatus status = DecodeStatus::Unknown;
    std::uint8_t length = 0;  // 1..15 when status == Ok, else 0
    std::uint8_t flags = 0;   // kInsn* bits, only when status == Ok
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
