// WP-60 (R-57): the checked-in scenario planner_pairing_o2 must show what the work package promises: help_6 (an own static next
// to an inlined `static` helper, the caller unchanged) applies in every cell, and r57_7 (the inlined helper deleted, its static's
// name declared in the caller) is refused with CML3207 under clang-cl (review of 8609b97: under cl either outcome, never the
// wrong pairing). And the reload step's `compiler` key: an expect_error for one compiler only.
#include <filesystem>
#include <string>

#include "doctest.h"
#include "scenario.h"

using namespace cmltest;

#ifdef CML_HARNESS_SCENARIOS_DIR
TEST_CASE("scenario planner_pairing_o2 (WP-60): help_6 applies in every cell, r57_7 is refused under clang-cl") {
    cml::Result<Scenario> r = load_scenario(std::filesystem::path(CML_HARNESS_SCENARIOS_DIR) / "planner_pairing_o2.toml");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    const Scenario& s = *r;
    std::string last_edit;
    bool help = false;
    bool r57 = false;
    for (std::size_t i = 0; i < s.steps.size(); ++i) {
        const Step& step = s.steps[i];
        if (step.kind == StepKind::Edit) {
            last_edit = step.patch;
        }
        if (step.kind != StepKind::Reload) {
            continue;
        }
        if (last_edit == "edits/help_6.edit") {
            help = true;
            CHECK(step.allow_error.empty());
            CHECK(step.expect_error.empty());
            REQUIRE(i + 1 < s.steps.size());
            CHECK(s.steps[i + 1].kind == StepKind::ExpectReloadResult);
            CHECK(s.steps[i + 1].key == "patched");
            CHECK(s.steps[i + 1].result_value == 1);
        }
        if (last_edit == "edits/r57_7.edit") {
            r57 = true;
            CHECK(step.expect_error == "CML3207");
            CHECK(step.compiler == "clang-cl");
        }
    }
    CHECK(help);
    CHECK(r57);
}
#endif

TEST_CASE("scenario: a reload step's `compiler` names the compiler its expect_error is for") {
    const char* const head = "name = \"x\"\nfixture = \"basic\"\n[[step]]\nkind = \"launch\"\n[[step]]\nkind = \"reload\"\n";
    cml::Result<Scenario> ok = parse_scenario(std::string(head) + "expect_error = \"CML3207\"\ncompiler = \"clang-cl\"\n");
    REQUIRE_MESSAGE(ok.has_value(), ok.error().format());
    CHECK(ok->steps[1].compiler == "clang-cl");
    CHECK(ok->steps[1].expect_error == "CML3207");
    CHECK_FALSE(parse_scenario(std::string(head) + "compiler = \"clang-cl\"\n").has_value());                              // needs expect_error
    CHECK_FALSE(parse_scenario(std::string(head) + "expect_error = \"CML3207\"\ncompiler = \"gcc\"\n").has_value());      // cl or clang-cl
    CHECK_FALSE(parse_scenario(std::string(head) + "allow_error = \"CML3207\"\ncompiler = \"cl\"\n").has_value());        // not with allow_error
}
