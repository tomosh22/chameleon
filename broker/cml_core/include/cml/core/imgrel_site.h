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
//      fs/gs override (the address would be relative to that segment base); its memory operand has a base register
//      (`[index*scale+disp32]` and the absolute `[disp32]` have no register that could hold __ImageBase); and it is not
//      a `lea` with a 32-bit or 16-bit destination (8D without REX.W). With the base register holding __ImageBase such a
//      lea of the full address already truncates in the original code; what it can validly compute is an RVA, as in
//      `lea r32,[index+sym@IMGREL]` (a 32-bit sum of the image-relative offset), and retargeting would turn that into
//      the low 32 bits of `target - patch_base + index`, which is not the old RVA. The rule protects that RVA arithmetic
//      (WP-40 wording, R-50; the rule is unchanged);
//   5. every relocation the sweep passes on the way (and the site's own) is EXACTLY the 32-bit displacement of one
//      decoded instruction, or lies wholly inside its immediate. A relocation that starts in one instruction and ends
//      in another, or covers prefix, opcode, ModRM or SIB bytes, means the sweep ran into data (a jump table) or lost
//      the instruction boundaries; from there on nothing is trusted.
// An instruction the decoder does not know stops the sweep as well: every site behind it is refused.
//
// The decoder reports the length and the displacement of an instruction, not where its ModRM byte or its immediate
// are. parse_layout() re-derives those from the instruction's own bytes (prefixes, REX, opcode, ModRM, SIB) and
// CHECKS the result against the decoder's displacement: on any disagreement the layout is unknown and no relocation
// in that instruction is accepted. For a VEX/EVEX instruction a relocation is accepted only at the displacement.

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
    NoBaseRegister,   // the operand is `[index*scale+disp32]` or `[disp32]`: no base register that could hold __ImageBase
    LeaTruncates,     // a `lea` with a 32-bit or 16-bit destination: it computes a 32-bit image-relative value
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

// Where the operands of one decoded instruction are, re-derived from its bytes and checked against the decoder.
struct InsnLayout {
    bool known = false;      // parsed, and the displacement found here is the one the decoder reported
    bool vex = false;        // VEX (C4, C5) or EVEX (62) encoded: only the displacement is located
    bool rex_w = false;      // REX.W (legacy encodings)
    bool has_modrm = false;
    bool no_base = false;    // memory operand without a base register (mod=00, r/m=100, SIB base=101)
    bool lea = false;        // opcode 8D
    // Offset of the first byte behind prefixes, opcode, ModRM, SIB and displacement: where an immediate starts, if the
    // instruction has one. Equal to the length when it has none. Legacy encodings only.
    std::uint8_t immediate_start = 0;
};

// `insn`: the `length` bytes of ONE instruction the length decoder accepted; `disp_offset` / `disp_size`: what the
// decoder reported for it. Never reads beyond `length`. Portable, allocation-free.
InsnLayout parse_layout(const std::uint8_t* insn, std::size_t length, std::uint8_t disp_offset, std::uint8_t disp_size) noexcept;

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
        InsnLayout layout;
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
