// WP-63: the checked-in scenario guard_bits, which must show what the work package promises: an inline function that keeps
// `static int calls = 0;` and gains a dynamic static reloads in every cell, without thread-safe statics (nts_1) and with
// them (ts_2): both reloads must apply (no refusal is allowed), and the constant-initialised statics keep counting across
// their reload.
#include <filesystem>
#include <string>

#include "doctest.h"
#include "scenario.h"

using namespace cmltest;

#ifdef CML_HARNESS_SCENARIOS_DIR
TEST_CASE("scenario guard_bits (WP-63): both edits apply in every cell and the constant-initialised statics keep counting") {
    cml::Result<Scenario> r = load_scenario(std::filesystem::path(CML_HARNESS_SCENARIOS_DIR) / "guard_bits.toml");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    const Scenario& s = *r;
    CHECK(s.fixture == "guard_bits");
    std::size_t reloads = 0;
    std::string continued;
    std::string edits;
    for (const Step& step : s.steps) {
        if (step.kind == StepKind::Reload) {
            ++reloads;
            CHECK(step.allow_error.empty());
            CHECK(step.expect_error.empty());
            CHECK(step.result_key == "patched");
            CHECK(step.result_value == 1);
        }
        if (step.kind == StepKind::ExpectMonotonic && step.since_reload > 0) {
            continued += step.key + ";";
        }
        if (step.kind == StepKind::Edit) {
            edits += step.patch + ";";
        }
    }
    CHECK(reloads == 2);
    CHECK(continued == "nts;ts;");
    CHECK(edits == "edits/nts_1.edit;edits/ts_2.edit;");
}
#endif
