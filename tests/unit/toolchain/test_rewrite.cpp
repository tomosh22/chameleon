// Behaviour of the rewriter that the corpus does not show: options, results, rule tables, paths.
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cml/toolchain/toolchain.h"
#include "doctest.h"

using namespace cml::toolchain;

namespace {

CompileRecord cl_record(std::string args, std::string source = R"(D:\p\a.cpp)") {
    CompileRecord r;
    r.compiler = R"(C:\VC\bin\cl.exe)";
    r.cwd = R"(D:\p)";
    r.source = std::move(source);
    r.args = split_command_line(args);
    return r;
}

RewriteOptions options(PchDebugMode mode = PchDebugMode::ForceZ7) {
    RewriteOptions o;
    o.output_object = R"(C:\tmp\mod\0123456789abcdef\a.obj)";
    o.pch_debug = mode;
    return o;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    for (const std::string& x : v) {
        if (x == s) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("rewrite: the plan 4.4 contract for cl") {
    auto r = rewrite_for_patch(cl_record(R"(/c /Zi /FS /MP4 /showIncludes /Fd"x.pdb" /Fo"old.obj" /I"inc dir" /DX=1 /std:c++20 /EHsc /MD /Zc:inline /d1reportAllClassLayout /d2cgsummary a.cpp)"), options());
    REQUIRE(r);
    const auto& a = r->args;
    CHECK(contains(a, "/Z7"));
    CHECK_FALSE(contains(a, "/Zi"));
    CHECK_FALSE(contains(a, "/FS"));
    CHECK_FALSE(contains(a, "/MP4"));
    CHECK_FALSE(contains(a, "/showIncludes"));
    CHECK_FALSE(contains(a, "/Fdx.pdb"));
    CHECK(contains(a, "/Fo" + options().output_object));
    CHECK_FALSE(contains(a, "/Foold.obj"));
    CHECK(contains(a, "/nologo"));
    CHECK(contains(a, "/c"));
    // preserved verbatim
    for (const char* keep : {"/Iinc dir", "/DX=1", "/std:c++20", "/EHsc", "/MD", "/Zc:inline", "/d1reportAllClassLayout", "/d2cgsummary"}) {
        CAPTURE(keep);
        CHECK(contains(a, keep));
    }
    CHECK(r->debug_info_replaced);
    CHECK_FALSE(r->uses_pch);
    CHECK(r->compiler == R"(C:\VC\bin\cl.exe)");
    CHECK(r->cwd == R"(D:\p)");
    CHECK(contains(a, "a.cpp"));
}

TEST_CASE("rewrite: the source file is appended exactly once and found by path, not spelling") {
    auto in_cmd = rewrite_for_patch(cl_record(R"(/c /Zi D:/p/./A.CPP)"), options());
    REQUIRE(in_cmd);
    int count = 0;
    for (const auto& t : in_cmd->args) count += (t.find("A.CPP") != std::string::npos || t.find("a.cpp") != std::string::npos) ? 1 : 0;
    CHECK(count == 1);
    auto relative = rewrite_for_patch(cl_record(R"(/c /Zi a.cpp)"), options());
    REQUIRE(relative);
    CHECK(contains(relative->args, "a.cpp"));
    auto missing = rewrite_for_patch(cl_record(R"(/c /Zi)"), options());
    REQUIRE(missing);
    CHECK(missing->args.back() == R"(D:\p\a.cpp)");
    // The source name falls back to the last source-looking input when the record has none.
    auto guessed = rewrite_for_patch(cl_record(R"(/c /Zi foo.cxx bar.cpp)", ""), options());
    REQUIRE(guessed);
    CHECK(guessed->source == "bar.cpp");
}

TEST_CASE("rewrite: /ZI is replaced by /Z7 and /Gy stays on; an explicit later /Gy- still wins") {
    auto r = rewrite_for_patch(cl_record("/c /ZI a.cpp"), options());
    REQUIRE(r);
    CHECK(contains(r->args, "/Z7"));
    CHECK(contains(r->args, "/Gy"));
    auto off = rewrite_for_patch(cl_record("/c /ZI /Gy- a.cpp"), options());
    REQUIRE(off);
    std::size_t gy = 0, gy_minus = 0;
    for (std::size_t i = 0; i < off->args.size(); ++i) {
        if (off->args[i] == "/Gy") gy = i;
        if (off->args[i] == "/Gy-") gy_minus = i;
    }
    CHECK(gy_minus > gy);
}

TEST_CASE("rewrite: /Yc is refused with CML2102 for cl and clang") {
    auto r = rewrite_for_patch(cl_record(R"(/c /Zi /Ycpch.h /Fppch.pch pch.cpp)", R"(D:\p\pch.cpp)"), options());
    REQUIRE_FALSE(r);
    CHECK(r.error().code == 2102);
    CHECK(r.error().message == R"(D:\p\pch.cpp creates the precompiled header (/Yc); changes to it cannot be hot-reloaded.)");
    CHECK(r.error().what_to_do == "Rebuild the project and hot-restart.");
}

TEST_CASE("rewrite: PCH strategies for a /Zi + /Yu TU") {
    const std::string cmd = R"(/c /Zi /Fd"D:\p\b\vc.pdb" /Yupch.h /Fpb\pch.pch /FIpch.h a.cpp)";
    SUBCASE("ForceZ7 keeps /Yu and /Fp") {
        auto r = rewrite_for_patch(cl_record(cmd), options(PchDebugMode::ForceZ7));
        REQUIRE(r);
        CHECK(r->uses_pch);
        CHECK(contains(r->args, "/Z7"));
        CHECK(contains(r->args, "/Yupch.h"));
        CHECK(contains(r->args, R"(/Fpb\pch.pch)"));
        CHECK(r->pdb_copy_from.empty());
    }
    SUBCASE("KeepZiPrivatePdb keeps /Zi, points /Fd at a private copy and records the copy") {
        auto r = rewrite_for_patch(cl_record(cmd), options(PchDebugMode::KeepZiPrivatePdb));
        REQUIRE(r);
        CHECK(contains(r->args, "/Zi"));
        CHECK_FALSE(contains(r->args, "/Z7"));
        CHECK(contains(r->args, R"(/FdC:\tmp\mod\0123456789abcdef\a.compiler.pdb)"));
        CHECK(r->pdb_copy_from == R"(D:\p\b\vc.pdb)");
        CHECK(r->pdb_copy_to == R"(C:\tmp\mod\0123456789abcdef\a.compiler.pdb)");
        CHECK_FALSE(r->debug_info_replaced);
    }
    SUBCASE("KeepZiPrivatePdb with only the PDB from the record resolves relative to the cwd") {
        CompileRecord rec = cl_record(R"(/c /Zi /Yupch.h a.cpp)");
        rec.recorded_pdb = R"(rel\vc.pdb)";
        auto r = rewrite_for_patch(rec, options(PchDebugMode::KeepZiPrivatePdb));
        REQUIRE(r);
        CHECK(r->pdb_copy_from == R"(D:\p\rel\vc.pdb)");
    }
    SUBCASE("KeepZiPrivatePdb without any recorded PDB warns") {
        auto r = rewrite_for_patch(cl_record(R"(/c /Zi /Yupch.h a.cpp)"), options(PchDebugMode::KeepZiPrivatePdb));
        REQUIRE(r);
        CHECK(r->pdb_copy_from.empty());
        REQUIRE(r->warnings.size() == 1);
        CHECK(r->warnings[0].code == diag::kPdbCopyFailed);
    }
    SUBCASE("DropPch removes /Yu and /Fp but keeps the forced include") {
        auto r = rewrite_for_patch(cl_record(cmd), options(PchDebugMode::DropPch));
        REQUIRE(r);
        CHECK(contains(r->args, "/Z7"));
        CHECK_FALSE(contains(r->args, "/Yupch.h"));
        CHECK_FALSE(contains(r->args, R"(/Fpb\pch.pch)"));
        CHECK(contains(r->args, "/FIpch.h"));
    }
    SUBCASE("a TU without a PCH is unaffected by the mode") {
        auto a = rewrite_for_patch(cl_record("/c /Zi a.cpp"), options(PchDebugMode::KeepZiPrivatePdb));
        auto b = rewrite_for_patch(cl_record("/c /Zi a.cpp"), options(PchDebugMode::DropPch));
        REQUIRE(a);
        REQUIRE(b);
        CHECK(a->command_line == b->command_line);
        CHECK(contains(a->args, "/Z7"));
    }
    SUBCASE("Auto is the documented default") {
        auto a = rewrite_for_patch(cl_record(cmd), options(PchDebugMode::Auto));
        auto b = rewrite_for_patch(cl_record(cmd), options(kDefaultPchDebugMode));
        REQUIRE(a);
        REQUIRE(b);
        CHECK(a->command_line == b->command_line);
    }
}

TEST_CASE("rewrite: capture_includes adds the switch for each dialect") {
    RewriteOptions o = options();
    o.capture_includes = true;
    auto cl = rewrite_for_patch(cl_record("/c /Zi a.cpp"), o);
    REQUIRE(cl);
    CHECK(contains(cl->args, "/showIncludes"));
    CompileRecord cc1;
    cc1.compiler = R"(C:\LLVM\bin\clang.exe)";
    cc1.cwd = R"(D:\p)";
    cc1.source = R"(D:\p\a.cpp)";
    cc1.args = {"-cc1", "-emit-obj", "-gcodeview", "-x", "c++", "-o", "x.obj"};
    auto r = rewrite_for_patch(cc1, o);
    REQUIRE(r);
    CHECK(contains(r->args, "-show-includes"));
}

TEST_CASE("rewrite: clang cc1 replay") {
    CompileRecord rec;
    rec.compiler = R"(C:\LLVM\bin\clang.exe)";
    rec.cwd = R"(D:\p)";
    rec.source = R"(D:\p\a.cpp)";
    rec.args = {"-cc1", "-triple", "x86_64-pc-windows-msvc19.44.0", "-gcodeview", "-debug-info-kind=constructor", "-dependency-file", "a.d",
                "-MT", "a.obj", "-sys-header-deps", "-flto-visibility-public-std", "-o", "old.obj", "-x", "c++", R"(D:\p\a.cpp)"};
    auto r = rewrite_for_patch(rec, options());
    REQUIRE(r);
    CHECK(r->dialect == Dialect::ClangCc1);
    CHECK(r->args.front() == "-cc1");
    CHECK(contains(r->args, "-gcodeview-command-line"));
    CHECK(contains(r->args, "-gcodeview"));
    CHECK(contains(r->args, "-flto-visibility-public-std"));  // not an LTO switch
    CHECK(r->warnings.empty());
    CHECK(contains(r->args, "-debug-info-kind=constructor"));
    CHECK(contains(r->args, "-emit-obj"));
    for (const char* dropped : {"-dependency-file", "a.d", "-MT", "a.obj", "-sys-header-deps", "old.obj"}) {
        CAPTURE(dropped);
        CHECK_FALSE(contains(r->args, dropped));
    }
    // -o <our object> is the last option before nothing: the source was already in the command.
    CHECK(r->args[r->args.size() - 2] == "-o");
    CHECK(r->args.back() == options().output_object);
    // Without debug info there is nothing to attach the command line to.
    rec.args = {"-cc1", "-emit-obj", "-x", "c++", "-o", "old.obj"};
    auto nodebug = rewrite_for_patch(rec, options());
    REQUIRE(nodebug);
    CHECK_FALSE(contains(nodebug->args, "-gcodeview-command-line"));
    // Chained patches: do not add the flag when asked not to.
    rec.args = {"-cc1", "-emit-obj", "-gcodeview", "-x", "c++", "-o", "old.obj"};
    RewriteOptions no_cv = options();
    no_cv.ensure_codeview_command_line = false;
    auto plain = rewrite_for_patch(rec, no_cv);
    REQUIRE(plain);
    CHECK_FALSE(contains(plain->args, "-gcodeview-command-line"));
}

TEST_CASE("rewrite: refusals") {
    CompileRecord unknown;
    unknown.compiler = "/usr/bin/g++";
    unknown.source = "a.cpp";
    unknown.args = {"-c", "a.cpp"};
    auto r = rewrite_for_patch(unknown, options());
    REQUIRE_FALSE(r);
    CHECK(r.error().code == diag::kUnsupportedCompiler);
    CHECK(detect_dialect(unknown) == Dialect::Unknown);
    RewriteOptions none;
    auto no_out = rewrite_for_patch(cl_record("/c a.cpp"), none);
    REQUIRE_FALSE(no_out);
    CHECK(no_out.error().code == diag::kCannotPrepareOutput);
}

TEST_CASE("rewrite: /GL is downgraded with a warning, once") {
    auto r = rewrite_for_patch(cl_record("/c /Zi /GL /GL a.cpp"), options());
    REQUIRE(r);
    CHECK(contains(r->args, "/GL-"));
    CHECK_FALSE(contains(r->args, "/GL"));
    REQUIRE(r->warnings.size() == 1);
    CHECK(r->warnings[0].code == diag::kWholeProgramDowngraded);
}

TEST_CASE("rule tables: every rule is well formed and documented") {
    const std::span<const Rule> tables[] = {cl_rules(), clang_cl_extra_rules(), cc1_rules()};
    for (const auto& table : tables) {
        CHECK_FALSE(table.empty());
        for (const Rule& rule : table) {
            CAPTURE(rule.pattern);
            CHECK(std::string(rule.pattern) != "");
            CHECK(std::string(rule.why) != "");
            if (rule.action == Action::Error) {
                CHECK(rule.diag != 0);
            }
            if (rule.action == Action::Replace) {
                CHECK(std::string(rule.replacement) != "");
            }
            CHECK((rule.warn_diag == 0 || rule.warn_diag >= 1000));
        }
    }
    // The regexes compile: every Regex rule is exercised through rewrite_for_patch on a matching option.
    CompileRecord rec = cl_record("/c /MP /MP8 /Zi a.cpp");
    auto r = rewrite_for_patch(rec, options());
    REQUIRE(r);
    CHECK_FALSE(contains(r->args, "/MP"));
    CHECK_FALSE(contains(r->args, "/MP8"));
}

TEST_CASE("rewrite: response files are expanded before the rules run") {
    CompileRecord rec = cl_record("@opts.rsp a.cpp");
    RewriteOptions o = options();
    o.file_reader = [](const std::string& path) -> cml::Result<std::string> {
        if (path == R"(D:\p\opts.rsp)") {
            return std::string("/Zi /Fd\"x.pdb\" /DFROM_RSP");
        }
        return cml::Diag::make(diag::kResponseFileUnreadable, "x", "y");
    };
    auto r = rewrite_for_patch(rec, o);
    REQUIRE(r);
    CHECK(contains(r->args, "/Z7"));
    CHECK(contains(r->args, "/DFROM_RSP"));
    CHECK(r->expanded_args.size() == 4);
    CHECK_FALSE(contains(r->args, "@opts.rsp"));
}

TEST_CASE("patch_object_path") {
    CHECK(patch_object_path(R"(C:\tmp\sess)", "Game.dll", 0x1234, R"(D:\p\src\logic.cpp)") == R"(C:\tmp\sess\Game.dll\0000000000001234\logic.obj)");
    CHECK(patch_object_path(R"(C:\tmp\sess\)", "a<b>:c", 0xFFFFFFFFFFFFFFFFull, "x.cc") == R"(C:\tmp\sess\a_b__c\ffffffffffffffff\x.obj)");
    CHECK(patch_object_path("T", "", 1, "noext") == R"(T\module\0000000000000001\noext.obj)");
}

TEST_CASE("detect_dialect") {
    CompileRecord r;
    r.compiler = R"(C:\VS\bin\Hostx64\x64\CL.EXE)";
    CHECK(detect_dialect(r) == Dialect::Cl);
    r.compiler = R"(C:\LLVM\bin\clang-cl.exe)";
    CHECK(detect_dialect(r) == Dialect::ClangClDriver);
    r.args = {"-cc1"};
    CHECK(detect_dialect(r) == Dialect::ClangCc1);
    r.compiler = "cl";
    r.args = {"/c"};
    CHECK(detect_dialect(r) == Dialect::Cl);
}

namespace {
std::size_t index_of(const std::vector<std::string>& v, const std::string& s) {
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] == s) return i;
    }
    return static_cast<std::size_t>(-1);
}
}  // namespace

TEST_CASE("rewrite: appended options go before the end-of-options marker") {
    const std::size_t none = static_cast<std::size_t>(-1);
    SUBCASE("cl dialect") {
        auto r = rewrite_for_patch(cl_record("/c /Zi /EHsc -- a.cpp"), options());
        REQUIRE(r);
        const std::size_t marker = index_of(r->args, "--");
        REQUIRE(marker != none);
        CHECK(index_of(r->args, "/Fo" + options().output_object) < marker);
        CHECK(index_of(r->args, "/nologo") < marker);
        CHECK(index_of(r->args, "/c") < marker);
        CHECK(r->args[marker + 1] == "a.cpp");
        CHECK(r->args.size() == marker + 2);
    }
    SUBCASE("clang-cl driver, /-- spelling, and the source added after the marker when missing") {
        CompileRecord rec;
        rec.compiler = R"(C:\LLVM\bin\clang-cl.exe)";
        rec.cwd = R"(D:\p)";
        rec.source = R"(D:\p\a.cpp)";
        rec.args = split_command_line("/Zi -c /--");
        auto r = rewrite_for_patch(rec, options());
        REQUIRE(r);
        const std::size_t marker = index_of(r->args, "/--");
        REQUIRE(marker != none);
        CHECK(index_of(r->args, "/clang:-gcodeview-command-line") < marker);
        CHECK(index_of(r->args, "/Fo" + options().output_object) < marker);
        CHECK(r->args.back() == R"(D:\p\a.cpp)");
        CHECK(r->args.size() == marker + 2);
    }
    SUBCASE("tokens after the marker that look like options are inputs, not rules") {
        auto r = rewrite_for_patch(cl_record("/c /Zi -- -Fofoo.obj a.cpp"), options());
        REQUIRE(r);
        CHECK(index_of(r->args, "-Fofoo.obj") != none);  // not dropped
        CHECK(index_of(r->args, "-Fofoo.obj") > index_of(r->args, "--"));
    }
    SUBCASE("cc1") {
        CompileRecord rec;
        rec.compiler = R"(C:\LLVM\bin\clang.exe)";
        rec.cwd = R"(D:\p)";
        rec.source = R"(D:\p\a.cpp)";
        rec.args = {"-cc1", "-emit-obj", "-gcodeview", "-x", "c++", "-o", "x.obj", "--", R"(D:\p\a.cpp)"};
        auto r = rewrite_for_patch(rec, options());
        REQUIRE(r);
        const std::size_t marker = index_of(r->args, "--");
        REQUIRE(marker != none);
        CHECK(index_of(r->args, "-o") + 1 < marker);
        CHECK(r->args[index_of(r->args, "-o") + 1] == options().output_object);
        CHECK(index_of(r->args, "-gcodeview-command-line") < marker);
        CHECK(r->args.size() == marker + 2);
    }
    SUBCASE("DropPch before the marker keeps the marker in front of the inputs") {
        auto r = rewrite_for_patch(cl_record("/c /Zi /Yupch.h /Fppch.pch -- a.cpp"), options(PchDebugMode::DropPch));
        REQUIRE(r);
        const std::size_t marker = index_of(r->args, "--");
        REQUIRE(marker != none);
        CHECK(index_of(r->args, "/Fo" + options().output_object) < marker);
        CHECK(r->args[marker + 1] == "a.cpp");
    }
}

TEST_CASE("rewrite: /Fo: /Fd: /Fe: colon forms") {
    auto sep = rewrite_for_patch(cl_record(R"(/c /Zi /Fo: out\a.obj /Fd: out\vc.pdb /Fe: out\a.exe /EHsc a.cpp)"), options());
    REQUIRE(sep);
    CHECK(index_of(sep->args, R"(out\a.obj)") == static_cast<std::size_t>(-1));
    CHECK(index_of(sep->args, R"(out\vc.pdb)") == static_cast<std::size_t>(-1));
    CHECK(index_of(sep->args, R"(out\a.exe)") == static_cast<std::size_t>(-1));
    CHECK(index_of(sep->args, "/EHsc") != static_cast<std::size_t>(-1));
    auto att = rewrite_for_patch(cl_record(R"(/c /Zi /Fo:out\a.obj /Fd:out\vc.pdb /Fe:out\a.exe /EHsc a.cpp)"), options());
    REQUIRE(att);
    CHECK(sep->command_line == att->command_line);
    // The value of /Fd: (either spelling) is the PDB a /Yu translation unit copies.
    auto pch = rewrite_for_patch(cl_record(R"(/c /Zi /Fd: D:\p\out\vc.pdb /Yupch.h a.cpp)"), options(PchDebugMode::KeepZiPrivatePdb));
    REQUIRE(pch);
    CHECK(pch->pdb_copy_from == R"(D:\p\out\vc.pdb)");
    auto pch2 = rewrite_for_patch(cl_record(R"(/c /Zi /Fd:D:\p\out\vc.pdb /Yupch.h a.cpp)"), options(PchDebugMode::KeepZiPrivatePdb));
    REQUIRE(pch2);
    CHECK(pch2->pdb_copy_from == R"(D:\p\out\vc.pdb)");
}

TEST_CASE("rewrite: /Fd naming a directory resolves to vc<NNN>.pdb") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("cml_fd_dir_" + std::to_string(reinterpret_cast<std::uintptr_t>(&root)));
    fs::create_directories(root / "dir");
    const auto touch = [&](const fs::path& p) { std::ofstream(p) << "x"; };
    touch(root / "dir" / "vc140.pdb");
    const std::string dir = (root / "dir").string();
    const std::string S(1, static_cast<char>(fs::path::preferred_separator));
    const std::string sepdir = dir + S;
    const auto rec_with = [&](const std::string& fd, const std::string& recorded) {
        CompileRecord rec = cl_record("/c /Zi /Fd" + fd + " /Yupch.h a.cpp");
        rec.cwd = root.string();
        rec.recorded_pdb = recorded;
        return rec;
    };
    SUBCASE("trailing separator, no recorded PDB: the only vc*.pdb in the directory") {
        auto r = rewrite_for_patch(rec_with(sepdir, ""), options(PchDebugMode::KeepZiPrivatePdb));
        REQUIRE(r);
        CHECK(r->pdb_copy_from == sepdir + "vc140.pdb");
    }
    SUBCASE("an existing directory without separator") {
        auto r = rewrite_for_patch(rec_with(dir, ""), options(PchDebugMode::KeepZiPrivatePdb));
        REQUIRE(r);
        CHECK(r->pdb_copy_from == sepdir + "vc140.pdb");
    }
    SUBCASE("relative directory is relative to the recorded cwd") {
        auto r = rewrite_for_patch(rec_with("dir" + S, ""), options(PchDebugMode::KeepZiPrivatePdb));
        REQUIRE(r);
        CHECK(r->pdb_copy_from == root.string() + S + "dir" + S + "vc140.pdb");
    }
    SUBCASE("several vc*.pdb: the one the recorded PDB names, else the highest number") {
        touch(root / "dir" / "vc143.pdb");
        touch(root / "dir" / "vc120.pdb");
        touch(root / "dir" / "vcfoo.pdb");  // not vc<digits>.pdb
        auto named = rewrite_for_patch(rec_with(sepdir, R"(D:\gone\vc120.pdb)"), options(PchDebugMode::KeepZiPrivatePdb));
        REQUIRE(named);
        CHECK(named->pdb_copy_from == sepdir + "vc120.pdb");
        auto highest = rewrite_for_patch(rec_with(sepdir, ""), options(PchDebugMode::KeepZiPrivatePdb));
        REQUIRE(highest);
        CHECK(highest->pdb_copy_from == sepdir + "vc143.pdb");
    }
    SUBCASE("an existing recorded PDB wins over /Fd") {
        touch(root / "recorded.pdb");
        auto r = rewrite_for_patch(rec_with(sepdir, (root / "recorded.pdb").string()), options(PchDebugMode::KeepZiPrivatePdb));
        REQUIRE(r);
        CHECK(r->pdb_copy_from == (root / "recorded.pdb").string());
    }
    SUBCASE("an empty directory falls back to vc140.pdb; a file-like /Fd gets .pdb") {
        fs::create_directories(root / "empty");
        auto r = rewrite_for_patch(rec_with((root / "empty").string() + S, ""), options(PchDebugMode::KeepZiPrivatePdb));
        REQUIRE(r);
        CHECK(r->pdb_copy_from == (root / "empty").string() + S + "vc140.pdb");
        auto f = rewrite_for_patch(rec_with("D:\\p\\mypdb", ""), options(PchDebugMode::KeepZiPrivatePdb));
        REQUIRE(f);
        CHECK(f->pdb_copy_from == "D:\\p\\mypdb.pdb");
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("rewrite: -Xclang and /clang: options are judged by the cc1 rule table") {
    CompileRecord rec;
    rec.compiler = R"(C:\LLVM\bin\clang-cl.exe)";
    rec.cwd = R"(D:\p)";
    rec.source = R"(D:\p\a.cpp)";
    SUBCASE("depfile and codeview switches dropped with their values, -include-pch kept and recorded") {
        rec.args = split_command_line(
            "/Zi -Xclang -dependency-file -Xclang dep.d -Xclang -gno-codeview-command-line -Xclang -include-pch -Xclang a.pch "
            "/clang:-sys-header-deps /clang:-gno-codeview-command-line /clang:-MT /clang:o.obj a.cpp");
        auto r = rewrite_for_patch(rec, options());
        REQUIRE(r);
        CHECK(r->uses_pch);
        CHECK(index_of(r->args, "-dependency-file") == static_cast<std::size_t>(-1));
        CHECK(index_of(r->args, "dep.d") == static_cast<std::size_t>(-1));
        CHECK(index_of(r->args, "-gno-codeview-command-line") == static_cast<std::size_t>(-1));
        CHECK(index_of(r->args, "/clang:-sys-header-deps") == static_cast<std::size_t>(-1));
        CHECK(index_of(r->args, "/clang:-MT") == static_cast<std::size_t>(-1));
        CHECK(index_of(r->args, "/clang:o.obj") == static_cast<std::size_t>(-1));
        CHECK(index_of(r->args, "-include-pch") != static_cast<std::size_t>(-1));
        CHECK(index_of(r->args, "a.pch") != static_cast<std::size_t>(-1));
        // The codeview-command-line switch is gone, so the rewriter adds its own.
        CHECK(index_of(r->args, "/clang:-gcodeview-command-line") != static_cast<std::size_t>(-1));
    }
    SUBCASE("-emit-pch is CML2102, in both spellings") {
        rec.args = split_command_line("/Zi -Xclang -emit-pch a.cpp");
        auto a = rewrite_for_patch(rec, options());
        REQUIRE_FALSE(a);
        CHECK(a.error().code == diag::kPchCreatingTu);
        rec.args = split_command_line("/Zi /clang:-emit-pch a.cpp");
        auto b = rewrite_for_patch(rec, options());
        REQUIRE_FALSE(b);
        CHECK(b.error().code == diag::kPchCreatingTu);
    }
    SUBCASE("-Xclang with an option the table does not know is passed through") {
        rec.args = split_command_line("/Zi -Xclang -fsomething-odd a.cpp");
        auto r = rewrite_for_patch(rec, options());
        REQUIRE(r);
        CHECK(index_of(r->args, "-Xclang") != static_cast<std::size_t>(-1));
        CHECK(index_of(r->args, "-fsomething-odd") != static_cast<std::size_t>(-1));
    }
    SUBCASE("-Xclang -emit-llvm-bc is replaced and warns") {
        rec.args = split_command_line("/Zi -Xclang -emit-llvm-bc a.cpp");
        auto r = rewrite_for_patch(rec, options());
        REQUIRE(r);
        CHECK(index_of(r->args, "-emit-obj") != static_cast<std::size_t>(-1));
        REQUIRE(r->warnings.size() == 1);
        CHECK(r->warnings[0].code == diag::kWholeProgramDowngraded);
    }
}
