#include "cml/core/imgrel_site.h"

#include <algorithm>

#include "cml/util/format.h"
// The agent's instruction-length decoder, compiled into the broker unchanged (ADR-0042 decision 3). The include
// directory is given to this one source file by broker/cml_core/CMakeLists.txt.
#include "hook/length_decoder.h"

namespace cml::core::bind {

namespace lde = cml::agent::hook;

namespace {

constexpr std::uint32_t kMaxInstruction = 15;

constexpr bool is_legacy_prefix(std::uint8_t b) noexcept {
    switch (b) {
        case 0x26: case 0x2E: case 0x36: case 0x3E: case 0x64: case 0x65: case 0x66: case 0x67: case 0xF0: case 0xF2: case 0xF3:
            return true;
        default:
            return false;
    }
}

// One-byte opcodes WITHOUT a ModRM byte that carry a 32-bit (or, for B8..BF with REX.W, 64-bit) immediate: the only
// instructions without ModRM whose immediate a 4- or 8-byte relocation can fill. Every other opcode is parsed as if it
// had a ModRM byte; when it has none, either the displacement derived from the pretended ModRM disagrees with the
// decoder (layout unknown) or what is left behind it is too short for a relocation.
constexpr bool is_immediate_without_modrm(std::uint8_t opcode) noexcept {
    switch (opcode) {
        case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D:  // add/or/adc/sbb/and/sub/xor/cmp eAX, imm32
        case 0x68:  // push imm32
        case 0xA9:  // test eAX, imm32
        case 0xE8: case 0xE9:  // call rel32, jmp rel32
            return true;
        default:
            return opcode >= 0xB8 && opcode <= 0xBF;  // mov r, imm32 / imm64
    }
}

}  // namespace

InsnLayout parse_layout(const std::uint8_t* insn, std::size_t length, std::uint8_t disp_offset, std::uint8_t disp_size) noexcept {
    InsnLayout out;
    if (insn == nullptr || length == 0 || length > kMaxInstruction) {
        return out;
    }
    // Prefixes, as the decoder reads them: a REX prefix only counts when it is the last one. (WP-40, R-50: what was seen at
    // all is kept as well; a VEX/EVEX prefix after any REX or after 66/F2/F3/F0 is invalid, wherever they stood.)
    std::size_t i = 0;
    std::uint8_t rex = 0;
    bool rex_seen = false;
    bool simd_prefix_seen = false;
    for (; i < length; ++i) {
        if (is_legacy_prefix(insn[i])) {
            rex = 0;
            simd_prefix_seen = simd_prefix_seen || insn[i] == 0x66 || insn[i] == 0xF2 || insn[i] == 0xF3 || insn[i] == 0xF0;
        } else if ((insn[i] & 0xF0) == 0x40) {
            rex = insn[i];
            rex_seen = true;
        } else {
            break;
        }
    }
    if (i >= length) {
        return out;
    }
    std::size_t modrm_at = 0;
    bool expect_modrm = true;
    const std::uint8_t first = insn[i];
    if (first == 0xC4 || first == 0xC5 || first == 0x62) {
        // VEX (C5 + 1 byte, C4 + 2 bytes) or EVEX (62 + 3 bytes), then the opcode, then ModRM. In 64-bit mode these bytes
        // are never LES/LDS/BOUND. A REX prefix before them is invalid, and so is 66, F2, F3 or F0.
        if (rex_seen || simd_prefix_seen) {
            return out;
        }
        out.vex = true;
        modrm_at = i + (first == 0xC5 ? 2u : (first == 0xC4 ? 3u : 4u)) + 1u;
    } else {
        out.rex_w = (rex & 0x08) != 0;
        std::size_t opcode_at = i;
        bool one_byte_map = true;
        if (first == 0x0F) {
            one_byte_map = false;
            ++opcode_at;
            if (opcode_at >= length) {
                return out;
            }
            if (insn[opcode_at] == 0x38 || insn[opcode_at] == 0x3A) {
                ++opcode_at;
                if (opcode_at >= length) {
                    return out;
                }
            }
        }
        const std::uint8_t opcode = insn[opcode_at];
        const bool two_byte_map = !one_byte_map && opcode_at == i + 1;
        out.lea = one_byte_map && opcode == 0x8D;
        if ((one_byte_map && is_immediate_without_modrm(opcode)) || (two_byte_map && opcode >= 0x80 && opcode <= 0x8F)) {  // 0F 80..8F: jcc rel32
            expect_modrm = false;
            if (disp_size != 0) {
                return out;  // the decoder found a displacement where there is no ModRM: disagreement
            }
            out.immediate_start = static_cast<std::uint8_t>(opcode_at + 1);
            out.known = true;
            return out;
        }
        modrm_at = opcode_at + 1;
    }
    // ModRM, SIB and the displacement they dictate (64-bit addressing; 67 selects the same layout).
    std::size_t my_disp_offset = 0;
    std::size_t my_disp_size = 0;
    std::size_t behind = modrm_at;  // first byte behind ModRM / SIB / displacement
    if (expect_modrm && modrm_at < length) {
        out.has_modrm = true;
        const std::uint8_t modrm = insn[modrm_at];
        const unsigned mod = modrm >> 6;
        const unsigned rm = modrm & 7u;
        behind = modrm_at + 1;
        if (mod != 3) {
            unsigned sib_base = 0;
            if (rm == 4) {
                if (behind >= length) {
                    return out;
                }
                sib_base = insn[behind] & 7u;
                ++behind;
            }
            if (mod == 0) {
                if (rm == 5) {
                    my_disp_size = 4;  // RIP-relative
                } else if (rm == 4 && sib_base == 5) {
                    my_disp_size = 4;  // no base register
                    out.no_base = true;
                }
            } else {
                my_disp_size = mod == 1 ? 1u : 4u;
            }
        }
        if (my_disp_size != 0) {
            my_disp_offset = behind;
            behind += my_disp_size;
        }
    }
    if (behind > length || my_disp_size != disp_size || my_disp_offset != disp_offset) {
        return out;  // not what the decoder saw: nothing in this instruction is located
    }
    out.immediate_start = static_cast<std::uint8_t>(out.vex ? length : behind);
    out.known = true;
    return out;
}

const char* to_string(SiteVerdict v) noexcept {
    switch (v) {
        case SiteVerdict::Displacement: return "displacement";
        case SiteVerdict::NotCode: return "not-code";
        case SiteVerdict::OutOfSection: return "out-of-section";
        case SiteVerdict::NoFunction: return "no-function";
        case SiteVerdict::Undecodable: return "undecodable";
        case SiteVerdict::RanIntoData: return "ran-into-data";
        case SiteVerdict::NotDisplacement: return "not-displacement";
        case SiteVerdict::RipRelative: return "rip-relative";
        case SiteVerdict::Address32: return "address-size-32";
        case SiteVerdict::SegmentFsGs: return "segment-fs-gs";
        case SiteVerdict::NoBaseRegister: return "no-base-register";
        case SiteVerdict::LeaTruncates: return "lea-truncates";
    }
    return "?";
}

SiteProver::SiteProver(bool is_code, std::span<const std::byte> data, std::vector<RelocField> fields, std::vector<std::uint32_t> function_starts)
    : is_code_(is_code), data_(data), fields_(std::move(fields)), starts_(std::move(function_starts)) {
    std::stable_sort(fields_.begin(), fields_.end(), [](const RelocField& a, const RelocField& b) { return a.offset < b.offset; });
    std::sort(starts_.begin(), starts_.end());
    starts_.erase(std::unique(starts_.begin(), starts_.end()), starts_.end());
}

std::string SiteProver::hex_bytes(std::uint32_t offset, std::uint32_t count) const {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    for (std::uint32_t i = 0; i < count && static_cast<std::uint64_t>(offset) + i < data_.size(); ++i) {
        const std::uint8_t b = std::to_integer<std::uint8_t>(data_[offset + i]);
        if (!out.empty()) {
            out.push_back(' ');
        }
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0xF]);
    }
    return out;
}

const SiteProver::Sweep& SiteProver::sweep_from(std::uint32_t start) {
    const auto found = sweeps_.find(start);
    if (found != sweeps_.end()) {
        return found->second;
    }
    Sweep s;
    // The function ends where the next one starts (or with the section).
    std::uint64_t limit = data_.size();
    const auto next = std::upper_bound(starts_.begin(), starts_.end(), start);
    if (next != starts_.end()) {
        limit = std::min<std::uint64_t>(limit, *next);
    }
    const std::uint8_t* const base = reinterpret_cast<const std::uint8_t*>(data_.data());
    std::size_t fi = static_cast<std::size_t>(
        std::lower_bound(fields_.begin(), fields_.end(), start, [](const RelocField& f, std::uint32_t v) { return f.offset < v; }) - fields_.begin());
    std::uint64_t off = start;
    bool stopped = false;
    // A relocation that begins before the function and reaches into it: the function does not start on a boundary. (WP-40,
    // R-50: every relocation that starts less than 8 bytes, the widest field, before the start is looked at, not only the
    // last one; one of a type whose width is not known counts as reaching in, as it stops the sweep inside a function.)
    for (std::size_t k = fi; !stopped && k > 0 && static_cast<std::uint64_t>(fields_[k - 1].offset) + 8 > start; --k) {
        const RelocField& f = fields_[k - 1];
        if (f.width == 0 || static_cast<std::uint64_t>(f.offset) + f.width > start) {
            s.stop = SiteVerdict::RanIntoData;
            s.stop_reason = "the relocation at +" + hex(f.offset) + (f.width == 0 ? " (of a type whose width is not known)" : std::string()) +
                            " reaches into the function that starts at +" + hex(start);
            stopped = true;
        }
    }
    while (!stopped && off < limit) {
        const lde::DecodeResult r = lde::decode_instruction(base + off, static_cast<std::size_t>(limit - off));
        if (r.status != lde::DecodeStatus::Ok) {
            s.stop = SiteVerdict::Undecodable;
            const std::string bytes = hex_bytes(static_cast<std::uint32_t>(off), static_cast<std::uint32_t>(std::min<std::uint64_t>(kMaxInstruction, limit - off)));
            s.stop_reason = r.status == lde::DecodeStatus::Truncated
                                ? "the instruction at +" + hex(off) + " (" + bytes + ") runs past the end of its function"
                                : "the instruction at +" + hex(off) + " (" + bytes + ") is not known to the instruction decoder, so nothing behind it can be proved";
            stopped = true;
            break;
        }
        const std::uint64_t end = off + r.length;
        const std::uint64_t disp_lo = off + r.disp_offset;
        // Where the operands are, derived from the bytes and checked against the decoder (see parse_layout).
        const InsnLayout layout = parse_layout(base + off, r.length, r.disp_offset, r.disp_size);
        const std::uint64_t immediate_lo = off + layout.immediate_start;
        for (; fi < fields_.size() && fields_[fi].offset < end; ++fi) {
            const RelocField& f = fields_[fi];
            const std::uint64_t f_end = static_cast<std::uint64_t>(f.offset) + f.width;
            // Exactly the 32-bit displacement, or wholly inside the immediate (legacy encodings; a relocated immediate is
            // 4 or 8 bytes and is the last thing in the instruction).
            const bool is_displacement = layout.known && r.disp_size == 4 && f.width == 4 && f.offset == disp_lo;
            const bool in_immediate = layout.known && !layout.vex && (f.width == 4 || f.width == 8) && f.offset >= immediate_lo && f_end == end;
            if (!is_displacement && !in_immediate) {
                s.stop = SiteVerdict::RanIntoData;
                s.stop_length = r.length;
                s.stop_reason = "the relocation at +" + hex(f.offset) + " does not fit an operand of the instruction decoded at +" + hex(off) + " (" +
                                hex_bytes(static_cast<std::uint32_t>(off), r.length) +
                                "): the bytes there are data (a jump table) or the instruction boundaries are lost";
                stopped = true;
                break;
            }
        }
        if (stopped) {
            break;
        }
        Insn insn;
        insn.start = static_cast<std::uint32_t>(off);
        insn.length = r.length;
        insn.flags = r.flags;
        insn.disp_offset = r.disp_offset;
        insn.disp_size = r.disp_size;
        insn.layout = layout;
        s.insns.push_back(insn);
        off = end;
    }
    s.end = static_cast<std::uint32_t>(off);
    s.complete = !stopped;
    return sweeps_.emplace(start, std::move(s)).first->second;
}

SiteCheck SiteProver::check(std::uint32_t site) {
    SiteCheck c;
    c.at = site;
    if (!is_code_) {
        c.verdict = SiteVerdict::NotCode;
        c.bytes = hex_bytes(site, 4);
        c.reason = "the reference is in a data section, not in an instruction (bytes at the site: " + c.bytes + ")";
        return c;
    }
    if (static_cast<std::uint64_t>(site) + 4 > data_.size()) {
        c.verdict = SiteVerdict::OutOfSection;
        c.reason = "the site at +" + hex(site) + " is not inside the section data";
        return c;
    }
    const auto after = std::upper_bound(starts_.begin(), starts_.end(), site);
    if (after == starts_.begin()) {
        c.verdict = SiteVerdict::NoFunction;
        c.bytes = hex_bytes(site, 4);
        c.reason = "no function symbol precedes the site at +" + hex(site) + " in its section, so there is no instruction boundary to decode from";
        return c;
    }
    c.function_start = *(after - 1);
    const Sweep& s = sweep_from(c.function_start);
    if (site >= s.end) {
        // The sweep never got as far as the site's instruction.
        c.at = s.end;
        c.length = s.stop_length;
        c.bytes = hex_bytes(s.end, s.stop_length != 0 ? s.stop_length : kMaxInstruction);
        if (s.complete) {
            c.verdict = SiteVerdict::Undecodable;  // cannot happen: the site lies before the next function start
            c.reason = "the site at +" + hex(site) + " lies behind the decoded instructions of its function";
        } else {
            c.verdict = s.stop;
            c.reason = s.stop_reason;
        }
        return c;
    }
    // The instruction that holds the site: the last one that starts at or before it (the trusted ones are contiguous).
    auto it = std::upper_bound(s.insns.begin(), s.insns.end(), site, [](std::uint32_t v, const Insn& i) { return v < i.start; });
    const Insn& insn = *(it - 1);
    c.at = insn.start;
    c.length = insn.length;
    c.bytes = hex_bytes(insn.start, insn.length);
    const std::string where = "the instruction at +" + hex(insn.start) + " (" + c.bytes + ")";
    if (insn.disp_size != 4 || insn.start + insn.disp_offset != site) {
        c.verdict = SiteVerdict::NotDisplacement;
        const bool behind = insn.disp_size == 0 || site >= static_cast<std::uint32_t>(insn.start + insn.disp_offset + insn.disp_size);
        c.reason = "the site at +" + hex(site) + " is not the 32-bit displacement of a memory operand of " + where +
                   (behind ? ": it is an immediate operand" : "");
        return c;
    }
    if ((insn.flags & lde::kInsnRipRel) != 0) {
        c.verdict = SiteVerdict::RipRelative;
        c.reason = where + " addresses it RIP-relatively";
        return c;
    }
    if ((insn.flags & lde::kInsnAddr32) != 0) {
        c.verdict = SiteVerdict::Address32;
        c.reason = where + " has a 67 prefix: its address is truncated to 32 bits";
        return c;
    }
    if ((insn.flags & lde::kInsnSegFsGs) != 0) {
        c.verdict = SiteVerdict::SegmentFsGs;
        c.reason = where + " has an fs/gs segment override: its address is relative to that segment";
        return c;
    }
    if (!insn.layout.known) {
        // Cannot happen: the site's own relocation was accepted as this instruction's displacement, which needs the layout.
        c.verdict = SiteVerdict::RanIntoData;
        c.reason = "the operands of " + where + " could not be located";
        return c;
    }
    if (insn.layout.no_base) {
        c.verdict = SiteVerdict::NoBaseRegister;
        c.reason = where + " has no base register that could hold the image base";
        return c;
    }
    if (insn.layout.lea && !insn.layout.rex_w) {
        c.verdict = SiteVerdict::LeaTruncates;
        c.reason = where + " is a lea with a 32-bit or 16-bit destination: it computes a 32-bit image-relative value, which retargeting would change";
        return c;
    }
    c.verdict = SiteVerdict::Displacement;
    c.reason = "the 32-bit displacement of " + where;
    return c;
}

}  // namespace cml::core::bind
