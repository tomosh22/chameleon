// The broker's per-agent session state machine, driven directly with messages (no pipes).
#include "cml/core/session.h"

#include <string>
#include <variant>

#include "cml/proc/diag_ids.h"
#include "cml/util/log.h"
#include "doctest.h"

using namespace cml;
using namespace cml::core;

namespace {

// Keeps test output quiet; restores the previous level afterwards.
struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

ipc::Hello hello(std::uint64_t id = 1, std::uint32_t pid = 4321) {
    ipc::Hello m;
    m.request_id = id;
    m.pid = pid;
    m.agent_path = "C:\\agent\\cml_agent.dll";
    m.command_line = "game.exe";
    return m;
}

ipc::ModuleList module_list(std::uint64_t id) {
    ipc::ModuleList m;
    m.request_id = id;
    m.modules.push_back({0x140000000ull, 0x30000, "C:\\Games\\Demo\\game.exe"});
    m.modules.push_back({0x180000000ull, 0x10000, "C:\\Games\\Demo\\Game.dll"});
    m.modules.push_back({0x7FF800000000ull, 0x90000, "C:\\Windows\\System32\\kernel32.dll"});
    return m;
}

ipc::EnableModule enable(std::uint64_t id, const char* path) {
    ipc::EnableModule m;
    m.request_id = id;
    m.path = path;
    return m;
}

const ipc::Reply& only_reply(const SessionOutcome& outcome) {
    REQUIRE(outcome.replies.size() == 1);
    REQUIRE(std::holds_alternative<ipc::Reply>(outcome.replies[0]));
    return std::get<ipc::Reply>(outcome.replies[0]);
}

// A session that finished the handshake and received the module list.
struct Registered {
    Session session{1, "0.1.0"};
    Registered() {
        session.on_message(hello());
        session.on_message(module_list(2));
    }
};

}  // namespace

TEST_CASE("Hello is answered with Welcome carrying the same request id") {
    QuietLogs quiet;
    Session session(5, "9.9.9");
    CHECK_FALSE(session.handshaken());
    const SessionOutcome outcome = session.on_message(hello(11, 777));
    CHECK_FALSE(outcome.close);
    REQUIRE(outcome.replies.size() == 1);
    REQUIRE(std::holds_alternative<ipc::Welcome>(outcome.replies[0]));
    const auto& welcome = std::get<ipc::Welcome>(outcome.replies[0]);
    CHECK(welcome.request_id == 11);
    CHECK(welcome.agent_id == 5);
    CHECK(welcome.broker_version == "9.9.9");
    CHECK(welcome.protocol_version == ipc::kProtocolVersion);
    CHECK(session.handshaken());
    CHECK(session.pid() == 777);
    CHECK(session.agent_path() == "C:\\agent\\cml_agent.dll");
    CHECK(session.command_line() == "game.exe");
}

TEST_CASE("a protocol version the broker does not speak is refused with CML5210") {
    QuietLogs quiet;
    Session session(1, "0.1.0");
    ipc::Hello h = hello();
    h.protocol_version = ipc::kProtocolVersion + 1;
    const SessionOutcome outcome = session.on_message(h);
    CHECK(outcome.close);
    CHECK(only_reply(outcome).status == ids::kHandshakeFailed);
    CHECK(only_reply(outcome).request_id == 1);
    CHECK_FALSE(session.handshaken());
}

TEST_CASE("anything before Hello, and a second Hello, is a protocol violation") {
    QuietLogs quiet;
    SUBCASE("request before Hello") {
        Session session(1, "0.1.0");
        const SessionOutcome outcome = session.on_message(enable(3, "game.exe"));
        CHECK(outcome.close);
        CHECK(only_reply(outcome).status == ids::kProtocolViolation);
    }
    SUBCASE("second Hello") {
        Session session(1, "0.1.0");
        session.on_message(hello());
        const SessionOutcome outcome = session.on_message(hello(2));
        CHECK(outcome.close);
        CHECK(only_reply(outcome).status == ids::kProtocolViolation);
        CHECK(only_reply(outcome).request_id == 2);
    }
    SUBCASE("messages that only the broker may send") {
        Registered r;
        ipc::Welcome w;
        const SessionOutcome outcome = r.session.on_message(w);
        CHECK(outcome.close);
        CHECK(only_reply(outcome).status == ids::kProtocolViolation);
    }
}

TEST_CASE("ModuleList is recorded and answered") {
    QuietLogs quiet;
    Session session(1, "0.1.0");
    session.on_message(hello());
    const SessionOutcome outcome = session.on_message(module_list(2));
    CHECK_FALSE(outcome.close);
    CHECK(only_reply(outcome).status == 0);
    CHECK(only_reply(outcome).request_id == 2);
    CHECK(session.modules().size() == 3);
}

TEST_CASE("EnableModule succeeds for a module in the process's list") {
    QuietLogs quiet;
    Registered r;
    SUBCASE("full path") {
        const ipc::Reply reply = only_reply(r.session.on_message(enable(10, "C:\\Games\\Demo\\game.exe")));
        CHECK(reply.status == 0);
        CHECK(reply.request_id == 10);
        CHECK(reply.message.find("game.exe") != std::string::npos);
        REQUIRE(r.session.enabled_modules().size() == 1);
        CHECK(r.session.enabled_modules()[0] == "C:\\Games\\Demo\\game.exe");
    }
    SUBCASE("case and slash style do not matter") {
        CHECK(only_reply(r.session.on_message(enable(10, "c:/games/demo/GAME.EXE"))).status == 0);
        CHECK(only_reply(r.session.on_message(enable(11, "\\\\?\\C:\\Games\\Demo\\game.exe"))).status == 0);
        CHECK(r.session.enabled_modules().size() == 1);  // all three are the same module
    }
    SUBCASE("a bare file name matches by name") {
        CHECK(only_reply(r.session.on_message(enable(10, "game.dll"))).status == 0);
        REQUIRE(r.session.enabled_modules().size() == 1);
        CHECK(r.session.enabled_modules()[0] == "C:\\Games\\Demo\\Game.dll");
    }
    SUBCASE("enabling twice is idempotent") {
        r.session.on_message(enable(10, "game.exe"));
        const ipc::Reply again = only_reply(r.session.on_message(enable(11, "game.exe")));
        CHECK(again.status == 0);
        CHECK(again.message.find("already enabled") != std::string::npos);
        CHECK(r.session.enabled_modules().size() == 1);
    }
}

TEST_CASE("EnableModule for a module that is not loaded fails with CML5212 naming the module") {
    QuietLogs quiet;
    Registered r;
    const SessionOutcome outcome = r.session.on_message(enable(10, "C:\\Elsewhere\\game.exe"));
    CHECK_FALSE(outcome.close);  // an error reply, not a protocol violation
    const ipc::Reply& reply = only_reply(outcome);
    CHECK(reply.status == ids::kModuleNotLoaded);
    CHECK(reply.request_id == 10);
    CHECK(reply.message.find("C:\\Elsewhere\\game.exe") != std::string::npos);
    CHECK(reply.message.find("4321") != std::string::npos);  // names the process
    CHECK(r.session.enabled_modules().empty());
    // A bare name must not match a module with a different directory requirement either.
    CHECK(only_reply(r.session.on_message(enable(11, "nothing.dll"))).status == ids::kModuleNotLoaded);
}

TEST_CASE("DisableModule") {
    QuietLogs quiet;
    Registered r;
    ipc::DisableModule d;
    d.request_id = 20;
    d.path = "game.exe";
    CHECK(only_reply(r.session.on_message(d)).status == ids::kModuleNotEnabled);

    r.session.on_message(enable(21, "game.exe"));
    d.request_id = 22;
    const ipc::Reply ok = only_reply(r.session.on_message(d));
    CHECK(ok.status == 0);
    CHECK(ok.request_id == 22);
    CHECK(r.session.enabled_modules().empty());

    d.request_id = 23;
    CHECK(only_reply(r.session.on_message(d)).status == ids::kModuleNotEnabled);
}

TEST_CASE("ModuleLoaded and ModuleUnloaded keep the module list current") {
    QuietLogs quiet;
    Registered r;
    CHECK(only_reply(r.session.on_message(enable(30, "late.dll"))).status == ids::kModuleNotLoaded);

    ipc::ModuleLoaded loaded;
    loaded.module = {0x200000000ull, 0x5000, "C:\\Games\\Demo\\late.dll"};
    const SessionOutcome event = r.session.on_message(loaded);
    CHECK(event.replies.empty());  // events get no answer
    CHECK(r.session.modules().size() == 4);
    CHECK(only_reply(r.session.on_message(enable(31, "late.dll"))).status == 0);

    // The same base reported again replaces the entry instead of duplicating it.
    r.session.on_message(loaded);
    CHECK(r.session.modules().size() == 4);

    ipc::ModuleUnloaded unloaded;
    unloaded.module = loaded.module;
    CHECK(r.session.on_message(unloaded).replies.empty());
    CHECK(r.session.modules().size() == 3);
    CHECK(only_reply(r.session.on_message(enable(32, "C:\\Games\\Demo\\late.dll"))).status ==
          ids::kModuleNotLoaded);
}

TEST_CASE("Log messages are accepted without an answer") {
    QuietLogs quiet;
    Registered r;
    ipc::Log log;
    log.level = 3;
    log.text = "something odd";
    const SessionOutcome outcome = r.session.on_message(log);
    CHECK(outcome.replies.empty());
    CHECK_FALSE(outcome.close);
}

TEST_CASE("same_path normalisation") {
    CHECK(Session::same_path("C:\\a\\b.dll", "c:/A/B.DLL"));
    CHECK(Session::same_path("\\\\?\\C:\\a\\b.dll", "C:\\a\\b.dll"));
    CHECK_FALSE(Session::same_path("C:\\a\\b.dll", "C:\\a\\c.dll"));
    CHECK_FALSE(Session::same_path("C:\\a\\b.dll", "C:\\a\\b.dll "));
    CHECK(Session::same_path("", ""));
}
