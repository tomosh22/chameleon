// ConnectionServer on real pipes: several agents at once, idle exit, startup timeout, stop().
// Every pipe name is unique and every wait is bounded.
#if defined(_WIN32)

#include "cml/core/connection_server.h"

#include <windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <variant>

#include "cml/ipc/pipe.h"
#include "cml/proc/diag_ids.h"
#include "cml/util/log.h"
#include "doctest.h"

using namespace cml;
using cml::core::ConnectionServer;
using cml::core::ServerExit;
using cml::core::ServerOptions;
using cml::ipc::IoStatus;
using cml::ipc::PipeConnection;

namespace {

constexpr std::uint32_t kWaitMs = 15000;

std::string unique_pipe_name(const char* tag) {
    static std::atomic<int> counter{0};
    return std::string("\\\\.\\pipe\\cml_ut_server_") + tag + "_" + std::to_string(GetCurrentProcessId()) + "_" +
           std::to_string(GetTickCount64()) + "_" + std::to_string(counter.fetch_add(1));
}

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

// Runs a server on a thread; join() returns its exit reason (or the error text).
class RunningServer {
public:
    explicit RunningServer(ServerOptions options) : options_(options), server_(std::make_unique<ConnectionServer>(options)) {
        thread_ = std::thread([this] {
            const Result<ServerExit> result = server_->run();
            if (result) {
                reason_ = *result;
                ok_ = true;
            } else {
                error_ = result.error().format();
            }
            finished_.store(true);
        });
        // Wait until it listens (or died).
        const ULONGLONG deadline = GetTickCount64() + kWaitMs;
        while (!ipc::pipe_exists(options_.pipe_name) && !finished_.load() && GetTickCount64() < deadline) {
            Sleep(5);
        }
    }
    ~RunningServer() {
        server_->stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    void stop() { server_->stop(); }
    bool finished() const { return finished_.load(); }
    // Waits for the server thread to end; true if it did within kWaitMs.
    bool join() {
        const ULONGLONG deadline = GetTickCount64() + kWaitMs;
        while (!finished_.load() && GetTickCount64() < deadline) {
            Sleep(5);
        }
        if (finished_.load()) {
            thread_.join();
            return true;
        }
        return false;
    }
    bool ok() const { return ok_; }
    ServerExit reason() const { return reason_; }
    const std::string& error() const { return error_; }

private:
    ServerOptions options_;
    std::unique_ptr<ConnectionServer> server_;
    std::thread thread_;
    std::atomic<bool> finished_{false};
    bool ok_ = false;
    ServerExit reason_ = ServerExit::Stopped;
    std::string error_;
};

PipeConnection connect(const std::string& name) {
    Result<PipeConnection> client = ipc::connect_pipe(name, kWaitMs);
    REQUIRE_MESSAGE(client.has_value(), (client.has_value() ? "" : client.error().format()));
    return std::move(*client);
}

ipc::Message exchange(PipeConnection& connection, const ipc::Message& request) {
    Result<IoStatus> sent = connection.send(request, kWaitMs);
    REQUIRE(sent.has_value());
    REQUIRE(*sent == IoStatus::Ok);
    Result<PipeConnection::Received> received = connection.receive(kWaitMs);
    REQUIRE_MESSAGE(received.has_value(), (received.has_value() ? "" : received.error().format()));
    REQUIRE(received->status == IoStatus::Ok);
    return std::move(received->message);
}

ipc::Hello hello(std::uint64_t id) {
    ipc::Hello m;
    m.request_id = id;
    m.pid = GetCurrentProcessId();
    return m;
}

}  // namespace

TEST_CASE("agents register, report modules and enable them over real pipes") {
    QuietLogs quiet;
    ServerOptions options;
    options.pipe_name = unique_pipe_name("session");
    RunningServer server(options);
    REQUIRE_MESSAGE(ipc::pipe_exists(options.pipe_name), server.error());

    PipeConnection agent = connect(options.pipe_name);
    const ipc::Message welcome = exchange(agent, hello(1));
    REQUIRE(std::holds_alternative<ipc::Welcome>(welcome));
    CHECK(std::get<ipc::Welcome>(welcome).request_id == 1);
    CHECK(std::get<ipc::Welcome>(welcome).agent_id == 1);

    ipc::ModuleList list;
    list.request_id = 2;
    list.modules.push_back({0x140000000ull, 0x1000, "C:\\app\\demo.exe"});
    const ipc::Message list_reply = exchange(agent, list);
    REQUIRE(std::holds_alternative<ipc::Reply>(list_reply));
    CHECK(std::get<ipc::Reply>(list_reply).status == 0);

    ipc::EnableModule enable;
    enable.request_id = 3;
    enable.path = "C:\\app\\demo.exe";
    const ipc::Message ok = exchange(agent, enable);
    REQUIRE(std::holds_alternative<ipc::Reply>(ok));
    CHECK(std::get<ipc::Reply>(ok).request_id == 3);
    CHECK(std::get<ipc::Reply>(ok).status == 0);

    enable.request_id = 4;
    enable.path = "C:\\app\\other.dll";
    const ipc::Message refused = exchange(agent, enable);
    REQUIRE(std::holds_alternative<ipc::Reply>(refused));
    CHECK(std::get<ipc::Reply>(refused).request_id == 4);
    CHECK(std::get<ipc::Reply>(refused).status == ids::kModuleNotLoaded);

    server.stop();
    CHECK(server.join());
    CHECK(server.ok());
    CHECK(server.reason() == ServerExit::Stopped);
}

TEST_CASE("a protocol violation gets an error reply and the connection is closed") {
    QuietLogs quiet;
    ServerOptions options;
    options.pipe_name = unique_pipe_name("violation");
    RunningServer server(options);
    REQUIRE(ipc::pipe_exists(options.pipe_name));

    PipeConnection agent = connect(options.pipe_name);
    ipc::EnableModule enable;
    enable.request_id = 9;
    enable.path = "demo.exe";
    const ipc::Message reply = exchange(agent, enable);  // no Hello first
    REQUIRE(std::holds_alternative<ipc::Reply>(reply));
    CHECK(std::get<ipc::Reply>(reply).status == ids::kProtocolViolation);

    // The server closes its end (when it reaps the finished connection); the agent sees EOF.
    Result<PipeConnection::Received> ended = agent.receive(kWaitMs);
    REQUIRE(ended.has_value());
    CHECK(ended->status == IoStatus::Closed);
}

TEST_CASE("several agents are served at the same time with separate sessions") {
    QuietLogs quiet;
    ServerOptions options;
    options.pipe_name = unique_pipe_name("many");
    RunningServer server(options);
    REQUIRE(ipc::pipe_exists(options.pipe_name));

    PipeConnection a = connect(options.pipe_name);
    PipeConnection b = connect(options.pipe_name);
    const ipc::Message wa = exchange(a, hello(1));
    const ipc::Message wb = exchange(b, hello(1));
    REQUIRE(std::holds_alternative<ipc::Welcome>(wa));
    REQUIRE(std::holds_alternative<ipc::Welcome>(wb));
    CHECK(std::get<ipc::Welcome>(wa).agent_id != std::get<ipc::Welcome>(wb).agent_id);

    // Module lists are per session: agent a knows demo.exe, agent b does not.
    ipc::ModuleList list;
    list.request_id = 2;
    list.modules.push_back({0x1000, 0x1000, "C:\\app\\demo.exe"});
    exchange(a, list);
    ipc::EnableModule enable;
    enable.request_id = 3;
    enable.path = "demo.exe";
    CHECK(std::get<ipc::Reply>(exchange(a, enable)).status == 0);
    ipc::ModuleList empty;
    empty.request_id = 2;
    exchange(b, empty);
    CHECK(std::get<ipc::Reply>(exchange(b, enable)).status == ids::kModuleNotLoaded);
}

TEST_CASE("--exit-when-idle: the server leaves after the last agent has gone") {
    QuietLogs quiet;
    ServerOptions options;
    options.pipe_name = unique_pipe_name("idle");
    options.exit_when_idle_ms = 300;
    RunningServer server(options);
    REQUIRE(ipc::pipe_exists(options.pipe_name));

    {
        PipeConnection agent = connect(options.pipe_name);
        exchange(agent, hello(1));
        CHECK_FALSE(server.finished());  // an agent is connected: not idle
    }  // disconnect
    REQUIRE(server.join());
    REQUIRE_MESSAGE(server.ok(), server.error());
    CHECK(server.reason() == ServerExit::Idle);
    CHECK_FALSE(ipc::pipe_exists(options.pipe_name));
}

TEST_CASE("the idle timer does not start before the first agent has connected") {
    QuietLogs quiet;
    ServerOptions options;
    options.pipe_name = unique_pipe_name("noidle");
    options.exit_when_idle_ms = 50;
    RunningServer server(options);
    REQUIRE(ipc::pipe_exists(options.pipe_name));
    // Far more than the idle time passes while waiting for an agent that has not arrived yet.
    Sleep(400);
    CHECK_FALSE(server.finished());
    PipeConnection agent = connect(options.pipe_name);
    exchange(agent, hello(1));
    CHECK_FALSE(server.finished());
}

TEST_CASE("--startup-timeout: the server leaves if no agent ever connects") {
    QuietLogs quiet;
    ServerOptions options;
    options.pipe_name = unique_pipe_name("startup");
    options.startup_timeout_ms = 200;
    RunningServer server(options);
    REQUIRE(server.join());
    REQUIRE_MESSAGE(server.ok(), server.error());
    CHECK(server.reason() == ServerExit::StartupTimeout);
}

TEST_CASE("stop() ends the server even with agents connected") {
    QuietLogs quiet;
    ServerOptions options;
    options.pipe_name = unique_pipe_name("stop");
    RunningServer server(options);
    REQUIRE(ipc::pipe_exists(options.pipe_name));

    PipeConnection agent = connect(options.pipe_name);
    exchange(agent, hello(1));
    server.stop();
    REQUIRE(server.join());
    CHECK(server.reason() == ServerExit::Stopped);
    Result<PipeConnection::Received> ended = agent.receive(kWaitMs);
    REQUIRE(ended.has_value());
    CHECK(ended->status == IoStatus::Closed);
}

TEST_CASE("a pipe that is already served makes run() fail with CML5207") {
    QuietLogs quiet;
    ServerOptions options;
    options.pipe_name = unique_pipe_name("taken");
    RunningServer first(options);
    REQUIRE(ipc::pipe_exists(options.pipe_name));
    RunningServer second(options);
    REQUIRE(second.join());
    CHECK_FALSE(second.ok());
    CHECK(second.error().find("CML5207") != std::string::npos);
}

#endif  // _WIN32
