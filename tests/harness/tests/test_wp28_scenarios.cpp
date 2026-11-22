// WP-28: the `inject_failure` step and the checked-in `inject_stages` scenario.
#include <filesystem>
#include <set>
#include <string>

#include "cml/ipc/control.h"
#include "cml/ipc/control_ids.h"
#include "cml/ipc/fault_stages.h"
#include "doctest.h"
#include "scenario.h"

using namespace cmltest;

TEST_CASE("scenario: an inject_failure step takes stage, count and expect_error and needs the fault-injection capability") {
    const char* text = R"(
name = "x"
fixture = "inject"
[[step]]
kind = "launch"
[[step]]
kind = "inject_failure"
stage = "link"
[[step]]
kind = "inject_failure"
stage = "hook-install"
count = 3
[[step]]
kind = "inject_failure"
stage = "nope"
expect_error = "CML5960"
[[step]]
kind = "inject_failure"
stage = "none"
)";
    cml::Result<Scenario> r = parse_scenario(text);
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    REQUIRE(r->steps.size() == 5);
    CHECK(r->steps[1].kind == StepKind::InjectFailure);
    CHECK(r->steps[1].fault_stage == "link");
    CHECK(r->steps[1].fault_count == 1);
    CHECK(r->steps[2].fault_count == 3);
    CHECK(r->steps[3].expect_error == "CML5960");
    CHECK(std::string(r->steps[1].required_capability()) == cml::ipc::capability::kFaultInjection);
    CHECK(std::string(r->steps[1].required_capability()) == "fault-injection");
    CHECK(r->steps[1].describe() == "inject_failure link");
    CHECK(r->steps[2].describe() == "inject_failure hook-install x3");
    CHECK(r->steps[3].describe() == "inject_failure nope expecting the refusal CML5960");
}

TEST_CASE("scenario: bad inject_failure keys are CML5411") {
    const std::string head = "name = \"x\"\nfixture = \"f\"\n[[step]]\nkind = \"launch\"\n";
    const auto invalid = [&](const std::string& steps) {
        cml::Result<Scenario> r = parse_scenario(head + steps);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == cml::ids::kScenarioInvalid);
    };
    invalid("[[step]]\nkind = \"inject_failure\"\n");
    invalid("[[step]]\nkind = \"inject_failure\"\nstage = 3\n");
    invalid("[[step]]\nkind = \"inject_failure\"\nstage = \"\"\n");
    invalid("[[step]]\nkind = \"inject_failure\"\nstage = \"Link\"\n");
    invalid("[[step]]\nkind = \"inject_failure\"\nstage = \"link\"\ncount = 0\n");
    invalid("[[step]]\nkind = \"inject_failure\"\nstage = \"link\"\ncount = 1001\n");
    invalid("[[step]]\nkind = \"inject_failure\"\nstage = \"link\"\nexpect_error = \"5960\"\n");
    invalid("[[step]]\nkind = \"inject_failure\"\nstage = \"link\"\nwhat = 1\n");
}

#ifdef CML_HARNESS_SCENARIOS_DIR
TEST_CASE("the checked-in inject_stages scenario covers every stage once, expects each stage's own id, and its edits are in the fixture") {
    namespace fs = std::filesystem;
    namespace f = cml::ipc::fault;
    cml::Result<Scenario> r = load_scenario(fs::path(CML_HARNESS_SCENARIOS_DIR) / "inject_stages.toml");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    CHECK(r->fixture == "inject");
    const fs::path fixture = fs::path(CML_FIXTURE_BASIC_DIR).parent_path() / "inject";

    std::set<std::string> covered;
    const Step* armed = nullptr;
    int edits = 0;
    for (const Step& s : r->steps) {
        if (s.kind == StepKind::Edit) {
            ++edits;
            CHECK_MESSAGE(fs::exists(fixture / s.patch), s.patch);
        }
        if (s.kind == StepKind::InjectFailure) {
            armed = s.expect_error.empty() && s.fault_stage != "none" ? &s : nullptr;
            if (armed != nullptr) {
                REQUIRE_MESSAGE(f::find_stage(s.fault_stage) != nullptr, s.fault_stage);
            }
        }
        if (s.kind == StepKind::Reload && !s.expect_error.empty()) {
            // The failing reload names the id of the stage that was armed just before it.
            REQUIRE(armed != nullptr);
            CHECK(s.expect_error == "CML" + std::to_string(f::find_stage(armed->fault_stage)->diag_id));
            CHECK(covered.insert(armed->fault_stage).second);  // each stage fails exactly once
            armed = nullptr;
        }
    }
    for (const f::StageInfo& info : f::kStages) {
        CHECK_MESSAGE(covered.count(info.name) == 1, info.name);
    }
    CHECK(edits == static_cast<int>(f::kStageCount) + 1);  // one per stage, one for the disarm check
}
#endif
