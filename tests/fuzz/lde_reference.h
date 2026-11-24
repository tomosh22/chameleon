#pragma once

// The hook installer's length decoder compared with a reference disassembler (WP-38, ADR-0042 point 3).
//
// The reference is Zydis (MIT, zyantific/zydis), fetched at a pinned commit by .github/workflows/fuzz.yml and handed to
// CMake as CML_ZYDIS_DIR. It is linked only into the executables that include this header (cml_fuzz_lde in a tree
// configured with CML_ZYDIS_DIR, and cml_lde_diff), never into the agent or the broker, and nothing of it is in the
// repository: it is the test oracle, not a source of the decoder's tables.
//
// The rule (`compare`): whenever decode_instruction() says Ok, Zydis must decode the same bytes in 64-bit mode to
//   * the same length,
//   * the same displacement (offset and size) and the same RIP-relative property,
//   * and the prefix flags must be the prefixes that are there (kInsnAddr32 = a 67 byte, kInsnSegFsGs = a 64 or 65 byte).
// Whenever decode_instruction() says Truncated, Zydis must not decode an instruction inside the same bytes.
//
// Zydis may reject bytes the decoder accepts. That is tolerated in exactly one, mechanically checked case (see
// docs/wp/WP-38.md, "Exceptions"): the instruction is VEX- or EVEX-encoded, and Zydis accepts a SIBLING of it (same
// legacy prefixes, same lead byte, map, `pp` and opcode; other values of the fields the length does not depend on:
// the register-extension bits, W, vvvv, L, the EVEX mask / zeroing / broadcast bits and the ModRM register and operand
// form), and on that sibling both decoders agree. Such an encoding is #UD on a processor only because of one of those
// fields (a vector length or operand form the instruction does not have, a register number out of range, a mask that
// is not allowed), and the decoder deliberately does not look at them. Everything else is a Mismatch.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <Zydis/Zydis.h>

#include "hook/length_decoder.h"

namespace cml::fuzz::lde {

enum class Verdict : std::uint8_t {
    NotCompared,  // the decoder did not say Ok (Unknown, or Truncated where Zydis has no instruction either)
    Agree,        // both decode, and every compared property is equal
    Exception,    // the sibling case above
    Mismatch,     // a disagreement: a failure
};

struct Comparison {
    Verdict verdict = Verdict::NotCompared;
    const char* why = "";            // for Mismatch and Exception
    cml::agent::hook::DecodeResult ours{};
    bool reference_ok = false;       // Zydis decoded the bytes
    unsigned reference_length = 0;   // when reference_ok
    unsigned reference_status = 0;   // the Zydis status code otherwise
    bool vector_encoded = false;     // ours is a VEX / EVEX instruction (when ours is Ok)
};

inline const char* status_name(unsigned status) noexcept {
    switch (static_cast<ZyanStatus>(status)) {
        case ZYDIS_STATUS_NO_MORE_DATA: return "NO_MORE_DATA";
        case ZYDIS_STATUS_DECODING_ERROR: return "DECODING_ERROR";
        case ZYDIS_STATUS_INSTRUCTION_TOO_LONG: return "INSTRUCTION_TOO_LONG";
        case ZYDIS_STATUS_BAD_REGISTER: return "BAD_REGISTER";
        case ZYDIS_STATUS_ILLEGAL_LOCK: return "ILLEGAL_LOCK";
        case ZYDIS_STATUS_ILLEGAL_LEGACY_PFX: return "ILLEGAL_LEGACY_PFX";
        case ZYDIS_STATUS_ILLEGAL_REX: return "ILLEGAL_REX";
        case ZYDIS_STATUS_INVALID_MAP: return "INVALID_MAP";
        case ZYDIS_STATUS_MALFORMED_EVEX: return "MALFORMED_EVEX";
        case ZYDIS_STATUS_MALFORMED_MVEX: return "MALFORMED_MVEX";
        case ZYDIS_STATUS_INVALID_MASK: return "INVALID_MASK";
        default: return "OK_OR_OTHER";
    }
}

class Reference {
public:
    Reference() noexcept {
        ZydisDecoderInit(&decoder_, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        // Zydis decodes 0F 1A / 0F 1B as the MPX instructions by default and then rejects bound registers 4..7. MPX is
        // gone from current processors, where these opcodes are the multi-byte NOPs the decoder takes them for.
        ZydisDecoderEnableMode(&decoder_, ZYDIS_DECODER_MODE_MPX, ZYAN_FALSE);
    }

    bool decode(const std::uint8_t* data, std::size_t size, ZydisDecodedInstruction& out, unsigned& status) const noexcept {
        const ZyanStatus s = ZydisDecoderDecodeInstruction(&decoder_, nullptr, data, size, &out);
        status = static_cast<unsigned>(s);
        return ZYAN_SUCCESS(s);
    }

    Comparison compare(const std::uint8_t* data, std::size_t size) const noexcept {
        using namespace cml::agent::hook;
        Comparison c;
        c.ours = decode_instruction(data, size);
        ZydisDecodedInstruction insn;
        c.reference_ok = decode(data, size, insn, c.reference_status);
        if (c.reference_ok) {
            c.reference_length = insn.length;
        }
        if (c.ours.status == DecodeStatus::Unknown) {
            return c;
        }
        if (c.ours.status == DecodeStatus::Truncated) {
            if (c.reference_ok) {
                c.verdict = Verdict::Mismatch;
                c.why = "the decoder says Truncated, the reference decodes an instruction inside the bytes";
            }
            return c;
        }
        const std::size_t lead = lead_offset(data, c.ours.length);
        c.vector_encoded = lead < c.ours.length && is_vector_lead(data[lead]);
        if (c.reference_ok) {
            c.why = properties_differ(c.ours, insn);
            c.verdict = c.why == nullptr ? Verdict::Agree : Verdict::Mismatch;
            if (c.why == nullptr) {
                c.why = "";
            }
            return c;
        }
        if (!c.vector_encoded) {
            c.verdict = Verdict::Mismatch;
            c.why = "the reference rejects a legacy-encoded instruction the decoder accepts";
            return c;
        }
        switch (sibling_check(data, c.ours.length, lead)) {
            case Sibling::Agrees:
                c.verdict = Verdict::Exception;
                c.why = "VEX/EVEX form the reference rejects; a sibling encoding of the same opcode agrees";
                break;
            case Sibling::Disagrees:
                c.verdict = Verdict::Mismatch;
                c.why = "the reference rejects the encoding and a sibling encoding of the same opcode disagrees";
                break;
            case Sibling::None:
                c.verdict = Verdict::Mismatch;
                c.why = "the reference knows no instruction with this VEX/EVEX map, pp and opcode";
                break;
        }
        return c;
    }

private:
    enum class Sibling : std::uint8_t { Agrees, Disagrees, None };

    static bool is_vector_lead(std::uint8_t b) noexcept { return b == 0xC4 || b == 0xC5 || b == 0x62; }

    // Index of the first byte that is not a segment-override or address-size prefix (the only prefixes the decoder
    // accepts in front of a VEX / EVEX prefix).
    static std::size_t lead_offset(const std::uint8_t* data, std::size_t length) noexcept {
        std::size_t i = 0;
        while (i < length) {
            const std::uint8_t b = data[i];
            if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65 || b == 0x67) {
                ++i;
            } else {
                break;
            }
        }
        return i;
    }

    // nullptr when everything the decoder reports matches what Zydis decoded; otherwise what differs.
    static const char* properties_differ(const cml::agent::hook::DecodeResult& ours, const ZydisDecodedInstruction& insn) noexcept {
        using namespace cml::agent::hook;
        if (insn.length != ours.length) {
            return "length";
        }
        const bool has_modrm = (insn.attributes & ZYDIS_ATTRIB_HAS_MODRM) != 0;
        // Zydis reports the moffs operand of A0..A3 as a displacement too; the decoder does not cover those opcodes.
        const unsigned reference_disp_size = has_modrm ? insn.raw.disp.size / 8u : 0u;
        if (reference_disp_size != ours.disp_size) {
            return "displacement size";
        }
        if (reference_disp_size != 0 && insn.raw.disp.offset != ours.disp_offset) {
            return "displacement offset";
        }
        const bool rip_relative = has_modrm && insn.raw.modrm.mod == 0 && insn.raw.modrm.rm == 5;
        if (rip_relative != ((ours.flags & kInsnRipRel) != 0)) {
            return "RIP-relative flag";
        }
        bool has_67 = false;
        bool has_fs_gs = false;
        for (unsigned i = 0; i < insn.raw.prefix_count; ++i) {
            const std::uint8_t value = insn.raw.prefixes[i].value;
            has_67 = has_67 || value == 0x67;
            has_fs_gs = has_fs_gs || value == 0x64 || value == 0x65;
        }
        if (has_67 != ((ours.flags & kInsnAddr32) != 0)) {
            return "address-size flag";
        }
        if (has_fs_gs != ((ours.flags & kInsnSegFsGs) != 0)) {
            return "fs/gs flag";
        }
        if ((insn.attributes & ZYDIS_ATTRIB_HAS_ADDRESSSIZE) != 0 && !has_67) {
            return "address-size attribute without a 67 prefix";
        }
        if ((insn.attributes & (ZYDIS_ATTRIB_HAS_SEGMENT_FS | ZYDIS_ATTRIB_HAS_SEGMENT_GS)) != 0 && !has_fs_gs) {
            return "fs/gs attribute without the prefix";
        }
        return nullptr;
    }

    // Tries the siblings of the VEX / EVEX instruction data[0..length) whose lead byte is at `lead`.
    Sibling sibling_check(const std::uint8_t* data, std::size_t length, std::size_t lead) const noexcept {
        using namespace cml::agent::hook;
        std::uint8_t head[16] = {};
        const std::uint8_t lead_byte = data[lead];
        const std::size_t payload = lead_byte == 0xC5 ? 1u : lead_byte == 0xC4 ? 2u : 3u;
        const std::size_t opcode_at = lead + 1 + payload;
        if (opcode_at + 1 > length || opcode_at + 1 > sizeof(head)) {
            return Sibling::None;
        }
        std::memcpy(head, data, opcode_at + 1);
        const std::uint8_t original_modrm = opcode_at + 1 < length ? data[opcode_at + 1] : std::uint8_t{0xC0};

        const unsigned vector_lengths = lead_byte == 0x62 ? 3u : 2u;
        const unsigned masks = lead_byte == 0x62 ? 2u : 1u;
        bool found = false;
        for (unsigned w = 0; w < 2; ++w) {
            for (unsigned l = 0; l < vector_lengths; ++l) {
                for (unsigned mask = 0; mask < masks; ++mask) {
                    // Canonical payload: no register extension, vvvv = 1111, no zeroing, no broadcast.
                    if (lead_byte == 0xC5) {
                        if (w != 0) {
                            continue;
                        }
                        head[lead + 1] = static_cast<std::uint8_t>(0xF8u | (l << 2) | (data[lead + 1] & 3u));
                    } else if (lead_byte == 0xC4) {
                        head[lead + 1] = static_cast<std::uint8_t>(0xE0u | (data[lead + 1] & 0x1Fu));
                        head[lead + 2] = static_cast<std::uint8_t>((w << 7) | 0x78u | (l << 2) | (data[lead + 2] & 3u));
                    } else {
                        head[lead + 1] = static_cast<std::uint8_t>(0xF0u | (data[lead + 1] & 0x0Fu));
                        head[lead + 2] = static_cast<std::uint8_t>((w << 7) | 0x7Cu | (data[lead + 2] & 3u));
                        head[lead + 3] = static_cast<std::uint8_t>((l << 5) | 0x08u | mask);
                    }
                    // Operand forms: as it is, a register, [rax], [rax + index] (SIB; the gathers need it).
                    for (unsigned form = 0; form < 4; ++form) {
                        for (unsigned reg = 0; reg < 9; ++reg) {
                            std::uint8_t buffer[32];
                            std::memset(buffer, 0x11, sizeof(buffer));
                            std::memcpy(buffer, head, opcode_at + 1);
                            std::size_t at = opcode_at + 1;
                            const unsigned reg_bits = reg == 8 ? (original_modrm >> 3) & 7u : reg;
                            if (form == 0) {
                                if (reg != 8) {
                                    continue;  // the original ModRM byte and what followed it, once
                                }
                                for (std::size_t k = at; k < length && k < sizeof(buffer); ++k) {
                                    buffer[k] = data[k];
                                }
                            } else if (form == 1) {
                                buffer[at++] = static_cast<std::uint8_t>(0xC0u | (reg_bits << 3) | 1u);
                            } else if (form == 2) {
                                buffer[at++] = static_cast<std::uint8_t>(reg_bits << 3);
                            } else {
                                buffer[at++] = static_cast<std::uint8_t>((reg_bits << 3) | 4u);
                                buffer[at++] = 0x08;  // base rax, index rcx / xmm1, scale 1
                            }
                            ZydisDecodedInstruction insn;
                            unsigned status = 0;
                            if (!decode(buffer, sizeof(buffer), insn, status)) {
                                continue;
                            }
                            found = true;
                            const DecodeResult ours = decode_instruction(buffer, sizeof(buffer));
                            if (ours.status != DecodeStatus::Ok || properties_differ(ours, insn) != nullptr) {
                                return Sibling::Disagrees;
                            }
                        }
                    }
                }
            }
        }
        return found ? Sibling::Agrees : Sibling::None;
    }

    ZydisDecoder decoder_{};
};

}  // namespace cml::fuzz::lde
