// The WP-12 scenario keys (expect_monotonic, reload result expectations) and the checked-in M1 scenarios.
#include <filesystem>
#include <string>

#include "cml/ipc/control_ids.h"
#include "doctest.h"
#include "scenario.h"

using namespace cmltest;

TEST_CASE("scenario: expect_monotonic and reload result expectations parse") {
    const char* text = R"(
name = "x"
fixture = "statics"
[[step]]
kind = "launch"
[[step]]
kind = "reload"
expect_result_key = "initializers_run"
expect_result_value = 1
[[step]]
kind = "expect_monotonic"
key = "calls"
min_samples = 20
)";
    cml::Result<Scenario> r = parse_scenario(text);
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    CHECK(r->steps[1].has_result_value);
    CHECK(r->steps[1].result_key == "initializers_run");
    CHECK(r->steps[1].result_value == 1);
    CHECK(r->steps[1].describe() == "reload within 60 s expecting initializers_run=1");
    CHECK(r->steps[2].kind == StepKind::ExpectMonotonic);
    CHECK(r->steps[2].key == "calls");
    CHECK(r->steps[2].min_samples == 20);
    CHECK(r->steps[2].required_capability() == nullptr);
}

TEST_CASE("scenario: bad WP-12 keys are CML5411") {
    const std::string head = "name = \"x\"\nfixture = \"statics\"\n[[step]]\nkind = \"launch\"\n[[step]]\n";
    const auto invalid = [&](const std::string& step) {
        cml::Result<Scenario> r = parse_scenario(head + step);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == cml::ids::kScenarioInvalid);
    };
    invalid("kind = \"reload\"\nexpect_result_key = \"a\"\n");               // one without the other
    invalid("kind = \"reload\"\nexpect_result_value = 1\n");
    invalid("kind = \"reload\"\nexpect_result_key = \"a b\"\nexpect_result_value = 1\n");
    invalid("kind = \"reload\"\nexpect_result_key = \"a\"\nexpect_result_value = \"1\"\n");
    invalid("kind = \"expect_monotonic\"\n");                                  // key missing
    invalid("kind = \"expect_monotonic\"\nkey = \"a b\"\n");
    invalid("kind = \"expect_monotonic\"\nkey = \"a\"\nmin_samples = 1\n");    // at least two samples
    invalid("kind = \"expect_monotonic\"\nkey = \"a\"\nwithin_s = 3\n");       // unknown key
}

// Every checked-in scenario is valid and has the shape the CI relies on.
TEST_CASE("the checked-in M1 scenarios parse and follow the launch / expect / edit / reload / expect pattern") {
    namespace fs = std::filesystem;
    const char* names[] = {"basic", "statics", "virtuals", "templates", "lambdas", "new_function", "new_global"};
    for (const char* name : names) {
        const fs::path file = fs::path(CML_HARNESS_SCENARIOS_DIR) / (std::string(name) + ".toml");
        cml::Result<Scenario> r = load_scenario(file);
        REQUIRE_MESSAGE(r.has_value(), file.string() << ": " << (r.has_value() ? std::string() : r.error().format()));
        CHECK(r->fixture == name);
        bool reload = false;
        bool value_after = false;
        for (const Step& s : r->steps) {
            reload = reload || s.kind == StepKind::Reload;
            value_after = value_after || (reload && s.kind == StepKind::ExpectStdout);
        }
        CHECK_MESSAGE(reload, name);
        CHECK_MESSAGE(value_after, name);
    }
}
