#pragma once

// Choosing how to hook a function (plan section 4.8, ADR-0020 items 4 and 5). Portable: it works on
// copies of the bytes around the entry, so it is unit-tested on any host.
//
// Strategy 1, PaddingTwoByte. At least 6 bytes of 0xCC / 0x90 precede the entry, the first
//   instruction is at least 2 bytes long, the entry is 2-byte aligned (so that the 16-bit exchange
//   never straddles a cache line) and the padding lies in executable memory (a jump written into a
//   data page would land on bytes the CPU cannot run). `jmp rel32` goes into the padding (the 5 bytes just
//   before the entry), then the 2 bytes at the entry become `EB F9` (`jmp -7`, which lands on the
//   padding jump). The entry write is one aligned-or-not 16-bit interlocked exchange, so a thread
//   that is not suspended sees the old or the new instruction, never half of one.
// Strategy 2, FiveByte. `E9 rel32` at the entry. The 5 bytes usually split an instruction
//   (cl emits `48 83 ec 28`: the jump ends in the middle of `sub rsp, 28h`); that is safe because
//   the hooked function's original body is never entered again at its entry, and nothing can be
//   executing inside the overwritten range, which the installer proves by checking every thread's
//   RIP against the range from the entry to the END of the last touched instruction. Functions
//   shorter than 5 bytes are accepted only when the bytes after them up to byte 5 are padding.
//
// Neither strategy relocates the original bytes: the hook replaces the function, it does not
// wrap it. A call instruction that ENDS inside the overwritten range is refused, because a thread
// that is inside the callee would return into overwritten bytes.

#include <cstddef>
#include <cstdint>

namespace cml::agent::hook {

enum class Strategy : std::uint8_t {
    None = 0,
    PaddingTwoByte = 1,
    FiveByte = 2,
};

const char* to_string(Strategy strategy) noexcept;

inline constexpr std::size_t kJmpRel32Size = 5;    // E9 rel32
inline constexpr std::size_t kShortJmpSize = 2;    // EB F9
inline constexpr std::size_t kMinPadding = 6;      // bytes of CC/90 required before the entry (5 are written)
inline constexpr std::size_t kLookBehind = 8;      // bytes before the entry that plan_hook may look at
inline constexpr std::size_t kLookAhead = 32;      // bytes from the entry that plan_hook may look at
inline constexpr std::size_t kMaxPatchRegion = 16; // bytes the installer saves per hook for revert

struct HookPlan {
    Strategy strategy = Strategy::None;
    std::uint32_t diag = 0;  // CML43xx when strategy == None

    // When diag != 0 and an instruction is to blame: its offset from the entry and its first bytes.
    std::uint32_t fail_offset = 0;
    std::uint8_t fail_bytes[4] = {0, 0, 0, 0};
    std::uint8_t fail_byte_count = 0;

    std::uint32_t first_insn_len = 0;
    // Bytes the hook writes at the entry: 2 (strategy 1) or 5 (strategy 2).
    std::uint32_t entry_write_len = 0;
    // Bytes written in front of the entry: 5 for strategy 1, 0 for strategy 2.
    std::uint32_t padding_write_len = 0;
    // The RIP window is [entry - window_before, entry + window_after).
    std::uint32_t window_before = 0;
    std::uint32_t window_after = 0;
    // Strategy 2: the function is shorter than the jump and the rest of the 5 bytes is padding.
    bool short_function = false;
};

// Facts about the addresses that the byte copies do not show. The defaults suit the unit tests of
// the portable planner; the installer fills both in from the real address and VirtualQuery.
struct PlanEnvironment {
    bool entry_two_byte_aligned = true;  // the entry address is even
    bool padding_executable = true;      // the 6 bytes before the entry are committed, executable memory
};

// `before` holds the `before_len` (<= kLookBehind) bytes immediately preceding the entry, so
// before[before_len - 1] is the byte at entry - 1; `at` holds the `at_len` (<= kLookAhead, at least
// the readable part) bytes from the entry on.
HookPlan plan_hook(const std::uint8_t* before, std::size_t before_len, const std::uint8_t* at, std::size_t at_len,
                   const PlanEnvironment& environment = {}) noexcept;

// E9 rel32 placed at address `site` jumping to `target`. False when the displacement does not fit
// in 32 bits (the target is more than 2 GB away).
bool encode_jmp_rel32(std::uint8_t out[5], std::uint64_t site, std::uint64_t target) noexcept;

}  // namespace cml::agent::hook
