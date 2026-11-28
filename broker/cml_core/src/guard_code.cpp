#include "cml/core/guard_code.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "cml/core/imgrel_site.h"
#include "cml/util/format.h"
// The agent's instruction-length decoder, compiled into the broker unchanged (ADR-0042 decision 3, as imgrel_site.cpp).
#include "hook/length_decoder.h"

namespace cml::core::bind {

namespace lde = cml::agent::hook;

namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;

// The operands of one legacy-encoded instruction, as far as this reader needs them.
struct Ops {
    bool ok = false;            // parsed (legacy encoding, layout checked against the decoder)
    bool p66 = false;           // operand-size prefix
    bool other_prefix = false;  // F0, F2, F3, a segment override or 67
    std::uint8_t rex = 0;
    bool map0f = false;
    std::uint8_t op = 0;
    bool has_modrm = false;
    std::uint8_t mod = 0;
    std::uint8_t reg = 0;  // ModRM.reg with REX.R
    std::uint8_t rm = 0;   // ModRM.rm with REX.B
    bool riprel = false;
    std::uint32_t disp_at = 0;  // where a RIP-relative disp32 starts in the instruction
    std::uint32_t imm_len = 0;
    std::int64_t imm = 0;  // the immediate, sign-extended
};

std::int64_t read_signed(const std::uint8_t* p, std::uint32_t n) {
    std::uint64_t v = 0;
    for (std::uint32_t k = 0; k < n; ++k) {
        v |= static_cast<std::uint64_t>(p[k]) << (8 * k);
    }
    if (n < 8 && n > 0 && (v >> (8 * n - 1)) != 0) {
        v |= ~0ull << (8 * n);
    }
    return static_cast<std::int64_t>(v);
}

bool is_other_prefix(std::uint8_t b) {
    switch (b) {
        case 0x26: case 0x2E: case 0x36: case 0x3E: case 0x64: case 0x65: case 0x67: case 0xF0: case 0xF2: case 0xF3:
            return true;
        default:
            return false;
    }
}

Ops parse_ops(const std::uint8_t* p, std::uint32_t len, const lde::DecodeResult& r) {
    Ops o;
    // (parse_layout reads a ModRM for every opcode it does not know to have none, so its verdict is checked only for the
    // instructions read with a ModRM below.)
    const InsnLayout lay = parse_layout(p, len, r.disp_offset, r.disp_size);
    std::uint32_t i = 0;
    for (; i < len; ++i) {
        const std::uint8_t b = p[i];
        if (b == 0x66) {
            o.p66 = true;
            o.rex = 0;
        } else if (is_other_prefix(b)) {
            o.other_prefix = true;
            o.rex = 0;
        } else if ((b & 0xF0) == 0x40) {
            o.rex = b;
        } else {
            break;
        }
    }
    if (i >= len) {
        return o;
    }
    if (p[i] == 0x0F) {
        o.map0f = true;
        ++i;
        if (i >= len || p[i] == 0x38 || p[i] == 0x3A) {
            return o;
        }
    }
    o.op = p[i];
    // Opcodes without ModRM that this reader looks at: or/and/cmp/test al|eax, imm, nop and the direct branches; parse_layout
    // pretends a ModRM for some of them, so their immediate is taken from the end of the instruction instead.
    const bool no_modrm = !o.map0f && (o.op == 0x0C || o.op == 0x0D || o.op == 0x24 || o.op == 0x25 || o.op == 0x3C || o.op == 0x3D || o.op == 0xA8 ||
                                       o.op == 0xA9 || o.op == 0x90 || (o.op >= 0x70 && o.op <= 0x7F) || o.op == 0xEB || o.op == 0xE8 || o.op == 0xE9 ||
                                       (o.op >= 0xE0 && o.op <= 0xE3));
    const bool jcc32 = o.map0f && o.op >= 0x80 && o.op <= 0x8F;
    if (no_modrm || jcc32) {
        std::uint32_t n = 0;
        if (jcc32) {
            n = 4;
        } else if (o.op == 0x90) {
            n = 0;
        } else if (o.op == 0x0C || o.op == 0x24 || o.op == 0x3C || o.op == 0xA8 || (o.op >= 0x70 && o.op <= 0x7F) || o.op == 0xEB || (o.op >= 0xE0 && o.op <= 0xE3)) {
            n = 1;
        } else if (o.op == 0x0D || o.op == 0x25 || o.op == 0x3D || o.op == 0xA9) {
            n = o.p66 ? 2u : 4u;
        } else {
            n = 4;  // E8, E9
        }
        if (len != i + 1 + n) {
            return o;
        }
        o.imm_len = n;
        o.imm = n == 0 ? 0 : read_signed(p + len - n, n);
        o.ok = r.disp_size == 0;
        return o;
    }
    if (!lay.known || lay.vex) {
        return o;
    }
    o.has_modrm = lay.has_modrm;
    if (o.has_modrm) {
        if (i + 1 >= len) {
            return o;
        }
        const std::uint8_t m = p[i + 1];
        o.mod = static_cast<std::uint8_t>(m >> 6);
        o.reg = static_cast<std::uint8_t>(((m >> 3) & 7u) | ((o.rex & 0x04) != 0 ? 8u : 0u));
        o.rm = static_cast<std::uint8_t>((m & 7u) | ((o.rex & 0x01) != 0 ? 8u : 0u));
        o.riprel = o.mod == 0 && (m & 7u) == 5u;
        if (o.riprel) {
            o.disp_at = i + 2;
        }
    }
    if (lay.immediate_start < len) {
        o.imm_len = len - lay.immediate_start;
        if (o.imm_len == 1 || o.imm_len == 2 || o.imm_len == 4 || o.imm_len == 8) {
            o.imm = read_signed(p + lay.immediate_start, o.imm_len);
        }
    }
    o.ok = true;
    return o;
}


// What one use of the guard does.
enum class Use : std::uint8_t { None, Read, Claim, Release, Header, Footer, Abort };

struct Insn {
    std::uint32_t start = 0;
    std::uint8_t length = 0;
    std::uint8_t flags = 0;
    Ops ops;
    std::vector<std::uint32_t> relocs;  // indices into the section's sorted relocations
    Use use = Use::None;
    std::uint32_t bits = 0;               // Claim: the bit set
    std::int64_t guard_at = 0;            // a use of the guard: the offset into it
    std::uint32_t jump_to = kNone;        // a direct branch inside the section: its target
    std::uint32_t claim_reg = kNone;      // a claim stored from a register: that register (byte registers without REX | 0x100)
    std::uint32_t claim_width = 0;        // ... and the width of the store
};

std::string hex_bytes(std::span<const std::byte> data, std::uint32_t offset, std::uint32_t count) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    for (std::uint32_t i = 0; i < count && static_cast<std::uint64_t>(offset) + i < data.size(); ++i) {
        const std::uint8_t b = std::to_integer<std::uint8_t>(data[offset + i]);
        if (!out.empty()) {
            out.push_back(' ');
        }
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0xF]);
    }
    return out;
}

bool single_bit(std::uint64_t v) { return v != 0 && (v & (v - 1)) == 0 && v <= 0xFFFFFFFFull; }

// The register an `or` / `and` / `bts` / `btr` of an immediate writes, when that is all the instruction does (else kNone):
// `out_bits` = the immediate's bits (bts/btr: the one bit), `width` the operand width in bytes. bts/btr: only the 32-bit
// form with a bit below 32 (a 66 or REX.W form, or a larger bit number, is not read).
std::uint32_t reg_op_with_immediate(const Ops& o, bool want_or, std::uint64_t& out_bits, std::uint32_t& width) {
    if (!o.ok || o.other_prefix) {
        return kNone;
    }
    const bool rex_w = (o.rex & 0x08) != 0;
    width = rex_w ? 8u : (o.p66 ? 2u : 4u);
    const unsigned ext = want_or ? 1u : 4u;
    if (!o.map0f && (o.op == 0x81 || o.op == 0x83) && o.has_modrm && o.mod == 3 && (o.reg & 7u) == ext) {
        out_bits = static_cast<std::uint64_t>(o.imm) & (width == 8 ? ~0ull : ((1ull << (8 * width)) - 1));
        return o.rm;
    }
    if (!o.map0f && o.op == 0x80 && o.has_modrm && o.mod == 3 && (o.reg & 7u) == ext) {
        width = 1;
        out_bits = static_cast<std::uint64_t>(o.imm) & 0xFFu;
        return o.rex != 0 ? o.rm : static_cast<std::uint32_t>(o.rm | 0x100u);  // no REX: 4..7 are ah..bh (marked)
    }
    if (!o.map0f && o.op == (want_or ? 0x0D : 0x25) && !rex_w) {
        out_bits = static_cast<std::uint64_t>(o.imm) & ((1ull << (8 * width)) - 1);
        return 0;
    }
    if (!o.map0f && o.op == (want_or ? 0x0C : 0x24)) {
        width = 1;
        out_bits = static_cast<std::uint64_t>(o.imm) & 0xFFu;
        return o.rex != 0 ? 0u : 0x100u;
    }
    if (o.map0f && o.op == 0xBA && o.has_modrm && o.mod == 3 && (o.reg & 7u) == (want_or ? 5u : 6u)) {
        const std::uint64_t bit = static_cast<std::uint64_t>(o.imm) & 0xFFu;
        if (rex_w || o.p66 || bit >= 32) {
            return kNone;
        }
        out_bits = 1ull << bit;
        return o.rm;
    }
    return kNone;
}

// Does the instruction write the memory its ModRM operand names (or form its address, `lea`)? Legacy encodings; an
// instruction that is not parsed is not answered here (the caller refuses it).
bool writes_operand(const Ops& o) {
    if (!o.has_modrm) {
        return false;
    }
    const unsigned r = o.reg & 7u;
    if (!o.map0f) {
        switch (o.op) {
            case 0x00: case 0x01: case 0x08: case 0x09: case 0x10: case 0x11: case 0x18: case 0x19:
            case 0x20: case 0x21: case 0x28: case 0x29: case 0x30: case 0x31:
            case 0x86: case 0x87: case 0x88: case 0x89: case 0x8D: case 0x8F: case 0xC6: case 0xC7:
            case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3:
                return true;
            case 0x80: case 0x81: case 0x83:
                return r != 7;
            case 0xF6: case 0xF7:
                return r == 2 || r == 3;
            case 0xFE: case 0xFF:
                return r == 0 || r == 1;
            default:
                return false;
        }
    }
    if (o.op >= 0x90 && o.op <= 0x9F) {
        return true;  // setcc
    }
    switch (o.op) {
        case 0x11: case 0x13: case 0x17: case 0x29: case 0x2B: case 0x7F: case 0xD6: case 0xE7: case 0xC3:
        case 0xAB: case 0xB3: case 0xBB: case 0xB0: case 0xB1: case 0xC0: case 0xC1:
            return true;
        case 0x7E:
            return o.p66 && !o.other_prefix;  // movd r/m32, xmm (F3 0F 7E is a load)
        case 0xBA:
            return r >= 5;
        default:
            return false;
    }
}

// Does the instruction leave every general register as it was (the walk back from a bitmask claim's `or` to the load of
// the guard)? Only what both compilers put between them: test, cmp, bt, conditional and direct jumps, nop.
bool preserves_registers(const Insn& insn) {
    const Ops& o = insn.ops;
    if (!o.ok || o.other_prefix) {
        return false;
    }
    if (!o.map0f) {
        if (o.op == 0x84 || o.op == 0x85 || o.op == 0xA8 || o.op == 0xA9 || (o.op >= 0x38 && o.op <= 0x3D) || (o.op >= 0x70 && o.op <= 0x7F)) {
            return true;
        }
        if ((o.op == 0xF6 || o.op == 0xF7) && (o.reg & 7u) == 0u) {
            return true;
        }
        if ((o.op == 0x80 || o.op == 0x81 || o.op == 0x83) && (o.reg & 7u) == 7u) {
            return true;
        }
        if ((o.op == 0xEB || o.op == 0xE9) && insn.relocs.empty()) {
            return true;
        }
        return o.op == 0x90 && o.rex == 0;
    }
    return (o.op >= 0x80 && o.op <= 0x8F) || o.op == 0xA3 || (o.op == 0xBA && (o.reg & 7u) == 4u) || o.op == 0x1F;
}

class Reader {
public:
    Reader(std::string_view guard, std::int32_t guard_section, GuardCodeKind kind, const std::map<std::string, std::int32_t, std::less<>>& candidates)
        : guard_(guard), guard_section_(guard_section), kind_(kind), candidates_(candidates) {
        for (const auto& [name, sec] : candidates_) {
            candidate_sections_.insert(sec);
        }
    }

    bool section(const GuardCodeSection& cs, GuardCodeReading& out);

private:
    std::string_view guard_;
    std::int32_t guard_section_;
    GuardCodeKind kind_;
    const std::map<std::string, std::int32_t, std::less<>>& candidates_;
    std::set<std::int32_t> candidate_sections_;

    bool fail(GuardCodeReading& out, std::string why) {
        out.why = std::move(why);
        return false;
    }
};

bool Reader::section(const GuardCodeSection& cs, GuardCodeReading& out) {
    // Aliases: a relocation into the guard's section that does not name the guard (a section symbol, a label, another
    // name) hides a use of the guard, in any code section. Checked before anything is copied or sorted.
    bool refers = false;
    for (const GuardCodeReloc& rel : cs.relocs) {
        if (rel.target == guard_) {
            refers = true;
        } else if (guard_section_ > 0 && rel.target_section == guard_section_) {
            return fail(out, "the code at +" + hex(rel.offset) + " in " + cs.name + " refers to the guard's section through another symbol, '" + rel.target + "'");
        }
    }
    if (!refers) {
        return true;
    }
    // In a section that uses the guard, the same for the candidates' sections: a write through an alias would not be seen.
    for (const GuardCodeReloc& rel : cs.relocs) {
        if (rel.target_section > 0 && candidate_sections_.count(rel.target_section) != 0) {
            const auto c = candidates_.find(rel.target);
            if (c == candidates_.end() || c->second != rel.target_section) {
                return fail(out, "the code at +" + hex(rel.offset) + " in " + cs.name + " refers to the section of one of the guard's statics through another symbol, '" +
                                     rel.target + "'");
            }
        }
    }
    std::vector<GuardCodeReloc> relocs = cs.relocs;
    std::stable_sort(relocs.begin(), relocs.end(), [](const GuardCodeReloc& a, const GuardCodeReloc& b) { return a.offset < b.offset; });
    std::vector<std::uint32_t> starts = cs.function_starts;
    starts.push_back(0);
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    const std::uint8_t* const base = reinterpret_cast<const std::uint8_t*>(cs.data.data());
    const std::uint64_t size = cs.data.size();
    std::vector<Insn> code;                 // every instruction of the section, in order
    std::vector<std::uint32_t> entries;     // the indices of the function starts
    std::vector<bool> consumed(relocs.size(), false);
    // The sweep (as WP-36's SiteProver): linear, from every start to the next; every relocation it passes must be exactly
    // the 32-bit displacement of one decoded instruction or lie wholly inside its immediate. Every function of a section
    // that uses the guard must decode completely: a region, a write or an address behind a stop would not be seen.
    for (std::size_t f = 0; f < starts.size(); ++f) {
        if (starts[f] >= size) {
            break;
        }
        entries.push_back(static_cast<std::uint32_t>(code.size()));
        const std::uint64_t limit = f + 1 < starts.size() ? std::min<std::uint64_t>(starts[f + 1], size) : size;
        std::size_t ri = static_cast<std::size_t>(
            std::lower_bound(relocs.begin(), relocs.end(), starts[f], [](const GuardCodeReloc& r, std::uint32_t v) { return r.offset < v; }) - relocs.begin());
        std::uint64_t off = starts[f];
        while (off < limit) {
            const lde::DecodeResult r = lde::decode_instruction(base + off, static_cast<std::size_t>(limit - off));
            if (r.status != lde::DecodeStatus::Ok) {
                return fail(out, "the function at +" + hex(starts[f]) + " in " + cs.name + " does not decode completely: the instruction at +" + hex(off) + " (" +
                                     hex_bytes(cs.data, static_cast<std::uint32_t>(off), 15) + ") is not known to the decoder");
            }
            const std::uint64_t end = off + r.length;
            const InsnLayout layout = parse_layout(base + off, r.length, r.disp_offset, r.disp_size);
            Insn insn;
            insn.start = static_cast<std::uint32_t>(off);
            insn.length = r.length;
            insn.flags = r.flags;
            insn.ops = parse_ops(base + off, r.length, r);
            for (; ri < relocs.size() && relocs[ri].offset < end; ++ri) {
                const GuardCodeReloc& rel = relocs[ri];
                const std::uint64_t r_end = static_cast<std::uint64_t>(rel.offset) + rel.width;
                const bool is_disp = layout.known && r.disp_size == 4 && rel.width == 4 && rel.offset == off + r.disp_offset;
                const bool in_imm = layout.known && !layout.vex && (rel.width == 4 || rel.width == 8) && rel.offset >= off + layout.immediate_start && r_end == end;
                if (rel.offset < off || (!is_disp && !in_imm)) {
                    return fail(out, "the function at +" + hex(starts[f]) + " in " + cs.name + " does not decode completely: the relocation at +" + hex(rel.offset) +
                                         " does not fit an operand of the instruction at +" + hex(off) + " (" + hex_bytes(cs.data, static_cast<std::uint32_t>(off), r.length) +
                                         ")");
                }
                insn.relocs.push_back(static_cast<std::uint32_t>(ri));
                consumed[ri] = true;
            }
            const Ops& o = insn.ops;
            const bool direct = o.ok && ((!o.map0f && ((o.op >= 0x70 && o.op <= 0x7F) || o.op == 0xEB || o.op == 0xE9 || (o.op >= 0xE0 && o.op <= 0xE3))) ||
                                         (o.map0f && o.op >= 0x80 && o.op <= 0x8F));
            if (direct && insn.relocs.empty()) {
                const std::int64_t to = static_cast<std::int64_t>(end) + o.imm;
                if (to < 0 || static_cast<std::uint64_t>(to) >= size) {
                    return fail(out, "the branch at +" + hex(off) + " in " + cs.name + " leaves its section");
                }
                insn.jump_to = static_cast<std::uint32_t>(to);
            }
            code.push_back(std::move(insn));
            off = end;
        }
    }
    for (std::size_t k = 0; k < relocs.size(); ++k) {
        if (!consumed[k] && (relocs[k].target == guard_ || candidates_.count(relocs[k].target) != 0)) {
            return fail(out, "the reference to '" + relocs[k].target + "' at +" + hex(relocs[k].offset) + " in " + cs.name + " is not in a decoded instruction");
        }
    }
    const auto where = [&](std::uint32_t k) { return "+" + hex(code[k].start) + " (" + hex_bytes(cs.data, code[k].start, code[k].length) + ") in " + cs.name; };
    const auto index_of = [&](std::uint32_t offset) -> std::uint32_t {
        const auto it = std::lower_bound(code.begin(), code.end(), offset, [](const Insn& i, std::uint32_t v) { return i.start < v; });
        return it != code.end() && it->start == offset ? static_cast<std::uint32_t>(it - code.begin()) : kNone;
    };
    // The control flow: successors of every instruction (calls return; a jump with a relocation is a tail call).
    std::vector<std::vector<std::uint32_t>> succ(code.size());
    std::vector<std::vector<std::uint32_t>> pred(code.size());
    for (std::uint32_t k = 0; k < code.size(); ++k) {
        const Insn& insn = code[k];
        const bool is_jmp = (insn.flags & lde::kInsnJmp) != 0;
        const bool is_jcc = (insn.flags & lde::kInsnJcc) != 0;
        if ((is_jmp || is_jcc) && insn.jump_to == kNone && insn.relocs.empty()) {
            return fail(out, "the code at " + where(k) + " jumps to an address the reader does not know (an indirect jump)");
        }
        if (insn.jump_to != kNone) {
            const std::uint32_t t = index_of(insn.jump_to);
            if (t == kNone) {
                return fail(out, "the branch at " + where(k) + " lands inside an instruction");
            }
            succ[k].push_back(t);
        }
        if ((insn.flags & (lde::kInsnRet | lde::kInsnTrap)) == 0 && !is_jmp && k + 1 < code.size()) {
            succ[k].push_back(k + 1);
        }
        for (const std::uint32_t s : succ[k]) {
            pred[s].push_back(k);
        }
    }
    // Every use of the guard, classified; every candidate whose address the code forms.
    const auto field_offset = [&](const GuardCodeReloc& rel, const Insn& insn) -> std::int64_t {
        // The address the instruction forms is S + A + (bytes behind the field) - k for REL32_k (k = 0 for REL32): the
        // offset into the target. clang writes REL32 with A = offset - tail, cl REL32_k with A = offset.
        std::int32_t a = 0;
        std::memcpy(&a, base + rel.offset, 4);
        const std::int64_t tail = static_cast<std::int64_t>(insn.start) + insn.length - (static_cast<std::int64_t>(rel.offset) + 4);
        return static_cast<std::int64_t>(a) + tail - rel.rel_tail;
    };
    const auto memory_operand = [&](const Insn& insn, const GuardCodeReloc& rel) {
        const Ops& o = insn.ops;
        return o.ok && o.riprel && (insn.flags & lde::kInsnRipRel) != 0 && rel.offset == insn.start + o.disp_at;
    };
    for (std::uint32_t i = 0; i < code.size(); ++i) {
        Insn& insn = code[i];
        const GuardCodeReloc* g = nullptr;
        for (const std::uint32_t k : insn.relocs) {
            if (candidates_.count(relocs[k].target) != 0 && (!memory_operand(insn, relocs[k]) || (!insn.ops.map0f && insn.ops.op == 0x8D))) {
                out.address_taken.insert(relocs[k].target);
            }
            if (relocs[k].target == guard_) {
                if (g != nullptr) {
                    return fail(out, "the instruction at " + where(i) + " refers to the guard twice");
                }
                g = &relocs[k];
            }
        }
        if (g == nullptr) {
            continue;
        }
        ++out.references;
        const Ops& o = insn.ops;
        if (o.other_prefix || !memory_operand(insn, *g) || g->rel_tail < 0) {
            return fail(out, "the guard is used at " + where(i) + " in a form that is not read (not a plain RIP-relative operand)");
        }
        const std::int64_t at = field_offset(*g, insn);
        insn.guard_at = at;
        const bool rex_w = (o.rex & 0x08) != 0;
        if (kind_ == GuardCodeKind::ThreadSafe) {
            if (at != 0) {
                return fail(out, "the guard is used at " + where(i) + " at offset " + std::to_string(at) + " into it");
            }
            if (!o.map0f && (o.op == 0x8B || o.op == 0x3B || o.op == 0x39 || ((o.op == 0x81 || o.op == 0x83) && (o.reg & 7u) == 7u))) {
                insn.use = Use::Read;
                continue;
            }
            if (!o.map0f && o.op == 0x8D && rex_w && o.reg == 1) {
                // lea rcx, [guard], then the call of the CRT's guard functions (their first argument).
                if (i + 1 >= code.size() || pred[i + 1].size() != 1 || pred[i + 1][0] != i) {
                    return fail(out, "the address of the guard is loaded at " + where(i) + " and not passed straight to a call");
                }
                const Insn& call = code[i + 1];
                std::string callee;
                for (const std::uint32_t k : call.relocs) {
                    callee = relocs[k].target;
                }
                const Ops& c = call.ops;
                const bool direct_call = c.ok && !c.map0f && c.op == 0xE8 && call.relocs.size() == 1;
                const bool import_call = c.ok && !c.map0f && c.op == 0xFF && c.has_modrm && (c.reg & 7u) == 2u && c.riprel && call.relocs.size() == 1;
                if (import_call && callee.compare(0, 6, "__imp_") == 0) {
                    callee = callee.substr(6);
                }
                if ((direct_call || import_call) && callee == "_Init_thread_header") {
                    insn.use = Use::Header;
                } else if ((direct_call || import_call) && callee == "_Init_thread_footer") {
                    insn.use = Use::Footer;
                } else if ((direct_call || import_call) && callee == "_Init_thread_abort") {
                    insn.use = Use::Abort;
                } else {
                    return fail(out, "the address of the guard is loaded at " + where(i) + " for " +
                                         (callee.empty() ? std::string("something that is not a call") : "a call of '" + callee + "'"));
                }
                continue;
            }
            return fail(out, "the guard is used at " + where(i) + " by an instruction that is not read");
        }
        // A bitmask.
        if (at < 0 || at > 3) {
            return fail(out, "the guard is used at " + where(i) + " at offset " + std::to_string(at) + " into it");
        }
        const bool read = (!o.map0f && (o.op == 0x8B || o.op == 0x8A || o.op == 0x3B || o.op == 0x39 || o.op == 0x3A || o.op == 0x38 || o.op == 0x85 || o.op == 0x84 ||
                                        ((o.op == 0xF6 || o.op == 0xF7) && (o.reg & 7u) == 0u) ||
                                        ((o.op == 0x80 || o.op == 0x81 || o.op == 0x83) && (o.reg & 7u) == 7u))) ||
                          (o.map0f && (o.op == 0xB6 || o.op == 0xB7 || o.op == 0xA3 || (o.op == 0xBA && (o.reg & 7u) == 4u)));
        if (read) {
            insn.use = Use::Read;
            continue;
        }
        const std::uint32_t mem_width = (!o.map0f && o.op == 0x80) ? 1u : (rex_w ? 8u : (o.p66 ? 2u : 4u));
        if (!o.map0f && (o.op == 0x80 || o.op == 0x81 || o.op == 0x83) && ((o.reg & 7u) == 1u || (o.reg & 7u) == 4u)) {
            if (at + mem_width > 4) {
                return fail(out, "the guard is written at " + where(i) + " beyond its four bytes");
            }
            const std::uint64_t imm = static_cast<std::uint64_t>(o.imm) & (mem_width == 8 ? ~0ull : ((1ull << (8 * mem_width)) - 1));
            if ((o.reg & 7u) == 4u) {
                insn.use = Use::Release;
                continue;
            }
            if (!single_bit(imm << (8 * at))) {
                return fail(out, "the store at " + where(i) + " sets " + hex(imm << (8 * at)) + " in the guard, not one bit");
            }
            insn.use = Use::Claim;  // or [guard], imm: memory destination, nothing to prove about a register
            insn.bits = static_cast<std::uint32_t>(imm << (8 * at));
            continue;
        }
        if (o.map0f && o.op == 0xBA && ((o.reg & 7u) == 5u || (o.reg & 7u) == 6u)) {
            const std::uint64_t bit = static_cast<std::uint64_t>(o.imm) & 0xFFu;
            if (at != 0 || rex_w || o.p66 || bit >= 32) {
                return fail(out, "the guard's bit is set or cleared at " + where(i) + " in a form that is not read (only the 32-bit bts/btr at its start, of a bit below 32)");
            }
            if ((o.reg & 7u) == 6u) {
                insn.use = Use::Release;
                continue;
            }
            insn.use = Use::Claim;
            insn.bits = 1u << bit;
            continue;
        }
        if (!o.map0f && (o.op == 0x89 || o.op == 0x88) && !rex_w) {
            // A register stored to the guard: what the instruction before it did to that register.
            const std::uint32_t width = o.op == 0x88 ? 1u : (o.p66 ? 2u : 4u);
            const std::uint32_t reg = o.op == 0x88 && o.rex == 0 ? static_cast<std::uint32_t>(o.reg | 0x100u) : o.reg;
            if (at + width > 4) {
                return fail(out, "the guard is written at " + where(i) + " beyond its four bytes");
            }
            if (i == 0 || pred[i].size() != 1 || pred[i][0] != i - 1) {
                return fail(out, "the store to the guard at " + where(i) + " can be reached other than from the instruction before it");
            }
            const Insn& prev = code[i - 1];
            std::uint64_t bits = 0;
            std::uint32_t pw = 0;
            if (reg_op_with_immediate(prev.ops, true, bits, pw) == reg && pw == width) {
                if (!single_bit(bits << (8 * at))) {
                    return fail(out, "the store at " + where(i) + " sets " + hex(bits << (8 * at)) + " in the guard, not one bit");
                }
                insn.use = Use::Claim;
                insn.bits = static_cast<std::uint32_t>(bits << (8 * at));
                insn.claim_reg = reg;
                insn.claim_width = width;
                continue;
            }
            if (reg_op_with_immediate(prev.ops, false, bits, pw) == reg && pw == width) {
                insn.use = Use::Release;
                continue;
            }
            return fail(out, "the value stored to the guard at " + where(i) + " is not an or (or and) of a constant by the instruction before it, " + where(i - 1));
        }
        return fail(out, "the guard is used at " + where(i) + " by an instruction that is not read");
    }
    // A claim stored from a register sets one bit in the guard's value only if the register holds that value: on every path
    // back from the `or`, the register is loaded from the guard (`mov r32, [guard]`, `mov r8, [guard+n]` for a byte) with
    // nothing between that could write it.
    const auto loaded_from_guard = [&](std::uint32_t claim, std::string& why) {
        const std::uint32_t or_at = claim - 1;
        const Insn& store = code[claim];
        std::vector<std::uint32_t> todo = pred[or_at];
        std::set<std::uint32_t> seen;
        if (todo.empty()) {
            why = "nothing before the `or` at " + where(or_at) + " loads the register from the guard";
            return false;
        }
        while (!todo.empty()) {
            const std::uint32_t p = todo.back();
            todo.pop_back();
            if (!seen.insert(p).second) {
                continue;
            }
            const Insn& in = code[p];
            const Ops& o = in.ops;
            if (in.use == Use::Read && o.ok && !o.map0f && !o.p66 && (o.rex & 0x08) == 0) {
                const std::uint32_t r = o.op == 0x8A && o.rex == 0 ? static_cast<std::uint32_t>(o.reg | 0x100u) : o.reg;
                const bool dword = o.op == 0x8B && store.claim_width == 4 && store.guard_at == 0 && in.guard_at == 0;
                const bool byte = o.op == 0x8A && store.claim_width == 1 && in.guard_at == store.guard_at;
                if ((dword || byte) && r == store.claim_reg) {
                    continue;  // this path loads the guard
                }
            }
            if (!preserves_registers(in)) {
                why = "the register the `or` at " + where(or_at) + " sets a bit in is not shown to hold the guard: " + where(p) + " comes before it without a load of the guard";
                return false;
            }
            if (pred[p].empty()) {
                why = "a path to the `or` at " + where(or_at) + " starts at " + where(p) + " without loading the register from the guard";
                return false;
            }
            todo.insert(todo.end(), pred[p].begin(), pred[p].end());
        }
        return true;
    };
    // The region of each claim: every instruction reachable from it that the rest of the code cannot reach without passing
    // the claim (the code that runs only once the bit is set or the header returned), up to the next use of the guard
    // (a bitmask) or the guard's footer (thread-safe). Nothing may enter it from outside. Its static is the one candidate
    // it writes (a store, or its address formed for a constructor); reads do not count.
    std::set<std::uint32_t> footers;
    for (std::uint32_t c = 0; c < code.size(); ++c) {
        const bool thread_safe = kind_ == GuardCodeKind::ThreadSafe;
        if (thread_safe ? code[c].use != Use::Header : code[c].use != Use::Claim) {
            continue;
        }
        std::string why;
        if (!thread_safe && code[c].claim_reg != kNone && !loaded_from_guard(c, why)) {
            return fail(out, why);
        }
        const std::uint32_t from = thread_safe ? c + 1 : c;  // the instruction the region follows (the header's call)
        // What the code reaches without the claim.
        std::vector<bool> shared(code.size(), false);
        std::vector<std::uint32_t> todo(entries.begin(), entries.end());
        while (!todo.empty()) {
            const std::uint32_t x = todo.back();
            todo.pop_back();
            if (x == c || shared[x]) {
                continue;
            }
            shared[x] = true;
            todo.insert(todo.end(), succ[x].begin(), succ[x].end());
        }
        std::vector<bool> in_region(code.size(), false);
        bool footer_reached = false;
        std::uint32_t last = from;
        todo = succ[from];
        while (!todo.empty()) {
            const std::uint32_t x = todo.back();
            todo.pop_back();
            if (in_region[x] || shared[x]) {
                continue;  // already seen, or a join with the rest of the code
            }
            const Insn& insn = code[x];
            if (thread_safe) {
                if (insn.use == Use::Footer) {
                    footer_reached = true;
                    footers.insert(x);
                    continue;
                }
                if (insn.use != Use::None && insn.use != Use::Read) {
                    return fail(out, "the region of the guard's _Init_thread_header at " + where(c) + " reaches another use of the guard at " + where(x));
                }
                if ((insn.flags & (lde::kInsnRet | lde::kInsnTrap)) != 0) {
                    return fail(out, "the region of the guard's _Init_thread_header at " + where(c) + " ends at " + where(x) + " before its _Init_thread_footer");
                }
            } else if (insn.use != Use::None) {
                continue;  // the guard again: the region ends
            }
            in_region[x] = true;
            last = std::max(last, x);
            todo.insert(todo.end(), succ[x].begin(), succ[x].end());
        }
        if (thread_safe && !footer_reached) {
            return fail(out, "the region of the guard's _Init_thread_header at " + where(c) + " never reaches its _Init_thread_footer");
        }
        std::set<std::string> written;
        for (std::uint32_t x = 0; x < code.size(); ++x) {
            if (!in_region[x]) {
                continue;
            }
            for (const std::uint32_t p : pred[x]) {
                if (!in_region[p] && p != from) {
                    return fail(out, "the region of the claim at " + where(c) + " is entered from " + where(p));
                }
            }
            for (const std::uint32_t k : code[x].relocs) {
                if (candidates_.count(relocs[k].target) == 0) {
                    continue;
                }
                if (!code[x].ops.ok) {
                    return fail(out, "the region of the claim at " + where(c) + " refers to " + relocs[k].target + " at " + where(x) + " in an instruction that is not read");
                }
                if (!memory_operand(code[x], relocs[k]) || writes_operand(code[x].ops)) {
                    written.insert(relocs[k].target);
                }
            }
        }
        if (written.size() != 1) {
            std::string list;
            for (const std::string& s : written) {
                list += (list.empty() ? "" : ", ") + s;
            }
            return fail(out, "the region of the claim at " + where(c) + " (to " + where(last) + ") writes " +
                                 (written.empty() ? std::string("none of the statics the guard's name ties") : std::to_string(written.size()) + " of them (" + list + ")"));
        }
        GuardClaim gc;
        gc.section = cs.name;
        gc.at = code[c].start;
        gc.end = code[last].start + code[last].length;
        gc.bits = code[c].bits;
        gc.static_name = *written.begin();
        out.claims.push_back(std::move(gc));
    }
    for (std::uint32_t x = 0; x < code.size(); ++x) {
        if (code[x].use == Use::Footer && footers.count(x) == 0) {
            return fail(out, "the guard's _Init_thread_footer at " + where(x) + " ends no region that began with its _Init_thread_header");
        }
    }
    return true;
}

}  // namespace

GuardCodeReading read_guard_code(std::span<const GuardCodeSection> code, std::string_view guard, std::int32_t guard_section, GuardCodeKind kind,
                                 const std::map<std::string, std::int32_t, std::less<>>& candidates, std::string_view data_reference) {
    GuardCodeReading out;
    if (!data_reference.empty()) {
        out.why = "the guard's address is stored in " + std::string(data_reference);
        return out;
    }
    Reader reader(guard, guard_section, kind, candidates);
    for (const GuardCodeSection& cs : code) {
        if (!reader.section(cs, out)) {
            return out;
        }
    }
    if (out.claims.empty()) {
        out.why = "no code of the patch initialises a static under the guard";
        return out;
    }
    // One static per bit (or per thread-safe guard), one bit per static.
    std::map<std::uint32_t, std::string> by_bit;
    std::map<std::string, std::uint32_t> by_static;
    for (const GuardClaim& c : out.claims) {
        const auto b = by_bit.emplace(c.bits, c.static_name);
        const auto s = by_static.emplace(c.static_name, c.bits);
        if (b.first->second != c.static_name || s.first->second != c.bits) {
            out.why = kind == GuardCodeKind::ThreadSafe ? "the guard's regions initialise two statics, " + b.first->second + " and " + c.static_name
                      : b.first->second != c.static_name
                          ? "the bits and statics of the guard's regions do not pair one to one: bit " + hex(c.bits) + " initialises " + b.first->second + " and " + c.static_name
                          : "the bits and statics of the guard's regions do not pair one to one: " + c.static_name + " is initialised under bits " + hex(s.first->second) +
                                " and " + hex(c.bits);
            out.claims.clear();
            return out;
        }
        out.statics.insert(c.static_name);
    }
    out.proved = true;
    return out;
}

}  // namespace cml::core::bind
