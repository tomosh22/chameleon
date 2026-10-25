// compile_commands.json reader, lookup and records (WP-05 fallback for ADR-0012).
#include <string>

#include "cml/toolchain/toolchain.h"
#include "doctest.h"

using namespace cml::toolchain;

namespace {
const char* kDb = R"([
  {"directory": "D:/proj/build", "command": "C:\\LLVM\\bin\\clang-cl.exe /nologo /TP /Zi -DX=\"a b\" /FoCMakeFiles\\a.obj -c D:\\proj\\src\\a.cpp",
   "file": "D:\\proj\\src\\a.cpp", "output": "CMakeFiles\\a.obj"},
  {"directory": "D:/proj/build", "arguments": ["cl.exe", "/c", "/Zi", "..\\src\\b.cpp", "/D", "Y=1"], "file": "../src/b.cpp"},
  {"directory": "D:/proj/build", "file": "no_command.cpp"},
  {"command": "x", "comment": "no file: skipped"},
  {"directory": "D:/p", "command": "cl /c \u00e9\ud83d\ude00.cpp", "file": "\u00e9\ud83d\ude00.cpp", "extra": [1, 2.5e3, -3, true, false, null, {"n": []}]}
])";
}

TEST_CASE("compile_commands: parse entries, commands and arguments") {
    auto db = parse_compile_commands(kDb);
    REQUIRE(db);
    REQUIRE(db->size() == 4);  // the entry without "file" is skipped
    CHECK((*db)[0].directory == "D:/proj/build");
    CHECK((*db)[0].output == R"(CMakeFiles\a.obj)");
    CHECK_FALSE((*db)[0].has_arguments);
    CHECK((*db)[1].has_arguments);
    CHECK((*db)[1].arguments.size() == 6);
    CHECK((*db)[3].file == "\xC3\xA9\xF0\x9F\x98\x80.cpp");
}

TEST_CASE("compile_commands: find by absolute source path, any spelling") {
    auto db = parse_compile_commands(kDb);
    REQUIRE(db);
    CHECK(find_compile_command(*db, R"(d:\PROJ\src\A.cpp)") == &(*db)[0]);
    CHECK(find_compile_command(*db, "D:/proj/src/a.cpp") == &(*db)[0]);
    CHECK(find_compile_command(*db, R"(D:\proj\src\b.cpp)") == &(*db)[1]);  // "../src/b.cpp" relative to its directory
    CHECK(find_compile_command(*db, "b.cpp", "D:/proj/src") == &(*db)[1]);
    CHECK(find_compile_command(*db, R"(D:\proj\src\zzz.cpp)") == nullptr);
}

TEST_CASE("compile_commands: records") {
    auto db = parse_compile_commands(kDb);
    REQUIRE(db);
    auto a = record_from_compile_command((*db)[0]);
    REQUIRE(a);
    CHECK(a->origin == RecordOrigin::CompileCommands);
    CHECK(a->compiler == R"(C:\LLVM\bin\clang-cl.exe)");
    CHECK(a->cwd == "D:/proj/build");
    CHECK(a->source == R"(D:\proj\src\a.cpp)");
    REQUIRE(a->args.size() == 7);
    CHECK(a->args[3] == "-DX=a b");
    CHECK(detect_dialect(*a) == Dialect::ClangClDriver);
    auto b = record_from_compile_command((*db)[1]);
    REQUIRE(b);
    CHECK(b->compiler == "cl.exe");
    CHECK(b->args.size() == 5);
    CHECK(b->source == R"(D:/proj/build\../src/b.cpp)");  // made absolute against the entry's directory
    // And through the rewriter.
    RewriteOptions o;
    o.output_object = R"(C:\t\a.obj)";
    auto rw = rewrite_for_patch(*a, o);
    REQUIRE(rw);
    CHECK(rw->command_line == R"(/nologo /c /TP /Z7 "-DX=a b" D:\proj\src\a.cpp /clang:-gcodeview-command-line /FoC:\t\a.obj)");
    // The empty entry has no command.
    CompileCommandEntry empty;
    empty.file = "x.cpp";
    auto bad = record_from_compile_command(empty);
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == diag::kNoCompileCommandsEntry);
}

TEST_CASE("compile_commands: malformed input is CML1505, never a crash") {
    const char* bad[] = {
        "",
        "[",
        "{",
        "[1,]",
        R"([{"file": "a.cpp",}])",
        R"([{"file" "a.cpp"}])",
        R"([{"file": "a.cpp"}] trailing)",
        R"(["unterminated)",
        R"(["bad \q escape"])",
        R"(["bad \u12 escape"])",
        R"(["lone \ud800 surrogate"])",
        R"(["lone \udc00 surrogate"])",
        "[\"control\x01char\"]",
        "[tru]",
        "[-]",
        "[1.]",
        "[1e]",
        R"({"not": "an array"})",
        R"([1, 2])",
    };
    for (const char* text : bad) {
        CAPTURE(text);
        auto r = parse_compile_commands(text);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == diag::kBadJson);
        CHECK(r.error().valid());
    }
    // Depth limit.
    std::string deep(200, '[');
    deep += std::string(200, ']');
    auto r = parse_compile_commands(deep);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == diag::kBadJson);
    // Truncating a valid document anywhere is an error or (for a prefix that is itself valid) not a crash.
    const std::string good = kDb;
    for (std::size_t n = 0; n < good.size(); n += 7) {
        auto t = parse_compile_commands(std::string_view(good).substr(0, n));
        if (!t) {
            CHECK(t.error().code == diag::kBadJson);
        }
    }
}

TEST_CASE("compile_commands: a BOM is accepted and a missing file is CML1506") {
    auto r = parse_compile_commands("\xEF\xBB\xBF[]");
    REQUIRE(r);
    CHECK(r->empty());
    auto missing = load_compile_commands("/no/such/compile_commands.json");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == diag::kCompileCommandsUnreadable);
    auto none = record_from_compile_commands_file("/no/such/compile_commands.json", "a.cpp");
    REQUIRE_FALSE(none);
}
