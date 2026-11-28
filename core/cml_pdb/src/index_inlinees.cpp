// WP-51 (R-54, ADR-0044): the functions a procedure inlined (PdbIndex::proc_inlinees).
//
// clang-cl at -O2 records the function-local statics of code it inlined as plain S_LDATA32 records of the outermost
// procedure, next to its S_INLINESITE records (docs/wp/WP-51.md, round 1). A record named `x` directly in a procedure is
// therefore the procedure's own static or a static of something it inlined; the inline sites say what it inlined. Each
// S_INLINESITE / S_INLINESITE2 names its inlinee by an IPI index: LF_FUNC_ID (scope, type, name) for a function,
// LF_MFUNC_ID (class, type, name) for a member function. Read on demand, for the procedures asked for in one walk of their
// module stream (procs_inlinees); nothing is kept.

#include <string>
#include <unordered_map>
#include <vector>

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

}  // namespace

// Fills `e` from the IPI record `e.id` (and the TPI record of its class).
void PdbIndex::resolve_inlinee(const TypeSource& types, PdbIndex::Inlinee& e) {
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
    } else if (const std::uint32_t scope = rd32(r.body); scope != 0) {
        // WP-60 (R-57): the parent scope, an LF_STRING_ID (its first field the substring list, then the name). A string with a
        // substring list is the tail of a longer one (review: not rebuilt here), so its scope is not known.
        TypeRecord s;
        const std::size_t slen = scope >= 0x1000 && types.find_id(scope, s) && s.kind == cv::LF_STRING_ID && s.body_len > 4 && rd32(s.body) == 0
                                     ? cv::cstr_len(s.body + 4, s.body_len - 4)
                                     : 0;
        if (slen == 0 || slen == s.body_len - 4) {
            e.scope_known = false;
        } else {
            e.scope.assign(reinterpret_cast<const char*>(s.body + 4), slen);
        }
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

Result<PdbIndex::ProcInlinees> PdbIndex::proc_inlinees(const SymbolView& proc) const {
    Result<std::vector<ProcInlinees>> all = procs_inlinees(std::vector<SymbolView>{proc});
    if (!all) {
        return all.error();
    }
    return std::move((*all)[0]);
}

Result<std::vector<PdbIndex::ProcInlinees>> PdbIndex::procs_inlinees(const std::vector<SymbolView>& procs) const {
    const Impl& impl = *impl_;
    if (procs.empty()) {
        return std::vector<ProcInlinees>{};
    }
    const std::uint32_t ci = procs.front().compiland;
    for (const SymbolView& p : procs) {
        if (!is_proc_kind(p.kind) || p.compiland >= impl.compilands.size() || p.compiland != ci) {
            return Diag::make(diag::kCompilandOutOfRange, "procs_inlinees needs proc symbols of one compiland of this index.", "Pass symbols returned by compiland_symbols.");
        }
    }
    const Compiland& c = impl.compilands[ci];
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

    // The wanted procedures by their section:offset (the same key proc_statics finds a procedure by). A procedure asked for
    // twice gets one walk and is copied at the end.
    std::unordered_map<std::uint64_t, std::size_t> wanted;
    for (std::size_t i = 0; i < procs.size(); ++i) {
        wanted.emplace((static_cast<std::uint64_t>(procs[i].section) << 32) | procs[i].offset, i);
    }
    std::vector<ProcInlinees> out(procs.size());
    std::vector<std::unordered_map<std::uint32_t, std::size_t>> seen(procs.size());  // per procedure: id -> index in inlinees
    enum class Found : std::uint8_t { No, Open, Done };
    std::vector<Found> found(procs.size(), Found::No);
    std::size_t done = 0;
    std::unordered_map<std::uint32_t, Inlinee> resolved;  // id -> the inlinee resolved once (sites and depth not set)

    // Every open scope: the wanted procedure it belongs to (kNone: none; a procedure that is not wanted owns its own scopes,
    // so its inline sites are nobody's) and the number of inline sites open inside that procedure, itself included.
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    struct Scope {
        std::size_t owner = kNone;
        std::uint32_t inline_depth = 0;
        bool root = false;  // the record of a wanted procedure
    };
    std::vector<Scope> scopes;
    for (std::uint64_t off = 4; off < c.symbol_bytes && done < wanted.size();) {
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
        if (cv::is_scope_closer(k)) {
            if (!scopes.empty()) {
                const Scope s = scopes.back();
                scopes.pop_back();
                if (s.root && found[s.owner] == Found::Open) {
                    found[s.owner] = Found::Done;
                    ++done;
                }
            }
            continue;
        }
        if (!cv::is_scope_opener(k)) {
            continue;
        }
        const Scope parent = scopes.empty() ? Scope{} : scopes.back();
        Scope s;
        if (cv::is_proc(k)) {
            // A procedure, at the top or nested in another: its scopes are its own.
            if (n >= 36) {
                const auto w = wanted.find((static_cast<std::uint64_t>(rd16(b + 32)) << 32) | rd32(b + 28));
                if (w != wanted.end() && found[w->second] == Found::No) {
                    s.owner = w->second;
                    s.root = true;
                    found[w->second] = Found::Open;
                }
            }
            scopes.push_back(s);
            continue;
        }
        const bool site = k == cv::S_INLINESITE || k == cv::S_INLINESITE2;
        s.owner = parent.owner;
        s.inline_depth = parent.inline_depth + (site ? 1u : 0u);
        scopes.push_back(s);
        if (!site || s.owner == kNone) {
            continue;
        }
        ProcInlinees& pi = out[s.owner];
        ++pi.sites;
        // S_INLINESITE: parent u32, end u32, inlinee u32, annotations; S_INLINESITE2 has an invocation count before them.
        const std::uint32_t id = n >= 12 ? rd32(b + 8) : 0;
        const auto at = seen[s.owner].find(id);
        Inlinee* entry = nullptr;
        if (at != seen[s.owner].end()) {
            entry = &pi.inlinees[at->second];
        } else {
            if (pi.inlinees.size() >= kMaxProcInlinees) {
                pi.complete = false;
                continue;
            }
            Inlinee e;
            if (n < 12) {
                e.why = "an inline site record is too short to name its inlinee";
            } else {
                auto r = resolved.find(id);
                if (r == resolved.end()) {
                    Inlinee fresh;
                    fresh.id = id;
                    resolve_inlinee(types(), fresh);
                    r = resolved.emplace(id, std::move(fresh)).first;
                }
                e = r->second;
            }
            e.id = id;
            e.depth = s.inline_depth;
            seen[s.owner].emplace(id, pi.inlinees.size());
            pi.inlinees.push_back(std::move(e));
            entry = &pi.inlinees.back();
        }
        ++entry->sites;
        if (s.inline_depth < entry->depth) {
            entry->depth = s.inline_depth;
        }
    }
    for (std::size_t i = 0; i < procs.size(); ++i) {
        const std::size_t first = wanted.find((static_cast<std::uint64_t>(procs[i].section) << 32) | procs[i].offset)->second;
        if (found[first] == Found::No) {
            return Diag::make(diag::kBadModuleSymbols, "The symbol record of '" + std::string(procs[i].name) + "' was not found in module '" + std::string(c.object_name) + "'.",
                              "Rebuild the module so the linker regenerates the PDB.");
        }
        if (found[first] == Found::Open) {
            return Diag::make(diag::kBadModuleSymbols, "The procedure '" + std::string(procs[i].name) + "' in module '" + std::string(c.object_name) + "' has no end record.",
                              "Rebuild the module so the linker regenerates the PDB.");
        }
        if (first != i) {
            out[i] = out[first];
        }
    }
    return out;
}

}  // namespace cml::pdb
