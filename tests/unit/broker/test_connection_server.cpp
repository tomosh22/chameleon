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
#include <vector>

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

namespace {

HANDLE open_raw_client(const std::string& name) {
    const std::wstring wide(name.begin(), name.end());
    return CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
}

void raw_write(HANDLE handle, const std::vector<std::uint8_t>& bytes) {
    DWORD written = 0;
    REQUIRE(WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr));
    REQUIRE(written == bytes.size());
}

void raw_send(HANDLE handle, const ipc::Message& message) {
    Result<std::vector<std::uint8_t>> frame = ipc::encode_frame(message);
    REQUIRE(frame.has_value());
    raw_write(handle, *frame);
}

ipc::Message raw_receive(HANDLE handle) {
    std::vector<std::uint8_t> header(ipc::kFrameHeaderSize);
    DWORD read = 0;
    REQUIRE(ReadFile(handle, header.data(), static_cast<DWORD>(header.size()), &read, nullptr));
    REQUIRE(read == header.size());
    Result<ipc::FrameHeader> parsed = ipc::parse_frame_header(header.data(), header.size());
    REQUIRE(parsed.has_value());
    std::vector<std::uint8_t> payload(parsed->payload_size);
    DWORD got = 0;
    while (got < payload.size()) {
        DWORD n = 0;
        REQUIRE(ReadFile(handle, payload.data() + got, static_cast<DWORD>(payload.size() - got), &n, nullptr));
        got += n;
    }
    Result<ipc::Message> message = ipc::decode_message(parsed->type, parsed->version, payload.data(), payload.size());
    REQUIRE(message.has_value());
    return *message;
}

}  // namespace

TEST_CASE("one malformed Log does not cost the agent its connection") {
    QuietLogs quiet;
    ServerOptions options;
    options.pipe_name = unique_pipe_name("badlog");
    RunningServer server(options);
    REQUIRE(ipc::pipe_exists(options.pipe_name));

    HANDLE raw = open_raw_client(options.pipe_name);
    REQUIRE(raw != INVALID_HANDLE_VALUE);
    raw_send(raw, hello(1));
    REQUIRE(std::holds_alternative<ipc::Welcome>(raw_receive(raw)));

    // A Log whose text is not UTF-8 (level 3, two bytes FF FE), framed correctly.
    const std::vector<std::uint8_t> bad_log = {0x0A, 0x00, 0x00, 0x00, 0x60, 0x00, 0x01, 0x00,
                                               0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xFF, 0xFE};
    raw_write(raw, bad_log);

    // The session is still alive and still has its state.
    ipc::ModuleList list;
    list.request_id = 2;
    list.modules.push_back({0x1000, 0x1000, "C:\\app\\demo.exe"});
    raw_send(raw, list);
    const ipc::Message answer = raw_receive(raw);
    REQUIRE(std::holds_alternative<ipc::Reply>(answer));
    CHECK(std::get<ipc::Reply>(answer).request_id == 2);
    CHECK(std::get<ipc::Reply>(answer).status == 0);
    CloseHandle(raw);
}

TEST_CASE("a client that connects and immediately leaves does not stop the broker") {
    QuietLogs quiet;
    ServerOptions options;
    options.pipe_name = unique_pipe_name("ghost");
    RunningServer server(options);
    REQUIRE(ipc::pipe_exists(options.pipe_name));

    for (int i = 0; i < 5; ++i) {
        HANDLE ghost = open_raw_client(options.pipe_name);
        if (ghost != INVALID_HANDLE_VALUE) {
            CloseHandle(ghost);  // gone before (or just after) the broker accepted it
        }
    }
    CHECK_FALSE(server.finished());

    PipeConnection agent = connect(options.pipe_name);
    const ipc::Message welcome = exchange(agent, hello(1));
    CHECK(std::holds_alternative<ipc::Welcome>(welcome));
    CHECK_FALSE(server.finished());
}

TEST_CASE("a request that arrives with a bad payload is answered with an error, not left hanging") {
    QuietLogs quiet;
    ServerOptions options;
    options.pipe_name = unique_pipe_name("badrequest");
    RunningServer server(options);
    REQUIRE(ipc::pipe_exists(options.pipe_name));

    HANDLE raw = open_raw_client(options.pipe_name);
    REQUIRE(raw != INVALID_HANDLE_VALUE);
    raw_send(raw, hello(1));
    REQUIRE(std::holds_alternative<ipc::Welcome>(raw_receive(raw)));

    // EnableModule, request id 77, options 0, path of two bytes FF FE (not UTF-8).
    std::vector<std::uint8_t> bad = {0x12, 0x00, 0x00, 0x00, 0x11, 0x00, 0x01, 0x00,  // header: 18 bytes, EnableModule
                                     77,   0,    0,    0,    0,    0,    0,    0,     // request id
                                     0,    0,    0,    0,                             // options
                                     2,    0,    0,    0,    0xFF, 0xFE};              // path
    // payload is 8 + 4 + 4 + 2 = 18 bytes
    raw_write(raw, bad);
    const ipc::Message answer = raw_receive(raw);
    REQUIRE(std::holds_alternative<ipc::Reply>(answer));
    CHECK(std::get<ipc::Reply>(answer).request_id == 77);
    CHECK(std::get<ipc::Reply>(answer).status == ids::kPayloadMalformed);

    // The session is still usable afterwards.
    ipc::ModuleList list;
    list.request_id = 78;
    raw_send(raw, list);
    const ipc::Message next = raw_receive(raw);
    REQUIRE(std::holds_alternative<ipc::Reply>(next));
    CHECK(std::get<ipc::Reply>(next).request_id == 78);
    CHECK(std::get<ipc::Reply>(next).status == 0);
    CloseHandle(raw);
}

TEST_CASE("a request whose id cannot even be read closes the connection") {
    QuietLogs quiet;
    ServerOptions options;
    options.pipe_name = unique_pipe_name("noid");
    RunningServer server(options);
    REQUIRE(ipc::pipe_exists(options.pipe_name));

    HANDLE raw = open_raw_client(options.pipe_name);
    REQUIRE(raw != INVALID_HANDLE_VALUE);
    raw_send(raw, hello(1));
    REQUIRE(std::holds_alternative<ipc::Welcome>(raw_receive(raw)));

    // An EnableModule with a 3-byte payload: no request id to answer.
    raw_write(raw, {0x03, 0x00, 0x00, 0x00, 0x11, 0x00, 0x01, 0x00, 0x01, 0x02, 0x03});
    std::uint8_t byte = 0;
    DWORD read = 0;
    CHECK_FALSE(ReadFile(raw, &byte, 1, &read, nullptr));  // the server hung up
    CloseHandle(raw);
}

TEST_CASE("a caller-owned stop event stops the server and is left open") {
    QuietLogs quiet;
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(event != nullptr);
    {
        ServerOptions options;
        options.pipe_name = unique_pipe_name("extstop");
        options.stop_event = event;
        RunningServer server(options);
        REQUIRE(ipc::pipe_exists(options.pipe_name));
        PipeConnection agent = connect(options.pipe_name);
        exchange(agent, hello(1));
        SetEvent(event);  // what a Ctrl+C handler does: no server object involved
        REQUIRE(server.join());
        CHECK(server.reason() == ServerExit::Stopped);
    }  // the server object is gone here
    CHECK(WaitForSingleObject(event, 0) == WAIT_OBJECT_0);  // the event is still ours and still valid
    CloseHandle(event);
}

TEST_CASE("a client waiting for its replacement pipe instance keeps the server from exiting as idle") {
    QuietLogs quiet;
    ServerOptions options;
    options.pipe_name = unique_pipe_name("pendingidle");
    options.exit_when_idle_ms = 700;
    RunningServer server(options);
    REQUIRE(ipc::pipe_exists(options.pipe_name));
    {
        PipeConnection first = connect(options.pipe_name);
        exchange(first, hello(1));
        ipc::set_instance_create_failures_for_testing(1000000);
    }  // the first agent leaves: the idle clock starts
    PipeConnection second = connect(options.pipe_name);  // connected, but it cannot be handed over yet
    Sleep(2000);  // well past the idle limit
    CHECK_FALSE(server.finished());
    ipc::set_instance_create_failures_for_testing(0);
    exchange(second, hello(2));  // the hand-over completes and the connection is served
    CHECK_FALSE(server.finished());
    second.close();
    REQUIRE(server.join());  // now it is really idle
    CHECK(server.reason() == ServerExit::Idle);
}

#endif  // _WIN32
