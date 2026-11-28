#include "cml/core/bind_plan.h"

#include <algorithm>
#include <array>
#include <limits>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

#include "cml/coff/section_symbols.h"
#include "cml/coff/writer.h"
#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_names.h"
#include "cml/core/bind_types.h"
#include "cml/core/imgrel_site.h"
#include "cml/util/format.h"
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
        case BindSource::CompilandSignature: return "compiland-static-signature";
        case BindSource::CompilandClosure: return "compiland-static-closure";
        case BindSource::CompilandProcedure: return "compiland-static-procedure";
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
        case FixupKind::Addr32NbDisp: return "ADDR32NB_DISP";
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

std::string BindPlan::imgrel_line(std::string_view label) const {
    std::string out = "WP36-IMGREL sites=" + std::to_string(imgrel_sites) + " verified=" + std::to_string(imgrel_verified) + " object=";
    out.append(label);
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
    bool via_secrel = false;  // referenced by a SECREL relocation (thread-local access)
    bool diagnosed = false;   // an error was already raised for it
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

// The PDB's spelling of a qualified name, brought to the decorated name's ("display") spelling.
std::string normalized_pdb_name(std::string_view name) {
    std::string n(name);
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
    return n;
}

// "a::b<c::d>::e" -> {"a", "b<c::d>", "e"}
std::vector<std::string> scope_components(const std::string& n) {
    std::vector<std::string> out;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t k = 0; k < n.size(); ++k) {
        const char c = n[k];
        if (c == '<' || c == '(' || c == '`') {
            ++depth;
        } else if ((c == '>' || c == ')' || c == '\'') && depth > 0) {
            --depth;
        } else if (c == ':' && depth == 0 && k + 1 < n.size() && n[k + 1] == ':') {
            out.push_back(n.substr(start, k - start));
            start = k + 2;
            ++k;
        }
    }
    out.push_back(n.substr(start));
    return out;
}

// WP-40: the length of the block prefix "?<n>?" / "?<letters>@?" of a local scope ("?1??f@@YAXXZ@4" -> 2), 0 when none.
std::size_t block_prefix_length(std::string_view scope) noexcept {
    if (scope.size() < 3 || scope[0] != '?') {
        return 0;
    }
    if (scope[1] >= '0' && scope[1] <= '9') {
        return scope[2] == '?' ? 2 : 0;
    }
    std::size_t k = 1;
    while (k < scope.size() && scope[k] >= 'A' && scope[k] <= 'P') {
        ++k;
    }
    return k > 1 && k + 1 < scope.size() && scope[k] == '@' && scope[k + 1] == '?' ? k + 1 : 0;
}

// WP-40: does the guard variable `guard` (`?$TSS0@<scope>@4HA`, `?$S1@<scope>@4IA`) guard the static `name`? A thread-safe guard
// `$TSSn` belongs to the statics of its own block (`?<id>@<scope>@4...`). A bitmask `$S<n>` (cl's /Zc:threadSafeInit-,
// clang-cl's -fno-threadsafe-statics) is ONE per function, written in the scope of its first block: it guards the statics
// of every block of that function.
// WP-52 (R-60): this compares the scopes as written, which is only right where neither spells a name by a back-reference
// (clang counts the guard's own name differently, bind_names.h local_scope_key). The planner compares the resolved scopes
// (scope_keys_cover) and uses this only for names the resolver cannot read (the comparison before WP-52).
// WP-59 (R-67): clang's bitmask of an inline function's statics, `??_B<scope>@5<n>`, is read as `?$S1@<scope>@4IA`.
bool raw_guard_covers(std::string_view guard, std::string_view name) {
    std::string inline_scope;
    std::string_view scope;  // "<scope>@4"
    bool bitmask = false;
    if (is_inline_bitmask_guard(guard)) {
        inline_scope = std::string(inline_bitmask_scope(guard)) + "@4";
        scope = inline_scope;
        bitmask = true;
    } else {
        const std::size_t at = guard.find('@');
        if (at == std::string_view::npos || guard.size() < at + 5 || guard.compare(0, 2, "?$") != 0) {
            return false;
        }
        const std::string_view tail = guard.substr(guard.size() - 4);
        if (tail != "@4HA" && tail != "@4IA") {
            return false;
        }
        scope = guard.substr(at + 1, guard.size() - at - 3);
        bitmask = guard[2] == 'S';
    }
    const std::size_t name_at = name.find('@');
    if (name.empty() || name[0] != '?' || name_at == std::string_view::npos) {
        return false;
    }
    const std::string_view rest = name.substr(name_at + 1);
    if (bitmask) {
        const std::size_t g_block = block_prefix_length(scope);
        const std::size_t s_block = block_prefix_length(rest);
        if (g_block == 0 || s_block == 0) {
            return false;
        }
        const std::string_view function = scope.substr(g_block);
        return rest.substr(s_block).substr(0, function.size()) == function;
    }
    return rest.substr(0, scope.size()) == scope;
}

// WP-52 (R-60): the same rule on the scopes with their back-references resolved (local_scope_key).
bool scope_keys_cover(const LocalScopeKey& guard, const LocalScopeKey& name, bool bitmask) {
    if (bitmask) {
        return !guard.block.empty() && !name.block.empty() && guard.rest == name.rest;
    }
    return guard.block == name.block && guard.rest == name.rest;
}

// WP-40 (second high review): is `guard` a bitmask `?$S<n>@<scope>@4IA`, and the decorated name of the function whose
// statics it guards ("?$S1@?1??f@@YAHXZ@4IA" -> "?f@@YAHXZ"; a lambda's: "??R<lambda_0>@?0??f@@YAXXZ@QEBA@XZ"). Empty when
// `guard` is not a bitmask or its scope cannot be read.
// WP-59 (R-67): clang's `??_B<scope>@5<n>` of an inline function as well ("??_B?1??counter@@YAHXZ@51" -> "?counter@@YAHXZ").
std::string bitmask_function(std::string_view guard) {
    std::string_view scope;  // "?1??f@@YAHXZ"
    if (is_inline_bitmask_guard(guard)) {
        scope = inline_bitmask_scope(guard);
    } else {
        if (guard.size() < 8 || guard.compare(0, 3, "?$S") != 0 || guard[3] < '0' || guard[3] > '9' || guard.substr(guard.size() - 4) != "@4IA") {
            return {};
        }
        const std::size_t at = guard.find('@');
        if (at == std::string_view::npos || guard.size() < at + 1 + 4) {
            return {};
        }
        scope = guard.substr(at + 1, guard.size() - at - 1 - 4);
    }
    const std::size_t block = block_prefix_length(scope);
    if (block == 0 || block + 1 >= scope.size() || scope[block] != '?') {
        return {};
    }
    return std::string(scope.substr(block + 1));
}

// WP-59 (ADR-0046, review item 0): a thread-safe guard `?$TSS<n>@<scope>@4HA`: its number and its scope as written
// ("?$TSS1@?1??counter@@YAHXZ@4HA" -> 1, "?1??counter@@YAHXZ"). False when `guard` is no such name.
bool tss_guard_parts(std::string_view guard, std::uint32_t& number, std::string_view& scope) {
    if (guard.size() < 10 || guard.compare(0, 5, "?$TSS") != 0 || guard.substr(guard.size() - 4) != "@4HA") {
        return false;
    }
    std::size_t k = 5;
    std::uint64_t n = 0;
    while (k < guard.size() && guard[k] >= '0' && guard[k] <= '9' && n < 0xFFFFFFFFull) {
        n = n * 10 + static_cast<std::uint64_t>(guard[k] - '0');
        ++k;
    }
    if (k == 5 || k >= guard.size() || guard[k] != '@' || n >= 0xFFFFFFFFull || guard.size() < k + 1 + 4) {
        return false;
    }
    number = static_cast<std::uint32_t>(n);
    scope = guard.substr(k + 1, guard.size() - k - 1 - 4);
    return !scope.empty();
}

// The decorated name of the function of a `$TSS<n>` guard ("?$TSS1@?1??counter@@YAHXZ@4HA" -> "?counter@@YAHXZ"), empty
// when it cannot be read.
std::string tss_guard_function(std::string_view guard) {
    std::uint32_t number = 0;
    std::string_view scope;
    if (!tss_guard_parts(guard, number, scope)) {
        return {};
    }
    const std::size_t block = block_prefix_length(scope);
    if (block == 0 || block + 1 >= scope.size() || scope[block] != '?') {
        return {};
    }
    return std::string(scope.substr(block + 1));
}

// WP-46 (F-41.2): the closure a decorated name belongs to when cl's legacy lambda processor named it by a hash
// ("??R<lambda_9c7f...>@@QEBA@XZ" -> "<lambda_9c7f...>": `<lambda_` and 32 lower-case hex digits). Empty otherwise (no
// closure, or a closure numbered by the compiler, "<lambda_1>").
std::string hashed_closure(std::string_view decorated) {
    for (std::size_t at = decorated.find("<lambda_"); at != std::string_view::npos; at = decorated.find("<lambda_", at + 1)) {
        const std::size_t digits = at + 8;
        if (decorated.size() < digits + 33 || decorated[digits + 32] != '>') {
            continue;
        }
        bool hex32 = true;
        for (std::size_t k = digits; hex32 && k < digits + 32; ++k) {
            const char c = decorated[k];
            hex32 = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }
        if (hex32) {
            return std::string(decorated.substr(at, 41));
        }
    }
    return {};
}

// WP-46 (review): the hash-named closure a function belongs to as one of its members: its call operator "??R<c>@@...", its
// conversion "??B<c>@@...", an invoker "?<lambda_invoker_cdecl>@<c>@@..." (any calling convention). Empty otherwise.
std::string hashed_closure_member(std::string_view name) {
    std::size_t at = std::string_view::npos;
    if (starts_with(name, "??R<lambda_") || starts_with(name, "??B<lambda_")) {
        at = 3;
    } else if (starts_with(name, "?<lambda_invoker_")) {
        const std::size_t sep = name.find('@');
        at = sep == std::string_view::npos ? sep : sep + 1;
    }
    if (at == std::string_view::npos || at >= name.size()) {
        return {};
    }
    const std::string c = hashed_closure(name.substr(at));
    return !c.empty() && name.compare(at, c.size() + 2, c + "@@") == 0 ? c : std::string();
}

// WP-46 (review): the hash-named closure a static is declared in (its scope is the closure's call operator,
// "?x@?1???R<c>@@..."). Empty otherwise.
std::string hashed_closure_static(std::string_view name) {
    const std::size_t at = name.find("???R<lambda_");
    if (at == std::string_view::npos || name.compare(0, 1, "?") != 0) {
        return {};
    }
    const std::string c = hashed_closure(name.substr(at + 4));
    return !c.empty() && name.compare(at + 4, c.size() + 2, c + "@@") == 0 ? c : std::string();
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
    // FU-12: per compiland symbol (same index as comp_syms_), the name spellings the matching needs, computed
    // once on first use instead of once per group (O(G*N) string work).
    struct CompNorm {
        bool last_done = false;
        bool done = false;
        std::string last;       // last_component(name)
        std::string normalized;  // normalized_pdb_name(name)
        std::string canonical;   // canonical_lambda_names(normalized)
        std::vector<std::string> comps;  // scope_components(normalized)
    };
    std::vector<CompNorm> comp_norm_;
    CompNorm& comp_entry(const pdb::SymbolView* v) { return comp_norm_[static_cast<std::size_t>(v - comp_syms_.data())]; }
    const std::string& comp_last(const pdb::SymbolView* v) {
        CompNorm& n = comp_entry(v);
        if (!n.last_done) {
            n.last = last_component(v->name);
            n.last_done = true;
        }
        return n.last;
    }
    const CompNorm& comp_norm(const pdb::SymbolView* v) {
        CompNorm& n = comp_entry(v);
        if (!n.done) {
            n.normalized = normalized_pdb_name(v->name);
            n.canonical = canonical_lambda_names(n.normalized);
            n.comps = scope_components(n.normalized);
            n.done = true;
        }
        return n;
    }
    std::map<std::uint32_t, std::int64_t> delta_;  // obj section -> (old RVA - section offset); absent: unknown
    std::set<std::uint32_t> delta_conflict_;
    // WP-40: the image section the layout of an object section maps into (every learned static agrees), so that a guard
    // placed from that layout must land in the same image section (ADR-0043, WP-40 point 2).
    std::map<std::uint32_t, std::uint16_t> delta_image_section_;
    void learn_delta(const Sym& sy, std::uint32_t old_rva);

    // WP-40: raw indices of the symbols a data record of the object's own `.debug$S` names (a SECREL relocation there
    // targets a data symbol only from S_LDATA32 / S_GDATA32 / S_LTHREAD32 / S_GTHREAD32). A piece of a split aggregate
    // (`name.N`) has none: its debug information is a fragment, which CodeView does not record.
    std::set<std::uint32_t> debug_named_;
    bool debug_named_loaded_ = false;
    bool named_by_debug_record(const Sym& sy);
    // WP-40: the static data of the patch that is declared in the decorated scope of a guard variable
    // (`?$TSS0@<scope>@4HA` guards statics `?<name>@<scope>@4...`): positions in syms_.
    // WP-52 (R-60): the statics a guard is tied to (the statics of its block, or of its function for a bitmask), by the
    // scopes with their back-references resolved, in the spelling of the compiler that wrote the patch object
    // (guard_spelling). Computed once per guard (names do not change). Where that spelling is not known, a guard the two
    // spellings tie differently (or only one of them ties) is tied to none, and guard_ambiguous_ keeps what either tied.
    const std::vector<std::uint32_t>& guard_scope_statics(const Sym& guard);
    bool guard_ties(std::uint32_t guard_pos, std::uint32_t static_pos);
    // WP-52 (ADR-0045, convergence ruling): every function-local static bound to the running process that needs a guard has
    // a guard tied to it that is bound to the old one, or it is refused (CML3211, naming the static). `untied`: the guards
    // tied to no static (each new). Returns the untied guards a refusal names (they get no warning of their own).
    std::set<std::uint32_t> require_bound_guards(const std::vector<std::uint32_t>& untied);
    // WP-52 (ADR-0045, ruling on R-66 and the cl nested case): a function-local static decided new while the old compiland
    // records a static of its name in a procedure of the same outermost function is refused (CML3207): it may be that
    // record's live object, which a new static would reset. `top_function_of`: the outermost function of a static of the
    // patch, as a canonical display name ("o2_nest_step"); nullopt when it cannot be read (then any procedure counts).
    void refuse_reset_statics();
    std::optional<std::string> top_function_of(std::uint32_t pos);
    bool old_scope_has_guard_record(const Sym& st);
    // [0]: the key of a static, or of a guard in clang 18's spelling (its own name not counted); [1]: a guard in cl's and
    // clang 19's (counted).
    using ScopeKeys = std::array<std::optional<LocalScopeKey>, 2>;
    const ScopeKeys& scope_keys(std::uint32_t pos);
    std::map<std::uint32_t, ScopeKeys> scope_keys_;
    std::map<std::uint32_t, std::vector<std::uint32_t>> guard_statics_;
    std::map<std::uint32_t, std::vector<std::uint32_t>> guard_ambiguous_;
    // WP-52 (review, finding 1; convergence ruling B): does the compiler that wrote the PATCH OBJECT (the guard and its
    // statics are its names) count a guard's own name (`$TSS<n>`, `$S<n>`) in the back-reference table? From the object's
    // own S_COMPILE3 (.debug$S): cl counts `$TSS<n>` (Jolt, corpus); clang counts it from 19.1.0 on (MicrosoftMangle.cpp:
    // `mangleSourceName("$TSS" + ...)`; 17.0.6 and 18.1.8 write it raw) and never counts `$S1` (raw in 17.0.6 .. 21.1.0).
    // Unknown: clang 19.0.x (development and vendor builds), a version that cannot be read, no S_COMPILE3, another
    // compiler, cl's `$S1`.
    enum class GuardSpelling { Unknown, NotCounted, Counted };
    GuardSpelling guard_spelling(bool bitmask);
    int compiler_family_ = -1;  // -1 not read yet, 0 unknown, 1 cl, 2 clang
    int clang_major_ = 0;
    int clang_minor_ = -1;
    std::string compiler_version_;
    void settle_guards();
    // WP-59 (R-67, ADR-0046): the external guards of an inline function's statics (clang's `??_B` bitmask, and the
    // thread-safe `?$TSS<n>` of every compiler), which decide_defined() bound by their public names like ordinary data.
    void settle_external_guard(Sym& sy, std::vector<std::uint32_t>& untied);
    bool external_tss_alone_in_block(const Sym& guard, std::uint32_t static_pos, std::string& why);
    std::set<std::uint32_t> external_guards_to_prove_;  // bound external `$TSS` guards that only the function's code proves
    bool old_tss_loaded_ = false;
    bool old_tss_listed_ = false;
    std::map<std::string, std::set<std::uint32_t>> old_tss_numbers_;  // the old image's `?$TSS<n>@<scope>@4HA` publics: scope -> numbers

    // WP-37 (ADR-0042): canonical signatures, read lazily on both sides. The old side is the module PDB's type stream,
    // the new side the patch object's .debug$S / .debug$T (or its type server PDB): cml/core/bind_types.h.
    std::unique_ptr<ObjectSignatures> new_sigs_;
    std::map<std::uint32_t, pdb::FunctionSignature> old_sigs_;  // by SymbolView::index within the compiland
    const pdb::FunctionSignature& old_signature(const pdb::SymbolView& proc);
    const pdb::FunctionSignature& new_signature(const Sym& sy);
    // The identity of the closure whose operator() encloses a function-local record of the old compiland: the unique
    // name of the member function's class without its ".?AV" prefix and without the translation-unit discriminator cl
    // appends ("<lambda_1>@?1??f@@YAXXZ@"), which is how the decorated name of a symbol spells it.
    struct ClosureId {
        bool known = false;  // false: the types cannot be read (`why`)
        std::string tail;    // empty when known: the enclosing function is not a member function of a uniquely named class
        std::string why;
    };
    ClosureId closure_of(const pdb::SymbolView& local);
    struct Match {
        const pdb::SymbolView* view = nullptr;
        BindSource how = BindSource::None;
    };
    bool pair_overloads_by_signature(const std::string& group_name, std::vector<std::uint32_t>& unmatched, std::vector<const pdb::SymbolView*>& free_cands,
                                     std::map<std::uint32_t, Match>& matched, std::map<std::uint32_t, std::string>& why);
    void pair_statics_by_closure(const std::string& base, std::vector<std::uint32_t>& unmatched, std::vector<const pdb::SymbolView*>& free_cands,
                                 std::map<std::uint32_t, Match>& matched, std::map<std::uint32_t, std::string>& why, std::set<std::uint32_t>& no_layout);
    ObjectSignatures& object_debug();
    // WP-40 (R-48 item 5, rule (d)): the lambdas whose statics the closure step paired, per closure of the patch, for the
    // pipeline's comparison of their call operators (BindPlan::lambda_sets).
    struct LambdaNote {
        std::string function;      // decorated function part of the closure ("?1??f@@YAXXZ@")
        std::string display;       // the enclosing function's display name
        std::string old_closure;   // the old closure it was paired with
        std::string call_operator; // the patch's operator() of the closure ("" when it has none: inlined)
        std::vector<std::string> statics;
    };
    std::map<std::string, LambdaNote> lambda_notes_;  // by the patch's closure ("<lambda_1>@?1??f@@YAXXZ@")
    void note_lambda(const Sym& sy, const std::string& base, const std::string& own_scope, const std::string& old_closure);
    // WP-40: closures are named without the translation unit's discriminator (pdb::without_tu_discriminator: the hash in
    // "?A0x<hash>@" of an anonymous namespace), on both sides; decorated names of the patch are compared in that spelling too.
    std::vector<std::string> plain_names_;
    const std::string& plain_name(const Sym& sy);
    bool in_closure(const Sym& sy, const std::string& closure);  // "??R" + closure occurs in its (plain) name
    // The symbol of the same section whose plain name is sy's with closure `from` replaced by `to` (its twin of that number).
    const Sym* closure_twin(const Sym& sy, const std::string& from, const std::string& to);
    void build_lambda_sets();
    void pair_statics_by_procedure(const std::string& base, bool is_tls, const std::vector<std::uint32_t>& members, const std::vector<const pdb::SymbolView*>& cands,
                                   std::vector<std::uint32_t>& unmatched, std::vector<const pdb::SymbolView*>& free_cands, std::map<std::uint32_t, Match>& matched,
                                   std::map<std::uint32_t, std::string>& why, const std::set<std::uint32_t>& no_layout);

    // WP-46 (F-41.2): is the old function at `v` the one an external definition of the patch replaces (a public symbol at its
    // address whose decorated name the patch defines with external linkage)? Such a function pairs by that name.
    std::set<std::string, std::less<>> external_defined_;
    bool external_defined_built_ = false;
    bool taken_by_public(const pdb::SymbolView& v, const std::string& closure);
    // The public at v's address that is the call operator of `closure` ("??R" + closure, the closure as a signature or
    // decorated name spells it: "<lambda_X>@@", "<lambda_1>@?0??f@@YAXXZ@"); "" when there is none.
    std::string own_public(const pdb::SymbolView& v, const std::string& closure);
    // WP-46 (F-41.3): is the old procedure a deleting destructor (`??_G` / `??_E`)? clang-cl records it under the
    // destructor's name and type; only its implicit parameter `should_call_delete` tells it apart.
    bool is_deleting_destructor(const pdb::SymbolView& v);
    // WP-46 (F-41.1): does the old compiland record any thread-safe guard (`$TSS<n>`)? clang-cl records none.
    int records_tss_guards_ = -1;
    bool compiland_records_tss_guards();
    // WP-46 (F-41.1): statics paired by their function alone (one of the name in it) whose guard has no record; with their
    // old RVAs they teach the layout from which settle_guards places those guards (and nothing else).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> place_taught_;  // (position in syms_, old rva)
    std::map<std::uint32_t, std::int64_t> guard_delta_;            // delta_ plus what place_taught_ adds; used by settle_guards
    std::map<std::uint32_t, std::uint16_t> guard_delta_image_section_;
    void learn_guard_layout();
    // WP-46 (F-41.2): BindPlan::hashed_lambdas, and CML3630 for a hashed lambda's static that no hooked function vouches for.
    void note_hashed_lambdas();
    // The closure pairing the planner follows: PlanInput::closure_pairing (rule (d)) plus the hash-named lambda paired by
    // elimination (pair_hashed_by_elimination).
    std::map<std::string, std::string> closure_pairing_;
    // WP-46 (F-41.2, round 2 ruling): cl's legacy hash depends on the lambda's own body, so an edited lambda is renamed. When
    // exactly one static hash-named closure is left unmatched on each side, they are paired (closure_pairing_); any other
    // leftover with a static, or any static leftover at all, is CML3630.
    std::map<std::string, std::string> eliminated_;  // patch closure "<lambda_X>" -> old closure "<lambda_Y>"
    void pair_hashed_by_elimination();
    // WP-46 (review): why the patch's closure `fresh` (call operator `op`) cannot be shown to be the old closure `old` (call
    // operator procedure `old_op`): the same enclosing function, the same layout, the same statics and the same call operator
    // signature are required. "" when they all agree.
    std::string elimination_problem(const std::string& fresh, const std::string& old, const Sym& op, const pdb::SymbolView& old_op);
    // WP-46 (final review): why a closure paired by elimination cannot be: state of a user of the closure that is not one of
    // its members (a static of `run<<lambda>>`, data whose name holds the closure) would be renamed with it, i.e. constructed
    // again while the old one is abandoned. "" when no such state exists on either side.
    std::string closure_user_state(const std::string& fresh, const std::string& old);

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
    void reject_new_thread_locals();
    void warn_thread_local_initialisers();
    void new_thread_local_error(Sym& sy);  // marks sy diagnosed
    void resolve_undefined();
    void build_fixups();
    void assemble();
    void find_initialisers();

    // ---- lookups ----
    std::optional<OldRef> lookup_registry(const std::string& name, SymbolType want);
    std::optional<OldRef> lookup_public(const std::string& name, SymbolType want);
    struct ThreadLookup {
        std::optional<OldRef> ref;
        bool ambiguous = false;
    };
    ThreadLookup lookup_thread_global(const Sym& sy);
    bool in_old_tls(std::uint32_t rva);
    std::set<std::uint32_t> tls_rvas_;  // RVAs of the thread-local records looked at
    bool ensure_compiland_symbols();
    std::vector<const pdb::SymbolView*> static_candidates(const std::string& base, const std::string& encl, bool want_function,
                                                          const std::string& display = {}, int tls_mode = 0, const std::string& decorated = {});
    // WP-40: a record of `base` directly in the procedure of the function that encloses the lambda of a static's display
    // name ("f::<lambda_1>::operator()::sname" -> a record "sname" of procedure "f"): clang-cl records the static of an
    // inlined call operator there. Empty `enclosing`: the static is not in a lambda.
    std::string lambda_enclosing(const std::string& display, const std::string& base) const;
    bool is_inlined_record(const pdb::SymbolView* v, const std::string& enclosing);
    // WP-51 (R-54, ADR-0044): what an old procedure inlined, read once per plan from its inline sites. `risk` names an
    // inlinee whose function-local statics the compiler records in this procedure under their own names: a member of a
    // closure (call operator, invoker, anything but a conversion operator, constructor, destructor or operator=), any
    // inlinee that mentions a closure (a template over one), or an inline site that cannot be read. `wider` names the
    // first other inlinee a wider rule (R-57: internal helpers) would take, for the WP51-WOULD-REFUSE evidence line.
    struct InlineVerdict {
        std::string risk;
        std::string wider;
        const char* wider_kind = "";
        // The closures whose code it inlined (unique names without ".?AV" and the unit's discriminator: how a static's
        // decorated scope spells its closure after "??R").
        std::vector<std::string> closure_tails;
        std::uint32_t sites = 0;
        std::size_t inlinees = 0;
        std::size_t closures = 0;
        std::size_t unreadable = 0;
        bool asked = false;  // counted in the WP51-INLINEES summary (a verdict of the batch nobody asked for is not)
        bool cached = false;  // WP-55: taken from PlanInput::inline_verdicts
    };
    std::map<std::uint32_t, InlineVerdict> inline_verdicts_;  // by the procedure's index in comp_syms_
    const InlineVerdict& inline_verdict(std::uint32_t proc);
    void judge_inlinees(std::uint32_t proc, const Result<pdb::PdbIndex::ProcInlinees>& sites);
    // WP-55 (R-62): the verdict an earlier plan against the same image kept (PlanInput::inline_verdicts), into inline_verdicts_.
    bool cached_verdict(std::uint32_t proc);
    bool inline_batch_done_ = false;
    std::set<std::string> wp51_static_bases_;  // the names of the patch's static data: the records a batch is read for
    // WP-51 (review): the closure a lambda's static belongs to, as the decorated scope spells it after "??R" (without the
    // unit's discriminator), with rule (d)'s closure pairing applied.
    std::string own_closure_scope(const Sym& sy) const;
    std::size_t wp51_procs_ = 0;
    std::size_t wp51_sites_ = 0;
    std::size_t wp51_closures_ = 0;
    std::size_t wp51_unreadable_ = 0;
    std::size_t wp51_refused_ = 0;
    std::size_t wp51_would_refuse_ = 0;
    std::set<std::uint32_t> wp51_checked_;  // positions of the statics already judged
    // WP-51: a static `sy` of function `encl` about to be paired with `record`. When the record lies directly in `encl`'s
    // own procedure (not in the function an inlined lambda's static is looked for in, WP-40) and that procedure inlined a
    // risky inlinee, the static is refused (CML3207) and true is returned.
    bool refuse_inlined_owner(Sym& sy, const pdb::SymbolView& record, const std::string& encl, const char* step);
    // WP-53 (R-61): the record of a call operator of `sy`'s own closure (rule (d)'s pairing applied) at `record`'s address,
    // in another procedure than `record`'s (cl's twin, WP-40), or null. The data records of call operators by RVA are
    // indexed once per compiland.
    const pdb::SymbolView* own_closure_twin(const Sym& sy, const pdb::SymbolView& record);
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> op_records_by_rva_;
    bool op_records_indexed_ = false;
    std::optional<OldRef> resolve_name(Sym& sy);  // the undefined-symbol chain

    void decide(Sym& sy, DecisionKind kind, BindSource source, SymbolType type, const TargetAddress& addr, std::string reason);
    void trace(const Sym& sy) const;
    std::uint32_t old_size_for(const TargetAddress& a);
    std::uint64_t module_size();
    bool rel32_reachable(const SymbolDecision& d);
    std::uint64_t module_size_ = 0;

    // WP-36: is the ADDR32NB site at `offset` of section `sec_index` (1-based) the displacement of a memory operand?
    SiteProver& site_prover(std::uint32_t sec_index);
    std::map<std::uint32_t, std::unique_ptr<SiteProver>> site_provers_;
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

// An external thread_local of the old image by its recorded display name: S_GTHREAD32 (segment = the .tls
// section, offset = offset inside it). The S_PUB32 of a thread-local does NOT carry that offset (CI evidence,
// WP-24: the public's RVA of a namespace-scope thread_local was ~0x10d54 past its neighbour's S_LTHREAD32 offset
// of 0x8 in the same object), so publics must not be used to place a SECREL.
Planner::ThreadLookup Planner::lookup_thread_global(const Sym& sy) {
    ThreadLookup out;
    const std::string& disp = sy.dec.display.empty() ? sy.name : sy.dec.display;
    Result<std::vector<pdb::SymbolView>> globals = pdb_.find_global_data(disp);
    if (!globals) {
        fatal(globals.error());
        return out;
    }
    const pdb::SymbolView* found = nullptr;
    for (const pdb::SymbolView& v : *globals) {
        if (v.kind != pdb::SymbolKind::GlobalThread || v.rva == pdb::kNoRva) {
            continue;
        }
        if (found != nullptr) {
            out.ambiguous = true;
            return out;
        }
        found = &v;
    }
    if (found == nullptr) {
        return out;
    }
    tls_rvas_.insert(found->rva);
    OldRef r;
    r.addr = module_address(found->rva);
    r.source = BindSource::PdbGlobal;
    r.type = SymbolType::Data;
    r.note = "S_GTHREAD32 at rva " + hex(found->rva) + " (section " + std::to_string(found->section) + " offset " + hex(found->offset) + ")";
    out.ref = std::move(r);
    return out;
}

// Does the RVA belong to the old image's thread-local storage? Exactly: it is the address of a thread-local record
// the planner has looked at (S_GTHREAD32 / S_LTHREAD32), or it lies in a section named .tls. (CI evidence: the
// section name alone is not reliable on MSVC images, so the records decide.)
bool Planner::in_old_tls(std::uint32_t rva) {
    if (tls_rvas_.count(rva) != 0) {
        return true;
    }
    const std::optional<pdb::SectionOffset> so = pdb_.section_offset_from_rva(rva);
    const std::span<const pdb::SectionHeader> secs = pdb_.sections();
    return so && so->section >= 1 && so->section <= secs.size() && starts_with(std::string_view(secs[so->section - 1u].name.data()), ".tls");
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
    comp_norm_.assign(comp_syms_.size(), CompNorm());
    comp_loaded_ = true;
    return true;
}

std::vector<const pdb::SymbolView*> Planner::static_candidates(const std::string& base, const std::string& encl, bool want_function,
                                                                const std::string& display, int tls_mode, const std::string& decorated) {
    // A static matches an old record ONLY when its scope matches exactly: the same enclosing function, or
    // file scope. Records of other scopes are never candidates (a wrong match would bind live state or a
    // hook to an unrelated entity): "f::counter" is not "g::counter", and a new file-scope "count" is
    // not the old "f::count".
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
        // S_LTHREAD32 / S_GTHREAD32 and S_LDATA32 / S_GDATA32 never stand for each other: a static whose
        // thread_local-ness changed is a different variable (tls_mode 0: plain data, 1: thread-local, -1: either).
        if (!want_function && tls_mode >= 0) {
            const bool thread = v.kind == pdb::SymbolKind::LocalThread || v.kind == pdb::SymbolKind::GlobalThread;
            if (thread != (tls_mode == 1)) {
                continue;
            }
        }
        if (comp_last(&v) != base) {
            continue;
        }
        if (!want_function && (v.kind == pdb::SymbolKind::LocalThread || v.kind == pdb::SymbolKind::GlobalThread)) {
            tls_rvas_.insert(v.rva);
        }
        all.push_back(&v);
    }
    const auto by_rva = [](const pdb::SymbolView* a, const pdb::SymbolView* b) { return a->rva < b->rva; };
    // The qualified name, when both sides spell it the same, is the strongest key: it contains the scope
    // ("lambdas_init::<lambda_0>::operator()" on clang, "Ns::Cls::f").
    if (!display.empty()) {
        const std::string canonical_display = canonical_lambda_names(display);
        for (const pdb::SymbolView* v : all) {
            if (comp_norm(v).canonical == canonical_display) {
                out.push_back(v);
            }
        }
        if (!out.empty()) {
            std::sort(out.begin(), out.end(), by_rva);
            return out;
        }
    }
    if (want_function) {
        // Functions are top-level procs in the PDB; a local scope (lambda, local class) lives in the
        // qualified name: it must contain the enclosing function, and a file-scope function must not be
        // inside a lambda.
        // WP-40 (R-48 item 3): a function that is not function-local must also be in the SAME scope (namespace and
        // class) as the member: a new file-scope `static int helper(int)` is not the old `Foo::helper`. And a record
        // whose address carries a public symbol with another decorated name is an external function (statics have no
        // S_PUB32), so it is never the old version of this one.
        std::vector<std::string> member_scope;
        if (!display.empty()) {
            member_scope = scope_components(canonical_lambda_names(display));
            member_scope.pop_back();
        } else if (!decorated.empty()) {
            member_scope = scope_components(canonical_lambda_names(display_name(decorated)));
            member_scope.pop_back();
        }
        for (const pdb::SymbolView* v : all) {
            const std::vector<std::string>& comps = comp_norm(v).comps;
            bool has_encl = false;
            bool in_lambda = false;
            for (std::size_t k = 0; k + 1 < comps.size(); ++k) {
                has_encl = has_encl || comps[k] == encl;
                in_lambda = in_lambda || starts_with(comps[k], "<lambda");
            }
            if (!(encl.empty() ? !in_lambda : has_encl)) {
                continue;
            }
            if (encl.empty()) {
                std::vector<std::string> cand_scope = scope_components(comp_norm(v).canonical);
                cand_scope.pop_back();
                if (cand_scope != member_scope) {
                    CML_LOG(Trace, "bind: '%s': record '%s' at rva %s is in another scope: not a candidate", base.c_str(), std::string(v->name).c_str(), hex(v->rva).c_str());
                    continue;
                }
            }
            if (!decorated.empty()) {
                const Result<std::vector<pdb::SymbolView>> pubs = pdb_.publics_at_rva(v->rva);
                bool other_public = false;
                for (std::size_t k = 0; pubs && k < pubs->size(); ++k) {
                    other_public = other_public || (*pubs)[k].name != decorated;
                }
                if (other_public) {
                    CML_LOG(Trace, "bind: '%s': record '%s' at rva %s has a public symbol of another name (an external function): not a candidate", base.c_str(),
                            std::string(v->name).c_str(), hex(v->rva).c_str());
                    continue;
                }
            }
            out.push_back(v);
        }
    } else {
        // WP-40 (R-48 item 1): a static of a lambda's call operator is also looked for directly in the function that
        // encloses the lambda. clang-cl records the static of an inlined call operator there (as an S_LDATA32 of the
        // calling procedure, next to its inline sites) and emits no procedure for the operator() at all. Such a record
        // says nothing about which lambda it belongs to: it is never paired by its name (pass A defers it); the section
        // layout or the place in the function decides, or the static is refused.
        const std::string enclosing = encl == "operator()" ? lambda_enclosing(display, base) : std::string();
        for (const pdb::SymbolView* v : all) {
            const bool local = v->parent_proc != pdb::kNoSymbol;
            if (encl.empty() ? !local : (local && (last_component(v->parent_name) == encl || is_inlined_record(v, enclosing)))) {
                out.push_back(v);
            }
        }
    }
    std::sort(out.begin(), out.end(), by_rva);
    return out;
}

std::string Planner::lambda_enclosing(const std::string& display, const std::string& base) const {
    if (display.empty() || base.empty()) {
        return {};
    }
    const std::string canonical = canonical_lambda_names(display);
    const std::string suffix = "::<lambda>::operator()::" + base;
    if (canonical.size() <= suffix.size() || canonical.compare(canonical.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return {};
    }
    return canonical.substr(0, canonical.size() - suffix.size());
}

bool Planner::is_inlined_record(const pdb::SymbolView* v, const std::string& enclosing) {
    if (enclosing.empty() || v->parent_proc == pdb::kNoSymbol || v->parent_proc >= comp_syms_.size()) {
        return false;
    }
    return comp_norm(&comp_syms_[v->parent_proc]).canonical == enclosing;
}

namespace {

// WP-51: a closure class, by its unique name (".?AV<lambda_0>@?0??f@@YAXXZ@", cl's ".?AV<lambda_<hash>>@@") or, when it has
// none, by the last component of its name ("f::<lambda_0>").
bool is_closure_class(const std::string& unique, const std::string& name) {
    if (unique.size() > 4 && unique.compare(0, 2, ".?") == 0 && unique.compare(4, 8, "<lambda_") == 0) {
        return true;
    }
    const std::size_t at = name.rfind("::");
    return starts_with(at == std::string::npos ? std::string_view(name) : std::string_view(name).substr(at + 2), "<lambda_");
}

// WP-51: members of a closure that run none of the lambda's own code, so hold none of its statics.
bool harmless_closure_member(const std::string& member, const std::string& class_name) {
    if (member.empty()) {
        return false;
    }
    if (member == "operator=" || member[0] == '~') {
        return true;
    }
    if (starts_with(member, "operator ") && !starts_with(member, "operator ()") && !starts_with(member, "operator()")) {
        return true;  // a conversion operator (to the invoker's function pointer)
    }
    // A constructor is named like the class ("<lambda_0>"); cl's invokers ("<lambda_invoker_cdecl>") are not.
    const std::size_t at = class_name.rfind("::");
    return member == (at == std::string::npos ? class_name : class_name.substr(at + 2));
}

// WP-51 (R-57 evidence only): a class with internal linkage, by its unique name: in an anonymous namespace ("?A0x") or local
// to a function ("@?1??f@@...": a scope component that is a function's number).
bool internal_class(const std::string& unique) {
    if (unique.find("?A0x") != std::string::npos) {
        return true;
    }
    for (std::size_t at = unique.find("@?"); at != std::string::npos; at = unique.find("@?", at + 1)) {
        if (at + 2 < unique.size() && unique[at + 2] >= '0' && unique[at + 2] <= '9') {
            return true;
        }
    }
    return false;
}

}  // namespace

void Planner::judge_inlinees(std::uint32_t proc, const Result<pdb::PdbIndex::ProcInlinees>& sites) {
    InlineVerdict& v = inline_verdicts_[proc];
    const pdb::SymbolView& p = comp_syms_[proc];
    const std::string where = "'" + std::string(p.name) + "' (rva " + hex(p.rva) + ")";
    if (in_.inline_verdicts != nullptr) {
        in_.inline_verdicts->count_read();  // WP-55: a verdict judged from a read (kept below only when the sites could be read)
    }
    if (!sites) {
        v.risk = "the inline sites of " + where + " cannot be read (" + sites.error().message + ")";
        v.unreadable = 1;
        CML_LOG(Trace, "WP51-BATCH proc=%s unreadable (%s)", where.c_str(), sites.error().message.c_str());
        return;
    }
    for (const pdb::PdbIndex::Inlinee& e : sites->inlinees) {
        const std::string what = e.member ? e.class_name + "::" + e.name : e.name;
        std::string risk;
        if (!e.readable) {
            ++v.unreadable;
            risk = "an inline site of " + where + " names an inlinee that cannot be read (" + e.why + ")";
        } else if (e.member && is_closure_class(e.class_unique, e.class_name)) {
            if (!harmless_closure_member(e.name, e.class_name)) {
                ++v.closures;
                risk = where + " inlined '" + what + "', a lambda's code";
                // ".?AV<lambda_1>@?0??f@@YAXXZ@" -> "<lambda_1>@?0??f@@YAXXZ@", how a static's decorated scope spells the closure.
                if (e.class_unique.size() > 4 && e.class_unique.compare(0, 2, ".?") == 0) {
                    v.closure_tails.push_back(e.class_unique.substr(4));
                }
            }
        } else if (what.find("<lambda_") != std::string::npos || e.class_unique.find("<lambda_") != std::string::npos ||
                   (e.signature_known && e.signature.find("<lambda_") != std::string::npos)) {
            ++v.closures;
            risk = where + " inlined '" + what + "', a function over a closure type (" + (e.signature_known ? e.signature : std::string("its name")) + ")";
        } else if (!e.signature_known) {
            ++v.unreadable;
            risk = "the type of '" + what + "', inlined into " + where + ", cannot be read (" + e.why + "), so it cannot be shown not to be a template over a closure";
        } else if (v.wider.empty()) {
            if (!e.member) {
                v.wider = what;
                v.wider_kind = "free-function";
            } else if (internal_class(e.class_unique)) {
                v.wider = what;
                v.wider_kind = "internal-class";
            }
        }
        CML_LOG(Trace, "WP51-BATCH-INLINEE proc=%s id=0x%X inlinee='%s' member=%d sites=%u depth=%u %s", where.c_str(), e.id, what.c_str(), e.member ? 1 : 0, e.sites, e.depth,
                risk.empty() ? "harmless" : "risky");
        if (v.risk.empty() && !risk.empty()) {
            v.risk = risk;
        }
    }
    if (!sites->complete) {
        // (Review: counted even when a risk was found already; the list is cut, so it is unreadable data either way.)
        ++v.unreadable;
        if (v.risk.empty()) {
            v.risk = where + " has more inline sites than the reader follows (" + std::to_string(pdb::PdbIndex::kMaxProcInlinees) + " inlinees)";
        }
    }
    v.sites = sites->sites;
    v.inlinees = sites->inlinees.size();
    if (in_.inline_verdicts != nullptr) {
        // WP-55 (R-62): kept for the next plans against this image (only a verdict whose sites could be read).
        in_.inline_verdicts->store(pdb_.id(), ci_, proc,
                                   CachedInlineVerdict{p.rva, std::string(p.name), v.risk, v.wider, v.wider_kind, v.closure_tails, v.sites, v.inlinees, v.closures, v.unreadable});
    }
    CML_LOG(Trace, "WP51-BATCH proc=%s sites=%u inlinees=%zu closures=%zu unreadable=%zu verdict=%s", where.c_str(), sites->sites, sites->inlinees.size(), v.closures,
            v.unreadable, v.risk.empty() ? "no-closure" : "risk");
}

bool Planner::cached_verdict(std::uint32_t proc) {
    if (in_.inline_verdicts == nullptr || proc >= comp_syms_.size()) {
        return false;
    }
    const pdb::SymbolView& p = comp_syms_[proc];
    std::optional<CachedInlineVerdict> kept = in_.inline_verdicts->find(pdb_.id(), ci_, proc, p.rva, p.name);
    if (!kept) {
        return false;
    }
    InlineVerdict& v = inline_verdicts_[proc];
    v.risk = std::move(kept->risk);
    v.wider = std::move(kept->wider);
    v.wider_kind = kept->wider_kind;
    v.closure_tails = std::move(kept->closure_tails);
    v.sites = kept->sites;
    v.inlinees = kept->inlinees;
    v.closures = kept->closures;
    v.unreadable = kept->unreadable;
    v.cached = true;
    CML_LOG(Trace, "WP55-INLINE-CACHE proc='%s' rva=%s verdict=%s (kept from an earlier plan)", std::string(p.name).c_str(), hex(p.rva).c_str(),
            v.risk.empty() ? "no-closure" : "risk");
    return true;
}

const Planner::InlineVerdict& Planner::inline_verdict(std::uint32_t proc) {
    auto found = inline_verdicts_.find(proc);
    if (found == inline_verdicts_.end() && cached_verdict(proc)) {
        found = inline_verdicts_.find(proc);
    }
    if (found == inline_verdicts_.end()) {
        // (Review: one walk of the module stream for every procedure of the compiland that holds a record named like a static
        // of the patch, the only ones a static can be paired into, instead of one walk per procedure asked for.)
        std::vector<std::uint32_t> batch;
        if (!inline_batch_done_) {
            inline_batch_done_ = true;
            std::set<std::uint32_t> holders;
            for (const pdb::SymbolView& v : comp_syms_) {
                if (pdb::is_data_kind(v.kind) && v.parent_proc != pdb::kNoSymbol && v.parent_proc < comp_syms_.size() && pdb::is_proc_kind(comp_syms_[v.parent_proc].kind) &&
                    inline_verdicts_.count(v.parent_proc) == 0 && wp51_static_bases_.count(comp_last(&v)) != 0 && !cached_verdict(v.parent_proc)) {
                    holders.insert(v.parent_proc);
                }
            }
            holders.insert(proc);
            batch.assign(holders.begin(), holders.end());
        }
        bool judged = false;
        if (batch.size() > 1) {
            std::vector<pdb::SymbolView> views;
            for (const std::uint32_t p : batch) {
                views.push_back(comp_syms_[p]);
            }
            const Result<std::vector<pdb::PdbIndex::ProcInlinees>> all = pdb_.procs_inlinees(views);
            if (all) {
                for (std::size_t i = 0; i < batch.size(); ++i) {
                    judge_inlinees(batch[i], Result<pdb::PdbIndex::ProcInlinees>((*all)[i]));
                }
                CML_LOG(Trace, "WP51-BATCH compiland=%u read %zu procedure(s) in one walk", ci_, batch.size());
                judged = true;
            } else {
                // One of them could not be read: the asked procedure on its own, so the error is its own.
                CML_LOG(Trace, "WP51-BATCH compiland=%u: the batch of %zu procedure(s) failed (%s); read one by one", ci_, batch.size(), all.error().message.c_str());
            }
        }
        if (!judged) {
            judge_inlinees(proc, pdb_.proc_inlinees(comp_syms_[proc]));
        }
        found = inline_verdicts_.find(proc);
    }
    InlineVerdict& v = found->second;
    if (!v.asked) {
        // (Review: the summary counts only the procedures whose verdict was asked for, not the rest of the batch.)
        v.asked = true;
        if (v.cached) {
            in_.inline_verdicts->count_kept();  // WP-55: only verdicts asked for count, not the batch's probes
        }
        ++wp51_procs_;
        wp51_sites_ += v.sites;
        wp51_closures_ += v.closures;
        wp51_unreadable_ += v.unreadable;
        const pdb::SymbolView& p = comp_syms_[proc];
        CML_LOG(Trace, "WP51-INLINEES proc='%s' rva=%s sites=%u inlinees=%zu closures=%zu unreadable=%zu verdict=%s", std::string(p.name).c_str(), hex(p.rva).c_str(), v.sites,
                v.inlinees, v.closures, v.unreadable, v.risk.empty() ? "no-closure" : "risk");
    }
    return v;
}

std::string Planner::own_closure_scope(const Sym& sy) const {
    std::string scope = pdb::without_tu_discriminator(call_operator_scope(sy.name));
    // A closure paired with an old closure of another number by the code of its call operator (WP-40, rule (d)): its statics
    // are the old closure's.
    for (const auto& [from, to] : closure_pairing_) {
        if (from != to && scope.size() > from.size() && scope.compare(0, from.size(), from) == 0) {
            scope = to + scope.substr(from.size());
            break;
        }
    }
    return scope;
}

const pdb::SymbolView* Planner::own_closure_twin(const Sym& sy, const pdb::SymbolView& record) {
    if (!op_records_indexed_) {
        op_records_indexed_ = true;
        for (std::uint32_t i = 0; i < comp_syms_.size(); ++i) {
            const pdb::SymbolView& v = comp_syms_[i];
            if (pdb::is_data_kind(v.kind) && v.rva != pdb::kNoRva && v.parent_proc != pdb::kNoSymbol && v.parent_proc < comp_syms_.size() &&
                last_component(comp_syms_[v.parent_proc].name) == "operator()") {
                op_records_by_rva_[v.rva].push_back(i);
            }
        }
    }
    const auto at = op_records_by_rva_.find(record.rva);
    if (at == op_records_by_rva_.end()) {
        return nullptr;
    }
    const std::string scope = own_closure_scope(sy);
    for (const std::uint32_t i : at->second) {
        const pdb::SymbolView& v = comp_syms_[i];
        if (&v == &record || v.parent_proc == record.parent_proc) {
            continue;
        }
        // The call operator's closure, by its type (the unique name without ".?AV" and the unit's discriminator), must begin
        // the static's decorated scope: display names do not tell lambdas apart (clang names them `f::<lambda_0>` alike),
        // and a record of another closure's call operator is that closure's (o's static, inlined into k), not a twin.
        const ClosureId id = closure_of(v);
        if (id.known && !id.tail.empty() && !scope.empty() && starts_with(scope, id.tail)) {
            return &v;
        }
        CML_LOG(Trace, "WP53-NOT-TWIN static='%s' record rva=%s in '%s': closure %s is not the static's own (%s)", sy.dec.display.c_str(), hex(v.rva).c_str(),
                std::string(comp_syms_[v.parent_proc].name).c_str(), id.known ? (id.tail.empty() ? "(none)" : id.tail.c_str()) : ("unknown: " + id.why).c_str(),
                scope.c_str());
    }
    return nullptr;
}

bool Planner::refuse_inlined_owner(Sym& sy, const pdb::SymbolView& record, const std::string& encl, const char* step) {
    const std::uint32_t pos = static_cast<std::uint32_t>(&sy - syms_.data());
    if (sy.diagnosed || sy.dec.type == SymbolType::Function || is_guard_variable(sy.name) || record.parent_proc == pdb::kNoSymbol ||
        record.parent_proc >= comp_syms_.size() || wp51_checked_.count(pos) != 0) {
        return false;
    }
    if (encl.empty() || last_component(record.parent_name) != encl) {
        // Not in the static's own function: a lambda's static looked for in the function its call operator was inlined into
        // (WP-40, R-48 item 1), decided by the layout or the place there.
        CML_LOG(Trace, "WP51-SKIP static='%s' record rva=%s in '%s': not the procedure of its own function '%s' (%s)", sy.dec.display.c_str(), hex(record.rva).c_str(),
                std::string(record.parent_name).c_str(), encl.c_str(), step);
        return false;
    }
    if (encl == "operator()") {
        // (Review: every call operator is named so.) The record is the static's own only when its procedure is the call operator
        // of the static's own closure. An inner lambda's static recorded in the outer lambda's call operator, which inlined
        // it, is WP-40's case. When the closure of either cannot be told, the record counts as the static's own and is checked.
        // (Second review: skipped only when that is proven, i.e. the record's procedure is another closure's call operator AND
        // it inlined the code of the static's own closure. A lambda renumbered without rule (d), or a scope the compiler spells
        // with other back-references than the closure's unique name, is checked like the static's own.)
        const std::string scope = own_closure_scope(sy);
        const ClosureId id = closure_of(record);
        if (id.known && !id.tail.empty() && !scope.empty() && !starts_with(scope, id.tail)) {
            const InlineVerdict& host = inline_verdict(record.parent_proc);
            const std::string* inlined_own = nullptr;
            for (const std::string& tail : host.closure_tails) {
                if (starts_with(scope, tail)) {
                    inlined_own = &tail;
                    break;
                }
            }
            if (inlined_own != nullptr) {
                CML_LOG(Trace, "WP51-SKIP static='%s' record rva=%s in '%s': the call operator of closure %s, which inlined the static's own closure %s (%s)",
                        sy.dec.display.c_str(), hex(record.rva).c_str(), std::string(record.parent_name).c_str(), id.tail.c_str(), inlined_own->c_str(), step);
                return false;
            }
            CML_LOG(Trace, "WP51-OWN static='%s' record rva=%s in '%s': the call operator of closure %s, which did not inline the static's own closure (%s): checked (%s)",
                    sy.dec.display.c_str(), hex(record.rva).c_str(), std::string(record.parent_name).c_str(), id.tail.c_str(), scope.c_str(), step);
        }
        CML_LOG(Trace, "WP51-OWN static='%s' record rva=%s in '%s': closure %s (%s)", sy.dec.display.c_str(), hex(record.rva).c_str(), std::string(record.parent_name).c_str(),
                id.known ? (id.tail.empty() ? "(none)" : id.tail.c_str()) : ("unknown: " + id.why).c_str(), step);
    }
    wp51_checked_.insert(pos);
    const InlineVerdict& v = inline_verdict(record.parent_proc);
    if (v.risk.empty()) {
        CML_LOG(Trace, "WP51-PAIR static='%s' record rva=%s in '%s': no closure inlined there (%s)", sy.dec.display.c_str(), hex(record.rva).c_str(),
                std::string(record.parent_name).c_str(), step);
        if (!v.wider.empty()) {
            ++wp51_would_refuse_;
            CML_LOG(Info, "WP51-WOULD-REFUSE static=%s inlinee=%s kind=%s proc='%s' rva=%s", sy.dec.display.c_str(), v.wider.c_str(), v.wider_kind,
                    std::string(record.parent_name).c_str(), hex(comp_syms_[record.parent_proc].rva).c_str());
        }
        return false;
    }
    ++wp51_refused_;
    CML_LOG(Info, "WP51-OWN-STATIC static='%s' refused (%s; %s)", sy.dec.display.c_str(), v.risk.c_str(), step);
    error(diag::kAmbiguousStatic,
          "static data '" + sy.dec.display + "' (" + sy.name + ") in " + in_.object_name + " cannot be paired with the record '" + std::string(record.name) + "' at rva " +
              hex(record.rva) + " of its function in compiland " + std::to_string(ci_) + ": " + v.risk +
              ", and a compiler records the statics of inlined code as records of the calling procedure under their own names, so the record may be that code's static and not this one",
          "Rename the static, or hot-restart.");
    sy.diagnosed = true;
    decide(sy, DecisionKind::PatchNewData, BindSource::None, sy.dec.type, {}, "own static of a function that inlined a lambda (CML3207)");
    return true;
}

const std::string& Planner::plain_name(const Sym& sy) {
    if (plain_names_.size() != syms_.size()) {
        plain_names_.assign(syms_.size(), std::string());
        for (std::size_t i = 0; i < syms_.size(); ++i) {
            plain_names_[i] = pdb::without_tu_discriminator(syms_[i].name);
        }
    }
    return plain_names_[static_cast<std::size_t>(&sy - syms_.data())];
}

bool Planner::in_closure(const Sym& sy, const std::string& closure) {
    return plain_name(sy).find("??R" + closure) != std::string::npos;
}

const Sym* Planner::closure_twin(const Sym& sy, const std::string& from, const std::string& to) {
    const std::string& name = plain_name(sy);
    const std::size_t at = name.find("??R" + from);
    if (at == std::string::npos) {
        return nullptr;
    }
    const std::string twin = name.substr(0, at + 3) + to + name.substr(at + 3 + from.size());
    for (const Sym& t : syms_) {
        if (t.defined && t.dec.section == sy.dec.section && plain_name(t) == twin) {
            return &t;
        }
    }
    return nullptr;
}

void Planner::note_lambda(const Sym& sy, const std::string& base, const std::string& own_scope, const std::string& old_closure) {
    const std::size_t first = old_closure.find('@');
    const std::size_t own_first = own_scope.find('@');
    if (first == std::string::npos || own_first == std::string::npos) {
        return;
    }
    const std::string function = old_closure.substr(first + 1);
    if (own_scope.compare(own_first + 1, function.size(), function) != 0) {
        return;
    }
    const std::string closure = own_scope.substr(0, own_first + 1 + function.size());
    LambdaNote& note = lambda_notes_[closure];
    if (note.function.empty()) {
        note.function = function;
        note.display = lambda_enclosing(sy.dec.display, base);
        note.old_closure = old_closure;
        for (const Sym& f : syms_) {
            if (f.defined && f.dec.type == SymbolType::Function && starts_with(plain_name(f), "??R" + closure)) {
                note.call_operator = f.name;
                break;
            }
        }
    }
    note.statics.push_back(sy.name);
}

// The lambdas of one enclosing function whose call operators have the same signature but for the closure's number. Their
// statics were paired by that number; the pipeline confirms the pairing or corrects it by the code of the call operators.
void Planner::build_lambda_sets() {
    std::map<std::string, std::map<std::string, std::vector<std::string>>> by_function;  // function -> signature -> closures
    for (const auto& [closure, note] : lambda_notes_) {
        if (note.call_operator.empty()) {
            continue;
        }
        const Sym* op = nullptr;
        for (const Sym& f : syms_) {
            if (f.name == note.call_operator) {
                op = &f;
                break;
            }
        }
        if (op == nullptr) {
            continue;
        }
        const pdb::FunctionSignature& sig = new_signature(*op);
        by_function[note.function][sig.known ? canonical_lambda_names(sig.text) : "?" + closure].push_back(closure);
    }
    for (const auto& [function, by_signature] : by_function) {
        for (const auto& [signature, closures] : by_signature) {
            if (closures.size() < 2) {
                continue;
            }
            LambdaSet set;
            set.function = lambda_notes_[closures.front()].display;
            for (const std::string& closure : closures) {
                const LambdaNote& note = lambda_notes_[closure];
                LambdaEntry e;
                e.closure = closure;
                e.old_closure = note.old_closure;
                e.call_operator = note.call_operator;
                e.statics = note.statics;
                set.lambdas.push_back(std::move(e));
            }
            CML_LOG(Trace, "bind: lambdas of '%s': %zu call operators with the signature %s cannot be told apart by their types", set.function.c_str(), closures.size(),
                    signature.c_str());
            plan_.lambda_sets.push_back(std::move(set));
        }
    }
}

bool Planner::named_by_debug_record(const Sym& sy) {
    if (!debug_named_loaded_) {
        debug_named_loaded_ = true;
        for (const coff::Section& sec : obj_.sections()) {
            if (!starts_with(sec.name, ".debug$S")) {
                continue;
            }
            const Result<std::vector<coff::ResolvedRelocation>> rr = obj_.resolve_relocations(sec);
            if (!rr) {
                continue;  // nothing is named: the pieces are refused
            }
            for (const coff::ResolvedRelocation& r : *rr) {
                if (r.reloc.type == coff::RelocType::SecRel) {
                    debug_named_.insert(r.target->index);
                }
            }
        }
    }
    return debug_named_.count(sy.s->index) != 0;
}

const Planner::ScopeKeys& Planner::scope_keys(std::uint32_t pos) {
    const auto it = scope_keys_.find(pos);
    if (it != scope_keys_.end()) {
        return it->second;
    }
    const std::string& name = syms_[pos].name;
    ScopeKeys keys;
    keys[0] = local_scope_key(name, false);
    if (is_guard_variable(name)) {
        keys[1] = local_scope_key(name, true);
    }
    return scope_keys_.emplace(pos, std::move(keys)).first->second;
}

namespace {

// WP-52 (convergence ruling B): the version string of the first S_COMPILE3 (0x113C) in the object's `.debug$S` sections
// (CodeView C13: signature 4, then subsections; DEBUG_S_SYMBOLS 0xF1 holds the records). Empty when there is none or the
// sections cannot be read.
std::string object_compiler_version(const coff::Object& obj) {
    for (const coff::Section& sec : obj.sections()) {
        if (!starts_with(sec.name, ".debug$S") || sec.data.size() < 8) {
            continue;
        }
        const auto* b = reinterpret_cast<const std::uint8_t*>(sec.data.data());
        const std::size_t n = sec.data.size();
        const auto u16 = [b](std::size_t at) { return static_cast<std::uint32_t>(b[at] | (b[at + 1] << 8)); };
        const auto u32 = [b](std::size_t at) {
            return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) | (static_cast<std::uint32_t>(b[at + 2]) << 16) |
                   (static_cast<std::uint32_t>(b[at + 3]) << 24);
        };
        if (u32(0) != 4) {
            continue;
        }
        std::size_t at = 4;
        while (at + 8 <= n) {
            const std::uint32_t type = u32(at);
            const std::size_t len = u32(at + 4);
            at += 8;
            if (len > n - at) {
                break;
            }
            if (type == 0xF1) {
                std::size_t r = at;
                const std::size_t end = at + len;
                while (r + 4 <= end) {
                    const std::size_t reclen = u16(r);
                    if (reclen < 2 || r + 2 + reclen > end) {
                        break;
                    }
                    if (u16(r + 2) == 0x113C && reclen >= 2 + 22) {
                        std::string version;
                        for (std::size_t k = r + 4 + 22; k < r + 2 + reclen && b[k] != 0; ++k) {
                            version += static_cast<char>(b[k]);
                        }
                        return version;
                    }
                    r += 2 + reclen;
                }
            }
            at += len;
            at = (at + 3) & ~static_cast<std::size_t>(3);
        }
    }
    return {};
}

}  // namespace

Planner::GuardSpelling Planner::guard_spelling(bool bitmask) {
    if (compiler_family_ < 0) {
        compiler_family_ = 0;
        compiler_version_ = object_compiler_version(obj_);
        const std::string& version = compiler_version_;
        if (version.find("Microsoft") != std::string::npos) {
            compiler_family_ = 1;
        } else if (const std::size_t at = version.find("clang version "); at != std::string::npos) {
            // "clang version 18.1.8 (...)", "Ubuntu clang version 18.1.3 (1ubuntu1)", "clang version 19.0.0git": major.minor.
            std::size_t k = at + 14;
            int major = 0;
            int minor = -1;
            for (; k < version.size() && version[k] >= '0' && version[k] <= '9' && major < 1000; ++k) {
                major = major * 10 + (version[k] - '0');
            }
            if (k < version.size() && version[k] == '.') {
                minor = 0;
                for (++k; k < version.size() && version[k] >= '0' && version[k] <= '9' && minor < 1000; ++k) {
                    minor = minor * 10 + (version[k] - '0');
                }
            }
            if (major > 0 && minor >= 0 && k > at + 14) {
                compiler_family_ = 2;
                clang_major_ = major;
                clang_minor_ = minor;
            }
        }
        CML_LOG(Info, "WP52-GUARD-SPELLING object=%s compiler='%s' family=%s clang=%d.%d", in_.object_name.c_str(), version.c_str(),
                compiler_family_ == 1 ? "cl" : (compiler_family_ == 2 ? "clang" : "unknown"), clang_major_, clang_minor_);
    }
    if (compiler_family_ == 1) {
        return bitmask ? GuardSpelling::Unknown : GuardSpelling::Counted;
    }
    if (compiler_family_ == 2) {
        if (clang_major_ == 19 && clang_minor_ == 0) {
            return GuardSpelling::Unknown;  // 19.0.x: before 19.1.0, after 18; not a release, either way possible
        }
        if (bitmask) {
            return GuardSpelling::NotCounted;
        }
        return clang_major_ >= 19 ? GuardSpelling::Counted : GuardSpelling::NotCounted;
    }
    return GuardSpelling::Unknown;
}

// WP-52 (R-60): a guard is tied to a static when their scopes, with the back-references resolved, are the same (the same
// block; for a bitmask, the same function). The guard's own name is counted in the back-reference table or not, as the
// compiler that wrote the patch object does it (guard_spelling). Where that is not known, both spellings are read: they agree
// on any scope without a back-reference; where they do not tie the guard to the same statics (also where only one of
// them ties it), which is right cannot be told and the guard is tied to none (settle_guards then refuses it when a bound
// static may be its own, CML3211). A name the resolver cannot read is compared as written (raw_guard_covers, the rule
// before WP-52).
const std::vector<std::uint32_t>& Planner::guard_scope_statics(const Sym& guard) {
    const std::uint32_t gpos = static_cast<std::uint32_t>(&guard - syms_.data());
    if (const auto it = guard_statics_.find(gpos); it != guard_statics_.end()) {
        return it->second;
    }
    // WP-59 (R-67): clang's `??_B` of an inline function is a bitmask too. It, the thread-safe `$TSS` guards of an inline
    // function (review item 0) and that function's statics are external: an external guard ties external statics, and never
    // a thread_local (whose guards are others: `??__J`, CML3400).
    const bool inline_bitmask = is_inline_bitmask_guard(guard.name);
    const bool external_guard = inline_bitmask || !guard.is_static;
    const bool bitmask = inline_bitmask || guard.name.compare(0, 3, "?$S") == 0;
    const GuardSpelling spelling = guard_spelling(bitmask);
    const std::array<bool, 2> use = {spelling != GuardSpelling::Counted, spelling != GuardSpelling::NotCounted};
    const ScopeKeys gk = scope_keys(gpos);
    const bool guard_read = (use[0] && gk[0].has_value()) || (use[1] && gk[1].has_value());
    std::array<std::vector<std::uint32_t>, 2> tied;
    for (std::uint32_t pos = 0; pos < syms_.size(); ++pos) {
        const Sym& s = syms_[pos];
        if (!s.defined || (!s.is_static && !external_guard) || s.dec.type != SymbolType::Data || s.name.empty() || s.name[0] != '?' || is_guard_variable(s.name)) {
            continue;
        }
        if (external_guard && s.dec.section != 0 && secs_[s.dec.section - 1].tls) {
            continue;
        }
        const std::optional<LocalScopeKey>& sk = scope_keys(pos)[0];
        if (!guard_read || !sk) {
            if (raw_guard_covers(guard.name, s.name)) {
                CML_LOG(Trace, "bind: guard %s: tied to %s by the scope as written (%s cannot be read)", guard.name.c_str(), s.name.c_str(),
                        guard_read ? "the static's" : "the guard's");
                tied[0].push_back(pos);
                tied[1].push_back(pos);
            }
            continue;
        }
        for (std::size_t m = 0; m < 2; ++m) {
            if (use[m] && gk[m] && scope_keys_cover(*gk[m], *sk, bitmask)) {
                tied[m].push_back(pos);
            }
        }
    }
    std::vector<std::uint32_t> out;
    if (spelling == GuardSpelling::NotCounted) {
        out = tied[0];
    } else if (spelling == GuardSpelling::Counted) {
        out = tied[1];
    } else if (tied[0] == tied[1]) {
        out = tied[0];
    } else {
        std::vector<std::uint32_t> either = tied[0];
        either.insert(either.end(), tied[1].begin(), tied[1].end());
        CML_LOG(Trace, "bind: guard %s: the compiler's spelling of back-references is not known, and the two spellings tie it to %zu and %zu static(s): tied to none",
                guard.name.c_str(), tied[0].size(), tied[1].size());
        guard_ambiguous_[gpos] = std::move(either);
    }
    return guard_statics_.emplace(gpos, std::move(out)).first->second;
}

// WP-52 (review, finding 2): does the old image record a guard variable in the procedure that holds the old record `st` is
// bound to? (cl records its guards; clang records none, so this is false for every clang compiland.)
bool Planner::old_scope_has_guard_record(const Sym& st) {
    if (st.dec.old_address.rva == kNoRva || !ensure_compiland_symbols()) {
        return false;
    }
    std::uint32_t parent = pdb::kNoSymbol;
    for (const pdb::SymbolView& v : comp_syms_) {
        if (pdb::is_data_kind(v.kind) && v.rva == st.dec.old_address.rva && v.parent_proc != pdb::kNoSymbol) {
            parent = v.parent_proc;
            break;
        }
    }
    if (parent == pdb::kNoSymbol) {
        return false;
    }
    for (const pdb::SymbolView& v : comp_syms_) {
        if (pdb::is_data_kind(v.kind) && v.parent_proc == parent && is_guard_variable("?" + std::string(v.name) + "@")) {
            return true;
        }
    }
    return false;
}

// WP-52 (R-60; ADR-0045 and its convergence ruling): the rule is the static's. Every function-local static of the patch
// bound to the running process that needs a guard must have a guard tied to it that is bound to the old guard: otherwise
// its guard (if any) is fresh and its initialiser runs again over the live object, and the static is refused (CML3211).
// Nothing here matches functions. A static needs a guard when something says it may have one:
//   * a guard is tied to it (but that guard is not bound to the old one);
//   * an untied guard's scope, in one of its two readings, is the static's (the patch's spelling is not known);
//   * an untied guard's scope cannot be read at all (it may be any static's);
//   * the static's own scope cannot be read, and the patch has an untied guard;
//   * the old image records a guard in the procedure that holds the static's old record (cl records its guards).
// A static none of these applies to has no guard (`static int n = 0;`). Statics whose tied guard already carries an
// error (CML3600, ...) and thread-locals (WP-24, CML3400) are left to those diagnostics.
std::set<std::uint32_t> Planner::require_bound_guards(const std::vector<std::uint32_t>& untied) {
    std::set<std::uint32_t> implicated;
    std::map<std::uint32_t, std::vector<std::uint32_t>> tied_to;  // static -> the guards tied to it
    for (std::uint32_t pos = 0; pos < syms_.size(); ++pos) {
        const Sym& g = syms_[pos];
        if (g.defined && g.dec.type == SymbolType::Data && is_guard_variable(g.name)) {  // WP-59: external guards as well
            for (const std::uint32_t st : guard_scope_statics(g)) {
                tied_to[st].push_back(pos);
            }
        }
    }
    const auto unreadable = [this](std::uint32_t pos) {
        const ScopeKeys& k = scope_keys(pos);
        return !k[0].has_value() && !k[1].has_value();
    };
    for (std::uint32_t pos = 0; pos < syms_.size(); ++pos) {
        const Sym& s = syms_[pos];
        // WP-59 (R-67): an external static counts where a guard is tied to it (an inline function's, by its external guard).
        if (!s.defined || (!s.is_static && tied_to.count(pos) == 0) || s.dec.type != SymbolType::Data || s.name.size() < 2 || s.name[0] != '?' || s.name[1] == '?' ||
            is_guard_variable(s.name) || s.diagnosed || !s.decided || s.dec.decision != DecisionKind::BindOldData) {
            continue;
        }
        if (s.dec.section != 0 && secs_[s.dec.section - 1].tls) {
            continue;
        }
        const std::size_t at = s.name.find('@');
        if (at == std::string::npos || block_prefix_length(std::string_view(s.name).substr(at + 1)) == 0) {
            continue;  // not function-local (its first scope is no block of a function)
        }
        bool bound_guard = false;
        bool diagnosed_guard = false;
        const Sym* unbound = nullptr;
        if (const auto it = tied_to.find(pos); it != tied_to.end()) {
            for (const std::uint32_t g : it->second) {
                const Sym& guard = syms_[g];
                if (guard.diagnosed) {
                    diagnosed_guard = true;
                } else if (guard.decided && guard.dec.decision == DecisionKind::BindOldData) {
                    bound_guard = true;
                } else {
                    unbound = &guard;
                }
            }
        }
        if (bound_guard || diagnosed_guard) {
            continue;
        }
        std::string why;
        std::uint32_t culprit = UINT32_MAX;
        if (unbound != nullptr) {
            why = "its guard " + describe(*unbound) + " is tied to it but not bound to the old guard";
        }
        for (std::size_t k = 0; why.empty() && k < untied.size(); ++k) {
            const auto it = guard_ambiguous_.find(untied[k]);
            if (it != guard_ambiguous_.end() && std::find(it->second.begin(), it->second.end(), pos) != it->second.end()) {
                why = "the guard " + describe(syms_[untied[k]]) + " may be its: one reading of that guard's name gives this static's scope, and which reading is right is not known (" +
                      (compiler_version_.empty() ? std::string("the object records no compiler") : "the object's compiler is '" + compiler_version_ + "'") + ")";
                culprit = untied[k];
            }
        }
        for (std::size_t k = 0; why.empty() && k < untied.size(); ++k) {
            if (unreadable(untied[k])) {
                why = "the scope of the guard " + describe(syms_[untied[k]]) + ", which is tied to no static, cannot be read, so it may be this static's";
                culprit = untied[k];
            }
        }
        if (why.empty() && !scope_keys(pos)[0].has_value() && !untied.empty()) {
            why = "its own scope cannot be read, so the guard " + describe(syms_[untied.front()]) + ", which is tied to no static, may be its";
            culprit = untied.front();
        }
        if (why.empty() && old_scope_has_guard_record(s)) {
            why = "the old image records a guard in the procedure of its old record, and no guard of the patch is tied to it";
        }
        if (why.empty()) {
            CML_LOG(Trace, "bind: %s is bound and has no guard tied to it, and nothing says it has one: it needs none", s.name.c_str());
            continue;
        }
        CML_LOG(Trace, "bind: %s is bound without a bound guard: %s (CML3211)", s.name.c_str(), why.c_str());
        error(diag::kGuardUnmatched,
              "the function-local static " + describe(s) + " in " + in_.object_name +
                  " is bound to the running process, and no guard of the patch that is bound to the old one is tied to it: " + why +
                  "; with a fresh guard its initialiser would run again over the live object",
              "Hot-restart. If this function was not edited, report the decorated names of the static and of the patch's guards (the trace log lists them as 'bind: guard ...').");
        if (culprit != UINT32_MAX) {
            implicated.insert(culprit);
        }
    }
    return implicated;
}

bool Planner::guard_ties(std::uint32_t guard_pos, std::uint32_t static_pos) {
    const std::vector<std::uint32_t>& statics = guard_scope_statics(syms_[guard_pos]);
    return std::find(statics.begin(), statics.end(), static_pos) != statics.end();
}

std::uint64_t Planner::module_size() {
    if (module_size_ == 0) {
        for (const pdb::SectionHeader& h : pdb_.sections()) {
            module_size_ = std::max<std::uint64_t>(module_size_, static_cast<std::uint64_t>(h.rva) + std::max(h.virtual_size, h.raw_size));
        }
    }
    return module_size_;
}

// Can a REL32 from EVERY place the patch may be loaded reach the target? The patch is reserved within
// +-1.5 GiB of the module (ADR-0020 #8, plan 4.7); rel32 reaches +-2 GiB. Targets of earlier patches
// (registry) are checked when the patch base is known (make_fixup_triples).
bool Planner::rel32_reachable(const SymbolDecision& d) {
    if (d.old_address.rva == kNoRva && d.source != BindSource::ExportOtherModule) {
        return true;
    }
    constexpr std::int64_t kWindow = 0x60000000;
    constexpr std::int64_t kLimit = 0x7FFFFFF0;
    const std::int64_t base = static_cast<std::int64_t>(in_.module_base);
    const std::int64_t end = base + static_cast<std::int64_t>(module_size());
    const std::int64_t t = static_cast<std::int64_t>(d.old_address.va);
    return t - (base - kWindow) <= kLimit && (end + kWindow) - t <= kLimit;
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

// ---- WP-37: signatures and closure identity ----------------------------------------------------

const pdb::FunctionSignature& Planner::old_signature(const pdb::SymbolView& proc) {
    const auto it = old_sigs_.find(proc.index);
    if (it != old_sigs_.end()) {
        return it->second;
    }
    return old_sigs_.emplace(proc.index, pdb_.proc_signature(proc)).first->second;
}

ObjectSignatures& Planner::object_debug() {
    if (!new_sigs_) {
        new_sigs_ = std::make_unique<ObjectSignatures>(obj_, in_.object_name);
    }
    return *new_sigs_;
}

const pdb::FunctionSignature& Planner::new_signature(const Sym& sy) {
    return object_debug().at(sy.dec.section, sy.s->value);
}

Planner::ClosureId Planner::closure_of(const pdb::SymbolView& local) {
    ClosureId id;
    if (local.parent_proc == pdb::kNoSymbol || local.parent_proc >= comp_syms_.size()) {
        id.known = true;  // not function-local: no closure
        return id;
    }
    const pdb::FunctionSignature& sig = old_signature(comp_syms_[local.parent_proc]);
    if (!sig.known) {
        id.why = sig.why;
        return id;
    }
    id.known = true;
    // ".?AV<lambda_1>@?1??f@@YAXXZ@": '.', '?', 'A', the class key ('V' class, 'U' struct), then the qualified name.
    const std::string u = pdb::without_tu_discriminator(sig.class_unique_name);
    if (sig.member && u.size() > 4 && u[0] == '.' && u[1] == '?' && u[2] == 'A') {
        id.tail = u.substr(4);
    }
    return id;
}

void Planner::learn_delta(const Sym& sy, std::uint32_t old_rva) {
    if (sy.dec.section == 0 || secs_[sy.dec.section - 1].sec->is_comdat() || delta_conflict_.count(sy.dec.section) != 0) {
        return;
    }
    const std::int64_t d = static_cast<std::int64_t>(old_rva) - static_cast<std::int64_t>(sy.s->value);
    const std::optional<pdb::SectionOffset> so = pdb_.section_offset_from_rva(old_rva);
    const std::uint16_t image_section = so ? so->section : 0;
    const auto it = delta_.find(sy.dec.section);
    if (it == delta_.end()) {
        delta_[sy.dec.section] = d;
        delta_image_section_[sy.dec.section] = image_section;
    } else if (it->second != d || delta_image_section_[sy.dec.section] != image_section) {
        delta_conflict_.insert(sy.dec.section);
    }
}

std::string Planner::own_public(const pdb::SymbolView& v, const std::string& closure) {
    if (v.rva == pdb::kNoRva || closure.empty()) {
        return {};
    }
    // (WP-46 review: with identical-code folding another function's public can sit at a static call operator's address; only
    // the call operator's own decorated name, "??R" + its closure, says that this record has external linkage.)
    const std::string prefix = "??R" + closure;
    const Result<std::vector<pdb::SymbolView>> pubs = pdb_.publics_at_rva(v.rva);
    for (std::size_t k = 0; pubs && k < pubs->size(); ++k) {
        const std::string name((*pubs)[k].name);
        if (starts_with(pdb::without_tu_discriminator(name), prefix)) {
            return name;
        }
    }
    return {};
}

bool Planner::taken_by_public(const pdb::SymbolView& v, const std::string& closure) {
    if (!external_defined_built_) {
        external_defined_built_ = true;
        for (const Sym& sy : syms_) {
            if (sy.defined && !sy.is_static) {
                external_defined_.insert(sy.name);
            }
        }
    }
    const std::string name = own_public(v, closure);
    return !name.empty() && external_defined_.count(name) != 0;
}

bool Planner::is_deleting_destructor(const pdb::SymbolView& v) {
    if (!pdb::is_proc_kind(v.kind)) {
        return false;
    }
    const Result<pdb::ProcInfo> info = pdb_.proc_info(v);
    if (!info) {
        return false;
    }
    for (const pdb::ProcVariable& var : info->variables) {
        if (var.is_param && var.name == "should_call_delete") {
            return true;
        }
    }
    return false;
}

bool Planner::compiland_records_tss_guards() {
    if (records_tss_guards_ < 0) {
        records_tss_guards_ = 0;
        if (!ensure_compiland_symbols()) {
            records_tss_guards_ = 1;  // unknown: as if it recorded them (the relaxation does not apply)
        } else {
            for (const pdb::SymbolView& v : comp_syms_) {
                if (pdb::is_data_kind(v.kind) && comp_last(&v).compare(0, 4, "$TSS") == 0) {
                    records_tss_guards_ = 1;
                    break;
                }
            }
        }
    }
    return records_tss_guards_ == 1;
}

// WP-46 (F-41.1): the layout settle_guards places guards without a record from. It is delta_ (what pass A and the closure step
// learned) plus what the statics paired by their function alone teach (place_taught_), with the same rules as learn_delta (no
// COMDAT section, one delta and one image section per object section): a disagreement drops the section's layout, also the one
// delta_ had. No other step reads it, so no pairing of a static depends on it.
void Planner::learn_guard_layout() {
    guard_delta_ = delta_;
    guard_delta_image_section_ = delta_image_section_;
    std::set<std::uint32_t> conflict;
    std::map<std::uint32_t, std::size_t> teachers;  // object section -> statics paired by function that teach it
    for (const auto& [pos, old_rva] : place_taught_) {
        const Sym& sy = syms_[pos];
        if (sy.diagnosed || !sy.decided || sy.dec.decision != DecisionKind::BindOldData || sy.dec.old_address.rva != old_rva) {
            continue;  // refused after all (two statics on one record, ...)
        }
        if (sy.dec.section == 0 || secs_[sy.dec.section - 1].sec->is_comdat() || delta_conflict_.count(sy.dec.section) != 0) {
            continue;
        }
        const std::int64_t d = static_cast<std::int64_t>(old_rva) - static_cast<std::int64_t>(sy.s->value);
        const std::optional<pdb::SectionOffset> so = pdb_.section_offset_from_rva(old_rva);
        const std::uint16_t image_section = so ? so->section : 0;
        const auto it = guard_delta_.find(sy.dec.section);
        if (it == guard_delta_.end()) {
            guard_delta_[sy.dec.section] = d;
            guard_delta_image_section_[sy.dec.section] = image_section;
        } else if (it->second != d || guard_delta_image_section_[sy.dec.section] != image_section) {
            conflict.insert(sy.dec.section);
        }
        ++teachers[sy.dec.section];
        CML_LOG(Trace, "bind: guard layout: %s (paired by its function) puts object section %u at delta %lld", sy.name.c_str(), sy.dec.section, static_cast<long long>(d));
    }
    // (Review: one static says where IT is, not where the rest of the section went; an unguarded static inserted between it and
    // its guard shifts the guard unseen. A layout the statics paired by function teach is used only when at least two of them
    // agree, or when it agrees with the layout pass A learned.)
    for (const auto& [sec, count] : teachers) {
        if (count < 2 && delta_.count(sec) == 0 && conflict.count(sec) == 0) {
            guard_delta_.erase(sec);
            CML_LOG(Trace, "bind: guard layout: object section %u: one static paired by its function and no other layout: not used", sec);
        }
    }
    for (const std::uint32_t sec : conflict) {
        guard_delta_.erase(sec);
        CML_LOG(Trace, "bind: guard layout: object section %u: the statics paired by their function disagree with each other or with the known layout: no layout", sec);
    }
    if (!place_taught_.empty()) {
        CML_LOG(Info, "WP46-GUARD-LAYOUT object=%s taught=%zu sections_known=%zu conflicts=%zu", in_.object_name.c_str(), place_taught_.size(), guard_delta_.size(), conflict.size());
    }
}

std::string Planner::elimination_problem(const std::string& fresh, const std::string& old, const Sym& op, const pdb::SymbolView& old_op) {
    const auto renamed = [&](std::string text) {
        for (std::size_t at = text.find(fresh); at != std::string::npos; at = text.find(fresh, at + old.size())) {
            text.replace(at, fresh.size(), old);
        }
        return text;
    };
    // 1. The call operator's signature (the closure's name aside).
    const pdb::FunctionSignature& new_sig = new_signature(op);
    const pdb::FunctionSignature& old_sig = old_signature(old_op);
    if (!new_sig.known || !old_sig.known) {
        return "the signature of a call operator cannot be read (" + (new_sig.known ? old_sig.why : new_sig.why) + ")";
    }
    if (renamed(new_sig.text) != old_sig.text) {
        return "their call operators have different signatures (" + new_sig.text + " in the patch, " + old_sig.text + " in the old image)";
    }
    // 2. The statics of the call operator: names and canonical types.
    ObjectSignatures& debug = object_debug();
    const ObjectSignatures::ProcStatics* new_statics = debug.statics_of(op.dec.section, op.s->value);
    if (new_statics == nullptr || !new_statics->usable) {
        return "the statics of the patch's call operator cannot be read" + (new_statics == nullptr ? std::string() : " (" + new_statics->why + ")");
    }
    bool complete = true;
    const Result<std::vector<pdb::ProcStatic>> old_statics = pdb_.proc_statics(old_op, &complete);
    if (!old_statics || !complete) {
        return "the statics of the old call operator cannot be read";
    }
    std::vector<std::string> new_list;
    for (const ObjectSignatures::LocalStatic& s : new_statics->statics) {
        std::string why;
        const std::optional<std::string> type = debug.type_text(s.record.type_index, &why);
        if (!type) {
            return "the type of the patch's static '" + s.record.name + "' cannot be read: " + why;
        }
        new_list.push_back(last_component(s.record.name) + ":" + renamed(*type));
    }
    std::vector<std::string> old_list;
    for (const pdb::ProcStatic& s : *old_statics) {
        std::string why;
        const std::optional<std::string> type = pdb::canonical_type(pdb_.types(), s.type_index, &why);
        if (!type) {
            return "the type of the old static '" + s.name + "' cannot be read: " + why;
        }
        old_list.push_back(last_component(s.name) + ":" + *type);
    }
    std::sort(new_list.begin(), new_list.end());
    std::sort(old_list.begin(), old_list.end());
    if (new_list != old_list) {
        return "their call operators have different statics (" + std::to_string(new_list.size()) + " in the patch, " + std::to_string(old_list.size()) +
               " in the old image, or other names or types)";
    }
    // 3. The closure's layout (its captures): the definitions of the two classes.
    const std::string old_unique = ".?AV" + old + "@@";
    const std::string new_unique = ".?AV" + fresh + "@@";
    const Result<const std::vector<pdb::ClosureClass>*> closures = pdb_.closure_classes();
    if (!closures) {
        return "the closure classes of the old image cannot be read (" + closures.error().message + ")";
    }
    std::uint32_t old_definition = 0;
    for (const pdb::ClosureClass& c : **closures) {
        if (c.unique == old_unique) {
            old_definition = c.definition;
        }
    }
    std::string why;
    const std::optional<std::string> old_layout = old_definition == 0 ? std::nullopt : pdb::udt_layout(pdb_.types(), old_definition, &why);
    if (!old_layout) {
        return "the layout of the old closure cannot be read (" + (old_definition == 0 ? std::string("no definition of ") + old_unique : why) + ")";
    }
    const std::optional<std::string> new_layout = debug.class_layout(new_unique, &why);
    if (!new_layout) {
        return "the layout of the patch's closure cannot be read (" + why + ")";
    }
    if (renamed(*new_layout) != *old_layout) {
        return "the closures have different layouts (" + *new_layout + " in the patch, " + *old_layout + " in the old image)";
    }
    // 4. The enclosing function: the one procedure that holds a local of the closure's type, on both sides, and they are one
    // function.
    const Result<std::vector<pdb::PdbIndex::LocalTypeUse>> uses = pdb_.compiland_local_types(ci_);
    if (!uses) {
        return "the local variables of the old compiland cannot be read (" + uses.error().message + ")";
    }
    // (Second review: a function whose own name mentions the closure, `run<lambda>(F f)`, uses the closure (a template over
    // it) and does not create it; canonically the old and new instances would even look alike. Such holders are left out, and
    // parameters do not count as locals at all.)
    std::set<std::uint32_t> old_holder_rvas;
    for (const pdb::PdbIndex::LocalTypeUse& u : *uses) {
        if (pdb::class_unique_name_of(pdb_.types(), u.type_index) == old_unique) {
            old_holder_rvas.insert(u.proc_rva);
        }
    }
    std::vector<const pdb::SymbolView*> old_holders;
    std::size_t old_users = 0;
    for (const std::uint32_t rva : old_holder_rvas) {
        const pdb::SymbolView* proc = nullptr;
        for (const pdb::SymbolView& v : comp_syms_) {
            if (pdb::is_proc_kind(v.kind) && v.rva == rva) {
                proc = &v;
                break;
            }
        }
        if (proc == nullptr) {
            return "a procedure of the old image that holds the lambda has no record";
        }
        if (std::string_view(proc->name).find(old) != std::string_view::npos) {
            ++old_users;
            continue;
        }
        old_holders.push_back(proc);
    }
    std::vector<const Sym*> holders;
    std::size_t new_users = 0;
    for (const std::uint64_t key : debug.functions_with_local(new_unique)) {
        const std::uint32_t section = static_cast<std::uint32_t>(key >> 32);
        const std::uint32_t value = static_cast<std::uint32_t>(key & 0xFFFFFFFFull);
        const Sym* f = nullptr;
        for (const Sym& s : syms_) {
            if (s.defined && s.dec.type == SymbolType::Function && s.dec.section == section && s.s->value == value) {
                f = &s;
                break;
            }
        }
        if (f == nullptr) {
            return "a procedure of the patch that holds the lambda has no symbol";
        }
        if (f->name.find(fresh) != std::string::npos) {
            ++new_users;
            continue;
        }
        holders.push_back(f);
    }
    if (old_holders.size() != 1 || holders.size() != 1) {
        return "the function that holds the lambda is not known on both sides (" + std::to_string(holders.size()) + " procedure(s) of the patch and " +
               std::to_string(old_holders.size()) + " of the old image have a local variable of the closure's type, not counting " + std::to_string(new_users) + " and " +
               std::to_string(old_users) + " that are templates over the closure)";
    }
    const pdb::SymbolView* old_holder = old_holders.front();
    const std::uint32_t old_rva = old_holder->rva;
    const Sym* holder = holders.front();
    std::size_t old_alike = 0;
    if (holder->decided) {
        if (holder->dec.decision != DecisionKind::PatchHooked || !holder->dec.old_address.valid || holder->dec.old_address.rva != old_rva) {
            return "the lambda is held by '" + holder->dec.display + "' in the patch and by '" + std::string(old_holder->name) + "' in the old image";
        }
        return {};
    }
    // A static function not yet bridged: the same name (canonically) and the same signature, and no other old function alike.
    const pdb::FunctionSignature& holder_new = new_signature(*holder);
    const pdb::FunctionSignature& holder_old = old_signature(*old_holder);
    const std::string display = canonical_lambda_names(holder->dec.display);
    if (!holder_new.known || !holder_old.known || renamed(holder_new.text) != holder_old.text || comp_norm(old_holder).canonical != display) {
        return "the lambda is held by '" + holder->dec.display + "' in the patch and by '" + std::string(old_holder->name) + "' in the old image";
    }
    for (const pdb::SymbolView& v : comp_syms_) {
        if (pdb::is_proc_kind(v.kind) && comp_norm(&v).canonical == display) {
            const pdb::FunctionSignature& s = old_signature(v);
            old_alike += s.known && s.text == holder_old.text ? 1u : 0u;
        }
    }
    if (old_alike != 1) {
        return "'" + holder->dec.display + "', which holds the lambda, matches " + std::to_string(old_alike) + " old functions";
    }
    return {};
}

std::string Planner::closure_user_state(const std::string& fresh, const std::string& old) {
    // The patch: writable data whose decorated name holds the new closure without being a static of its call operator: a
    // function-local static (or guard) of a template over it (`?r@?1???$run@V<lambda_X>@@...`), or a variable whose type is it.
    // Its name changed with the closure's, so it would be a new variable. Read-only data (vtables, RTTI of such templates) is
    // a fresh copy anyway and holds no state.
    for (const Sym& sy : syms_) {
        if (!sy.defined || sy.dec.type != SymbolType::Data || sy.dec.section == 0 || sy.name.find(fresh) == std::string::npos) {
            continue;
        }
        const SecInfo& sec = secs_[sy.dec.section - 1];
        if (sec.runtime_table || (sec.sec->characteristics & coff::scn::MemWrite) == 0 || hashed_closure_static(sy.name) == fresh) {
            continue;
        }
        CML_LOG(Info, "WP46-ELIMINATION object=%s new=%s user-state=%s refused", in_.object_name.c_str(), fresh.c_str(), sy.name.c_str());
        return "the patch's data " + describe(sy) + " belongs to a user of the lambda (a template over it, or a variable of its type), whose name changes with the lambda's: it would be constructed again and the running one abandoned";
    }
    // The old image: data recorded in a procedure that names the old closure without being one of its members
    // ("run<<lambda_Y>>" holds `r`; the closure's own members are "<lambda_Y>::operator()", "<lambda_Y>::<lambda_invoker_cdecl>").
    if (!ensure_compiland_symbols()) {
        return "the old records of compiland " + std::to_string(ci_) + " cannot be read";
    }
    for (const pdb::SymbolView& v : comp_syms_) {
        if (!pdb::is_data_kind(v.kind)) {
            continue;
        }
        const std::string_view owner = v.parent_proc != pdb::kNoSymbol && v.parent_proc < comp_syms_.size() ? comp_syms_[v.parent_proc].name : v.name;
        if (owner.find(old) == std::string_view::npos || starts_with(owner, old + "::")) {
            continue;
        }
        CML_LOG(Info, "WP46-ELIMINATION object=%s old=%s user-state=%s in '%s' refused", in_.object_name.c_str(), old.c_str(), std::string(v.name).c_str(), std::string(owner).c_str());
        return "the old image's static '" + std::string(v.name) + "' of '" + std::string(owner) +
               "' belongs to a user of the lambda (a template over it), whose name changes with the lambda's: its state would be abandoned and constructed again";
    }
    return {};
}

void Planner::pair_hashed_by_elimination() {
    closure_pairing_ = in_.closure_pairing;
    struct Side {
        bool external = false;   // its call operator has external linkage (a lambda of an inline function)
        bool statics = false;    // it has function-local statics
        std::string op;          // the call operator's decorated name (patch) / display name (old)
        const pdb::SymbolView* proc = nullptr;  // old: the call operator's procedure
    };
    std::map<std::string, Side> new_side;  // "<lambda_X>" (scope-less hash closures only)
    for (const Sym& sy : syms_) {
        if (!sy.defined) {
            continue;
        }
        const std::string c = hashed_closure(sy.name);
        if (c.empty()) {
            continue;
        }
        if (sy.dec.type == SymbolType::Function && starts_with(sy.name, "??R" + c + "@@")) {
            Side& s = new_side[c];
            s.external = s.external || !sy.is_static;
            s.op = sy.name;
        } else if (sy.dec.type == SymbolType::Data && sy.name.find("???R" + c + "@@") != std::string::npos) {
            new_side[c].statics = true;
        }
    }
    if (new_side.empty() || !ensure_compiland_symbols()) {
        return;
    }
    std::map<std::string, Side> old_side;
    for (const pdb::SymbolView& v : comp_syms_) {
        if (pdb::is_proc_kind(v.kind) && starts_with(v.name, "<lambda_") && comp_last(&v) == "operator()") {
            const std::string c = hashed_closure(v.name);
            if (!c.empty() && v.name.size() > c.size() + 1 && v.name.compare(c.size(), 2, "::") == 0) {
                Side& s = old_side[c];
                s.op = std::string(v.name);
                s.proc = &v;
            }
        }
    }
    for (const pdb::SymbolView& v : comp_syms_) {
        if (pdb::is_data_kind(v.kind) && v.parent_proc != pdb::kNoSymbol && v.parent_proc < comp_syms_.size()) {
            const std::string c = hashed_closure(comp_syms_[v.parent_proc].name);
            const auto it = old_side.find(c);
            if (!c.empty() && it != old_side.end()) {
                it->second.statics = true;
            }
        }
    }
    for (auto& [c, s] : old_side) {
        s.external = s.proc != nullptr && !own_public(*s.proc, c + "@@").empty();
    }
    // What is left on each side after the names are matched. A lambda with external linkage pairs by its public name; one
    // that has no partner of its name (an inline function's lambda that is new, or gone) only counts when it has statics:
    // they would be fresh while an old lambda's statics vanished (round 2 ruling, points 2 and 3).
    // Static (internal-linkage) lambdas left over always count: the set rule refuses them anyway, and one on each side is the
    // edited lambda. External ones (lambdas of inline functions, which pair by their public name and are otherwise new or gone)
    // count only where statics could be lost: a new one with statics while an old one with statics vanished.
    std::vector<std::string> new_left;
    std::vector<std::string> old_left;
    std::vector<std::string> new_external;  // external, with statics, no old function of its name anywhere
    std::vector<std::string> old_external;  // external, with statics, not defined by the patch
    bool new_statics = false;
    bool old_statics = false;
    for (const auto& [c, s] : new_side) {
        if (old_side.count(c) != 0) {
            continue;
        }
        if (s.external) {
            bool has_public = false;
            if (!s.op.empty()) {
                const Result<std::vector<pdb::SymbolView>> pubs = pdb_.find_public(s.op);
                has_public = pubs && !pubs->empty();
            }
            if (!has_public && s.statics) {
                new_external.push_back(c);
                new_statics = true;
            }
            continue;  // otherwise paired by its public name (its old function is in another unit), or nothing to lose
        }
        new_left.push_back(c);
        new_statics = new_statics || s.statics;
    }
    for (const auto& [c, s] : old_side) {
        if (new_side.count(c) != 0 || (s.proc != nullptr && taken_by_public(*s.proc, c + "@@"))) {
            continue;
        }
        if (s.external) {
            if (s.statics) {
                old_external.push_back(c);
                old_statics = true;
            }
            continue;
        }
        old_left.push_back(c);
        old_statics = old_statics || s.statics;
    }
    const bool external_left = new_statics && old_statics && (!new_external.empty() || !old_external.empty());
    if (external_left) {
        new_left.insert(new_left.end(), new_external.begin(), new_external.end());
        old_left.insert(old_left.end(), old_external.begin(), old_external.end());
    }
    if (new_left.empty() && old_left.empty()) {
        return;
    }
    const auto names = [](const std::vector<std::string>& list) {
        std::string text;
        for (std::size_t i = 0; i < list.size() && i < 8; ++i) {
            text += (text.empty() ? "" : ", ") + list[i];
        }
        return list.size() > 8 ? text + " and " + std::to_string(list.size() - 8) + " more" : text;
    };
    if (new_left.size() == 1 && old_left.size() == 1 && !external_left) {
        // WP-46 (review): being the only ones left is no evidence that they are one lambda (one removed from f and one added
        // to g are left too). They must agree in every respect the records can show.
        const Side& fresh = new_side[new_left[0]];
        const Side& gone = old_side[old_left[0]];
        const Sym* op = nullptr;
        for (const Sym& sy : syms_) {
            if (sy.defined && sy.name == fresh.op) {
                op = &sy;
                break;
            }
        }
        std::string problem = op == nullptr || gone.proc == nullptr ? std::string("one of them has no call operator")
                                                                    : elimination_problem(new_left[0], old_left[0], *op, *gone.proc);
        if (problem.empty()) {
            problem = closure_user_state(new_left[0], old_left[0]);
        }
        if (!problem.empty()) {
            CML_LOG(Info, "WP46-ELIMINATION object=%s new=%s old=%s refused (%s)", in_.object_name.c_str(), new_left[0].c_str(), old_left[0].c_str(), problem.c_str());
            error(diag::kHashedLambdasUnproven,
                  "the lambdas of " + in_.object_name + " are named by a hash of their own code (cl's legacy lambda processor); the one left in the patch (" + new_left[0] +
                      ") and the one left in the old image (" + old_left[0] + ") cannot be shown to be the same lambda: " + problem,
                  "Change at most one lambda of this file per reload (adding or removing a lambda counts), compile with /Zc:lambda (the default under /std:c++20), or hot-restart.");
            return;
        }
        eliminated_[new_left[0]] = old_left[0];
        closure_pairing_[new_left[0] + "@@"] = old_left[0] + "@@";
        CML_LOG(Info, "WP46-ELIMINATION object=%s new=%s old=%s (the one lambda left on each side; proven only if every other lambda is unchanged)", in_.object_name.c_str(),
                new_left[0].c_str(), old_left[0].c_str());
        return;
    }
    CML_LOG(Info, "WP46-ELIMINATION object=%s new_left=%zu old_left=%zu external=%d refused", in_.object_name.c_str(), new_left.size(), old_left.size(), external_left ? 1 : 0);
    error(diag::kHashedLambdasUnproven,
          "the lambdas of " + in_.object_name +
              " are named by a hash of their own code (cl's legacy lambda processor), and after their names are matched " + std::to_string(new_left.size()) +
              " lambda(s) of the patch (" + (new_left.empty() ? std::string("none") : names(new_left)) + ") and " + std::to_string(old_left.size()) +
              " of the old image (" + (old_left.empty() ? std::string("none") : names(old_left)) +
              ") are left; only one changed lambda on each side can be paired (by elimination), so which old lambda (and which old statics) each new one is cannot be told",
          "Change at most one lambda of this file per reload (adding or removing a lambda counts), compile with /Zc:lambda (the default under /std:c++20), or hot-restart.");
}

void Planner::note_hashed_lambdas() {
    std::map<std::string, HashedLambda> by_closure;
    for (const Sym& sy : syms_) {
        if (!sy.defined || !sy.is_static || !sy.decided || sy.diagnosed) {
            continue;
        }
        const bool hooked = sy.dec.type == SymbolType::Function && sy.dec.decision == DecisionKind::PatchHooked;
        const bool bound = sy.dec.type == SymbolType::Data && sy.dec.decision == DecisionKind::BindOldData;
        if (!hooked && !bound) {
            continue;
        }
        // (Review: only the closure's own members: its call operator, conversion and invokers, and the statics declared in its
        // call operator. A function that merely mentions the closure, `apply<lambda>`, proves nothing about it, and a static
        // whose TYPE is the closure is not one of its statics.)
        const std::string closure = hooked ? hashed_closure_member(sy.name) : hashed_closure_static(sy.name);
        if (closure.empty()) {
            continue;
        }
        HashedLambda& h = by_closure[closure];
        h.closure = closure;
        const auto eliminated = eliminated_.find(closure);
        h.old_closure = eliminated == eliminated_.end() ? std::string() : eliminated->second;
        (hooked ? h.functions : h.statics).push_back(sy.name);
    }
    for (auto& [closure, h] : by_closure) {
        if (h.functions.empty()) {
            // Its statics were paired by the hash name, and no function of the closure is hooked to an old one whose code
            // could prove that this is the same lambda.
            for (const std::string& name : h.statics) {
                for (Sym& sy : syms_) {
                    if (sy.name != name) {
                        continue;
                    }
                    error(diag::kHashedLambdasUnproven,
                          "static data " + describe(sy) + " in " + in_.object_name + " belongs to lambda '" + closure +
                              "', which the compiler names by a hash; it was paired with the old static of that name, but no function of the lambda is hooked to an old one whose code could prove that it is the same lambda",
                          "Compile with /Zc:lambda (or /std:c++20), keep the lambda's code unchanged, or hot-restart.");
                    sy.diagnosed = true;
                    decide(sy, DecisionKind::PatchNewData, BindSource::None, sy.dec.type, {}, "static of a hash-named lambda without a hooked function (CML3630)");
                }
            }
            CML_LOG(Info, "WP46-HASHED-LAMBDA closure=%s statics=%zu functions=0 refused", closure.c_str(), h.statics.size());
            continue;
        }
        CML_LOG(Trace, "bind: hashed lambda %s: %zu function(s), %zu static(s) paired by its name: to be proven by code", closure.c_str(), h.functions.size(), h.statics.size());
        plan_.hashed_lambdas.push_back(std::move(h));
    }
    if (!plan_.hashed_lambdas.empty()) {
        CML_LOG(Info, "WP46-HASHED-LAMBDAS object=%s closures=%zu (proven by code in the pipeline)", in_.object_name.c_str(), plan_.hashed_lambdas.size());
    }
}

// Static functions that share scope and name (overloads; the call operators of same-numbered lambdas of overloaded
// functions): one-to-one by canonical signature. Never by position, never by a decorated name. Members that get a
// partner move to `matched`; a member whose signature no old function has is a new function (decided here); what is
// left in `unmatched` is ambiguous and `why` says so. If any signature of the group is unreadable nothing is paired
// (an unknown signature could be equal to any other) and the result is false: the caller falls back to what the
// planner did before it had types.
bool Planner::pair_overloads_by_signature(const std::string& group_name, std::vector<std::uint32_t>& unmatched,
                                          std::vector<const pdb::SymbolView*>& free_cands, std::map<std::uint32_t, Match>& matched,
                                          std::map<std::uint32_t, std::string>& why) {
    std::vector<const pdb::FunctionSignature*> new_sig;
    std::vector<const pdb::FunctionSignature*> old_sig;
    std::string unreadable;
    std::vector<std::unique_ptr<pdb::FunctionSignature>> renamed;  // WP-40: signatures under an overridden closure name
    for (const std::uint32_t pos : unmatched) {
        const Sym& sy = syms_[pos];
        const pdb::FunctionSignature* sigp = &new_signature(sy);
        // WP-40 (R-48 item 5, rule (d)): this call operator's lambda is the old lambda of another number (the pipeline
        // compared their code): it is matched under that closure's name.
        for (const auto& [from, to] : closure_pairing_) {
            if (from != to && sigp->known && starts_with(plain_name(sy), "??R" + from)) {
                auto copy = std::make_unique<pdb::FunctionSignature>(*sigp);
                copy->class_unique_name = pdb::without_tu_discriminator(copy->class_unique_name);
                for (std::string* text : {&copy->text, &copy->class_unique_name}) {
                    for (std::size_t at = text->find(from); at != std::string::npos; at = text->find(from, at + to.size())) {
                        text->replace(at, from.size(), to);
                    }
                }
                CML_LOG(Trace, "bind: overloads '%s': %s is matched as closure %s (by the code of its call operator)", group_name.c_str(), sy.name.c_str(), to.c_str());
                sigp = copy.get();
                renamed.push_back(std::move(copy));
                break;
            }
        }
        const pdb::FunctionSignature& sig = *sigp;
        new_sig.push_back(&sig);
        CML_LOG(Trace, "bind: overloads '%s': new %s signature=%s", group_name.c_str(), sy.name.c_str(), sig.known ? sig.text.c_str() : ("unknown (" + sig.why + ")").c_str());
        if (!sig.known && unreadable.empty()) {
            unreadable = "the signature of '" + sy.name + "' in " + in_.object_name + " cannot be read: " + sig.why;
        }
    }
    // WP-46 (F-41.3): clang-cl records a deleting destructor (`??_G`, `??_E`) under the destructor's display name and with the
    // destructor's type, so the old records of a destructor are the destructor AND its deleting destructor, which no signature
    // tells apart. Only the deleting destructor has the implicit parameter `should_call_delete` (a destructor has none): such
    // a record is not a candidate of the destructor. The patch's deleting destructor is a symbol of its own (`??_G...`). Where
    // the parameter is not recorded the records stay ambiguous (CML3214), as before.
    if (!group_name.empty() && group_name[0] == '~') {
        std::size_t dropped = 0;
        for (auto it = free_cands.begin(); it != free_cands.end();) {
            if (is_deleting_destructor(**it)) {
                CML_LOG(Trace, "bind: overloads '%s': old record '%s' at rva %s has the parameter 'should_call_delete': a deleting destructor, not a candidate", group_name.c_str(),
                        std::string((*it)->name).c_str(), hex((*it)->rva).c_str());
                it = free_cands.erase(it);
                ++dropped;
            } else {
                ++it;
            }
        }
        if (dropped != 0) {
            CML_LOG(Info, "WP46-DELETING-DTOR group='%s' dropped=%zu left=%zu", group_name.c_str(), dropped, free_cands.size());
        }
    }
    for (const pdb::SymbolView* c : free_cands) {
        const pdb::FunctionSignature& sig = old_signature(*c);
        old_sig.push_back(&sig);
        CML_LOG(Trace, "bind: overloads '%s': old record '%s' rva=%s signature=%s", group_name.c_str(), std::string(c->name).c_str(), hex(c->rva).c_str(),
                sig.known ? sig.text.c_str() : ("unknown (" + sig.why + ")").c_str());
        if (!sig.known && unreadable.empty()) {
            unreadable = "the signature of the old function '" + std::string(c->name) + "' at rva " + hex(c->rva) + " cannot be read from the PDB of " + in_.module_name + ": " + sig.why;
        }
    }
    if (!unreadable.empty()) {
        CML_LOG(Trace, "bind: overloads '%s': not paired by signature: %s", group_name.c_str(), unreadable.c_str());
        for (const std::uint32_t pos : unmatched) {
            why[pos] = "the types needed to tell the overloads apart cannot be read (" + unreadable + ")";
        }
        return false;
    }
    // A group in which NOT ONE new signature occurs among the old ones is not "every overload is new": it is far more
    // likely that the two sides spell a type differently (a compiler or linker this reader has not met) than that
    // every overload was replaced at once, and deciding "new" would silently leave every old function unhooked.
    // That is doubt, and doubt refuses: the group stays ambiguous (CML3214) and says why.
    if (!free_cands.empty()) {
        bool any_match = false;
        for (const pdb::FunctionSignature* n : new_sig) {
            for (const pdb::FunctionSignature* o : old_sig) {
                any_match = any_match || n->text == o->text;
            }
        }
        if (!any_match) {
            CML_LOG(Trace, "bind: overloads '%s': none of the %zu new signature(s) occurs among the %zu old one(s): not paired", group_name.c_str(), new_sig.size(), old_sig.size());
            for (std::size_t m = 0; m < unmatched.size(); ++m) {
                why[unmatched[m]] = "none of the " + std::to_string(new_sig.size()) + " signature(s) in the patch occurs among the " + std::to_string(old_sig.size()) +
                                    " of the old image (new: " + new_sig[m]->text + "; old: " + old_sig.front()->text + "), so the functions cannot be told from new ones";
            }
            return true;
        }
    }
    // Call operators of lambdas. The class in such a signature is the closure, and a closure's name is a number the
    // compiler assigns ("<lambda_1>"): it says which lambda this is only while the lambdas of the enclosing function are
    // the same as before. Insert a lambda in front of another and cl renumbers them; the old "<lambda_1>" would be hooked
    // to the new, unrelated body (another capture layout behind a stored std::function). So, as for the statics of
    // lambdas (pair_statics_by_closure): the call operators of a function pair only when the SET of its closures is the
    // same in the old image and in the patch; otherwise they stay ambiguous. A function none of whose lambdas is in the
    // old image is not restricted (its call operators are new functions).
    const auto closure_of_signature = [](const pdb::FunctionSignature& sig) {
        const std::string u = pdb::without_tu_discriminator(sig.class_unique_name);
        return sig.member && u.size() > 12 && u.compare(0, 3, ".?A") == 0 && u.compare(4, 8, "<lambda_") == 0 ? u.substr(4) : std::string();
    };
    const auto function_of_closure = [](const std::string& closure) {
        const std::size_t at = closure.find('@');
        return at == std::string::npos ? std::string() : closure.substr(at + 1);
    };
    std::map<std::string, std::set<std::string>> old_closures;  // enclosing function -> its closures with a call operator
    std::map<std::string, std::set<std::string>> new_closures;
    std::vector<std::string> member_function(unmatched.size());
    for (std::size_t m = 0; m < unmatched.size(); ++m) {
        const std::string closure = closure_of_signature(*new_sig[m]);
        if (!closure.empty()) {
            member_function[m] = function_of_closure(closure);
            new_closures[member_function[m]].insert(closure);
        }
    }
    // WP-46 (F-41.2): an old call operator with external linkage (a lambda of an inline function, a COMDAT the linker took
    // from this translation unit) that the patch defines under the same decorated name is paired by that public name
    // (decide_defined), like every external function; it is not one of the lambdas this group of static call operators pairs.
    // Counted here it made the sets differ whenever the linker had kept such COMDATs from this unit (Jolt, cl: 39 call
    // operators in the patch against 90 records, the other 51 external).
    std::size_t taken_by_public_count = 0;
    for (std::size_t c = 0; c < old_sig.size(); ++c) {
        const std::string closure = closure_of_signature(*old_sig[c]);
        if (closure.empty()) {
            continue;
        }
        // (Round 2: cl's hash of a lambda follows its code, so an edited external lambda of a hash-named set is renamed: its old
        // function is not defined by the patch at all. Such an old function has a public and pairs, or not, by that name; it is
        // never one of the static call operators of this set. Its statics are pair_hashed_by_elimination's concern.)
        // (Review: only the call operator's OWN public counts. With identical-code folding another function's public can sit at
        // the address of a static call operator, whose record must then stay in the set.)
        const bool external_hashed = !hashed_closure(closure).empty() && !own_public(*free_cands[c], closure).empty();
        if (external_hashed || taken_by_public(*free_cands[c], closure)) {
            CML_LOG(Trace, "bind: overloads '%s': old record '%s' at rva %s has external linkage (%s): not in the set of lambdas", group_name.c_str(),
                    std::string(free_cands[c]->name).c_str(), hex(free_cands[c]->rva).c_str(), external_hashed ? "its own public" : "the patch defines its own public");
            ++taken_by_public_count;
            continue;
        }
        old_closures[function_of_closure(closure)].insert(closure);
    }
    if (taken_by_public_count != 0) {
        CML_LOG(Info, "WP46-PUBLIC-LAMBDAS group='%s' old=%zu external=%zu", group_name.c_str(), old_sig.size(), taken_by_public_count);
    }
    // WP-46 (D1 evidence): for closures cl names by a hash, how the two sets compare (do the hashes of unchanged lambdas
    // stay when lambdas are added or removed?).
    for (const auto& [function, new_set] : new_closures) {
        const auto old_set = old_closures.find(function);
        if (old_set == old_closures.end() || new_set.empty() || hashed_closure(*new_set.begin()).empty()) {
            continue;
        }
        std::size_t common = 0;
        for (const std::string& c : new_set) {
            common += old_set->second.count(c);
        }
        CML_LOG(Info, "WP46-HASH-SET group='%s' scope='%s' old=%zu new=%zu common=%zu %s", group_name.c_str(), function.c_str(), old_set->second.size(), new_set.size(), common,
                old_set->second == new_set ? "same" : "changed");
    }
    std::vector<bool> lambdas_changed(unmatched.size(), false);
    for (std::size_t m = 0; m < unmatched.size(); ++m) {
        const auto old_set = member_function[m].empty() ? old_closures.end() : old_closures.find(member_function[m]);
        if (old_set == old_closures.end()) {
            continue;
        }
        const std::set<std::string>& new_set = new_closures[member_function[m]];
        if (old_set->second != new_set) {
            lambdas_changed[m] = true;
            CML_LOG(Trace, "bind: overloads '%s': %s: the lambdas of its function changed (%zu call operator(s) in the old image, %zu in the patch): not paired", group_name.c_str(),
                    syms_[unmatched[m]].name.c_str(), old_set->second.size(), new_set.size());
            why[unmatched[m]] = "the lambdas of the enclosing function are not the same as in the old image (" + std::to_string(old_set->second.size()) +
                                " call operator(s) there, " + std::to_string(new_set.size()) +
                                " in the patch); a lambda's compiler-assigned number is its only identity, and the numbers may have shifted";
        }
    }
    std::vector<bool> member_done(unmatched.size(), false);
    std::vector<bool> cand_taken(free_cands.size(), false);
    for (std::size_t m = 0; m < unmatched.size(); ++m) {
        if (lambdas_changed[m]) {
            continue;  // stays in `unmatched`: CML3214 with the reason
        }
        Sym& sy = syms_[unmatched[m]];
        std::size_t same_new = 0;
        for (const pdb::FunctionSignature* other : new_sig) {
            same_new += other->text == new_sig[m]->text ? 1u : 0u;
        }
        std::vector<std::size_t> hits;
        for (std::size_t c = 0; c < free_cands.size(); ++c) {
            if (old_sig[c]->text == new_sig[m]->text) {
                hits.push_back(c);
            }
        }
        if (hits.empty()) {
            // No old function of this name and scope has the signature: a new overload. (An old overload without a
            // partner simply stays as it is.)
            CML_LOG(Trace, "bind: overloads '%s': %s has a signature no old overload has: new function", group_name.c_str(), sy.name.c_str());
            decide(sy, DecisionKind::PatchNewFunction, BindSource::None, sy.dec.type, {},
                   "static overload whose signature no old function named '" + group_name + "' has (" + new_sig[m]->text + "): new function, not hooked");
            member_done[m] = true;
        } else if (hits.size() == 1 && same_new == 1) {
            CML_LOG(Trace, "bind: overloads '%s': %s paired by signature with the old record at rva %s", group_name.c_str(), sy.name.c_str(), hex(free_cands[hits[0]]->rva).c_str());
            matched[unmatched[m]] = {free_cands[hits[0]], BindSource::CompilandSignature};
            cand_taken[hits[0]] = true;
            member_done[m] = true;
        } else {
            CML_LOG(Trace, "bind: overloads '%s': %s is ambiguous: %zu new and %zu old functions have its signature", group_name.c_str(), sy.name.c_str(), same_new, hits.size());
            why[unmatched[m]] = std::to_string(same_new) + " new and " + std::to_string(hits.size()) + " old functions have the same signature (" + new_sig[m]->text + ")";
        }
    }
    std::vector<std::uint32_t> left;
    for (std::size_t m = 0; m < unmatched.size(); ++m) {
        if (!member_done[m]) {
            left.push_back(unmatched[m]);
        }
    }
    unmatched = std::move(left);
    std::vector<const pdb::SymbolView*> free_left;
    for (std::size_t c = 0; c < free_cands.size(); ++c) {
        if (!cand_taken[c]) {
            free_left.push_back(free_cands[c]);
        }
    }
    free_cands = std::move(free_left);
    return true;
}

// Same-named function-local statics of several lambdas (Godot's SNAME: `sname` and its guard in every lambda). The
// old record belongs to the lambda whose closure class has the unique name ".?AV" + T; the new symbol's decorated
// scope starts with the same T (bind_names.h: call_operator_scope).
//
// A lambda's name is a number the compiler assigns ("<lambda_2>"), so it identifies the lambda only while the lambdas
// of the function are the same as before. The pairing is therefore made only when the closures that hold a static of
// this name are the SAME SET in the old image and in the patch (for the enclosing function of the group); if a lambda
// with such a static was added or removed the numbers may have shifted, nothing is paired and nothing is guessed from
// the section layout either (`no_layout`): the statics stay ambiguous (CML3207). When the old image has no such
// static in any lambda of the function, the new ones are new.
void Planner::pair_statics_by_closure(const std::string& base, std::vector<std::uint32_t>& unmatched, std::vector<const pdb::SymbolView*>& free_cands,
                                      std::map<std::uint32_t, Match>& matched, std::map<std::uint32_t, std::string>& why, std::set<std::uint32_t>& no_layout) {
    // The members that live in an operator(), with their scope and the scope without its first component (the
    // closure's own name): what is left names the enclosing function.
    const auto after_first_component = [](std::string_view text) {
        const std::size_t at = text.find('@');
        return at == std::string_view::npos ? std::string_view() : text.substr(at + 1);
    };
    // Both sides are compared without the translation unit's hash (closure_of): the decorated scope of a lambda in an
    // anonymous namespace, or of a function with a parameter of such a type, spells it ("?A0xa8f45668@") like the
    // closure's unique name does.
    std::vector<std::size_t> scoped;  // indices into unmatched
    std::vector<std::string> scope_text(unmatched.size());
    std::vector<std::string_view> scope(unmatched.size());
    std::vector<std::string> own_scope(unmatched.size());   // WP-40: the scope as the patch spells it (before an override)
    std::vector<bool> overridden(unmatched.size(), false);  // WP-40: paired with another closure than its number names
    for (std::size_t m = 0; m < unmatched.size(); ++m) {
        scope_text[m] = pdb::without_tu_discriminator(call_operator_scope(syms_[unmatched[m]].name));
        own_scope[m] = scope_text[m];
        // WP-40 (R-48 item 5, rule (d)): the pipeline compared the code of the call operators and found that this
        // closure's lambda is the old lambda of ANOTHER number (they changed places): the static is looked for in that one.
        for (const auto& [from, to] : closure_pairing_) {
            if (from != to && scope_text[m].size() > from.size() && scope_text[m].compare(0, from.size(), from) == 0) {
                scope_text[m] = to + scope_text[m].substr(from.size());
                overridden[m] = true;
                CML_LOG(Trace, "bind: closure statics '%s': %s: closure %s is paired with the old %s (by the code of its call operator)", base.c_str(),
                        syms_[unmatched[m]].name.c_str(), from.c_str(), to.c_str());
                break;
            }
        }
        scope[m] = scope_text[m];
        if (!scope[m].empty() && !after_first_component(scope[m]).empty()) {
            scoped.push_back(m);
        }
    }
    if (scoped.empty()) {
        return;
    }
    std::vector<ClosureId> ids;
    std::string unreadable;
    for (const pdb::SymbolView* c : free_cands) {
        ids.push_back(closure_of(*c));
        const ClosureId& id = ids.back();
        CML_LOG(Trace, "bind: closure statics '%s': old record at rva %s in '%s' closure=%s", base.c_str(), hex(c->rva).c_str(), std::string(c->parent_name).c_str(),
                id.known ? (id.tail.empty() ? "(none)" : id.tail.c_str()) : ("unknown (" + id.why + ")").c_str());
        // (WP-40: only a call operator's type can identify a lambda; a record of the enclosing function itself, which the
        // candidates include since WP-40, needs none.)
        if (!id.known && unreadable.empty() && last_component(c->parent_name) == "operator()") {
            unreadable = "the type of the old function '" + std::string(c->parent_name) + "' cannot be read from the PDB of " + in_.module_name + ": " + id.why;
        }
    }
    // An independent second opinion on which old records belong to lambdas of a function with this NAME (a display name
    // has no parameter list, so the lambdas of every overload are meant): the display name of the record's enclosing
    // proc ("`f'::`2'::<lambda_1>::operator()" under cl, "f::<lambda_0>::operator()" under clang-cl) against the display
    // name of the members' enclosing function, lambda numbers aside. The closure classes must not say less: read as the
    // class of a call operator, the closure's unique name must give that display name for every such record. A record
    // the names put into a lambda of the function and the closure classes do not is spelled in a way this reader does
    // not know, and nothing is decided ("no closure matches, so the statics are new" would restart them silently). The
    // other direction is harmless: a closure whose unique name IS the decorated scope of a member identifies its record
    // exactly, however the linker spelled the procedure's display name.
    std::string members_function = canonical_lambda_names(syms_[unmatched[scoped.front()]].dec.display);
    const std::string suffix = "::" + base;
    if (members_function.size() > suffix.size() && members_function.compare(members_function.size() - suffix.size(), suffix.size(), suffix) == 0) {
        members_function.resize(members_function.size() - suffix.size());
    }
    std::vector<bool> by_name(free_cands.size(), false);
    std::vector<bool> by_closure_name(free_cands.size(), false);
    std::size_t old_by_name = 0;
    std::size_t old_by_closure_name = 0;
    for (std::size_t c = 0; c < free_cands.size(); ++c) {
        const std::uint32_t parent = free_cands[c]->parent_proc;
        by_name[c] = parent != pdb::kNoSymbol && parent < comp_syms_.size() && comp_norm(&comp_syms_[parent]).canonical == members_function;
        old_by_name += by_name[c] ? 1u : 0u;
        by_closure_name[c] = !ids[c].tail.empty() && canonical_lambda_names(display_name("??R" + ids[c].tail)) == members_function;
        old_by_closure_name += by_closure_name[c] ? 1u : 0u;
    }
    // Old records in a closure of the same enclosing function as a member (the decorated function: name, class and
    // parameter list; the lambdas of another overload are not among them), and which member each belongs to.
    std::vector<bool> in_function(free_cands.size(), false);
    std::vector<std::vector<std::size_t>> hits(unmatched.size());
    std::vector<std::size_t> cand_hits(free_cands.size(), 0);
    std::size_t old_in_function = 0;
    for (std::size_t c = 0; c < free_cands.size(); ++c) {
        if (ids[c].tail.empty()) {
            continue;
        }
        const std::string_view function_part = after_first_component(ids[c].tail);
        for (const std::size_t m : scoped) {
            if (!function_part.empty() && starts_with(after_first_component(scope[m]), function_part)) {
                in_function[c] = true;
            }
            if (starts_with(scope[m], ids[c].tail)) {
                hits[m].push_back(c);
                ++cand_hits[c];
            }
        }
        old_in_function += in_function[c] ? 1u : 0u;
    }
    std::vector<bool> member_done(unmatched.size(), false);
    std::vector<bool> cand_taken(free_cands.size(), false);
    bool names_say_more = false;
    for (std::size_t c = 0; c < free_cands.size(); ++c) {
        names_say_more = names_say_more || (by_name[c] && !by_closure_name[c]);
    }
    // WP-40 (R-48 item 1): records of the name directly in the procedure of the lambdas' enclosing function. clang-cl
    // records the static of an inlined call operator there, without anything that says which lambda it belongs to. Then
    // no closure identifies the statics of this function and none of them is new: they are left to the section layout
    // (their RVA) and to their place in the function, which pair them or refuse. Only when the function has as many
    // records of the name in its lambdas (in their own procedures and inlined) as the patch has statics, though: a lambda
    // added or removed shifts the layout of the rest, and then nothing is placed by layout either.
    const std::string enclosing = lambda_enclosing(syms_[unmatched[scoped.front()]].dec.display, base);
    std::size_t old_inlined = 0;
    std::size_t old_of_function = 0;
    for (std::size_t c = 0; c < free_cands.size(); ++c) {
        bool inlined = is_inlined_record(free_cands[c], enclosing);
        // cl at /O2 /Ob2 records the static of an inlined lambda twice: in the lambda's own operator() (which it also
        // emits) and in the calling procedure, at the same address (CI, cl 19.44: "6 there, 3 in the patch"). A record in
        // the caller at the address of a record in a call operator is that same variable, not an inlined lambda's.
        // Only an equal RVA makes them one variable; a record in the caller at any other address stays an inlined lambda's
        // (counted, and the count decides as before).
        for (std::size_t o = 0; inlined && o < free_cands.size(); ++o) {
            if (o != c && free_cands[o]->rva == free_cands[c]->rva && (by_name[o] || by_closure_name[o] || !ids[o].tail.empty())) {
                inlined = false;
                CML_LOG(Info, "WP40-INLINED function='%s' statics=%s same-variable caller_rva=%s operator_rva=%s operator='%s'", enclosing.c_str(), base.c_str(),
                        hex(free_cands[c]->rva).c_str(), hex(free_cands[o]->rva).c_str(), std::string(free_cands[o]->parent_name).c_str());
            }
        }
        if (inlined) {
            CML_LOG(Info, "WP40-INLINED function='%s' statics=%s inlined-record caller_rva=%s (no record of a call operator at that address)", enclosing.c_str(), base.c_str(),
                    hex(free_cands[c]->rva).c_str());
        }
        old_inlined += inlined ? 1u : 0u;
        old_of_function += (inlined || by_name[c] || by_closure_name[c]) ? 1u : 0u;
    }
    // A record directly in the function may be the function's own static's or an inlined lambda's, which nothing tells
    // apart. The function's own statics of the name are refused (pass A, own_static_lambda_risk: WP-40 second high review,
    // ADR-0043 ruling; until then a record in the function was taken for the own static's whenever every lambda static of
    // the patch had a record in a call operator, so a new own static could take a removed inlined lambda's live record).
    // They are still counted here, so that the lambdas' statics are left to the layout only if the counts agree.
    std::size_t own_in_patch = 0;
    if (old_inlined != 0 && !enclosing.empty()) {
        const std::string own = enclosing + "::" + base;
        for (const Sym& s : syms_) {
            own_in_patch += s.defined && s.is_static && s.dec.type == SymbolType::Data && canonical_lambda_names(s.dec.display) == own ? 1u : 0u;
        }
    }
    if (old_inlined != 0) {
        const bool same_count = old_of_function == scoped.size() + own_in_patch;
        CML_LOG(Trace, "bind: closure statics '%s': %zu old record(s) are in the procedure of '%s' itself (inlined lambdas), %zu in all, %zu in the patch: %s", base.c_str(),
                old_inlined, enclosing.c_str(), old_of_function, scoped.size(), same_count ? "left to the layout and the place in the function" : "not paired");
        // (The evidence tag of the harness: cmltest prints broker-log lines that carry "WP<nn>-<NAME>".)
        CML_LOG(Info, "WP40-INLINED function='%s' statics=%s inlined=%zu old=%zu patch=%zu own=%zu %s", enclosing.c_str(), base.c_str(), old_inlined, old_of_function, scoped.size(), own_in_patch,
                same_count ? "placed-by=layout-or-place" : "refused=lambdas-changed");
        for (const std::size_t m : scoped) {
            if (same_count) {
                why[unmatched[m]] = std::to_string(old_inlined) + " of the old records named '" + base + "' are in the procedure of '" + enclosing +
                                    "' itself (the compiler inlined the lambdas), which does not say which lambda each belongs to";
            } else {
                why[unmatched[m]] = "the lambdas of '" + enclosing + "' that hold a static named '" + base + "' are not the same as in the old image (" + std::to_string(old_of_function) +
                                    " there, some of them inlined, " + std::to_string(scoped.size() + own_in_patch) + " in the patch" + (own_in_patch != 0 ? std::string(", " + std::to_string(own_in_patch) + " of them declared in the function itself") : std::string()) + ")";
                no_layout.insert(unmatched[m]);
            }
        }
        return;
    }
    // (After the inlined records: they need no types.)
    if (!unreadable.empty()) {
        // Degrade to what the planner did before it had types (the layout step may still place them).
        CML_LOG(Trace, "bind: closure statics '%s': not paired by closure: %s", base.c_str(), unreadable.c_str());
        for (const std::size_t m : scoped) {
            why[unmatched[m]] = "the enclosing lambdas cannot be told apart (" + unreadable + ")";
        }
        return;
    }
    if (names_say_more) {
        CML_LOG(Trace, "bind: closure statics '%s': %zu old record(s) are in lambdas of '%s' by name but %zu by closure class: not paired", base.c_str(), old_by_name,
                members_function.c_str(), old_by_closure_name);
        for (const std::size_t m : scoped) {
            why[unmatched[m]] = "the old records of the lambdas of '" + members_function + "' cannot be identified consistently (" + std::to_string(old_by_name) +
                                " by the name of their function, " + std::to_string(old_by_closure_name) + " by their closure class)";
            no_layout.insert(unmatched[m]);
        }
    } else if (old_in_function == 0 && old_by_name + old_by_closure_name != 0) {
        // Functions of this name have such statics in their lambdas, but none of them is a function of the patch (its
        // decorated spelling: class, name and parameter list). A lambda that is new in an overload that had none, and
        // a function whose parameter list changed, look the same from here; "new" would restart the statics silently.
        const std::size_t namesakes = std::max(old_by_name, old_by_closure_name);
        CML_LOG(Trace, "bind: closure statics '%s': %zu old record(s) are in lambdas of functions named like '%s', none in a function of the patch: not paired", base.c_str(),
                namesakes, members_function.c_str());
        for (const std::size_t m : scoped) {
            why[unmatched[m]] = "the old image has " + std::to_string(namesakes) + " static(s) named '" + base + "' in lambdas of functions named like '" + members_function +
                                "', but none in a function with the parameter list of the patch's; a lambda that is new there cannot be told from one whose function changed";
            no_layout.insert(unmatched[m]);
        }
    } else if (old_in_function == 0) {
        // (Guards included: this compiler records guard variables, or the group would have no candidates.)
        for (const std::size_t m : scoped) {
            Sym& sy = syms_[unmatched[m]];
            CML_LOG(Trace, "bind: closure statics '%s': %s: no lambda of its function had a static of this name: new", base.c_str(), sy.name.c_str());
            decide(sy, DecisionKind::PatchNewData, BindSource::None, sy.dec.type, {},
                   "function-local static of a lambda; no lambda of that function had a static named '" + base + "' in the old image: new");
            member_done[m] = true;
        }
    } else {
        bool same_set = true;
        for (const std::size_t m : scoped) {
            same_set = same_set && !hits[m].empty();
        }
        for (std::size_t c = 0; c < free_cands.size(); ++c) {
            same_set = same_set && (!in_function[c] || cand_hits[c] != 0);
        }
        // WP-40 (R-48 item 5): a lambda's number is its only name, and two lambdas that change places keep their numbers.
        // What the type records do say about a lambda is the signature of its call operator. Where the old operator() of
        // a closure and the patch's operator() of the closure with that number have different signatures, the number
        // now names another lambda (or the lambda changed): the statics of the function are not paired by number.
        std::string changed_lambda;
        if (same_set) {
            std::vector<std::pair<std::uint32_t, std::string>> call_operators;  // new "??R<lambda_N>..." functions, without the unit's hash
            for (std::uint32_t pos = 0; pos < syms_.size(); ++pos) {
                const Sym& f = syms_[pos];
                if (f.defined && f.dec.type == SymbolType::Function && starts_with(f.name, "??R<lambda_")) {
                    call_operators.emplace_back(pos, pdb::without_tu_discriminator(std::string_view(f.name).substr(3)));
                }
            }
            for (const std::size_t m : scoped) {
                if (!changed_lambda.empty() || hits[m].size() != 1) {
                    continue;
                }
                const pdb::SymbolView* c = free_cands[hits[m][0]];
                const Sym* op = nullptr;
                const std::string_view mine = own_scope[m];
                for (const std::pair<std::uint32_t, std::string>& entry : call_operators) {
                    const std::string& text = entry.second;
                    if (mine.size() > text.size() && mine.compare(0, text.size(), text) == 0 && mine[text.size()] == '@') {
                        op = &syms_[entry.first];
                        break;
                    }
                }
                if (op == nullptr || c->parent_proc == pdb::kNoSymbol || c->parent_proc >= comp_syms_.size()) {
                    continue;  // no call operator of its own in the patch (inlined) or none recorded: nothing to compare
                }
                const pdb::FunctionSignature& old_sig = old_signature(comp_syms_[c->parent_proc]);
                const pdb::FunctionSignature& new_sig = new_signature(*op);
                CML_LOG(Trace, "bind: closure statics '%s': %s: call operator %s old signature=%s new signature=%s", base.c_str(), syms_[unmatched[m]].name.c_str(), op->name.c_str(),
                        old_sig.known ? old_sig.text.c_str() : "unknown", new_sig.known ? new_sig.text.c_str() : "unknown");
                // (The closure's number is part of the text; with an override the numbers differ on purpose.)
                if (old_sig.known && new_sig.known && canonical_lambda_names(old_sig.text) != canonical_lambda_names(new_sig.text)) {
                    changed_lambda = "the call operator of '" + display_name(op->name) + "' has another signature than the old one of that number (" + old_sig.text + " there, " +
                                     new_sig.text + " in the patch): the lambdas may have changed places, and a lambda's compiler-assigned number is its only other identity";
                }
            }
        }
        if (!changed_lambda.empty()) {
            CML_LOG(Trace, "bind: closure statics '%s': %s: not paired", base.c_str(), changed_lambda.c_str());
            for (const std::size_t m : scoped) {
                why[unmatched[m]] = changed_lambda;
                no_layout.insert(unmatched[m]);
            }
        } else if (!same_set) {
            CML_LOG(Trace, "bind: closure statics '%s': the lambdas holding it changed (%zu old record(s) in the function's lambdas, %zu in the patch): not paired", base.c_str(),
                    old_in_function, scoped.size());
            for (const std::size_t m : scoped) {
                why[unmatched[m]] = "the lambdas of the enclosing function that hold a static named '" + base + "' are not the same as in the old image (" +
                                    std::to_string(old_in_function) + " there, " + std::to_string(scoped.size()) +
                                    " in the patch); a lambda's compiler-assigned number is its only identity, and the numbers may have shifted";
                no_layout.insert(unmatched[m]);
            }
        } else {
            for (const std::size_t m : scoped) {
                Sym& sy = syms_[unmatched[m]];
                if (hits[m].size() == 1 && cand_hits[hits[m][0]] == 1 && refuse_inlined_owner(sy, *free_cands[hits[m][0]], "operator()", "closure")) {
                    member_done[m] = true;  // WP-51: refused before it teaches the layout
                } else if (hits[m].size() == 1 && cand_hits[hits[m][0]] == 1) {
                    const pdb::SymbolView* c = free_cands[hits[m][0]];
                    CML_LOG(Trace, "bind: closure statics '%s': %s paired with the old record at rva %s (closure %s)", base.c_str(), sy.name.c_str(), hex(c->rva).c_str(),
                            ids[hits[m][0]].tail.c_str());
                    matched[unmatched[m]] = {c, BindSource::CompilandClosure};
                    if (!overridden[m]) {
                        learn_delta(sy, c->rva);
                    } else {
                        // A static paired across the numbers does not sit where the section's positions put its old record;
                        // the patch's static of the lambda with the old record's number does (its twin): that one teaches the
                        // layout, which the guards of these lambdas are placed from (settle_guards).
                        for (const auto& [from, to] : closure_pairing_) {
                            if (from == to || !in_closure(sy, from)) {
                                continue;
                            }
                            if (const Sym* twin = closure_twin(sy, from, to)) {
                                learn_delta(*twin, c->rva);
                            }
                            break;
                        }
                    }
                    note_lambda(sy, base, own_scope[m], ids[hits[m][0]].tail);
                    cand_taken[hits[m][0]] = true;
                    member_done[m] = true;
                } else if (hits[m].size() != 1) {
                    // Several statics of this name in one lambda (different blocks): the layout step may tell them apart.
                    why[unmatched[m]] = "the lambda has " + std::to_string(hits[m].size()) + " statics named '" + base + "' in the old image";
                } else {
                    why[unmatched[m]] = std::to_string(cand_hits[hits[m][0]]) + " statics named '" + base + "' of the patch are in the lambda that has one in the old image";
                }
            }
        }
    }
    std::vector<std::uint32_t> left;
    for (std::size_t m = 0; m < unmatched.size(); ++m) {
        if (!member_done[m]) {
            left.push_back(unmatched[m]);
        }
    }
    unmatched = std::move(left);
    std::vector<const pdb::SymbolView*> free_left;
    for (std::size_t c = 0; c < free_cands.size(); ++c) {
        if (!cand_taken[c]) {
            free_left.push_back(free_cands[c]);
        }
    }
    free_cands = std::move(free_left);
}

// Pass B (4), data (ADR-0042, amended). Two statics of one name in one function, declared in different blocks
// (Godot's `if (i == 0) { static const Vector<StringName> names = ...; } else if (i == 1) { static const ... names ... }`),
// have the same name, scope and type; under cl each is a COMDAT section of its own, so no layout tells them apart
// either. What is left is their place in the function. Both sides record it the same way: the data records of the
// procedure, in the order the compiler emitted them, in the old PDB's module stream and in the patch object's
// `.debug$S`. The k-th record of the name in the old procedure is paired with the k-th in the patch ONLY when
//   (a) both sides have the same number of them,
//   (b) the canonical types of every pair are equal,
//   (c) every pair sits in the same blocks (the index of each enclosing S_BLOCK32 among its parent's blocks),
// and the function itself is matched to its old version. Anything else stays ambiguous (CML3207, with the reason).
// Swapping two such blocks, or replacing one, exchanges the statics without a trace in these records, so a pairing of
// several is never silent: CML3570 (warning) names the function and the statics. One static of the name in the
// function needs no order and no warning: its function is its identity (the statics of two overloads).
//
// This is the fallback for statics the layout has no opinion on.
//   * The layout WINS where it predicts something: when the known layout of a section (a delta every matched static of
//     the section agrees on) puts a static of the function on an old record of this group, the layout step has decided
//     (it paired the static, or the record was already taken), and no static of that function and name is paired by
//     place. An order that disagrees with a layout never overrules it.
//   * Where the layout predicts no old record of the group for ANY static of the function and name (no layout is known
//     for their sections, or the known one points at nothing of the group, as after an unrelated static was added in
//     front of them), they are paired by place.
//   * A pairing made here never teaches, contradicts or drops a section layout (no learn_delta): the layout other
//     statics and the guards of pass C rely on must not depend on an order-based guess.
//   * For the same reason a static is paired by place only if every guard variable of its block is already bound to
//     its old RECORD (cl records the guards; clang-cl does not). A guard without a record could only be placed by pass C
//     from a section layout, which here is unknown or does not describe these statics: the patch would get a fresh
//     guard (CML3211) and construct the bound old object a second time. That is refused (CML3207).
void Planner::pair_statics_by_procedure(const std::string& base, bool is_tls, const std::vector<std::uint32_t>& members, const std::vector<const pdb::SymbolView*>& cands,
                                        std::vector<std::uint32_t>& unmatched, std::vector<const pdb::SymbolView*>& free_cands, std::map<std::uint32_t, Match>& matched,
                                        std::map<std::uint32_t, std::string>& why, const std::set<std::uint32_t>& no_layout) {
    ObjectSignatures& debug = object_debug();
    // The old record of this group that the known layout of the member's section puts it on; null when no layout is
    // known for the section or it points at no record of the group.
    const auto predicted_by_layout = [&](const Sym& sy) -> const pdb::SymbolView* {
        const auto d = delta_.find(sy.dec.section);
        if (d == delta_.end()) {
            return nullptr;
        }
        const std::int64_t want = d->second + static_cast<std::int64_t>(sy.s->value);
        for (const pdb::SymbolView* c : cands) {
            if (static_cast<std::int64_t>(c->rva) == want) {
                return c;
            }
        }
        return nullptr;
    };
    // The function each member of the group is declared in (every member: the layout's opinion on a member that is
    // already paired counts too).
    std::map<std::uint64_t, std::vector<std::uint32_t>> all_of_function;
    for (const std::uint32_t pos : members) {
        const Sym& sy = syms_[pos];
        const std::optional<std::uint64_t> fn = sy.dec.section == 0 ? std::nullopt : debug.function_of_static(sy.dec.section, sy.s->value);
        if (fn) {
            all_of_function[*fn].push_back(pos);
        }
    }
    std::map<std::uint64_t, std::vector<std::uint32_t>> by_function;  // (section << 32) | offset of the function -> members declared in it
    for (const std::uint32_t pos : unmatched) {
        if (no_layout.count(pos) != 0) {
            continue;  // refused by the rule for lambdas: not placed by any other step
        }
        const Sym& sy = syms_[pos];
        bool several = false;
        const std::optional<std::uint64_t> fn = sy.dec.section == 0 ? std::nullopt : debug.function_of_static(sy.dec.section, sy.s->value, &several);
        if (fn) {
            by_function[*fn].push_back(pos);
        } else if (why.count(pos) == 0) {
            why[pos] = several ? "the records of several procedures of " + in_.object_name + " name it as their static"
                               : "no procedure of " + in_.object_name + " records it as its static (" +
                                     (debug.origin() == ObjectTypeOrigin::Unreadable ? debug.detail() : std::string("the object's .debug$S has no such record")) + ")";
        }
    }
    const auto path_text = [](const std::vector<std::uint32_t>& path) {
        std::string text;
        for (const std::uint32_t index : path) {
            text += (text.empty() ? "" : ".") + std::to_string(index);
        }
        return text.empty() ? std::string("-") : text;
    };
    for (const auto& entry : by_function) {
        const std::uint64_t fn = entry.first;
        const std::vector<std::uint32_t>& positions = entry.second;  // (not a structured binding: a lambda below captures it)
        const std::uint32_t fn_section = static_cast<std::uint32_t>(fn >> 32);
        const std::uint32_t fn_value = static_cast<std::uint32_t>(fn & 0xFFFFFFFFull);
        // The function and its old version.
        std::string function = "the function at section " + std::to_string(fn_section) + "+" + hex(fn_value);
        const pdb::SymbolView* old_proc = nullptr;
        for (std::uint32_t pos = 0; pos < syms_.size() && old_proc == nullptr; ++pos) {
            const Sym& f = syms_[pos];
            if (!f.defined || f.dec.section != fn_section || f.s->value != fn_value || f.dec.type != SymbolType::Function) {
                continue;
            }
            function = "'" + f.dec.display + "'";
            // A static function: the record it was bridged to. An external one: the procedure of this compiland at the
            // address of its public symbol (not the address its decision binds: after an earlier patch that is the
            // patch's copy, and the statics still live in the module).
            const auto m = matched.find(pos);
            if (m != matched.end()) {
                old_proc = pdb::is_proc_kind(m->second.view->kind) ? m->second.view : nullptr;
            } else if (!f.is_static) {
                const Result<std::vector<pdb::SymbolView>> publics = pdb_.find_public(f.name);
                for (std::size_t p = 0; publics && old_proc == nullptr && p < publics->size(); ++p) {
                    const std::uint32_t rva = (*publics)[p].rva;
                    for (const pdb::SymbolView& v : comp_syms_) {
                        if (rva != pdb::kNoRva && pdb::is_proc_kind(v.kind) && v.rva == rva) {
                            old_proc = &v;
                            break;
                        }
                    }
                }
            }
        }
        const auto refuse = [&](const std::string& reason) {
            CML_LOG(Trace, "bind: statics '%s' of %s: not matched by their place in the function: %s", base.c_str(), function.c_str(), reason.c_str());
            for (const std::uint32_t pos : positions) {
                why[pos] = "the statics named '" + base + "' of " + function + " cannot be matched by their place in the function: " + reason;
            }
        };
        // Does the layout have an opinion on a static of this function and name? Then it decides. (WP-40, R-48 item 10:
        // the reason names the static whose prediction decided and the section of THAT static, and it is added to what an
        // earlier step said instead of replacing it.)
        const Sym* decider = nullptr;
        const pdb::SymbolView* decider_at = nullptr;
        for (const std::uint32_t pos : all_of_function[fn]) {
            const pdb::SymbolView* at = predicted_by_layout(syms_[pos]);
            if (at != nullptr && decider == nullptr) {
                decider = &syms_[pos];
                decider_at = at;
            }
        }
        if (decider != nullptr) {
            CML_LOG(Trace, "bind: statics '%s' of %s: the known layout of object section %u puts %s on the old record at rva %s: not paired by place", base.c_str(), function.c_str(),
                    decider->dec.section, decider->name.c_str(), hex(decider_at->rva).c_str());
            for (const std::uint32_t pos : positions) {
                const Sym& sy = syms_[pos];
                const pdb::SymbolView* at = predicted_by_layout(sy);
                const std::string reason =
                    at != nullptr ? "the layout of object section " + std::to_string(sy.dec.section) + " puts it on the old record at rva " + hex(at->rva) +
                                        ", which another static of the patch was already paired with"
                                  : "the layout of object section " + std::to_string(decider->dec.section) + " puts " + describe(*decider) + " on the old record at rva " +
                                        hex(decider_at->rva) + " and places no old static named '" + base + "' of " + function + " at the offset of this one" +
                                        (sy.dec.section != decider->dec.section ? " (in object section " + std::to_string(sy.dec.section) + ")" : std::string()) +
                                        "; the layout decides, so it is not matched by its place in the function";
                const auto earlier = why.find(pos);
                why[pos] = earlier == why.end() || earlier->second.empty() ? reason : earlier->second + "; " + reason;
            }
            continue;
        }
        if (old_proc == nullptr) {
            refuse("the function is not matched to a function of this translation unit in the old image");
            continue;
        }
        const ObjectSignatures::ProcStatics* new_all = debug.statics_of(fn_section, fn_value);
        if (new_all == nullptr || !new_all->usable) {
            refuse(new_all == nullptr ? std::string("the patch object has no procedure record for it") : new_all->why);
            continue;
        }
        bool complete = true;
        const Result<std::vector<pdb::ProcStatic>> old_all = pdb_.proc_statics(*old_proc, &complete);
        if (!old_all || !complete) {
            refuse(!old_all ? "the old function's records cannot be read: " + old_all.error().message : std::string("the old function has too many statics"));
            continue;
        }
        // (WP-40, R-48 item 11: under cl the record of a static of a function with internal linkage carries a qualified
        // name, "`f'::`2'::names"; the last component is compared, on both sides alike.)
        std::vector<const ObjectSignatures::LocalStatic*> new_list;
        for (const ObjectSignatures::LocalStatic& s : new_all->statics) {
            if (last_component(s.record.name) == base && s.record.thread_storage == is_tls) {
                new_list.push_back(&s);
            }
        }
        std::vector<const pdb::ProcStatic*> old_list;
        for (const pdb::ProcStatic& s : *old_all) {
            if (last_component(s.name) == base && s.thread_storage == is_tls) {
                old_list.push_back(&s);
            }
        }
        // (a) the same number on both sides
        if (old_list.size() != new_list.size() || new_list.empty()) {
            refuse("the function declares " + std::to_string(new_list.size()) + " of them in the patch and " + std::to_string(old_list.size()) + " in the old image");
            continue;
        }
        const std::size_t count = new_list.size();
        // The old records as the planner's candidates (the same records, by the same walk order).
        std::vector<const pdb::SymbolView*> old_views;
        for (const pdb::SymbolView& v : comp_syms_) {
            const bool thread = v.kind == pdb::SymbolKind::LocalThread || v.kind == pdb::SymbolKind::GlobalThread;
            if (v.parent_proc == old_proc->index && pdb::is_data_kind(v.kind) && thread == is_tls && comp_last(&v) == base) {
                old_views.push_back(&v);
            }
        }
        bool sound = old_views.size() == count;
        for (std::size_t k = 0; sound && k < count; ++k) {
            sound = old_views[k]->rva != pdb::kNoRva && old_views[k]->rva == old_list[k]->rva;
        }
        if (!sound) {
            refuse("the old function's records do not agree with the symbol table of its compiland");
            continue;
        }
        // Every record of the patch names a member of this group.
        std::vector<std::uint32_t> new_pos(count, kNone32);
        std::string problem;
        for (std::size_t k = 0; problem.empty() && k < count; ++k) {
            for (const std::uint32_t pos : members) {
                if (new_list[k]->section != 0 && syms_[pos].dec.section == new_list[k]->section && syms_[pos].s->value == new_list[k]->value) {
                    new_pos[k] = pos;
                    break;
                }
            }
            if (new_pos[k] == kNone32) {
                problem = "record " + std::to_string(k + 1) + " of the patch names no static symbol of the object";
            }
        }
        // (c) the same blocks, (b) the same type, and no contradiction with what is already known.
        std::vector<std::string> type_text(count);
        for (std::size_t k = 0; problem.empty() && k < count; ++k) {
            const pdb::ProcStatic& o = *old_list[k];
            const pdb::ProcStatic& n = new_list[k]->record;
            const std::string which = "static " + std::to_string(k + 1) + " of " + std::to_string(count);
            if (!o.plain_scopes || !n.plain_scopes) {
                problem = which + " is declared inside an inlined call or too deeply nested for its place to be known";
                break;
            }
            if (o.block_path != n.block_path) {
                problem = which + " is declared in another block than in the old image (block path " + path_text(o.block_path) + " there, " + path_text(n.block_path) + " in the patch)";
                break;
            }
            std::string unreadable;
            const std::optional<std::string> old_type = pdb::canonical_type(pdb_.types(), o.type_index, &unreadable);
            if (!old_type) {
                problem = "the type of " + which + " cannot be read from the PDB of " + in_.module_name + ": " + unreadable;
                break;
            }
            const std::optional<std::string> new_type = debug.type_text(n.type_index, &unreadable);
            if (!new_type) {
                problem = "the type of " + which + " cannot be read from " + in_.object_name + ": " + unreadable;
                break;
            }
            if (*old_type != *new_type) {
                problem = which + " has another type than in the old image (" + *old_type + " there, " + *new_type + " in the patch)";
                break;
            }
            type_text[k] = *new_type;
            const auto earlier = matched.find(new_pos[k]);
            if (earlier != matched.end() ? earlier->second.view != old_views[k] : std::find(free_cands.begin(), free_cands.end(), old_views[k]) == free_cands.end()) {
                problem = which + " or its old record was already paired differently by an earlier step";
                break;
            }
        }
        // The guard variables of the blocks these statics are declared in must be bound to their old records already
        // (see above). A guard `?$TSS0@<scope>@4HA` belongs to the block whose statics are `?<name>@<scope>@4...`.
        std::vector<std::uint32_t> unbound_guards;
        std::vector<std::uint32_t> relaxed_guards;  // WP-46: guards without a record left to settle_guards (see below)
        for (std::size_t k = 0; (problem.empty() || !unbound_guards.empty()) && k < count; ++k) {
            if (matched.count(new_pos[k]) != 0) {
                continue;  // paired by an earlier step, which answers for its guard
            }
            const std::string& member = syms_[new_pos[k]].name;
            if (is_guard_variable(member)) {
                continue;  // WP-40 (R-48 item 7): the members are guard variables themselves (cl's `$S1` of /Zc:threadSafeInit-), which have no guard
            }
            for (std::uint32_t pos = 0; pos < syms_.size(); ++pos) {
                const Sym& guard = syms_[pos];
                if (pos == new_pos[k] || !guard.defined || !guard.is_static || guard.dec.type != SymbolType::Data || !is_guard_variable(guard.name) || guard.name.size() < 6) {
                    continue;
                }
                // (WP-40: a `$TSSn` guards its block, a bitmask `$S<n>` every block of the function; WP-52: by the scopes with
                // their back-references resolved, guard_scope_statics.)
                if (!guard_ties(pos, new_pos[k])) {
                    continue;
                }
                const auto bound = matched.find(pos);
                const bool by_record = (bound != matched.end() && pdb::is_data_kind(bound->second.view->kind)) ||
                                       (guard.decided && guard.dec.decision == DecisionKind::BindOldData && guard.dec.source != BindSource::CompilandLayout);
                // WP-46 (F-41.1, ADR-0043): a compiler that records no thread-safe guard at all (clang-cl) leaves every `$TSS<n>`
                // without a record, so this requirement refused every such static (Jolt's GetRTTIOfType overloads). When the static
                // is the only one of its name in its function (the function is its identity, no order is involved), it is paired
                // here and its guard is left to settle_guards, which places it from the layout these statics teach (and only
                // settle_guards reads that layout) or refuses with CML3600. cl records its `$TSS<n>` guards; its `$S<n>` bitmask
                // (no record, WP-40 item 7) is not relaxed.
                if (!by_record && count == 1 && !is_tls && guard.name.compare(0, 5, "?$TSS") == 0 && !compiland_records_tss_guards()) {
                    relaxed_guards.push_back(pos);
                    continue;
                }
                if (!by_record) {
                    unbound_guards.push_back(pos);
                    if (problem.empty()) {
                        problem = "the guard variable '" + guard.dec.display + "' (" + guard.name + ") of static " + std::to_string(k + 1) + " of " + std::to_string(count) +
                                  " has no record in the old image, so it cannot be bound to the old guard and the static would be constructed a second time";
                    }
                }
            }
        }
        if (!problem.empty()) {
            refuse(problem);
            // The guards of the refused statics are settled here (pass C would give them the warning for a guard that was
            // merely not found, CML3211; these belong to an error).
            for (const std::uint32_t pos : unbound_guards) {
                Sym& guard = syms_[pos];
                if (!guard.decided) {
                    guard.diagnosed = true;
                    decide(guard, DecisionKind::PatchNewData, BindSource::None, SymbolType::Data, {}, "guard variable of a static that is refused (CML3207)");
                }
            }
            continue;
        }
        std::size_t paired = 0;
        for (std::size_t k = 0; k < count; ++k) {
            const std::uint32_t pos = new_pos[k];
            if (matched.count(pos) != 0) {
                continue;
            }
            CML_LOG(Trace, "bind: statics '%s' of %s: record %zu of %zu (block path %s, type %s) %s paired with the old record at rva %s", base.c_str(), function.c_str(), k + 1, count,
                    path_text(new_list[k]->record.block_path).c_str(), type_text[k].c_str(), syms_[pos].name.c_str(), hex(old_views[k]->rva).c_str());
            matched[pos] = {old_views[k], BindSource::CompilandProcedure};
            unmatched.erase(std::remove(unmatched.begin(), unmatched.end(), pos), unmatched.end());
            free_cands.erase(std::remove(free_cands.begin(), free_cands.end(), old_views[k]), free_cands.end());
            why.erase(pos);
            ++paired;
            if (!relaxed_guards.empty()) {
                place_taught_.emplace_back(pos, old_views[k]->rva);
                CML_LOG(Info, "WP46-GUARD-BY-PLACE static=%s function=%s guards=%zu (no guard record in the compiland: placed by settle_guards from the layout, or CML3600)",
                        syms_[pos].name.c_str(), function.c_str(), relaxed_guards.size());
            }
        }
        if (paired != 0 && count > 1) {
            warn(diag::kStaticsMatchedByOrder,
                 "Function-local statics '" + base + "' of " + function + " (" + std::to_string(count) +
                     ") are matched by their order in the function; if you reordered or replaced blocks that declare them, their values may be exchanged.",
                 "Rename one of them, or hot-restart after reordering.");
            // (The evidence tag of the harness: cmltest prints broker-log lines that carry "WP<nn>-<NAME>".)
            CML_LOG(Info, "WP37-ORDER function=%s statics=%s count=%zu warning=CML3570", function.c_str(), base.c_str(), count);
        }
    }
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

        // thread_local data (WP-24): an external one is looked up through its S_GTHREAD32 record
        // (lookup_thread_global, never the publics: their offset is not the TLS offset); a static one is matched
        // in bridge_statics(). Whatever is not in the old image is rejected (CML3400).
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

        // Read-only data (.rdata tables, constants) carries no state, and binding it to the old copy would
        // silently ignore the user's edit of it: a fresh copy is correct. (The address of const data
        // therefore changes across a patch; KNOWN_LIMITATIONS.)
        if (type == SymbolType::Data && si != nullptr && !si->code && (si->sec->characteristics & coff::scn::MemWrite) == 0) {
            decide(sy, DecisionKind::PatchFresh, BindSource::None, type, {}, "read-only data: a fresh copy is correct, the edit takes effect (the address of const data changes)");
            continue;
        }

        if (s.is_static()) {
            continue;  // matched in bridge_statics()
        }

        // External (or COMMON): registry, then the module's publics.
        std::optional<OldRef> old;
        if (si != nullptr && si->tls) {
            const ThreadLookup tl = lookup_thread_global(sy);
            if (tl.ambiguous) {
                error(diag::kAmbiguousThreadLocal,
                      "thread_local '" + sy.dec.display + "' (" + sy.name + ") matches several S_GTHREAD32 records of the old image; cannot tell which one it is",
                      "Rename one of the thread_locals, or hot-restart.");
                decide(sy, DecisionKind::PatchNewData, BindSource::None, type, {}, "ambiguous thread_local (CML3402)");
                continue;
            }
            old = tl.ref;
        } else {
            old = lookup_registry(sy.name, type);
            if (type == SymbolType::Data) {
                // The old definition may have been thread_local while this one is not: its public would give a
                // meaningless address.
                const ThreadLookup tl = lookup_thread_global(sy);
                if (tl.ref || tl.ambiguous) {
                    error(diag::kThreadLocal,
                          "'" + sy.dec.display + "' is a thread_local in the old image but " + in_.object_name + " defines it as ordinary data; storage of a different kind cannot be bound",
                          "Keep the thread_local keyword, or hot-restart.");
                    decide(sy, DecisionKind::PatchNewData, BindSource::None, type, {}, "was thread_local in the old image (CML3102)");
                    continue;
                }
            }
            if (!old) {
                old = lookup_public(sy.name, type);
            }
        }
        if (type == SymbolType::Function) {
            if (old) {
                decide(sy, DecisionKind::PatchHooked, old->source, type, old->addr, "defined function present in the old image (" + old->note + "): old entry is hooked to the patch definition");
            } else {
                decide(sy, DecisionKind::PatchNewFunction, BindSource::None, type, {}, "defined function not in the old image: new canonical symbol, no hook");
            }
        } else {
            if (old) {
                decide(sy, DecisionKind::BindOldData, old->source, type, old->addr,
                       (si != nullptr && si->tls ? "thread_local present in the old image (" : "defined data present in the old image (") + old->note +
                           "): references are retargeted, the patch's copy is dead");
            } else if (si != nullptr && si->tls) {
                new_thread_local_error(sy);
                decide(sy, DecisionKind::PatchNewData, BindSource::None, type, {}, "thread_local not in the old image (CML3400)");
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
        bool is_tls = false;
        std::string base;
        std::string encl;
        std::string display;
        std::vector<std::uint32_t> members;  // positions in syms_
    };
    std::map<std::string, Group> groups;
    // WP-46 (F-41.2): an edited hash-named lambda has a new name; it is paired with the one old lambda left, or refused.
    pair_hashed_by_elimination();
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
        // WP-40 (R-48 item 4): clang-cl at /O2 splits an internal-linkage aggregate into scalars named "<name>.0",
        // "<name>.1", ... (`?s_last@?A0x520C9D4F@@3PAHA.0`, `s_last.1` in C). The debug information of such a piece is a
        // fragment, which CodeView does not record: neither the old PDB nor the patch object has a record of it, so
        // nothing says where the old piece is. A writable static with such a suffix is accepted only when a data record
        // of the patch object names it (then it is a whole variable: LLVM's uniquifier, or a piece that covers the whole
        // aggregate, as fixture c_statics has); otherwise it is refused instead of being given a fresh copy.
        if (!is_func && sy.dec.section != 0 && is_split_piece_name(sy.name) && !secs_[sy.dec.section - 1].runtime_table &&
            (secs_[sy.dec.section - 1].sec->characteristics & coff::scn::MemWrite) != 0 && !named_by_debug_record(sy)) {
            const std::size_t dot = sy.name.rfind('.');
            error(diag::kSplitAggregate,
                  "static data " + describe(sy) + " in " + in_.object_name + " is piece " + sy.name.substr(dot + 1) + " of '" + display_name(sy.name.substr(0, dot)) +
                      "', which the compiler split into separate variables; the debug information records no piece, so the old piece in the running process cannot be found and a reload would reset it",
                  "Give the variable external linkage or take its address (the compiler then keeps it whole), or hot-restart.");
            sy.diagnosed = true;
            decide(sy, DecisionKind::PatchNewData, BindSource::None, sy.dec.type, {}, "piece of a split aggregate without a debug record (CML3601)");
            continue;
        }
        // C statics (WP-26c): the object's name is not a Microsoft-decorated one ("platform.0", "next.counter" under
        // clang-cl); read the source identity from it.
        const bool c_data = !is_func && !sy.name.empty() && sy.name[0] != '?';
        const CStaticName cname = c_data ? c_static_name(sy.name) : CStaticName();
        const std::string base = c_data ? cname.base : base_identifier(sy.name);
        if (base.empty() && !is_func && sy.dec.section != 0 && !secs_[sy.dec.section - 1].runtime_table &&
            (secs_[sy.dec.section - 1].sec->characteristics & coff::scn::MemWrite) != 0 && !is_fresh_data_name(sy.name) &&
            !is_compiler_generated_data_name(sy.name)) {
            // Writable data that looks like a user's static (not a name on the explicit compiler-generated list) whose identity
            // cannot be established: binding it as new would give the patch a zero-filled
            // copy of live state (WP-26c: the raylib crash). Refuse before anything is applied.
            error(diag::kUnnamedStatic,
                  "static data " + describe(sy) + " in " + in_.object_name + " has no source-level name the planner can match to the old image (object section '" +
                      secs_[sy.dec.section - 1].name + "'), so it cannot be bound to the running process's copy",
                  "Give the variable an ordinary name, or hot-restart.");
            sy.diagnosed = true;
            decide(sy, DecisionKind::PatchNewData, BindSource::None, sy.dec.type, {}, "writable static without a source-level name (CML3500)");
            continue;
        }
        if (base.empty()) {
            // Nothing to match by: a static without a source-level name (compiler temporary, read-only).
            decide(sy, is_func ? DecisionKind::PatchNewFunction : DecisionKind::PatchNewData, BindSource::None, sy.dec.type, {},
                   "static without a source-level name: treated as new");
            continue;
        }
        const std::string encl = c_data ? cname.encl : enclosing_function(sy.name);
        const bool is_tls = !is_func && sy.dec.section != 0 && secs_[sy.dec.section - 1].tls;
        // The spelling ("platform.0" vs "platform") is not part of a C static's identity: one group per (name, scope).
        const std::string key = std::string(is_func ? "F|" : (is_tls ? "T|" : "D|")) + (c_data ? std::string() : canonical_lambda_names(sy.dec.display)) + "|" + base + "|" + encl;
        Group& g = groups[key];
        g.is_func = is_func;
        g.is_tls = is_tls;
        g.base = base;
        if (!is_func) {
            wp51_static_bases_.insert(base);
        }
        g.encl = encl;
        g.display = (sy.dec.display == sy.name || c_data) ? std::string() : sy.dec.display;
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

    std::map<std::uint32_t, Match> matched;  // pos -> match
    std::set<std::string> deferred;          // group keys that need pass B

    // WP-40 (review): functions whose lambdas in the patch hold statics of a name. A record of that name directly in such a
    // function may be the function's own static or an inlined lambda's (clang-cl /O2 records it in the caller), and its name
    // does not say which; the function's own statics of the name are therefore not paired by name in pass A either.
    std::set<std::string> lambda_static_functions;  // "<function display>|<name>"
    for (const auto& [key, g] : groups) {
        if (!g.is_func && g.encl == "operator()") {
            const std::string enclosing = lambda_enclosing(g.display, g.base);
            if (!enclosing.empty()) {
                lambda_static_functions.insert(enclosing + "|" + g.base);
            }
        }
    }
    // WP-40 (second high review, ADR-0043 ruling): a static of a function `f` itself whose name is the name of a static of
    // one of f's lambdas, in the patch or in the old image, inlined or not, pairs only when every record it could pair
    // with is provably f's own. A record directly in f is the own static's or an inlined lambda's (clang-cl /O2 records
    // the static of an inlined lambda in the caller, under the same name), and nothing in the records tells which; so
    // such a static is refused (CML3207), never paired. (A lambda that the compiler inlined everywhere and that the patch
    // deleted leaves no record that names it: that case is not seen here, see docs/wp/WP-40.md.)
    // Returns why the static is at risk ("" when it is not).
    bool old_lambda_statics_done = false;
    std::map<std::string, const pdb::SymbolView*> old_lambda_statics;  // "<function>|<name>" -> a record
    const auto own_static_lambda_risk = [&](const Group& g) -> std::string {
        if (g.is_func || g.encl.empty() || g.encl == "operator()" || g.display.empty() || is_guard_variable("?" + g.base + "@")) {
            return {};
        }
        const std::string canonical = canonical_lambda_names(g.display);
        const std::string suffix = "::" + g.base;
        if (canonical.size() <= suffix.size() || canonical.compare(canonical.size() - suffix.size(), suffix.size(), suffix) != 0) {
            return {};
        }
        const std::string function = canonical.substr(0, canonical.size() - suffix.size());
        if (lambda_static_functions.count(function + "|" + g.base) != 0) {
            return "a lambda of '" + function + "' in the patch has a static named '" + g.base + "'";
        }
        if (!ensure_compiland_symbols()) {
            return "the old records of compiland " + std::to_string(ci_) + " cannot be read";
        }
        if (!old_lambda_statics_done) {
            // Once per object: "<function>|<name>" for every old record of a lambda's static (in a call operator of the
            // function's lambda, or named so).
            old_lambda_statics_done = true;
            const std::string op = "::<lambda>::operator()";
            for (const pdb::SymbolView& v : comp_syms_) {
                if (!pdb::is_data_kind(v.kind)) {
                    continue;
                }
                std::string scope;
                if (v.parent_proc != pdb::kNoSymbol && v.parent_proc < comp_syms_.size()) {
                    const std::string& parent = comp_norm(&comp_syms_[v.parent_proc]).canonical;
                    if (parent.size() > op.size() && parent.compare(parent.size() - op.size(), op.size(), op) == 0) {
                        scope = parent.substr(0, parent.size() - op.size());
                    }
                }
                const std::string& own = comp_norm(&v).canonical;
                const std::string tail = op + "::" + comp_last(&v);
                if (scope.empty() && own.size() > tail.size() && own.compare(own.size() - tail.size(), tail.size(), tail) == 0) {
                    scope = own.substr(0, own.size() - tail.size());
                }
                if (!scope.empty()) {
                    old_lambda_statics.emplace(scope + "|" + comp_last(&v), &v);
                }
            }
        }
        const auto hit = old_lambda_statics.find(function + "|" + g.base);
        if (hit != old_lambda_statics.end()) {
            const pdb::SymbolView& v = *hit->second;
            return "a lambda of '" + function + "' in the old image has a static named '" + g.base + "' (the record at rva " + hex(v.rva) + " in '" +
                   std::string(v.parent_proc != pdb::kNoSymbol && v.parent_proc < comp_syms_.size() ? comp_syms_[v.parent_proc].name : v.name) + "')";
        }
        return {};
    };

    // Pass A: unambiguous name matches (also learns the section deltas).
    for (auto& [key, g] : groups) {
        std::vector<const pdb::SymbolView*> cands =
            static_candidates(g.base, g.encl, g.is_func, g.display, g.is_tls ? 1 : 0, g.is_func && g.members.size() == 1 ? syms_[g.members.front()].name : std::string());
        // WP-46 (review): a destructor's record that is its deleting destructor (`should_call_delete`, F-41.3) is no candidate
        // in pass A either, where one member and one candidate pair by the name alone.
        if (g.is_func && !g.base.empty() && g.base[0] == '~') {
            const std::size_t before = cands.size();
            cands.erase(std::remove_if(cands.begin(), cands.end(), [this](const pdb::SymbolView* v) { return is_deleting_destructor(*v); }), cands.end());
            if (cands.size() != before) {
                CML_LOG(Info, "WP46-DELETING-DTOR group='%s' dropped=%zu left=%zu (pass A)", g.base.c_str(), before - cands.size(), cands.size());
            }
        }
        if (!cands.empty()) {
            const std::string risk = own_static_lambda_risk(g);
            if (!risk.empty()) {
                for (const std::uint32_t pos : g.members) {
                    Sym& sy = syms_[pos];
                    CML_LOG(Info, "WP40-OWN-STATIC static='%s' refused (%s)", sy.dec.display.c_str(), risk.c_str());
                    error(diag::kAmbiguousStatic,
                          "static data '" + sy.dec.display + "' (" + sy.name + ") in " + in_.object_name + " cannot be paired with the " + std::to_string(cands.size()) +
                              " record(s) named '" + g.base + "' of its function in compiland " + std::to_string(ci_) + ": " + risk +
                              ", and a record of that name in the function may be that lambda's (a compiler records the static of an inlined lambda in the function it was inlined into), so none is provably this static's",
                          "Rename the static (or the lambda's static), or hot-restart.");
                    sy.diagnosed = true;
                    decide(sy, DecisionKind::PatchNewData, BindSource::None, sy.dec.type, {}, "own static named like a static of a lambda of its function (CML3207)");
                }
                continue;
            }
        }
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
        // (WP-40: a record of a lambda's static found in the procedure that inlined the lambda is not identified by its
        // name; see static_candidates.)
        const std::string enclosing = g.is_func || g.encl != "operator()" ? std::string() : lambda_enclosing(g.display, g.base);
        if (g.members.size() == 1 && cands.size() == 1 && !is_inlined_record(cands.front(), enclosing)) {
            // WP-51 (R-54): before the pair teaches the section layout.
            if (!g.is_func && refuse_inlined_owner(syms_[g.members.front()], *cands.front(), g.encl, "pass A")) {
                continue;
            }
            matched[g.members.front()] = {cands.front(), BindSource::CompilandStatic};
            if (!g.is_func) {
                learn_delta(syms_[g.members.front()], cands.front()->rva);
            }
            continue;
        }
        deferred.insert(key);
    }

    // What pass B has left of each deferred group.
    struct Pending {
        std::vector<const pdb::SymbolView*> cands;       // every candidate of the group (messages)
        std::vector<const pdb::SymbolView*> free_cands;  // not yet paired
        std::vector<std::uint32_t> unmatched;
        std::map<std::uint32_t, std::string> why;        // pos -> why a type-based step could not decide it
        std::set<std::uint32_t> no_layout;               // members the layout step must not place (pair_statics_by_closure)
    };
    std::map<std::string, Pending> pending;
    for (const std::string& key : deferred) {
        Group& g = groups[key];
        Pending& p = pending[key];
        p.cands = static_candidates(g.base, g.encl, g.is_func, g.display, g.is_tls ? 1 : 0, g.is_func && g.members.size() == 1 ? syms_[g.members.front()].name : std::string());
        p.free_cands = p.cands;
        p.unmatched = g.members;
    }
    // Pass B0 (WP-37, data): function-local statics of lambdas, by the identity of the closure class. It runs for
    // every group before the layout-based steps because its matches are as strong as a name match and teach the
    // section deltas (clang-cl records no guard variable: pass C places the guards from them).
    for (const std::string& key : deferred) {
        const Group& g = groups[key];
        if (!g.is_func) {
            Pending& p = pending[key];
            pair_statics_by_closure(g.base, p.unmatched, p.free_cands, matched, p.why, p.no_layout);
        }
    }
    for (const std::uint32_t sec : delta_conflict_) {
        delta_.erase(sec);
    }

    // Pass B: groups with several members and/or several candidates in the same scope. Never paired by position
    // alone.
    //   functions  (1) one-to-one by the canonical signature from the type records (WP-37, ADR-0042). Only when a
    //              signature of the group cannot be read: (2) by the member's exact qualified name, as before WP-37
    //              (clang spells lambdas with the same counter as the decorated name).
    //   data       (2) the exact qualified name, (3) the RVA in the unchanged section layout, (4) where no layout is
    //              known for its section: the function it is declared in and its place among that function's statics
    //              of the name (ADR-0042, amended).
    // Whatever is left is ambiguous: CML3214 for functions (equal signatures, a changed set of lambdas, or types that
    // cannot be read), CML3207 for data.
    // Step (4) needs to know which old function each function of the patch is, so every function group is handled
    // before the first data group (the two rounds of the outer loop; the body is not indented a second time).
    for (const bool functions : {true, false}) {
    for (const std::string& key : deferred) {
        Group& g = groups[key];
        if (g.is_func != functions) {
            continue;
        }
        Pending& pend = pending[key];
        const std::vector<const pdb::SymbolView*>& cands = pend.cands;
        std::vector<std::uint32_t>& unmatched = pend.unmatched;
        std::vector<const pdb::SymbolView*>& free_cands = pend.free_cands;
        std::map<std::uint32_t, std::string>& why = pend.why;
        // (1) functions: one-to-one by signature
        const bool by_signature = g.is_func && pair_overloads_by_signature(g.base, unmatched, free_cands, matched, why);
        // (2) exact qualified names, one-to-one. The qualified name of a lambda's call operator holds the lambda's
        // compiler-assigned number, which identifies it only while the lambdas are the same as before: such a group is
        // paired by name only when the names of the patch are exactly the names of the old image.
        bool exact_names = !by_signature;
        if (exact_names && g.is_func && g.display.find("<lambda") != std::string::npos) {
            std::multiset<std::string> new_names;
            std::multiset<std::string> old_names;
            for (const std::uint32_t pos : unmatched) {
                new_names.insert(syms_[pos].dec.display);
            }
            for (const pdb::SymbolView* c : free_cands) {
                old_names.insert(comp_norm(c).normalized);
            }
            if (new_names != old_names) {
                exact_names = false;
                for (const std::uint32_t pos : unmatched) {
                    why[pos] += "; the lambdas of this name are not the same as in the old image (" + std::to_string(old_names.size()) + " there, " +
                                std::to_string(new_names.size()) + " in the patch), so they are not paired by their compiler-assigned numbers either";
                }
            }
        }
        for (auto it = unmatched.begin(); exact_names && it != unmatched.end();) {
            const Sym& sy = syms_[*it];
            std::vector<std::size_t> hits;
            for (std::size_t c = 0; c < free_cands.size(); ++c) {
                if (comp_norm(free_cands[c]).normalized == sy.dec.display) {
                    hits.push_back(c);
                }
            }
            std::size_t same_display = 0;
            for (const std::uint32_t other : unmatched) {
                same_display += syms_[other].dec.display == sy.dec.display ? 1u : 0u;
            }
            if (hits.size() == 1 && same_display == 1) {
                matched[*it] = {free_cands[hits[0]], BindSource::CompilandStatic};
                free_cands.erase(free_cands.begin() + static_cast<std::ptrdiff_t>(hits[0]));
                it = unmatched.erase(it);
            } else {
                ++it;
            }
        }
        // (3) data: the RVA the unchanged layout predicts
        if (!g.is_func) {
            for (auto it = unmatched.begin(); it != unmatched.end();) {
                const Sym& sy = syms_[*it];
                const auto d = delta_.find(sy.dec.section);
                bool taken = false;
                if (d != delta_.end() && pend.no_layout.count(*it) == 0) {
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
        // (4) data: function-local statics the layout has no opinion on (under cl every function-local static is a
        // COMDAT section of its own; or the known layout of the section predicts no old record of the group for any
        // of them), by the function they are declared in and their place in it. Where the layout predicts an old record
        // for a static of the function it has decided above, and it wins: that function's statics are not paired here.
        if (!g.is_func && !g.encl.empty() && !unmatched.empty()) {
            pair_statics_by_procedure(g.base, g.is_tls, g.members, cands, unmatched, free_cands, matched, why, pend.no_layout);
        }
        for (const std::uint32_t pos : unmatched) {
            Sym& sy = syms_[pos];
            const auto reason = why.find(pos);
            const std::string because = reason == why.end() ? std::string() : ": " + reason->second;
            if (g.is_func) {
                error(diag::kAmbiguousOverloads,
                      "ambiguous overloads of static '" + sy.dec.display + "' (" + sy.name + ") in " + in_.object_name +
                          "; cannot tell which old function each new one replaces (" + std::to_string(g.members.size()) + " in the patch, " +
                          std::to_string(cands.size()) + " records named '" + g.base + "' in compiland " + std::to_string(ci_) + ")" + because,
                      "Rename one overload, or hot-restart.");
                sy.diagnosed = true;
                decide(sy, DecisionKind::PatchNewFunction, BindSource::None, sy.dec.type, {}, "ambiguous static overloads (CML3214)");
                continue;
            }
            error(diag::kAmbiguousStatic,
                  "static data '" + sy.dec.display + "' (" + sy.name + ") matches " + std::to_string(cands.size()) +
                      " records named '" + g.base + "' in compiland " + std::to_string(ci_) + " but the patch object defines " + std::to_string(g.members.size()) + " of them" + because,
                  "Rename one of the statics, keep their number unchanged, or hot-restart.");
            sy.diagnosed = true;
            decide(sy, DecisionKind::PatchNewData, BindSource::None, sy.dec.type, {}, "ambiguous static (CML3207)");
        }
    }
    }

    // Two statics of the patch must never share one old record (each step pairs one-to-one, but two groups can see the
    // same record: a lambda's static recorded in the procedure that inlined it is also a candidate of that procedure's
    // own statics of the name).
    {
        // (Keyed by the RVA, not by the record: cl at /O2 records an inlined lambda's static twice, in its operator() and in
        // the caller, so two records can be one live object. WP-40 review.)
        std::map<std::uint32_t, std::uint32_t> owner;
        for (const auto& [pos, m] : matched) {
            const auto ins = owner.emplace(m.view->rva, pos);
            if (!ins.second) {
                for (const std::uint32_t p : {ins.first->second, pos}) {
                    Sym& sy = syms_[p];
                    if (sy.diagnosed) {
                        continue;
                    }
                    error(diag::kAmbiguousStatic,
                          "static data '" + sy.dec.display + "' (" + sy.name + ") and another static of " + in_.object_name + " were both paired with the old record '" +
                              std::string(m.view->name) + "' at rva " + hex(m.view->rva) + " in compiland " + std::to_string(ci_),
                          "Rename one of the statics, or hot-restart.");
                    sy.diagnosed = true;
                    decide(sy, sy.dec.type == SymbolType::Function ? DecisionKind::PatchNewFunction : DecisionKind::PatchNewData, BindSource::None, sy.dec.type, {},
                           "two statics paired with one old record (CML3207)");
                }
            }
        }
    }

    // WP-51 (R-54, ADR-0044): every pairing of a static with a record directly in its own function's procedure, whichever step
    // made it, is checked against what that procedure inlined (pass A and the closure step checked theirs before they taught
    // the layout). Before the guards are settled, so a refused static takes its guard along.
    for (const auto& [key, g] : groups) {
        if (g.is_func) {
            continue;
        }
        for (const std::uint32_t pos : g.members) {
            const auto m = matched.find(pos);
            if (m != matched.end() && !syms_[pos].decided && refuse_inlined_owner(syms_[pos], *m->second.view, g.encl, to_string(m->second.how))) {
                matched.erase(m);
            }
        }
    }
    // WP-53 (R-61, ADR-0045): a static of a call operator paired with a record directly in the procedure its lambda was
    // inlined into (WP-40's in-caller pairing, by the layout or by its place in the function) is that lambda's static or
    // the procedure's own, and nothing in the records tells which (clang-cl writes both alike). The pairing stands only if
    // the code of that procedure proves it: it is listed for the pipeline (prove_inlined_lambda_statics), which refuses it
    // (CML3207) unless the patch's version of the procedure is the old code under the plan. A record of a call operator at
    // the same address (cl's twin, WP-40) identifies the lambda by itself: such a pairing is the closure step's and is
    // not listed.
    for (const auto& [key, g] : groups) {
        if (g.is_func || g.encl != "operator()") {
            continue;
        }
        const std::string enclosing = lambda_enclosing(g.display, g.base);
        if (enclosing.empty()) {
            continue;
        }
        for (const std::uint32_t pos : g.members) {
            const auto m = matched.find(pos);
            if (m == matched.end() || syms_[pos].decided || !is_inlined_record(m->second.view, enclosing)) {
                continue;
            }
            const pdb::SymbolView& record = *m->second.view;
            // (Review of bb63263: only a call operator of the static's OWN closure makes a twin; any record in a procedure named
            // `operator()` at that address was taken for one, so a static of an outer lambda o that was also inlined into
            // another lambda k left the pairing unproven.)
            if (const pdb::SymbolView* twin = own_closure_twin(syms_[pos], record)) {
                CML_LOG(Trace, "WP53-TWIN static='%s' record rva=%s: the call operator '%s' of its own closure records it at that address too; not listed",
                        syms_[pos].dec.display.c_str(), hex(record.rva).c_str(), std::string(comp_syms_[twin->parent_proc].name).c_str());
                continue;
            }
            const pdb::SymbolView& proc = comp_syms_[record.parent_proc];
            InlinedLambdaStatic s;
            s.symbol = syms_[pos].name;
            s.display = syms_[pos].dec.display;
            s.record_rva = record.rva;
            s.proc = std::string(proc.name);
            s.proc_rva = proc.rva;
            s.how = m->second.how;
            CML_LOG(Trace, "WP53-LISTED static='%s' record rva=%s in '%s' (rva %s) paired by %s: proven by the code of '%s' or refused", s.display.c_str(), hex(s.record_rva).c_str(),
                    s.proc.c_str(), hex(s.proc_rva).c_str(), to_string(s.how), s.proc.c_str());
            plan_.inlined_lambda_statics.push_back(std::move(s));
        }
    }

    if (wp51_procs_ != 0) {
        CML_LOG(Info, "WP51-INLINEES object=%s procs=%zu sites=%zu closures=%zu unreadable=%zu refused=%zu would_refuse=%zu", in_.object_name.c_str(), wp51_procs_, wp51_sites_,
                wp51_closures_, wp51_unreadable_, wp51_refused_, wp51_would_refuse_);
    }

    // Decide the matched ones.
    for (const auto& [pos, m] : matched) {
        Sym& sy = syms_[pos];
        if (sy.decided) {
            continue;
        }
        const TargetAddress addr = module_address(m.view->rva);
        const std::string how = std::string("matched record '") + std::string(m.view->name) + "' at rva " + hex(m.view->rva) + " in compiland " + std::to_string(ci_) +
                                " (" + to_string(m.how) + ")";
        if (sy.dec.type == SymbolType::Function) {
            decide(sy, DecisionKind::PatchHooked, m.how, SymbolType::Function, addr, "static function present in the old compiland: " + how);
        } else {
            decide(sy, DecisionKind::BindOldData, m.how, SymbolType::Data, addr, "static data present in the old compiland: " + how);
        }
    }

    // WP-46: the lambdas named by a hash (F-41.2), before the guards are settled (a static refused here takes its guard
    // along); then the layout for guards without a record (F-41.1).
    note_hashed_lambdas();
    learn_guard_layout();
    settle_guards();
    build_lambda_sets();
    // Anything still undecided (defensive).
    for (Sym& sy : syms_) {
        if (sy.defined && !sy.decided) {
            decide(sy, sy.dec.type == SymbolType::Function ? DecisionKind::PatchNewFunction : DecisionKind::PatchNewData, BindSource::None, sy.dec.type, {}, "unmatched static: new");
        }
    }
    refuse_reset_statics();
}

namespace {

// WP-52: the outermost function of a canonical display name of a function-local entity's scope: the part in front of its
// first lambda ("o2_nest_step::<lambda>::operator()::<lambda>::operator()" -> "o2_nest_step"; "f" -> "f").
std::string top_of(const std::string& canonical) {
    const std::size_t at = canonical.find("::<lambda>");
    return at == std::string::npos ? canonical : canonical.substr(0, at);
}

}  // namespace

std::optional<std::string> Planner::top_function_of(std::uint32_t pos) {
    const Sym& s = syms_[pos];
    const std::string base = base_identifier(s.name);
    if (base.empty()) {
        return std::nullopt;
    }
    std::string display;
    if (const std::optional<LocalScopeKey>& key = scope_keys(pos)[0]) {
        // The display of the name with its back-references written out (cl's `??R1?1??` reads as `<lambda_1>`).
        display = display_name("?" + base + "@" + key->block + key->rest);
    } else {
        display = s.dec.display;
    }
    if (display.empty() || display[0] == '?') {
        return std::nullopt;
    }
    const std::string canonical = canonical_lambda_names(normalized_pdb_name(display));
    const std::string suffix = "::" + base;
    if (canonical.size() <= suffix.size() || canonical.compare(canonical.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return std::nullopt;
    }
    const std::string top = top_of(canonical.substr(0, canonical.size() - suffix.size()));
    if (top.empty()) {
        return std::nullopt;
    }
    return top;
}

// WP-52 (ADR-0045 ruling, R-66 and cl's nested lambdas): a function-local static the plan decided NEW while the old
// compiland holds a data record of its name in a procedure of the same outermost function (the function itself, the call
// operators of its lambdas, procedures they were inlined into) may be that record's live object: the planner did not find
// which record is its own (R-68: closures whose names use back-references; R-66: a nested lambda inlined all the way), and a
// new static would silently reset the running state. Refused (CML3207). A static of a name that no procedure of its
// function records is new, as before. Procedures are matched by their display name, lambda numbers and parameter lists
// aside (overloads collapse: an accepted over-refusal); one whose function cannot be told counts as the same. Guards are
// not looked at: a guard follows its static.
void Planner::refuse_reset_statics() {
    bool have_records = false;
    for (std::uint32_t pos = 0; pos < syms_.size(); ++pos) {
        Sym& s = syms_[pos];
        if (!s.defined || s.dec.type != SymbolType::Data || !s.decided || s.dec.decision != DecisionKind::PatchNewData || s.diagnosed || s.name.size() < 2 ||
            s.name[0] != '?' || s.name[1] == '?' || is_guard_variable(s.name)) {
            continue;
        }
        if (s.dec.section != 0 && secs_[s.dec.section - 1].tls) {
            continue;  // a new thread_local is WP-24's (CML3400)
        }
        const std::size_t at = s.name.find('@');
        if (at == std::string::npos || block_prefix_length(std::string_view(s.name).substr(at + 1)) == 0) {
            continue;  // not function-local
        }
        if (!have_records) {
            if (!ensure_compiland_symbols()) {
                return;  // nothing of the compiland can be read: no record was found by anyone either
            }
            have_records = true;
        }
        const std::string base = base_identifier(s.name);
        const std::optional<std::string> top = top_function_of(pos);
        for (const pdb::SymbolView& v : comp_syms_) {
            if (!pdb::is_data_kind(v.kind) || v.parent_proc == pdb::kNoSymbol || v.parent_proc >= comp_syms_.size() || comp_last(&v) != base) {
                continue;
            }
            // (A thread-local record never stands for plain data: a static that lost thread_local is another variable.)
            if (v.kind == pdb::SymbolKind::LocalThread || v.kind == pdb::SymbolKind::GlobalThread) {
                continue;
            }
            const pdb::SymbolView& proc = comp_syms_[v.parent_proc];
            const std::string proc_top = top_of(comp_norm(&proc).canonical);
            const bool same = !top.has_value() || proc_top.empty() || proc_top == *top;
            CML_LOG(Trace, "bind: new static %s: old record '%s' at rva %s in '%s' (function '%s'): %s", s.name.c_str(), std::string(v.name).c_str(), hex(v.rva).c_str(),
                    std::string(proc.name).c_str(), proc_top.c_str(), same ? "of its function: refused" : "another function");
            if (!same) {
                continue;
            }
            const std::string whose = top.has_value() && !proc_top.empty() ? "a procedure of its function '" + *top + "'"
                                                                            : "a procedure whose function cannot be told apart from its own";
            error(diag::kAmbiguousStatic,
                  "static data " + describe(s) + " in " + in_.object_name + " would be new in the patch, but compiland " + std::to_string(ci_) + " records '" +
                      std::string(v.name) + "' at rva " + hex(v.rva) + " in '" + std::string(proc.name) + "', " + whose +
                      ": that record may be this static's live object (the planner could not tell which record is its own), and a new static would reset it",
                  "Hot-restart, or rename the static if it really is new.");
            s.diagnosed = true;
            break;
        }
    }
}

// WP-59 (R-67, ADR-0046): the external guards of an inline function's statics. Like those statics they are external, each
// in a COMDAT of its own, and decide_defined() bound them to the old guard of the same name by its public symbol, or made
// them new, as ordinary data. Two kinds:
//   * clang's bitmask `??_B<scope>@5<n>` (no thread-safe statics): ONE per function, its bits go to the function's statics
//     in the order they are declared (every static counted, constant-initialised ones too);
//   * the thread-safe guard `?$TSS<n>@<scope>@4HA` of one static (the default; review item 0): named in the static's block,
//     numbered by the static's place among the FUNCTION's statics in declaration order (clang-cl 18: a static declared in
//     front of `a` gets `$TSS0`, the name of a's old guard, and a's guard becomes `$TSS1`).
// So a guard's name says which static it belongs to only while the function's statics are as before. The rules of cl's
// `$S1` apply (ADR-0043 ruling), on the statics of its function (`??_B`) or of its block (`$TSS`), thread_locals not
// counted; the layout never places them:
//   * a static refused: the guard is not bound either;
//   * statics bound to the running process next to new ones: refused (CML3600), with the old guard or a fresh one;
//   * every static new: a new guard, also where the old image has one of that name (it is other statics');
//   * every static bound: the old guard of its name; where the old image has none, refused (CML3600): a fresh one would
//     construct them again;
//   * no static tied: new where the old image has none (the warning CML3211, as for any guard tied to no static), else as
//     decided.
// A bound guard must then be proven: a `??_B` and a `$TSS` by the code of its function (bitmask_guards,
// prove_bitmask_guards), except a `$TSS` whose block holds that one static and that one guard in the patch, and only that
// guard number in the old image (external_tss_alone_in_block): its static is the only one it can belong to.
void Planner::settle_external_guard(Sym& sy, std::vector<std::uint32_t>& untied) {
    const bool bitmask = is_inline_bitmask_guard(sy.name);
    if (!bitmask && sy.name.compare(0, 5, "?$TSS") != 0) {
        return;  // another external guard (an external `?$S<n>` is not known to exist): as decided
    }
    if (sy.dec.section != 0 && secs_[sy.dec.section - 1].tls) {
        return;  // a thread-local guard: WP-24's (CML3400)
    }
    const char* const kind = bitmask ? "the bitmask guard " : "the guard ";
    const char* const what = bitmask ? " (an inline function's statics without thread-safe initialisation)" : " (of an inline function's static)";
    const bool found = sy.decided && sy.dec.decision == DecisionKind::BindOldData;
    const std::vector<std::uint32_t> scope = guard_scope_statics(sy);
    std::size_t refused = 0;
    std::size_t fresh = 0;
    std::size_t bound = 0;
    std::string a_new;
    std::string a_bound;
    std::uint32_t the_static = UINT32_MAX;
    for (const std::uint32_t pos : scope) {
        const Sym& st = syms_[pos];
        if (st.decided && st.dec.decision == DecisionKind::PatchFresh) {
            continue;  // read-only data: constant-initialised, no state, no guard
        }
        the_static = pos;
        if (st.diagnosed) {
            ++refused;
        } else if (st.decided && st.dec.decision == DecisionKind::BindOldData) {
            ++bound;
            a_bound = a_bound.empty() ? describe(st) : a_bound;
        } else {
            ++fresh;
            a_new = a_new.empty() ? describe(st) : a_new;
        }
    }
    CML_LOG(Trace, "bind: external guard %s: %zu static(s) of its %s (%zu bound, %zu new, %zu refused), %s", sy.name.c_str(), scope.size(), bitmask ? "function" : "block", bound, fresh,
            refused, found ? "an old guard of its name is in the old image" : "no old guard of its name");
    if (refused != 0) {
        sy.diagnosed = true;
        decide(sy, DecisionKind::PatchNewData, BindSource::None, SymbolType::Data, {}, "external guard of a static that is refused");
        return;
    }
    if (bound != 0 && fresh != 0) {
        error(diag::kGuardMismatch,
              std::string(kind) + describe(sy) + " in " + in_.object_name + what + " guards statics of which some are bound to the running process (" + a_bound + ") and some are new (" +
                  a_new + "); " +
                  (bitmask ? "its bits go to the function's statics in the order they are declared"
                           : "the guards of the function are numbered by their statics' place in it, in the order they are declared") +
                  ", so with the old guard a new static could take an old one's and never be constructed, and with a fresh one the old statics would be constructed again",
              "Declare the new static in another function (or keep this function's statics as they are), or hot-restart.");
        sy.diagnosed = true;
        decide(sy, DecisionKind::PatchNewData, BindSource::None, SymbolType::Data, {}, "external guard of new and bound statics (CML3600)");
        return;
    }
    if (bound == 0) {
        if (fresh != 0) {
            decide(sy, DecisionKind::PatchNewData, BindSource::None, SymbolType::Data, {},
                   std::string("external guard of new statics: new") + (found ? " (the old guard of that name belongs to other statics)" : ""));
        } else if (!found) {
            untied.push_back(static_cast<std::uint32_t>(&sy - syms_.data()));
            decide(sy, DecisionKind::PatchNewData, BindSource::None, SymbolType::Data, {}, "external guard tied to no static and not in the old image (CML3211): new");
        } else if (!bitmask) {
            external_guards_to_prove_.insert(static_cast<std::uint32_t>(&sy - syms_.data()));  // bound by its name, to whose static?
        }
        return;
    }
    if (!found) {
        error(diag::kGuardMismatch,
              std::string(kind) + describe(sy) + " in " + in_.object_name + what + " cannot be bound to the old guard of " + a_bound +
                  ", which is bound to the running process: the old image has no " + (bitmask ? "bitmask" : "guard") +
                  " of that name (there the static was constant-initialised, or the function's statics were others or in another order); with a fresh one the old object would be constructed a second time",
              "Keep this function's statics as they are (or declare the new one in another function), or hot-restart.");
        sy.diagnosed = true;
        decide(sy, DecisionKind::PatchNewData, BindSource::None, SymbolType::Data, {}, "external guard of bound statics that is not in the old image (CML3600)");
        return;
    }
    if (bitmask) {
        return;  // the old bitmask, kept only while the function is unchanged (bitmask_guards)
    }
    std::string why;
    if (bound == 1 && external_tss_alone_in_block(sy, the_static, why)) {
        CML_LOG(Trace, "bind: external guard %s: the only guard of its block, and the only one of that block in the old image, tied to its only static %s: bound by its name",
                sy.name.c_str(), syms_[the_static].name.c_str());
        return;
    }
    CML_LOG(Trace, "bind: external guard %s is bound by its name, which does not prove its static (%s): the code of its function must be unchanged", sy.name.c_str(), why.c_str());
    external_guards_to_prove_.insert(static_cast<std::uint32_t>(&sy - syms_.data()));
}

// WP-59 (review item 0): may the external `$TSS<n>` guard `guard`, tied to the one static `static_pos` of its block, keep
// the old guard of its name without its function's code? A guard is named in its static's block, so in a block with ONE
// static and ONE guard the guard is that static's. The old guard of the same name is the old static's when the old image has
// exactly that one guard number in the block: the block's one old guarded static had that number. (The residual: that one
// was another static, deleted in the same edit that made the static right behind it, constant-initialised until then,
// dynamic: the new initialiser runs over the live value or not at all, docs/wp/WP-59.md.) Anything else is left to the code.
bool Planner::external_tss_alone_in_block(const Sym& guard, std::uint32_t static_pos, std::string& why) {
    std::uint32_t number = 0;
    std::string_view scope;
    if (static_pos == UINT32_MAX || !tss_guard_parts(guard.name, number, scope)) {
        why = "its name cannot be read";
        return false;
    }
    for (const Sym& other : syms_) {
        std::uint32_t n = 0;
        std::string_view sc;
        if (&other != &guard && other.defined && !other.is_static && tss_guard_parts(other.name, n, sc) && sc == scope) {
            why = "the patch has another guard in its block, " + other.name;
            return false;
        }
    }
    if (!old_tss_loaded_) {
        old_tss_loaded_ = true;
        Result<std::vector<pdb::SymbolView>> pubs = pdb_.all_publics();
        if (pubs) {
            old_tss_listed_ = true;
            for (const pdb::SymbolView& v : *pubs) {
                std::uint32_t n = 0;
                std::string_view sc;
                if (tss_guard_parts(v.name, n, sc)) {
                    old_tss_numbers_[std::string(sc)].insert(n);
                }
            }
        } else {
            CML_LOG(Debug, "bind: the publics of the old image cannot be listed: %s", pubs.error().message.c_str());
        }
    }
    if (!old_tss_listed_) {
        why = "the old image's publics cannot be listed";
        return false;
    }
    const auto it = old_tss_numbers_.find(std::string(scope));
    if (it == old_tss_numbers_.end() || it->second.size() != 1 || *it->second.begin() != number) {
        why = "the old image has " + std::to_string(it == old_tss_numbers_.end() ? 0 : it->second.size()) + " guard(s) in its block";
        return false;
    }
    return true;
}

// Pass C, and the rule that ties every guard variable to the statics of its scope (WP-40, ADR-0043 point 1 and 2). A
// guard `?$TSS0@<scope>@4HA` (or cl's `?$S1@<scope>@4IA` without thread-safe statics) guards the statics declared in
// the same block, `?<name>@<scope>@4...`. Binding a guard to the old guard while its static is new makes the patch use
// an object nobody constructed (the old guard says "done"); giving a bound static a fresh guard constructs the old,
// live object a second time. So:
//   * a static of the scope is refused: the guard belongs to an error, it is settled without a word (no CML3211 next to
//     the CML3207, R-48 item 9);
//   * every static of the scope is new (or the scope has none and the guard has no record): the guard is new as well. A
//     guard that was matched to an old guard by its name (cl records guards, and numbers them per function: a new static
//     declared in front of an old one takes the old one's guard name) is unbound again. A guard whose scope has NO static
//     and no record is new with the warning CML3211; then require_bound_guards (WP-52, ADR-0045) refuses every bound
//     static that needs a guard and has no bound one (the error CML3211, naming the static);
//   * some are new and some are bound: a guard WITHOUT an old record cannot be told to be the new static's or the old
//     one's (cl and clang-cl number the guards of a function, not of a static; a new guarded static in front of an old one
//     takes the old one's number), and either way one of them is wrong: CML3600. A guard WITH an old record stays bound:
//     the new static next to it either needs no guard, or its own guard has no record and is refused here;
//   * every static is bound: a guard with an old record keeps it; one without (clang-cl records no guards) is placed by
//     the section layout ONLY if the layout is known (every matched static of the object section agrees on it), the
//     address lies in this compiland's contribution to the image section that layout maps into, and no record of the
//     compiland names other data at that address. Otherwise CML3600: the static is bound and would be constructed again
//     (R-48 item 6: before, this was the warning CML3211 and the reload went on).
void Planner::settle_guards() {
    std::vector<std::uint32_t> untied;  // WP-52: guards tied to no static (new)
    for (Sym& sy : syms_) {
        if (!sy.defined || sy.dec.type != SymbolType::Data || !is_guard_variable(sy.name) || sy.diagnosed) {
            continue;
        }
        if (is_inline_bitmask_guard(sy.name) || !sy.is_static) {
            settle_external_guard(sy, untied);
            continue;
        }
        const bool by_record = sy.decided && sy.dec.decision == DecisionKind::BindOldData && sy.dec.source != BindSource::CompilandLayout;
        if (sy.decided && !by_record) {
            continue;  // decided by an earlier step of its own (a thread-local guard rejected, ...)
        }
        const std::vector<std::uint32_t> scope = guard_scope_statics(sy);
        std::size_t refused = 0;
        std::size_t fresh = 0;
        std::size_t bound = 0;
        std::string a_new;
        std::string a_bound;
        for (const std::uint32_t pos : scope) {
            const Sym& st = syms_[pos];
            if (st.decided && st.dec.decision == DecisionKind::PatchFresh) {
                continue;  // read-only data: no state, no guard
            }
            if (st.diagnosed) {
                ++refused;
            } else if (st.decided && st.dec.decision == DecisionKind::BindOldData) {
                ++bound;
                a_bound = a_bound.empty() ? describe(st) : a_bound;
            } else {
                ++fresh;
                a_new = a_new.empty() ? describe(st) : a_new;
            }
        }
        const bool tls = sy.dec.section != 0 && secs_[sy.dec.section - 1].tls;
        CML_LOG(Trace, "bind: guard %s: %zu static(s) in its scope (%zu bound, %zu new, %zu refused)%s", sy.name.c_str(), scope.size(), bound, fresh, refused,
                by_record ? ", bound by its record" : "");
        if (refused != 0) {
            sy.diagnosed = true;
            decide(sy, DecisionKind::PatchNewData, BindSource::None, SymbolType::Data, {}, "guard variable of a static that is refused");
            continue;
        }
        if (fresh != 0 && bound != 0 && !by_record) {
            error(diag::kGuardMismatch,
                  "the guard variable " + describe(sy) + " in " + in_.object_name + " guards statics of one block of which some are bound to the running process (" + a_bound +
                      ") and some are new (" + a_new + "); which guard belongs to which static cannot be told, so either an old static would be constructed again or a new one never",
                  "Declare the new static in a block of its own (or in another function), or hot-restart.");
            sy.diagnosed = true;
            decide(sy, DecisionKind::PatchNewData, BindSource::None, SymbolType::Data, {}, "guard of a block with new and bound statics (CML3600)");
            continue;
        }
        if (bound == 0) {
            // Every static of the scope is new, or there is none.
            if (by_record && fresh == 0) {
                continue;  // a guard with an old record and nothing in the patch to contradict it
            }
            if (tls) {
                decide(sy, DecisionKind::PatchNewData, BindSource::None, SymbolType::Data, {}, "guard variable of a function-local thread_local not found in the old image (rejected: CML3400)");
                continue;
            }
            if (fresh != 0) {
                CML_LOG(Trace, "bind: guard %s: its static%s new: the guard is new%s", sy.name.c_str(), fresh == 1 ? " is" : "s are",
                        by_record ? " (its name matched an old guard, which belongs to another static)" : "");
                decide(sy, DecisionKind::PatchNewData, BindSource::None, SymbolType::Data, {},
                       std::string("guard variable of a new static: new") + (by_record ? " (the old guard of that name belongs to another static)" : ""));
                continue;
            }
            // WP-52 (R-60): a guard tied to no static is new. Its warning comes after require_bound_guards, which refuses a
            // bound static that needs a guard and has no bound one (that refusal names the guard, which then gets no warning).
            untied.push_back(static_cast<std::uint32_t>(&sy - syms_.data()));
            decide(sy, DecisionKind::PatchNewData, BindSource::None, SymbolType::Data, {}, "guard variable not found in the old image (CML3211): new");
            continue;
        }
        // Every static of the scope is bound (or, for a guard with an old record, some are: a new static without a guard of
        // its own next to them; a new guarded one would have a guard without a record, which is refused above).
        if (by_record) {
            continue;
        }
        std::string why_not;
        // WP-40 (rule (d)): a guard of a lambda paired by the code of its call operator with the old lambda of another
        // number sits, in the old image, where the layout puts the guard of the patch's lambda of THAT number.
        const Sym* position = &sy;
        for (const auto& [from, to] : closure_pairing_) {
            if (from == to || !in_closure(sy, from)) {
                continue;
            }
            position = closure_twin(sy, from, to);
            break;
        }
        const auto d = guard_delta_.find(sy.dec.section);
        if (position == nullptr) {
            why_not = "its lambda was paired with the old lambda of another number, and the patch has no guard of that number in the same section";
        } else if (sy.dec.section == 0 || d == guard_delta_.end()) {
            why_not = "no layout of its object section is known (no static matched there, or the matched ones disagree)";
        } else {
            const std::int64_t rva = d->second + static_cast<std::int64_t>(position->s->value);
            const auto image_section = guard_delta_image_section_.find(sy.dec.section);
            const std::optional<pdb::SectionOffset> so =
                rva >= 0 && rva < 0x7FFFFFFF ? pdb_.section_offset_from_rva(static_cast<std::uint32_t>(rva)) : std::nullopt;
            const std::optional<std::uint32_t> owner = so ? pdb_.compiland_for_rva(static_cast<std::uint32_t>(rva)) : std::nullopt;
            const std::optional<std::uint32_t> owner_end = so ? pdb_.compiland_for_rva(static_cast<std::uint32_t>(rva) + 3u) : std::nullopt;
            // WP-46 (review): the guard (4 bytes) must not overlap any variable the old compiland records, guards included (they
            // were skipped by name): each record's extent is its type's size, or, where that cannot be known, everything up to
            // the next record of the section (when in doubt, refuse).
            const pdb::SymbolView* other = nullptr;
            std::string other_extent;
            if (owner && *owner == ci_ && so && ensure_compiland_symbols()) {
                const std::uint64_t g_begin = static_cast<std::uint64_t>(rva);
                const std::uint64_t g_end = g_begin + 4;
                std::vector<const pdb::SymbolView*> records;
                for (const pdb::SymbolView& v : comp_syms_) {
                    if (pdb::is_data_kind(v.kind) && v.rva != pdb::kNoRva && v.section == so->section) {
                        records.push_back(&v);
                    }
                }
                std::sort(records.begin(), records.end(), [](const pdb::SymbolView* a, const pdb::SymbolView* b) { return a->rva < b->rva; });
                for (std::size_t k = 0; k < records.size() && other == nullptr; ++k) {
                    const pdb::SymbolView& v = *records[k];
                    std::optional<std::uint64_t> size = pdb_.type_size(v.type_index);
                    const bool known = size.has_value() && *size != 0;
                    std::uint64_t end = known ? v.rva + *size : 0;
                    if (!known) {
                        std::size_t next = k + 1;
                        while (next < records.size() && records[next]->rva == v.rva) {
                            ++next;
                        }
                        end = next < records.size() ? records[next]->rva : std::numeric_limits<std::uint64_t>::max();
                    }
                    if (g_begin < end && g_end > v.rva) {
                        other = &v;
                        other_extent = known ? hex(v.rva) + ".." + hex(end) : hex(v.rva) + " (size unknown)";
                    }
                }
            }
            if (!so || !owner || *owner != ci_ || !owner_end || *owner_end != ci_) {
                why_not = "the layout of object section " + std::to_string(sy.dec.section) + " puts it at rva " + hex(static_cast<std::uint64_t>(std::max<std::int64_t>(rva, 0))) +
                          ", outside this translation unit's contribution to the old image";
            } else if (image_section == guard_delta_image_section_.end() || image_section->second != so->section) {
                why_not = "the layout of object section " + std::to_string(sy.dec.section) + " puts it at rva " + hex(static_cast<std::uint64_t>(rva)) +
                          ", in another image section than the statics that taught the layout";
            } else if (other != nullptr) {
                why_not = "the layout of object section " + std::to_string(sy.dec.section) + " puts it at rva " + hex(static_cast<std::uint64_t>(rva)) + ", where the old image records '" +
                          std::string(other->name) + "' (" + other_extent + ")";
            } else {
                decide(sy, DecisionKind::BindOldData, BindSource::CompilandLayout, SymbolType::Data, module_address(static_cast<std::uint32_t>(rva)),
                       "guard variable of a function-local static: inferred at rva " + hex(static_cast<std::uint64_t>(rva)) + " from the unchanged layout of object section " +
                           std::to_string(sy.dec.section) + " (every matched static there agrees)");
                continue;
            }
        }
        if (tls) {
            decide(sy, DecisionKind::PatchNewData, BindSource::None, SymbolType::Data, {}, "guard variable of a function-local thread_local not found in the old image (rejected: CML3400)");
            continue;
        }
        error(diag::kGuardMismatch,
              "the guard variable " + describe(sy) + " in " + in_.object_name + " cannot be bound to the old guard of " + a_bound + ", which is bound to the running process: " + why_not +
                  "; with a fresh guard the old object would be constructed a second time",
              "Keep the statics of this source file in their order (do not add or remove statics in front of them), or hot-restart.");
        sy.diagnosed = true;
        decide(sy, DecisionKind::PatchNewData, BindSource::None, SymbolType::Data, {}, "guard of a bound static that cannot be found (CML3600)");
    }
    // WP-52 (ADR-0045, convergence ruling): every bound static that needs a guard has a bound guard, or it is refused.
    const std::set<std::uint32_t> implicated = require_bound_guards(untied);
    for (const std::uint32_t pos : untied) {
        if (implicated.count(pos) == 0) {
            warn(diag::kGuardUnmatched, "guard variable '" + syms_[pos].dec.display + "' of a function-local static was not found in the old image; the patch gets a fresh guard",
                 "Keep the layout of the statics in this source file unchanged, or hot-restart if the static must not be initialised again.");
        }
    }
    // WP-40 (second high review, ADR-0043 ruling): a bitmask bound to the old one, by its record or by the layout, is right
    // only while the function hands out its bits as before (the same dynamically initialised statics in the same order).
    // Nothing in the records shows that; the pipeline proves it from the function's code (BitmaskGuard) or refuses.
    for (const Sym& sy : syms_) {
        if (sy.defined && sy.decided && !sy.diagnosed && sy.dec.decision == DecisionKind::BindOldData && is_guard_variable(sy.name)) {
            const std::string function = bitmask_function(sy.name);
            if (!function.empty() || is_inline_bitmask_guard(sy.name) || (sy.name.size() > 3 && sy.name.compare(0, 3, "?$S") == 0 && sy.name[3] >= '0' && sy.name[3] <= '9')) {
                plan_.bitmask_guards.push_back({sy.name, function});
                CML_LOG(Trace, "bind: bitmask guard %s is bound to the old one: the code of '%s' must be unchanged", sy.name.c_str(), function.c_str());
            } else if (external_guards_to_prove_.count(static_cast<std::uint32_t>(&sy - syms_.data())) != 0) {
                // WP-59 (review item 0): an inline function's `$TSS<n>` bound by its name, which nothing but the code proves.
                const std::string tss_function = tss_guard_function(sy.name);
                plan_.bitmask_guards.push_back({sy.name, tss_function});
                CML_LOG(Trace, "bind: external guard %s is bound to the old one by its name: the code of '%s' must be unchanged", sy.name.c_str(), tss_function.c_str());
            }
        }
    }
}

// ---- thread_local (WP-24) ----------------------------------------------------------------------

void Planner::new_thread_local_error(Sym& sy) {
    sy.diagnosed = true;
    if (is_guard_variable(sy.name)) {
        error(diag::kNewThreadLocal,
              "the initialisation guard '" + sy.dec.display + "' of a function-local thread_local in " + in_.object_name +
                  " was not found in the old image (the thread_local is new, or the layout of the thread-locals of this source file changed); patch-local thread-local storage is not supported",
              "Hot-restart, or keep the thread_locals of this source file unchanged.");
        return;
    }
    error(diag::kNewThreadLocal,
          "'" + sy.dec.display + "' is a new thread_local: " + in_.object_name + " defines it but the old image has no such thread-local variable; patch-local thread-local storage is not supported",
          "Hot-restart, or move the new thread_local to an unchanged source file (and rebuild).");
}

// Statics are decided in bridge_statics(); a thread_local static that did not match an old record (or whose
// match is not bound) is new. One error per variable: statics that already got a diagnostic (ambiguity) are skipped.
void Planner::reject_new_thread_locals() {
    for (Sym& sy : syms_) {
        if (!sy.defined || !sy.decided || sy.dec.section == 0 || !secs_[sy.dec.section - 1].tls || !sy.is_static || sy.diagnosed) {
            continue;
        }
        if (sy.dec.decision == DecisionKind::BindOldData) {
            continue;
        }
        new_thread_local_error(sy);
    }
}

// A thread_local with a dynamic initialiser or a destructor: the old image's initialiser (the CRT's TLS
// callback over the old module's .CRT$XD* table) stays in effect. The patch has no TLS directory, so
// nothing of the patch's own table ever runs.
void Planner::warn_thread_local_initialisers() {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < secs_.size(); ++i) {
        if (!starts_with(secs_[i].name, ".CRT$XD")) {
            continue;
        }
        for (const coff::ResolvedRelocation& rr : relocs_[i]) {
            names.push_back(std::string(rr.target->name));
        }
        if (relocs_[i].empty()) {
            names.push_back(secs_[i].name);
        }
    }
    for (const Sym& sy : syms_) {
        if (!sy.defined && (sy.name == "__tlregdtor" || sy.name == "_tlregdtor" || starts_with(sy.name, "__dyn_tls_") || starts_with(sy.name, "_dyn_tls_"))) {
            names.push_back(sy.name);
        }
    }
    if (names.empty()) {
        return;
    }
    std::string list;
    for (std::size_t i = 0; i < names.size() && i < 4; ++i) {
        list += (i != 0 ? ", " : "") + std::string("'") + names[i] + "'";
    }
    if (names.size() > 4) {
        list += ", ...";
    }
    warn(diag::kThreadLocalInit,
         in_.object_name + " has thread_local variables with dynamic initialisation or a destructor (" + list +
             "); the old image's initialiser and destructor stay in effect (existing threads keep their values, new threads run the OLD initialiser), a changed initialiser is not run",
         "Hot-restart if the changed initialiser must take effect.");
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
            const std::vector<const pdb::SymbolView*> cands =
                static_candidates(base, encl, want_function, sy.dec.display == name ? std::string() : sy.dec.display, -1, want_function ? name : std::string());
            // (A record of a lambda's static in the procedure that inlined the lambda does not identify it by name.)
            const std::string enclosing = want_function || encl != "operator()" ? std::string() : lambda_enclosing(sy.dec.display, base);
            if (cands.size() == 1 && !is_inlined_record(cands.front(), enclosing)) {
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
    // (3a) an external thread_local of the module: S_GTHREAD32 (its public does not carry the TLS offset). One
    // global-stream lookup serves every undefined data symbol; a SECREL target must come from here (or from the
    // compiland's S_LTHREAD32 above), never from the publics.
    {
        const ThreadLookup tl = lookup_thread_global(sy);
        if (tl.ambiguous) {
            error(diag::kAmbiguousThreadLocal,
                  "thread_local " + describe(sy) + " matches several S_GTHREAD32 records of the old image; cannot tell which one " + in_.object_name + " uses",
                  "Rename one of the thread_locals, or hot-restart.");
            sy.diagnosed = true;
            return std::nullopt;
        }
        if (tl.ref) {
            return tl.ref;
        }
    }
    if (fatal_) {
        return std::nullopt;
    }
    if (sy.via_secrel) {
        error(diag::kThreadLocal,
              "thread-local reference to " + describe(sy) + " from " + referencing_description(sy.dec.first_ref_section, sy.dec.first_ref_offset) + " in " + in_.object_name +
                  ": the old image's PDB has no S_GTHREAD32 / S_LTHREAD32 record for it, and a public symbol does not give a thread-local's offset",
              "Move the thread_local to an unchanged source file, or hot-restart.");
        sy.diagnosed = true;
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
            if (rr.reloc.type == coff::RelocType::SecRel) {
                sy.via_secrel = true;
            }
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
            if (!sy.diagnosed) {
                const std::string where = referencing_description(sy.dec.first_ref_section, sy.dec.first_ref_offset);
                error(diag::kUnresolved,
                      "unresolved external symbol " + describe(sy) + " referenced from " + where + " in " + in_.object_name,
                      "Define it in a module of the process (or link the library that provides it into the patched module), or hot-restart.");
            }
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

// Bytes a relocation of this type patches; 0 for a type whose field is not known (it then fits no operand).
std::uint32_t reloc_field_width(coff::RelocType t) {
    switch (t) {
        case coff::RelocType::Addr64: return 8;
        case coff::RelocType::Addr32:
        case coff::RelocType::Addr32Nb:
        case coff::RelocType::Rel32:
        case coff::RelocType::Rel32_1:
        case coff::RelocType::Rel32_2:
        case coff::RelocType::Rel32_3:
        case coff::RelocType::Rel32_4:
        case coff::RelocType::Rel32_5:
        case coff::RelocType::SecRel:
        case coff::RelocType::SRel32: return 4;
        case coff::RelocType::Section: return 2;
        case coff::RelocType::SecRel7: return 1;
        default: return 0;
    }
}
}  // namespace

// The prover of one section (WP-36): its relocation fields and the starts of the functions it defines.
SiteProver& Planner::site_prover(std::uint32_t sec_index) {
    std::unique_ptr<SiteProver>& slot = site_provers_[sec_index];
    if (!slot) {
        const SecInfo& si = secs_[sec_index - 1];
        std::vector<RelocField> fields;
        fields.reserve(relocs_[sec_index - 1].size());
        for (const coff::ResolvedRelocation& rr : relocs_[sec_index - 1]) {
            if (rr.reloc.type == coff::RelocType::Absolute) {
                continue;  // the linker ignores it: it patches nothing
            }
            fields.push_back(RelocField{rr.reloc.virtual_address, reloc_field_width(rr.reloc.type)});
        }
        std::vector<std::uint32_t> starts;
        for (const coff::Symbol& s : obj_.symbols()) {
            if (s.section_number == static_cast<std::int32_t>(sec_index) && s.is_function() && (s.is_external() || s.is_static())) {
                starts.push_back(s.value);
            }
        }
        slot = std::make_unique<SiteProver>(si.code, si.sec->data, std::move(fields), std::move(starts));
    }
    return *slot;
}

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
            // The TLS invariant: a SECREL against data of the old image must resolve inside the old .tls section
            // from a thread-local record (S_GTHREAD32 / S_LTHREAD32), and nothing else may point into .tls (the
            // address of a thread-local is per thread, a REL32/ADDR64 to the template is silent corruption).
            if (dk == DecisionKind::BindOldData && ts.dec.old_address.rva != kNoRva) {
                const bool in_tls = in_old_tls(ts.dec.old_address.rva);
                const BindSource src = ts.dec.source;
                // (WP-37: a function-local thread_local paired by its closure or by its place in its function was paired
                // with a thread-local record as well: both steps only see candidates of the member's storage kind.)
                const bool thread_source = src == BindSource::PdbGlobal || src == BindSource::CompilandStatic || src == BindSource::CompilandRva ||
                                           src == BindSource::CompilandClosure || src == BindSource::CompilandProcedure;
                if (secrel && !(in_tls && thread_source)) {
                    error(diag::kThreadLocal,
                          "SECREL reference to " + describe(ts) + " from " + where() + " in " + in_.object_name +
                              ", which is not a thread-local variable of the old image (" + (in_tls ? "no thread-local record" : "not in the thread-local storage") + "; source " + to_string(src) +
                              ", rva " + hex(ts.dec.old_address.rva) + ")",
                          "Move the thread_local to an unchanged source file, or hot-restart.");
                    continue;
                }
                if (!secrel && in_tls) {
                    error(diag::kThreadLocal,
                          std::string(coff::reloc_type_name(type)) + " reference to thread_local " + describe(ts) + " from " + where() + " in " + in_.object_name +
                              "; only the SECREL access sequence of thread-local variables can be retargeted",
                          "Hot-restart, or move the thread_local to an unchanged source file.");
                    continue;
                }
            }
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
            const bool imgrel = type == coff::RelocType::Addr32Nb;
            if (imgrel) {
                if (dk != DecisionKind::BindOldData) {
                    continue;  // to a thunk: legal, resolved by the linker
                }
                if (ts.dec.source == BindSource::SynthesizedImportSlot) {
                    continue;  // the placeholder itself is the slot: the reference stays as linked, relative to the patch
                }
                // WP-36 (ADR-0042): retargetable only where the site is PROVED to be the sign-extended 32-bit
                // displacement of a memory operand (`[reg + index*scale + sym@IMGREL]`). Everything else is CML3101.
                ++plan_.imgrel_sites;
                const SiteCheck sc = site_prover(sec_index).check(off);
                CML_LOG(Trace, "bind: imgrel site sec=%u(%s) off=0x%x target=%s verdict=%s: %s", sec_index, si.name.c_str(), off, ts.name.c_str(),
                        to_string(sc.verdict), sc.reason.c_str());
                if (!sc.proved()) {
                    error(diag::kAddr32NbToOldData,
                          where() + " references " + describe(ts) + " in " + ts.dec.old_address.module + " image-relatively (ADDR32NB) at +" + hex(off) +
                              " of section '" + si.name + "' in " + in_.object_name + ", and the site is not proved to be an address displacement: " +
                              sc.reason + "; this cannot be retargeted",
                          "Move the reference behind a function or pointer, or hot-restart.");
                    continue;
                }
                if (!rel32_reachable(ts.dec)) {
                    error(diag::kImgrelOutOfRange,
                          "image-relative reference to " + describe(ts) + " (" + hex(ts.dec.old_address.va) + ", " + ts.dec.old_address.module + ") from " + where() +
                              " in " + in_.object_name + " cannot reach it: it is more than 2 GiB away from every place the patch can be loaded",
                          "Access the data through a function or pointer in the module, or hot-restart.");
                    continue;
                }
                ++plan_.imgrel_verified;
            }
            const bool rel = coff::is_rel32_family(type);
            if (!rel && !secrel && !imgrel && type != coff::RelocType::Addr64) {
                error(diag::kUnsupportedReloc,
                      "unsupported relocation " + std::string(coff::reloc_type_name(type)) + " against " + describe(ts) + " from " + where() + " in " + in_.object_name,
                      "Compile with /MD-compatible 64-bit code models (no 32-bit absolute addresses), or hot-restart.");
                continue;
            }
            bool thunk_fallback = false;
            if (dk == DecisionKind::BindOldFunction && rel) {
                // Retarget directly (call or lea alike) so that pointer identity matches the old code;
                // the thunk stays for ADDR32NB and for targets that no patch base can reach.
                if (!rel32_reachable(ts.dec)) {
                    CML_LOG(Trace, "bind: %s -> %s out of rel32 reach of any patch base: through the thunk", ts.name.c_str(), hex(ts.dec.old_address.va).c_str());
                    continue;
                }
                thunk_fallback = true;
            } else if (dk == DecisionKind::BindOldData && rel && !rel32_reachable(ts.dec)) {
                error(diag::kRel32OutOfRange,
                      "REL32 reference to " + describe(ts) + " (" + hex(ts.dec.old_address.va) + ", " + ts.dec.old_address.module + ") from " + where() + " in " + in_.object_name +
                          " cannot reach it: it is more than 2 GiB away from every place the patch can be loaded",
                      "Access the data through a function or pointer in the module, or hot-restart.");
                continue;
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
            const std::size_t width = (rel || secrel || imgrel) ? 4u : 8u;
            const coff::ByteSpan data = site.sec->data;
            if (static_cast<std::uint64_t>(off) + width > data.size()) {
                error(diag::kBadRelocationSite,
                      "relocation at offset " + hex(off) + " of section '" + site.name + "' in " + in_.object_name + " lies outside the section data",
                      "Rebuild the object with the compiler; the file is damaged.");
                continue;
            }
            std::int64_t addend = 0;
            if (rel || secrel || imgrel) {
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
            f.kind = rel ? rel32_kind(type) : (secrel ? FixupKind::SecRel32 : (imgrel ? FixupKind::Addr32NbDisp : FixupKind::Addr64));
            f.target_section_offset = section_offset;
            f.thunk_fallback = thunk_fallback;
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
                    const bool match = !prefix.empty() && initializer_matches_global(fn.name, g.name);
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
                if (!found) {
                    error(diag::kUnmatchedInitialiser,
                          "dynamic initialiser '" + fn.name + "' (" + fn.dec.display + ") in " + in_.object_name + " could not be matched to a new global",
                          "Report the symbol names; as a workaround move the global with dynamic initialisation to an unchanged source file, or hot-restart.");
                    continue;
                }
                if (e.globals.empty()) {
                    CML_LOG(Trace, "bind: initialiser %s skipped: its global is bound to the old image", fn.name.c_str());
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
        // Everything collected so far, the fatal error (an unreadable PDB or object) last.
        if (all_errors != nullptr) {
            *all_errors = errors_;
            if (fatal_) {
                all_errors->push_back(*fatal_);
            }
        }
        if (partial_ != nullptr) {
            assemble();  // what was decided, for tools and evidence
            *partial_ = std::move(plan_);
        }
        return fatal_ ? *fatal_ : errors_.front();
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
    reject_new_thread_locals();
    warn_thread_local_initialisers();
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
    CML_LOG(Debug, "bind: %s", plan_.imgrel_line(in_.object_name).c_str());
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
