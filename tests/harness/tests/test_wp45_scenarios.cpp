// WP-45: the checked-in scenarios plugin_reload and plugin_late_load (fixture plugin_reload) parse, and cover what the work package
// must show: an edit still in effect after FreeLibrary + LoadLibrary, a second edit that links afterwards, a hot-restart whose new
// process loads the plugin late, and a failed re-apply that is the diagnostic CML5600.
#include <filesystem>
#include <string>
#include <vector>

#include "doctest.h"
#include "scenario.h"

using namespace cmltest;

#ifdef CML_HARNESS_SCENARIOS_DIR
namespace {

namespace fs = std::filesystem;

Scenario load(const char* file) {
    cml::Result<Scenario> r = load_scenario(fs::path(CML_HARNESS_SCENARIOS_DIR) / file);
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    return *r;
}

std::size_t count(const Scenario& s, StepKind kind) {
    std::size_t n = 0;
    for (const Step& step : s.steps) {
        n += step.kind == kind ? 1 : 0;
    }
    return n;
}

// The index of the first step at or after `from` of this kind whose regex / id / patch contains `text`; s.steps.size() if none.
std::size_t find(const Scenario& s, std::size_t from, StepKind kind, const std::string& text) {
    for (std::size_t i = from; i < s.steps.size(); ++i) {
        const Step& step = s.steps[i];
        if (step.kind == kind && (step.regex.find(text) != std::string::npos || step.id.find(text) != std::string::npos ||
                                  step.patch.find(text) != std::string::npos || text.empty())) {
            return i;
        }
    }
    return s.steps.size();
}

}  // namespace

TEST_CASE("scenario plugin_reload (WP-45): the edit survives FreeLibrary + LoadLibrary, a second edit links, a failed re-apply is CML5600") {
    const Scenario s = load("plugin_reload.toml");
    CHECK(s.fixture == "plugin_reload");
    CHECK(count(s, StepKind::SignalFixture) == 4);  // load, free + load, free + load inside one poll (WP-48), free + load
    // F-42.1: after the second load (gen=2) the edited value, without a reload in between.
    const std::size_t edit2 = find(s, 0, StepKind::Edit, "value_2");
    const std::size_t reloaded = find(s, edit2, StepKind::ExpectStdout, "value=2 loaded=1 gen=2");
    REQUIRE(reloaded < s.steps.size());
    CHECK(find(s, edit2 + 2, StepKind::Reload, "") > reloaded);
    // F-42.2: the next edit reloads with a patch.
    const std::size_t edit3 = find(s, reloaded, StepKind::Edit, "value_3");
    REQUIRE(edit3 < s.steps.size());
    const std::size_t reload3 = find(s, edit3, StepKind::Reload, "");
    REQUIRE(reload3 < s.steps.size());
    CHECK(s.steps[reload3].result_key == "patched");
    CHECK(s.steps[reload3].result_value == 1);
    // The failure diagnostic, then the original code.
    const std::size_t diag = find(s, edit3, StepKind::ExpectDiag, "CML5600");
    REQUIRE(diag < s.steps.size());
    CHECK(s.steps[diag].expect_error_text == "fixture_plugin_reload_plugin.dll");
    CHECK(find(s, diag, StepKind::ExpectStdout, "value=1 loaded=1 gen=4") < s.steps.size());
}

TEST_CASE("scenario plugin_late_load (WP-45): a restart hands the plugin's edit over, it is re-applied when the plugin is loaded later") {
    const Scenario s = load("plugin_late_load.toml");
    CHECK(s.fixture == "plugin_reload");
    REQUIRE(count(s, StepKind::Restart) == 2);
    const std::size_t restart = find(s, 0, StepKind::Restart, "");
    CHECK(s.steps[restart].expect_not_enabled == "fixture_plugin_reload_plugin.dll");
    CHECK(s.steps[restart].expect_warning == "CML5809");
    const std::size_t load_late = find(s, restart, StepKind::SignalFixture, "");
    REQUIRE(load_late < s.steps.size());
    const std::size_t reapplied = find(s, load_late, StepKind::ExpectStdout, "value=2 loaded=1 gen=1 pid=\\d+ restarted=1");
    REQUIRE(reapplied < s.steps.size());
    CHECK(find(s, load_late, StepKind::Reload, "") > reapplied);  // no reload between the late load and the edited value
    const std::size_t diag = find(s, reapplied, StepKind::ExpectDiag, "CML5600");
    REQUIRE(diag < s.steps.size());
    CHECK(find(s, reapplied, StepKind::Restart, "") < diag);
}
#endif
