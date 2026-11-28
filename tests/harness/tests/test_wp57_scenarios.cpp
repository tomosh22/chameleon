// WP-57: the checked-in scenario inlined_owner_o2, which must show what the work package promises: an own static next to a
// std::sort with a lambda reloads in every cell while its function is unchanged (both reloads must apply, no refusal is
// allowed), the static keeps counting across both reloads, and the second reload is the stacked one (compared with patch 1).
#include <filesystem>
#include <string>

#include "doctest.h"
#include "scenario.h"

using namespace cmltest;

#ifdef CML_HARNESS_SCENARIOS_DIR
TEST_CASE("scenario inlined_owner_o2 (WP-57): two edits of another function of the file apply in every cell and the static keeps counting") {
    cml::Result<Scenario> r = load_scenario(std::filesystem::path(CML_HARNESS_SCENARIOS_DIR) / "inlined_owner_o2.toml");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    const Scenario& s = *r;
    CHECK(s.fixture == "inlined_owner_o2");
    std::size_t reloads = 0;
    std::size_t continued = 0;
    std::string edits;
    for (const Step& step : s.steps) {
        if (step.kind == StepKind::Reload) {
            ++reloads;
            CHECK(step.allow_error.empty());
            CHECK(step.expect_error.empty());
            CHECK(step.result_key == "patched");
            CHECK(step.result_value == 1);
        }
        if (step.kind == StepKind::ExpectMonotonic) {
            CHECK(step.key == "count");
            CHECK(step.since_reload > 0);
            ++continued;
        }
        if (step.kind == StepKind::Edit) {
            edits += step.patch + ";";
        }
    }
    CHECK(reloads == 2);
    CHECK(continued == 2);
    CHECK(edits == "edits/mark_2.edit;edits/stacked/mark_3.edit;");
}
#endif
