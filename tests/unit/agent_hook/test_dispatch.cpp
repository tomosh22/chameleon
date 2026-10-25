// InstallHooks through the agent's request dispatcher (Windows): an in-process fake broker on a real
// named pipe sends the request, the agent applies it (on its own thread in immediate mode, inside
// SyncPoint() on the calling thread in synchronised mode) and answers with HooksInstalled.
#if defined(_WIN32)

#if !defined(CML_AGENT_BUILD)
#define CML_AGENT_BUILD
#endif
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "agent.h"
#include "cml/ipc/messages.h"
#include "cml/ipc/pipe.h"
#include "doctest.h"
#include "hook/diag_ids.h"
#include "hook/installer.h"
#include "hook_test_support.h"

using namespace hooktest;
namespace ipc = cml::ipc;
namespace hook = cml::agent::hook;
using cml::agent::Agent;

namespace {

std::string unique_pipe_name() {
    static std::atomic<int> counter{0};
    return "\\\\.\\pipe\\cml_ut_hooks_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()) +
           "_" + std::to_string(counter.fetch_add(1));
}

class ScopedPipeEnv {
public:
    explicit ScopedPipeEnv(const std::string& name) {
        SetEnvironmentVariableW(L"CML_PIPE_NAME", std::wstring(name.begin(), name.end()).c_str());
    }
    ~ScopedPipeEnv() { SetEnvironmentVariableW(L"CML_PIPE_NAME", nullptr); }
};

// The broker's side: a pipe server and one accepted connection, answering the handshake.
class FakeBroker {
public:
    explicit FakeBroker(const std::string& name) {
        cml::Result<ipc::PipeServer> created = ipc::PipeServer::create(name);
        if (created) {
            server_ = std::make_unique<ipc::PipeServer>(std::move(*created));
        }
    }
    bool listening() const { return server_ != nullptr; }

    // Accepts the agent and completes Hello/Welcome and the first ModuleList.
    bool handshake() {
        cml::Result<ipc::PipeServer::Accepted> accepted = server_->accept(15000);
        if (!accepted || accepted->status != ipc::IoStatus::Ok) {
            return false;
        }
        connection_ = std::move(accepted->connection);
        bool welcomed = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            std::optional<ipc::Message> message = receive(500);
            if (!message) {
                continue;
            }
            if (const auto* hello = std::get_if<ipc::Hello>(&*message)) {
                ipc::Welcome welcome;
                welcome.request_id = hello->request_id;
                welcome.agent_id = 1;
                welcome.broker_version = "fake";
                if (!send(welcome)) {
                    return false;
                }
                welcomed = true;
            } else if (const auto* list = std::get_if<ipc::ModuleList>(&*message)) {
                ipc::Reply reply;
                reply.request_id = list->request_id;
                send(reply);
                return welcomed;
            }
        }
        return false;
    }

    bool send(const ipc::Message& message) {
        cml::Result<ipc::IoStatus> sent = connection_.send(message, 5000);
        return sent && *sent == ipc::IoStatus::Ok;
    }

    std::optional<ipc::Message> receive(std::uint32_t timeout_ms) {
        cml::Result<ipc::PipeConnection::Received> received = connection_.receive(timeout_ms);
        if (!received || received->status != ipc::IoStatus::Ok) {
            return std::nullopt;
        }
        return std::move(received->message);
    }

    // Next HooksInstalled within the time (other traffic is answered or skipped).
    std::optional<ipc::HooksInstalled> wait_for_answer(std::uint32_t timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            std::optional<ipc::Message> message = receive(100);
            if (!message) {
                continue;
            }
            if (const auto* answer = std::get_if<ipc::HooksInstalled>(&*message)) {
                return *answer;
            }
            if (const auto* list = std::get_if<ipc::ModuleList>(&*message)) {
                ipc::Reply reply;
                reply.request_id = list->request_id;
                send(reply);
            }
        }
        return std::nullopt;
    }

private:
    std::unique_ptr<ipc::PipeServer> server_;
    ipc::PipeConnection connection_;
};

// Remembers which thread applied the hooks.
std::atomic<DWORD> g_window_thread{0};
void note_window_thread(void*) {
    g_window_thread = GetCurrentThreadId();
}

struct Rig {
    Arena arena;
    Placed old_fn, other_fn, tiny, new_1, new_2;
    Bytes original;
    Rig() {
        old_fn = arena.place_padded(cl_release(0), 8, 8);
        other_fn = arena.place_padded(cl_debug(0), 7, 8);
        tiny = arena.place_padded(xor_ret(), 0, 0);  // cannot be hooked: shorter than 5 bytes, no padding
        new_1 = arena.place_padded(lea_ret(100), 8, 8);
        new_2 = arena.place_padded(lea_ret(200), 8, 8);
        arena.seal();
        original = snapshot(old_fn.entry - 16, 64);
    }
};

ipc::InstallHooks request(std::uint64_t id, std::uint32_t flags,
                          std::initializer_list<std::pair<const std::uint8_t*, const std::uint8_t*>> hooks) {
    ipc::InstallHooks r;
    r.request_id = id;
    r.flags = flags;
    int n = 0;
    for (const auto& h : hooks) {
        r.hooks.push_back({reinterpret_cast<std::uint64_t>(h.first), reinterpret_cast<std::uint64_t>(h.second),
                           "fn" + std::to_string(n++)});
    }
    return r;
}

void set_window_probe(bool on) {
    hook::InstallerOptions options;
    if (on) {
        options.test_in_window = &note_window_thread;
    }
    hook::process_installer().set_test_options(options);
    g_window_thread = 0;
}

}  // namespace

TEST_CASE("immediate mode: the agent thread applies InstallHooks and answers HooksInstalled") {
    Rig rig;
    const std::string pipe = unique_pipe_name();
    FakeBroker broker(pipe);
    REQUIRE(broker.listening());
    ScopedPipeEnv env(pipe);
    set_window_probe(true);

    Agent agent(CML_MODE_IMMEDIATE);
    REQUIRE(agent.start().has_value());
    REQUIRE(broker.handshake());

    SUBCASE("install, re-point, revert") {
        REQUIRE(broker.send(request(5, 0, {{rig.old_fn.entry, rig.new_1.entry}, {rig.other_fn.entry, rig.new_1.entry}})));
        std::optional<ipc::HooksInstalled> answer = broker.wait_for_answer(15000);
        REQUIRE(answer.has_value());
        CHECK(answer->request_id == 5);
        CHECK_MESSAGE(answer->status == 0, answer->message);
        REQUIRE(answer->results.size() == 2);
        CHECK(answer->results[0].status == ipc::HookResultStatus::Installed);
        CHECK(answer->results[0].strategy == ipc::HookStrategy::PaddingTwoByte);
        CHECK(answer->results[1].status == ipc::HookResultStatus::Installed);
        CHECK(answer->attempts == 1);
        CHECK(rig.old_fn.as_int()(1) == 101);
        CHECK(rig.other_fn.as_int()(1) == 101);
        CHECK(g_window_thread.load() != 0);
        CHECK(g_window_thread.load() != GetCurrentThreadId());  // the agent thread, not this one

        REQUIRE(broker.send(request(6, 0, {{rig.old_fn.entry, rig.new_2.entry}})));
        answer = broker.wait_for_answer(15000);
        REQUIRE(answer.has_value());
        CHECK(answer->request_id == 6);
        REQUIRE(answer->results.size() == 1);
        CHECK(answer->results[0].status == ipc::HookResultStatus::Repointed);
        CHECK(rig.old_fn.as_int()(1) == 201);

        REQUIRE(broker.send(request(7, ipc::kInstallHooksRevert,
                                    {{rig.old_fn.entry, nullptr}, {rig.other_fn.entry, nullptr}})));
        answer = broker.wait_for_answer(15000);
        REQUIRE(answer.has_value());
        CHECK(answer->request_id == 7);
        CHECK_MESSAGE(answer->status == 0, answer->message);
        REQUIRE(answer->results.size() == 2);
        CHECK(answer->results[0].status == ipc::HookResultStatus::Reverted);
        CHECK(rig.old_fn.as_int()(1) == 1);
        CHECK(snapshot(rig.old_fn.entry - 16, 64) == rig.original);
    }
    SUBCASE("a request that cannot succeed is answered with the diagnostic and changes nothing") {
        REQUIRE(broker.send(request(9, 0, {{rig.old_fn.entry, rig.new_1.entry}, {rig.tiny.entry, rig.new_1.entry}})));
        std::optional<ipc::HooksInstalled> answer = broker.wait_for_answer(15000);
        REQUIRE(answer.has_value());
        CHECK(answer->request_id == 9);
        CHECK(answer->status == hook::ids::kFunctionTooShort);
        CHECK(answer->message.find("CML4301") != std::string::npos);
        REQUIRE(answer->results.size() == 2);
        CHECK(answer->results[0].status == ipc::HookResultStatus::NotAttempted);
        CHECK(answer->results[0].diag == hook::ids::kBatchAborted);
        CHECK(answer->results[1].status == ipc::HookResultStatus::Failed);
        CHECK(answer->results[1].message.find("fn1") != std::string::npos);
        CHECK(answer->attempts == 0);
        CHECK(snapshot(rig.old_fn.entry - 16, 64) == rig.original);
        CHECK(rig.old_fn.as_int()(1) == 1);
    }
    SUBCASE("an empty request is answered") {
        REQUIRE(broker.send(request(11, 0, {})));
        std::optional<ipc::HooksInstalled> answer = broker.wait_for_answer(15000);
        REQUIRE(answer.has_value());
        CHECK(answer->request_id == 11);
        CHECK(answer->status == 0);
        CHECK(answer->results.empty());
    }
    agent.stop_and_wait_for_calls();
    set_window_probe(false);
}

TEST_CASE("synchronised mode: the hooks are applied inside SyncPoint on the calling thread") {
    Rig rig;
    const std::string pipe = unique_pipe_name();
    FakeBroker broker(pipe);
    REQUIRE(broker.listening());
    ScopedPipeEnv env(pipe);
    set_window_probe(true);

    Agent agent(CML_MODE_SYNCHRONIZED);
    REQUIRE(agent.start().has_value());
    REQUIRE(broker.handshake());

    REQUIRE(broker.send(request(21, 0, {{rig.old_fn.entry, rig.new_1.entry}})));
    // Nothing happens until the application reaches a SyncPoint: no answer, no hook.
    CHECK_FALSE(broker.wait_for_answer(400).has_value());
    CHECK(rig.old_fn.as_int()(1) == 1);
    CHECK(g_window_thread.load() == 0);

    // The request may still be in flight to the agent thread: SyncPoint until it has run.
    std::optional<ipc::HooksInstalled> answer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!answer && std::chrono::steady_clock::now() < deadline) {
        agent.sync_point();
        answer = broker.wait_for_answer(100);
    }
    REQUIRE(answer.has_value());
    CHECK(answer->request_id == 21);
    CHECK_MESSAGE(answer->status == 0, answer->message);
    CHECK(rig.old_fn.as_int()(1) == 101);
    CHECK(g_window_thread.load() == GetCurrentThreadId());  // the thread that called SyncPoint

    REQUIRE(broker.send(request(22, ipc::kInstallHooksRevert, {{rig.old_fn.entry, nullptr}})));
    answer.reset();
    while (!answer && std::chrono::steady_clock::now() < deadline) {
        agent.sync_point();
        answer = broker.wait_for_answer(100);
    }
    REQUIRE(answer.has_value());
    CHECK(answer->request_id == 22);
    CHECK(answer->results.size() == 1);
    CHECK(snapshot(rig.old_fn.entry - 16, 64) == rig.original);
    agent.stop_and_wait_for_calls();
    set_window_probe(false);
}

#endif  // _WIN32
