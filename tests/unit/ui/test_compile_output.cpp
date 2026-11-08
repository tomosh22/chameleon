#include "cml/ui/compile_output.h"

#include "doctest.h"

using namespace cml::ui;

TEST_CASE("cl error with line and column") {
    const auto m = parse_compiler_line(R"(C:\src\game\player.cpp(42,17): error C2065: 'speed': undeclared identifier)");
    REQUIRE(m);
    CHECK(m->severity == OutputSeverity::Error);
    CHECK(m->location.file == R"(C:\src\game\player.cpp)");
    CHECK(m->location.line == 42);
    CHECK(m->location.column == 17);
    CHECK(m->location.clickable());
    CHECK(m->code == "C2065");
    CHECK(m->text == "'speed': undeclared identifier");
}

TEST_CASE("cl warning without column, path with parentheses and spaces") {
    const auto m = parse_compiler_line(R"(C:\Program Files (x86)\lib\a b.cpp(7): warning C4100: 'p': unreferenced parameter)");
    REQUIRE(m);
    CHECK(m->severity == OutputSeverity::Warning);
    CHECK(m->location.file == R"(C:\Program Files (x86)\lib\a b.cpp)");
    CHECK(m->location.line == 7);
    CHECK(m->location.column == 0);
    CHECK(m->code == "C4100");
}

TEST_CASE("clang-cl uses the paren form without a code") {
    const auto m = parse_compiler_line(R"(D:\w\x.cpp(3,9): error: use of undeclared identifier 'q')");
    REQUIRE(m);
    CHECK(m->severity == OutputSeverity::Error);
    CHECK(m->location.line == 3);
    CHECK(m->location.column == 9);
    CHECK(m->code.empty());
    CHECK(m->text == "use of undeclared identifier 'q'");
}

TEST_CASE("clang colon form, with and without drive letter") {
    auto m = parse_compiler_line(R"(C:\w\x.cpp:12:5: warning: unused variable 'v' [-Wunused-variable])");
    REQUIRE(m);
    CHECK(m->severity == OutputSeverity::Warning);
    CHECK(m->location.file == R"(C:\w\x.cpp)");
    CHECK(m->location.line == 12);
    CHECK(m->location.column == 5);

    m = parse_compiler_line("src/a.cpp:8: error: expected ';'");
    REQUIRE(m);
    CHECK(m->location.file == "src/a.cpp");
    CHECK(m->location.line == 8);
    CHECK(m->location.column == 0);
    CHECK(m->location.clickable());
}

TEST_CASE("fatal error and notes") {
    auto m = parse_compiler_line(R"(a.cpp(1): fatal error C1083: Cannot open include file: 'x.h': No such file or directory)");
    REQUIRE(m);
    CHECK(m->severity == OutputSeverity::Error);
    CHECK(m->code == "C1083");
    m = parse_compiler_line(R"(a.cpp(5): note: see declaration of 'f')");
    REQUIRE(m);
    CHECK(m->severity == OutputSeverity::Info);
}

TEST_CASE("linker errors have no line and are not clickable") {
    const auto m = parse_compiler_line("main.obj : error LNK2019: unresolved external symbol \"void __cdecl f(void)\" referenced in function main");
    REQUIRE(m);
    CHECK(m->severity == OutputSeverity::Error);
    CHECK(m->code == "LNK2019");
    CHECK(m->location.file == "main.obj");
    CHECK_FALSE(m->location.clickable());
    const auto fatal = parse_compiler_line("LINK : fatal error LNK1104: cannot open file 'x.lib'");
    REQUIRE(fatal);
    CHECK(fatal->code == "LNK1104");
}

TEST_CASE("plain lines are not diagnostics") {
    CHECK_FALSE(parse_compiler_line(""));
    CHECK_FALSE(parse_compiler_line("Microsoft (R) C/C++ Optimizing Compiler Version 19.40"));
    CHECK_FALSE(parse_compiler_line("compiling 3 files (2 errors expected later)"));
    CHECK_FALSE(parse_compiler_line("a.cpp(12): this is not an error line"));
    CHECK_FALSE(parse_compiler_line("see errors: here"));
    CHECK_FALSE(parse_compiler_line("a.cpp(x,y): error C1: bad numbers"));
    CHECK_FALSE(parse_compiler_line("(5): error C1: no file"));
}

TEST_CASE("trailing carriage return and indentation are ignored") {
    const auto m = parse_compiler_line("  a.cpp(2,3): error C2143: syntax error\r");
    REQUIRE(m);
    CHECK(m->location.file == "a.cpp");
    CHECK(m->text == "syntax error");
}

TEST_CASE("split_output_lines") {
    CHECK(split_output_lines("").empty());
    CHECK(split_output_lines("a\r\nb\n\nc\n") == std::vector<std::string>{"a", "b", "", "c"});
    CHECK(split_output_lines("single") == std::vector<std::string>{"single"});
}
