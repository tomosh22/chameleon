// WP-47: the expect_event key `absent` (no event of that name may arrive) and the checked-in scenario atomic_reload, which must show
// what the work package promises: a header edit over three compilands, the third refused (CML3400), applies nothing (no
// patch_applied, the old values of all three), the application keeps running, and the same edit without the refused part reloads
// all three.
#include <filesystem>
#include <string>

#include "cml/ipc/control.h"
#include "cml/ipc/control_ids.h"
#include "doctest.h"
#include "scenario.h"

using namespace cmltest;

TEST_CASE("scenario (WP-47): expect_event takes `absent`, a boolean; the step then says `no <event>`") {
    const char* text = R"(
name = "x"
fixture = "basic"
[[step]]
kind = "launch"
[[step]]
kind = "expect_event"
event = "patch_applied"
absent = true
within_s = 3
[[step]]
kind = "expect_event"
event = "patch_applied"
absent = false
)";
    cml::Result<Scenario> r = parse_scenario(text);
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    REQUIRE(r->steps.size() == 3);
    CHECK(r->steps[1].kind == StepKind::ExpectEvent);
    CHECK(r->steps[1].event_absent);
    CHECK(r->steps[1].within_s == 3);
    CHECK(r->steps[1].describe() == "expect_event no patch_applied within 3 s");
    CHECK(std::string(r->steps[1].required_capability()) == cml::ipc::capability::kSubscribe);
    CHECK_FALSE(r->steps[2].event_absent);
    CHECK(r->steps[2].describe() == "expect_event patch_applied within 10 s");

    cml::Result<Scenario> bad = parse_scenario("name = \"x\"\nfixture = \"basic\"\n[[step]]\nkind = \"launch\"\n[[step]]\nkind = \"expect_event\"\nevent = \"x\"\nabsent = 1\n");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == cml::ids::kScenarioInvalid);
}

#ifdef CML_HARNESS_SCENARIOS_DIR
TEST_CASE("scenario atomic_reload (WP-47): the third compiland is refused, nothing is applied, then the same edit reloads all three") {
    cml::Result<Scenario> r = load_scenario(std::filesystem::path(CML_HARNESS_SCENARIOS_DIR) / "atomic_reload.toml");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    const Scenario& s = *r;
    CHECK(s.fixture == "atomic_reload");
    const auto next = [&](std::size_t from, StepKind kind) {
        for (std::size_t i = from; i < s.steps.size(); ++i) {
            if (s.steps[i].kind == kind) {
                return i;
            }
        }
        return s.steps.size();
    };
    const std::size_t edit1 = next(0, StepKind::Edit);
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
    CHECK(s.steps[old_values].regex.find("a=11 b=101 c=1001") != std::string::npos);
    const std::size_t edit2 = next(old_values, StepKind::Edit);
    REQUIRE(edit2 < s.steps.size());
    CHECK(s.steps[edit2].patch == "edits/stacked/no_tls.edit");
    const std::size_t reloaded = next(edit2, StepKind::Reload);
    REQUIRE(reloaded < s.steps.size());
    CHECK(s.steps[reloaded].expect_error.empty());
    CHECK(s.steps[reloaded].result_key == "patched");
    CHECK(s.steps[reloaded].result_value == 3);
    const std::size_t new_values = next(reloaded, StepKind::ExpectStdout);
    REQUIRE(new_values < s.steps.size());
    CHECK(s.steps[new_values].regex.find("a=22 b=202 c=2002") != std::string::npos);
}
#endif
