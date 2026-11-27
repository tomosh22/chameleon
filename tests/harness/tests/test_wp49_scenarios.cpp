// WP-49: the `compiler` key of expect_reload_result (an expectation for one compiler only) and the checked-in scenario
// jump_tables (fixture jump_tables), which must show: one hook per one-function edit under cl although the unit's other
// functions hold jump tables, a hooked jump-table-only change with the program printing the new values, and the stacked
// edit compared with the earlier patch's record.
#include <filesystem>
#include <string>
#include <vector>

#include "doctest.h"
#include "scenario.h"

using namespace cmltest;

namespace {

cml::Result<Scenario> parse(const std::string& steps) {
    return parse_scenario("name = \"x\"\nfixture = \"basic\"\n[[step]]\nkind = \"launch\"\n[[step]]\nkind = \"reload\"\n" + steps);
}

}  // namespace

TEST_CASE("scenario key (WP-49): expect_reload_result takes compiler = \"cl\" or \"clang-cl\"") {
    {
        cml::Result<Scenario> r = parse("[[step]]\nkind = \"expect_reload_result\"\nkey = \"hooks_installed\"\nvalue = 1\ncompiler = \"cl\"\n");
        REQUIRE_MESSAGE(r.has_value(), r.error().format());
        REQUIRE(r->steps.size() == 3);
        CHECK(r->steps[2].compiler == "cl");
        CHECK(r->steps[2].describe() == "expect_reload_result hooks_installed=1 (cl only)");
    }
    {
        cml::Result<Scenario> r = parse("[[step]]\nkind = \"expect_reload_result\"\nkey = \"hooks_installed\"\nvalue = 3\ncompiler = \"clang-cl\"\n");
        REQUIRE_MESSAGE(r.has_value(), r.error().format());
        CHECK(r->steps[2].compiler == "clang-cl");
    }
    {
        cml::Result<Scenario> r = parse("[[step]]\nkind = \"expect_reload_result\"\nkey = \"hooks_installed\"\nvalue = 3\n");
        REQUIRE_MESSAGE(r.has_value(), r.error().format());
        CHECK(r->steps[2].compiler.empty());
        CHECK(r->steps[2].describe() == "expect_reload_result hooks_installed=3");
    }
    for (const char* bad : {"compiler = \"gcc\"\n", "compiler = \"\"\n", "compiler = 1\n"}) {
        CAPTURE(bad);
        cml::Result<Scenario> r = parse(std::string("[[step]]\nkind = \"expect_reload_result\"\nkey = \"hooks_installed\"\nvalue = 1\n") + bad);
        CHECK_FALSE(r.has_value());
    }
    // Only expect_reload_result takes it.
    CHECK(parse("[[step]]\nkind = \"reload\"\n").has_value());
    CHECK_FALSE(parse("[[step]]\nkind = \"reload\"\ncompiler = \"cl\"\n").has_value());
}

TEST_CASE("compiler_family (WP-49): the cell's compiler as cl, clang-cl or neither") {
    CHECK(compiler_family("cl") == "cl");
    CHECK(compiler_family("msvc") == "cl");
    CHECK(compiler_family("CL.EXE") == "cl");
    CHECK(compiler_family("C:\\VS\\VC\\Tools\\MSVC\\14.44\\bin\\Hostx64\\x64\\cl.exe") == "cl");
    CHECK(compiler_family("clang-cl") == "clang-cl");
    CHECK(compiler_family("C:/Program Files/LLVM/bin/clang-cl.exe") == "clang-cl");
    CHECK(compiler_family("Clang-CL.exe") == "clang-cl");
    CHECK(compiler_family("clang").empty());
    CHECK(compiler_family("gcc").empty());
    CHECK(compiler_family("").empty());
    CHECK(compiler_family("C:/tools/icx.exe").empty());
}

#ifdef CML_HARNESS_SCENARIOS_DIR
TEST_CASE("scenario jump_tables (WP-49): one hook per edit under cl, the jump-table-only change is hooked and shows") {
    cml::Result<Scenario> loaded = load_scenario(std::filesystem::path(CML_HARNESS_SCENARIOS_DIR) / "jump_tables.toml");
    REQUIRE_MESSAGE(loaded.has_value(), loaded.error().format());
    const Scenario& s = *loaded;
    CHECK(s.fixture == "jump_tables");
    std::vector<std::string> edits;
    std::vector<long long> cl_hooks;
    std::vector<long long> clang_hooks;
    for (const Step& step : s.steps) {
        if (step.kind == StepKind::Edit) {
            edits.push_back(step.patch);
        }
        if (step.kind == StepKind::ExpectReloadResult && step.key == "hooks_installed") {
            (step.compiler == "cl" ? cl_hooks : clang_hooks).push_back(step.result_value);
            CHECK(!step.compiler.empty());
        }
    }
    CHECK(edits == std::vector<std::string>{"edits/value_2.edit", "edits/stacked/swap_cases.edit", "edits/stacked/value_3.edit"});
    CHECK(cl_hooks == std::vector<long long>{1, 1, 1});
    CHECK(clang_hooks == std::vector<long long>{3, 2, 3});
    // After the table-only edit the program must print the exchanged cases (a dropped hook would keep d1=50).
    bool swapped_shown = false;
    for (const Step& step : s.steps) {
        swapped_shown = swapped_shown || (step.kind == StepKind::ExpectStdout && step.regex.find("d1=3 d2=50") != std::string::npos);
    }
    CHECK(swapped_shown);
}
#endif
