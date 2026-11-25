// WP-43: the new keys of the restart, revert and expect_diag steps, and the checked-in scenarios restart_inject and revert_inject,
// which must take every stage of the groups "restart" and "revert" (cml/ipc/fault_stages.h) through a failure exactly once.
#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "cml/ipc/control_ids.h"
#include "cml/ipc/fault_stages.h"
#include "doctest.h"
#include "scenario.h"

using namespace cmltest;

namespace {

const std::string kHead = "name = \"x\"\nfixture = \"f\"\n[[step]]\nkind = \"launch\"\n";

}  // namespace

TEST_CASE("scenario (WP-43): a restart step takes hold_new, hold_ms, expect_restarting, expect_error(_text), expect_warning, expect_reapply_error, expect_not_enabled") {
    cml::Result<Scenario> r = parse_scenario(kHead + R"(
[[step]]
kind = "restart"
hold_new = true
hold_ms = 2000
expect_restarting = false
expect_warning = "CML5974"
expect_result_key = "patched"
expect_result_value = 1
[[step]]
kind = "restart"
expect_error = "CML5804"
expect_error_text = "CML4657"
[[step]]
kind = "restart"
expect_warning = "CML5810"
expect_reapply_error = "CML5977"
expect_not_enabled = "app.exe"
[[step]]
kind = "revert"
scope = "all"
expect_error = "CML5851"
expect_error_text = "CML4659"
[[step]]
kind = "expect_diag"
id = "CML5855"
expect_text = "CML4660"
)");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    REQUIRE(r->steps.size() == 6);
    const Step& held = r->steps[1];
    CHECK(held.hold_new);
    CHECK(held.hold_ms == 2000);
    CHECK_FALSE(held.expect_restarting);
    CHECK(held.expect_warning == "CML5974");
    CHECK(held.describe().find("held") != std::string::npos);
    CHECK(held.describe().find("expecting the warning CML5974") != std::string::npos);
    CHECK(r->steps[2].expect_error == "CML5804");
    CHECK(r->steps[2].expect_error_text == "CML4657");
    CHECK_FALSE(r->steps[2].hold_new);
    CHECK(r->steps[2].hold_ms == 10000);
    CHECK(r->steps[2].expect_restarting);
    CHECK(r->steps[3].expect_reapply_error == "CML5977");
    CHECK(r->steps[3].expect_not_enabled == "app.exe");
    CHECK(r->steps[4].expect_error_text == "CML4659");
    CHECK(r->steps[5].expect_error_text == "CML4660");
    CHECK(r->steps[5].describe().find("mentioning CML4660") != std::string::npos);
}

TEST_CASE("scenario (WP-43): bad restart, revert and expect_diag keys are CML5411") {
    const auto invalid = [&](const std::string& steps) {
        CAPTURE(steps);
        cml::Result<Scenario> r = parse_scenario(kHead + steps);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == cml::ids::kScenarioInvalid);
    };
    invalid("[[step]]\nkind = \"restart\"\nhold_new = 1\n");
    invalid("[[step]]\nkind = \"restart\"\nhold_ms = 100\n");                     // needs hold_new
    invalid("[[step]]\nkind = \"restart\"\nhold_new = true\nhold_ms = 0\n");
    invalid("[[step]]\nkind = \"restart\"\nhold_new = true\nhold_ms = 60001\n");
    invalid("[[step]]\nkind = \"restart\"\nexpect_restarting = false\n");         // needs hold_new
    invalid("[[step]]\nkind = \"restart\"\nexpect_error = \"5804\"\n");
    invalid("[[step]]\nkind = \"restart\"\nexpect_warning = \"CML58\"\n");
    invalid("[[step]]\nkind = \"restart\"\nexpect_reapply_error = 5977\n");
    invalid("[[step]]\nkind = \"restart\"\nexpect_error_text = \"x\"\n");          // needs expect_error
    invalid("[[step]]\nkind = \"restart\"\nexpect_not_enabled = \"\"\n");
    invalid("[[step]]\nkind = \"restart\"\nexpect_error = \"CML5804\"\nexpect_warning = \"CML5810\"\n");  // a refusal has no result
    invalid("[[step]]\nkind = \"restart\"\nwhat = 1\n");
    invalid("[[step]]\nkind = \"revert\"\nexpect_error_text = \"CML4659\"\n");     // needs expect_error
    invalid("[[step]]\nkind = \"expect_diag\"\nid = \"CML5855\"\nexpect_text = \"\"\n");
}

#ifdef CML_HARNESS_SCENARIOS_DIR
namespace {

namespace fs = std::filesystem;
namespace f = cml::ipc::fault;

// The id a step after an armed stage expects, in any of the keys that can carry it.
bool step_names(const Step& s, const std::string& id) {
    return s.expect_error == id || s.expect_error_text == id || s.expect_warning == id || s.expect_reapply_error == id;
}

// Every stage of `group` is armed exactly once, and the first step of `kind` after it expects the stage's own id (in the refusal, its
// text, a warning, the reapply error) or, for the stages whose failure the step does not see, the id in `outcome_of`.
void check_covers(const char* file, const char* fixture, const char* group, StepKind kind, const std::map<std::string, std::string>& outcome_of) {
    cml::Result<Scenario> r = load_scenario(fs::path(CML_HARNESS_SCENARIOS_DIR) / file);
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    CHECK(r->fixture == fixture);
    const fs::path fixture_dir = fs::path(CML_FIXTURE_BASIC_DIR).parent_path() / fixture;
    std::set<std::string> covered;
    std::string armed;
    for (std::size_t i = 0; i < r->steps.size(); ++i) {
        const Step& s = r->steps[i];
        if (s.kind == StepKind::Edit) {
            CHECK_MESSAGE(fs::exists(fixture_dir / s.patch), s.patch);
        }
        if (s.kind == StepKind::InjectFailure && s.expect_error.empty() && s.fault_stage != "none") {
            const f::StageInfo* info = f::find_stage(s.fault_stage);
            REQUIRE_MESSAGE(info != nullptr, s.fault_stage);
            CHECK_MESSAGE(std::string(info->group) == group, s.fault_stage);
            // A stage armed and then disarmed ("none") is the disarm check, not a covered stage.
            const bool disarmed_next = i + 1 < r->steps.size() && r->steps[i + 1].kind == StepKind::InjectFailure && r->steps[i + 1].fault_stage == "none";
            armed = disarmed_next ? std::string() : s.fault_stage;
            continue;
        }
        if (!armed.empty() && (s.kind == kind || (s.kind == StepKind::ExpectDiag && outcome_of.count(armed) != 0))) {
            const std::string id = "CML" + std::to_string(f::find_stage(armed)->diag_id);
            const auto other = outcome_of.find(armed);
            const bool named = step_names(s, id) || (other != outcome_of.end() && (step_names(s, other->second) || s.id == other->second));
            if (s.kind == kind && other != outcome_of.end() && !step_names(s, id)) {
                continue;  // this stage's failure is checked by the expect_diag that follows the step
            }
            const std::string where = armed + " is not checked by the step on line " + std::to_string(s.line);
            CHECK_MESSAGE(named, where);
            if (s.kind == StepKind::ExpectDiag) {
                CHECK_MESSAGE(s.expect_error_text == id, armed);
            }
            CHECK_MESSAGE(covered.insert(armed).second, armed);  // each stage fails exactly once
            armed.clear();
        }
    }
    for (const f::StageInfo& info : f::kStages) {
        if (std::string(info.group) == group) {
            CHECK_MESSAGE(covered.count(info.name) == 1, info.name);
        }
    }
}

}  // namespace

TEST_CASE("the checked-in restart_inject scenario takes every restart stage through a failure once, each checked by its own id") {
    check_covers("restart_inject.toml", "hot_restart", "restart", StepKind::Restart, {});
}

TEST_CASE("the checked-in revert_inject scenario takes every revert stage through a failure once, each checked by its own id") {
    // revert-uncommit does not fail the revert: the warning CML5855 that names CML4660 is checked by an expect_diag.
    check_covers("revert_inject.toml", "revert", "revert", StepKind::Revert, {{"revert-uncommit", "CML5855"}});
}
#endif
