#include "cml/ui/view_model.h"

#include "doctest.h"
#include "fake_broker.h"

using namespace cml::ui;
using cml::ui::test::event;

namespace {

// ViewModel holds a mutex and cannot be returned by value: tests declare it, then pin the clock.
void fixed_clock(ViewModel& vm) {
    vm.set_clock([] { return std::int64_t{1000}; });
}

}  // namespace

TEST_CASE("status fills the process and module lists") {
    ViewModel vm;
    cml::ui::test::FakeBroker broker;
    vm.apply_status(broker.status);
    REQUIRE(vm.processes().size() == 1);
    CHECK(vm.processes()[0].pid == 4242);
    REQUIRE(vm.processes()[0].modules.size() == 2);
    CHECK(vm.processes()[0].modules[0].enabled);
    CHECK(vm.processes()[0].modules[0].index_state == "ready");
    CHECK_FALSE(vm.processes()[0].modules[1].enabled);
    CHECK(vm.broker_version() == "0.1.0-test");
    CHECK(vm.feature_available(Feature::Reload));
    CHECK_FALSE(vm.feature_available(Feature::Restart));
    CHECK_FALSE(vm.feature_available(Feature::Revert));
    CHECK_FALSE(vm.feature_available(Feature::Exceptions));
}

TEST_CASE("a diag event becomes output lines with clickable errors") {
    ViewModel vm;
    fixed_clock(vm);
    vm.apply_event(event(R"({"event":"diag","id":"CML2101","severity":"error","stage":"compile","pid":7,
        "message":"compile of player.cpp failed\nC:\\src\\player.cpp(42,17): error C2065: 'speed': undeclared identifier\nC:\\src\\player.cpp(50): warning C4100: 'x': unreferenced\n  see also",
        "action":"Fix the error and save again."})"));
    REQUIRE(vm.output().size() == 5);
    const auto& o = vm.output();
    CHECK(o[0].diag_id == "CML2101");
    CHECK(o[0].severity == OutputSeverity::Error);
    CHECK_FALSE(o[0].location.clickable());
    CHECK(o[1].location.clickable());
    CHECK(o[1].location.file == R"(C:\src\player.cpp)");
    CHECK(o[1].location.line == 42);
    CHECK(o[1].location.column == 17);
    CHECK(o[2].severity == OutputSeverity::Warning);
    CHECK(o[2].location.line == 50);
    CHECK(o[3].severity == OutputSeverity::Info);
    CHECK(o[4].text == "-> Fix the error and save again.");
    CHECK(vm.error_count() == 2);   // the header line and the C2065 line
    CHECK(vm.warning_count() == 1);
    CHECK(o[1].pid == 7);
    CHECK(o[1].stage == "compile");
    vm.clear_output();
    CHECK(vm.output().empty());
}

TEST_CASE("patch_applied creates history; a reload result attaches the timings") {
    ViewModel vm;
    fixed_clock(vm);
    vm.apply_event(event(R"({"event":"patch_applied","pid":10,"module":"g.exe","generation":3,"hooks":4,"new_functions":1,"new_globals":0})"));
    REQUIRE(vm.patches().size() == 1);
    CHECK_FALSE(vm.patches()[0].timing.present);
    CHECK(vm.patches()[0].hooks == 4);
    CHECK(vm.patches()[0].when_ms == 1000);
    CHECK(vm.generation() == 3);
    ReloadSummary r;
    r.generation = 3;
    r.patched = 1;
    ReloadPatch p;
    p.pid = 10;
    p.module = "g.exe";
    p.generation = 3;
    p.hooks_installed = 4;
    p.timing.present = true;
    p.timing.total = 123.4;
    r.patches.push_back(p);
    r.changed_files = {R"(C:\src\a.cpp)"};
    vm.apply_reload_result(r);
    REQUIRE(vm.patches().size() == 1);  // merged, not duplicated
    CHECK(vm.patches()[0].timing.present);
    CHECK(vm.patches()[0].timing.total == doctest::Approx(123.4));
    // the change list has the module entry (event) and the file (reload answer)
    bool file_listed = false;
    for (const ChangeRecord& c : vm.changes()) {
        file_listed = file_listed || (c.path == R"(C:\src\a.cpp)" && c.state == "patched");
    }
    CHECK(file_listed);
    CHECK(vm.status_message().find("Patched 1") == 0);
}

TEST_CASE("a reload answer without a matching event still makes a history row") {
    ViewModel vm;
    fixed_clock(vm);
    ReloadSummary r;
    r.patched = 2;
    r.generation = 9;
    r.timing.present = true;
    r.timing.total = 50;
    vm.apply_reload_result(r);
    REQUIRE(vm.patches().size() == 1);
    CHECK(vm.patches()[0].generation == 9);
}

TEST_CASE("log events are filtered by level and text") {
    ViewModel vm;
    fixed_clock(vm);
    vm.apply_event(event(R"({"event":"log","level":"debug","message":"scanning headers"})"));
    vm.apply_event(event(R"({"event":"log","level":"info","message":"reload started"})"));
    vm.apply_event(event(R"({"event":"log","level":"warn","message":"Header Scan slow"})"));
    vm.apply_event(event(R"({"event":"log","level":"error","message":"link failed"})"));
    CHECK(vm.log().size() == 4);
    LogFilter filter;
    filter.min_level = LogLevel::Info;
    CHECK(vm.filtered_log(filter) == std::vector<std::size_t>{1, 2, 3});
    filter.min_level = LogLevel::Trace;
    CHECK(vm.filtered_log(filter).size() == 4);
    filter.min_level = LogLevel::Warn;
    CHECK(vm.filtered_log(filter) == std::vector<std::size_t>{2, 3});
    filter.min_level = LogLevel::Trace;
    filter.text = "SCAN";
    CHECK(vm.filtered_log(filter) == std::vector<std::size_t>{0, 2});
}

TEST_CASE("the log is bounded") {
    ViewModel vm;
    fixed_clock(vm);
    for (std::size_t i = 0; i < ViewModel::kMaxLog + 50; ++i) {
        vm.add_log(LogLevel::Info, "line " + std::to_string(i));
    }
    CHECK(vm.log().size() == ViewModel::kMaxLog);
    CHECK(vm.log().front().message == "line 50");
}

TEST_CASE("watcher diagnostics reach the change list") {
    ViewModel vm;
    fixed_clock(vm);
    vm.apply_event(event(R"({"event":"diag","id":"CML5501","severity":"warning","stage":"watch","message":"file events were lost under C:\\src","action":"none"})"));
    REQUIRE(vm.changes().size() == 1);
    CHECK(vm.changes()[0].state == "watch");
    CHECK(vm.changes()[0].detail == "CML5501");
    CHECK(vm.output().size() >= 1);
}

TEST_CASE("progress events and their reset") {
    ViewModel vm;
    fixed_clock(vm);
    vm.apply_event(event(R"({"event":"progress","stage":"compile","pid":1,"module":"g.exe","done":2,"total":5})"));
    REQUIRE(vm.progress());
    CHECK(vm.progress()->done == 2);
    CHECK(vm.progress()->total == 5);
    vm.apply_event(event(R"({"event":"patch_applied","pid":1,"module":"g.exe","generation":1})"));
    CHECK_FALSE(vm.progress());
}

TEST_CASE("unknown events are counted and ignored") {
    ViewModel vm;
    fixed_clock(vm);
    CHECK_FALSE(vm.apply_event(event(R"({"event":"from_the_future","x":1})")));
    CHECK(vm.ignored_events() == 1);
}

TEST_CASE("exception events (ADR-0032) are queued for the modal") {
    ViewModel vm;
    fixed_clock(vm);
    const auto e = event(R"({"event":"exception","pid":10,"tid":11,"exception_id":5,"code":"0xC0000005","address":"0x7ff612340000",
        "module":"g.exe","function":"Player::Update","offset":"0x2c","actions":["continue","leave","ignore"]})");
    CHECK(vm.apply_event(e));
    CHECK(vm.apply_event(e));  // the same exception again (event plus `exceptions` answer) is not queued twice
    REQUIRE(vm.pending_exceptions().size() == 1);
    const ExceptionRow* row = vm.current_exception();
    REQUIRE(row != nullptr);
    CHECK(row->function == "Player::Update");
    CHECK(row->code == "0xC0000005");
    CHECK(row->actions.size() == 3);
    vm.apply_event(event(R"({"event":"exception","pid":10,"tid":11,"exception_id":6,"code":"0xC0000094","module":"g.exe"})"));
    CHECK(vm.pending_exceptions().size() == 2);
    vm.resolve_exception(10, 5);
    REQUIRE(vm.current_exception() != nullptr);
    CHECK(vm.current_exception()->exception_id == 6);
    CHECK(vm.current_exception()->actions.size() == 3);  // default actions when the event names none
    vm.resolve_exception(10, 6);
    CHECK(vm.current_exception() == nullptr);
}

TEST_CASE("restarted and patch_reverted events (ADR-0032)") {
    ViewModel vm;
    fixed_clock(vm);
    vm.apply_event(event(R"({"event":"patch_applied","pid":10,"module":"g.exe","generation":1})"));
    vm.apply_event(event(R"({"event":"exception","pid":10,"tid":1,"exception_id":1,"code":"0x1"})"));
    vm.apply_event(event(R"({"event":"patch_reverted","pid":10,"module":"g.exe","generation":1})"));
    CHECK(vm.patches()[0].reverted);
    vm.apply_event(event(R"({"event":"restarted","old_pid":10,"new_pid":11})"));
    CHECK(vm.pending_exceptions().empty());
    CHECK(vm.status_message().find("11") != std::string::npos);
}

TEST_CASE("CML5551 arms the confirm prompt") {
    ViewModel vm;
    fixed_clock(vm);
    cml::Diag d = cml::Diag::make(5551, "the change affects 90 compilands, more than the limit of 50", "Repeat with confirm.");
    vm.apply_command_failure("reload", d, 10, "g.exe");
    REQUIRE(vm.confirm_prompt());
    CHECK(vm.confirm_prompt()->pid == 10);
    CHECK(vm.confirm_prompt()->module == "g.exe");
    CHECK(vm.status_severity() == StatusSeverity::Warning);
    vm.clear_confirm_prompt();
    CHECK_FALSE(vm.confirm_prompt());
}

TEST_CASE("CML5402 disables the feature instead of failing") {
    ViewModel vm;
    fixed_clock(vm);
    cml::ui::test::FakeBroker broker({"status", "reload", "restart"});
    vm.apply_status(broker.status);
    CHECK(vm.feature_available(Feature::Restart));
    vm.apply_command_failure("restart", cml::Diag::make(5402, "control command 'restart' is not provided by this broker", "x"));
    CHECK_FALSE(vm.feature_available(Feature::Restart));
    CHECK(vm.feature_available(Feature::Reload));
    CHECK(vm.output().empty());  // not an error the user has to read
    CHECK(vm.status_severity() == StatusSeverity::Warning);
}

TEST_CASE("a command failure and its diag event show the diagnostic once, in either order") {
    cml::Diag d = cml::Diag::make(5300, "no enabled module to reload", "Enable a module.");
    const auto ev = event(R"({"event":"diag","id":"CML5300","severity":"error","stage":"reload","message":"no enabled module to reload","action":"Enable a module."})");
    {
        ViewModel vm;
    fixed_clock(vm);
        vm.apply_event(ev);
        const std::size_t after_event = vm.output().size();
        vm.apply_command_failure("reload", d);
        CHECK(vm.output().size() == after_event);
    }
    {
        ViewModel vm;
    fixed_clock(vm);
        vm.apply_command_failure("reload", d);
        const std::size_t after_failure = vm.output().size();
        CHECK(after_failure > 0);
        vm.apply_event(ev);
        CHECK(vm.output().size() == after_failure);
    }
}

TEST_CASE("connection loss clears the broker's reload flag") {
    ViewModel vm;
    fixed_clock(vm);
    cml::ipc::StatusReport report;
    report.reload_running = true;
    vm.apply_status(report);
    CHECK(vm.busy());
    vm.set_connection(ConnectionState::Failed, "lost");
    CHECK_FALSE(vm.busy());
    CHECK(vm.connection() == ConnectionState::Failed);
}

TEST_CASE("log level names") {
    LogLevel l = LogLevel::Info;
    CHECK(parse_log_level("warning", &l));
    CHECK(l == LogLevel::Warn);
    CHECK_FALSE(parse_log_level("loud", &l));
    CHECK(std::string(to_string(LogLevel::Error)) == "error");
}

// --- review fixes --------------------------------------------------------------------------------------------------

TEST_CASE("a new diagnostic with the same id but another text is never dropped") {
    ViewModel vm;
    fixed_clock(vm);
    cml::Diag a = cml::Diag::make(5401, "bad request: argument 'pid' must be an integer", "Fix pid.");
    cml::Diag b = cml::Diag::make(5401, "bad request: argument 'module' must be a string", "Fix module.");
    vm.apply_command_failure("reload", a);
    vm.apply_command_failure("reload", b);
    std::size_t headers = 0;
    for (const OutputLine& l : vm.output()) {
        headers += l.diag_id == "CML5401" ? 1 : 0;
    }
    CHECK(headers == 2);
    // the same through events, with compile locations that differ
    vm.apply_event(event(R"({"event":"diag","id":"CML2101","severity":"error","stage":"compile","message":"a.cpp(1,1): error C1: x"})"));
    vm.apply_event(event(R"({"event":"diag","id":"CML2101","severity":"error","stage":"compile","message":"b.cpp(2,2): error C2: y"})"));
    std::size_t located = 0;
    for (const OutputLine& l : vm.output()) {
        located += l.location.clickable() ? 1 : 0;
    }
    CHECK(located == 2);
    // and a later event with the first command failure's id and text is still the same diagnostic: shown once
    const auto ev = event(R"({"event":"diag","id":"CML5401","severity":"error","stage":"reload","message":"bad request: argument 'pid' must be an integer","action":"Fix pid."})");
    const std::size_t before = vm.output().size();
    vm.apply_event(ev);
    CHECK(vm.output().size() == before);
}

TEST_CASE("every reload trigger passes one guard") {
    ViewModel vm;
    cml::ui::test::FakeBroker broker;
    vm.apply_status(broker.status);
    std::string why;
    CHECK_FALSE(vm.try_begin_reload(&why));  // not connected yet
    CHECK(why.find("Not connected") == 0);
    vm.set_connection(ConnectionState::Connected, "ok");
    CHECK(vm.try_begin_reload(&why));
    CHECK(vm.reload_in_flight());
    CHECK_FALSE(vm.try_begin_reload(&why));  // a second trigger before the first one ran
    CHECK(why.find("already") != std::string::npos);
    vm.set_reload_in_flight(false);
    CHECK(vm.try_begin_reload());
    vm.set_connection(ConnectionState::Failed, "lost");  // the queued task will never run
    CHECK_FALSE(vm.reload_in_flight());
    vm.set_connection(ConnectionState::Connected, "ok");
    CHECK(vm.try_begin_reload());
    vm.set_reload_in_flight(false);
    // the broker is reloading for somebody else
    cml::ipc::StatusReport busy = broker.status;
    busy.reload_running = true;
    vm.apply_status(busy);
    CHECK_FALSE(vm.try_begin_reload());
    // no reload capability
    cml::ui::test::FakeBroker without({"status"});
    vm.apply_status(without.status);
    CHECK_FALSE(vm.try_begin_reload(&why));
    CHECK(why.find("no 'reload'") != std::string::npos);
}

TEST_CASE("the exceptions answer is authoritative, and a lost connection clears the list") {
    ViewModel vm;
    fixed_clock(vm);
    vm.apply_event(event(R"({"event":"exception","pid":10,"tid":1,"exception_id":1,"code":"0x1"})"));
    vm.apply_event(event(R"({"event":"exception","pid":10,"tid":1,"exception_id":2,"code":"0x2"})"));
    REQUIRE(vm.pending_exceptions().size() == 2);
    ExceptionRow keep;
    keep.pid = 10;
    keep.exception_id = 2;
    keep.code = "0x2";
    vm.set_pending_exceptions({keep});
    REQUIRE(vm.pending_exceptions().size() == 1);
    CHECK(vm.current_exception()->exception_id == 2);
    vm.set_pending_exceptions({});
    CHECK(vm.current_exception() == nullptr);
    vm.apply_event(event(R"({"event":"exception","pid":10,"tid":1,"exception_id":3,"code":"0x3"})"));
    vm.set_connection(ConnectionState::Failed, "lost");
    CHECK(vm.pending_exceptions().empty());
}

TEST_CASE("one CML5551 prompt per reload request") {
    ViewModel vm;
    fixed_clock(vm);
    cml::ipc::StatusReport report;
    report.capabilities = {"status", "reload"};
    vm.apply_status(report);
    vm.set_connection(ConnectionState::Connected, "ok");
    const auto prompt_event = event(R"({"event":"diag","id":"CML5551","severity":"error","stage":"sweep","pid":10,"message":"the change affects 90 compilands","action":"confirm"})");
    const cml::Diag failure = cml::Diag::make(5551, "the change affects 90 compilands", "confirm");

    REQUIRE(vm.try_begin_reload());
    const std::uint64_t serial = vm.begin_reload_request();
    vm.apply_event(prompt_event);  // the event comes first
    REQUIRE(vm.confirm_prompt());
    vm.clear_confirm_prompt();      // the user answers at once
    vm.apply_command_failure("reload", failure, 0, "", serial);  // the answer of the same request arrives afterwards
    CHECK_FALSE(vm.confirm_prompt());
    vm.apply_event(prompt_event);   // and a repeated event of the same request
    CHECK_FALSE(vm.confirm_prompt());
    vm.set_reload_in_flight(false);

    // the next request gets its own prompt, from either path
    REQUIRE(vm.try_begin_reload());
    const std::uint64_t second = vm.begin_reload_request();
    CHECK(second != serial);
    vm.apply_command_failure("reload", failure, 0, "", second);
    CHECK(vm.confirm_prompt());
    // a stale answer of an older request does not arm anything
    vm.clear_confirm_prompt();
    vm.apply_command_failure("reload", failure, 0, "", serial);
    CHECK_FALSE(vm.confirm_prompt());
}

TEST_CASE("error and warning counters follow push, trim and clear") {
    ViewModel vm;
    fixed_clock(vm);
    for (std::size_t i = 0; i < ViewModel::kMaxOutput + 25; ++i) {
        vm.add_output_text("a.cpp(1): error C1: e", OutputSeverity::Error, "t");
    }
    vm.add_output_text("a.cpp(1): warning C2: w", OutputSeverity::Warning, "t");
    CHECK(vm.output().size() == ViewModel::kMaxOutput);
    CHECK(vm.error_count() == ViewModel::kMaxOutput - 1);  // the warning took one slot
    CHECK(vm.warning_count() == 1);
    vm.clear_output();
    CHECK(vm.error_count() == 0);
    CHECK(vm.warning_count() == 0);
}
