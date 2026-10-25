// Named-pipe transport tests on real pipes. Client and server both live in this process; every
// pipe name is unique, every wait is bounded, and nothing relies on a fixed sleep for ordering.
#if defined(_WIN32)

#include "cml/ipc/pipe.h"

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

#include <atomic>
#include <chrono>
#include <cwchar>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "cml/proc/diag_ids.h"
#include "cml/proc/proc.h"
#include "doctest.h"

using namespace cml;
using cml::ipc::IoStatus;
using cml::ipc::PipeConnection;
using cml::ipc::PipeServer;

namespace {

constexpr std::uint32_t kWaitMs = 10000;

std::string unique_pipe_name(const char* tag) {
    static std::atomic<int> counter{0};
    return std::string("\\\\.\\pipe\\cml_ut_ipc_") + tag + "_" + std::to_string(GetCurrentProcessId()) + "_" +
           std::to_string(GetTickCount64()) + "_" + std::to_string(counter.fetch_add(1));
}

struct Pair {
    std::string name;
    PipeServer server;
    PipeConnection client;  // the connecting side
    PipeConnection peer;    // the accepted side
};

// Connects a client to a fresh server and accepts it, in one thread: the client may connect to the
// listening instance before the server has called accept().
Pair make_pair(const char* tag) {
    Pair pair;
    pair.name = unique_pipe_name(tag);
    Result<PipeServer> server = PipeServer::create(pair.name);
    REQUIRE_MESSAGE(server.has_value(), (server.has_value() ? "" : server.error().format()));
    pair.server = std::move(*server);
    Result<PipeConnection> client = ipc::connect_pipe(pair.name, kWaitMs);
    REQUIRE_MESSAGE(client.has_value(), (client.has_value() ? "" : client.error().format()));
    pair.client = std::move(*client);
    Result<PipeServer::Accepted> accepted = pair.server.accept(kWaitMs);
    REQUIRE(accepted.has_value());
    REQUIRE(accepted->status == IoStatus::Ok);
    pair.peer = std::move(accepted->connection);
    return pair;
}

ipc::Message receive_ok(PipeConnection& connection, std::uint32_t timeout_ms = kWaitMs) {
    Result<PipeConnection::Received> received = connection.receive(timeout_ms);
    REQUIRE_MESSAGE(received.has_value(), (received.has_value() ? "" : received.error().format()));
    REQUIRE(received->status == IoStatus::Ok);
    return std::move(received->message);
}

void send_ok(PipeConnection& connection, const ipc::Message& message) {
    Result<IoStatus> sent = connection.send(message, kWaitMs);
    REQUIRE_MESSAGE(sent.has_value(), (sent.has_value() ? "" : sent.error().format()));
    REQUIRE(*sent == IoStatus::Ok);
}

HANDLE open_raw(const std::string& name) {
    const std::wstring wide = proc::wide_from_utf8(name);
    return CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
}

void write_raw(HANDLE handle, const void* data, DWORD size) {
    DWORD written = 0;
    REQUIRE(WriteFile(handle, data, size, &written, nullptr));
    REQUIRE(written == size);
}

}  // namespace

TEST_CASE("a message travels both ways over a real pipe") {
    Pair pair = make_pair("roundtrip");
    CHECK(pair.client.is_open());
    CHECK(pair.peer.is_open());
    CHECK(pair.client.peer_process_id() == GetCurrentProcessId());

    ipc::Hello hello;
    hello.request_id = 7;
    hello.pid = 4242;
    hello.agent_path = "C:\\agent\\cml_agent.dll";
    hello.command_line = "game.exe --flag";
    send_ok(pair.client, hello);
    const ipc::Message got = receive_ok(pair.peer);
    REQUIRE(std::holds_alternative<ipc::Hello>(got));
    CHECK(std::get<ipc::Hello>(got).request_id == 7);
    CHECK(std::get<ipc::Hello>(got).command_line == "game.exe --flag");

    ipc::Welcome welcome;
    welcome.request_id = 7;
    welcome.agent_id = 1;
    welcome.broker_version = "0.1.0";
    send_ok(pair.peer, welcome);
    const ipc::Message back = receive_ok(pair.client);
    REQUIRE(std::holds_alternative<ipc::Welcome>(back));
    CHECK(std::get<ipc::Welcome>(back).broker_version == "0.1.0");
}

TEST_CASE("receive times out without losing the stream, and the next message still arrives") {
    Pair pair = make_pair("timeout");
    Result<PipeConnection::Received> nothing = pair.client.receive(30);
    REQUIRE(nothing.has_value());
    CHECK(nothing->status == IoStatus::Timeout);
    nothing = pair.client.receive(0);
    REQUIRE(nothing.has_value());
    CHECK(nothing->status == IoStatus::Timeout);

    ipc::Reply reply;
    reply.request_id = 3;
    reply.message = "late";
    send_ok(pair.peer, reply);
    const ipc::Message got = receive_ok(pair.client);
    REQUIRE(std::holds_alternative<ipc::Reply>(got));
    CHECK(std::get<ipc::Reply>(got).message == "late");
}

TEST_CASE("a wake event interrupts receive") {
    Pair pair = make_pair("wake");
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(wake != nullptr);
    SetEvent(wake);
    Result<PipeConnection::Received> woken = pair.client.receive(ipc::kInfiniteTimeout, wake);
    REQUIRE(woken.has_value());
    CHECK(woken->status == IoStatus::Woken);
    // Auto-reset: consumed, so the next call really waits (and times out).
    Result<PipeConnection::Received> timeout = pair.client.receive(20, wake);
    REQUIRE(timeout.has_value());
    CHECK(timeout->status == IoStatus::Timeout);
    CloseHandle(wake);
}

TEST_CASE("cancel wakes a blocked receive and is sticky") {
    Pair pair = make_pair("cancel");
    std::atomic<int> status{-1};
    std::thread blocked([&] {
        Result<PipeConnection::Received> received = pair.client.receive(ipc::kInfiniteTimeout);
        status.store(received.has_value() ? static_cast<int>(received->status) : -2);
    });
    pair.client.cancel();  // may run before the thread blocks: still ends the wait
    blocked.join();
    CHECK(status.load() == static_cast<int>(IoStatus::Cancelled));

    Result<PipeConnection::Received> again = pair.client.receive(ipc::kInfiniteTimeout);
    REQUIRE(again.has_value());
    CHECK(again->status == IoStatus::Cancelled);
    Result<IoStatus> sent = pair.client.send(ipc::Reply{}, 100);
    REQUIRE(sent.has_value());
    CHECK(*sent == IoStatus::Cancelled);
}

TEST_CASE("closing one end is seen as Closed by the other") {
    Pair pair = make_pair("close");
    send_ok(pair.peer, ipc::Reply{});
    pair.peer.close();
    CHECK_FALSE(pair.peer.is_open());
    // Data sent before the close is still delivered, then the stream ends.
    const ipc::Message last = receive_ok(pair.client);
    CHECK(std::holds_alternative<ipc::Reply>(last));
    Result<PipeConnection::Received> ended = pair.client.receive(kWaitMs);
    REQUIRE(ended.has_value());
    CHECK(ended->status == IoStatus::Closed);
    Result<IoStatus> sent = pair.client.send(ipc::Reply{}, 1000);
    REQUIRE(sent.has_value());
    CHECK(*sent == IoStatus::Closed);
}

TEST_CASE("messages larger than the pipe buffer and long bursts arrive intact and in order") {
    Pair pair = make_pair("large");
    ipc::ModuleList list;
    list.request_id = 99;
    for (int i = 0; i < 3000; ++i) {
        list.modules.push_back({0x10000u + static_cast<std::uint64_t>(i) * 0x1000u, 0x1000u,
                                "C:\\Windows\\System32\\module_number_" + std::to_string(i) + ".dll"});
    }
    // ~250 KB: far more than the 64 KB pipe buffer, so sender and receiver must overlap.
    std::atomic<bool> all_sent{true};  // assertions stay on the main thread
    std::thread sender([&] {
        Result<IoStatus> sent = pair.client.send(list, kWaitMs);
        all_sent.store(all_sent.load() && sent.has_value() && *sent == IoStatus::Ok);
        for (int i = 0; i < 200; ++i) {
            ipc::Reply reply;
            reply.request_id = static_cast<std::uint64_t>(i);
            reply.message = "burst " + std::to_string(i);
            sent = pair.client.send(reply, kWaitMs);
            all_sent.store(all_sent.load() && sent.has_value() && *sent == IoStatus::Ok);
        }
    });
    const ipc::Message got = receive_ok(pair.peer);
    REQUIRE(std::holds_alternative<ipc::ModuleList>(got));
    const auto& received = std::get<ipc::ModuleList>(got);
    CHECK(received.request_id == 99);
    REQUIRE(received.modules.size() == 3000);
    CHECK(received.modules[2999].path == list.modules[2999].path);
    for (int i = 0; i < 200; ++i) {
        const ipc::Message burst = receive_ok(pair.peer);
        REQUIRE(std::holds_alternative<ipc::Reply>(burst));
        CHECK(std::get<ipc::Reply>(burst).request_id == static_cast<std::uint64_t>(i));
    }
    sender.join();
    CHECK(all_sent.load());
}

TEST_CASE("malformed input on the wire is diagnosed") {
    const std::string name = unique_pipe_name("malformed");
    Result<PipeServer> server = PipeServer::create(name);
    REQUIRE(server.has_value());

    SUBCASE("oversized length") {
        HANDLE raw = open_raw(name);
        REQUIRE(raw != INVALID_HANDLE_VALUE);
        Result<PipeServer::Accepted> accepted = server->accept(kWaitMs);
        REQUIRE(accepted.has_value());
        REQUIRE(accepted->status == IoStatus::Ok);
        const unsigned char header[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x01, 0x00};
        write_raw(raw, header, sizeof(header));
        Result<PipeConnection::Received> received = accepted->connection.receive(kWaitMs);
        REQUIRE_FALSE(received.has_value());
        CHECK(received.error().code == ids::kFrameHeaderInvalid);
        CloseHandle(raw);
    }
    SUBCASE("unknown message type") {
        HANDLE raw = open_raw(name);
        REQUIRE(raw != INVALID_HANDLE_VALUE);
        Result<PipeServer::Accepted> accepted = server->accept(kWaitMs);
        REQUIRE(accepted.has_value());
        REQUIRE(accepted->status == IoStatus::Ok);
        const unsigned char header[8] = {0x00, 0x00, 0x00, 0x00, 0x7E, 0x7E, 0x01, 0x00};
        write_raw(raw, header, sizeof(header));
        // The frame is whole, only its type is unknown: it is rejected, not fatal ...
        Result<PipeConnection::Received> received = accepted->connection.receive(kWaitMs);
        REQUIRE(received.has_value());
        CHECK(received->status == IoStatus::Rejected);
        CHECK(received->error.code == ids::kMessageTypeUnsupported);
        // ... and the stream is still in sync: the next, valid frame is read normally.
        ipc::Reply reply;
        reply.request_id = 42;
        Result<std::vector<std::uint8_t>> frame = ipc::encode_frame(reply);
        REQUIRE(frame.has_value());
        write_raw(raw, frame->data(), static_cast<DWORD>(frame->size()));
        const ipc::Message next = receive_ok(accepted->connection);
        REQUIRE(std::holds_alternative<ipc::Reply>(next));
        CHECK(std::get<ipc::Reply>(next).request_id == 42);
        CloseHandle(raw);
    }
    SUBCASE("a Log with invalid UTF-8 is rejected without breaking the stream") {
        HANDLE raw = open_raw(name);
        REQUIRE(raw != INVALID_HANDLE_VALUE);
        Result<PipeServer::Accepted> accepted = server->accept(kWaitMs);
        REQUIRE(accepted.has_value());
        REQUIRE(accepted->status == IoStatus::Ok);
        // level 3, text length 2, bytes FF FE (not UTF-8)
        const unsigned char bad_log[] = {0x0A, 0x00, 0x00, 0x00, 0x60, 0x00, 0x01, 0x00,
                                         0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xFF, 0xFE};
        write_raw(raw, bad_log, sizeof(bad_log));
        Result<PipeConnection::Received> received = accepted->connection.receive(kWaitMs);
        REQUIRE(received.has_value());
        CHECK(received->status == IoStatus::Rejected);
        CHECK(received->error.code == ids::kPayloadMalformed);
        CloseHandle(raw);
    }
    SUBCASE("a frame cut off by a disconnect is Closed, not a crash") {
        HANDLE raw = open_raw(name);
        REQUIRE(raw != INVALID_HANDLE_VALUE);
        Result<PipeServer::Accepted> accepted = server->accept(kWaitMs);
        REQUIRE(accepted.has_value());
        REQUIRE(accepted->status == IoStatus::Ok);
        const unsigned char partial[5] = {0x10, 0x00, 0x00, 0x00, 0x01};
        write_raw(raw, partial, sizeof(partial));
        CloseHandle(raw);
        Result<PipeConnection::Received> received = accepted->connection.receive(kWaitMs);
        REQUIRE(received.has_value());
        CHECK(received->status == IoStatus::Closed);
    }
}

TEST_CASE("connecting to a pipe that does not exist is CML5205") {
    const std::string name = unique_pipe_name("absent");
    CHECK_FALSE(ipc::pipe_exists(name));
    Result<PipeConnection> client = ipc::connect_pipe(name, 100);
    REQUIRE_FALSE(client.has_value());
    CHECK(client.error().code == ids::kBrokerUnreachable);
    CHECK_FALSE(client.error().what_to_do.empty());
}

TEST_CASE("pipe names must live in the pipe namespace") {
    Result<PipeConnection> client = ipc::connect_pipe("C:\\temp\\not_a_pipe", 100);
    REQUIRE_FALSE(client.has_value());
    CHECK(client.error().code == ids::kPipeNameInvalid);
    Result<PipeServer> server = PipeServer::create("cml_without_prefix");
    REQUIRE_FALSE(server.has_value());
    CHECK(server.error().code == ids::kPipeNameInvalid);
    CHECK_FALSE(PipeServer::create("\\\\.\\pipe\\").has_value());
}

TEST_CASE("a second server cannot take over a name that is in use") {
    const std::string name = unique_pipe_name("squat");
    Result<PipeServer> first = PipeServer::create(name);
    REQUIRE(first.has_value());
    CHECK(ipc::pipe_exists(name));
    Result<PipeServer> second = PipeServer::create(name);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == ids::kPipeCreateFailed);
}

TEST_CASE("accept times out, can be woken and can be cancelled") {
    const std::string name = unique_pipe_name("accept");
    Result<PipeServer> server = PipeServer::create(name);
    REQUIRE(server.has_value());

    Result<PipeServer::Accepted> none = server->accept(30);
    REQUIRE(none.has_value());
    CHECK(none->status == IoStatus::Timeout);

    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(wake != nullptr);
    SetEvent(wake);
    Result<PipeServer::Accepted> woken = server->accept(ipc::kInfiniteTimeout, wake);
    REQUIRE(woken.has_value());
    CHECK(woken->status == IoStatus::Woken);
    CloseHandle(wake);

    // A client that arrives after a timed-out accept is still accepted by the next call.
    Result<PipeConnection> client = ipc::connect_pipe(name, kWaitMs);
    REQUIRE(client.has_value());
    Result<PipeServer::Accepted> accepted = server->accept(kWaitMs);
    REQUIRE(accepted.has_value());
    CHECK(accepted->status == IoStatus::Ok);

    server->cancel();
    Result<PipeServer::Accepted> cancelled = server->accept(ipc::kInfiniteTimeout);
    REQUIRE(cancelled.has_value());
    CHECK(cancelled->status == IoStatus::Cancelled);
}

TEST_CASE("one server accepts several simultaneous clients") {
    const std::string name = unique_pipe_name("multi");
    Result<PipeServer> server = PipeServer::create(name);
    REQUIRE(server.has_value());

    constexpr int kClients = 4;
    std::vector<PipeConnection> clients;
    std::vector<PipeConnection> peers;
    for (int i = 0; i < kClients; ++i) {
        // The client connects to the instance that is already listening; accept hands it over and
        // creates the next one.
        Result<PipeConnection> client = ipc::connect_pipe(name, kWaitMs);
        REQUIRE_MESSAGE(client.has_value(), (client.has_value() ? "" : client.error().format()));
        clients.push_back(std::move(*client));
        Result<PipeServer::Accepted> accepted = server->accept(kWaitMs);
        REQUIRE(accepted.has_value());
        REQUIRE(accepted->status == IoStatus::Ok);
        peers.push_back(std::move(accepted->connection));
    }
    for (int i = 0; i < kClients; ++i) {
        ipc::Reply reply;
        reply.request_id = static_cast<std::uint64_t>(i);
        send_ok(clients[static_cast<std::size_t>(i)], reply);
    }
    for (int i = kClients - 1; i >= 0; --i) {  // other order than they connected
        const ipc::Message got = receive_ok(peers[static_cast<std::size_t>(i)]);
        REQUIRE(std::holds_alternative<ipc::Reply>(got));
        CHECK(std::get<ipc::Reply>(got).request_id == static_cast<std::uint64_t>(i));
    }
}

TEST_CASE("the pipe is restricted to the current user") {
    const std::string name = unique_pipe_name("security");
    Result<PipeServer> server = PipeServer::create(name);
    REQUIRE(server.has_value());
    HANDLE raw = open_raw(name);
    REQUIRE(raw != INVALID_HANDLE_VALUE);

    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    REQUIRE(GetSecurityInfo(raw, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                            nullptr, &descriptor) == ERROR_SUCCESS);
    REQUIRE(dacl != nullptr);
    CHECK(dacl->AceCount == 1);  // exactly one entry: nobody else is granted anything
    LPVOID ace = nullptr;
    REQUIRE(GetAce(dacl, 0, &ace));
    CHECK(static_cast<ACE_HEADER*>(ace)->AceType == ACCESS_ALLOWED_ACE_TYPE);
    PSID sid = &static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart;
    LPWSTR sid_text = nullptr;
    REQUIRE(ConvertSidToStringSidW(sid, &sid_text));
    const std::string allowed = proc::utf8_from_wide(sid_text, std::wcslen(sid_text));
    LocalFree(sid_text);
    Result<std::string> user = proc::current_user_sid();
    REQUIRE(user.has_value());
    CHECK(allowed == *user);
    LocalFree(descriptor);
    CloseHandle(raw);
}

TEST_CASE("resolve_pipe_name precedence: explicit, environment, session default") {
    SetEnvironmentVariableW(L"CML_PIPE_NAME", nullptr);

    Result<std::string> by_default = ipc::resolve_pipe_name(std::string());
    REQUIRE(by_default.has_value());
    Result<std::string> sid = proc::logon_session_sid();
    REQUIRE(sid.has_value());
    CHECK(*by_default == "\\\\.\\pipe\\cml_broker_" + *sid);

    SetEnvironmentVariableW(L"CML_PIPE_NAME", L"\\\\.\\pipe\\from_environment");
    Result<std::string> by_env = ipc::resolve_pipe_name(std::string());
    REQUIRE(by_env.has_value());
    CHECK(*by_env == "\\\\.\\pipe\\from_environment");

    Result<std::string> by_flag = ipc::resolve_pipe_name("\\\\.\\PIPE\\from_flag");
    REQUIRE(by_flag.has_value());
    CHECK(*by_flag == "\\\\.\\PIPE\\from_flag");  // explicit wins; prefix check is case-insensitive

    SetEnvironmentVariableW(L"CML_PIPE_NAME", L"not_a_pipe_name");
    Result<std::string> bad = ipc::resolve_pipe_name(std::string());
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ids::kPipeNameInvalid);

    SetEnvironmentVariableW(L"CML_PIPE_NAME", nullptr);
}

TEST_CASE("refreshing the listening instance never frees the pipe name") {
    const std::string name = unique_pipe_name("refresh");
    Result<PipeServer> server = PipeServer::create(name);
    REQUIRE(server.has_value());
    for (int i = 0; i < 20; ++i) {
        server->refresh_instance();  // the replacement exists before the old instance is closed
        CHECK(ipc::pipe_exists(name));
    }
    // Still the owner: nobody else can create the pipe, and clients are still served.
    Result<PipeServer> usurper = PipeServer::create(name);
    CHECK_FALSE(usurper.has_value());
    Result<PipeConnection> client = ipc::connect_pipe(name, kWaitMs);
    REQUIRE(client.has_value());
    Result<PipeServer::Accepted> accepted = server->accept(kWaitMs);
    REQUIRE(accepted.has_value());
    CHECK(accepted->status == IoStatus::Ok);
}

#endif  // _WIN32
