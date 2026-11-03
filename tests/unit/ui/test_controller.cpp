#include "cml/ui/controller.h"

#include "doctest.h"
#include "fake_broker.h"

using namespace cml::ui;
using cml::ui::test::FakeBroker;
using cml::ui::test::parse;
namespace ipc = cml::ipc;
namespace json = cml::ipc::json;

namespace {

const std::vector<std::string> kWave6 = {"status", "reload", "wait", "set-option", "subscribe", "diag-events", "exceptions", "restart", "revert"};

std::size_t count_calls(const FakeBroker& broker, const std::string& cmd) {
    std::size_t n = 0;
    for (const auto& c : broker.calls) {
        n += c.first == cmd ? 1 : 0;
    }
    return n;
}

}  // namespace

TEST_CASE("refresh_status fills the view model; subscribe asks for the events the capabilities allow") {
    FakeBroker broker(kWave6);
    broker.handlers["subscribe"] = [](const json::Value& args) {
        ipc::ControlResponse r;
        r.id = 1;
        r.ok = true;
        r.result.set("events", *args.find("events"));
        return r;
    };
    ViewModel vm;
    Controller controller(vm, broker);
    REQUIRE(controller.refresh_status());
    CHECK(vm.broker_version() == "0.1.0-test");
    CHECK(vm.has_capability("revert"));
    REQUIRE(controller.subscribe());
    const json::Value& args = broker.calls.back().second;
    CHECK(json::serialize(*args.find("events")) == R"(["diag","patch_applied","progress","log","exception","restarted","patch_reverted"])");
}

TEST_CASE("reload: request, result, in-flight flag") {
    FakeBroker broker;
    bool in_flight_during_call = false;
    ViewModel vm;
    broker.handlers["reload"] = [&](const json::Value& args) {
        in_flight_during_call = vm.reload_in_flight();
        CHECK(args.get_integer("pid") == 4242);
        CHECK(args.get_string("module") == "game.exe");
        ipc::ControlResponse r;
        r.id = 1;
        r.ok = true;
        r.result = parse(R"({"generation":1,"patched":1,"hooks_installed":3,"changed_files":["a.cpp"],
            "timing_ms":{"total":80},"patches":[{"pid":4242,"module":"game.exe","generation":1,"timing_ms":{"compile":50,"total":80}}]})");
        return r;
    };
    Controller controller(vm, broker);
    REQUIRE(controller.refresh_status());
    const auto result = controller.reload(4242, "game.exe", false);
    REQUIRE(result);
    CHECK(in_flight_during_call);
    CHECK_FALSE(vm.reload_in_flight());
    CHECK(result->patched == 1);
    REQUIRE(vm.patches().size() == 1);
    CHECK(vm.patches()[0].timing.compile == doctest::Approx(50));
    CHECK(vm.generation() == 1);
}

TEST_CASE("reload failure surfaces the broker's diagnostic") {
    FakeBroker broker;
    broker.handlers["reload"] = [](const json::Value&) {
        return ipc::make_error_response(1, cml::Diag::make(5300, "no enabled module to reload", "Enable a module."));
    };
    ViewModel vm;
    Controller controller(vm, broker);
    REQUIRE(controller.refresh_status());
    const auto result = controller.reload(0, "", false);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == 5300);
    CHECK(vm.status_severity() == StatusSeverity::Error);
    CHECK_FALSE(vm.output().empty());
    CHECK_FALSE(vm.reload_in_flight());
}

TEST_CASE("CML5551: the prompt appears, confirming repeats the reload with confirm=true") {
    FakeBroker broker;
    int attempts = 0;
    broker.handlers["reload"] = [&](const json::Value& args) {
        ++attempts;
        if (!args.get_bool("confirm")) {
            return ipc::make_error_response(1, cml::Diag::make(5551, "the change affects 90 compilands", "Repeat the reload with \"confirm\": true"));
        }
        ipc::ControlResponse r;
        r.id = 1;
        r.ok = true;
        r.result = parse(R"({"generation":2,"patched":90})");
        return r;
    };
    ViewModel vm;
    Controller controller(vm, broker);
    REQUIRE(controller.refresh_status());
    REQUIRE_FALSE(controller.reload(0, "", false));
    REQUIRE(vm.confirm_prompt());
    const auto confirmed = controller.reload(vm.confirm_prompt()->pid, vm.confirm_prompt()->module, true);
    REQUIRE(confirmed);
    CHECK(attempts == 2);
    CHECK_FALSE(vm.confirm_prompt());
    CHECK(confirmed->patched == 90);
}

TEST_CASE("commands the broker does not advertise are not sent") {
    FakeBroker broker;  // WP-12 capabilities only
    ViewModel vm;
    Controller controller(vm, broker);
    REQUIRE(controller.refresh_status());
    const std::size_t before = broker.calls.size();
    const auto restart = controller.restart(4242);
    REQUIRE_FALSE(restart);
    CHECK(restart.error().code == 5702);
    const auto revert = controller.revert(0, "", "all");
    REQUIRE_FALSE(revert);
    CHECK(revert.error().code == 5702);
    CHECK_FALSE(controller.refresh_exceptions());
    CHECK_FALSE(controller.exception_action(1, 1, "continue"));
    CHECK(broker.calls.size() == before);
}

TEST_CASE("an advertised command the broker answers with CML5402 is treated as not available") {
    FakeBroker broker(kWave6);  // advertises restart but has no handler for it
    broker.handlers.erase("restart");
    ViewModel vm;
    Controller controller(vm, broker);
    REQUIRE(controller.refresh_status());
    CHECK(vm.feature_available(Feature::Restart));
    // The fake answers CML5402 because there is no handler.
    const auto restart = controller.restart(4242);
    REQUIRE_FALSE(restart);
    CHECK(restart.error().code == 5402);
    CHECK_FALSE(vm.feature_available(Feature::Restart));
    CHECK(count_calls(broker, "restart") == 1);
    // A second attempt is stopped by the UI before it reaches the pipe.
    CHECK(controller.restart(4242).error().code == 5702);
    CHECK(count_calls(broker, "restart") == 1);
}

TEST_CASE("ADR-0032: restart, revert, exceptions against a scripted broker") {
    FakeBroker broker(kWave6);
    broker.handlers["restart"] = [](const json::Value& args) {
        CHECK(args.get_integer("pid") == 4242);
        ipc::ControlResponse r;
        r.id = 1;
        r.ok = true;
        r.result = parse(R"({"old_pid":4242,"new_pid":5000,"modules":["C:\\game\\game.exe"]})");
        return r;
    };
    broker.handlers["revert"] = [](const json::Value& args) {
        CHECK(args.get_string("scope") == "last");
        ipc::ControlResponse r;
        r.id = 1;
        r.ok = true;
        r.result = parse(R"({"reverted":[{"pid":4242,"module":"game.exe","generation":1}],"generation":0})");
        return r;
    };
    broker.handlers["exceptions"] = [](const json::Value&) {
        ipc::ControlResponse r;
        r.id = 1;
        r.ok = true;
        r.result = parse(R"({"pending":[{"event":"exception","pid":4242,"tid":2,"exception_id":9,"code":"0xC0000005","module":"game.exe",
            "function":"f","actions":["continue","leave"]}]})");
        return r;
    };
    broker.handlers["exception-action"] = [](const json::Value& args) {
        ipc::ControlResponse r;
        r.id = 1;
        r.ok = true;
        r.result = args;
        return r;
    };
    ViewModel vm;
    vm.apply_event(cml::ui::test::event(R"({"event":"patch_applied","pid":4242,"module":"game.exe","generation":1})"));
    Controller controller(vm, broker);
    REQUIRE(controller.refresh_status());

    REQUIRE(controller.refresh_exceptions());
    REQUIRE(vm.current_exception() != nullptr);
    CHECK(vm.current_exception()->exception_id == 9);
    CHECK(vm.current_exception()->actions.size() == 2);
    REQUIRE(controller.exception_action(4242, 9, "leave"));
    CHECK(vm.current_exception() == nullptr);
    CHECK(json::serialize(broker.calls.back().second) == R"({"pid":4242,"exception_id":9,"action":"leave"})");

    const auto reverted = controller.revert(0, "", "last");
    REQUIRE(reverted);
    CHECK(vm.patches()[0].reverted);

    const auto restarted = controller.restart(4242);
    REQUIRE(restarted);
    CHECK(restarted->new_pid == 5000);
}

TEST_CASE("a transport failure is returned and shown") {
    FakeBroker broker;
    ViewModel vm;
    Controller controller(vm, broker);
    REQUIRE(controller.refresh_status());
    broker.disconnected = true;
    const auto r = controller.refresh_status();
    REQUIRE_FALSE(r);
    CHECK(r.error().code == 5405);
    CHECK(vm.status_severity() == StatusSeverity::Error);
}

TEST_CASE("apply_settings sends set-option for each setting and reports the first failure") {
    FakeBroker broker;
    std::vector<std::string> names;
    broker.handlers["set-option"] = [&](const json::Value& args) {
        names.push_back(args.get_string("name"));
        if (args.get_string("name") == "max_parallel_compiles") {
            return ipc::make_error_response(1, cml::Diag::make(5311, "bad value for max_parallel_compiles", "use 1 to 64"));
        }
        ipc::ControlResponse r;
        r.id = 1;
        r.ok = true;
        r.result = parse(R"({"name":"x","previous":false})");
        return r;
    };
    ViewModel vm;
    Controller controller(vm, broker);
    REQUIRE(controller.refresh_status());
    Settings s;
    s.compile_on_save = true;
    s.jobs = 999;
    s.max_affected_compilands = 10;
    const auto r = controller.apply_settings(s);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == 5311);
    CHECK(names == std::vector<std::string>{"compile_on_save", "max_parallel_compiles", "max_affected_compilands"});
}

TEST_CASE("wait and set_option") {
    FakeBroker broker;
    broker.handlers["wait"] = [](const json::Value& args) {
        CHECK(args.get_string("for") == "idle");
        ipc::ControlResponse r;
        r.id = 1;
        r.ok = true;
        r.result = parse(R"({"for":"idle","modules":1,"failed":[],"waited_ms":3})");
        return r;
    };
    ViewModel vm;
    Controller controller(vm, broker);
    REQUIRE(controller.refresh_status());
    CHECK(controller.wait("idle", 1000));
}
