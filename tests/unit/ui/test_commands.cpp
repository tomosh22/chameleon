#include "cml/ui/commands.h"

#include <algorithm>

#include "doctest.h"

using namespace cml::ui;
namespace json = cml::ipc::json;

TEST_CASE("the command table covers every WP-12 and ADR-0032 command") {
    std::vector<std::string> commands;
    for (const CommandInfo& c : command_table()) {
        commands.emplace_back(c.cmd);
        CHECK(std::string(c.feature).size() > 0);
    }
    for (const char* expected : {"status", "reload", "wait", "set-option", "subscribe", "exceptions", "exception-action", "restart", "revert"}) {
        CHECK(std::find(commands.begin(), commands.end(), expected) != commands.end());
    }
}

TEST_CASE("every capability a broker advertises today is covered") {
    const std::vector<std::string> today = {"status", "reload", "wait", "set-option", "subscribe", "diag-events"};
    CHECK(uncovered_capabilities(today).empty());
    const std::vector<std::string> wave6 = {"status", "reload", "wait", "set-option", "subscribe", "diag-events", "exceptions", "restart", "revert"};
    CHECK(uncovered_capabilities(wave6).empty());
    CHECK(uncovered_capabilities({"status", "teleport"}) == std::vector<std::string>{"teleport"});
}

TEST_CASE("the test-only fault-injection capability (WP-28) needs no UI command") {
    CHECK(uncovered_capabilities({"status", "reload", "fault-injection"}).empty());
    const auto unavailable = unavailable_commands({"status", "reload", "wait", "set-option", "subscribe", "fault-injection"});
    CHECK(std::find(unavailable.begin(), unavailable.end(), "fault-injection") == unavailable.end());
}

TEST_CASE("commands of a missing capability are reported unavailable") {
    const auto unavailable = unavailable_commands({"status", "reload", "wait", "set-option", "subscribe"});
    CHECK(unavailable == std::vector<std::string>{"exceptions", "exception-action", "restart", "revert"});
    CHECK(unavailable_commands({"status", "reload", "wait", "set-option", "subscribe", "exceptions", "restart", "revert"}).empty());
}

TEST_CASE("request builders") {
    CHECK(json::serialize(make_reload_request(0, "", false, 0).args) == "{}");
    CHECK(json::serialize(make_reload_request(12, "game.exe", true, 5000).args) ==
          R"({"pid":12,"module":"game.exe","confirm":true,"timeout_ms":5000})");
    CHECK(json::serialize(make_set_option_request("compile_on_save", json::Value::boolean(true)).args) ==
          R"({"name":"compile_on_save","value":true})");
    CHECK(json::serialize(make_wait_request("idle", 100).args) == R"({"for":"idle","timeout_ms":100})");
    CHECK(json::serialize(make_exception_action_request(7, 3, "continue").args) == R"({"pid":7,"exception_id":3,"action":"continue"})");
    CHECK(json::serialize(make_restart_request(7, 0).args) == R"({"pid":7})");
    CHECK(json::serialize(make_revert_request(0, "", "all").args) == R"({"scope":"all"})");
    CHECK(json::serialize(make_revert_request(7, "g.exe", "last").args) == R"({"pid":7,"module":"g.exe","scope":"last"})");
    CHECK(make_exceptions_request().cmd == "exceptions");
}

TEST_CASE("subscribe asks only for events the broker knows") {
    CHECK(subscribe_events_for({"status", "subscribe"}) == std::vector<std::string>{"diag", "patch_applied", "progress", "log"});
    CHECK(subscribe_events_for({"exceptions", "restart", "revert"}) ==
          std::vector<std::string>{"diag", "patch_applied", "progress", "log", "exception", "restarted", "patch_reverted"});
}

TEST_CASE("reload result parsing: timings per patch, changed files") {
    const auto v = json::parse(R"({"generation":4,"patched":2,"hooks_installed":9,"changed_files":["a.cpp","b.h"],
        "timing_ms":{"sweep":1.5,"compile":100,"total":150},
        "patches":[{"pid":10,"module":"g.exe","generation":3,"hooks_installed":4,"new_functions":1,"new_globals":2,
                    "timing_ms":{"compile":60,"link":20,"load":5,"total":90}},
                   {"pid":10,"module":"g.exe","generation":4}]})");
    REQUIRE(v);
    const ReloadSummary r = parse_reload_result(*v);
    CHECK(r.generation == 4);
    CHECK(r.patched == 2);
    CHECK(r.hooks_installed == 9);
    CHECK(r.changed_files == std::vector<std::string>{"a.cpp", "b.h"});
    CHECK(r.timing.present);
    CHECK(r.timing.total == doctest::Approx(150));
    REQUIRE(r.patches.size() == 2);
    CHECK(r.patches[0].timing.present);
    CHECK(r.patches[0].timing.link == doctest::Approx(20));
    CHECK(r.patches[0].new_globals == 2);
    CHECK_FALSE(r.patches[1].timing.present);
}

TEST_CASE("ADR-0032 results: restart and revert") {
    const auto restart = json::parse(R"({"old_pid":5,"new_pid":6,"modules":["C:\\g\\g.exe",{"path":"C:\\g\\p.dll"}]})");
    REQUIRE(restart);
    const RestartSummary rs = parse_restart_result(*restart);
    CHECK(rs.old_pid == 5);
    CHECK(rs.new_pid == 6);
    CHECK(rs.modules.size() == 2);
    const auto revert = json::parse(R"({"reverted":[{"pid":5,"module":"g.exe","generation":2}],"generation":1})");
    REQUIRE(revert);
    const RevertSummary rv = parse_revert_result(*revert);
    REQUIRE(rv.reverted.size() == 1);
    CHECK(rv.reverted[0].generation == 2);
    CHECK(rv.generation == 1);
}
