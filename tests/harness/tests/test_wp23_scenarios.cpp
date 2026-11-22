// The WP-23 scenario steps (expect_exception_updated, offers / lacks / reason, exception_action reexecute) and the checked-in
// scenario crash_reexecute.
#include <filesystem>
#include <string>

#include "cml/ipc/control.h"
#include "cml/ipc/control_ids.h"
#include "doctest.h"
#include "scenario.h"

using namespace cmltest;

TEST_CASE("scenario: the re-execute steps parse, describe themselves and need the reexecute capability") {
    const char* text = R"(
name = "x"
fixture = "crash_reexecute"
[[step]]
kind = "launch"
[[step]]
kind = "expect_exception"
function = "step"
lacks = "reexecute"
reason = "CML5900"
[[step]]
kind = "expect_exception_updated"
offers = "reexecute"
within_s = 20
[[step]]
kind = "expect_exception_updated"
lacks = "reexecute"
reason = "optimi"
[[step]]
kind = "exception_action"
action = "reexecute"
[[step]]
kind = "exception_action"
action = "reexecute"
expect_error = "CML5908"
)";
    cml::Result<Scenario> r = parse_scenario(text);
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    REQUIRE(r->steps.size() == 6);
    CHECK(r->steps[1].kind == StepKind::ExpectException);
    CHECK(r->steps[1].exception_lacks == "reexecute");
    CHECK(r->steps[1].exception_reason == "CML5900");
    CHECK(r->steps[1].exception_offers.empty());
    CHECK(r->steps[2].kind == StepKind::ExpectExceptionUpdated);
    CHECK(r->steps[2].exception_offers == "reexecute");
    CHECK(r->steps[2].within_s == 20);
    CHECK(r->steps[3].exception_lacks == "reexecute");
    CHECK(r->steps[3].exception_reason == "optimi");
    CHECK(r->steps[4].exception_action == "reexecute");
    CHECK(r->steps[5].expect_error == "CML5908");
    // The step that waits for an update and the action itself need the capability `reexecute`; the plain expect needs `exceptions`.
    CHECK(std::string(r->steps[1].required_capability()) == cml::ipc::capability::kExceptions);
    for (std::size_t i : {2u, 3u, 4u, 5u}) {
        REQUIRE(r->steps[i].required_capability() != nullptr);
        CHECK(std::string(r->steps[i].required_capability()) == cml::ipc::capability::kReexecute);
    }
    CHECK(r->steps[2].describe() == "expect_exception_updated offering reexecute within 20 s");
    CHECK(r->steps[3].describe() == "expect_exception_updated without reexecute within 30 s");
    CHECK(std::string(to_string(StepKind::ExpectExceptionUpdated)) == "expect_exception_updated");
}

TEST_CASE("scenario: bad re-execute step keys are CML5411") {
    const std::string head = "name = \"x\"\nfixture = \"crash_reexecute\"\n[[step]]\nkind = \"launch\"\n[[step]]\n";
    const auto invalid = [&](const std::string& text) {
        cml::Result<Scenario> r = parse_scenario(text);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == cml::ids::kScenarioInvalid);
    };
    invalid(head + "kind = \"expect_exception_updated\"\n");                                  // neither offers nor lacks
    invalid(head + "kind = \"expect_exception_updated\"\nreason = \"x\"\n");                  // a reason alone is not a condition
    invalid(head + "kind = \"expect_exception_updated\"\noffers = \"explode\"\n");
    invalid(head + "kind = \"expect_exception_updated\"\nlacks = 3\n");
    invalid(head + "kind = \"expect_exception_updated\"\noffers = \"reexecute\"\nwithin_s = 0\n");
    invalid(head + "kind = \"expect_exception_updated\"\noffers = \"reexecute\"\ncode = \"0xC0000005\"\n");  // unknown key for this step
    invalid(head + "kind = \"expect_exception\"\nreason = \"\"\n");
}

TEST_CASE("the checked-in scenario crash_reexecute checks what it claims to") {
    namespace fs = std::filesystem;
    const fs::path file = fs::path(CML_HARNESS_SCENARIOS_DIR) / "crash_reexecute.toml";
    cml::Result<Scenario> r = load_scenario(file);
    REQUIRE_MESSAGE(r.has_value(), file.string() << ": " << (r.has_value() ? std::string() : r.error().format()));
    CHECK(r->name == "crash_reexecute");
    CHECK(r->fixture == "crash_reexecute");
    std::size_t watch_at = 0, signal_at = 0, reexecute_at = 0, updated_at = 0, leave_at = 0, refused_at = 0;
    int edits = 0;
    int reloads = 0;
    bool offers = false;
    bool optimised_reason = false;
    bool step_function = false;
    bool opt_function = false;
    for (std::size_t i = 0; i < r->steps.size(); ++i) {
        const Step& s = r->steps[i];
        switch (s.kind) {
            case StepKind::WatchExceptions: watch_at = i; break;
            case StepKind::SignalFixture: signal_at = i; break;
            case StepKind::Edit: ++edits; break;
            case StepKind::Reload: ++reloads; break;
            case StepKind::ExpectException:
                step_function = step_function || (s.exception_function == "step" && s.exception_lacks == "reexecute");
                opt_function = opt_function || (s.exception_function == "opt_step" && s.exception_lacks == "reexecute");
                break;
            case StepKind::ExpectExceptionUpdated:
                offers = offers || s.exception_offers == "reexecute";
                optimised_reason = optimised_reason || (s.exception_lacks == "reexecute" && !s.exception_reason.empty());
                if (s.exception_offers == "reexecute") updated_at = i;
                break;
            case StepKind::ExceptionAction:
                if (s.exception_action == "reexecute" && s.expect_error.empty()) reexecute_at = i;
                if (s.exception_action == "reexecute" && s.expect_error == "CML5908") refused_at = i;
                if (s.exception_action == "leave") leave_at = i;
                break;
            default: break;
        }
    }
    CHECK(watch_at < signal_at);
    CHECK(step_function);
    CHECK(opt_function);
    CHECK(offers);
    CHECK(optimised_reason);
    CHECK(edits == 2);
    CHECK(reloads == 2);
    CHECK(updated_at < reexecute_at);          // the action comes after the patch made it available
    CHECK(refused_at != 0);                    // the optimised function: asking anyway is refused ...
    CHECK(leave_at > refused_at);              // ... and Leave resolves it
    // The proof of the original arguments: the exact result and the values the function saw, not "any number".
    bool exact = false;
    for (const Step& s : r->steps) {
        if (s.kind == StepKind::ExpectStdout && s.regex.find("result=102756 seen_k=13 seen_scale=2\\.750") != std::string::npos) {
            exact = true;
        }
    }
    CHECK(exact);
}
