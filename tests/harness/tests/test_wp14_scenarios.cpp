// The WP-14 scenario steps (set_option, wait, expect_event) and the checked-in scenarios that use them.
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include "cml/ipc/control.h"
#include "cml/ipc/control_ids.h"
#include "doctest.h"
#include "scenario.h"
#include "unified_diff.h"

using namespace cmltest;

TEST_CASE("scenario: set_option, wait and expect_event parse and name their capabilities") {
    const char* text = R"(
name = "x"
fixture = "basic"
[[step]]
kind = "launch"
[[step]]
kind = "wait"
for = "index"
within_s = 30
[[step]]
kind = "set_option"
option = "compile_on_save"
value = true
[[step]]
kind = "set_option"
option = "max_parallel_compiles"
value = 2
[[step]]
kind = "set_option"
option = "linker"
value = "lld-link"
[[step]]
kind = "expect_event"
event = "patch_applied"
within_s = 45
)";
    cml::Result<Scenario> r = parse_scenario(text);
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    REQUIRE(r->steps.size() == 6);
    CHECK(r->steps[1].kind == StepKind::Wait);
    CHECK(r->steps[1].wait_for == "index");
    CHECK(r->steps[1].within_s == 30);
    CHECK(std::string(r->steps[1].required_capability()) == cml::ipc::capability::kWait);
    CHECK(r->steps[2].kind == StepKind::SetOption);
    CHECK(r->steps[2].option_kind == Step::OptionKind::Bool);
    CHECK(r->steps[2].option_bool);
    CHECK(r->steps[2].describe() == "set_option compile_on_save = true");
    CHECK(std::string(r->steps[2].required_capability()) == cml::ipc::capability::kSetOption);
    CHECK(r->steps[3].option_kind == Step::OptionKind::Integer);
    CHECK(r->steps[3].option_integer == 2);
    CHECK(r->steps[4].option_kind == Step::OptionKind::String);
    CHECK(r->steps[4].describe() == "set_option linker = \"lld-link\"");
    CHECK(r->steps[5].kind == StepKind::ExpectEvent);
    CHECK(r->steps[5].event == "patch_applied");
    CHECK(r->steps[5].within_s == 45);
    CHECK(r->steps[5].describe() == "expect_event patch_applied within 45 s");
    CHECK(std::string(r->steps[5].required_capability()) == cml::ipc::capability::kSubscribe);
}

TEST_CASE("scenario: bad WP-14 steps are CML5411") {
    const std::string head = "name = \"x\"\nfixture = \"basic\"\n[[step]]\nkind = \"launch\"\n[[step]]\n";
    const auto invalid = [&](const std::string& step) {
        cml::Result<Scenario> r = parse_scenario(head + step);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == cml::ids::kScenarioInvalid);
    };
    invalid("kind = \"set_option\"\nvalue = true\n");                         // option missing
    invalid("kind = \"set_option\"\noption = \"compile_on_save\"\n");         // value missing
    invalid("kind = \"set_option\"\noption = \"a b\"\nvalue = true\n");
    invalid("kind = \"set_option\"\noption = \"x\"\nvalue = true\nwithin_s = 1\n");  // unknown key
    invalid("kind = \"wait\"\n");
    invalid("kind = \"wait\"\nfor = \"never\"\n");
    invalid("kind = \"expect_event\"\n");
    invalid("kind = \"expect_event\"\nevent = \"a b\"\n");
    invalid("kind = \"expect_event\"\nevent = \"x\"\nwithin_s = 0\n");
    // Like every step except edit they need a launch first.
    cml::Result<Scenario> early = parse_scenario("name = \"x\"\nfixture = \"basic\"\n[[step]]\nkind = \"set_option\"\noption = \"a\"\nvalue = true\n");
    REQUIRE_FALSE(early.has_value());
}

TEST_CASE("the WP-14 scenarios: save_triggers_reload has no reload step, reload_twice reloads the same unit twice") {
    namespace fs = std::filesystem;
    {
        cml::Result<Scenario> r = load_scenario(fs::path(CML_HARNESS_SCENARIOS_DIR) / "save_triggers_reload.toml");
        REQUIRE_MESSAGE(r.has_value(), (r.has_value() ? std::string() : r.error().format()));
        bool compile_on_save = false, event = false, value_after = false, edit_after_option = false;
        for (const Step& s : r->steps) {
            CHECK(s.kind != StepKind::Reload);  // the point of the scenario
            if (s.kind == StepKind::SetOption && s.option == "compile_on_save" && s.option_bool) compile_on_save = true;
            if (s.kind == StepKind::Edit && compile_on_save) edit_after_option = true;
            if (s.kind == StepKind::ExpectEvent && s.event == "patch_applied" && edit_after_option) event = true;
            if (s.kind == StepKind::ExpectStdout && event) value_after = true;
        }
        CHECK(compile_on_save);
        CHECK(edit_after_option);
        CHECK(event);
        CHECK(value_after);
    }
    {
        cml::Result<Scenario> r = load_scenario(fs::path(CML_HARNESS_SCENARIOS_DIR) / "reload_twice.toml");
        REQUIRE_MESSAGE(r.has_value(), (r.has_value() ? std::string() : r.error().format()));
        CHECK(r->fixture == "new_function");
        int edits = 0, reloads = 0;
        for (const Step& s : r->steps) {
            edits += s.kind == StepKind::Edit;
            reloads += s.kind == StepKind::Reload;
        }
        CHECK(edits == 2);
        CHECK(reloads == 2);
        // Both edits apply, one after the other, to a scratch copy of the fixture, and the result has what the
        // scenario expects: patch 2 changes only the constant that patch 1's helper is added to.
        const fs::path fixture = fs::path(CML_FIXTURE_BASIC_DIR).parent_path() / "new_function";
        const fs::path scratch = fs::temp_directory_path() / "cml_wp14_reload_twice_scratch";
        std::error_code ec;
        fs::remove_all(scratch, ec);
        fs::create_directories(scratch);
        fs::copy_file(fixture / "logic.cpp", scratch / "logic.cpp");
        for (const Step& s : r->steps) {
            if (s.kind != StepKind::Edit) continue;
            std::ifstream in(fixture / s.patch, std::ios::binary);
            REQUIRE_MESSAGE(in.good(), s.patch);
            std::ostringstream text;
            text << in.rdbuf();
            cml::Result<AppliedPatch> applied = apply_patch_to_tree(scratch, text.str(), 1);
            REQUIRE_MESSAGE(applied.has_value(), (applied.has_value() ? std::string() : applied.error().format()));
        }
        std::ifstream result(scratch / "logic.cpp", std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(result)), std::istreambuf_iterator<char>());
        CHECK(content.find("return 41;") != std::string::npos);
        CHECK(content.find("return 2 + new_helper();") != std::string::npos);
        fs::remove_all(scratch, ec);
    }
}
