// WP-07 acceptance on the M1 fixtures (plan section 10), in every CI cell.
//
// For each fixture the edited TU (tests/unit/bind/edited: the fixture's edits/*.edit applied to a copy,
// compiled with the fixture's flags by this cell's compiler) is planned against the ORIGINAL fixture
// module and its PDB. The expectation files golden/fixtures/<fixture>.json are compiler-independent:
// they name source-level symbols by display name ("Square::area", "(anonymous namespace)::g_fn") and
// the decision the plan must reach, not RVAs, section numbers or the compiler's incidental symbols
// (the set of STL instantiations, /RTC1 helpers, EH handlers differs between cl and clang-cl and
// between Debug and RelWithDebInfo). The complete plan of every cell is written to
// <build>/test_work/bindplan/<fixture>.plan.json and summarised in evidence.txt.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cml/coff/binding.h"
#include "cml/coff/coff.h"
#include "cml/core/bind_plan.h"
#include "cml/pdb/index.h"
#include "doctest.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace cml::core;
using namespace cml::core::bind;

#ifndef CML_BIND_PATHS_FILE
#define CML_BIND_PATHS_FILE ""
#endif

namespace {

// ---- a tiny JSON reader for the expectation files --------------------------------------------

struct JVal {
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::string> keys;
    std::vector<JVal> vals;

    const JVal* get(const std::string& key) const {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) {
                return &vals[i];
            }
        }
        return nullptr;
    }
    std::string str(const std::string& key, const std::string& def = {}) const {
        const JVal* v = get(key);
        return v != nullptr && v->t == T::Str ? v->s : def;
    }
    long num(const std::string& key, long def = 0) const {
        const JVal* v = get(key);
        return v != nullptr && v->t == T::Num ? static_cast<long>(v->n) : def;
    }
    bool flag(const std::string& key, bool def = false) const {
        const JVal* v = get(key);
        return v != nullptr && v->t == T::Bool ? v->b : def;
    }
};

class JsonReader {
public:
    explicit JsonReader(const std::string& text) : s_(text) {}
    bool parse(JVal* out) {
        skip();
        if (!value(out)) {
            return false;
        }
        skip();
        return i_ == s_.size();
    }

private:
    void skip() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) {
            ++i_;
        }
    }
    bool lit(const char* word) {
        const std::size_t n = std::char_traits<char>::length(word);
        if (s_.compare(i_, n, word) == 0) {
            i_ += n;
            return true;
        }
        return false;
    }
    bool string(std::string* out) {
        if (i_ >= s_.size() || s_[i_] != '"') {
            return false;
        }
        ++i_;
        out->clear();
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_++];
            if (c == '\\' && i_ < s_.size()) {
                const char e = s_[i_++];
                switch (e) {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    default: c = e;
                }
            }
            out->push_back(c);
        }
        if (i_ >= s_.size()) {
            return false;
        }
        ++i_;
        return true;
    }
    bool value(JVal* v) {
        skip();
        if (i_ >= s_.size()) {
            return false;
        }
        const char c = s_[i_];
        if (c == '{') {
            ++i_;
            v->t = JVal::T::Obj;
            skip();
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                return true;
            }
            for (;;) {
                skip();
                std::string key;
                if (!string(&key)) {
                    return false;
                }
                skip();
                if (i_ >= s_.size() || s_[i_] != ':') {
                    return false;
                }
                ++i_;
                JVal child;
                if (!value(&child)) {
                    return false;
                }
                v->keys.push_back(std::move(key));
                v->vals.push_back(std::move(child));
                skip();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == '}') {
                    ++i_;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++i_;
            v->t = JVal::T::Arr;
            skip();
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                return true;
            }
            for (;;) {
                JVal child;
                if (!value(&child)) {
                    return false;
                }
                v->a.push_back(std::move(child));
                skip();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == ']') {
                    ++i_;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            v->t = JVal::T::Str;
            return string(&v->s);
        }
        if (lit("true")) {
            v->t = JVal::T::Bool;
            v->b = true;
            return true;
        }
        if (lit("false")) {
            v->t = JVal::T::Bool;
            return true;
        }
        if (lit("null")) {
            return true;
        }
        const std::size_t start = i_;
        while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '-' || s_[i_] == '.')) {
            ++i_;
        }
        if (i_ == start) {
            return false;
        }
        v->t = JVal::T::Num;
        v->n = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
        return true;
    }

    const std::string& s_;
    std::size_t i_ = 0;
};

// ---- paths and evidence ------------------------------------------------------------------------

struct FixtureEntry {
    std::string name, exe, pdb, obj, tu, edit;
};

struct Paths {
    std::string config;
    std::vector<FixtureEntry> fixtures;
};

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t e = s.find(sep, start);
        out.push_back(s.substr(start, e == std::string::npos ? std::string::npos : e - start));
        if (e == std::string::npos) {
            break;
        }
        start = e + 1;
    }
    return out;
}

const Paths& paths() {
    static const Paths p = [] {
        Paths r;
        std::ifstream in(CML_BIND_PATHS_FILE);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (line.rfind("config=", 0) == 0) {
                r.config = line.substr(7);
            } else if (!line.empty()) {
                const std::vector<std::string> f = split(line, '|');
                if (f.size() == 6) {
                    r.fixtures.push_back({f[0], f[1], f[2], f[3], f[4], f[5]});
                }
            }
        }
        return r;
    }();
    return p;
}

std::ofstream& summary_file() {
    static std::ofstream out = [] {
        std::error_code ec;
        std::filesystem::create_directories(CML_BIND_WORK_DIR, ec);
        return std::ofstream(std::string(CML_BIND_WORK_DIR) + "/summary.txt", std::ios::binary | std::ios::trunc);
    }();
    return out;
}

std::ofstream& evidence() {
    static std::ofstream out = [] {
        std::error_code ec;
        std::filesystem::create_directories(CML_BIND_WORK_DIR, ec);
        return std::ofstream(std::string(CML_BIND_WORK_DIR) + "/evidence.txt", std::ios::binary | std::ios::trunc);
    }();
    return out;
}

// Full evidence (build/test_work/bindplan/evidence.txt) and the shorter summary the CI step prints.
void line(const std::string& text) {
    evidence() << text << "\n";
    evidence().flush();
}
void brief(const std::string& text) {
    line(text);
    summary_file() << text << "\n";
    summary_file().flush();
}

std::string read_text(const std::string& path, bool* ok) {
    std::ifstream in(path, std::ios::binary);
    *ok = static_cast<bool>(in);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string hex(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string base_name(const std::string& path) {
    const std::size_t s = path.find_last_of("\\/");
    return s == std::string::npos ? path : path.substr(s + 1);
}

#ifdef CML_BIND_COMPILER_IS_CL
constexpr bool kIsCl = true;
#else
constexpr bool kIsCl = false;
#endif

// ---- other loaded modules (resolution step 5) ----------------------------------------------------

// The exports of the system DLLs this very process has loaded (ntdll exports strlen, memcpy, ...): the
// real data WP-12's ProcessModules will offer. The old module's own IAT is not needed by these fixtures.
class SystemModules final : public ProcessModules {
public:
    std::optional<SymbolAddress> iat_slot(std::string_view, std::string_view) const override { return std::nullopt; }
    std::optional<SymbolAddress> find_export(std::string_view name) const override {
#ifdef _WIN32
        if (name.empty() || name[0] == '?') {
            return std::nullopt;
        }
        const std::string n(name);
        for (const char* mod : {"ntdll.dll", "kernelbase.dll", "kernel32.dll", "ucrtbase.dll"}) {
            HMODULE h = GetModuleHandleA(mod);
            if (h == nullptr) {
                continue;
            }
            if (FARPROC p = GetProcAddress(h, n.c_str())) {
                SymbolAddress a;
                a.va = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p));
                a.is_function = true;
                a.origin = mod;
                return a;
            }
        }
#else
        (void)name;
#endif
        return std::nullopt;
    }
};

// ---- the test ---------------------------------------------------------------------------------

void emit_decisions(const std::string& tag, const BindPlan& p, const cml::pdb::PdbIndex& pdb_index) {
    for (const SymbolDecision& d : p.decisions) {
        if (d.decision == DecisionKind::BindOldData) {
            (d.defined ? brief : line)("BIND-OLD-DATA " + tag + " " + d.display + (d.defined ? " (defined" : " (undefined") + (d.is_static ? ", static)" : ")") +
                 " source=" + to_string(d.source) + (d.old_address.rva != kNoRva ? " rva=" + hex(d.old_address.rva) : " va=" + hex(d.old_address.va)));
        }
        // ADR-0010 evidence: how each STATIC of the new object was bridged to the PDB.
        if (d.defined && d.is_static && d.decision != DecisionKind::PatchFresh) {
            brief("STATIC " + tag + " obj=" + d.name + " display=" + d.display + " section=" + std::to_string(d.section) + " value=" + hex(d.value) +
                 " -> " + to_string(d.decision) + " source=" + to_string(d.source) + (d.old_address.rva != kNoRva ? " rva=" + hex(d.old_address.rva) : ""));
        }
    }
    {
        // What the PDB records for the compiland (the other side of the bridge).
        auto syms = pdb_index.compiland_symbols(p.compiland_index);
        std::size_t shown = 0;
        if (syms) {
            for (const cml::pdb::SymbolView& v : *syms) {
                if (v.kind == cml::pdb::SymbolKind::Udt || v.rva == cml::pdb::kNoRva) {
                    continue;
                }
                if (v.kind == cml::pdb::SymbolKind::LocalData || v.kind == cml::pdb::SymbolKind::LocalProc ||
                    v.kind == cml::pdb::SymbolKind::LocalThread || v.parent_proc != cml::pdb::kNoSymbol) {
                    (tag == "statics" ? brief : line)("PDB-STATIC " + tag + " kind=" + cml::pdb::to_string(v.kind) + " name='" + std::string(v.name) + "' parent='" +
                         std::string(v.parent_name) + "' rva=" + hex(v.rva));
                    if (++shown >= 80) {
                        break;
                    }
                }
            }
        }
    }

}


void plan_fixture(const std::string& name) {
    const FixtureEntry* fx = nullptr;
    for (const FixtureEntry& f : paths().fixtures) {
        if (f.name == name) {
            fx = &f;
        }
    }
    REQUIRE_MESSAGE(fx != nullptr, "fixture " << name << " is not in " << CML_BIND_PATHS_FILE);
    const std::string tag = fx->name;

    cml::pdb::OpenOptions oo;
    oo.use_cache = false;
    auto pdb = cml::pdb::PdbIndex::open_pdb(fx->pdb, oo);
    REQUIRE_MESSAGE(pdb.has_value(), (pdb.has_value() ? std::string() : pdb.error().format()));
    auto obj = cml::coff::load_object_file(fx->obj);
    REQUIRE_MESSAGE(obj.has_value(), (obj.has_value() ? std::string() : obj.error().format()));

    PlanInput in;
    in.object = &obj->object;
    in.object_name = base_name(fx->obj);
    in.module_pdb = pdb->get();
    in.module_name = base_name(fx->exe);
    in.module_base = 0x140000000ull;
    in.compiland = base_name(fx->obj);  // the compiler names the original object the same way

    SystemModules system_modules;
    in.modules = &system_modules;

    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto plan = plan_bind(in, &errors, &partial);
    brief("== " + tag + " (" + fx->edit + " on " + fx->tu + ", " + std::string(kIsCl ? "cl" : "clang-cl") + ", " + paths().config + ") ==");
    if (!plan) {
        for (const cml::Diag& d : errors) {
            brief("PLAN-ERROR " + tag + " " + d.format());
        }
        brief("BINDPLAN " + tag + " FAILED errors=" + std::to_string(errors.size()));
        emit_decisions(tag, partial, **pdb);
        FAIL("planning " << tag << " failed with " << errors.size() << " error(s); first: " << plan.error().format());
    }
    const BindPlan& p = *plan;
    emit_decisions(tag, p, **pdb);
    const std::string summary = p.summary_line(tag);
    brief(summary);
    std::printf("%s\n", summary.c_str());
    brief("PLAN-INFO " + tag + " compiland=" + p.compiland + " placeholders=" + std::to_string(p.placeholders.size()) +
         " new_functions=" + std::to_string(p.new_functions.size()) + " new_globals=" + std::to_string(p.new_globals.size()) +
         " initialisers=" + std::to_string(p.initialisers.size()) + " warnings=" + std::to_string(p.warnings.size()) +
         " decisions=" + std::to_string(p.decisions.size()));
    for (const cml::Diag& w : p.warnings) {
        brief("PLAN-WARNING " + tag + " " + w.format());
    }
    for (const HookEntry& h : p.hooks) {
        line("HOOK " + tag + " " + h.display + " source=" + to_string(h.source) + " rva=" + hex(h.old_rva) + " size=" + std::to_string(h.old_size));
    }
    for (const NewFunction& n : p.new_functions) {
        brief("NEW-FUNCTION " + tag + " " + n.display + (n.is_static ? " static" : ""));
    }
    for (const NewGlobal& n : p.new_globals) {
        std::string inits;
        for (const std::string& i : n.initialisers) {
            inits += " init=" + i;
        }
        brief("NEW-GLOBAL " + tag + " " + n.display + (n.is_static ? " static" : "") + inits);
    }
    for (const InitialiserEntry& e : p.initialisers) {
        brief("INITIALISER " + tag + " " + e.function + " table=" + e.table_section_name + "+" + std::to_string(e.table_offset) +
             (e.tu_wide ? " tu-wide" : "") + " globals=" + std::to_string(e.globals.size()));
    }
    for (const ThunkEntry& t : p.thunks) {
        line("THUNK " + tag + " " + t.display + " source=" + to_string(t.source) + (t.target.rva != kNoRva ? " rva=" + hex(t.target.rva) : " va=" + hex(t.target.va)));
    }
    // The complete plan, for the artifacts.
    {
        std::ofstream out(std::string(CML_BIND_WORK_DIR) + "/" + tag + ".plan.json", std::ios::binary | std::ios::trunc);
        out << to_json(p);
    }

    // ---- invariants every plan must satisfy --------------------------------------------------
    std::set<std::string> hooked;
    for (const HookEntry& h : p.hooks) {
        hooked.insert(h.new_symbol);
        CHECK_MESSAGE((h.old_va == in.module_base + h.old_rva || h.old_rva == kNoRva), h.new_symbol);
        const SymbolDecision* d = p.find_decision(h.new_symbol);
        REQUIRE(d != nullptr);
        CHECK(d->decision == DecisionKind::PatchHooked);
    }
    for (const SymbolDecision& d : p.decisions) {
        CHECK_MESSAGE(d.decision != DecisionKind::Unresolved, d.name);
        if (d.decision == DecisionKind::PatchHooked) {
            CHECK_MESSAGE(hooked.count(d.name) == 1, d.name);
            CHECK_MESSAGE(d.name.rfind("??__E", 0) != 0, "initialisers are never hooked: " << d.name);
        }
    }
    // The binding object accepts the plan's symbols.
    {
        const std::vector<cml::coff::BindSymbol> syms = p.binding_symbols();
        auto bytes = cml::coff::make_binding_object(syms);
        REQUIRE_MESSAGE(bytes.has_value(), (bytes.has_value() ? std::string() : bytes.error().format()));
        auto bobj = cml::coff::Object::parse(std::span<const std::byte>(bytes->data(), bytes->size()));
        CHECK(bobj.has_value());
    }
    // (rva, kind, value) generation with a synthetic layout inside rel32 reach of the module.
    {
        PatchLayout layout;
        layout.patch_base = in.module_base + 0x10000000ull;
        std::uint32_t slot = 0x100000;
        for (const Fixup& f : p.fixups) {
            if (f.kind == FixupKind::Slot) {
                if (layout.symbol_rva.emplace(f.symbol, slot).second) {
                    slot += 8;
                }
            } else {
                layout.section_rva[f.section_index] = 0x10000u * f.section_index;
            }
        }
        auto triples = make_fixup_triples(p, layout);
        CHECK_MESSAGE(triples.has_value(), (triples.has_value() ? std::string() : triples.error().format()));
        if (triples) {
            CHECK(triples->size() == p.fixups.size());
        }
    }

    // ---- the compiler-independent expectation ---------------------------------------------------
    const std::string golden_path = std::string(CML_BIND_FIXTURE_GOLDEN_DIR) + "/" + tag + ".json";
    bool ok = false;
    const std::string text = read_text(golden_path, &ok);
    REQUIRE_MESSAGE(ok, "missing expectation file " << golden_path);
    JVal g;
    JsonReader reader(text);
    REQUIRE_MESSAGE(reader.parse(&g), "cannot parse " << golden_path);
    CHECK(g.str("fixture") == tag);

    const auto decisions_for = [&](const std::string& symbol) {
        std::vector<const SymbolDecision*> out;
        for (const SymbolDecision& d : p.decisions) {
            if (d.display == symbol || d.name == symbol) {
                out.push_back(&d);
            }
        }
        return out;
    };
    // An entry names its symbol exactly ("symbol") or by a part of the display name ("symbol_contains",
    // for names that embed compiler-numbered pieces such as <lambda_0> vs <lambda_1>).
    const auto select = [&](const JVal& e) {
        const std::string contains = e.str("symbol_contains");
        if (contains.empty()) {
            return decisions_for(e.str("symbol"));
        }
        std::vector<const SymbolDecision*> out;
        for (const SymbolDecision& d : p.decisions) {
            if (d.display.find(contains) != std::string::npos) {
                out.push_back(&d);
            }
        }
        return out;
    };
    const auto describe = [&](const std::vector<const SymbolDecision*>& v) {
        std::string s;
        for (const SymbolDecision* d : v) {
            s += std::string(" [") + d->name + " -> " + to_string(d->decision) + "/" + to_string(d->source) + "]";
        }
        return s.empty() ? std::string(" (no such symbol in the plan)") : s;
    };
    const auto applies = [&](const JVal& e) {
        const std::string when = e.str("when");
        return when.empty() || (when == "cl") == kIsCl;
    };

    std::set<std::string> expected_hooks;
    if (const JVal* expect = g.get("expect")) {
        for (const JVal& e : expect->a) {
            if (!applies(e)) {
                continue;
            }
            const std::string symbol = e.str("symbol").empty() ? e.str("symbol_contains") : e.str("symbol");
            const std::string want = e.str("decision");
            const std::string source = e.str("source");
            const std::vector<const SymbolDecision*> found = select(e);
            bool ok_entry = false;
            for (const SymbolDecision* d : found) {
                if (want == to_string(d->decision) && (source.empty() || source == to_string(d->source))) {
                    ok_entry = true;
                }
            }
            CHECK_MESSAGE(ok_entry, tag << ": expected '" << symbol << "' -> " << want << (source.empty() ? "" : "/" + source) << "; plan has:" << describe(found));
            if (want == "patch-hooked") {
                for (const SymbolDecision* d : found) {
                    if (d->decision == DecisionKind::PatchHooked) {
                        expected_hooks.insert(d->display);
                    }
                }
            }
        }
    }
    if (const JVal* absent = g.get("absent")) {
        for (const JVal& e : absent->a) {
            if (!applies(e)) {
                continue;
            }
            const std::string symbol = e.str("symbol").empty() ? e.str("symbol_contains") : e.str("symbol");
            const std::string not_want = e.str("decision");
            for (const SymbolDecision* d : select(e)) {
                CHECK_MESSAGE(not_want != to_string(d->decision), tag << ": '" << symbol << "' must not be " << not_want);
            }
        }
    }
    if (g.flag("exact_hooks")) {
        std::set<std::string> actual;
        for (const HookEntry& h : p.hooks) {
            actual.insert(h.display);
        }
        CHECK_MESSAGE(actual == expected_hooks, tag << ": the hook set differs from the expectation");
    }
    CHECK_MESSAGE(static_cast<long>(p.thunks.size()) >= g.num("min_thunks"), tag << ": thunks");
    CHECK_MESSAGE(static_cast<long>(p.fixups.size()) >= g.num("min_fixups"), tag << ": fixups");
    CHECK_MESSAGE(static_cast<long>(p.summary().bind_old_data) >= g.num("min_bind_old_data"), tag << ": bind_old_data");
    CHECK_MESSAGE(static_cast<long>(p.hooks.size()) >= g.num("min_hooks"), tag << ": hooks");
    CHECK_MESSAGE(static_cast<long>(p.new_functions.size()) >= g.num("min_new_functions"), tag << ": new functions");
    CHECK_MESSAGE(static_cast<long>(p.new_globals.size()) >= g.num("min_new_globals"), tag << ": new globals");
    CHECK_MESSAGE(static_cast<long>(p.initialisers.size()) >= g.num("min_initialisers"), tag << ": initialisers");
    std::set<std::string> allowed;
    if (const JVal* aw = g.get("allowed_warnings")) {
        for (const JVal& w : aw->a) {
            allowed.insert(w.s);
        }
    }
    for (const cml::Diag& w : p.warnings) {
        CHECK_MESSAGE(allowed.count(w.id()) == 1, tag << ": unexpected warning " << w.format());
    }
}

}  // namespace

TEST_CASE("fixture basic: logic.cpp value_2") { plan_fixture("basic"); }
TEST_CASE("fixture statics: statics.cpp value_30") { plan_fixture("statics"); }
TEST_CASE("fixture virtuals: shapes.cpp square_doubled") { plan_fixture("virtuals"); }
TEST_CASE("fixture templates: a.cpp offset_100") { plan_fixture("templates"); }
TEST_CASE("fixture lambdas: lambdas.cpp lambda_times_10") { plan_fixture("lambdas"); }
TEST_CASE("fixture new_function: logic.cpp add_helper") { plan_fixture("new_function"); }
TEST_CASE("fixture new_global: logic.cpp add_tracker") { plan_fixture("new_global"); }
