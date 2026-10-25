// WP-05 acceptance (2): recompile every translation unit of the fixtures `basic` and `pch` from the
// command recorded in their PDB, and compare the new object's symbol set with the original
// compiland's object. Runs under whichever compiler built the cell (cl or clang-cl), in Debug and
// RelWithDebInfo. Windows only: it runs the real compiler.
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cml/pdb/pdb.h"
#include "cml/pe/pe.h"
#include "cml/toolchain/toolchain.h"
#include "cml/util/hash.h"
#include "doctest.h"
#include "fixture_paths.h"

#ifndef CML_TEST_WORK_DIR
#define CML_TEST_WORK_DIR "."
#endif
#ifndef CML_TEST_CXX_COMPILER
#define CML_TEST_CXX_COMPILER ""
#endif

using namespace cml::toolchain;
using cml_test::FixturePaths;

namespace {

namespace fs = std::filesystem;

bool is_cl_cell() {
#ifdef CML_TEST_COMPILER_IS_CL
    return true;
#else
    return false;
#endif
}

std::string stem_of(std::string_view object_name) {
    std::string s(object_name.substr(object_name.find_last_of("\\/") == std::string_view::npos ? 0 : object_name.find_last_of("\\/") + 1));
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* ext : {".cpp.obj", ".cxx.obj", ".cc.obj", ".c.obj", ".obj"}) {
        const std::string e = ext;
        if (lower.size() > e.size() && lower.compare(lower.size() - e.size(), e.size(), e) == 0) {
            return s.substr(0, s.size() - e.size());
        }
    }
    return s;
}

std::string find_object(const std::string& list_key, const std::string& stem) {
    for (const char* suffix : {".cpp.obj", ".obj", ".cxx.obj", ".cc.obj", ".c.obj"}) {
        const std::string p = FixturePaths::get().object(list_key, stem + suffix);
        if (!p.empty()) {
            return p;
        }
    }
    return {};
}

std::vector<std::byte> read_bytes(const std::string& path) {
    std::ifstream in(fs::path(path), std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(static_cast<unsigned char>(s[i]));
    return out;
}

struct SourceCompiland {
    std::uint32_t index;
    std::string stem;
    std::string object_name;
    cml::pdb::BuildEnv env;
    cml::pdb::CompileInfo compile3;
    bool has_compile3;
};

std::vector<SourceCompiland> source_compilands(const cml::pdb::PdbIndex& idx) {
    std::vector<SourceCompiland> out;
    for (std::uint32_t ci = 0; ci < idx.compiland_count(); ++ci) {
        const cml::pdb::Compiland& c = idx.compiland(ci);
        if (!c.library_name.empty()) continue;
        auto env = idx.build_env(ci);
        if (!env) continue;
        const auto src = env->source_file();
        if (!src || src->empty()) continue;
        out.push_back({ci, stem_of(c.object_name), std::string(c.object_name), *env, c.compile, c.has_compile3});
    }
    return out;
}

std::string work_dir(const std::string& leaf) {
    const fs::path p = fs::path(CML_TEST_WORK_DIR) / "toolchain" / leaf;
    std::error_code ec;
    fs::create_directories(p, ec);
    return p.string();
}

std::vector<std::string> fallback_dirs() {
    std::vector<std::string> dirs;
    const fs::path cxx = fs::path(CML_TEST_CXX_COMPILER);
    if (cxx.has_parent_path()) {
        dirs.push_back(cxx.parent_path().string());
    }
    return dirs;
}

struct Fixture {
    const char* name;
    const char* objs_key;
    const char* pdb_key;
    const char* exe_key;
};
constexpr Fixture kFixtures[] = {{"basic", "basic_objs", "basic_pdb", "basic_exe"}, {"pch", "pch_objs", "pch_pdb", "pch_exe"}};

void check_recompile(const Fixture& fx, PchDebugMode mode, bool expect_all_ok, std::string* evidence) {
    const FixturePaths& paths = FixturePaths::get();
    REQUIRE(paths.loaded());
    const std::string pdb_path = paths.value(fx.pdb_key);
    REQUIRE_FALSE(pdb_path.empty());
    auto idx = cml::pdb::PdbIndex::open_pdb(pdb_path);
    REQUIRE_MESSAGE(idx, idx.error().format());
    const auto compilands = source_compilands(**idx);
    REQUIRE_MESSAGE(compilands.size() >= 2, "fixture " << fx.name << " has " << compilands.size() << " source compilands");

    for (const SourceCompiland& c : compilands) {
        const std::string original = find_object(fx.objs_key, c.stem);
        INFO("fixture " << fx.name << " compiland " << c.stem << " (" << c.object_name << ")");
        if (original.empty()) {
            continue;  // objects that are not fixture sources (none expected)
        }
        auto rec = record_from_build_env(c.env);
        REQUIRE_MESSAGE(rec, rec.error().format());
        CompileRequest req;
        req.record = *rec;
        req.output_object = patch_object_path(work_dir(fx.name), "fixture", cml::hash64(c.object_name), rec->source);
        req.pch_debug = mode;
        req.fallback_search_dirs = fallback_dirs();
        CompileOutcome out;
        cml::Result<void> r = compile_tu(req, out);
        char line[512];
        std::snprintf(line, sizeof(line), "PCH-STRATEGY fixture=%s mode=%d stem=%s result=%s exit=%d seconds=%.2f", fx.name,
                      static_cast<int>(mode), c.stem.c_str(), r ? "ok" : r.error().id().c_str(), out.exit_code, out.seconds);
        std::printf("%s\n", line);
        if (evidence) {
            *evidence += std::string(line) + "\n";
        }

        const bool creates_pch = rec->args.size() > 0 && [&] {
            for (const std::string& t : rec->args) {
                if (t.rfind("-Yc", 0) == 0 || t.rfind("/Yc", 0) == 0 || t == "-building-pch-with-obj" || t == "-emit-pch") return true;
            }
            return false;
        }();
        if (creates_pch) {
            REQUIRE_FALSE(r);
            CHECK(r.error().code == diag::kPchCreatingTu);
            continue;
        }
        if (!r) {
            std::printf("%s\n%s\n", r.error().format().c_str(), out.output.c_str());
            if (!expect_all_ok) {
                continue;
            }
        }
        REQUIRE_MESSAGE(r, r.error().format() << "\ncommand: " << out.command_line << "\n" << out.output);
        CHECK(out.compiler_fallback_used == false);

        auto before = symbol_set_of_file(original);
        auto after = symbol_set_of_file(out.object_path);
        REQUIRE_MESSAGE(before, before.error().format());
        REQUIRE_MESSAGE(after, after.error().format());
        const SymbolSetDiff d = diff_symbol_sets(*before, *after);
        CHECK_MESSAGE(d.empty(), "symbol sets differ:\n" << d.to_string() << "command: " << out.command_line);
        CHECK(before->size() > 0);

        // The new object still carries CodeView.
        auto obj = cml::coff::load_object_file(out.object_path);
        REQUIRE(obj);
        CHECK(obj->object.find_section(".debug$S") != nullptr);
    }
}

}  // namespace

TEST_CASE("fixture basic: every TU recompiled from its PDB record has the original symbol set") {
    check_recompile(kFixtures[0], PchDebugMode::Auto, true, nullptr);
}

TEST_CASE("fixture pch: /Yu TUs recompile with the default strategy; the /Yc TU is rejected with CML2102") {
    check_recompile(kFixtures[1], PchDebugMode::Auto, true, nullptr);
}

TEST_CASE("fixture pch: evidence for every PCH strategy (only the default one is asserted)") {
    std::string evidence;
    int working = 0;
    for (const PchDebugMode mode : {PchDebugMode::ForceZ7, PchDebugMode::KeepZiPrivatePdb, PchDebugMode::DropPch}) {
        const std::size_t before = evidence.size();
        check_recompile(kFixtures[1], mode, false, &evidence);
        const std::string section = evidence.substr(before);
        // A strategy works when no line of it reports an error id.
        if (section.find("result=CML") == std::string::npos) {
            ++working;
        }
    }
    std::printf("PCH-STRATEGY summary: %d of 3 non-default strategies work in this cell\n", working);
    // The default is one of them (it was chosen from this evidence), so at least one works.
    CHECK(working >= 1);
}

TEST_CASE("fixture basic: the compiler recorded in the PDB is replaced by the installed one when it is gone") {
    const FixturePaths& paths = FixturePaths::get();
    REQUIRE(paths.loaded());
    auto idx = cml::pdb::PdbIndex::open_pdb(paths.value("basic_pdb"));
    REQUIRE(idx);
    for (const SourceCompiland& c : source_compilands(**idx)) {
        if (c.stem != "logic") continue;
        auto rec = record_from_build_env(c.env);
        REQUIRE(rec);
        rec->compiler = R"(C:\this\toolchain\was\removed\)" + std::string(is_cl_cell() ? "cl.exe" : "clang-cl.exe");
        CompileRequest req;
        req.record = *rec;
        req.output_object = patch_object_path(work_dir("fallback"), "fixture", 1, rec->source);
        req.fallback_search_dirs = fallback_dirs();
        CompileOutcome out;
        auto r = compile_tu(req, out);
        REQUIRE_MESSAGE(r, r.error().format() << "\n" << out.output);
        CHECK(out.compiler_fallback_used);
        bool warned = false;
        for (const cml::Diag& w : out.warnings) warned = warned || w.code == diag::kCompilerFallback;
        CHECK(warned);
        // And with fallback disabled it is an error.
        req.allow_compiler_fallback = false;
        auto strict = compile_tu(req, out);
        REQUIRE_FALSE(strict);
        CHECK(strict.error().code == diag::kCannotStartCompiler);
    }
}

TEST_CASE("fixture basic: a command line longer than the limit goes through a response file and still compiles") {
    const FixturePaths& paths = FixturePaths::get();
    REQUIRE(paths.loaded());
    auto idx = cml::pdb::PdbIndex::open_pdb(paths.value("basic_pdb"));
    REQUIRE(idx);
    for (const SourceCompiland& c : source_compilands(**idx)) {
        if (c.stem != "logic") continue;
        auto rec = record_from_build_env(c.env);
        REQUIRE(rec);
        CompileRequest req;
        req.record = *rec;
        req.output_object = patch_object_path(work_dir("rsp"), "fixture", 2, rec->source);
        req.force_response_file = true;
        req.fallback_search_dirs = fallback_dirs();
        CompileOutcome out;
        auto r = compile_tu(req, out);
        REQUIRE_MESSAGE(r, r.error().format() << "\ncommand: " << out.command_line << "\n" << out.output);
        auto before = symbol_set_of_file(find_object("basic_objs", "logic"));
        auto after = symbol_set_of_file(out.object_path);
        REQUIRE(before);
        REQUIRE(after);
        CHECK(diff_symbol_sets(*before, *after).empty());
    }
}

TEST_CASE("fixture basic: compile errors come back as clickable file(line) diagnostics with CML2200") {
    const FixturePaths& paths = FixturePaths::get();
    REQUIRE(paths.loaded());
    auto idx = cml::pdb::PdbIndex::open_pdb(paths.value("basic_pdb"));
    REQUIRE(idx);
    const fs::path broken = fs::path(work_dir("broken")) / "broken.cpp";
    {
        std::ofstream f(broken);
        f << "int broken() {\n  return undeclared_identifier_xyz;\n}\n";
    }
    for (const SourceCompiland& c : source_compilands(**idx)) {
        if (c.stem != "logic") continue;
        auto rec = record_from_build_env(c.env);
        REQUIRE(rec);
        rec->source = broken.string();
        // The recorded command names no source (S_BUILDINFO keeps it separately); if it does, point it at the broken file.
        for (std::string& t : rec->args) {
            if (t.size() > 9 && t.substr(t.size() - 9) == "logic.cpp") t = broken.string();
        }
        CompileRequest req;
        req.record = *rec;
        req.output_object = patch_object_path(work_dir("broken"), "fixture", 3, rec->source);
        req.fallback_search_dirs = fallback_dirs();
        CompileOutcome out;
        auto r = compile_tu(req, out);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == diag::kCompileFailed);
        CHECK(out.exit_code != 0);
        REQUIRE_FALSE(out.diagnostics.empty());
        bool clickable = false;
        for (const ToolDiagnostic& d : out.diagnostics) {
            if (d.severity == Severity::Error && d.line == 2 && d.file.find("broken.cpp") != std::string::npos) clickable = true;
        }
        CHECK_MESSAGE(clickable, out.output);
        CHECK(out.output.find("broken.cpp(2") != std::string::npos);
    }
}

TEST_CASE("fixture basic: build-flag facts (/Gy, /hotpatch, /GL, /guard:cf) with their evidence") {
    const FixturePaths& paths = FixturePaths::get();
    REQUIRE(paths.loaded());
    auto idx = cml::pdb::PdbIndex::open_pdb(paths.value("basic_pdb"));
    REQUIRE(idx);
    int checked = 0;
    for (const SourceCompiland& c : source_compilands(**idx)) {
        auto rec = record_from_build_env(c.env);
        REQUIRE(rec);
        const std::string original = find_object("basic_objs", c.stem);
        if (original.empty()) continue;
        const std::vector<std::byte> bytes = read_bytes(original);
        RewriteOptions ro;
        ro.output_object = "C:\\x.obj";
        auto rw = rewrite_for_patch(*rec, ro);
        REQUIRE(rw);
        CompilandFlagInputs in;
        in.args = rw->expanded_args;
        in.dialect = rw->dialect;
        in.have_command_line = true;
        in.compile3 = c.has_compile3 ? &c.compile3 : nullptr;
        in.object_bytes = bytes;
        const CompilandFlagFacts f = detect_compiland_flags(in);
        INFO("compiland " << c.stem << " /GL " << f.whole_program_optimization.detail << " /Gy " << f.function_level_linking.detail);
        CHECK(f.object_kind == ObjectKind::Coff);
        CHECK(f.whole_program_optimization.value == Tri::No);
        CHECK(f.whole_program_optimization.source == Evidence::ObjectFormat);
        CHECK(f.function_level_linking.value == Tri::Yes);  // CmlFixture.cmake passes /Gy
        CHECK(f.guard_cf.value == Tri::No);
        if (is_cl_cell()) {
            CHECK(f.hotpatch.value == Tri::Yes);  // /hotpatch is in S_COMPILE3
        }
        std::printf("FLAGS %s dialect=%s gl=%s(%s) gy=%s(%s) hotpatch=%s(%s) guard_cf=%s(%s)\n", c.stem.c_str(), to_string(rw->dialect),
                    to_string(f.whole_program_optimization.value), to_string(f.whole_program_optimization.source),
                    to_string(f.function_level_linking.value), to_string(f.function_level_linking.source), to_string(f.hotpatch.value),
                    to_string(f.hotpatch.source), to_string(f.guard_cf.value), to_string(f.guard_cf.source));
        ++checked;
    }
    CHECK(checked >= 2);

    auto img = cml::pe::load_image_file(paths.value("basic_exe"));
    REQUIRE(img);
    const FlagFact cfg = detect_module_guard_cf(img->image);
    std::printf("MODULE guard_cf=%s (%s)\n", to_string(cfg.value), cfg.detail.c_str());
    CHECK(cfg.value == Tri::No);
    CHECK(cfg.source == Evidence::PeLoadConfig);
}

TEST_CASE("fixture basic: recorded commands name a compiler that exists and a cwd that exists") {
    const FixturePaths& paths = FixturePaths::get();
    REQUIRE(paths.loaded());
    auto idx = cml::pdb::PdbIndex::open_pdb(paths.value("basic_pdb"));
    REQUIRE(idx);
    for (const SourceCompiland& c : source_compilands(**idx)) {
        auto rec = record_from_build_env(c.env);
        REQUIRE(rec);
        INFO("compiland " << c.stem);
        CHECK(fs::exists(rec->compiler));
        CHECK(fs::is_directory(rec->cwd));
        CHECK(detect_dialect(*rec) != Dialect::Unknown);
        if (is_cl_cell()) {
            CHECK(detect_dialect(*rec) == Dialect::Cl);
        } else {
            CHECK(detect_dialect(*rec) == Dialect::ClangCc1);  // ADR-0012
        }
        auto info = detect_tool(rec->compiler);
        REQUIRE_MESSAGE(info, info.error().format());
        std::printf("TOOL %s family=%s version=%s\n", rec->compiler.c_str(), to_string(info->family), info->version_text.c_str());
    }
}
