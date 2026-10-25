// Control protocol over real named pipes: the minimal broker control server, the client library, and
// the full ConnectionServer with a control pipe next to the agent pipe. Every pipe name is unique
// and every wait is bounded.
#if defined(_WIN32)

#include <windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <variant>

#include "cml/core/connection_server.h"
#include "cml/core/control_server.h"
#include "cml/core/process_registry.h"
#include "cml/ipc/control.h"
#include "cml/ipc/pipe.h"
#include "cml/proc/diag_ids.h"
#include "cml/util/log.h"
#include "doctest.h"

using namespace cml;
using namespace cml::ipc;

namespace {

constexpr std::uint32_t kWaitMs = 15000;

std::string unique_name(const char* tag) {
    static std::atomic<int> counter{0};
    return std::string("\\\\.\\pipe\\cml_ut_control_") + tag + "_" + std::to_string(GetCurrentProcessId()) + "_" +
           std::to_string(GetTickCount64()) + "_" + std::to_string(counter.fetch_add(1));
}

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

core::ControlContext context(const core::ProcessRegistry* registry) {
    core::ControlContext c;
    c.version = "9.9.9";
    c.registry = registry;
    return c;
}

ControlClient connect_client(const std::string& name) {
    Result<ControlClient> client = ControlClient::connect(name, kWaitMs);
    REQUIRE_MESSAGE(client.has_value(), (client.has_value() ? "" : client.error().format()));
    return std::move(*client);
}

ControlChannel raw_channel(const std::string& name) {
    Result<PipeConnection> connection = connect_pipe(name, kWaitMs);
    REQUIRE_MESSAGE(connection.has_value(), (connection.has_value() ? "" : connection.error().format()));
    return ControlChannel(std::move(*connection));
}

std::string read_line(ControlChannel& channel) {
    Result<ControlChannel::Line> line = channel.receive_line(kWaitMs);
    REQUIRE_MESSAGE(line.has_value(), (line.has_value() ? "" : line.error().format()));
    REQUIRE(line->status == IoStatus::Ok);
    return line->text;
}

ControlResponse as_response(const std::string& line) {
    Result<std::variant<ControlResponse, ControlEvent>> decoded = decode_server_message(line);
    REQUIRE_MESSAGE(decoded.has_value(), (decoded.has_value() ? "" : decoded.error().format()));
    REQUIRE(std::holds_alternative<ControlResponse>(*decoded));
    return std::get<ControlResponse>(*decoded);
}

}  // namespace

TEST_CASE("control pipe: name resolution") {
    Result<std::string> explicit_name = resolve_control_pipe_name("\\\\.\\pipe\\mine");
    REQUIRE(explicit_name.has_value());
    CHECK(*explicit_name == "\\\\.\\pipe\\mine");
    Result<std::string> bad = resolve_control_pipe_name("mine");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ids::kPipeNameInvalid);
    Result<std::string> fallback = resolve_control_pipe_name("");
    REQUIRE(fallback.has_value());
    // Without CML_CONTROL_PIPE the default is cml_control_<SID> (the test environment has no override).
    if (!SetEnvironmentVariableW(L"CML_CONTROL_PIPE", nullptr)) {
        FAIL("cannot clear CML_CONTROL_PIPE");
    }
    Result<std::string> def = resolve_control_pipe_name("");
    REQUIRE(def.has_value());
    CHECK(def->rfind("\\\\.\\pipe\\cml_control_S-1-", 0) == 0);
    SetEnvironmentVariableW(L"CML_CONTROL_PIPE", L"\\\\.\\pipe\\from_env");
    Result<std::string> from_env = resolve_control_pipe_name("");
    REQUIRE(from_env.has_value());
    CHECK(*from_env == "\\\\.\\pipe\\from_env");
    Result<std::string> flag_wins = resolve_control_pipe_name("\\\\.\\pipe\\flag");
    CHECK(*flag_wins == "\\\\.\\pipe\\flag");
    SetEnvironmentVariableW(L"CML_CONTROL_PIPE", nullptr);
}

TEST_CASE("control pipe: the minimal server answers status and unknown commands") {
    QuietLogs quiet;
    core::ProcessRegistry registry;
    const std::string name = unique_name("basic");
    core::ControlServer server(name, context(&registry));
    Result<void> started = server.start();
    REQUIRE_MESSAGE(started.has_value(), (started.has_value() ? "" : started.error().format()));

    ControlClient client = connect_client(name);
    Result<StatusReport> status = client.status(kWaitMs);
    REQUIRE_MESSAGE(status.has_value(), (status.has_value() ? "" : status.error().format()));
    CHECK(status->version == "9.9.9");
    CHECK(status->capabilities == std::vector<std::string>{"status"});
    CHECK(status->processes.empty());

    Result<ControlResponse> reload = client.call("reload", json::Value::object(), kWaitMs);
    REQUIRE(reload.has_value());
    CHECK_FALSE(reload->ok);
    REQUIRE(reload->diag.has_value());
    CHECK(reload->diag->id == "CML5402");

    // several calls on one connection, ids are matched
    for (int i = 0; i < 20; ++i) {
        CHECK(client.status(kWaitMs).has_value());
    }
    server.stop();
}

TEST_CASE("control pipe: malformed input gets a diag and the connection keeps working") {
    QuietLogs quiet;
    core::ProcessRegistry registry;
    const std::string name = unique_name("malformed");
    core::ControlServer server(name, context(&registry));
    REQUIRE(server.start().has_value());

    ControlChannel channel = raw_channel(name);
    for (const char* bad : {"garbage", "[1,2]", "{\"id\":4,\"cmd\":7}", "{\"id\":", "\xFF\xFE"}) {
        INFO("sent: " << bad);
        REQUIRE(channel.send_line(bad, kWaitMs).has_value());
        const ControlResponse r = as_response(read_line(channel));
        CHECK_FALSE(r.ok);
        REQUIRE(r.diag.has_value());
        CHECK((r.diag->id == "CML5400" || r.diag->id == "CML5401"));
    }
    // blank lines are ignored and a trailing CR is accepted
    REQUIRE(channel.send_line("", kWaitMs).has_value());
    REQUIRE(channel.send_line("{\"id\":76,\"cmd\":\"status\"}\r", kWaitMs).has_value());
    const ControlResponse after = as_response(read_line(channel));
    CHECK(after.ok);
    CHECK(after.id == std::optional<std::int64_t>(76));
    server.stop();
}

TEST_CASE("control pipe: an over-long line is answered with CML5403 and the connection is closed") {
    QuietLogs quiet;
    core::ProcessRegistry registry;
    const std::string name = unique_name("toolong");
    core::ControlServer server(name, context(&registry));
    REQUIRE(server.start().has_value());

    Result<PipeConnection> connection = connect_pipe(name, kWaitMs);
    REQUIRE(connection.has_value());
    const std::string junk(kMaxControlLineBytes + 4096, 'x');  // no newline
    // The server stops reading once it has seen too much, so the write may be cut short by the close.
    (void)connection->send_bytes(junk.data(), junk.size(), kWaitMs);
    ControlChannel channel(std::move(*connection));
    bool saw_error = false;
    for (int i = 0; i < 4 && !saw_error; ++i) {
        Result<ControlChannel::Line> line = channel.receive_line(kWaitMs);
        if (!line || line->status != IoStatus::Ok) {
            break;
        }
        const ControlResponse r = as_response(line->text);
        saw_error = r.diag.has_value() && r.diag->id == "CML5403";
    }
    CHECK(saw_error);
    // The server itself is fine: a new client works.
    ControlClient fresh = connect_client(name);
    CHECK(fresh.status(kWaitMs).has_value());
    server.stop();
}

TEST_CASE("control pipe: several clients at once, and the name cannot be taken twice") {
    QuietLogs quiet;
    core::ProcessRegistry registry;
    const std::string name = unique_name("many");
    core::ControlServer server(name, context(&registry));
    REQUIRE(server.start().has_value());

    ControlClient a = connect_client(name);
    ControlClient b = connect_client(name);
    CHECK(a.status(kWaitMs).has_value());
    CHECK(b.status(kWaitMs).has_value());
    CHECK(a.status(kWaitMs).has_value());

    core::ControlServer second(name, context(&registry));
    Result<void> taken = second.start();
    REQUIRE_FALSE(taken.has_value());
    CHECK(taken.error().code == ids::kPipeCreateFailed);

    core::ControlServer bad_name("not-a-pipe", context(&registry));
    Result<void> bad = bad_name.start();
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ids::kPipeNameInvalid);

    server.stop();
    // After stop the connected clients see the pipe closed (an error, not a hang).
    Result<StatusReport> after = a.status(kWaitMs);
    CHECK_FALSE(after.has_value());
}

TEST_CASE("control client: failures are diagnostics, not hangs") {
    QuietLogs quiet;
    Result<ControlClient> none = ControlClient::connect(unique_name("nobody"), 100);
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().code == ids::kControlUnreachable);
    CHECK_FALSE(none.error().what_to_do.empty());

    Result<ControlClient> bad = ControlClient::connect("nope", 100);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ids::kPipeNameInvalid);

    // A server that never answers: the call times out with CML5408.
    const std::string name = unique_name("silent");
    Result<PipeServer> silent = PipeServer::create(name);
    REQUIRE(silent.has_value());
    std::thread acceptor([&] {
        Result<PipeServer::Accepted> accepted = silent->accept(kWaitMs);
        if (accepted && accepted->status == IoStatus::Ok) {
            Sleep(1500);  // hold the connection open without answering
        }
    });
    ControlClient client = connect_client(name);
    Result<ControlResponse> timed_out = client.call("status", json::Value::object(), 300);
    REQUIRE_FALSE(timed_out.has_value());
    CHECK(timed_out.error().code == ids::kControlTimeout);
    silent->cancel();
    acceptor.join();
    // The peer is gone now.
    Result<ControlResponse> closed = client.call("status", json::Value::object(), 2000);
    CHECK_FALSE(closed.has_value());
}

TEST_CASE("control client: events arriving before a response are queued, a null-id error answers the call") {
    QuietLogs quiet;
    const std::string name = unique_name("events");
    Result<PipeServer> raw = PipeServer::create(name);
    REQUIRE(raw.has_value());
    std::atomic<bool> release{false};
    std::thread fake([&] {
        Result<PipeServer::Accepted> accepted = raw->accept(kWaitMs);
        if (!accepted || accepted->status != IoStatus::Ok) {
            return;
        }
        ControlChannel channel(std::move(accepted->connection));
        for (int round = 0; round < 2; ++round) {
            Result<ControlChannel::Line> request = channel.receive_line(kWaitMs);
            if (!request || request->status != IoStatus::Ok) {
                return;
            }
            const std::optional<std::int64_t> id = peek_control_request_id(request->text);
            ControlEvent event;
            event.name = "diag";
            event.body.set("id", json::Value::string("CML3101"));
            (void)channel.send_line(*encode_event(event), kWaitMs);
            (void)channel.send_line("{\"id\":999,\"ok\":true,\"result\":{}}", kWaitMs);  // stale answer: ignored
            if (round == 0) {
                (void)channel.send_line(*encode_response(make_ok_response(id.value_or(0), json::Value::object())), kWaitMs);
            } else {
                (void)channel.send_line(*encode_response(make_error_response(std::nullopt, Diag::make(ids::kControlRequestInvalid, "bad", "fix"))), kWaitMs);
            }
        }
        // keep the connection open until the test has looked at everything
        const ULONGLONG until = GetTickCount64() + kWaitMs;
        while (!release.load() && GetTickCount64() < until) {
            Sleep(5);
        }
    });
    ControlClient client = connect_client(name);
    Result<ControlResponse> first = client.call("status", json::Value::object(), kWaitMs);
    REQUIRE(first.has_value());
    CHECK(first->ok);
    CHECK(client.queued_events() == 1);
    Result<std::optional<ControlEvent>> event = client.next_event(100);
    REQUIRE(event.has_value());
    REQUIRE(event->has_value());
    CHECK((*event)->name == "diag");
    CHECK(client.queued_events() == 0);

    Result<ControlResponse> second = client.call("status", json::Value::object(), kWaitMs);
    REQUIRE(second.has_value());
    CHECK_FALSE(second->ok);
    REQUIRE(second->diag.has_value());
    CHECK(second->diag->id == "CML5401");
    // the second call's event is queued; next_event returns it, then times out cleanly
    Result<std::optional<ControlEvent>> queued = client.next_event(100);
    REQUIRE(queued.has_value());
    CHECK(queued->has_value());
    Result<std::optional<ControlEvent>> none = client.next_event(50);
    REQUIRE(none.has_value());
    CHECK_FALSE(none->has_value());
    release.store(true);
    fake.join();
}

TEST_CASE("connection server with a control pipe: status lists the agents' processes and modules") {
    QuietLogs quiet;
    core::ServerOptions options;
    options.pipe_name = unique_name("agent");
    options.control_pipe_name = unique_name("ctl");
    core::ConnectionServer server(options);
    std::thread thread([&] { (void)server.run(); });
    const ULONGLONG deadline = GetTickCount64() + kWaitMs;
    while ((!pipe_exists(options.pipe_name) || !pipe_exists(options.control_pipe_name)) && GetTickCount64() < deadline) {
        Sleep(5);
    }
    REQUIRE(pipe_exists(options.pipe_name));
    REQUIRE(pipe_exists(options.control_pipe_name));

    ControlClient client = connect_client(options.control_pipe_name);
    Result<StatusReport> empty = client.status(kWaitMs);
    REQUIRE(empty.has_value());
    CHECK(empty->processes.empty());
    CHECK(empty->has_capability("status"));

    Result<PipeConnection> agent = connect_pipe(options.pipe_name, kWaitMs);
    REQUIRE(agent.has_value());
    auto exchange = [&](const Message& request) {
        REQUIRE(agent->send(request, kWaitMs).has_value());
        Result<PipeConnection::Received> received = agent->receive(kWaitMs);
        REQUIRE(received.has_value());
        REQUIRE(received->status == IoStatus::Ok);
    };
    Hello hello;
    hello.request_id = 1;
    hello.pid = 4242;
    exchange(hello);
    ModuleList list;
    list.request_id = 2;
    list.modules.push_back({0x140000000ull, 0x1000, "C:\\app\\demo.exe"});
    list.modules.push_back({0x180000000ull, 0x1000, "C:\\app\\lib.dll"});
    exchange(list);
    EnableModule enable;
    enable.request_id = 3;
    enable.path = "C:\\app\\demo.exe";
    exchange(enable);

    Result<StatusReport> one = client.status(kWaitMs);
    REQUIRE(one.has_value());
    REQUIRE(one->processes.size() == 1);
    CHECK(one->processes[0].pid == 4242);
    REQUIRE(one->processes[0].modules.size() == 2);
    CHECK(one->processes[0].modules[0].path == "C:\\app\\demo.exe");
    CHECK(one->processes[0].modules[0].enabled);
    CHECK_FALSE(one->processes[0].modules[1].enabled);

    // when the agent disconnects the process disappears from the list
    agent->close();
    bool gone = false;
    const ULONGLONG until = GetTickCount64() + kWaitMs;
    while (!gone && GetTickCount64() < until) {
        Result<StatusReport> s = client.status(kWaitMs);
        gone = s.has_value() && s->processes.empty();
        if (!gone) {
            Sleep(20);
        }
    }
    CHECK(gone);

    server.stop();
    thread.join();
}

TEST_CASE("connection server: a control pipe that cannot be created makes run() fail") {
    QuietLogs quiet;
    core::ServerOptions options;
    options.pipe_name = unique_name("agent2");
    options.control_pipe_name = "not-a-pipe";
    core::ConnectionServer server(options);
    Result<core::ServerExit> result = server.run();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ids::kPipeNameInvalid);
}

TEST_CASE("connection server: control clients do not keep an idle broker alive or count as agents") {
    QuietLogs quiet;
    core::ServerOptions options;
    options.pipe_name = unique_name("agent3");
    options.control_pipe_name = unique_name("ctl3");
    options.startup_timeout_ms = 400;
    core::ConnectionServer server(options);
    std::atomic<bool> finished{false};
    core::ServerExit reason = core::ServerExit::Stopped;
    std::thread thread([&] {
        Result<core::ServerExit> r = server.run();
        if (r) reason = *r;
        finished.store(true);
    });
    const ULONGLONG deadline = GetTickCount64() + kWaitMs;
    while (!pipe_exists(options.control_pipe_name) && GetTickCount64() < deadline) {
        Sleep(5);
    }
    ControlClient client = connect_client(options.control_pipe_name);
    CHECK(client.status(kWaitMs).has_value());
    // No agent ever connects: the startup timeout still fires although a control client is connected.
    const ULONGLONG until = GetTickCount64() + kWaitMs;
    while (!finished.load() && GetTickCount64() < until) {
        Sleep(10);
    }
    CHECK(finished.load());
    thread.join();
    CHECK(reason == core::ServerExit::StartupTimeout);
}

TEST_CASE("control pipe: accept failures never end the accept loop; it recovers and reports health (review fix 8)") {
    QuietLogs quiet;
    core::ProcessRegistry registry;
    const std::string name = unique_name("recover");
    core::ControlServer server(name, context(&registry));
    core::ControlServer::set_accept_failures_for_testing(3);
    REQUIRE(server.start().has_value());

    // While the failures are being worked through (100 + 200 + 400 ms of backoff) the server reports it.
    bool saw_unhealthy = false;
    const ULONGLONG until = GetTickCount64() + kWaitMs;
    while (GetTickCount64() < until && !(saw_unhealthy && server.healthy())) {
        saw_unhealthy = saw_unhealthy || !server.healthy();
        Sleep(5);
    }
    CHECK(saw_unhealthy);
    CHECK(server.healthy());

    // Afterwards a client can connect and is served; status says the pipe is healthy again.
    ControlClient client = connect_client(name);
    Result<StatusReport> status = client.status(kWaitMs);
    REQUIRE_MESSAGE(status.has_value(), (status.has_value() ? "" : status.error().format()));
    CHECK(status->control_pipe_healthy);

    // A longer run of failures (still finite here) is survived too, with clients already connected.
    core::ControlServer::set_accept_failures_for_testing(6);  // 100+200+400+800+1600+2000 ms of backoff
    ControlClient early = connect_client(name);  // connects to the (recreated) instance while the loop backs off
    Result<StatusReport> late = early.status(30000);  // served as soon as the loop accepts again
    REQUIRE_MESSAGE(late.has_value(), (late.has_value() ? "" : late.error().format()));
    CHECK(client.status(kWaitMs).has_value());
    core::ControlServer::set_accept_failures_for_testing(0);
    server.stop();
}

TEST_CASE("control channel: newline search is linear (review fix 10)") {
    QuietLogs quiet;
    const std::string name = unique_name("linear");
    Result<PipeServer> raw = PipeServer::create(name);
    REQUIRE(raw.has_value());
    const std::string long_line(900 * 1000, 'q');
    constexpr int kShortLines = 30000;
    std::uint64_t scan_work = 0;
    std::size_t received_bytes = 0;
    bool all_ok = false;
    std::thread reader([&] {
        Result<PipeServer::Accepted> accepted = raw->accept(kWaitMs);
        if (!accepted || accepted->status != IoStatus::Ok) {
            return;
        }
        ControlChannel channel(std::move(accepted->connection));
        Result<ControlChannel::Line> first = channel.receive_line(kWaitMs);
        if (!first || first->status != IoStatus::Ok || first->text != long_line) {
            return;
        }
        received_bytes += first->text.size() + 1;
        for (int i = 0; i < kShortLines; ++i) {
            Result<ControlChannel::Line> line = channel.receive_line(kWaitMs);
            if (!line || line->status != IoStatus::Ok || line->text != "L" + std::to_string(i)) {
                return;
            }
            received_bytes += line->text.size() + 1;
        }
        scan_work = channel.scan_work();
        all_ok = true;
    });
    Result<PipeConnection> connection = connect_pipe(name, kWaitMs);
    REQUIRE(connection.has_value());
    std::string payload = long_line + "\n";
    for (int i = 0; i < kShortLines; ++i) {
        payload += "L" + std::to_string(i) + "\n";
    }
    REQUIRE(connection->send_bytes(payload.data(), payload.size(), kWaitMs).has_value());
    reader.join();
    REQUIRE(all_ok);
    CHECK(received_bytes == payload.size());
    // Quadratic scanning would examine ~ (900000 / 4096) * 450000 = 100 MB for the long line alone.
    CHECK(scan_work <= 2 * payload.size());
}

#endif  // _WIN32
