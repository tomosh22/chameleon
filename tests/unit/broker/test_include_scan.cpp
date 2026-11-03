// WP-15: the include scan that maps headers to compilands independently of the PDB's file list (ADR-0013).
#include "doctest.h"

#include <map>
#include <string>

#include "cml/core/include_scan.h"

using namespace cml;
using namespace cml::core;

namespace {

// A fake file system keyed by normalised path (lower case, backslashes).
struct FakeFs {
    std::map<std::string, std::string> files;
    ScanFileReader reader() {
        return [this](const std::string& path) -> std::optional<std::string> {
            const auto it = files.find(path);
            if (it == files.end()) return std::nullopt;
            return it->second;
        };
    }
};

}  // namespace

TEST_CASE("parse_includes finds quoted and angled directives and skips comments") {
    const auto v = parse_includes(
        "#pragma once\n"
        "#include \"a.h\"\n"
        "  #  include <b/c.h>   // trailing\n"
        "// #include \"commented.h\"\n"
        "/* #include \"block.h\"\n"
        "   #include \"block2.h\" */\n"
        "#include_next <next.h>\n"
        "#import \"imp.h\"\n"
        "#include MACRO_NAME\n"
        "#define X \"not.h\"\n"
        "int x; /* c */ \n"
        "#include \"last.h\"");
    REQUIRE(v.size() == 5);
    CHECK(v[0].name == "a.h");
    CHECK_FALSE(v[0].angled);
    CHECK(v[1].name == "b/c.h");
    CHECK(v[1].angled);
    CHECK(v[2].name == "next.h");
    CHECK(v[3].name == "imp.h");
    CHECK(v[4].name == "last.h");
}

TEST_CASE("the scan follows includes across directories, -I lists and forced includes") {
    FakeFs fs;
    fs.files["c:\\proj\\src\\main.cpp"] = "#include \"util.h\"\n#include <third.h>\n";
    fs.files["c:\\proj\\src\\util.h"] = "#include \"sub/deep.h\"\n#include \"util.h\"\n";  // includes itself: a cycle
    fs.files["c:\\proj\\src\\sub\\deep.h"] = "#include \"../common.h\"\n#include \"missing.h\"\n";
    fs.files["c:\\proj\\src\\common.h"] = "// no includes\n";
    fs.files["c:\\proj\\inc\\third.h"] = "#include <fourth.h>\n";
    fs.files["c:\\proj\\inc\\fourth.h"] = "";
    fs.files["c:\\proj\\build\\pch_force.h"] = "#include \"util.h\"\n";
    fs.files["c:\\proj\\src\\unrelated.h"] = "";

    IncludeScanner scanner(fs.reader());
    ScanRoots roots;
    roots.source = "c:\\proj\\src\\main.cpp";
    roots.cwd = "c:\\proj\\build";
    roots.include_dirs = {"..\\inc", "c:\\proj\\build"};

    const std::set<std::string> deps = scanner.dependencies(roots);
    CHECK(deps == std::set<std::string>{"c:\\proj\\src\\util.h", "c:\\proj\\src\\sub\\deep.h", "c:\\proj\\src\\common.h", "c:\\proj\\inc\\third.h",
                                        "c:\\proj\\inc\\fourth.h"});

    roots.forced_includes = {"pch_force.h"};  // found next to the cwd / in the -I list
    const std::set<std::string> forced = scanner.dependencies(roots);
    CHECK(forced.count("c:\\proj\\build\\pch_force.h") == 1);
    CHECK(forced.count("c:\\proj\\src\\unrelated.h") == 0);
}

TEST_CASE("an angled include is not searched in the including file's directory; a quoted one falls back to -I") {
    FakeFs fs;
    fs.files["c:\\p\\a.cpp"] = "#include <local.h>\n#include \"shared.h\"\n";
    fs.files["c:\\p\\local.h"] = "";
    fs.files["c:\\p\\inc\\shared.h"] = "";
    IncludeScanner scanner(fs.reader());
    ScanRoots roots;
    roots.source = "c:\\p\\a.cpp";
    roots.include_dirs = {"c:\\p\\inc"};
    CHECK(scanner.dependencies(roots) == std::set<std::string>{"c:\\p\\inc\\shared.h"});
}

TEST_CASE("a header that holds no code is found through the scan (the clang-cl case of ADR-0013)") {
    FakeFs fs;
    fs.files["c:\\p\\one.cpp"] = "#include \"decls.h\"\n";
    fs.files["c:\\p\\two.cpp"] = "#include \"decls.h\"\n";
    fs.files["c:\\p\\three.cpp"] = "int x;\n";
    fs.files["c:\\p\\decls.h"] = "struct S;\n";
    IncludeScanner scanner(fs.reader());
    for (const char* tu : {"c:\\p\\one.cpp", "c:\\p\\two.cpp"}) {
        ScanRoots roots;
        roots.source = tu;
        CHECK(scanner.dependencies(roots).count("c:\\p\\decls.h") == 1);
    }
    ScanRoots three;
    three.source = "c:\\p\\three.cpp";
    CHECK(scanner.dependencies(three).empty());
}

TEST_CASE("scan_roots_from_record reads cl, clang-cl and cc1 spellings") {
    toolchain::CompileRecord cl;
    cl.cwd = "c:\\build";
    cl.source = "c:\\src\\a.cpp";
    cl.args = {"/nologo", "/c", "/Zi", "/Ic:\\inc1", "/I", "c:\\inc2", "-Ic:\\inc3", "/external:Ic:\\ext", "/FIpch.h", "/FI", "other.h", "/Yupch.h", "/Fpx.pch"};
    const ScanRoots a = scan_roots_from_record(cl);
    CHECK(a.include_dirs == std::vector<std::string>{"c:\\inc1", "c:\\inc2", "c:\\inc3", "c:\\ext"});
    CHECK(a.forced_includes == std::vector<std::string>{"pch.h", "other.h"});
    CHECK(a.uses_pch);
    CHECK_FALSE(a.creates_pch);

    toolchain::CompileRecord yc = cl;
    yc.args = {"/c", "/Ycpch.h", "/FIpch.h"};
    CHECK(scan_roots_from_record(yc).creates_pch);

    toolchain::CompileRecord cc1;
    cc1.cwd = "c:\\build";
    cc1.source = "c:\\src\\a.cpp";
    cc1.args = {"-cc1", "-triple", "x86_64-pc-windows-msvc", "-I", "c:\\inc", "-isystem", "c:\\sys", "-include", "pch.h", "-include-pch", "x.pch"};
    const ScanRoots b = scan_roots_from_record(cc1);
    CHECK(b.include_dirs == std::vector<std::string>{"c:\\inc", "c:\\sys"});
    CHECK(b.forced_includes == std::vector<std::string>{"pch.h"});
    CHECK(b.uses_pch);

    toolchain::CompileRecord emit = cc1;
    emit.args = {"-cc1", "-emit-pch", "-o", "x.pch"};
    CHECK(scan_roots_from_record(emit).creates_pch);
}

TEST_CASE("comment openers inside string and character literals do not hide later includes") {
    const auto v = parse_includes(
        "const char* a = \"assets/*\";\n"
        "#include \"after_string.h\"\n"
        "char c = '\"'; char d = '/'; char e = '*';\n"
        "#include \"after_chars.h\"\n"
        "const char* r = R\"x(/* #include \"inside_raw.h\" \n  )x\";\n"
        "#include \"after_raw.h\"\n"
        "const char* s = \"//\";\n"
        "int n = 1'000'000;\n"
        "#include \"after_separator.h\"\n"
        "/* a comment with \"a quote */\n"
        "#include \"after_comment.h\"\n"
        "// a line comment with a backslash \\\n"
        "#include \"continued_comment.h\"\n"
        "#include \"last.h\"\n");
    std::vector<std::string> names;
    for (const IncludeDirective& d : v) names.push_back(d.name);
    CHECK(names == std::vector<std::string>{"after_string.h", "after_chars.h", "after_raw.h", "after_separator.h", "after_comment.h", "last.h"});
}

TEST_CASE("a quoted include is also searched in the directories of the files further up the include stack") {
    FakeFs fs;
    fs.files["c:\\p\\a.cpp"] = "#include \"sub/x.h\"\n";
    fs.files["c:\\p\\sub\\x.h"] = "#include \"y.h\"\n";
    fs.files["c:\\p\\y.h"] = "";  // exists only next to a.cpp
    IncludeScanner scanner(fs.reader());
    ScanRoots roots;
    roots.source = "c:\\p\\a.cpp";
    CHECK(scanner.dependencies(roots) == std::set<std::string>{"c:\\p\\sub\\x.h", "c:\\p\\y.h"});
}

TEST_CASE("a header reached again through a longer include stack is searched in the new directories") {
    FakeFs fs;
    fs.files["c:\\p\\a.cpp"] = "#include \"shared.h\"\n#include \"deep/d.h\"\n";
    fs.files["c:\\p\\shared.h"] = "#include \"needs_stack.h\"\n";
    fs.files["c:\\p\\deep\\d.h"] = "#include \"../shared.h\"\n";
    fs.files["c:\\p\\deep\\needs_stack.h"] = "";  // only resolvable when shared.h is reached from deep/d.h
    IncludeScanner scanner(fs.reader());
    ScanRoots roots;
    roots.source = "c:\\p\\a.cpp";
    CHECK(scanner.dependencies(roots).count("c:\\p\\deep\\needs_stack.h") == 1);
}

TEST_CASE("scan_roots_from_record reads joined and separate directory options of every kind") {
    toolchain::CompileRecord r;
    r.cwd = "c:\\b";
    r.source = "c:\\s\\a.cpp";
    r.args = {"-iquotec:\\q1", "-iquote", "c:\\q2", "-isystemc:\\s1", "-isystem", "c:\\s2", "-idirafterc:\\d1", "-idirafter", "c:\\d2",
              "/external:Ic:\\e1", "/external:I", "c:\\e2", "/imsvcc:\\m1", "/imsvc", "c:\\m2", "-Ic:\\i1", "-I", "c:\\i2"};
    const ScanRoots roots = scan_roots_from_record(r);
    CHECK(roots.include_dirs == std::vector<std::string>{"c:\\q1", "c:\\q2", "c:\\s1", "c:\\s2", "c:\\d1", "c:\\d2", "c:\\e1", "c:\\e2", "c:\\m1", "c:\\m2",
                                                         "c:\\i1", "c:\\i2"});
}

TEST_CASE("the precompiled header is the named header and the forced includes with everything they include") {
    FakeFs fs;
    fs.files["c:\\b\\cmake_pch.hxx"] = "#include \"c:/p/pch.h\"\n";
    fs.files["c:\\p\\pch.h"] = "#include \"base.h\"\n";
    fs.files["c:\\p\\base.h"] = "";
    fs.files["c:\\p\\main.cpp"] = "#include \"other.h\"\n";
    fs.files["c:\\p\\other.h"] = "";
    IncludeScanner scanner(fs.reader());

    toolchain::CompileRecord yu;
    yu.cwd = "c:\\b";
    yu.source = "c:\\p\\main.cpp";
    yu.args = {"/Yuc:\\b\\cmake_pch.hxx", "/FIc:\\b\\cmake_pch.hxx", "/Fpc:\\b\\x.pch"};
    ScanRoots roots = scan_roots_from_record(yu);
    CHECK(roots.pch_header == "c:\\b\\cmake_pch.hxx");
    CHECK(scanner.pch_files(roots) == std::set<std::string>{"c:\\b\\cmake_pch.hxx", "c:\\p\\pch.h", "c:\\p\\base.h"});

    // A relative name is looked up next to the source, then in the cwd.
    yu.args = {"/Yucmake_pch.hxx"};
    roots = scan_roots_from_record(yu);
    CHECK(scanner.pch_files(roots).count("c:\\p\\pch.h") == 1);

    // Nothing named and nothing forced: unknown.
    yu.args = {"/Yu"};
    CHECK(scanner.pch_files(scan_roots_from_record(yu)).empty());

    toolchain::CompileRecord yc = yu;
    yc.args = {"/Yccmake_pch.hxx"};
    roots = scan_roots_from_record(yc);
    CHECK(roots.creates_pch);
    CHECK(roots.pch_header == "cmake_pch.hxx");
}
