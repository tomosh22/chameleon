// The WP-17 scenario keys (fixture_args, set_option, signal_fixture, reload expect_failure) and the checked-in
// sync_point scenarios.
#include <filesystem>
#include <string>

#include "cml/ipc/control.h"
#include "cml/ipc/control_ids.h"
#include "doctest.h"
#include "scenario.h"

using namespace cmltest;

TEST_CASE("scenario: fixture_args, set_option, signal_fixture and reload expect_failure parse") {
    const char* text = R"(
name = "x"
fixture = "sync_point"
fixture_args = "--hold-sync --immediate"
[[step]]
kind = "launch"
[[step]]
kind = "set_option"
name = "hooks_timeout_ms"
value = 1500
[[step]]
kind = "reload"
within_s = 30
expect_failure = "CML5309"
[[step]]
kind = "signal_fixture"
)";
    cml::Result<Scenario> r = parse_scenario(text);
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    CHECK(r->fixture_args == "--hold-sync --immediate");
    CHECK(r->steps[1].kind == StepKind::SetOption);
    CHECK(r->steps[1].option_name == "hooks_timeout_ms");
    CHECK(r->steps[1].option_value == 1500);
    CHECK(std::string(r->steps[1].required_capability()) == cml::ipc::capability::kSetOption);
    CHECK(r->steps[1].describe() == "set_option hooks_timeout_ms=1500");
    CHECK(r->steps[2].expect_failure == "CML5309");
    CHECK(r->steps[2].describe() == "reload within 30 s expecting failure CML5309");
    CHECK(r->steps[3].kind == StepKind::SignalFixture);
    CHECK(r->steps[3].required_capability() == nullptr);
    CHECK(std::string(to_string(StepKind::SignalFixture)) == "signal_fixture");
}

TEST_CASE("scenario: no fixture_args is the empty string") {
    cml::Result<Scenario> r = parse_scenario("name = \"x\"\nfixture = \"basic\"\n[[step]]\nkind = \"launch\"\n");
    REQUIRE(r.has_value());
    CHECK(r->fixture_args.empty());
}

TEST_CASE("scenario: bad WP-17 keys are CML5411") {
    const std::string head = "name = \"x\"\nfixture = \"sync_point\"\n";
    const std::string launch = "[[step]]\nkind = \"launch\"\n[[step]]\n";
    const auto invalid = [&](const std::string& text) {
        cml::Result<Scenario> r = parse_scenario(text);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == cml::ids::kScenarioInvalid);
    };
    invalid(head + "fixture_args = 3\n" + launch + "kind = \"signal_fixture\"\n");
    invalid(head + "fixture_args = \"--x; del *\"\n" + launch + "kind = \"signal_fixture\"\n");
    invalid(head + launch + "kind = \"set_option\"\nname = \"a\"\n");                         // value missing
    invalid(head + launch + "kind = \"set_option\"\nname = \"a\"\nvalue = \"1\"\n");         // not an integer
    invalid(head + launch + "kind = \"set_option\"\nname = \"a b\"\nvalue = 1\n");           // bad name
    invalid(head + launch + "kind = \"set_option\"\nvalue = 1\n");                           // name missing
    invalid(head + launch + "kind = \"signal_fixture\"\nwithin_s = 3\n");                    // unknown key
    invalid(head + launch + "kind = \"reload\"\nexpect_failure = \"5309\"\n");               // not a diagnostic id
    invalid(head + launch + "kind = \"reload\"\nexpect_failure = 5309\n");
    invalid(head + launch + "kind = \"reload\"\nexpect_failure = \"CML5309\"\nexpect_result_key = \"a\"\nexpect_result_value = 1\n");
}

TEST_CASE("the checked-in sync_point scenarios parse and check what they claim to") {
    namespace fs = std::filesystem;
    for (const char* name : {"sync_point_apply", "hooks_order", "sync_point_late"}) {
        const fs::path file = fs::path(CML_HARNESS_SCENARIOS_DIR) / (std::string(name) + ".toml");
        cml::Result<Scenario> r = load_scenario(file);
        REQUIRE_MESSAGE(r.has_value(), file.string() << ": " << (r.has_value() ? std::string() : r.error().format()));
        CHECK(r->name == name);
        CHECK(r->fixture == "sync_point");
        bool reload = false;
        bool hook_line = false;
        for (const Step& s : r->steps) {
            reload = reload || s.kind == StepKind::Reload;
            hook_line = hook_line || (s.kind == StepKind::ExpectStdout && s.regex.find("HOOK") != std::string::npos);
        }
        CHECK_MESSAGE(reload, name);
        CHECK_MESSAGE(hook_line, name);
    }
    cml::Result<Scenario> late = load_scenario(fs::path(CML_HARNESS_SCENARIOS_DIR) / "sync_point_late.toml");
    REQUIRE(late.has_value());
    bool failure = false;
    bool signal = false;
    bool option = false;
    for (const Step& s : late->steps) {
        failure = failure || (s.kind == StepKind::Reload && !s.expect_failure.empty());
        signal = signal || s.kind == StepKind::SignalFixture;
        option = option || s.kind == StepKind::SetOption;
    }
    CHECK(failure);
    CHECK(signal);
    CHECK(option);
}
