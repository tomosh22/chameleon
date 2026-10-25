// cml_pdb against the PDB of fixture `basic`, built by whichever compiler this CI cell uses
// (cl or clang-cl). Paths come from compile definitions set in tests/unit/CMakeLists.txt.
//
// The assertions are the facts both compilers agree on. Where they differ (S_ENVBLOCK versus
// S_BUILDINFO, display names of statics and initialisers) the test checks structure and the CI log
// of `cml_pdb_dump` records the actual names (see docs/wp/WP-04.md).
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "cml/pdb/pdb.h"
#include <ostream>  // doctest streams string_view operands; MSVC needs the full definition

#include "doctest.h"

#if defined(CML_FIXTURE_BASIC_PDB) && defined(CML_FIXTURE_BASIC_EXE) && defined(CML_FIXTURE_BASIC_SRC)

using cml::pdb::PdbIndex;
using cml::pdb::SymbolKind;

namespace {

std::shared_ptr<const PdbIndex> open_fixture() {
    auto r = PdbIndex::open_pdb(CML_FIXTURE_BASIC_PDB);
    if (!r) {
        FAIL(r.error().format());
    }
    return *r;
}

std::uint32_t only(const std::vector<std::uint32_t>& v, const char* what) {
    INFO(what);
    REQUIRE(v.size() == 1);
    return v[0];
}

std::string src(const char* file) {
    return std::string(CML_FIXTURE_BASIC_SRC) + "/" + file;
}

bool contains(const std::vector<std::uint32_t>& v, std::uint32_t x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

bool has_symbol(const std::vector<cml::pdb::SymbolView>& syms, const char* substr, SymbolKind kind) {
    for (const auto& s : syms) {
        if (s.kind == kind && s.name.find(substr) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("fixture basic: identity, compilands and compiler records") {
    const auto idx = open_fixture();
    CHECK(idx->machine() == 0x8664);
    CHECK_FALSE(idx->id().is_zero());
    CHECK(idx->file_size() > 0);
    CHECK(idx->compiland_count() >= 3);

    const std::uint32_t main_c = only(idx->compilands_by_object_stem("main"), "main compiland");
    const std::uint32_t logic_c = only(idx->compilands_by_object_stem("logic"), "logic compiland");
    const std::uint32_t extras_c = only(idx->compilands_by_object_stem("extras"), "extras compiland");
    CHECK(main_c != logic_c);
    CHECK(logic_c != extras_c);

    for (std::uint32_t c : {main_c, logic_c, extras_c}) {
        const auto& comp = idx->compiland(c);
        INFO(std::string(comp.object_name));
        CHECK_FALSE(comp.object_name.empty());
        CHECK(comp.library_name.empty());  // plain objects, not archive members
        REQUIRE(comp.has_compile3);
        CHECK(std::string(comp.compile.language_name()) == "C++");
        CHECK(comp.compile.machine == 0xD0);  // CV_CFL_X64
        CHECK_FALSE(comp.compile.version.empty());
        CHECK_FALSE(comp.compile.ltcg);  // the fixture is not built with /GL
        CHECK(comp.file_count >= 1);

        // Build environment: S_ENVBLOCK (cl) or S_BUILDINFO (clang-cl). Whatever is recorded must
        // be well formed.
        auto env = idx->build_env(c);
        REQUIRE(env);
        if (env->source != cml::pdb::EnvSource::None) {
            REQUIRE(env->cwd().has_value());
            CHECK_FALSE(env->cwd()->empty());
        }
    }

    // The linker module and the CRT/import modules are listed too.
    bool linker = false;
    for (std::uint32_t i = 0; i < idx->compiland_count(); ++i) {
        linker = linker || idx->compiland(i).object_name == "* Linker *";
    }
    CHECK(linker);
}

TEST_CASE("fixture basic: opening from the PE debug directory finds and validates the PDB") {
    const auto by_path = open_fixture();
    auto by_pe = PdbIndex::open_for_pe(CML_FIXTURE_BASIC_EXE);
    REQUIRE(by_pe);
    CHECK(by_pe->get() == by_path.get());  // same (GUID, age): shared through the cache
    CHECK(by_pe->get()->id() == by_path->id());

    auto ref = cml::pdb::read_codeview_ref(CML_FIXTURE_BASIC_EXE);
    REQUIRE(ref);
    CHECK(ref->id == by_path->id());
    CHECK_FALSE(ref->pdb_path.empty());
}

TEST_CASE("fixture basic: file lists and the inverted file map") {
    const auto idx = open_fixture();
    const std::uint32_t main_c = only(idx->compilands_by_object_stem("main"), "main");
    const std::uint32_t logic_c = only(idx->compilands_by_object_stem("logic"), "logic");
    const std::uint32_t extras_c = only(idx->compilands_by_object_stem("extras"), "extras");

    CHECK(idx->compilands_for_file(src("logic.cpp")) == std::vector<std::uint32_t>{logic_c});
    CHECK(idx->compilands_for_file(src("main.cpp")) == std::vector<std::uint32_t>{main_c});
    CHECK(idx->compilands_for_file(src("extras.cpp")) == std::vector<std::uint32_t>{extras_c});

    CHECK(idx->distinct_file_count() >= 3);
    for (std::uint32_t c : {main_c, logic_c, extras_c}) {
        CHECK(idx->compiland_files(c).size() >= 1);
        CHECK(idx->compiland_files(c).size() == idx->compiland_files_normalized(c).size());
    }
    CHECK(idx->compilands_for_file(src("nothing.h")).empty());

    // Spelling does not matter: upper case, backslashes, a redundant "..".
    std::string odd = src("logic.cpp");
    std::transform(odd.begin(), odd.end(), odd.begin(), [](unsigned char c) { return static_cast<char>(c == '/' ? '\\' : std::toupper(c)); });
    CHECK(idx->compilands_for_file(odd) == std::vector<std::uint32_t>{logic_c});
    CHECK(idx->compilands_for_file(src("../basic/logic.cpp")) == std::vector<std::uint32_t>{logic_c});

#if defined(CML_TEST_COMPILER_IS_CL)
    // MSVC lists every file it opened, headers included, so the dependency graph is complete:
    // logic.h is included by logic.cpp and main.cpp; extras.h by extras.cpp and main.cpp.
    const auto logic_h = idx->compilands_for_file(src("logic.h"));
    CHECK(contains(logic_h, logic_c));
    CHECK(contains(logic_h, main_c));
    CHECK_FALSE(contains(logic_h, extras_c));
    const auto extras_h = idx->compilands_for_file(src("extras.h"));
    CHECK(contains(extras_h, extras_c));
    CHECK(contains(extras_h, main_c));
    CHECK_FALSE(contains(extras_h, logic_c));
    CHECK(idx->compilands_for_file(src("../basic/LOGIC.H")) == logic_h);
    // The system headers pulled in by windows.h are part of the graph as well.
    CHECK(idx->distinct_file_count() > 5);
    CHECK(idx->compiland_files(extras_c).size() > 3);
#else
    // clang-cl lists only files that carry line information (see docs/wp/WP-04.md): the headers of
    // this fixture are declaration-only, so they may be absent. Nothing is asserted about them
    // beyond the .cpp files above; the cml_pdb_dump log of the CI cell shows what was recorded.
    (void)contains;
#endif
}

TEST_CASE("fixture basic: symbols, sections, RVAs") {
    const auto idx = open_fixture();
    const std::uint32_t main_c = only(idx->compilands_by_object_stem("main"), "main");
    const std::uint32_t logic_c = only(idx->compilands_by_object_stem("logic"), "logic");
    const std::uint32_t extras_c = only(idx->compilands_by_object_stem("extras"), "extras");

    REQUIRE(idx->has_sections());
    bool text = false;
    for (const auto& s : idx->sections()) {
        text = text || std::string(s.name.data()) == ".text";
    }
    CHECK(text);
    CHECK(idx->contribution_count() > 0);

    // compute_value lives in logic.cpp: a proc, a public with its decorated name, same RVA.
    auto cv = idx->find_in_compiland(logic_c, "compute_value");
    REQUIRE(cv);
    REQUIRE(cv->size() == 1);
    const auto& proc = (*cv)[0];
    CHECK(cml::pdb::is_proc_kind(proc.kind));
    CHECK(proc.size > 0);
    REQUIRE(proc.rva != cml::pdb::kNoRva);
    CHECK(idx->rva_from_section_offset(proc.section, proc.offset) == proc.rva);
    const auto so = idx->section_offset_from_rva(proc.rva);
    REQUIRE(so.has_value());
    CHECK(so->section == proc.section);
    CHECK(so->offset == proc.offset);

    auto pub = idx->find_public("?compute_value@@YAHXZ");
    REQUIRE(pub);
    REQUIRE(pub->size() == 1);
    CHECK((*pub)[0].rva == proc.rva);
    CHECK((*pub)[0].is_function());

    auto at = idx->publics_at_rva(proc.rva);
    REQUIRE(at);
    bool found = false;
    for (const auto& p : *at) {
        found = found || p.name == "?compute_value@@YAHXZ";
    }
    CHECK(found);

    CHECK(idx->compiland_for_rva(proc.rva) == logic_c);
    CHECK(idx->compiland_for_section_offset(proc.section, proc.offset) == logic_c);
    auto containing = idx->find_proc_by_rva(proc.rva + proc.size - 1);
    REQUIRE(containing);
    REQUIRE(containing->has_value());
    CHECK((*containing)->name == "compute_value");
    CHECK((*containing)->compiland == logic_c);
    auto pc = idx->public_containing_rva(proc.rva + 1);
    REQUIRE(pc);
    REQUIRE(pc->has_value());
    CHECK((*pc)->name == "?compute_value@@YAHXZ");

    // main.cpp defines the globals g_tick and fixture_note.
    // g_tick: global data. Its S_GDATA32 is in the global symbol stream (and, with link.exe, also
    // in main.obj's module stream), so the global lookup is the portable one.
    auto gtick = idx->find_global_data("g_tick");
    REQUIRE(gtick);
    REQUIRE(gtick->size() >= 1);
    CHECK((*gtick)[0].kind == SymbolKind::GlobalData);
    auto note = idx->find_in_compiland(main_c, "fixture_note");
    REQUIRE(note);
    REQUIRE(note->size() >= 1);
    CHECK((*note)[0].kind == SymbolKind::GlobalProc);

    auto g1 = idx->find_global("?g_tick@@3HA");
    REQUIRE(g1);
    CHECK_FALSE(g1->empty());
    auto g2 = idx->find_global("g_tick");
    REQUIRE(g2);
    CHECK_FALSE(g2->empty());
    CHECK((*g1)[0].rva == (*gtick)[0].rva);
    auto g3 = idx->find_public("?fixture_note@@YAXPEBD@Z");
    REQUIRE(g3);
    CHECK(g3->size() == 1);
    CHECK(idx->compiland_for_rva((*g1)[0].rva) == main_c);  // the variable's bytes came from main.obj

    // extras.cpp: static data of an anonymous namespace, the dynamic initialiser, a template.
    // (s_counter itself may be folded into a constant by an optimising compiler, s_dynamic cannot.)
    auto extras = idx->compiland_symbols(extras_c);
    REQUIRE(extras);
    CHECK(has_symbol(*extras, "s_dynamic", SymbolKind::LocalData));
    CHECK(has_symbol(*extras, "add_one", SymbolKind::GlobalProc));
    CHECK(has_symbol(*extras, "extras_value", SymbolKind::GlobalProc));
    for (const auto& s : *extras) {
        if (s.kind == SymbolKind::LocalData) {
            // File-local data is attributed to the compiland that defined it through the section
            // contributions; it has no S_PUB32 to find it by (WP-07 bridges this by display name).
            REQUIRE(s.rva != cml::pdb::kNoRva);
            CHECK(idx->compiland_for_rva(s.rva) == extras_c);
        }
    }
}

TEST_CASE("fixture basic: full load and concurrent queries") {
    const auto idx = open_fixture();
    REQUIRE(idx->load_all());
    REQUIRE(idx->load_publics());
    CHECK(idx->public_count() > 5);
    for (std::uint32_t i = 0; i < idx->compiland_count(); ++i) {
        REQUIRE(idx->compiland_symbols(i));
    }
    const auto t = idx->timings();
    CHECK(t.open_total_ms >= 0);
    CHECK(t.module_symbols_ms >= 0);
}

#else

TEST_CASE("fixture basic: not configured") {
    // Built without the fixture paths (portable syntax-check configuration).
    CHECK(true);
}

#endif
