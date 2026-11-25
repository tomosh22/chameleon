// WP-44: the checked-in scenarios crash_inject, crash_reexecute_inject and watch_inject, which must take every stage of the groups
// "exception", "exception-action" and "watch" (cml/ipc/fault_stages.h) through a failure, each checked by its own id; and the step
// expect_pending_exceptions, which makes the only held thread the current exception.
#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "cml/ipc/fault_stages.h"
#include "doctest.h"
#include "scenario.h"

using namespace cmltest;

#ifdef CML_HARNESS_SCENARIOS_DIR
namespace {

namespace fs = std::filesystem;
namespace f = cml::ipc::fault;

// Whether step `s` checks the failure of `stage` (whose id is `id`): an expect_diag of the id, an exception_action refused with it, or,
// for exception-hold (the agent passes the fault on and the broker never sees it), the application's own handler reporting the fault.
// A stage that its batch does not reach is checked by the CML5983 warning that names it.
bool checks(const Step& s, const std::string& stage, const std::string& id) {
    if (s.kind == StepKind::ExpectDiag) {
        return s.id == id || (s.id == "CML" + std::to_string(f::ids::kWatchDisarmed) && s.expect_error_text == stage);
    }
    if (s.kind == StepKind::ExceptionAction) {
        return s.expect_error == id;
    }
    return stage == "exception-hold" && s.kind == StepKind::ExpectStdout && s.regex.find("PASSED_ON") != std::string::npos;
}

// Each armed stage of `groups` is checked by one of the next `window` steps; returns the stages armed. Edits must exist.
std::set<std::string> check_scenario(const char* file, const char* fixture, const std::set<std::string>& groups, std::size_t window = 8) {
    cml::Result<Scenario> r = load_scenario(fs::path(CML_HARNESS_SCENARIOS_DIR) / file);
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    CHECK(r->fixture == fixture);
    const fs::path fixture_dir = fs::path(CML_FIXTURE_BASIC_DIR).parent_path() / fixture;
    std::set<std::string> armed_stages;
    for (std::size_t i = 0; i < r->steps.size(); ++i) {
        const Step& s = r->steps[i];
        if (s.kind == StepKind::Edit) {
            CHECK_MESSAGE(fs::exists(fixture_dir / s.patch), s.patch);
        }
        if (s.kind != StepKind::InjectFailure || s.fault_stage == "none" || !s.expect_error.empty()) {
            continue;
        }
        const f::StageInfo* info = f::find_stage(s.fault_stage);
        REQUIRE_MESSAGE(info != nullptr, s.fault_stage);
        CHECK_MESSAGE(groups.count(info->group) == 1, s.fault_stage);
        armed_stages.insert(s.fault_stage);
        const std::string id = "CML" + std::to_string(info->diag_id);
        bool checked = false;
        for (std::size_t j = i + 1; j < r->steps.size() && j <= i + window && !checked; ++j) {
            checked = checks(r->steps[j], s.fault_stage, id);
        }
        const std::string where = s.fault_stage + " (line " + std::to_string(s.line) + ") is not checked by its id in the steps after it";
        CHECK_MESSAGE(checked, where);
    }
    return armed_stages;
}

}  // namespace

TEST_CASE("the checked-in WP-44 scenarios take every crash-handler and watcher stage through a failure, each checked by its own id") {
    std::set<std::string> covered = check_scenario("crash_inject.toml", "crash", {"exception", "exception-action"});
    for (const std::string& s : check_scenario("crash_reexecute_inject.toml", "crash_reexecute", {"exception-action"})) {
        covered.insert(s);
    }
    for (const std::string& s : check_scenario("watch_inject.toml", "inject", {"watch"})) {
        covered.insert(s);
    }
    for (const f::StageInfo& info : f::kStages) {
        const std::string group = info.group;
        if (group == "exception" || group == "exception-action" || group == "watch") {
            CHECK_MESSAGE(covered.count(info.name) == 1, info.name);
        }
    }
    // The crash scenario runs the fixture with its own handler, so the fault exception-hold passes on does not end the process.
    cml::Result<Scenario> crash = load_scenario(fs::path(CML_HARNESS_SCENARIOS_DIR) / "crash_inject.toml");
    REQUIRE(crash.has_value());
    CHECK(crash->fixture_args == "--seh");
}
#endif
