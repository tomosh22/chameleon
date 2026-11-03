// The WP-20 scenario steps (watch_exceptions, expect_exception, exception_action, expect_pending_exceptions) and the checked-in
// crash scenarios.
#include <filesystem>
#include <string>

#include "cml/ipc/control.h"
#include "cml/ipc/control_ids.h"
#include "doctest.h"
#include "scenario.h"

using namespace cmltest;

TEST_CASE("scenario: the crash steps parse, describe themselves and need the exceptions capability") {
    const char* text = R"(
name = "x"
fixture = "crash"
[[step]]
kind = "launch"
[[step]]
kind = "watch_exceptions"
[[step]]
kind = "expect_exception"
code = "0xC0000005"
function = "cell_store"
within_s = 15
[[step]]
kind = "expect_pending_exceptions"
count = 1
[[step]]
kind = "exception_action"
action = "leave"
[[step]]
kind = "exception_action"
action = "continue"
expect_error = "CML4401"
)";
    cml::Result<Scenario> r = parse_scenario(text);
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    REQUIRE(r->steps.size() == 6);
    CHECK(r->steps[1].kind == StepKind::WatchExceptions);
    CHECK(r->steps[2].kind == StepKind::ExpectException);
    CHECK(r->steps[2].exception_code == "0xC0000005");
    CHECK(r->steps[2].exception_function == "cell_store");
    CHECK(r->steps[2].within_s == 15);
    CHECK(r->steps[3].kind == StepKind::ExpectPendingExceptions);
    CHECK(r->steps[3].result_value == 1);
    CHECK(r->steps[4].kind == StepKind::ExceptionAction);
    CHECK(r->steps[4].exception_action == "leave");
    CHECK(r->steps[5].expect_error == "CML4401");
    for (std::size_t i = 1; i < r->steps.size(); ++i) {
        REQUIRE(r->steps[i].required_capability() != nullptr);
        CHECK(std::string(r->steps[i].required_capability()) == cml::ipc::capability::kExceptions);
    }
    CHECK(r->steps[2].describe() == "expect_exception 0xC0000005 in cell_store within 15 s");
    CHECK(r->steps[4].describe() == "exception_action leave");
    CHECK(r->steps[5].describe() == "exception_action continue expecting the refusal CML4401");
    CHECK(r->steps[3].describe() == "expect_pending_exceptions 1");
    CHECK(std::string(to_string(StepKind::WatchExceptions)) == "watch_exceptions");
    CHECK(std::string(to_string(StepKind::ExpectPendingExceptions)) == "expect_pending_exceptions");
}

TEST_CASE("scenario: bad crash step keys are CML5411") {
    const std::string head = "name = \"x\"\nfixture = \"crash\"\n[[step]]\nkind = \"launch\"\n[[step]]\n";
    const auto invalid = [&](const std::string& text) {
        cml::Result<Scenario> r = parse_scenario(text);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == cml::ids::kScenarioInvalid);
    };
    invalid(head + "kind = \"watch_exceptions\"\nwithin_s = 3\n");                       // unknown key
    invalid(head + "kind = \"expect_exception\"\ncode = \"C0000005\"\n");                // not 0x........
    invalid(head + "kind = \"expect_exception\"\ncode = \"0xC000000Z\"\n");              // not hex
    invalid(head + "kind = \"expect_exception\"\nfunction = \"\"\n");                    // empty
    invalid(head + "kind = \"expect_exception\"\nwithin_s = 0\n");
    invalid(head + "kind = \"exception_action\"\n");                                     // no action
    invalid(head + "kind = \"exception_action\"\naction = \"explode\"\n");
    invalid(head + "kind = \"exception_action\"\naction = \"leave\"\nexpect_error = \"4401\"\n");
    invalid(head + "kind = \"expect_pending_exceptions\"\n");                            // no count
    invalid(head + "kind = \"expect_pending_exceptions\"\ncount = -1\n");
    invalid(head + "kind = \"expect_pending_exceptions\"\ncount = \"1\"\n");
}

TEST_CASE("the checked-in crash scenarios parse and check what they claim to") {
    namespace fs = std::filesystem;
    for (const char* name : {"crash_null_deref_fixed", "crash_leave_function"}) {
        const fs::path file = fs::path(CML_HARNESS_SCENARIOS_DIR) / (std::string(name) + ".toml");
        cml::Result<Scenario> r = load_scenario(file);
        REQUIRE_MESSAGE(r.has_value(), file.string() << ": " << (r.has_value() ? std::string() : r.error().format()));
        CHECK(r->name == name);
        CHECK(r->fixture == "crash");
        bool watch = false;
        bool signal = false;
        bool expect = false;
        bool action = false;
        std::size_t watch_at = 0;
        std::size_t signal_at = 0;
        for (std::size_t i = 0; i < r->steps.size(); ++i) {
            const Step& s = r->steps[i];
            if (s.kind == StepKind::WatchExceptions) {
                watch = true;
                watch_at = i;
            }
            if (s.kind == StepKind::SignalFixture) {
                signal = true;
                signal_at = i;
            }
            expect = expect || (s.kind == StepKind::ExpectException && s.exception_function == "cell_store");
            action = action || s.kind == StepKind::ExceptionAction;
        }
        CHECK_MESSAGE(watch, name);
        CHECK_MESSAGE(signal, name);
        CHECK_MESSAGE(expect, name);
        CHECK_MESSAGE(action, name);
        // The agents must hold faulting threads before the fixture faults.
        CHECK_MESSAGE(watch_at < signal_at, name);
    }
    cml::Result<Scenario> fixed = load_scenario(fs::path(CML_HARNESS_SCENARIOS_DIR) / "crash_null_deref_fixed.toml");
    REQUIRE(fixed.has_value());
    bool reload = false;
    bool continue_action = false;
    for (const Step& s : fixed->steps) {
        reload = reload || s.kind == StepKind::Reload;
        continue_action = continue_action || (s.kind == StepKind::ExceptionAction && s.exception_action == "continue");
    }
    CHECK(reload);
    CHECK(continue_action);
    cml::Result<Scenario> leave = load_scenario(fs::path(CML_HARNESS_SCENARIOS_DIR) / "crash_leave_function.toml");
    REQUIRE(leave.has_value());
    CHECK(leave->fixture_args == "--leave");
    for (const Step& s : leave->steps) {
        CHECK(s.kind != StepKind::Reload);
        CHECK(!(s.kind == StepKind::ExceptionAction && s.exception_action != "leave"));
    }
}
