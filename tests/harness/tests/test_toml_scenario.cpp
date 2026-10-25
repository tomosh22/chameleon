// TOML subset reader and scenario validation.
#include <string>
#include <vector>

#include "cml/ipc/control_ids.h"
#include "doctest.h"
#include "scenario.h"
#include "toml.h"

using namespace cmltest;

namespace {

toml::Document parse_ok(const std::string& text) {
    cml::Result<toml::Document> r = toml::parse(text);
    REQUIRE_MESSAGE(r.has_value(), (r.has_value() ? "" : r.error().format()));
    return *r;
}

void check_syntax_error(const std::string& text, const std::string& expect_in_message = std::string()) {
    cml::Result<toml::Document> r = toml::parse(text);
    INFO("input: " << text.substr(0, 100));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == cml::ids::kScenarioSyntax);
    CHECK(r.error().message.find("line ") != std::string::npos);
    if (!expect_in_message.empty()) {
        CHECK(r.error().message.find(expect_in_message) != std::string::npos);
    }
}

}  // namespace

TEST_CASE("toml: keys, strings, integers, booleans, comments") {
    const toml::Document d = parse_ok(
        "# a comment\n"
        "name = \"basic\"   # trailing comment\n"
        "literal = 'C:\\raw\\path'\n"
        "n = 120\n"
        "neg = -5\n"
        "plus = +7\n"
        "big = 1_000\n"
        "yes = true\n"
        "no = false\n"
        "esc = \"a\\tb\\n\\\"q\\\" \\\\ \\u00e9 \\U0001F600\"\n"
        "\n"
        "hash = \"not # a comment\"\n");
    CHECK(d.root.find("name")->text == "basic");
    CHECK(d.root.find("literal")->text == "C:\\raw\\path");
    CHECK(d.root.find("n")->integer == 120);
    CHECK(d.root.find("neg")->integer == -5);
    CHECK(d.root.find("plus")->integer == 7);
    CHECK(d.root.find("big")->integer == 1000);
    CHECK(d.root.find("yes")->boolean);
    CHECK_FALSE(d.root.find("no")->boolean);
    CHECK(d.root.find("esc")->text == "a\tb\n\"q\" \\ \xC3\xA9 \xF0\x9F\x98\x80");
    CHECK(d.root.find("hash")->text == "not # a comment");
    CHECK(d.root.find("name")->line == 2);
    CHECK(d.root.find("missing") == nullptr);
}

TEST_CASE("toml: tables and arrays of tables, CRLF, BOM") {
    const toml::Document d = parse_ok(
        "\xEF\xBB\xBF" "title = \"x\"\r\n"
        "[meta]\r\n"
        "k = 1\r\n"
        "[[step]]\r\n"
        "kind = \"a\"\r\n"
        "[[step]]\r\n"
        "kind = \"b\"  # c\r\n"
        "n = 2\r\n");
    CHECK(d.root.find("title")->text == "x");
    REQUIRE(d.find_table("meta") != nullptr);
    CHECK(d.find_table("meta")->find("k")->integer == 1);
    REQUIRE(d.array_tables.size() == 2);
    CHECK(d.array_tables[0].name == "step");
    CHECK(d.array_tables[0].find("kind")->text == "a");
    CHECK(d.array_tables[1].find("kind")->text == "b");
    CHECK(d.array_tables[1].find("n")->integer == 2);
    CHECK(d.array_tables[1].line == 6);
}

TEST_CASE("toml: unsupported and malformed input is rejected with CML5410 and a line number") {
    check_syntax_error("a.b = 1\n", "dotted keys");
    check_syntax_error("\"quoted\" = 1\n", "quoted keys");
    check_syntax_error("a = [1, 2]\n", "arrays of values");
    check_syntax_error("a = { b = 1 }\n", "inline tables");
    check_syntax_error("a = \"\"\"multi\nline\"\"\"\n", "multi-line");
    check_syntax_error("a = '''multi'''\n", "multi-line");
    check_syntax_error("a = 1.5\n", "unsupported value");
    check_syntax_error("a = 0x10\n", "unsupported value");
    check_syntax_error("a = 1979-05-27\n", "unsupported value");
    check_syntax_error("a = tru\n");
    check_syntax_error("a =\n");
    check_syntax_error("a\n");
    check_syntax_error("= 1\n");
    check_syntax_error("a = 1 2\n", "after the value");
    check_syntax_error("a = \"unterminated\n", "unterminated");
    check_syntax_error("a = 'unterminated\n", "unterminated");
    check_syntax_error("a = \"bad \\q escape\"\n", "unknown escape");
    check_syntax_error("a = \"\\u12\"\n");
    check_syntax_error("a = \"\\uD800\"\n", "scalar value");
    check_syntax_error("a = 99999999999999999999\n", "out of range");
    check_syntax_error("a = 1__0\n");
    check_syntax_error("a = _1\n");
    check_syntax_error("a = 1\na = 2\n", "duplicate key 'a'");
    check_syntax_error("[t]\n[t]\n", "defined twice");
    check_syntax_error("[t]\n[[t]]\n", "conflicts");
    check_syntax_error("[[t]]\n[t]\n", "conflicts");
    check_syntax_error("[a.b]\n", "dotted table");
    check_syntax_error("[a\n", "']' expected");
    check_syntax_error("[[a]\n", "']]' expected");
    check_syntax_error("[]\n");
    check_syntax_error("[t] x\n", "after the value");
    check_syntax_error("a = 1\n\n\nb = $\n", "line 4");
    check_syntax_error(std::string("a = \"x\0y\"\n", 10), "NUL");
}

TEST_CASE("toml: limits") {
    check_syntax_error(std::string(toml::kMaxDocumentBytes + 1, '#'), "larger than 1 MiB");
    check_syntax_error("# " + std::string(toml::kMaxLineBytes, 'x') + "\n", "longer than 64 KiB");
    std::string many_keys;
    for (std::size_t i = 0; i <= toml::kMaxKeysPerTable; ++i) {
        many_keys += "k" + std::to_string(i) + " = 1\n";
    }
    check_syntax_error(many_keys, "1024 keys");
    std::string many_tables;
    for (std::size_t i = 0; i <= toml::kMaxTables; ++i) {
        many_tables += "[[t]]\n";
    }
    check_syntax_error(many_tables, "4096 tables");
    // a document without a final newline, and an empty one, are fine
    CHECK(toml::parse("a = 1").has_value());
    CHECK(toml::parse("").has_value());
    CHECK(toml::parse("\n\n# only comments\n").has_value());
}

TEST_CASE("toml: truncated documents never crash") {
    const std::string doc = "name = \"basic\"\n[[step]]\nkind = \"expect_stdout\"\nregex = \"TICK n=\\\\d+\"\nwithin_s = 10\n";
    REQUIRE(toml::parse(doc).has_value());
    for (std::size_t n = 0; n <= doc.size(); ++n) {
        (void)toml::parse(doc.substr(0, n));  // may succeed or fail, must not crash
    }
}

// --- scenarios -------------------------------------------------------------------------------

namespace {

const char* kBasic =
    "name = \"basic\"\n"
    "fixture = \"basic\"\n"
    "timeout_s = 120\n"
    "[[step]]\nkind = \"launch\"\n"
    "[[step]]\nkind = \"expect_stdout\"\nregex = \"TICK n=\\\\d+ value=1\"\nwithin_s = 10\n"
    "[[step]]\nkind = \"expect_module_enabled\"\nmodule = \"fixture_basic.exe\"\n"
    "[[step]]\nkind = \"edit\"\npatch = \"edits/value_2.edit\"\n"
    "[[step]]\nkind = \"reload\"\n"
    "[[step]]\nkind = \"expect_stdout\"\nregex = \"value=2\"\n"
    "[[step]]\nkind = \"expect_diag\"\nid = \"CML3101\"\nwithin_s = 5\n";

void check_invalid(const std::string& text, std::uint32_t code, const std::string& in_message = std::string()) {
    cml::Result<Scenario> r = parse_scenario(text);
    INFO("scenario: " << text.substr(0, 200));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == code);
    if (!in_message.empty()) {
        CHECK(r.error().message.find(in_message) != std::string::npos);
    }
}

std::string with_steps(const std::string& steps) {
    return "name = \"s\"\nfixture = \"basic\"\n" + steps;
}

}  // namespace

TEST_CASE("scenario: the basic scenario parses") {
    cml::Result<Scenario> r = parse_scenario(kBasic);
    REQUIRE_MESSAGE(r.has_value(), (r.has_value() ? "" : r.error().format()));
    CHECK(r->name == "basic");
    CHECK(r->fixture == "basic");
    CHECK(r->timeout_s == 120);
    REQUIRE(r->steps.size() == 7);
    CHECK(r->steps[0].kind == StepKind::Launch);
    CHECK(r->steps[1].kind == StepKind::ExpectStdout);
    CHECK(r->steps[1].regex == "TICK n=\\d+ value=1");
    CHECK(r->steps[1].within_s == 10);
    CHECK(r->steps[2].kind == StepKind::ExpectModuleEnabled);
    CHECK(r->steps[2].module == "fixture_basic.exe");
    CHECK(r->steps[3].kind == StepKind::Edit);
    CHECK(r->steps[3].patch == "edits/value_2.edit");
    CHECK(r->steps[4].kind == StepKind::Reload);
    CHECK(r->steps[4].within_s == 60);
    CHECK(r->steps[5].within_s == 10);  // default
    CHECK(r->steps[6].id == "CML3101");
}

TEST_CASE("scenario: required capabilities of steps") {
    cml::Result<Scenario> r = parse_scenario(kBasic);
    REQUIRE(r.has_value());
    CHECK(r->steps[0].required_capability() == nullptr);
    CHECK(r->steps[1].required_capability() == nullptr);
    CHECK(r->steps[2].required_capability() == nullptr);
    CHECK(r->steps[3].required_capability() == nullptr);
    CHECK(std::string(r->steps[4].required_capability()) == "reload");
    CHECK(r->steps[5].required_capability() == nullptr);
    CHECK(std::string(r->steps[6].required_capability()) == "diag-events");
    CHECK(r->steps[1].describe() == "expect_stdout /TICK n=\\d+ value=1/ within 10 s");
    CHECK(r->steps[3].describe() == "edit edits/value_2.edit");
}

TEST_CASE("scenario: invalid files are rejected with CML5411 (or CML5410 / CML5420)") {
    const std::uint32_t invalid = cml::ids::kScenarioInvalid;
    check_invalid("", invalid, "'name'");
    check_invalid("name = \"x\"\n", invalid, "'fixture'");
    check_invalid("name = 5\nfixture = \"basic\"\n", invalid, "string");
    check_invalid("name = \"bad name\"\nfixture = \"basic\"\n", invalid, "'name'");
    check_invalid("name = \"x\"\nfixture = \"../evil\"\n", invalid, "'fixture'");
    check_invalid("name = \"x\"\nfixture = \"basic\"\nstray = 1\n[[step]]\nkind = \"launch\"\n", invalid, "unknown key 'stray'");
    check_invalid("name = \"x\"\nfixture = \"basic\"\ntimeout_s = 1\n[[step]]\nkind = \"launch\"\n", invalid, "timeout_s");
    check_invalid("name = \"x\"\nfixture = \"basic\"\ntimeout_s = \"60\"\n[[step]]\nkind = \"launch\"\n", invalid, "timeout_s");
    check_invalid("name = \"x\"\nfixture = \"basic\"\n", invalid, "no [[step]]");
    check_invalid(with_steps("[extra]\na = 1\n[[step]]\nkind = \"launch\"\n"), invalid, "plain tables");
    check_invalid(with_steps("[[other]]\na = 1\n"), invalid, "[[other]]");
    check_invalid(with_steps("[[step]]\n"), invalid, "'kind'");
    check_invalid(with_steps("[[step]]\nkind = \"fly\"\n"), invalid, "unknown step kind 'fly'");
    check_invalid(with_steps("[[step]]\nkind = \"launch\"\nextra = 1\n"), invalid, "unknown key 'extra'");
    check_invalid(with_steps("[[step]]\nkind = \"launch\"\n[[step]]\nkind = \"launch\"\n"), invalid, "only once");
    check_invalid(with_steps("[[step]]\nkind = \"reload\"\n"), invalid, "needs a launch");
    check_invalid(with_steps("[[step]]\nkind = \"expect_stdout\"\nregex = \"x\"\n"), invalid, "needs a launch");
    check_invalid(with_steps("[[step]]\nkind = \"launch\"\n[[step]]\nkind = \"expect_stdout\"\n"), invalid, "'regex'");
    check_invalid(with_steps("[[step]]\nkind = \"launch\"\n[[step]]\nkind = \"expect_stdout\"\nregex = \"x\"\nwithin_s = 0\n"), invalid, "within_s");
    check_invalid(with_steps("[[step]]\nkind = \"launch\"\n[[step]]\nkind = \"expect_stdout\"\nregex = \"x\"\nwithin_s = \"5\"\n"), invalid, "within_s");
    check_invalid(with_steps("[[step]]\nkind = \"launch\"\n[[step]]\nkind = \"expect_stdout\"\nregex = \"(\"\n"), cml::ids::kRegexInvalid);
    check_invalid(with_steps("[[step]]\nkind = \"launch\"\n[[step]]\nkind = \"expect_module_enabled\"\n"), invalid, "'module'");
    check_invalid(with_steps("[[step]]\nkind = \"edit\"\n"), invalid, "'patch'");
    check_invalid(with_steps("[[step]]\nkind = \"edit\"\npatch = \"../outside.edit\"\n"), invalid, "relative");
    check_invalid(with_steps("[[step]]\nkind = \"edit\"\npatch = \"C:\\\\abs.edit\"\n"), invalid, "relative");
    check_invalid(with_steps("[[step]]\nkind = \"launch\"\n[[step]]\nkind = \"expect_diag\"\nid = \"3101\"\n"), invalid, "CML3101");
    check_invalid(with_steps("[[step]]\nkind = \"launch\"\n[[step]]\nkind = \"expect_diag\"\n"), invalid, "'id'");
    check_invalid("name = \"x\"\n[[step\n", cml::ids::kScenarioSyntax);
}

TEST_CASE("scenario: an edit may precede the launch") {
    cml::Result<Scenario> r = parse_scenario(with_steps("[[step]]\nkind = \"edit\"\npatch = \"a.edit\"\n[[step]]\nkind = \"launch\"\n"));
    CHECK(r.has_value());
}

TEST_CASE("scenario: the real basic.toml in the repository parses") {
#ifdef CML_HARNESS_SCENARIOS_DIR
    cml::Result<Scenario> r = load_scenario(std::string(CML_HARNESS_SCENARIOS_DIR) + "/basic.toml");
    REQUIRE_MESSAGE(r.has_value(), (r.has_value() ? "" : r.error().format()));
    CHECK(r->name == "basic");
    CHECK(r->fixture == "basic");
    bool has_reload = false;
    for (const Step& s : r->steps) {
        has_reload = has_reload || s.kind == StepKind::Reload;
    }
    CHECK(has_reload);
    cml::Result<Scenario> missing = load_scenario(std::string(CML_HARNESS_SCENARIOS_DIR) + "/does_not_exist.toml");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == cml::ids::kHarnessUsage);
#endif
}
