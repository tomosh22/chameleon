#include "cml/core/bind_plan.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

#include "cml/coff/section_symbols.h"
#include "cml/coff/writer.h"
#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_names.h"
#include "cml/util/log.h"

namespace cml::core::bind {

// ---------------------------------------------------------------------------------------------
// Small public helpers
// ---------------------------------------------------------------------------------------------

const char* to_string(DecisionKind k) noexcept {
    switch (k) {
        case DecisionKind::PatchHooked: return "patch-hooked";
        case DecisionKind::PatchNewFunction: return "patch-new-function";
        case DecisionKind::BindOldData: return "bind-old-data";
        case DecisionKind::PatchNewData: return "patch-new-data";
        case DecisionKind::PatchFresh: return "patch-fresh";
        case DecisionKind::BindOldFunction: return "bind-old-function";
        case DecisionKind::LinkerProvided: return "linker-provided";
        case DecisionKind::WeakDefault: return "weak-default";
        case DecisionKind::Unresolved: return "unresolved";
    }
    return "?";
}

const char* to_string(BindSource s) noexcept {
    switch (s) {
        case BindSource::None: return "none";
        case BindSource::Registry: return "registry";
        case BindSource::CompilandStatic: return "compiland-static";
        case BindSource::CompilandOrdinal: return "compiland-static-ordinal";
        case BindSource::CompilandRva: return "compiland-static-rva";
        case BindSource::CompilandLayout: return "compiland-layout";
        case BindSource::PdbPublic: return "pdb-public";
        case BindSource::PdbGlobal: return "pdb-global";
        case BindSource::ImportSlotPdb: return "import-slot-pdb";
        case BindSource::ImportSlotModule: return "import-slot-module";
        case BindSource::ExportOtherModule: return "export";
        case BindSource::SynthesizedImportSlot: return "synthesized-import-slot";
    }
    return "?";
}

const char* to_string(SymbolType t) noexcept { return t == SymbolType::Function ? "function" : "data"; }

const char* to_string(FixupKind k) noexcept {
    switch (k) {
        case FixupKind::Rel32: return "REL32";
        case FixupKind::Rel32_1: return "REL32_1";
        case FixupKind::Rel32_2: return "REL32_2";
        case FixupKind::Rel32_3: return "REL32_3";
        case FixupKind::Rel32_4: return "REL32_4";
        case FixupKind::Rel32_5: return "REL32_5";
        case FixupKind::Addr64: return "ADDR64";
        case FixupKind::SecRel32: return "SECREL32";
        case FixupKind::Slot: return "SLOT";
    }
    return "?";
}

PlanSummary BindPlan::summary() const {
    PlanSummary s;
    s.hooks = hooks.size();
    for (const SymbolDecision& d : decisions) {
        if (d.decision == DecisionKind::BindOldData) {
            ++s.bind_old_data;
        }
    }
    s.thunks = thunks.size();
    s.new_symbols = new_functions.size() + new_globals.size();
    s.fixups = fixups.size();
    return s;
}

std::string BindPlan::summary_line(std::string_view label) const {
    const PlanSummary s = summary();
    std::string out = "BINDPLAN ";
    out.append(label);
    out += " hooks=" + std::to_string(s.hooks);
    out += " bind_old_data=" + std::to_string(s.bind_old_data);
    out += " thunks=" + std::to_string(s.thunks);
    out += " new=" + std::to_string(s.new_symbols);
    out += " fixups=" + std::to_string(s.fixups);
    return out;
}

std::vector<coff::BindSymbol> BindPlan::binding_symbols() const {
    std::vector<coff::BindSymbol> out;
    out.reserve(thunks.size() + placeholders.size());
    for (const ThunkEntry& t : thunks) {
        out.push_back({t.name, coff::BindKind::Function});
    }
    for (const PlaceholderEntry& p : placeholders) {
        out.push_back({p.name, coff::BindKind::Data});
    }
    return out;
}

const SymbolDecision* BindPlan::find_decision(std::string_view name) const {
    for (const SymbolDecision& d : decisions) {
        if (d.name == name) {
            return &d;
        }
    }
    return nullptr;
}

namespace {

bool starts_with(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

constexpr std::uint32_t kNone32 = 0xFFFFFFFFu;

std::string hex(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

// ---------------------------------------------------------------------------------------------
// The planner
// ---------------------------------------------------------------------------------------------

struct SecInfo {
    const coff::Section* sec = nullptr;
    std::string name;
    bool linked = false;
    bool associative = false;
    std::uint32_t parent = 0;
    bool code = false;
    bool tls = false;
    bool crt_init = false;
    bool runtime_table = false;  // .pdata/.xdata/.rtc$/.CRT$ ... (always fresh)
    bool has_locator = false;
};

struct Sym {
    const coff::Symbol* s = nullptr;
    std::string name;
    bool relevant = false;
    bool defined = false;
    bool is_static = false;
    bool weak = false;
    bool common = false;
    bool decided = false;
    SymbolDecision dec;
};

struct OldRef {
    TargetAddress addr;
    BindSource source = BindSource::None;
    SymbolType type = SymbolType::Data;
    std::uint32_t size = 0;
    std::string note;
};

// The PDB spells a lambda "<lambda_9c7f...>" (a content hash), the decorated name "<lambda_1>" (a counter):
// compare them as "<lambda>".
std::string canonical_lambda_names(std::string n) {
    for (std::size_t at = n.find("<lambda_"); at != std::string::npos; at = n.find("<lambda_", at + 1)) {
        std::size_t e = at + 8;
        while (e < n.size() && ((n[e] >= '0' && n[e] <= '9') || (n[e] >= 'a' && n[e] <= 'z') || (n[e] >= 'A' && n[e] <= 'Z'))) {
            ++e;
        }
        if (e < n.size() && n[e] == '>') {
            n.replace(at, e + 1 - at, "<lambda>");
        }
    }
    return n;
}

class Planner {
public:
    Planner(const PlanInput& in, BindPlan* partial) : in_(in), obj_(*in.object), pdb_(*in.module_pdb), partial_(partial) {}

    Result<BindPlan> run(std::vector<Diag>* all_errors);

private:
    const PlanInput& in_;
    const coff::Object& obj_;
    const pdb::PdbIndex& pdb_;
    BindPlan plan_;
    BindPlan* partial_ = nullptr;
    std::vector<Diag> errors_;
    std::optional<Diag> fatal_;
    std::uint32_t ci_ = 0;

    std::vector<SecInfo> secs_;  // element i = section i + 1
    std::vector<Sym> syms_;
    std::vector<std::uint32_t> pos_of_raw_;  // raw symbol index -> position in syms_ (kNone32 when not relevant)
    std::vector<std::vector<coff::ResolvedRelocation>> relocs_;  // per section (element i = section i + 1)
    std::vector<std::vector<std::uint32_t>> sec_syms_;           // per section: positions of defined syms, by value

    bool comp_loaded_ = false;
    std::vector<pdb::SymbolView> comp_syms_;
    std::map<std::uint32_t, std::int64_t> delta_;  // obj section -> (old RVA - section offset); absent: unknown
    std::set<std::uint32_t> delta_conflict_;

    // ---- helpers ----
    void fatal(Diag d) {
        if (!fatal_) {
            fatal_ = std::move(d);
        }
    }
    void error(std::uint32_t code, std::string message, std::string what_to_do) {
        errors_.push_back(Diag::make(code, std::move(message), std::move(what_to_do)));
    }
    void warn(std::uint32_t code, std::string message, std::string what_to_do) {
        plan_.warnings.push_back(Diag::make(code, std::move(message), std::move(what_to_do)));
    }
    TargetAddress module_address(std::uint32_t rva) const {
        TargetAddress t;
        t.valid = true;
        t.rva = rva;
        t.va = in_.module_base + rva;
        t.module = in_.module_name;
        return t;
    }
    static TargetAddress process_address(const SymbolAddress& a) {
        TargetAddress t;
        t.valid = true;
        t.va = a.va;
        t.module = a.origin;
        return t;
    }
    bool pdb_is_function(const pdb::SymbolView& v) const {
        if (v.is_function()) {
            return true;
        }
        if (v.section >= 1 && v.section <= pdb_.sections().size()) {
            return (pdb_.sections()[v.section - 1u].characteristics & coff::scn::MemExecute) != 0;
        }
        return false;
    }
    std::string describe(const Sym& sy) const {
        const std::string disp = sy.dec.display;
        return disp == sy.name ? "'" + sy.name + "'" : "'" + disp + "' ('" + sy.name + "')";
    }
    std::string referencing_function(std::uint32_t section, std::uint32_t offset) const;
    std::string referencing_description(std::uint32_t section, std::uint32_t offset) const;

    // ---- phases ----
    bool resolve_compiland();
    void collect_sections();
    void collect_symbols();
    void load_relocations();
    void decide_defined();
    void bridge_statics();
    void resolve_undefined();
    void build_fixups();
    void assemble();
    void find_initialisers();

    // ---- lookups ----
    std::optional<OldRef> lookup_registry(const std::string& name, SymbolType want);
    std::optional<OldRef> lookup_public(const std::string& name, SymbolType want);
    bool ensure_compiland_symbols();
    std::vector<const pdb::SymbolView*> static_candidates(const std::string& base, const std::string& encl, bool want_function,
                                                          const std::string& display = {});
    std::optional<OldRef> resolve_name(Sym& sy);  // the undefined-symbol chain

    void decide(Sym& sy, DecisionKind kind, BindSource source, SymbolType type, const TargetAddress& addr, std::string reason);
    void trace(const Sym& sy) const;
    std::uint32_t old_size_for(const TargetAddress& a);
};

// ---- naming of the referencing function --------------------------------------------------

std::string Planner::referencing_function(std::uint32_t section, std::uint32_t offset) const {
    std::uint32_t sec = section;
    for (int hop = 0; hop < 8 && sec >= 1 && sec <= secs_.size(); ++hop) {
        const SecInfo& si = secs_[sec - 1];
        if (si.associative && si.parent != 0 && si.parent != sec) {
            sec = si.parent;
            offset = 0;
            continue;
        }
        break;
    }
    if (sec < 1 || sec > secs_.size()) {
        return {};
    }
    const std::vector<std::uint32_t>& list = sec_syms_[sec - 1];
    const Sym* best = nullptr;
    for (const std::uint32_t pos : list) {
        const Sym& sy = syms_[pos];
        if (sy.s->value > offset) {
            break;
        }
        if (best == nullptr || sy.s->value >= best->s->value) {
            // Prefer a function-typed symbol among those at the same offset.
            if (best != nullptr && sy.s->value == best->s->value && best->dec.type == SymbolType::Function &&
                sy.dec.type != SymbolType::Function) {
                continue;
            }
            best = &sy;
        }
    }
    if (best == nullptr) {
        return std::string(secs_[sec - 1].name);
    }
    return best->name;
}

std::string Planner::referencing_description(std::uint32_t section, std::uint32_t offset) const {
    const std::string fn = referencing_function(section, offset);
    if (fn.empty()) {
        return "section " + std::to_string(section);
    }
    const std::string disp = display_name(fn);
    return disp == fn ? "'" + fn + "'" : "'" + disp + "' ('" + fn + "')";
}

void Planner::trace(const Sym& sy) const {
    CML_LOG(Trace, "bind: sym=%s display=%s type=%s defined=%d static=%d decision=%s source=%s addr=%s reason=%s",
            sy.name.c_str(), sy.dec.display.c_str(), to_string(sy.dec.type), sy.defined ? 1 : 0, sy.is_static ? 1 : 0,
            to_string(sy.dec.decision), to_string(sy.dec.source),
            sy.dec.old_address.valid ? hex(sy.dec.old_address.va).c_str() : "-", sy.dec.reason.c_str());
}

void Planner::decide(Sym& sy, DecisionKind kind, BindSource source, SymbolType type, const TargetAddress& addr,
                     std::string reason) {
    sy.decided = true;
    sy.dec.decision = kind;
    sy.dec.source = source;
    sy.dec.type = type;
    sy.dec.old_address = addr;
    sy.dec.reason = std::move(reason);
    trace(sy);
}

// ---- compiland ------------------------------------------------------------------------------

bool Planner::resolve_compiland() {
    const std::uint32_t count = pdb_.compiland_count();
    if (in_.compiland_index >= 0) {
        if (static_cast<std::uint64_t>(in_.compiland_index) >= count) {
            error(diag::kCompilandNotFound, "compiland index " + std::to_string(in_.compiland_index) + " is out of range in " + in_.module_name,
                  "Pass the index of the compiland that was compiled from the patched source file.");
            return false;
        }
        ci_ = static_cast<std::uint32_t>(in_.compiland_index);
    } else {
        std::vector<std::uint32_t> hits = pdb_.compilands_by_object_name(in_.compiland);
        if (hits.empty()) {
            hits = pdb_.compilands_by_object_stem(in_.compiland);
        }
        if (hits.size() > 1) {
            std::vector<std::uint32_t> plain;
            for (const std::uint32_t h : hits) {
                if (pdb_.compiland(h).library_name.empty()) {
                    plain.push_back(h);
                }
            }
            if (!plain.empty()) {
                hits = plain;
            }
        }
        if (hits.empty()) {
            error(diag::kCompilandNotFound, "no compiland of " + in_.module_name + " matches the translation unit '" + in_.compiland + "'",
                  "Check that the module was built from this source file with debug info (/Zi or /Z7), or hot-restart.");
            return false;
        }
        if (hits.size() > 1) {
            std::string names;
            for (const std::uint32_t h : hits) {
                names += (names.empty() ? "" : ", ");
                names += std::string(pdb_.compiland(h).object_name);
            }
            error(diag::kCompilandAmbiguous, "translation unit '" + in_.compiland + "' matches several compilands of " + in_.module_name + ": " + names,
                  "Pass the full object path or the compiland index.");
            return false;
        }
        ci_ = hits.front();
    }
    plan_.compiland = std::string(pdb_.compiland(ci_).object_name);
    plan_.compiland_index = ci_;
    CML_LOG(Trace, "bind: object=%s module=%s compiland=%u (%s)", in_.object_name.c_str(), in_.module_name.c_str(), ci_,
            plan_.compiland.c_str());
    return true;
}

// ---- sections -------------------------------------------------------------------------------

void Planner::collect_sections() {
    const std::span<const coff::Section> sections = obj_.sections();
    secs_.resize(sections.size());
    sec_syms_.resize(sections.size());
    relocs_.resize(sections.size());
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const coff::Section& sec = sections[i];
        SecInfo& si = secs_[i];
        si.sec = &sec;
        si.name = std::string(sec.name);
        coff::ModelSection ms;
        ms.name = si.name;
        ms.characteristics = sec.characteristics;
        si.linked = coff::is_anchorable(ms);
        const std::optional<coff::ComdatInfo> cd = obj_.comdat(sec);
        if (cd && cd->selection == coff::ComdatSelection::Associative) {
            si.associative = true;
            si.parent = cd->associated_section;
        }
        si.code = sec.is_code() || (sec.characteristics & coff::scn::MemExecute) != 0;
        si.tls = starts_with(si.name, ".tls");
        si.crt_init = starts_with(si.name, ".CRT$XC");
        si.runtime_table = starts_with(si.name, ".pdata") || starts_with(si.name, ".xdata") || starts_with(si.name, ".rtc$") ||
                           starts_with(si.name, ".CRT$") || starts_with(si.name, ".gfids") || starts_with(si.name, ".giats") ||
                           starts_with(si.name, ".gljmp") || starts_with(si.name, ".gehcont") || starts_with(si.name, ".voltbl") ||
                           starts_with(si.name, ".sxdata");
        si.has_locator = si.linked && !si.associative;
    }
}

// ---- symbols --------------------------------------------------------------------------------

void Planner::collect_symbols() {
    pos_of_raw_.assign(obj_.header().symbol_count, kNone32);
    for (const coff::Symbol& s : obj_.symbols()) {
        const std::int32_t sn = s.section_number;
        if (s.storage_class == coff::StorageClass::File || s.storage_class == coff::StorageClass::Section ||
            s.storage_class == coff::StorageClass::Label || s.name.empty()) {
            continue;
        }
        if (sn == coff::kSectionAbsolute || sn == coff::kSectionDebug) {
            continue;
        }
        Sym sy;
        sy.s = &s;
        sy.name = std::string(s.name);
        sy.is_static = s.is_static();
        sy.weak = s.is_weak_external();
        if (sn > 0) {
            if (static_cast<std::size_t>(sn) > secs_.size()) {
                continue;
            }
            if (coff::is_section_definition_symbol(static_cast<std::uint8_t>(s.storage_class), sn, s.aux_count, s.value, s.type)) {
                continue;
            }
            if (s.is_static() && sy.name == secs_[static_cast<std::size_t>(sn) - 1].name) {
                continue;
            }
            if (!s.is_external() && !s.is_static()) {
                continue;
            }
            sy.defined = true;
        } else if (sy.weak) {
            // undefined with a default
        } else if (s.is_external() && s.value != 0) {
            sy.common = true;
            sy.defined = true;
        } else if (s.is_external()) {
            // plain undefined external
        } else {
            continue;  // undefined static (UndefinedStatic): nothing to bind
        }
        sy.relevant = true;
        sy.dec.symbol_index = s.index;
        sy.dec.name = sy.name;
        sy.dec.display = display_name(sy.name);
        sy.dec.defined = sy.defined;
        sy.dec.is_static = sy.is_static;
        sy.dec.section = sn > 0 ? static_cast<std::uint32_t>(sn) : 0;
        if (sn > 0) {
            sy.dec.section_name = secs_[static_cast<std::size_t>(sn) - 1].name;
        }
        sy.dec.value = s.value;
        if (s.index < pos_of_raw_.size()) {
            pos_of_raw_[s.index] = static_cast<std::uint32_t>(syms_.size());
        }
        syms_.push_back(std::move(sy));
    }
    // Defined symbols per section, ordered by value.
    for (std::size_t pos = 0; pos < syms_.size(); ++pos) {
        const Sym& sy = syms_[pos];
        if (sy.defined && sy.dec.section != 0) {
            sec_syms_[sy.dec.section - 1].push_back(static_cast<std::uint32_t>(pos));
        }
    }
    for (std::vector<std::uint32_t>& list : sec_syms_) {
        std::stable_sort(list.begin(), list.end(),
                         [this](std::uint32_t a, std::uint32_t b) { return syms_[a].s->value < syms_[b].s->value; });
    }
}

void Planner::load_relocations() {
    for (std::size_t i = 0; i < secs_.size(); ++i) {
        if (!secs_[i].linked) {
            continue;
        }
        Result<std::vector<coff::ResolvedRelocation>> rr = obj_.resolve_relocations(*secs_[i].sec);
        if (!rr) {
            fatal(rr.error());
            return;
        }
        relocs_[i] = std::move(*rr);
    }
}

// ---- lookups --------------------------------------------------------------------------------

std::optional<OldRef> Planner::lookup_registry(const std::string& name, SymbolType want) {
    if (in_.registry == nullptr) {
        return std::nullopt;
    }
    const std::optional<SymbolAddress> a = in_.registry->find(name);
    if (!a) {
        return std::nullopt;
    }
    const SymbolType got = a->is_function ? SymbolType::Function : SymbolType::Data;
    if (got != want) {
        CML_LOG(Trace, "bind: registry has '%s' as %s, expected %s: ignored", name.c_str(), to_string(got), to_string(want));
        return std::nullopt;
    }
    OldRef r;
    r.addr = process_address(*a);
    r.source = BindSource::Registry;
    r.type = got;
    r.note = "registered by " + a->origin;
    return r;
}

std::optional<OldRef> Planner::lookup_public(const std::string& name, SymbolType want) {
    Result<std::vector<pdb::SymbolView>> pubs = pdb_.find_public(name);
    if (!pubs) {
        fatal(pubs.error());
        return std::nullopt;
    }
    for (const pdb::SymbolView& v : *pubs) {
        if (v.rva == pdb::kNoRva) {
            continue;
        }
        const SymbolType got = pdb_is_function(v) ? SymbolType::Function : SymbolType::Data;
        if (got != want) {
            continue;
        }
        OldRef r;
        r.addr = module_address(v.rva);
        r.source = BindSource::PdbPublic;
        r.type = got;
        r.note = "public at rva " + hex(v.rva);
        return r;
    }
    return std::nullopt;
}

bool Planner::ensure_compiland_symbols() {
    if (comp_loaded_) {
        return true;
    }
    Result<std::vector<pdb::SymbolView>> syms = pdb_.compiland_symbols(ci_);
    if (!syms) {
        fatal(syms.error());
        return false;
    }
    comp_syms_ = std::move(*syms);
    comp_loaded_ = true;
    return true;
}

std::vector<const pdb::SymbolView*> Planner::static_candidates(const std::string& base, const std::string& encl, bool want_function,
                                                                const std::string& display) {
    std::vector<const pdb::SymbolView*> out;
    if (base.empty() || !ensure_compiland_symbols()) {
        return out;
    }
    std::vector<const pdb::SymbolView*> all;
    for (const pdb::SymbolView& v : comp_syms_) {
        if (v.rva == pdb::kNoRva) {
            continue;
        }
        const bool is_func = pdb::is_proc_kind(v.kind);
        const bool is_data = pdb::is_data_kind(v.kind);
        if (want_function ? !is_func : !is_data) {
            continue;
        }
        if (last_component(v.name) != base) {
            continue;
        }
        all.push_back(&v);
    }
    // The qualified name, when both sides spell it the same, is a much stronger key than the last
    // component: "lambdas_init::<lambda_0>::operator()" (clang) or "Ns::Cls::f".
    if (!display.empty()) {
        const std::string canonical_display = canonical_lambda_names(display);
        std::vector<const pdb::SymbolView*> exact;
        for (const pdb::SymbolView* v : all) {
            std::string n(v->name);
            const std::string_view anon = "`anonymous namespace'";
            for (std::size_t at = n.find(anon); at != std::string::npos; at = n.find(anon, at)) {
                n.replace(at, anon.size(), "(anonymous namespace)");
            }
            // cl spells the scope of a local entity with quotes and block numbers:
            // BACKTICK f QUOTE :: BACKTICK 2 QUOTE :: <lambda_1>::operator()  (as the CI evidence shows).
            for (std::size_t at = n.find('`'); at != std::string::npos; at = n.find('`', at)) {
                const std::size_t close = n.find('\'', at + 1);
                if (close == std::string::npos) {
                    break;
                }
                const std::string inner = n.substr(at + 1, close - at - 1);
                const bool digits = !inner.empty() && inner.find_first_not_of("0123456789") == std::string::npos;
                if (digits) {
                    std::size_t end = close + 1;
                    if (n.compare(end, 2, "::") == 0) {
                        end += 2;
                    }
                    n.erase(at, end - at);
                } else {
                    n.replace(at, close + 1 - at, inner);
                }
            }
            // Older spelling: "f::__l2::<lambda_1>::operator()".
            for (std::size_t at = n.find("::__l"); at != std::string::npos; at = n.find("::__l", at)) {
                std::size_t e = at + 5;
                while (e < n.size() && n[e] >= '0' && n[e] <= '9') {
                    ++e;
                }
                if (e > at + 5 && n.compare(e, 2, "::") == 0) {
                    n.erase(at, e - at);
                } else {
                    ++at;
                }
            }
            if (canonical_lambda_names(n) == canonical_display) {
                exact.push_back(v);
            }
        }
        if (!exact.empty()) {
            std::sort(exact.begin(), exact.end(), [](const pdb::SymbolView* a, const pdb::SymbolView* b) { return a->rva < b->rva; });
            return exact;
        }
    }
    if (want_function) {
        // Member functions of local classes (lambdas) are top-level procs in the PDB; the function scope
        // in the decorated name only matters for data.
        out = all;
    } else if (!encl.empty()) {
        for (const pdb::SymbolView* v : all) {
            if (v->parent_proc != pdb::kNoSymbol && last_component(v->parent_name) == encl) {
                out.push_back(v);
            }
        }
        if (out.empty()) {
            for (const pdb::SymbolView* v : all) {
                if (v->parent_proc != pdb::kNoSymbol) {
                    out.push_back(v);
                }
            }
        }
    } else {
        for (const pdb::SymbolView* v : all) {
            if (v->parent_proc == pdb::kNoSymbol) {
                out.push_back(v);
            }
        }
        if (out.empty()) {
            out = all;
        }
    }
    std::sort(out.begin(), out.end(), [](const pdb::SymbolView* a, const pdb::SymbolView* b) { return a->rva < b->rva; });
    return out;
}

std::uint32_t Planner::old_size_for(const TargetAddress& a) {
    if (!a.valid || a.rva == kNoRva) {
        return 0;
    }
    Result<std::optional<pdb::SymbolView>> p = pdb_.find_proc_by_rva(a.rva);
    if (p && p->has_value() && (*p)->rva == a.rva) {
        return (*p)->size;
    }
    return 0;
}

// ---- defined symbols --------------------------------------------------------------------------

void Planner::decide_defined() {
    for (Sym& sy : syms_) {
        if (!sy.defined || sy.decided) {
            continue;
        }
        const coff::Symbol& s = *sy.s;
        const SecInfo* si = sy.dec.section != 0 ? &secs_[sy.dec.section - 1] : nullptr;
        const SymbolType type = (si != nullptr && si->code) ? SymbolType::Function : SymbolType::Data;
        sy.dec.type = type;

        if (si != nullptr && si->tls) {
            error(diag::kThreadLocal, "'" + sy.dec.display + "' is thread_local data defined by " + in_.object_name + "; thread-local variables in patched code are not supported",
                  "Move the thread_local to an unchanged source file, or hot-restart.");
            decide(sy, DecisionKind::PatchFresh, BindSource::None, type, {}, "thread_local (CML3102)");
            continue;
        }
        // Fresh copies (plan 4.5): vtables, RTTI, constants, unwind tables, initialiser stubs, funclets.
        if (type == SymbolType::Data && (is_fresh_data_name(sy.name) || sy.name.find("$rtc") != std::string::npos ||
                                         (si != nullptr && si->runtime_table))) {
            decide(sy, DecisionKind::PatchFresh, BindSource::None, type, {}, "vtable, RTTI, constant or runtime table: a fresh copy is correct");
            continue;
        }
        if (type == SymbolType::Function || (si != nullptr && si->code)) {
            if (is_dynamic_initializer(sy.name) || is_tu_wide_initializer(sy.name) || is_atexit_destructor(sy.name)) {
                decide(sy, DecisionKind::PatchFresh, BindSource::None, type, {}, "dynamic initialiser or atexit stub: run through the initialiser list, never hooked");
                continue;
            }
            if (is_funclet(sy.name)) {
                decide(sy, DecisionKind::PatchFresh, BindSource::None, type, {}, "EH funclet: reached only through the patch's own unwind data");
                continue;
            }
            if (!sy.name.empty() && sy.name[0] == '$') {
                decide(sy, DecisionKind::PatchFresh, BindSource::None, type, {}, "compiler-generated local label");
                continue;
            }
        }
        if (type == SymbolType::Data && si != nullptr && si->crt_init) {
            decide(sy, DecisionKind::PatchFresh, BindSource::None, type, {}, "initialiser table");
            continue;
        }

        if (s.is_static()) {
            continue;  // matched in bridge_statics()
        }

        // External (or COMMON): registry, then the module's publics.
        std::optional<OldRef> old = lookup_registry(sy.name, type);
        if (!old) {
            old = lookup_public(sy.name, type);
        }
        if (type == SymbolType::Function) {
            if (old) {
                decide(sy, DecisionKind::PatchHooked, old->source, type, old->addr, "defined function present in the old image (" + old->note + "): old entry is hooked to the patch definition");
            } else {
                decide(sy, DecisionKind::PatchNewFunction, BindSource::None, type, {}, "defined function not in the old image: new canonical symbol, no hook");
            }
        } else {
            if (old) {
                decide(sy, DecisionKind::BindOldData, old->source, type, old->addr, "defined data present in the old image (" + old->note + "): references are retargeted, the patch's copy is dead");
            } else {
                decide(sy, DecisionKind::PatchNewData, BindSource::None, type, {}, "defined data not in the old image: patch-local, its initialiser runs once");
            }
        }
    }
}

void Planner::bridge_statics() {
    // ADR-0010: statics have no S_PUB32. Bridge the object's decorated name to the compiland's
    // S_LPROC32 / S_LDATA32 records by (name, compiland); ties by position and by RVA.
    struct Group {
        bool is_func = false;
        std::string base;
        std::string encl;
        std::string display;
        std::vector<std::uint32_t> members;  // positions in syms_
    };
    std::map<std::string, Group> groups;
    for (std::size_t pos = 0; pos < syms_.size(); ++pos) {
        Sym& sy = syms_[pos];
        if (!sy.defined || sy.decided || !sy.is_static) {
            continue;
        }
        const bool is_func = sy.dec.type == SymbolType::Function;
        if (is_func && starts_with(sy.name, "??$")) {
            // A static function template instantiation (typically over a local/lambda type): same base
            // name as unrelated instantiations, and the PDB spells the arguments differently. Not bridged.
            decide(sy, DecisionKind::PatchNewFunction, BindSource::None, sy.dec.type, {},
                   "static function template instantiation: not matched to the old image, not hooked");
            continue;
        }
        const std::string base = base_identifier(sy.name);
        if (base.empty()) {
            // Nothing to match by: a static without a source-level name (compiler temporary).
            decide(sy, is_func ? DecisionKind::PatchNewFunction : DecisionKind::PatchNewData, BindSource::None, sy.dec.type, {},
                   "static without a source-level name: treated as new");
            continue;
        }
        const std::string encl = enclosing_function(sy.name);
        const std::string key = std::string(is_func ? "F|" : "D|") + canonical_lambda_names(sy.dec.display) + "|" + base + "|" + encl;
        Group& g = groups[key];
        g.is_func = is_func;
        g.base = base;
        g.encl = encl;
        g.display = sy.dec.display == sy.name ? std::string() : sy.dec.display;
        g.members.push_back(static_cast<std::uint32_t>(pos));
    }
    for (auto& [key, g] : groups) {
        std::stable_sort(g.members.begin(), g.members.end(), [this](std::uint32_t a, std::uint32_t b) {
            const Sym& x = syms_[a];
            const Sym& y = syms_[b];
            if (x.dec.section != y.dec.section) {
                return x.dec.section < y.dec.section;
            }
            return x.s->value < y.s->value;
        });
    }

    struct Match {
        const pdb::SymbolView* view = nullptr;
        BindSource how = BindSource::None;
    };
    std::map<std::uint32_t, Match> matched;  // pos -> match
    std::set<std::string> deferred;          // group keys that need pass B

    // Pass A: unambiguous name matches (also learns the section deltas).
    for (auto& [key, g] : groups) {
        const std::vector<const pdb::SymbolView*> cands = static_candidates(g.base, g.encl, g.is_func, g.display);
        if (cands.empty()) {
            for (const std::uint32_t pos : g.members) {
                Sym& sy = syms_[pos];
                if (!g.is_func && is_guard_variable(sy.name)) {
                    continue;  // compilers without a PDB record for the guard (clang): pass C
                }
                decide(sy, g.is_func ? DecisionKind::PatchNewFunction : DecisionKind::PatchNewData, BindSource::None, sy.dec.type, {},
                       std::string("static ") + (g.is_func ? "function" : "data") + " '" + g.base + "' has no record in compiland " + std::to_string(ci_) + ": new");
            }
            continue;
        }
        if (g.members.size() == 1 && cands.size() == 1) {
            matched[g.members.front()] = {cands.front(), BindSource::CompilandStatic};
            const Sym& sy = syms_[g.members.front()];
            if (!g.is_func && sy.dec.section != 0 && !secs_[sy.dec.section - 1].sec->is_comdat()) {
                const std::int64_t d = static_cast<std::int64_t>(cands.front()->rva) - static_cast<std::int64_t>(sy.s->value);
                const auto it = delta_.find(sy.dec.section);
                if (it == delta_.end()) {
                    delta_[sy.dec.section] = d;
                } else if (it->second != d) {
                    delta_conflict_.insert(sy.dec.section);
                }
            }
            continue;
        }
        deferred.insert(key);
    }
    for (const std::uint32_t sec : delta_conflict_) {
        delta_.erase(sec);
    }

    // Pass B: ambiguous groups, by RVA in the unchanged section layout, then by position.
    for (const std::string& key : deferred) {
        Group& g = groups[key];
        const std::vector<const pdb::SymbolView*> cands = static_candidates(g.base, g.encl, g.is_func, g.display);
        std::vector<std::uint32_t> unmatched = g.members;
        std::vector<const pdb::SymbolView*> free_cands = cands;
        if (!g.is_func) {
            for (auto it = unmatched.begin(); it != unmatched.end();) {
                const Sym& sy = syms_[*it];
                const auto d = delta_.find(sy.dec.section);
                bool taken = false;
                if (d != delta_.end()) {
                    const std::int64_t want = d->second + static_cast<std::int64_t>(sy.s->value);
                    for (auto c = free_cands.begin(); c != free_cands.end(); ++c) {
                        if (static_cast<std::int64_t>((*c)->rva) == want) {
                            matched[*it] = {*c, BindSource::CompilandRva};
                            free_cands.erase(c);
                            taken = true;
                            break;
                        }
                    }
                }
                it = taken ? unmatched.erase(it) : it + 1;
            }
        }
        if (unmatched.empty()) {
            continue;
        }
        if (unmatched.size() == free_cands.size()) {
            for (std::size_t k = 0; k < unmatched.size(); ++k) {
                matched[unmatched[k]] = {free_cands[k], BindSource::CompilandOrdinal};
            }
            continue;
        }
        for (const std::uint32_t pos : unmatched) {
            Sym& sy = syms_[pos];
            if (g.is_func) {
                warn(diag::kAmbiguousStaticFunction,
                     "static function '" + sy.dec.display + "' (" + sy.name + ") matches " + std::to_string(cands.size()) + " records named '" + g.base +
                         "' in compiland " + std::to_string(ci_) + " but the patch object defines " + std::to_string(g.members.size()) + "; it is not hooked",
                     "Rename the overloads or keep their number unchanged; callers outside the patch keep the old code of this function.");
                decide(sy, DecisionKind::PatchNewFunction, BindSource::None, sy.dec.type, {}, "ambiguous static function (CML3213): not hooked");
                continue;
            }
            error(diag::kAmbiguousStatic,
                  "static " + std::string(g.is_func ? "function" : "data") + " '" + sy.dec.display + "' (" + sy.name + ") matches " + std::to_string(cands.size()) +
                      " records named '" + g.base + "' in compiland " + std::to_string(ci_) + " but the patch object defines " + std::to_string(g.members.size()) + " of them",
                  "Rename one of the statics, keep their number unchanged, or hot-restart.");
            decide(sy, g.is_func ? DecisionKind::PatchNewFunction : DecisionKind::PatchNewData, BindSource::None, sy.dec.type, {}, "ambiguous static (CML3207)");
        }
    }

    // Decide the matched ones.
    for (const auto& [pos, m] : matched) {
        Sym& sy = syms_[pos];
        const TargetAddress addr = module_address(m.view->rva);
        const std::string how = std::string("matched record '") + std::string(m.view->name) + "' at rva " + hex(m.view->rva) + " in compiland " + std::to_string(ci_) +
                                " (" + to_string(m.how) + ")";
        if (sy.dec.type == SymbolType::Function) {
            decide(sy, DecisionKind::PatchHooked, m.how, SymbolType::Function, addr, "static function present in the old compiland: " + how);
        } else {
            decide(sy, DecisionKind::BindOldData, m.how, SymbolType::Data, addr, "static data present in the old compiland: " + how);
        }
    }

    // Pass C: compiler-generated guard variables (no PDB record) from the unchanged section layout.
    for (Sym& sy : syms_) {
        if (!sy.defined || sy.decided || !sy.is_static || sy.dec.type != SymbolType::Data || !is_guard_variable(sy.name)) {
            continue;
        }
        const auto d = delta_.find(sy.dec.section);
        if (d != delta_.end() && sy.dec.section != 0) {
            const std::int64_t rva = d->second + static_cast<std::int64_t>(sy.s->value);
            if (rva >= 0 && rva < 0x7FFFFFFF) {
                const std::optional<std::uint32_t> owner = pdb_.compiland_for_rva(static_cast<std::uint32_t>(rva));
                if (owner && *owner == ci_) {
                    decide(sy, DecisionKind::BindOldData, BindSource::CompilandLayout, SymbolType::Data, module_address(static_cast<std::uint32_t>(rva)),
                           "guard variable of a function-local static: inferred at rva " + hex(static_cast<std::uint64_t>(rva)) +
                               " from the unchanged layout of object section " + std::to_string(sy.dec.section) + " (every matched static there agrees)");
                    continue;
                }
            }
        }
        warn(diag::kGuardUnmatched, "guard variable '" + sy.dec.display + "' of a function-local static was not found in the old image; the patch gets a fresh guard",
             "Keep the layout of the statics in this source file unchanged, or hot-restart if the static must not be initialised again.");
        decide(sy, DecisionKind::PatchNewData, BindSource::None, SymbolType::Data, {}, "guard variable not found in the old image (CML3211): new");
    }
    // Anything still undecided (defensive).
    for (Sym& sy : syms_) {
        if (sy.defined && !sy.decided) {
            decide(sy, sy.dec.type == SymbolType::Function ? DecisionKind::PatchNewFunction : DecisionKind::PatchNewData, BindSource::None, sy.dec.type, {}, "unmatched static: new");
        }
    }
}

// ---- undefined symbols ------------------------------------------------------------------------

std::optional<OldRef> Planner::resolve_name(Sym& sy) {
    const std::string& name = sy.name;
    // (1) process symbol registry (functions/data introduced by earlier patches)
    if (in_.registry != nullptr) {
        if (const std::optional<SymbolAddress> a = in_.registry->find(name)) {
            OldRef r;
            r.addr = process_address(*a);
            r.source = BindSource::Registry;
            r.type = a->is_function ? SymbolType::Function : SymbolType::Data;
            r.note = "registered by " + a->origin;
            return r;
        }
    }
    // (2) the compiland's own statics: only names that can denote a TU-private entity
    if (is_tu_private_name(name)) {
        const std::string base = base_identifier(name);
        const std::string encl = enclosing_function(name);
        for (const bool want_function : {false, true}) {
            const std::vector<const pdb::SymbolView*> cands = static_candidates(base, encl, want_function, sy.dec.display == name ? std::string() : sy.dec.display);
            if (cands.size() == 1) {
                OldRef r;
                r.addr = module_address(cands.front()->rva);
                r.source = BindSource::CompilandStatic;
                r.type = want_function ? SymbolType::Function : SymbolType::Data;
                r.note = "compiland " + std::to_string(ci_) + " record '" + std::string(cands.front()->name) + "'";
                return r;
            }
        }
    }
    if (fatal_) {
        return std::nullopt;
    }
    // (3) PDB publics, then global data
    {
        Result<std::vector<pdb::SymbolView>> pubs = pdb_.find_public(name);
        if (!pubs) {
            fatal(pubs.error());
            return std::nullopt;
        }
        for (const pdb::SymbolView& v : *pubs) {
            if (v.rva == pdb::kNoRva) {
                continue;
            }
            OldRef r;
            r.addr = module_address(v.rva);
            r.type = pdb_is_function(v) ? SymbolType::Function : SymbolType::Data;
            r.source = (r.type == SymbolType::Data && starts_with(name, "__imp_")) ? BindSource::ImportSlotPdb : BindSource::PdbPublic;
            r.note = "public at rva " + hex(v.rva);
            return r;
        }
        if (!name.empty() && name[0] != '?') {
            Result<std::vector<pdb::SymbolView>> globals = pdb_.find_global_data(name);
            if (!globals) {
                fatal(globals.error());
                return std::nullopt;
            }
            for (const pdb::SymbolView& v : *globals) {
                if (v.rva == pdb::kNoRva) {
                    continue;
                }
                OldRef r;
                r.addr = module_address(v.rva);
                r.type = SymbolType::Data;
                r.source = BindSource::PdbGlobal;
                r.note = "global data at rva " + hex(v.rva);
                return r;
            }
        }
    }
    // (4) __imp_X: the module's IAT slot, by parsing its import directory
    const bool is_imp = starts_with(name, "__imp_") && name.size() > 6;
    if (is_imp && in_.modules != nullptr) {
        if (const std::optional<SymbolAddress> a = in_.modules->iat_slot(in_.module_name, std::string_view(name).substr(6))) {
            OldRef r;
            r.addr = process_address(*a);
            r.addr.module = in_.module_name;
            r.source = BindSource::ImportSlotModule;
            r.type = SymbolType::Data;
            r.note = "IAT slot of " + std::string(name.substr(6)) + " in " + in_.module_name;
            return r;
        }
    }
    // (5) exports of other loaded modules
    if (in_.modules != nullptr) {
        if (const std::optional<SymbolAddress> a = in_.modules->find_export(name)) {
            OldRef r;
            r.addr = process_address(*a);
            r.source = BindSource::ExportOtherModule;
            r.type = a->is_function ? SymbolType::Function : SymbolType::Data;
            r.note = "export of " + a->origin;
            return r;
        }
        if (is_imp) {
            if (const std::optional<SymbolAddress> a = in_.modules->find_export(std::string_view(name).substr(6))) {
                OldRef r;
                r.addr = process_address(*a);
                r.source = BindSource::SynthesizedImportSlot;
                r.type = SymbolType::Data;
                r.note = "export " + std::string(name.substr(6)) + " of " + a->origin + " (module does not import it)";
                return r;
            }
        }
    }
    return std::nullopt;
}

void Planner::resolve_undefined() {
    // Which undefined symbols does the patch actually use, and where first?
    std::vector<bool> referenced(syms_.size(), false);
    for (std::size_t i = 0; i < relocs_.size(); ++i) {
        for (const coff::ResolvedRelocation& rr : relocs_[i]) {
            if (rr.target->index >= pos_of_raw_.size()) {
                continue;
            }
            const std::uint32_t pos = pos_of_raw_[rr.target->index];
            if (pos == kNone32) {
                continue;
            }
            Sym& sy = syms_[pos];
            if (!referenced[pos]) {
                referenced[pos] = true;
                sy.dec.first_ref_section = static_cast<std::uint32_t>(i + 1);
                sy.dec.first_ref_offset = rr.reloc.virtual_address;
            }
        }
    }
    for (std::size_t pos = 0; pos < syms_.size(); ++pos) {
        Sym& sy = syms_[pos];
        if (sy.defined) {
            continue;
        }
        if (sy.name == "__ImageBase") {
            decide(sy, DecisionKind::LinkerProvided, BindSource::None, SymbolType::Data, {}, "defined by the linker in the patch image");
            continue;
        }
        if (!referenced[pos]) {
            sy.decided = false;  // never used by linked code: no decision needed
            continue;
        }
        std::optional<OldRef> r = resolve_name(sy);
        if (fatal_) {
            return;
        }
        if (!r) {
            if (sy.weak) {
                decide(sy, DecisionKind::WeakDefault, BindSource::None, SymbolType::Data, {}, "weak external found nowhere: the default symbol of the object is used");
                continue;
            }
            const std::string where = referencing_description(sy.dec.first_ref_section, sy.dec.first_ref_offset);
            error(diag::kUnresolved,
                  "unresolved external symbol " + describe(sy) + " referenced from " + where + " in " + in_.object_name,
                  "Define it in a module of the process (or link the library that provides it into the patched module), or hot-restart.");
            decide(sy, DecisionKind::Unresolved, BindSource::None, SymbolType::Function, {}, "not found in the registry, the module's PDB, its imports or other modules' exports");
            continue;
        }
        if (r->type == SymbolType::Function) {
            decide(sy, DecisionKind::BindOldFunction, r->source, SymbolType::Function, r->addr, "undefined function resolved (" + r->note + "): thunk and slot");
        } else {
            decide(sy, DecisionKind::BindOldData, r->source, SymbolType::Data, r->addr,
                   r->source == BindSource::SynthesizedImportSlot
                       ? "undefined import slot resolved (" + r->note + "): placeholder filled with the export's address"
                       : "undefined data resolved (" + r->note + "): placeholder, references retargeted");
        }
    }
}

// ---- fix-ups ----------------------------------------------------------------------------------

namespace {
FixupKind rel32_kind(coff::RelocType t) {
    return static_cast<FixupKind>(static_cast<unsigned>(t) - static_cast<unsigned>(coff::RelocType::Rel32));
}
}  // namespace

void Planner::build_fixups() {
    std::set<std::uint32_t> warned_sections;
    std::set<std::uint32_t> bound_sections;
    std::set<std::uint32_t> locator_set;
    std::set<std::uint32_t> associative_set;
    for (const Sym& sy : syms_) {
        if (sy.defined && sy.decided && sy.dec.decision == DecisionKind::BindOldData && sy.dec.section != 0) {
            bound_sections.insert(sy.dec.section);
        }
    }
    for (std::size_t i = 0; i < relocs_.size(); ++i) {
        const SecInfo& si = secs_[i];
        const std::uint32_t sec_index = static_cast<std::uint32_t>(i + 1);
        for (const coff::ResolvedRelocation& rr : relocs_[i]) {
            const coff::Symbol& t = *rr.target;
            const std::uint32_t pos = t.index < pos_of_raw_.size() ? pos_of_raw_[t.index] : kNone32;
            if (pos == kNone32) {
                // A section symbol (or other skipped symbol): flag references into sections that hold bound data.
                if (t.section_number > 0 && bound_sections.count(static_cast<std::uint32_t>(t.section_number)) != 0 &&
                    warned_sections.insert(static_cast<std::uint32_t>(t.section_number)).second) {
                    warn(diag::kSectionRelative,
                         "object section " + std::to_string(t.section_number) + " ('" + secs_[static_cast<std::size_t>(t.section_number) - 1].name +
                             "') holds data bound to the old image and is also referenced through its section symbol (first from " +
                             referencing_description(sec_index, rr.reloc.virtual_address) + "); those references stay on the patch's dead copy",
                         "Give the data external linkage or avoid private (section-relative) references to it.");
                }
                continue;
            }
            const Sym& ts = syms_[pos];
            if (!ts.decided) {
                continue;
            }
            const DecisionKind dk = ts.dec.decision;
            if (dk != DecisionKind::BindOldData && dk != DecisionKind::BindOldFunction) {
                continue;
            }
            const coff::RelocType type = rr.reloc.type;
            const std::uint32_t off = rr.reloc.virtual_address;
            const auto where = [&]() { return referencing_description(sec_index, off); };
            if (type == coff::RelocType::Absolute) {
                continue;
            }
            if (type == coff::RelocType::Section || type == coff::RelocType::SecRel7) {
                error(diag::kThreadLocal, "section-index reference (" + std::string(coff::reloc_type_name(type)) + ") to " + describe(ts) + " from " + where() + " in " + in_.object_name,
                      "Move the thread_local to an unchanged source file, or hot-restart.");
                continue;
            }
            const bool secrel = type == coff::RelocType::SecRel;
            std::uint32_t section_offset = 0;
            if (secrel) {
                // The offset of the target inside ITS section of the old image (what the linker would
                // write): the CRT's _Init_thread_epoch is read like this by every thread-safe static.
                const std::optional<pdb::SectionOffset> so =
                    ts.dec.old_address.rva != kNoRva ? pdb_.section_offset_from_rva(ts.dec.old_address.rva) : std::nullopt;
                if (dk != DecisionKind::BindOldData || !so) {
                    error(diag::kThreadLocal,
                          "thread-local reference to " + describe(ts) + " from " + where() + " in " + in_.object_name +
                              ", which is not data of the patched module; it cannot be retargeted",
                          "Move the thread_local to an unchanged source file, or hot-restart.");
                    continue;
                }
                section_offset = so->offset;
            }
            if (type == coff::RelocType::Addr32Nb) {
                if (dk == DecisionKind::BindOldData) {
                    error(diag::kAddr32NbToOldData,
                          where() + " references " + describe(ts) + " in " + ts.dec.old_address.module + " image-relatively (ADDR32NB); this cannot be retargeted",
                          "Move the reference behind a function or pointer, or hot-restart.");
                }
                continue;  // to a thunk: legal, resolved by the linker
            }
            const bool rel = coff::is_rel32_family(type);
            if (!rel && !secrel && type != coff::RelocType::Addr64) {
                error(diag::kUnsupportedReloc,
                      "unsupported relocation " + std::string(coff::reloc_type_name(type)) + " against " + describe(ts) + " from " + where() + " in " + in_.object_name,
                      "Compile with /MD-compatible 64-bit code models (no 32-bit absolute addresses), or hot-restart.");
                continue;
            }
            if (dk == DecisionKind::BindOldFunction && rel) {
                continue;  // call/jump through the binding object's thunk
            }
            if (ts.dec.source == BindSource::SynthesizedImportSlot) {
                continue;  // the placeholder itself holds the address
            }
            const SecInfo& site = si;
            if (!site.linked) {
                error(diag::kNoLocator,
                      "reference to " + describe(ts) + " from " + where() + " in " + in_.object_name + " sits in section '" + site.name +
                          "' which is not linked into the image; it cannot be retargeted",
                      "Hot-restart; this is not expected for a compiler-generated object.");
                continue;
            }
            // Sections with fix-ups need a locator; associative ones only get one on request (ADR-0024
            // amended: cl's `.text$x` funclets reference bound statics).
            locator_set.insert(sec_index);
            if (site.associative) {
                associative_set.insert(sec_index);
            }
            // In-place addend (ADR-0025).
            const std::size_t width = (rel || secrel) ? 4u : 8u;
            const coff::ByteSpan data = site.sec->data;
            if (static_cast<std::uint64_t>(off) + width > data.size()) {
                error(diag::kBadRelocationSite,
                      "relocation at offset " + hex(off) + " of section '" + site.name + "' in " + in_.object_name + " lies outside the section data",
                      "Rebuild the object with the compiler; the file is damaged.");
                continue;
            }
            std::int64_t addend = 0;
            if (rel || secrel) {
                std::int32_t a32 = 0;
                std::memcpy(&a32, data.data() + off, 4);
                addend = a32;
            } else {
                std::int64_t a64 = 0;
                std::memcpy(&a64, data.data() + off, 8);
                addend = a64;
            }
            Fixup f;
            f.section_index = sec_index;
            f.offset = off;
            f.kind = rel ? rel32_kind(type) : (secrel ? FixupKind::SecRel32 : FixupKind::Addr64);
            f.target_section_offset = section_offset;
            f.addend = addend;
            f.target_va = ts.dec.old_address.va;
            f.target_rva = ts.dec.old_address.rva;
            f.symbol = ts.name;
            f.section_name = site.name;
            CML_LOG(Trace, "bind: fixup sec=%u(%s) off=0x%x kind=%s target=%s S=0x%llx A=%lld", sec_index, site.name.c_str(), off,
                    to_string(f.kind), ts.name.c_str(), static_cast<unsigned long long>(f.target_va), static_cast<long long>(addend));
            plan_.fixups.push_back(std::move(f));
        }
    }
    std::stable_sort(plan_.fixups.begin(), plan_.fixups.end(), [](const Fixup& a, const Fixup& b) {
        return a.section_index != b.section_index ? a.section_index < b.section_index : a.offset < b.offset;
    });
    plan_.locator_sections.assign(locator_set.begin(), locator_set.end());
    plan_.associative_locator_sections.assign(associative_set.begin(), associative_set.end());
}

// ---- initialisers -----------------------------------------------------------------------------

void Planner::find_initialisers() {
    bool any_new_data = false;
    bool any_bound_defined_data = false;
    for (const Sym& sy : syms_) {
        if (sy.defined && sy.decided && sy.dec.decision == DecisionKind::PatchNewData) {
            any_new_data = true;
        }
        // Only writable data can hold state a re-run initialiser would clobber (inline constants in
        // .rdata have no dynamic initialisation).
        if (sy.defined && sy.decided && sy.dec.decision == DecisionKind::BindOldData && sy.dec.section != 0 &&
            (secs_[sy.dec.section - 1].sec->characteristics & coff::scn::MemWrite) != 0 && !secs_[sy.dec.section - 1].sec->is_comdat()) {
            any_bound_defined_data = true;
        }
    }
    for (std::size_t i = 0; i < secs_.size(); ++i) {
        const SecInfo& si = secs_[i];
        if (!si.crt_init) {
            continue;
        }
        for (const coff::ResolvedRelocation& rr : relocs_[i]) {
            if (rr.reloc.type != coff::RelocType::Addr64) {
                continue;
            }
            const std::uint32_t pos = rr.target->index < pos_of_raw_.size() ? pos_of_raw_[rr.target->index] : kNone32;
            if (pos == kNone32) {
                continue;
            }
            const Sym& fn = syms_[pos];
            if (!fn.defined) {
                continue;
            }
            InitialiserEntry e;
            e.function = fn.name;
            e.display = fn.dec.display;
            e.table_section_name = si.name;
            e.table_section = static_cast<std::uint32_t>(i + 1);
            e.table_offset = rr.reloc.virtual_address;
            if (is_tu_wide_initializer(fn.name)) {
                e.tu_wide = true;
                if (!any_new_data) {
                    CML_LOG(Trace, "bind: initialiser %s skipped: the TU defines no new global", fn.name.c_str());
                    continue;
                }
                for (const Sym& g : syms_) {
                    if (g.defined && g.decided && g.dec.decision == DecisionKind::PatchNewData) {
                        e.globals.push_back(g.name);
                    }
                }
                if (any_bound_defined_data) {
                    warn(diag::kSharedInitialiser,
                         "initialiser '" + fn.name + "' initialises every global of " + in_.object_name + ", including ones bound to the old image; running it may re-initialise them",
                         "Put new globals with dynamic initialisation into a separate source file, or hot-restart.");
                }
            } else if (starts_with(fn.name, "??__E")) {
                const std::string prefix = initializer_target_prefix(fn.name);
                bool found = false;
                for (const Sym& g : syms_) {
                    if (!g.defined || !g.decided) {
                        continue;
                    }
                    const bool match = !prefix.empty() && (prefix[0] == '?' ? g.name == prefix
                                                                           : (g.name.size() > 1 && std::string_view(g.name).substr(1).substr(0, prefix.size()) == prefix));
                    if (!match) {
                        continue;
                    }
                    found = true;
                    if (g.dec.decision == DecisionKind::PatchNewData) {
                        e.globals.push_back(g.name);
                    } else {
                        CML_LOG(Trace, "bind: initialiser %s skipped: global %s is bound to the old image", fn.name.c_str(), g.name.c_str());
                    }
                }
                if (e.globals.empty()) {
                    if (!found) {
                        CML_LOG(Trace, "bind: initialiser %s skipped: no global of the TU matches it", fn.name.c_str());
                    }
                    continue;
                }
            } else {
                CML_LOG(Trace, "bind: table entry %s in %s is not a recognised dynamic initialiser: not run", fn.name.c_str(), si.name.c_str());
                continue;
            }
            CML_LOG(Trace, "bind: initialiser %s (table %s+0x%x, %zu globals)", e.function.c_str(), si.name.c_str(), e.table_offset, e.globals.size());
            plan_.initialisers.push_back(std::move(e));
        }
    }
    // Run order: by table section name (.CRT$XCA < .CRT$XCT < .CRT$XCU ...), then table offset.
    std::stable_sort(plan_.initialisers.begin(), plan_.initialisers.end(), [](const InitialiserEntry& a, const InitialiserEntry& b) {
        if (a.table_section_name != b.table_section_name) {
            return a.table_section_name < b.table_section_name;
        }
        return a.table_offset < b.table_offset;
    });
}

// ---- assembly ---------------------------------------------------------------------------------

void Planner::assemble() {
    std::set<std::string> thunk_names;
    std::set<std::string> placeholder_names;
    for (Sym& sy : syms_) {
        if (!sy.decided) {
            continue;
        }
        plan_.decisions.push_back(sy.dec);
        const SymbolDecision& d = sy.dec;
        switch (d.decision) {
            case DecisionKind::PatchHooked: {
                HookEntry h;
                h.new_symbol = d.name;
                h.display = d.display;
                h.old_va = d.old_address.va;
                h.old_rva = d.old_address.rva;
                h.old_size = old_size_for(d.old_address);
                h.module = d.old_address.module;
                h.source = d.source;
                plan_.hooks.push_back(std::move(h));
                break;
            }
            case DecisionKind::PatchNewFunction: {
                NewFunction n;
                n.name = d.name;
                n.display = d.display;
                n.is_static = d.is_static;
                n.section = d.section;
                n.value = d.value;
                plan_.new_functions.push_back(std::move(n));
                break;
            }
            case DecisionKind::PatchNewData: {
                NewGlobal n;
                n.name = d.name;
                n.display = d.display;
                n.is_static = d.is_static;
                n.section = d.section;
                n.value = d.value;
                plan_.new_globals.push_back(std::move(n));
                break;
            }
            case DecisionKind::BindOldFunction:
                if (thunk_names.insert(d.name).second) {
                    ThunkEntry t;
                    t.name = d.name;
                    t.display = d.display;
                    t.slot = coff::slot_name(d.name);
                    t.target = d.old_address;
                    t.source = d.source;
                    plan_.thunks.push_back(std::move(t));
                }
                break;
            case DecisionKind::BindOldData:
                if (!d.defined && placeholder_names.insert(d.name).second) {
                    PlaceholderEntry p;
                    p.name = d.name;
                    p.display = d.display;
                    p.target = d.old_address;
                    p.source = d.source;
                    p.is_synthesized_slot = d.source == BindSource::SynthesizedImportSlot;
                    plan_.placeholders.push_back(std::move(p));
                }
                break;
            default:
                break;
        }
    }
    // Slot writes: every thunk's slot receives the old function's address; a synthesized import
    // slot placeholder receives the export's address.
    for (const ThunkEntry& t : plan_.thunks) {
        Fixup f;
        f.kind = FixupKind::Slot;
        f.symbol = t.slot;
        f.target_va = t.target.va;
        f.target_rva = t.target.rva;
        f.section_name = "binding.obj .data";
        plan_.fixups.push_back(std::move(f));
    }
    for (const PlaceholderEntry& p : plan_.placeholders) {
        if (!p.is_synthesized_slot) {
            continue;
        }
        Fixup f;
        f.kind = FixupKind::Slot;
        f.symbol = p.name;
        f.target_va = p.target.va;
        f.target_rva = p.target.rva;
        f.section_name = "binding.obj .data";
        plan_.fixups.push_back(std::move(f));
    }
}

Result<BindPlan> Planner::run(std::vector<Diag>* all_errors) {
    plan_.object_name = in_.object_name;
    plan_.module_name = in_.module_name;
    plan_.module_base = in_.module_base;

    const auto finish_errors = [&]() -> Result<BindPlan> {
        if (fatal_) {
            if (all_errors != nullptr) {
                all_errors->push_back(*fatal_);
            }
            return *fatal_;
        }
        if (all_errors != nullptr) {
            *all_errors = errors_;
        }
        if (partial_ != nullptr) {
            assemble();  // what was decided, for tools and evidence
            *partial_ = std::move(plan_);
        }
        return errors_.front();
    };

    if (obj_.machine() != coff::Machine::Amd64) {
        error(diag::kUnsupportedObject, "object " + in_.object_name + " is not an x64 object", "Compile the patch for x64.");
        return finish_errors();
    }
    if (!resolve_compiland()) {
        return finish_errors();
    }
    collect_sections();
    collect_symbols();
    load_relocations();
    if (fatal_) {
        return finish_errors();
    }
    decide_defined();
    bridge_statics();
    if (fatal_) {
        return finish_errors();
    }
    resolve_undefined();
    if (fatal_) {
        return finish_errors();
    }
    build_fixups();
    find_initialisers();
    if (!errors_.empty()) {
        // Still describe what was decided in the trace, then fail.
        return finish_errors();
    }
    assemble();
    // New globals know the initialisers that run for them.
    for (NewGlobal& g : plan_.new_globals) {
        for (const InitialiserEntry& e : plan_.initialisers) {
            if (std::find(e.globals.begin(), e.globals.end(), g.name) != e.globals.end()) {
                g.initialisers.push_back(e.function);
            }
        }
    }
    const PlanSummary s = plan_.summary();
    CML_LOG(Debug, "bind: plan for %s: hooks=%zu bind_old_data=%zu thunks=%zu new=%zu fixups=%zu warnings=%zu", in_.object_name.c_str(), s.hooks,
            s.bind_old_data, s.thunks, s.new_symbols, s.fixups, plan_.warnings.size());
    return std::move(plan_);
}

}  // namespace

Result<BindPlan> plan_bind(const PlanInput& input, std::vector<Diag>* all_errors, BindPlan* partial) {
    if (all_errors != nullptr) {
        all_errors->clear();
    }
    if (input.object == nullptr || input.module_pdb == nullptr) {
        return Diag::make(diag::kUnsupportedObject, "plan_bind called without an object or a module PDB", "This is a programming error in the caller.");
    }
    Planner planner(input, partial);
    return planner.run(all_errors);
}

}  // namespace cml::core::bind
