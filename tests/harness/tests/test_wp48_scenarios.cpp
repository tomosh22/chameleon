// WP-48: the checked-in scenario plugin_reload (fixture plugin_reload) has the step ADR-0044 asks for: the plugin is freed and loaded
// again with nothing in between (inside one agent module poll, under 50 ms), and the edit is still in effect afterwards, without a
// reload in between.
#include <filesystem>
#include <string>

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

// The index of the first step at or after `from` of this kind whose regex contains `text`; s.steps.size() if none.
std::size_t find(const Scenario& s, std::size_t from, StepKind kind, const std::string& text) {
    for (std::size_t i = from; i < s.steps.size(); ++i) {
        if (s.steps[i].kind == kind && (text.empty() || s.steps[i].regex.find(text) != std::string::npos)) {
            return i;
        }
    }
    return s.steps.size();
}

}  // namespace

TEST_CASE("scenario plugin_reload (WP-48): FreeLibrary + LoadLibrary inside one agent poll keeps the edit, without a reload") {
    const Scenario s = load("plugin_reload.toml");
    REQUIRE_FALSE(s.steps.empty());
    CHECK(s.steps[0].kind == StepKind::Launch);
    CHECK(s.steps[0].args.find("--quick-reload-at 3") != std::string::npos);
    // The edit 2 -> 3 is in effect in gen=2, then the quick free + load (gen=3) ...
    const std::size_t before = find(s, 0, StepKind::ExpectStdout, "value=3 loaded=1 gen=2");
    REQUIRE(before < s.steps.size());
    const std::size_t quick = find(s, before, StepKind::ExpectStdout, "quick=1");
    REQUIRE(quick < s.steps.size());
    CHECK(find(s, before, StepKind::SignalFixture, "") < quick);
    // ... under 50 ms ...
    const std::size_t gap = find(s, quick, StepKind::ExpectStdout, "QUICK gap_ms=");
    REQUIRE(gap < s.steps.size());
    CHECK(s.steps[gap].regex.find("[1-4][0-9]") != std::string::npos);
    // ... and the edited value in the new image, with no reload in between.
    const std::size_t after = find(s, gap, StepKind::ExpectStdout, "value=3 loaded=1 gen=3");
    REQUIRE(after < s.steps.size());
    CHECK(find(s, before, StepKind::Reload, "") > after);
}
#endif
