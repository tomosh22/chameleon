// WP-40: a reload step that names alternative refusals (`expect_error = "CML3207|CML3600"`) or allows a refusal
// (`allow_error`): the planner pairs under one compiler and refuses under the other, and the scenario format has no step
// that depends on the compiler. Also: every checked-in WP-40 scenario parses.
#include <string>

#include "cml/ipc/control_ids.h"
#include "doctest.h"
#include "scenario.h"

using namespace cmltest;

TEST_CASE("scenario: a reload step's expect_error may name alternatives, allow_error lets it succeed or be refused") {
    const std::string head = "name = \"x\"\nfixture = \"f\"\n[[step]]\nkind = \"launch\"\n[[step]]\nkind = \"reload\"\n";
    cml::Result<Scenario> one = parse_scenario(head + "expect_error = \"CML3207\"\n");
    REQUIRE_MESSAGE(one.has_value(), one.error().format());
    CHECK(one->steps[1].expect_error == "CML3207");
    cml::Result<Scenario> two = parse_scenario(head + "expect_error = \"CML3207|CML3600\"\n");
    REQUIRE_MESSAGE(two.has_value(), two.error().format());
    CHECK(two->steps[1].expect_error == "CML3207|CML3600");
    CHECK(two->steps[1].describe() == "reload within 60 s expecting the refusal CML3207|CML3600");
    cml::Result<Scenario> allow = parse_scenario(head + "allow_error = \"CML3207|CML3601\"\n");
    REQUIRE_MESSAGE(allow.has_value(), allow.error().format());
    CHECK(allow->steps[1].allow_error == "CML3207|CML3601");
    CHECK(allow->steps[1].expect_error.empty());
    CHECK(allow->steps[1].describe() == "reload within 60 s allowing the refusal CML3207|CML3601");
    for (const char* bad : {"expect_error = \"CML3207|\"\n", "expect_error = \"CML3207,CML3600\"\n", "expect_error = \"CML320\"\n", "allow_error = \"\"\n",
                            "allow_error = \"CML3207|CML36\"\n", "allow_error = 3207\n", "allow_error = \"CML3207\"\nexpect_error = \"CML3207\"\n",
                            "allow_error = \"CML3207\"\nexpect_result_key = \"patched\"\nexpect_result_value = 1\n"}) {
        CAPTURE(bad);
        cml::Result<Scenario> r = parse_scenario(head + bad);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == cml::ids::kScenarioInvalid);
    }
}

TEST_CASE("scenario: the WP-40 scenarios parse") {
    for (const char* name : {"planner_pairing.toml", "planner_pairing_o2.toml"}) {
        CAPTURE(name);
        cml::Result<Scenario> r = load_scenario(std::string(CML_HARNESS_SCENARIOS_DIR) + "/" + name);
        REQUIRE_MESSAGE(r.has_value(), r.error().format());
        CHECK(r->steps.size() > 5);
    }
}
