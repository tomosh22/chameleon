// cml_pdb against synthetic PDBs built by pdb_builder.h: exact, compiler-independent expectations.
#include <algorithm>
#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "cml/pdb/pdb.h"
#include <ostream>  // doctest streams string_view operands; MSVC needs the full definition

#include "doctest.h"
#include "test_util.h"

using namespace cml::testpdb;
using cml::pdb::EnvSource;
using cml::pdb::OpenOptions;
using cml::pdb::PdbIndex;
using cml::pdb::SymbolKind;

namespace {

std::shared_ptr<const PdbIndex> open_bytes(const TempDir& dir, const Bytes& bytes, const std::string& name = "a.pdb",
                                           bool cache = false) {
    const std::string path = dir.file(name);
    write_file(path, bytes);
    OpenOptions opts;
    opts.use_cache = cache;
    auto r = PdbIndex::open_pdb(path, opts);
    if (!r) {
        FAIL(r.error().format());
    }
    return *r;
}

const cml::pdb::SymbolView* find_by_name(const std::vector<cml::pdb::SymbolView>& v, std::string_view name) {
    for (const auto& s : v) {
        if (s.name == name) {
            return &s;
        }
    }
    return nullptr;
}

void check_standard(const PdbIndex& idx) {
    // ---- identity ----
    CHECK(idx.id().age == 1);
    CHECK(idx.id().to_string() == "8CC3833C-8700-EB0D-4C4C-44205044422E/1");
    CHECK(idx.machine() == 0x8664);
    CHECK(idx.dbi_age() == 1);
    REQUIRE(idx.compiland_count() == 5);
    CHECK(idx.timings().open_total_ms >= 0);

    // ---- compiland table ----
    const auto& c0 = idx.compiland(0);
    CHECK(c0.object_name == "C:\\proj\\obj\\logic.cpp.obj");
    CHECK(c0.library_name.empty());
    CHECK(c0.objname_record == c0.object_name);
    CHECK(c0.file_count == 3);
    REQUIRE(c0.has_compile3);
    CHECK(c0.compile.language == 1);
    CHECK(std::string(c0.compile.language_name()) == "C++");
    CHECK(c0.compile.machine == 0xD0);
    CHECK(c0.compile.version == "Test Compiler 19.44");
    CHECK(c0.compile.frontend == std::array<std::uint16_t, 4>{19, 29, 30133, 0});
    CHECK(c0.compile.backend == std::array<std::uint16_t, 4>{14, 29, 30133, 0});
    CHECK(c0.compile.ltcg);
    CHECK(c0.compile.hotpatch);
    CHECK(c0.compile.security_checks);
    CHECK_FALSE(c0.compile.edit_and_continue);
    CHECK_FALSE(c0.compile.sdl);

    CHECK(c0.has_env_block());
    CHECK(c0.env_source == EnvSource::EnvBlock);
    REQUIRE(c0.env.size() == 5);
    CHECK(c0.env[0].first == "cwd");  // order is preserved
    CHECK(c0.env[4].first == "pdb");
    CHECK(c0.cwd().value() == "C:\\proj");
    CHECK(c0.compiler().value() == "C:\\tools\\cl.exe");
    CHECK(c0.command().value() == "/c /Zi /Od /O2 /Gy logic.cpp");
    CHECK(c0.source().value() == "C:\\proj\\src\\logic.cpp");
    CHECK(c0.pdb().value() == "C:\\proj\\obj\\vc143.pdb");
    CHECK(c0.optimization() == cml::pdb::OptimizationHint::Speed);

    const auto& c1 = idx.compiland(1);
    CHECK(c1.object_name == "C:\\proj\\obj\\main.cpp.obj");
    CHECK(c1.library_name == "C:\\libs\\mylib.lib");
    CHECK(c1.compile.version == "Test Compiler 19.44");
    CHECK_FALSE(c1.compile.ltcg);
    CHECK_FALSE(c1.has_env_block());
    CHECK_FALSE(c1.cwd().has_value());
    CHECK_FALSE(c1.command().has_value());

    const auto& c2 = idx.compiland(2);
    CHECK(c2.compile.language == 0);
    CHECK(std::string(c2.compile.language_name()) == "C");
    CHECK(c2.cwd().value() == "D:\\work\\sub");
    CHECK(c2.source().value() == "rel.cpp");
    CHECK_FALSE(c2.compiler().has_value());  // keys may be absent
    CHECK_FALSE(c2.pdb().has_value());

    const auto& c3 = idx.compiland(3);
    CHECK(c3.object_name == "Import:kernel32.dll");
    CHECK(c3.symbol_stream == 0xFFFF);
    CHECK_FALSE(c3.has_compile3);

    const auto& c4 = idx.compiland(4);
    CHECK(c4.object_name == "* Linker *");
    CHECK(c4.env_value("exe").value() == "C:\\tools\\link.exe");  // vendor keys are kept

    // ---- build_env: S_ENVBLOCK, S_BUILDINFO fallback, neither ----
    {
        auto e0 = idx.build_env(0);
        REQUIRE(e0);
        CHECK(e0->source == EnvSource::EnvBlock);
        CHECK(e0->cwd().value() == "C:\\proj");
        CHECK(e0->command().value() == "/c /Zi /Od /O2 /Gy logic.cpp");

        auto e1 = idx.build_env(1);
        REQUIRE(e1);
        CHECK(e1->source == EnvSource::BuildInfo);
        CHECK(e1->cwd().value() == "C:\\proj");
        CHECK(e1->compiler().value() == "C:\\llvm\\clang-cl.exe");
        CHECK(e1->source_file().value() == "main.cpp");
        CHECK(e1->pdb().value().empty());
        CHECK(e1->command().value() == "\"-cc1\" -c -O2");  // joined through LF_SUBSTR_LIST
        CHECK(e1->optimization() == cml::pdb::OptimizationHint::Speed);

        auto e3 = idx.build_env(3);
        REQUIRE(e3);
        CHECK(e3->source == EnvSource::None);
        CHECK(e3->pairs.empty());

        auto bad = idx.build_env(99);
        REQUIRE_FALSE(bad);
        CHECK(bad.error().code == cml::pdb::diag::kCompilandOutOfRange);
    }

    // ---- object name lookups ----
    CHECK(idx.compilands_by_object_name("logic.cpp.obj") == std::vector<std::uint32_t>{0});
    CHECK(idx.compilands_by_object_name("C:\\PROJ\\obj\\LOGIC.cpp.obj") == std::vector<std::uint32_t>{0});
    CHECK(idx.compilands_by_object_name("c:/proj/obj/main.cpp.obj") == std::vector<std::uint32_t>{1});
    CHECK(idx.compilands_by_object_stem("main") == std::vector<std::uint32_t>{1});
    CHECK(idx.compilands_by_object_stem("REL") == std::vector<std::uint32_t>{2});
    CHECK(idx.compilands_by_object_stem("nothing").empty());

    // ---- files ----
    {
        const auto raw0 = idx.compiland_files(0);
        REQUIRE(raw0.size() == 3);
        CHECK(raw0[0] == "C:\\proj\\src\\logic.cpp");
        CHECK(raw0[2] == "C:\\Program Files\\SDK\\include\\stdio.h");
        const auto norm0 = idx.compiland_files_normalized(0);
        CHECK(norm0[0] == "c:\\proj\\src\\logic.cpp");
        CHECK(norm0[2] == "c:\\program files\\sdk\\include\\stdio.h");

        const auto raw1 = idx.compiland_files(1);
        REQUIRE(raw1.size() == 3);
        CHECK(raw1[0] == "c:/proj/src/MAIN.cpp");
        const auto norm1 = idx.compiland_files_normalized(1);
        CHECK(norm1[0] == "c:\\proj\\src\\main.cpp");
        CHECK(norm1[1] == "c:\\proj\\src\\logic.h");   // ".." resolved
        CHECK(norm1[2] == "..\\include\\rel.h");        // relative and no cwd: stays relative

        const auto norm2 = idx.compiland_files_normalized(2);
        REQUIRE(norm2.size() == 2);
        CHECK(norm2[0] == "d:\\work\\include\\rel.h");  // resolved against the compiland's cwd
        CHECK(norm2[1] == "d:\\work\\sub\\rel.cpp");

        CHECK(idx.compiland_files(3).empty());
        CHECK(idx.compiland_files(1234).empty());

        CHECK(idx.compilands_for_file("C:/PROJ/src/../src/Logic.H") == std::vector<std::uint32_t>{0, 1});
        CHECK(idx.compilands_for_file("c:\\proj\\src\\main.cpp") == std::vector<std::uint32_t>{1});
        CHECK(idx.compilands_for_file("D:\\Work\\Include\\Rel.h") == std::vector<std::uint32_t>{2});
        CHECK(idx.compilands_for_file("..\\include\\rel.h") == std::vector<std::uint32_t>{1});
        CHECK(idx.compilands_for_file("c:\\nope.h").empty());
        CHECK(idx.compilands_for_file("").empty());
        CHECK(idx.distinct_file_count() == 7);
    }

    // ---- sections ----
    {
        REQUIRE(idx.sections().size() == 3);
        CHECK(std::string(idx.sections()[0].name.data()) == ".text");
        CHECK(idx.sections()[1].rva == 0x2000);
        CHECK(idx.rva_from_section_offset(1, 0x100) == 0x1100u);
        CHECK(idx.rva_from_section_offset(3, 0x10) == 0x3010u);
        CHECK_FALSE(idx.rva_from_section_offset(0, 0).has_value());
        CHECK_FALSE(idx.rva_from_section_offset(4, 0).has_value());
        const auto so = idx.section_offset_from_rva(0x3010);
        REQUIRE(so.has_value());
        CHECK(so->section == 3);
        CHECK(so->offset == 0x10);
        CHECK_FALSE(idx.section_offset_from_rva(0x5000).has_value());
        CHECK_FALSE(idx.section_offset_from_rva(0x10).has_value());
    }

    // ---- contributions ----
    {
        const auto cs = idx.contributions();
        REQUIRE(cs.size() == 4);
        CHECK(idx.contribution_count() == 4);
        CHECK(idx.contribution(0).rva == cs[0].rva);
        CHECK(cs[0].section == 1);
        CHECK(cs[0].offset == 0x100);
        CHECK(cs[0].size == 0x30);  // the entry for section 9 and the one for module 77 are dropped
        CHECK(std::is_sorted(cs.begin(), cs.end(), [](const auto& a, const auto& b) { return a.rva < b.rva; }));
        CHECK(idx.compiland_for_rva(0x1100) == 0u);
        CHECK(idx.compiland_for_rva(0x112F) == 0u);
        CHECK_FALSE(idx.compiland_for_rva(0x1130).has_value());
        CHECK(idx.compiland_for_rva(0x1230) == 1u);
        CHECK(idx.compiland_for_rva(0x1305) == 2u);
        CHECK(idx.compiland_for_rva(0x3010) == 0u);
        CHECK_FALSE(idx.compiland_for_rva(0x1050).has_value());
        CHECK(idx.compiland_for_section_offset(1, 0x205) == 1u);
        CHECK_FALSE(idx.compiland_for_section_offset(7, 0).has_value());
    }

    // ---- compiland symbols ----
    {
        auto syms = idx.compiland_symbols(0);
        REQUIRE(syms);
        REQUIRE(syms->size() == 8);
        const auto& s = *syms;
        CHECK(s[0].name == "compute_value");
        CHECK(s[0].kind == SymbolKind::GlobalProc);
        CHECK(s[0].rva == 0x1100);
        CHECK(s[0].size == 0x20);
        CHECK(s[0].section == 1);
        CHECK(s[0].offset == 0x100);
        CHECK(s[0].is_function());
        CHECK(s[0].parent_name.empty());
        CHECK(s[0].index == 0);
        CHECK(s[0].compiland == 0);
        // Function-local statics are nested in the proc scope; one even inside a nested block.
        CHECK(s[1].name == "counter");
        CHECK(s[1].kind == SymbolKind::LocalData);
        CHECK(s[1].rva == 0x3010);
        CHECK(s[1].parent_name == "compute_value");
        CHECK(s[1].parent_proc == 0);
        CHECK(s[2].name == "in_block");
        CHECK(s[2].parent_name == "compute_value");
        // After both S_END records the scope stack is empty again.
        CHECK(s[3].name == "helper");
        CHECK(s[3].kind == SymbolKind::LocalProc);
        CHECK(s[3].rva == 0x1120);
        CHECK(s[3].parent_name.empty());
        CHECK(s[4].name == "`anonymous namespace'::s_counter");
        CHECK(s[4].kind == SymbolKind::LocalData);
        CHECK(s[4].parent_name.empty());
        CHECK(s[4].rva == 0x3020);
        CHECK(s[5].name == "g_tick");
        CHECK(s[5].kind == SymbolKind::GlobalData);
        CHECK(s[6].kind == SymbolKind::LocalThread);
        CHECK(s[7].name == "Point");
        CHECK(s[7].kind == SymbolKind::Udt);
        CHECK(s[7].rva == cml::pdb::kNoRva);
        CHECK(s[7].type_index == 0x1001);

        auto rel_syms = idx.compiland_symbols(2);
        REQUIRE(rel_syms);
        REQUIRE(rel_syms->size() == 3);
        CHECK((*rel_syms)[0].name == "rel_func");
        CHECK((*rel_syms)[0].kind == SymbolKind::GlobalProc);  // S_GPROC32_ID
        CHECK((*rel_syms)[1].parent_name == "rel_func");
        CHECK((*rel_syms)[2].name == "after_proc");            // S_PROC_ID_END closed the proc
        CHECK((*rel_syms)[2].parent_name.empty());

        auto import_syms = idx.compiland_symbols(3);
        REQUIRE(import_syms);
        CHECK(import_syms->empty());

        auto oob = idx.compiland_symbols(5);
        REQUIRE_FALSE(oob);
        CHECK(oob.error().code == cml::pdb::diag::kCompilandOutOfRange);

        auto named = idx.find_in_compiland(0, "counter");
        REQUIRE(named);
        REQUIRE(named->size() == 1);
        CHECK((*named)[0].index == 1);
        auto none = idx.find_in_compiland(0, "missing");
        REQUIRE(none);
        CHECK(none->empty());
        auto dup = idx.find_in_compiland(1, "counter");
        REQUIRE(dup);
        CHECK(dup->empty());  // lookups are per compiland
    }

    // ---- publics and global data ----
    {
        auto pub = idx.find_public("?compute_value@@YAHXZ");
        REQUIRE(pub);
        REQUIRE(pub->size() == 1);
        CHECK((*pub)[0].kind == SymbolKind::Public);
        CHECK((*pub)[0].rva == 0x1100);
        CHECK((*pub)[0].is_function());
        CHECK((*pub)[0].flags == (cml::pdb::kPublicCode | cml::pdb::kPublicFunction));
        CHECK((*pub)[0].compiland == cml::pdb::kNoCompiland);
        auto data = idx.find_public("?g_tick@@3HA");
        REQUIRE(data);
        REQUIRE(data->size() == 1);
        CHECK_FALSE((*data)[0].is_function());
        CHECK((*data)[0].rva == 0x3000);
        auto nope = idx.find_public("nope");
        REQUIRE(nope);
        CHECK(nope->empty());

        auto gd = idx.find_global_data("g_tick");
        REQUIRE(gd);
        REQUIRE(gd->size() == 1);
        CHECK((*gd)[0].kind == SymbolKind::GlobalData);
        CHECK((*gd)[0].rva == 0x3000);

        auto g1 = idx.find_global("?g_tick@@3HA");  // decorated -> publics
        REQUIRE(g1);
        CHECK(g1->size() == 1);
        auto g2 = idx.find_global("g_tick");  // display name -> global data
        REQUIRE(g2);
        CHECK(g2->size() == 1);
        auto g3 = idx.find_global("main");
        REQUIRE(g3);
        CHECK(g3->size() == 1);

        CHECK(idx.public_count() == 5);
        auto all = idx.all_publics();
        REQUIRE(all);
        CHECK(all->size() == 5);
        CHECK(find_by_name(*all, "_tls_index") != nullptr);

        auto at = idx.publics_at_rva(0x1100);
        REQUIRE(at);
        std::set<std::string> names;
        for (const auto& v : *at) {
            names.insert(std::string(v.name));
        }
        CHECK(names == std::set<std::string>{"?compute_value@@YAHXZ", "compute_alias"});
        auto at2 = idx.publics_at_rva(0x1101);
        REQUIRE(at2);
        CHECK(at2->empty());

        auto cont = idx.public_containing_rva(0x1110);
        REQUIRE(cont);
        REQUIRE(cont->has_value());
        CHECK(((*cont)->name == "?compute_value@@YAHXZ" || (*cont)->name == "compute_alias"));
        auto before = idx.public_containing_rva(0x1000);
        REQUIRE(before);
        CHECK_FALSE(before->has_value());
        auto other_section = idx.public_containing_rva(0x2500);  // nearest lower public is in .text
        REQUIRE(other_section);
        CHECK_FALSE(other_section->has_value());
    }

    // ---- proc by rva ----
    {
        auto p = idx.find_proc_by_rva(0x1105);
        REQUIRE(p);
        REQUIRE(p->has_value());
        CHECK((*p)->name == "compute_value");
        CHECK((*p)->compiland == 0);
        auto h = idx.find_proc_by_rva(0x112F);
        REQUIRE(h);
        REQUIRE(h->has_value());
        CHECK((*h)->name == "helper");
        auto r = idx.find_proc_by_rva(0x1300);
        REQUIRE(r);
        REQUIRE(r->has_value());
        CHECK((*r)->name == "rel_func");
        CHECK((*r)->compiland == 2);
        auto gap = idx.find_proc_by_rva(0x1150);
        REQUIRE(gap);
        CHECK_FALSE(gap->has_value());
        auto data_rva = idx.find_proc_by_rva(0x3010);
        REQUIRE(data_rva);
        CHECK_FALSE(data_rva->has_value());
    }

    REQUIRE(idx.load_all());
    CHECK(idx.timings().load_all_wall_ms >= 0);
}

}  // namespace

TEST_CASE("synthetic PDB: contiguous streams") {
    TempDir dir;
    Spec spec = standard_spec();
    spec.fragmented = false;
    const auto idx = open_bytes(dir, build(spec));
    check_standard(*idx);
}

TEST_CASE("synthetic PDB: fragmented streams (interleaved blocks) give the same answers") {
    TempDir dir;
    Spec spec = standard_spec();
    spec.fragmented = true;
    const auto idx = open_bytes(dir, build(spec));
    check_standard(*idx);
}

TEST_CASE("synthetic PDB: other block sizes") {
    for (std::uint32_t bs : {512u, 1024u, 8192u}) {
        TempDir dir;
        Spec spec = standard_spec();
        spec.block_size = bs;
        spec.fragmented = (bs == 512);
        const auto idx = open_bytes(dir, build(spec));
        check_standard(*idx);
    }
}

TEST_CASE("synthetic PDB: find_proc_by_rva falls back to a full search without section contributions") {
    TempDir dir;
    Spec spec = standard_spec();
    spec.contributions.clear();
    const auto idx = open_bytes(dir, build(spec));
    CHECK(idx->contribution_count() == 0);
    CHECK_FALSE(idx->compiland_for_rva(0x1100).has_value());
    auto p = idx->find_proc_by_rva(0x1105);
    REQUIRE(p);
    REQUIRE(p->has_value());
    CHECK((*p)->name == "compute_value");
    auto r = idx->find_proc_by_rva(0x1300);
    REQUIRE(r);
    REQUIRE(r->has_value());
    CHECK((*r)->name == "rel_func");
}

TEST_CASE("synthetic PDB: without a section header stream RVAs are unavailable but nothing fails") {
    TempDir dir;
    Spec spec = standard_spec();
    spec.omit_section_stream = true;
    const auto idx = open_bytes(dir, build(spec));
    CHECK_FALSE(idx->has_sections());
    CHECK(idx->contribution_count() == 0);
    auto syms = idx->compiland_symbols(0);
    REQUIRE(syms);
    CHECK((*syms)[0].rva == cml::pdb::kNoRva);
    CHECK((*syms)[0].section == 1);
    CHECK((*syms)[0].offset == 0x100);
    auto pub = idx->find_public("main");
    REQUIRE(pub);
    REQUIRE(pub->size() == 1);
    CHECK((*pub)[0].rva == cml::pdb::kNoRva);
}

TEST_CASE("synthetic PDB: empty PDB (no modules) opens") {
    TempDir dir;
    Spec spec;
    const auto idx = open_bytes(dir, build(spec));
    CHECK(idx->compiland_count() == 0);
    CHECK(idx->distinct_file_count() == 0);
    CHECK(idx->public_count() == 0);
    auto pub = idx->find_public("x");
    REQUIRE(pub);
    CHECK(pub->empty());
    auto proc = idx->find_proc_by_rva(0x1000);
    REQUIRE(proc);
    CHECK_FALSE(proc->has_value());
    CHECK(idx->load_all());
}

TEST_CASE("synthetic PDB: preload options build the tables during open") {
    TempDir dir;
    const std::string path = dir.file("p.pdb");
    write_file(path, build(standard_spec()));
    OpenOptions opts;
    opts.use_cache = false;
    opts.preload_all_symbols = true;
    auto r = PdbIndex::open_pdb(path, opts);
    REQUIRE(r);
    const auto t = (*r)->timings();
    CHECK(t.publics_ms >= 0);
    CHECK(t.module_symbols_ms >= 0);
    CHECK((*r)->public_count() == 5);
}

TEST_CASE("cache: one shared index per (GUID, age)") {
    TempDir dir;
    Spec a = standard_spec();
    write_file(dir.file("one.pdb"), build(a));
    write_file(dir.file("copy.pdb"), build(a));  // same identity, different file
    Spec b = standard_spec();
    b.age = 2;
    write_file(dir.file("age2.pdb"), build(b));
    Spec c = standard_spec();
    c.guid[3] ^= 0x55;
    write_file(dir.file("guid2.pdb"), build(c));

    const std::size_t before = PdbIndex::cache_size();
    auto r1 = PdbIndex::open_pdb(dir.file("one.pdb"));
    REQUIRE(r1);
    auto r2 = PdbIndex::open_pdb(dir.file("copy.pdb"));
    REQUIRE(r2);
    CHECK(r1->get() == r2->get());  // shared
    auto r3 = PdbIndex::open_pdb(dir.file("age2.pdb"));
    REQUIRE(r3);
    CHECK(r3->get() != r1->get());
    CHECK((*r3)->id().age == 2);
    auto r4 = PdbIndex::open_pdb(dir.file("guid2.pdb"));
    REQUIRE(r4);
    CHECK(r4->get() != r1->get());
    CHECK(PdbIndex::cache_size() == before + 3);

    OpenOptions no_cache;
    no_cache.use_cache = false;
    auto r5 = PdbIndex::open_pdb(dir.file("one.pdb"), no_cache);
    REQUIRE(r5);
    CHECK(r5->get() != r1->get());

    // The cache does not keep indexes alive: once every user lets go, the next open rebuilds.
    r3 = PdbIndex::open_pdb(dir.file("one.pdb"));  // drops the age-2 index
    REQUIRE(r3);
    CHECK(PdbIndex::cache_size() == before + 2);
    auto r6 = PdbIndex::open_pdb(dir.file("age2.pdb"));
    REQUIRE(r6);
    CHECK((*r6)->id().age == 2);
    CHECK(PdbIndex::cache_size() == before + 3);

    PdbIndex::cache_clear();
    CHECK(PdbIndex::cache_size() == 0);
    auto r7 = PdbIndex::open_pdb(dir.file("one.pdb"));
    REQUIRE(r7);
    CHECK(r7->get() != r1->get());  // forgotten entry: a new index is built, the old one stays valid
    CHECK((*r1)->compiland_count() == 5);
}

TEST_CASE("cache and lazy loading are thread-safe") {
    TempDir dir;
    const std::string path = dir.file("t.pdb");
    write_file(path, build(standard_spec()));
    PdbIndex::cache_clear();

    constexpr int kThreads = 8;
    std::vector<std::shared_ptr<const PdbIndex>> got(kThreads);
    std::atomic<int> failures{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            while (!go.load()) {
                std::this_thread::yield();
            }
            auto r = PdbIndex::open_pdb(path);
            if (!r) {
                ++failures;
                return;
            }
            got[static_cast<std::size_t>(t)] = *r;
            const PdbIndex& idx = **r;
            for (int round = 0; round < 20; ++round) {
                auto p = idx.find_public("?compute_value@@YAHXZ");
                auto s = idx.find_in_compiland(static_cast<std::uint32_t>(round % 3), "counter");
                auto f = idx.find_proc_by_rva(0x1105);
                auto e = idx.build_env(1);
                if (!p || !s || !f || !e || p->size() != 1 || !f->has_value() || e->source != EnvSource::BuildInfo) {
                    ++failures;
                }
            }
            if (!idx.load_all(2)) {
                ++failures;
            }
        });
    }
    go = true;
    for (auto& t : threads) {
        t.join();
    }
    CHECK(failures.load() == 0);
    for (int t = 1; t < kThreads; ++t) {
        CHECK(got[static_cast<std::size_t>(t)].get() == got[0].get());  // one shared index
    }
}

TEST_CASE("memory accounting: tracked and mapped bytes return to the baseline") {
    TempDir dir;
    const std::string path = dir.file("m.pdb");
    const Bytes image = build(standard_spec());
    write_file(path, image);
    PdbIndex::cache_clear();
    const auto before = cml::pdb::memory_stats();
    {
        cml::pdb::reset_memory_peaks();
        OpenOptions opts;
        opts.use_cache = false;
        auto r = PdbIndex::open_pdb(path, opts);
        REQUIRE(r);
        REQUIRE((*r)->load_all());
        REQUIRE((*r)->load_publics());
        const auto during = cml::pdb::memory_stats();
        CHECK(during.mapped_bytes == before.mapped_bytes + image.size());
        CHECK(during.tracked_bytes > before.tracked_bytes);
        CHECK(during.tracked_peak_bytes >= during.tracked_bytes);
        CHECK(during.peak_total() >= during.tracked_bytes + during.mapped_bytes);
    }
    const auto after = cml::pdb::memory_stats();
    CHECK(after.tracked_bytes == before.tracked_bytes);
    CHECK(after.mapped_bytes == before.mapped_bytes);
}
