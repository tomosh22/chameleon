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
    // Opcodes without ModRM that this reader looks at: or/and al|eax, imm and the direct branches; parse_layout pretends a
    // ModRM for some of them, so their immediate is taken from the end of the instruction instead.
    const bool no_modrm = !o.map0f && (o.op == 0x0C || o.op == 0x0D || o.op == 0x24 || o.op == 0x25 || (o.op >= 0x70 && o.op <= 0x7F) || o.op == 0xEB ||
                                       o.op == 0xE8 || o.op == 0xE9 || (o.op >= 0xE0 && o.op <= 0xE3));
    const bool jcc32 = o.map0f && o.op >= 0x80 && o.op <= 0x8F;
    if (no_modrm || jcc32) {
        std::uint32_t n = 0;
        if (o.op == 0x0C || o.op == 0x24 || (!o.map0f && o.op >= 0x70 && o.op <= 0x7F) || o.op == 0xEB || (!o.map0f && o.op >= 0xE0 && o.op <= 0xE3)) {
            n = 1;
        } else if (o.op == 0x0D || o.op == 0x25) {
            n = o.p66 ? 2u : 4u;
        } else {
            n = 4;  // E8, E9, 0F 8x
        }
        if (len != i + 1 + n) {
            return o;
        }
        o.imm_len = n;
        o.imm = read_signed(p + len - n, n);
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
    std::uint32_t bits = 0;  // Claim: the bit set
    std::uint32_t jump_to = 0xFFFFFFFFu;  // a direct branch inside the section: its target

    // `jmp` to the instruction right behind it (clang at -O0: an invoke's normal destination): control falls through.
    bool falls_through() const noexcept { return (flags & cml::agent::hook::kInsnJmp) != 0 && jump_to == start + length; }
};

struct Function {
    std::uint32_t start = 0;
    std::uint32_t end = 0;  // the first byte not decoded
    bool complete = false;  // reached the next function (or the section end)
    std::string stop;       // why not
    std::vector<Insn> insns;
};

struct Branch {
    std::uint32_t from = 0;  // the branch instruction
    std::uint32_t to = 0;
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
// `out_bits` = the immediate's bits (bts/btr: the one bit), `width` the operand width in bytes.
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
        out_bits = 1ull << (static_cast<std::uint64_t>(o.imm) & (8 * width - 1));
        return o.rm;
    }
    return kNone;
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
    std::vector<GuardCodeReloc> relocs = cs.relocs;
    std::stable_sort(relocs.begin(), relocs.end(), [](const GuardCodeReloc& a, const GuardCodeReloc& b) { return a.offset < b.offset; });
    // A reference to the guard's section through its section symbol hides a use of the guard (in any code section), one to
    // a candidate's section a reference to that static (in a section that uses the guard: its regions are read).
    bool refers = false;
    for (const GuardCodeReloc& rel : relocs) {
        refers = refers || rel.target == guard_;
    }
    for (const GuardCodeReloc& rel : relocs) {
        const bool guard_section = guard_section_ > 0 && rel.target_section == guard_section_;
        if (rel.target_is_section && (guard_section || (refers && candidate_sections_.count(rel.target_section) != 0))) {
            return fail(out, "the code at +" + hex(rel.offset) + " in " + cs.name + " refers to the section of " +
                                 (guard_section ? std::string("the guard") : std::string("one of its statics")) + " through its section symbol '" + rel.target + "'");
        }
    }
    if (!refers) {
        return true;
    }
    std::vector<std::uint32_t> starts = cs.function_starts;
    starts.push_back(0);
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    const std::uint8_t* const base = reinterpret_cast<const std::uint8_t*>(cs.data.data());
    const std::uint64_t size = cs.data.size();
    std::vector<Function> functions;
    std::vector<Branch> branches;
    std::set<std::uint32_t> targets;
    std::map<std::uint32_t, std::vector<std::uint32_t>> sources;  // branch target -> the branches to it
    std::vector<bool> consumed(relocs.size(), false);
    // The sweep (as WP-36's SiteProver): linear, from every start to the next; every relocation it passes must be exactly
    // the 32-bit displacement of one decoded instruction or lie wholly inside its immediate.
    for (std::size_t f = 0; f < starts.size(); ++f) {
        Function fn;
        fn.start = starts[f];
        const std::uint64_t limit = f + 1 < starts.size() ? std::min<std::uint64_t>(starts[f + 1], size) : size;
        std::size_t ri = static_cast<std::size_t>(
            std::lower_bound(relocs.begin(), relocs.end(), fn.start, [](const GuardCodeReloc& r, std::uint32_t v) { return r.offset < v; }) - relocs.begin());
        std::uint64_t off = fn.start;
        bool stopped = false;
        while (off < limit) {
            const lde::DecodeResult r = lde::decode_instruction(base + off, static_cast<std::size_t>(limit - off));
            if (r.status != lde::DecodeStatus::Ok) {
                fn.stop = "the instruction at +" + hex(off) + " (" + hex_bytes(cs.data, static_cast<std::uint32_t>(off), 15) + ") is not known to the decoder";
                stopped = true;
                break;
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
                    fn.stop = "the relocation at +" + hex(rel.offset) + " does not fit an operand of the instruction at +" + hex(off) + " (" +
                              hex_bytes(cs.data, static_cast<std::uint32_t>(off), r.length) + ")";
                    stopped = true;
                    break;
                }
                insn.relocs.push_back(static_cast<std::uint32_t>(ri));
            }
            if (stopped) {
                break;
            }
            // A direct branch inside the section: its target is a join.
            const Ops& o = insn.ops;
            const bool direct = o.ok && ((!o.map0f && ((o.op >= 0x70 && o.op <= 0x7F) || o.op == 0xEB || o.op == 0xE9 || (o.op >= 0xE0 && o.op <= 0xE3))) ||
                                         (o.map0f && o.op >= 0x80 && o.op <= 0x8F));
            if (direct && insn.relocs.empty()) {
                const std::int64_t to = static_cast<std::int64_t>(end) + o.imm;
                if (to >= 0 && static_cast<std::uint64_t>(to) < size) {
                    branches.push_back({insn.start, static_cast<std::uint32_t>(to)});
                    targets.insert(static_cast<std::uint32_t>(to));
                    sources[static_cast<std::uint32_t>(to)].push_back(insn.start);
                    insn.jump_to = static_cast<std::uint32_t>(to);
                }
            }
            fn.insns.push_back(std::move(insn));
            off = end;
        }
        fn.end = static_cast<std::uint32_t>(off);
        fn.complete = !stopped;
        functions.push_back(std::move(fn));
    }
    const auto field_offset = [&](const GuardCodeReloc& rel, const Insn& insn) -> std::int64_t {
        // The address the instruction forms is S + A + (bytes behind the field) - k for REL32_k (k = 0 for REL32): the
        // offset into the target. clang writes REL32 with A = offset - tail, cl REL32_k with A = offset.
        std::int32_t a = 0;
        std::memcpy(&a, base + rel.offset, 4);
        const std::int64_t tail = static_cast<std::int64_t>(insn.start) + insn.length - (static_cast<std::int64_t>(rel.offset) + 4);
        return static_cast<std::int64_t>(a) + tail - rel.rel_tail;
    };
    const auto where = [&](const Insn& insn) { return "+" + hex(insn.start) + " (" + hex_bytes(cs.data, insn.start, insn.length) + ") in " + cs.name; };
    // Every use of the guard, classified.
    for (Function& fn : functions) {
        for (std::size_t i = 0; i < fn.insns.size(); ++i) {
            Insn& insn = fn.insns[i];
            const GuardCodeReloc* g = nullptr;
            for (const std::uint32_t k : insn.relocs) {
                if (candidates_.find(relocs[k].target) != candidates_.end()) {
                    const Ops& co = insn.ops;
                    const bool memory_operand = co.ok && co.riprel && (insn.flags & lde::kInsnRipRel) != 0 && relocs[k].offset == insn.start + co.disp_at &&
                                                !(!co.map0f && co.op == 0x8D);
                    if (!memory_operand) {
                        out.address_taken.insert(relocs[k].target);
                    }
                }
                if (relocs[k].target == guard_) {
                    if (g != nullptr) {
                        return fail(out, "the instruction at " + where(insn) + " refers to the guard twice");
                    }
                    g = &relocs[k];
                    consumed[k] = true;
                }
            }
            if (g == nullptr) {
                continue;
            }
            ++out.references;
            const Ops& o = insn.ops;
            if (!o.ok || o.other_prefix || !o.riprel || (insn.flags & lde::kInsnRipRel) == 0 || g->rel_tail < 0 || g->offset != insn.start + o.disp_at) {
                return fail(out, "the guard is used at " + where(insn) + " in a form that is not read (not a plain RIP-relative operand)");
            }
            const std::int64_t at = field_offset(*g, insn);
            const bool rex_w = (o.rex & 0x08) != 0;
            if (kind_ == GuardCodeKind::ThreadSafe) {
                if (at != 0) {
                    return fail(out, "the guard is used at " + where(insn) + " at offset " + std::to_string(at) + " into it");
                }
                if (!o.map0f && (o.op == 0x8B || o.op == 0x3B || o.op == 0x39 || ((o.op == 0x81 || o.op == 0x83) && (o.reg & 7u) == 7u))) {
                    insn.use = Use::Read;
                    continue;
                }
                if (!o.map0f && o.op == 0x8D && rex_w && o.reg == 1) {
                    // lea rcx, [guard], then the call of the CRT's guard functions (their first argument).
                    if (i + 1 >= fn.insns.size() || targets.count(fn.insns[i + 1].start) != 0) {
                        return fail(out, "the address of the guard is loaded at " + where(insn) + " and not passed straight to a call");
                    }
                    const Insn& call = fn.insns[i + 1];
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
                        return fail(out, "the address of the guard is loaded at " + where(insn) + " for " +
                                             (callee.empty() ? std::string("something that is not a call") : "a call of '" + callee + "'"));
                    }
                    continue;
                }
                return fail(out, "the guard is used at " + where(insn) + " by an instruction that is not read");
            }
            // A bitmask.
            if (at < 0 || at > 3) {
                return fail(out, "the guard is used at " + where(insn) + " at offset " + std::to_string(at) + " into it");
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
                    return fail(out, "the guard is written at " + where(insn) + " beyond its four bytes");
                }
                const std::uint64_t imm = static_cast<std::uint64_t>(o.imm) & (mem_width == 8 ? ~0ull : ((1ull << (8 * mem_width)) - 1));
                if ((o.reg & 7u) == 4u) {
                    insn.use = Use::Release;
                    continue;
                }
                insn.use = Use::Claim;
                insn.bits = static_cast<std::uint32_t>(imm << (8 * at));
                if (!single_bit(imm << (8 * at))) {
                    return fail(out, "the store at " + where(insn) + " sets " + hex(imm << (8 * at)) + " in the guard, not one bit");
                }
                continue;
            }
            if (o.map0f && o.op == 0xBA && ((o.reg & 7u) == 5u || (o.reg & 7u) == 6u)) {
                if (at != 0) {
                    return fail(out, "the guard's bit is set at " + where(insn) + " at offset " + std::to_string(at) + " into it");
                }
                if ((o.reg & 7u) == 6u) {
                    insn.use = Use::Release;
                    continue;
                }
                insn.use = Use::Claim;
                insn.bits = 1u << (static_cast<std::uint64_t>(o.imm) & 31u);
                continue;
            }
            if (!o.map0f && (o.op == 0x89 || o.op == 0x88) && !rex_w) {
                // A register stored to the guard: what the instruction before it did to that register.
                const std::uint32_t width = o.op == 0x88 ? 1u : (o.p66 ? 2u : 4u);
                const std::uint32_t reg = o.op == 0x88 && o.rex == 0 ? static_cast<std::uint32_t>(o.reg | 0x100u) : o.reg;
                if (at + width > 4) {
                    return fail(out, "the guard is written at " + where(insn) + " beyond its four bytes");
                }
                if (i == 0 || targets.count(insn.start) != 0) {
                    return fail(out, "the store to the guard at " + where(insn) + " can be reached other than from the instruction before it");
                }
                const Insn& prev = fn.insns[i - 1];
                std::uint64_t bits = 0;
                std::uint32_t pw = 0;
                if (reg_op_with_immediate(prev.ops, true, bits, pw) == reg && pw == width) {
                    insn.use = Use::Claim;
                    insn.bits = static_cast<std::uint32_t>(bits << (8 * at));
                    if (!single_bit(bits << (8 * at))) {
                        return fail(out, "the store at " + where(insn) + " sets " + hex(bits << (8 * at)) + " in the guard, not one bit");
                    }
                    continue;
                }
                if (reg_op_with_immediate(prev.ops, false, bits, pw) == reg && pw == width) {
                    insn.use = Use::Release;
                    continue;
                }
                return fail(out, "the value stored to the guard at " + where(insn) + " is not an or (or and) of a constant by the instruction before it, " +
                                     where(prev));
            }
            return fail(out, "the guard is used at " + where(insn) + " by an instruction that is not read");
        }
    }
    for (std::size_t k = 0; k < relocs.size(); ++k) {
        if (relocs[k].target == guard_ && !consumed[k]) {
            std::string stop;
            for (const Function& fn : functions) {
                if (!fn.complete && relocs[k].offset >= fn.end && (stop.empty() || fn.start <= relocs[k].offset)) {
                    stop = fn.stop;
                }
            }
            return fail(out, "the guard is used at +" + hex(relocs[k].offset) + " in " + cs.name + ", behind code that was not decoded" + (stop.empty() ? "" : ": " + stop));
        }
    }
    // The regions of the claims, and the one candidate each refers to.
    const auto candidate_of = [&](const Insn& insn, std::set<std::string>& seen) {
        for (const std::uint32_t k : insn.relocs) {
            if (candidates_.find(relocs[k].target) != candidates_.end()) {
                seen.insert(relocs[k].target);
            }
        }
    };
    std::set<std::uint32_t> footers;  // the footers that end a region
    for (const Function& fn : functions) {
        for (std::size_t i = 0; i < fn.insns.size(); ++i) {
            const Insn& claim = fn.insns[i];
            if (kind_ == GuardCodeKind::ThreadSafe ? claim.use != Use::Header : claim.use != Use::Claim) {
                continue;
            }
            std::set<std::string> seen;
            std::uint32_t end = kNone;
            const std::size_t first = kind_ == GuardCodeKind::ThreadSafe ? i + 2 : i + 1;
            std::size_t j = first;
            for (; j < fn.insns.size(); ++j) {
                const Insn& insn = fn.insns[j];
                if (kind_ == GuardCodeKind::Bitmask) {
                    // A join: a branch from outside the region so far reaches this instruction (a branch from inside it, as a
                    // jump over part of the initialiser, does not end it).
                    bool join = false;
                    if (const auto s = sources.find(insn.start); s != sources.end()) {
                        for (const std::uint32_t from : s->second) {
                            join = join || from < claim.start || from >= insn.start;
                        }
                    }
                    if (join || insn.use != Use::None) {
                        end = insn.start;  // a join, or the guard again
                        break;
                    }
                    candidate_of(insn, seen);
                    if (lde::ends_flow(insn.flags) && !insn.falls_through()) {
                        end = insn.start + insn.length;
                        break;
                    }
                    continue;
                }
                if (insn.use == Use::Footer) {
                    end = insn.start;
                    footers.insert(insn.start);
                    break;
                }
                if (insn.use != Use::None && insn.use != Use::Read) {
                    return fail(out, "the region of the guard's _Init_thread_header at " + where(claim) + " reaches another use of the guard at " + where(insn));
                }
                if (lde::ends_flow(insn.flags) && !insn.falls_through()) {
                    return fail(out, "the region of the guard's _Init_thread_header at " + where(claim) + " ends at " + where(insn) + " before its _Init_thread_footer");
                }
                candidate_of(insn, seen);
            }
            if (end == kNone) {
                if (!fn.complete || kind_ == GuardCodeKind::ThreadSafe) {
                    return fail(out, "the region of the claim at " + where(claim) + " runs to the end of the decoded code" + (fn.complete ? std::string() : ": " + fn.stop));
                }
                end = fn.end;
            }
            if (kind_ == GuardCodeKind::ThreadSafe) {
                // Nothing may enter the region from outside it (it would skip the header).
                const std::uint32_t lo = first < fn.insns.size() ? fn.insns[first].start : end;
                for (const Branch& b : branches) {
                    if (b.to >= lo && b.to <= end && (b.from < claim.start || b.from >= end)) {
                        return fail(out, "the region of the guard's _Init_thread_header at " + where(claim) + " is entered from +" + hex(b.from));
                    }
                }
            }
            if (seen.size() != 1) {
                std::string list;
                for (const std::string& s : seen) {
                    list += (list.empty() ? "" : ", ") + s;
                }
                return fail(out, "the region of the claim at " + where(claim) + " (to +" + hex(end) + ") refers to " +
                                     (seen.empty() ? std::string("none of the statics the guard's name ties") : std::to_string(seen.size()) + " of them (" + list + ")"));
            }
            GuardClaim gc;
            gc.section = cs.name;
            gc.at = claim.start;
            gc.end = end;
            gc.bits = claim.bits;
            gc.static_name = *seen.begin();
            out.claims.push_back(std::move(gc));
        }
    }
    for (const Function& fn : functions) {
        for (const Insn& insn : fn.insns) {
            if (insn.use == Use::Footer && footers.count(insn.start) == 0) {
                return fail(out, "the guard's _Init_thread_footer at " + where(insn) + " ends no region that began with its _Init_thread_header");
            }
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
