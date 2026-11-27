// WP-51 (R-54, ADR-0044): the functions a procedure inlined (PdbIndex::proc_inlinees).
//
// clang-cl at -O2 records the function-local statics of code it inlined as plain S_LDATA32 records of the outermost
// procedure, next to its S_INLINESITE records (docs/wp/WP-51.md, round 1). A record named `x` directly in a procedure is
// therefore the procedure's own static or a static of something it inlined; the inline sites say what it inlined. Each
// S_INLINESITE / S_INLINESITE2 names its inlinee by an IPI index: LF_FUNC_ID (scope, type, name) for a function,
// LF_MFUNC_ID (class, type, name) for a member function. Read on demand, for one procedure at a time; nothing is kept.

#include <string>

#include "cml/pdb/diag_ids.h"
#include "detail/codeview.h"
#include "detail/le.h"
#include "index_impl.h"

namespace cml::pdb {

using namespace detail;

namespace {

constexpr std::uint16_t LF_FUNC_ID = 0x1601;
constexpr std::uint16_t LF_MFUNC_ID = 0x1602;

std::string hex_id(std::uint32_t v) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out = "0x";
    bool started = false;
    for (int shift = 28; shift >= 0; shift -= 4) {
        const unsigned d = (v >> static_cast<unsigned>(shift)) & 0xFu;
        if (d != 0 || started || shift == 0) {
            out.push_back(digits[d]);
            started = true;
        }
    }
    return out;
}

// Fills `e` from the IPI record `e.id` (and the TPI record of its class).
void resolve_inlinee(const TypeSource& types, PdbIndex::Inlinee& e) {
    TypeRecord r;
    if (e.id < 0x1000 || !types.find_id(e.id, r)) {
        e.why = "the inline site names id " + hex_id(e.id) + ", which the IPI stream does not hold";
        return;
    }
    if ((r.kind != LF_FUNC_ID && r.kind != LF_MFUNC_ID) || r.body_len < 9) {
        e.why = "the inline site names id " + hex_id(e.id) + ", a record of kind " + hex_id(r.kind) + " (neither LF_FUNC_ID nor LF_MFUNC_ID)";
        return;
    }
    e.member = r.kind == LF_MFUNC_ID;
    const std::size_t len = cv::cstr_len(r.body + 8, r.body_len - 8);
    if (len == r.body_len - 8) {
        e.why = "the name of id " + hex_id(e.id) + " is not terminated";
        return;
    }
    e.name.assign(reinterpret_cast<const char*>(r.body + 8), len);
    if (e.member) {
        const std::uint32_t cls = rd32(r.body);
        TypeRecord c;
        std::string_view name;
        std::string_view unique;
        if (!types.find_type(cls, c) || !udt_record_names(c, name, unique)) {
            e.why = "the class " + hex_id(cls) + " of member function '" + e.name + "' (id " + hex_id(e.id) + ") cannot be read from the TPI stream";
            return;
        }
        e.class_name.assign(name);
        e.class_unique = without_tu_discriminator(unique);
    }
    const FunctionSignature sig = function_signature(types, e.id, true);
    e.signature_known = sig.known;
    if (sig.known) {
        e.signature = sig.text;
    } else {
        e.why = "the signature of '" + e.name + "' (id " + hex_id(e.id) + ") cannot be read: " + sig.why;
    }
    e.readable = true;
}

}  // namespace

Result<PdbIndex::ProcInlinees> PdbIndex::proc_inlinees(const SymbolView& proc) const {
    const Impl& impl = *impl_;
    if (!is_proc_kind(proc.kind) || proc.compiland >= impl.compilands.size()) {
        return Diag::make(diag::kCompilandOutOfRange, "proc_inlinees needs a proc symbol of a compiland of this index.", "Pass a symbol returned by compiland_symbols.");
    }
    const Compiland& c = impl.compilands[proc.compiland];
    if (c.symbol_stream == 0xFFFF || c.symbol_bytes <= 4 || c.symbol_bytes > impl.msf->stream_size(c.symbol_stream)) {
        return Diag::make(diag::kBadModuleSymbols, "The symbols of module '" + std::string(c.object_name) + "' in '" + impl.path + "' are unreadable.",
                          "Rebuild the module so the linker regenerates the PDB.");
    }
    CountedBuffer scratch;
    std::size_t got = 0;
    const std::uint8_t* data = impl.msf->read_range(c.symbol_stream, 0, c.symbol_bytes, scratch, got);
    if (data == nullptr || got < c.symbol_bytes) {
        return Diag::make(diag::kBadModuleSymbols, "The symbols of module '" + std::string(c.object_name) + "' in '" + impl.path + "' cannot be read.",
                          "Rebuild the module so the linker regenerates the PDB.");
    }
    ProcInlinees out;
    enum class State { Before, Inside, Done } state = State::Before;
    std::size_t depth = 0;         // scopes open since the procedure's record, itself included
    std::size_t inline_depth = 0;  // inline sites among them
    std::vector<bool> open_inline;  // per open scope inside the procedure: is it an inline site
    for (std::uint64_t off = 4; off < c.symbol_bytes && state != State::Done;) {
        if (c.symbol_bytes - off < 4) {
            break;
        }
        const std::uint32_t reclen = rd16(data + off);
        if (reclen < 2 || reclen > c.symbol_bytes - off - 2) {
            return Diag::make(diag::kBadModuleSymbols, "The symbols of module '" + std::string(c.object_name) + "' in '" + impl.path + "' are corrupt (a record leaves the symbol area).",
                              "Rebuild the module so the linker regenerates the PDB.");
        }
        const std::uint16_t k = rd16(data + off + 2);
        const std::uint8_t* b = data + off + 4;
        const std::uint32_t n = reclen - 2;
        off += 2 + reclen;
        if (state == State::Before) {
            if (cv::is_proc(k) && n >= 36 && rd16(b + 32) == proc.section && rd32(b + 28) == proc.offset) {
                state = State::Inside;
                depth = 1;
                open_inline.assign(1, false);
            }
            continue;
        }
        if (cv::is_scope_opener(k)) {
            const bool site = k == cv::S_INLINESITE || k == cv::S_INLINESITE2;
            ++depth;
            open_inline.push_back(site);
            if (!site) {
                continue;
            }
            ++inline_depth;
            ++out.sites;
            // S_INLINESITE: parent u32, end u32, inlinee u32, annotations; S_INLINESITE2 has an invocation count before them.
            const std::uint32_t id = n >= 12 ? rd32(b + 8) : 0;
            Inlinee* entry = nullptr;
            for (Inlinee& e : out.inlinees) {
                if (e.id == id) {
                    entry = &e;
                    break;
                }
            }
            if (entry == nullptr) {
                if (out.inlinees.size() >= kMaxProcInlinees) {
                    out.complete = false;
                    continue;
                }
                Inlinee e;
                e.id = id;
                e.depth = static_cast<std::uint32_t>(inline_depth);
                if (n < 12) {
                    e.why = "an inline site record is too short to name its inlinee";
                } else {
                    resolve_inlinee(types(), e);
                }
                out.inlinees.push_back(std::move(e));
                entry = &out.inlinees.back();
            }
            ++entry->sites;
            if (static_cast<std::uint32_t>(inline_depth) < entry->depth) {
                entry->depth = static_cast<std::uint32_t>(inline_depth);
            }
            continue;
        }
        if (cv::is_scope_closer(k)) {
            if (!open_inline.empty()) {
                if (open_inline.back()) {
                    --inline_depth;
                }
                open_inline.pop_back();
            }
            if (--depth == 0) {
                state = State::Done;
            }
            continue;
        }
    }
    if (state == State::Before) {
        return Diag::make(diag::kBadModuleSymbols, "The symbol record of '" + std::string(proc.name) + "' was not found in module '" + std::string(c.object_name) + "'.",
                          "Rebuild the module so the linker regenerates the PDB.");
    }
    if (state == State::Inside) {
        return Diag::make(diag::kBadModuleSymbols, "The procedure '" + std::string(proc.name) + "' in module '" + std::string(c.object_name) + "' has no end record.",
                          "Rebuild the module so the linker regenerates the PDB.");
    }
    return out;
}

}  // namespace cml::pdb
