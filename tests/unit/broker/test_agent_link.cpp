// AgentLink (WP-12): the request/answer exchange between the reload pipeline and the connection thread, and the
// Session's treatment of the agent's answers (WP-10 note: unknown request ids are ignored; HooksInstalled is
// accepted).
#include "cml/core/agent_link.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "cml/proc/diag_ids.h"
#include "cml/util/log.h"
#include "doctest.h"

using namespace cml;
using namespace cml::core;

namespace {

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

ipc::Hello hello(ipc::AgentMode mode = ipc::AgentMode::Immediate) {
    ipc::Hello m;
    m.request_id = 1;
    m.pid = 99;
    m.mode = mode;
    m.agent_path = "C:\\agent\\cml_agent.dll";
    m.command_line = "game.exe";
    return m;
}

ipc::ReserveRange reserve(std::uint64_t id) {
    ipc::ReserveRange r;
    r.request_id = id;
    return r;
}

ipc::ReserveRangeResult reserve_result(std::uint64_t id) {
    ipc::ReserveRangeResult r;
    r.request_id = id;
    r.blocks.push_back({0x100000000ull, 64ull << 20});
    return r;
}

}  // namespace

TEST_CASE("request ids: read from and written to the message types that carry one") {
    ipc::Message m = ipc::LoadPatch{};
    CHECK(set_request_id(m, 77));
    CHECK(request_id_of(m) == 77);
    ipc::Message events = ipc::Log{};
    CHECK_FALSE(set_request_id(events, 1));
    CHECK_FALSE(request_id_of(events).has_value());
    CHECK(request_id_of(ipc::HooksInstalled{9, 0, "", 0, 0, 0, 0, {}}) == 9);
    CHECK(request_id_of(ipc::PatchLoaded{}) == 0);
}

TEST_CASE("a request completes when the answer with its id is delivered") {
    QuietLogs quiet;
    std::vector<ipc::Message> sent;
    std::atomic<AgentLink*> self{nullptr};
    AgentLink link(7, [&](const ipc::Message& m) -> Result<void> {
        sent.push_back(m);
        // The "agent" answers from another thread, a little later.
        const std::uint64_t id = *request_id_of(m);
        std::thread([&, id] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            self.load()->deliver(reserve_result(id));
        }).detach();
        return Result<void>();
    });
    self = &link;
    const std::uint64_t id = link.next_request_id();
    Result<ipc::Message> answer = link.request(reserve(id), 5000);
    REQUIRE(answer.has_value());
    REQUIRE(std::holds_alternative<ipc::ReserveRangeResult>(*answer));
    CHECK(std::get<ipc::ReserveRangeResult>(*answer).blocks.size() == 1);
    CHECK(sent.size() == 1);
    CHECK(link.unmatched_answers() == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let the detached thread finish
}

TEST_CASE("an answer that nobody waits for is dropped and counted; a late answer after a timeout too") {
    QuietLogs quiet;
    AgentLink link(7, [](const ipc::Message&) { return Result<void>(); });
    CHECK_FALSE(link.deliver(reserve_result(12345)));
    CHECK(link.unmatched_answers() == 1);

    const std::uint64_t id = link.next_request_id();
    Result<ipc::Message> answer = link.request(reserve(id), 30);
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().id() == "CML5320");
    CHECK(answer.error().message.find("30 ms") != std::string::npos);
    CHECK_FALSE(link.deliver(reserve_result(id)));  // arrives too late
    CHECK(link.unmatched_answers() == 2);
}

TEST_CASE("only the first answer to a request counts") {
    QuietLogs quiet;
    AgentLink* self = nullptr;
    AgentLink link(7, [&](const ipc::Message& m) -> Result<void> {
        const std::uint64_t id = *request_id_of(m);
        CHECK(self->deliver(reserve_result(id)));
        CHECK_FALSE(self->deliver(reserve_result(id)));  // a duplicate
        return Result<void>();
    });
    self = &link;
    CHECK(link.request(reserve(link.next_request_id()), 1000).has_value());
}

TEST_CASE("close() ends pending requests with CML5317 and refuses new ones") {
    QuietLogs quiet;
    AgentLink link(7, [](const ipc::Message&) { return Result<void>(); });
    std::thread closer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        link.close();
    });
    Result<ipc::Message> pending = link.request(reserve(link.next_request_id()), 10000);
    closer.join();
    REQUIRE_FALSE(pending.has_value());
    CHECK(pending.error().id() == "CML5317");
    CHECK(link.closed());
    Result<ipc::Message> after = link.request(reserve(link.next_request_id()), 100);
    REQUIRE_FALSE(after.has_value());
    CHECK(after.error().id() == "CML5317");
    CHECK_FALSE(link.send(reserve(1)).has_value());
}

TEST_CASE("a send failure is reported without waiting") {
    QuietLogs quiet;
    AgentLink link(7, [](const ipc::Message&) -> Result<void> { return Diag::make(ids::kPipeIoFailed, "pipe broke", "restart"); });
    const auto t0 = std::chrono::steady_clock::now();
    Result<ipc::Message> r = link.request(reserve(link.next_request_id()), 10000);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().id() == "CML5306");
    CHECK(r.error().message.find("pipe broke") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
}

TEST_CASE("a request without an id is refused") {
    QuietLogs quiet;
    AgentLink link(7, [](const ipc::Message&) { return Result<void>(); });
    Result<ipc::Message> r = link.request(ipc::Log{}, 10);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().id() == "CML5306");
}

TEST_CASE("publish() copies what the session knows, including the agent's mode and the module bases") {
    QuietLogs quiet;
    Session session(3, "0.1.0");
    AgentLink link(3, [](const ipc::Message&) { return Result<void>(); });
    session.on_message(hello(ipc::AgentMode::Synchronized));
    ipc::ModuleList list;
    list.request_id = 2;
    list.modules.push_back({0x140000000ull, 0x30000, "C:\\Games\\game.exe"});
    list.modules.push_back({0x7FF800000000ull, 0x90000, "C:\\Windows\\System32\\ntdll.dll"});
    session.on_message(list);
    ipc::EnableModule enable;
    enable.request_id = 3;
    enable.path = "game.exe";
    session.on_message(enable);
    link.publish(session);
    const LinkSnapshot s = link.snapshot();
    CHECK(s.handshaken);
    CHECK(s.pid == 99);
    CHECK(s.mode == ipc::AgentMode::Synchronized);
    REQUIRE(s.modules.size() == 2);
    CHECK(s.modules[1].base == 0x7FF800000000ull);
    REQUIRE(s.enabled.size() == 1);
    CHECK(s.enabled[0] == "C:\\Games\\game.exe");
    CHECK(s.revision == session.revision());
}

TEST_CASE("Session accepts the agent's answers: HooksInstalled, PatchLoaded, ReserveRangeResult, Reply") {
    QuietLogs quiet;
    Session session(1, "0.1.0");
    session.on_message(hello());
    std::vector<ipc::Message> delivered;
    session.set_answer_sink([&](const ipc::Message& m) {
        delivered.push_back(m);
        return request_id_of(m) == 42;  // only request 42 is awaited
    });
    ipc::HooksInstalled hooks;
    hooks.request_id = 42;
    for (const ipc::Message& answer : {ipc::Message(hooks), ipc::Message(ipc::PatchLoaded{}), ipc::Message(ipc::ReserveRangeResult{}), ipc::Message(ipc::Reply{})}) {
        const SessionOutcome outcome = session.on_message(answer);
        CHECK_FALSE(outcome.close);  // never a protocol violation
        CHECK(outcome.replies.empty());
    }
    CHECK(delivered.size() == 4);
    CHECK(session.ignored_answers() == 3);  // request id 0 is not awaited
}

TEST_CASE("Session without a sink ignores answers") {
    QuietLogs quiet;
    Session session(1, "0.1.0");
    session.on_message(hello());
    const SessionOutcome outcome = session.on_message(ipc::HooksInstalled{});
    CHECK_FALSE(outcome.close);
    CHECK(session.ignored_answers() == 1);
}

TEST_CASE("messages that only the broker may send are still violations") {
    QuietLogs quiet;
    Session session(1, "0.1.0");
    session.on_message(hello());
    for (const ipc::Message& m : {ipc::Message(ipc::InstallHooks{}), ipc::Message(ipc::LoadPatch{}), ipc::Message(ipc::ApplyFixups{}),
                                  ipc::Message(ipc::RunInitializers{}), ipc::Message(ipc::ReleaseRange{}), ipc::Message(ipc::ReserveRange{})}) {
        const SessionOutcome outcome = session.on_message(m);
        CHECK(outcome.close);
    }
}
