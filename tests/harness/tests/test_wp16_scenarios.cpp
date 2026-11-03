// WP-16 scenario keys: several processes (launch exe/label/args, process=), expect_reload_result, and the checked-in
// multi-module / multi-process scenarios.
#include <filesystem>
#include <string>

#include "cml/ipc/control_ids.h"
#include "doctest.h"
#include "scenario.h"

using namespace cmltest;

TEST_CASE("scenario: a second launch with exe, labels and process= parse") {
    const char* text = R"(
name = "x"
fixture = "two_processes"
[[step]]
kind = "launch"
label = "server"
[[step]]
kind = "launch"
exe = "fixture_two_processes_client"
args = "--force-load-collisions 1"
[[step]]
kind = "expect_stdout"
process = "fixture_two_processes_client"
regex = "TICK"
[[step]]
kind = "expect_module_enabled"
process = "server"
module = "a.dll"
[[step]]
kind = "reload"
[[step]]
kind = "expect_reload_result"
key = "compiled"
value = 1
[[step]]
kind = "expect_monotonic"
process = "server"
key = "calls"
)";
    cml::Result<Scenario> r = parse_scenario(text);
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    CHECK(r->steps[0].label == "server");
    CHECK(r->steps[1].exe == "fixture_two_processes_client");
    CHECK(r->steps[1].label == "fixture_two_processes_client");  // default: the exe name
    CHECK(r->steps[1].args == "--force-load-collisions 1");
    CHECK(r->steps[2].process == "fixture_two_processes_client");
    CHECK(r->steps[3].process == "server");
    CHECK(r->steps[5].kind == StepKind::ExpectReloadResult);
    CHECK(r->steps[5].key == "compiled");
    CHECK(r->steps[5].result_value == 1);
    CHECK(r->steps[5].describe() == "expect_reload_result compiled=1");
    CHECK(r->steps[5].required_capability() == nullptr);
    CHECK(r->steps[1].describe() == "launch fixture_two_processes_client as fixture_two_processes_client [--force-load-collisions 1]");
}

TEST_CASE("scenario: bad WP-16 keys are CML5411") {
    const std::string head = "name = \"x\"\nfixture = \"f\"\n[[step]]\nkind = \"launch\"\n";
    const auto invalid = [&](const std::string& steps) {
        cml::Result<Scenario> r = parse_scenario(head + steps);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == cml::ids::kScenarioInvalid);
    };
    invalid("[[step]]\nkind = \"launch\"\n");                                          // the primary exe twice
    invalid("[[step]]\nkind = \"launch\"\nexe = \"b\"\n[[step]]\nkind = \"launch\"\nexe = \"b\"\n");  // same label twice
    invalid("[[step]]\nkind = \"launch\"\nexe = \"a b\"\n");
    invalid("[[step]]\nkind = \"launch\"\nexe = \"b\"\nlabel = \"a b\"\n");
    invalid("[[step]]\nkind = \"launch\"\nexe = \"b\"\nargs = \"say \\\"x\\\"\"\n");
    invalid("[[step]]\nkind = \"expect_stdout\"\nregex = \"x\"\nprocess = \"nobody\"\n");  // not launched
    invalid("[[step]]\nkind = \"edit\"\npatch = \"a.edit\"\nprocess = \"fixture\"\n");     // the key does not belong to edit
    invalid("[[step]]\nkind = \"expect_reload_result\"\nkey = \"a\"\n");                   // value missing
    invalid("[[step]]\nkind = \"expect_reload_result\"\nkey = \"a b\"\nvalue = 1\n");
    invalid("[[step]]\nkind = \"expect_reload_result\"\nkey = \"a\"\nvalue = \"1\"\n");
}

TEST_CASE("the checked-in WP-16 scenarios parse") {
    namespace fs = std::filesystem;
    for (const char* name : {"exe_plus_dll", "two_processes", "relink_other_base"}) {
        const fs::path file = fs::path(CML_HARNESS_SCENARIOS_DIR) / (std::string(name) + ".toml");
        cml::Result<Scenario> r = load_scenario(file);
        REQUIRE_MESSAGE(r.has_value(), file.string() << ": " << (r.has_value() ? std::string() : r.error().format()));
        CHECK(r->name == name);
        bool reload = false;
        for (const Step& s : r->steps) {
            reload = reload || s.kind == StepKind::Reload;
        }
        CHECK_MESSAGE(reload, name);
    }
    cml::Result<Scenario> two = load_scenario(fs::path(CML_HARNESS_SCENARIOS_DIR) / "two_processes.toml");
    REQUIRE(two.has_value());
    int launches = 0;
    for (const Step& s : two->steps) {
        launches += s.kind == StepKind::Launch ? 1 : 0;
    }
    CHECK(launches == 2);
}

TEST_CASE("scenario: the default label of a launched exe drops a case-insensitive .exe suffix only") {
    const std::string head = "name = \"x\"\nfixture = \"f\"\n[[step]]\nkind = \"launch\"\n";
    cml::Result<Scenario> r = parse_scenario(head + "[[step]]\nkind = \"launch\"\nexe = \"Client.EXE\"\n[[step]]\nkind = \"launch\"\nexe = \"b.exe.txt\"\n");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    CHECK(r->steps[1].label == "Client");
    CHECK(r->steps[2].label == "b.exe.txt");
}
