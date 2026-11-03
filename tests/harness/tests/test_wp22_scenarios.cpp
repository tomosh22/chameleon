// WP-22: the `revert` step and the checked-in `revert` scenario.
#include <filesystem>
#include <string>

#include "cml/ipc/control.h"
#include "cml/ipc/control_ids.h"
#include "doctest.h"
#include "scenario.h"

using namespace cmltest;

TEST_CASE("scenario: a revert step takes scope, within_s and expect_error and needs the revert capability") {
    const char* text = R"(
name = "x"
fixture = "revert"
[[step]]
kind = "launch"
[[step]]
kind = "revert"
[[step]]
kind = "revert"
scope = "all"
within_s = 20
[[step]]
kind = "revert"
scope = "last"
expect_error = "CML5850"
)";
    cml::Result<Scenario> r = parse_scenario(text);
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    REQUIRE(r->steps.size() == 4);
    CHECK(r->steps[1].kind == StepKind::Revert);
    CHECK(r->steps[1].revert_scope == "last");  // the default
    CHECK(r->steps[2].revert_scope == "all");
    CHECK(r->steps[2].within_s == 20);
    CHECK(r->steps[3].expect_error == "CML5850");
    CHECK(std::string(r->steps[1].required_capability()) == cml::ipc::capability::kRevert);
    CHECK(std::string(r->steps[1].required_capability()) == "revert");
    CHECK(r->steps[2].describe() == "revert all within 20 s");
    CHECK(r->steps[3].describe() == "revert last within 30 s expecting the refusal CML5850");
}

TEST_CASE("scenario: bad revert keys are CML5411") {
    const std::string head = "name = \"x\"\nfixture = \"f\"\n[[step]]\nkind = \"launch\"\n";
    const auto invalid = [&](const std::string& steps) {
        cml::Result<Scenario> r = parse_scenario(head + steps);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == cml::ids::kScenarioInvalid);
    };
    invalid("[[step]]\nkind = \"revert\"\nscope = \"everything\"\n");
    invalid("[[step]]\nkind = \"revert\"\nscope = 1\n");
    invalid("[[step]]\nkind = \"revert\"\nexpect_error = \"5850\"\n");
    invalid("[[step]]\nkind = \"revert\"\nwhat = \"x\"\n");
    invalid("[[step]]\nkind = \"revert\"\nwithin_s = 0\n");
}

#ifdef CML_HARNESS_SCENARIOS_DIR
TEST_CASE("the checked-in revert scenario parses, reverts last then all, and its edits are in the fixture") {
    namespace fs = std::filesystem;
    cml::Result<Scenario> r = load_scenario(fs::path(CML_HARNESS_SCENARIOS_DIR) / "revert.toml");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    CHECK(r->fixture == "revert");
    std::string scopes;
    int reloads = 0;
    for (const Step& s : r->steps) {
        if (s.kind == StepKind::Revert) {
            scopes += s.revert_scope + ";";
        }
        reloads += s.kind == StepKind::Reload ? 1 : 0;
        if (s.kind == StepKind::Edit) {
            CHECK_MESSAGE(fs::exists(fs::path(CML_FIXTURE_BASIC_DIR).parent_path() / r->fixture / s.patch), s.patch);
        }
    }
    CHECK(scopes == "last;all;all;");
    CHECK(reloads == 3);
}
#endif
