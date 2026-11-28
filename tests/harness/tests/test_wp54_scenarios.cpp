// WP-54: the checked-in scenario cross_module_atomic, which must show what the work package promises: a header edit over a translation
// unit of the exe and one of its plugin DLL, the DLL's refused (CML3400), applies nothing to either module (no patch_applied, the old
// values of both), the application keeps running, and the same edit without the refused part reloads both modules, holding one COFF
// object at a time.
#include <filesystem>
#include <string>

#include "doctest.h"
#include "scenario.h"

using namespace cmltest;

#ifdef CML_HARNESS_SCENARIOS_DIR
TEST_CASE("scenario cross_module_atomic (WP-54): the DLL's compiland is refused, neither module is applied, then the same edit reloads both") {
    cml::Result<Scenario> r = load_scenario(std::filesystem::path(CML_HARNESS_SCENARIOS_DIR) / "cross_module_atomic.toml");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    const Scenario& s = *r;
    CHECK(s.fixture == "cross_module_atomic");
    const auto next = [&](std::size_t from, StepKind kind) {
        for (std::size_t i = from; i < s.steps.size(); ++i) {
            if (s.steps[i].kind == kind) {
                return i;
            }
        }
        return s.steps.size();
    };
    // Both modules are enabled before the edit.
    const std::size_t exe = next(0, StepKind::ExpectModuleEnabled);
    REQUIRE(exe < s.steps.size());
    CHECK(s.steps[exe].module == "fixture_cross_module_atomic.exe");
    const std::size_t dll = next(exe + 1, StepKind::ExpectModuleEnabled);
    REQUIRE(dll < s.steps.size());
    CHECK(s.steps[dll].module == "fixture_cross_module_atomic_plugin.dll");

    const std::size_t edit1 = next(dll, StepKind::Edit);
    REQUIRE(edit1 < s.steps.size());
    CHECK(s.steps[edit1].patch == "edits/scale_2_tls.edit");
    const std::size_t refused = next(edit1, StepKind::Reload);
    REQUIRE(refused < s.steps.size());
    CHECK(s.steps[refused].expect_error == "CML3400");
    const std::size_t none = next(refused, StepKind::ExpectEvent);
    REQUIRE(none < s.steps.size());
    CHECK(s.steps[none].event == "patch_applied");
    CHECK(s.steps[none].event_absent);
    const std::size_t old_values = next(none, StepKind::ExpectStdout);
    REQUIRE(old_values < s.steps.size());
    CHECK(s.steps[old_values].regex.find("exe=11 plugin=101") != std::string::npos);

    const std::size_t edit2 = next(old_values, StepKind::Edit);
    REQUIRE(edit2 < s.steps.size());
    CHECK(s.steps[edit2].patch == "edits/stacked/no_tls.edit");
    const std::size_t reloaded = next(edit2, StepKind::Reload);
    REQUIRE(reloaded < s.steps.size());
    CHECK(s.steps[reloaded].expect_error.empty());
    CHECK(s.steps[reloaded].result_key == "modules_patched");
    CHECK(s.steps[reloaded].result_value == 2);
    bool peak = false;
    for (std::size_t i = reloaded; i < s.steps.size(); ++i) {
        peak = peak || (s.steps[i].kind == StepKind::ExpectReloadResult && s.steps[i].key == "objects_held_peak" && s.steps[i].result_value == 1);
    }
    CHECK(peak);
    const std::size_t new_values = next(reloaded, StepKind::ExpectStdout);
    REQUIRE(new_values < s.steps.size());
    CHECK(s.steps[new_values].regex.find("exe=22 plugin=202") != std::string::npos);
    CHECK(next(new_values, StepKind::ExpectMonotonic) < s.steps.size());
}
#endif
