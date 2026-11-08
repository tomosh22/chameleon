// The `restart` step (WP-21) and the checked-in hot_restart scenario.
#include <filesystem>
#include <string>

#include "cml/ipc/control.h"
#include "cml/ipc/control_ids.h"
#include "doctest.h"
#include "scenario.h"

using namespace cmltest;

TEST_CASE("scenario: a restart step parses, describes itself and needs the restart capability") {
    const char* text = R"(
name = "x"
fixture = "hot_restart"
[[step]]
kind = "launch"
[[step]]
kind = "restart"
within_s = 45
expect_result_key = "patched"
expect_result_value = 1
[[step]]
kind = "restart"
process = "fixture"
)";
    cml::Result<Scenario> r = parse_scenario(text);
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    REQUIRE(r->steps.size() == 3);
    const Step& first = r->steps[1];
    CHECK(first.kind == StepKind::Restart);
    CHECK(first.within_s == 45);
    CHECK(first.has_result_value);
    CHECK(first.result_key == "patched");
    CHECK(first.result_value == 1);
    CHECK(first.describe() == "restart within 45 s expecting patched=1");
    CHECK(std::string(first.required_capability()) == cml::ipc::capability::kRestart);
    const Step& second = r->steps[2];
    CHECK(second.within_s == 90);  // default
    CHECK(second.process == "fixture");
    CHECK_FALSE(second.has_result_value);
    CHECK(std::string(to_string(StepKind::Restart)) == "restart");
}

TEST_CASE("scenario: bad restart steps are CML5411") {
    const std::string head = "name = \"x\"\nfixture = \"f\"\n[[step]]\nkind = \"launch\"\n";
    const auto invalid = [&](const std::string& step) {
        cml::Result<Scenario> r = parse_scenario(head + "[[step]]\nkind = \"restart\"\n" + step);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == cml::ids::kScenarioInvalid);
    };
    invalid("within_s = 0\n");
    invalid("within_s = \"9\"\n");
    invalid("colour = \"red\"\n");
    invalid("expect_result_key = \"patched\"\n");   // the value is missing
    invalid("expect_result_value = 1\n");           // the key is missing
    invalid("expect_result_key = \"a b\"\nexpect_result_value = 1\n");
    invalid("expect_result_key = \"patched\"\nexpect_result_value = \"1\"\n");
    invalid("process = \"nobody\"\n");              // never launched

    // A restart needs a launch before it.
    cml::Result<Scenario> early = parse_scenario("name = \"x\"\nfixture = \"f\"\n[[step]]\nkind = \"restart\"\n");
    REQUIRE_FALSE(early.has_value());
    CHECK(early.error().code == cml::ids::kScenarioInvalid);
}

TEST_CASE("the checked-in hot_restart scenario parses and restarts after a reload") {
    namespace fs = std::filesystem;
    const fs::path file = fs::path(CML_HARNESS_SCENARIOS_DIR) / "hot_restart.toml";
    cml::Result<Scenario> r = load_scenario(file);
    REQUIRE_MESSAGE(r.has_value(), file.string() << ": " << (r.has_value() ? std::string() : r.error().format()));
    CHECK(r->name == "hot_restart");
    CHECK(r->fixture == "hot_restart");
    int reload_at = -1;
    int restart_at = -1;
    for (std::size_t i = 0; i < r->steps.size(); ++i) {
        if (r->steps[i].kind == StepKind::Reload && reload_at < 0) {
            reload_at = static_cast<int>(i);
        }
        if (r->steps[i].kind == StepKind::Restart) {
            restart_at = static_cast<int>(i);
        }
    }
    CHECK(reload_at >= 0);
    CHECK(restart_at > reload_at);
}
