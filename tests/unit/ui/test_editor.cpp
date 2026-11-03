#include "cml/ui/editor.h"

#include "doctest.h"

using namespace cml::ui;

namespace {
SourceLocation loc(const char* file, int line, int col) {
    SourceLocation l;
    l.file = file;
    l.line = line;
    l.column = col;
    return l;
}
}  // namespace

TEST_CASE("VS Code preset: code -g file:line:col") {
    EditorConfig config;
    config.kind = EditorKind::VSCode;
    const auto launch = make_editor_launch(config, loc(R"(C:\src\a b.cpp)", 12, 4));
    REQUIRE(launch);
    CHECK(launch->argv == std::vector<std::string>{"code", "-g", R"(C:\src\a b.cpp:12:4)"});
    config.program = R"(C:\Tools\Code.exe)";
    CHECK(make_editor_launch(config, loc("x.cpp", 1, 0))->argv[0] == R"(C:\Tools\Code.exe)");
}

TEST_CASE("Visual Studio preset: devenv /edit file /command \"Edit.GoTo line\"") {
    EditorConfig config;
    config.kind = EditorKind::VisualStudio;
    const auto launch = make_editor_launch(config, loc(R"(C:\src\a.cpp)", 99, 3));
    REQUIRE(launch);
    CHECK(launch->argv == std::vector<std::string>{"devenv", "/edit", R"(C:\src\a.cpp)", "/command", "Edit.GoTo 99"});
    CHECK(join_command_line(launch->argv) == R"(devenv /edit C:\src\a.cpp /command "Edit.GoTo 99")");
}

TEST_CASE("custom template with placeholders and quoting") {
    EditorConfig config;
    config.kind = EditorKind::Custom;
    config.custom_template = R"("C:\Program Files\Editor\ed.exe" --open "{file}" --at {line}:{col} --brace {{x}})";
    const auto launch = make_editor_launch(config, loc(R"(C:\my src\f.cpp)", 7, 2));
    REQUIRE(launch);
    CHECK(launch->argv == std::vector<std::string>{R"(C:\Program Files\Editor\ed.exe)", "--open", R"(C:\my src\f.cpp)", "--at", "7:2",
                                                   "--brace", "{x}"});
    // The file name stays one argument whatever it contains.
    const auto tricky = make_editor_launch(config, loc("a & b \" c.cpp", 1, 1));
    REQUIRE(tricky);
    CHECK(tricky->argv[2] == "a & b \" c.cpp");
    const std::string expected_quoted = "\"a & b \\\" c.cpp\"";
    CHECK(join_command_line({tricky->argv[2]}) == expected_quoted);
}

TEST_CASE("missing line and column default to 1") {
    EditorConfig config;
    config.kind = EditorKind::Custom;
    config.custom_template = "ed {file} {line} {col}";
    CHECK(make_editor_launch(config, loc("f.cpp", 0, 0))->argv == std::vector<std::string>{"ed", "f.cpp", "1", "1"});
}

TEST_CASE("bad templates are CML5703") {
    EditorConfig config;
    config.kind = EditorKind::Custom;
    for (const char* bad : {"", "   ", "ed \"{file}", "ed {nope}", "ed {file", "ed }", "{file} x"}) {
        config.custom_template = bad;
        const auto r = make_editor_launch(config, loc("f.cpp", 1, 1));
        INFO(bad);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == 5703);
    }
    config.custom_template = "ed {file}";
    CHECK_FALSE(make_editor_launch(config, loc("", 1, 1)));
    CHECK_FALSE(make_editor_launch(config, loc("a\nb.cpp", 1, 1)));
}

TEST_CASE("program override replaces the first word of a custom template") {
    EditorConfig config;
    config.kind = EditorKind::Custom;
    config.custom_template = "ed {file}";
    config.program = R"(C:\bin\myed.exe)";
    CHECK(make_editor_launch(config, loc("f.cpp", 1, 1))->argv[0] == R"(C:\bin\myed.exe)");
}

TEST_CASE("quote_argument follows the CommandLineToArgvW rules") {
    CHECK(quote_argument("plain") == "plain");
    CHECK(quote_argument("") == "\"\"");
    CHECK(quote_argument("a b") == "\"a b\"");
    const std::string trailing = "C:\\dir with space\\";
    const std::string trailing_quoted = "\"C:\\dir with space\\\\\"";
    CHECK(quote_argument(trailing) == trailing_quoted);
    const std::string said = "say \"hi\"";
    const std::string said_quoted = "\"say \\\"hi\\\"\"";
    CHECK(quote_argument(said) == said_quoted);
}

TEST_CASE("editor kind names round trip") {
    for (EditorKind k : {EditorKind::VSCode, EditorKind::VisualStudio, EditorKind::Custom}) {
        EditorKind back = EditorKind::Custom;
        REQUIRE(editor_kind_from_string(to_string(k), &back));
        CHECK(back == k);
    }
    EditorKind unused = EditorKind::VSCode;
    CHECK_FALSE(editor_kind_from_string("emacs", &unused));
}
