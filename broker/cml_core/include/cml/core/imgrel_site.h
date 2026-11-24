#pragma once

// Proving that a relocation site is a sign-extended address displacement (WP-36, ADR-0042 decision 3).
//
// Under cl with optimisation a function that holds `__ImageBase` in a register addresses globals as
// `[reg + index*scale + sym@IMGREL]`: an IMAGE_REL_AMD64_ADDR32NB relocation on the 32-bit displacement of a
// ModRM/SIB memory operand. The processor sign-extends a displacement, so such a site can be retargeted to data of
// the old image with `int32(S + A - patch_image_base)` (fix-up kind Addr32NbDisp). The same relocation type on
// anything else (a jump-table entry, an immediate that is zero-extended, unwind data) cannot. The planner therefore
// accepts a site only when this file PROVES the first case; everything else keeps CML3101. Nothing is guessed.
//
// The proof, per site:
//   1. the section is code;
//   2. the instructions are decoded linearly, with the agent's length decoder (agent/src/hook/length_decoder.cpp,
//      compiled into the broker unchanged), from the nearest FUNCTION symbol at or before the site. Not from the
//      section start: a section can hold several functions (/Gy-, #pragma code_seg) and each one's jump table
//      follows its code;
//   3. the sweep reaches an instruction whose displacement is 4 bytes wide and starts exactly at the site;
//   4. that instruction has no RIP-relative operand, no 67 prefix (the address would be truncated to 32 bits) and no
//      fs/gs override (the address would be relative to that segment base);
//   5. every relocation the sweep passes on the way (and the site's own) lies wholly inside the displacement or the
//      immediate of ONE decoded instruction. A relocation that starts in one instruction and ends in another, or covers
//      opcode bytes, means the sweep ran into data (a jump table) or lost the instruction boundaries; from there on
//      nothing is trusted. The decoder does not report where an immediate starts, so "the immediate" is taken as: the
//      field ends where the instruction ends and starts after the displacement (or after the first byte when there is
//      none). That is narrower than the truth, never wider.
// An instruction the decoder does not know stops the sweep as well: every site behind it is refused.

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace cml::core::bind {

enum class SiteVerdict : std::uint8_t {
    Displacement,     // proved: the 32-bit displacement of a ModRM/SIB memory operand with a plain 64-bit address
    NotCode,          // the section is not code
    OutOfSection,     // the four bytes of the site are not inside the section data
    NoFunction,       // no function symbol at or before the site: there is nowhere to start decoding
    Undecodable,      // the sweep stopped before the site at an instruction the decoder does not know (or a cut-off one)
    RanIntoData,      // the sweep stopped before (or at) the site: a relocation does not fit one instruction's operand field
    NotDisplacement,  // the site is inside a decoded instruction but is not its 32-bit displacement (an immediate, ...)
    RipRelative,      // the displacement belongs to a RIP-relative operand
    Address32,        // the instruction has a 67 prefix
    SegmentFsGs,      // the instruction has an fs or gs override
};

const char* to_string(SiteVerdict v) noexcept;

struct SiteCheck {
    SiteVerdict verdict = SiteVerdict::NotCode;
    std::uint32_t function_start = 0;  // where the sweep started (offset in the section)
    std::uint32_t at = 0;              // the instruction that holds the site, or where the sweep stopped
    std::uint32_t length = 0;          // length of that instruction; 0 when it could not be decoded
    std::string bytes;                 // its bytes ("8b 84 87 00 00 00 00"); up to 15 bytes where the sweep stopped
    std::string reason;                // one clause for the diagnostic, naming the offset and the bytes

    bool proved() const noexcept { return verdict == SiteVerdict::Displacement; }
};

// The field one relocation of the section patches.
struct RelocField {
    std::uint32_t offset = 0;
    std::uint32_t width = 0;  // bytes; 0 = a relocation type of unknown width (never fits an operand field)
};

// One per section. Sweeps are made on demand, one per function start, and kept.
class SiteProver {
public:
    // `data`: the section's bytes. `fields`: every relocation of the section (any order). `function_starts`: the
    // offsets of the function symbols defined in the section (any order, duplicates allowed).
    SiteProver(bool is_code, std::span<const std::byte> data, std::vector<RelocField> fields, std::vector<std::uint32_t> function_starts);

    SiteCheck check(std::uint32_t site);

    // "8b 84 87 00 00 00 00" for `count` bytes at `offset` (clipped to the section).
    std::string hex_bytes(std::uint32_t offset, std::uint32_t count) const;

private:
    struct Insn {
        std::uint32_t start = 0;
        std::uint8_t length = 0;
        std::uint8_t flags = 0;
        std::uint8_t disp_offset = 0;
        std::uint8_t disp_size = 0;
    };
    struct Sweep {
        std::vector<Insn> insns;        // the trusted instructions, in order
        std::uint32_t end = 0;          // first byte that is not trusted any more
        SiteVerdict stop = SiteVerdict::Undecodable;  // why the sweep stopped at `end` (when it did before `limit`)
        std::uint32_t stop_length = 0;  // length of the instruction decoded at `end`, when one was
        std::string stop_reason;
        bool complete = false;          // reached the next function (or the section end) without a stop
    };

    const Sweep& sweep_from(std::uint32_t start);

    bool is_code_ = false;
    std::span<const std::byte> data_;
    std::vector<RelocField> fields_;       // by offset
    std::vector<std::uint32_t> starts_;    // sorted, unique
    std::map<std::uint32_t, Sweep> sweeps_;
};

}  // namespace cml::core::bind
