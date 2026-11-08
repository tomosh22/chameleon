// InstallHooks through the agent's request dispatcher (Windows): an in-process fake broker on a real
// named pipe sends the request, the agent applies it (on its own thread in immediate mode, inside
// SyncPoint() on the calling thread in synchronised mode) and answers with HooksInstalled.
#if defined(_WIN32)

#if !defined(CML_AGENT_BUILD)
#define CML_AGENT_BUILD
#endif
#include <atomic>
#include <cstring>
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
#include "hook/user_hook_ids.h"
#include "hook_test_support.h"
#include "user_hooks_probe.h"

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
                hello_ = *hello;
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

    // The Hello the agent sent (WP-21: the restart token).
    const ipc::Hello& hello() const { return hello_; }

    // The next message of type T within the time (WP-21); module lists and EnableModule requests are answered on the way.
    template <class T>
    std::optional<T> wait_for(std::uint32_t timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            std::optional<ipc::Message> message = receive(100);
            if (!message) {
                continue;
            }
            if (const auto* wanted = std::get_if<T>(&*message)) {
                return *wanted;
            }
            if (const auto* list = std::get_if<ipc::ModuleList>(&*message)) {
                ipc::Reply reply;
                reply.request_id = list->request_id;
                send(reply);
            } else if (const auto* enable = std::get_if<ipc::EnableModule>(&*message)) {
                ipc::Reply reply;
                reply.request_id = enable->request_id;
                send(reply);
            }
        }
        return std::nullopt;
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
            } else if (const auto* enable = std::get_if<ipc::EnableModule>(&*message)) {
                ipc::Reply reply;  // WP-17: the application enabled a module; accept it
                reply.request_id = enable->request_id;
                send(reply);
            }
        }
        return std::nullopt;
    }

private:
    std::unique_ptr<ipc::PipeServer> server_;
    ipc::PipeConnection connection_;
    ipc::Hello hello_;
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
    SUBCASE("WP-22: a quiet re-point is applied like any other, and a quiet revert restores the original bytes") {
        REQUIRE(broker.send(request(13, 0, {{rig.old_fn.entry, rig.new_1.entry}})));
        std::optional<ipc::HooksInstalled> answer = broker.wait_for_answer(15000);
        REQUIRE(answer.has_value());
        CHECK(answer->status == 0);
        CHECK(rig.old_fn.as_int()(1) == 101);

        REQUIRE(broker.send(request(14, ipc::kInstallHooksQuiet, {{rig.old_fn.entry, rig.new_2.entry}})));
        answer = broker.wait_for_answer(15000);
        REQUIRE(answer.has_value());
        CHECK(answer->request_id == 14);
        CHECK_MESSAGE(answer->status == 0, answer->message);
        REQUIRE(answer->results.size() == 1);
        CHECK(answer->results[0].status == ipc::HookResultStatus::Repointed);
        CHECK(rig.old_fn.as_int()(1) == 201);

        REQUIRE(broker.send(request(15, ipc::kInstallHooksRevert | ipc::kInstallHooksQuiet, {{rig.old_fn.entry, nullptr}})));
        answer = broker.wait_for_answer(15000);
        REQUIRE(answer.has_value());
        CHECK(answer->request_id == 15);
        CHECK_MESSAGE(answer->status == 0, answer->message);
        REQUIRE(answer->results.size() == 1);
        CHECK(answer->results[0].status == ipc::HookResultStatus::Reverted);
        CHECK(rig.old_fn.as_int()(1) == 1);
        CHECK(snapshot(rig.old_fn.entry - 16, 64) == rig.original);
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

// --- application hooks (WP-17) ------------------------------------------------------------------------------

namespace {

// Enables this test executable (whose hooks live in user_hooks_probe.cpp) through the agent and waits until the
// agent tracks it, pumping the fake broker so that it answers the EnableModule.
bool enable_this_module(Agent& agent, FakeBroker& broker) {
    wchar_t path[32768];
    if (GetModuleFileNameW(nullptr, path, 32768) == 0) {
        return false;
    }
    std::uint64_t id = 0;
    if (agent.enable_module(path, 0, &id) != CML_STATUS_OK) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (agent.tracked_modules_for_testing() == 0 && std::chrono::steady_clock::now() < deadline) {
        broker.wait_for_answer(50);
    }
    return agent.tracked_modules_for_testing() == 1;
}

std::ptrdiff_t index_of(const std::vector<std::string>& log, const std::string& entry) {
    for (std::size_t i = 0; i < log.size(); ++i) {
        if (log[i] == entry) {
            return static_cast<std::ptrdiff_t>(i);
        }
    }
    return -1;
}

ipc::CompileEvent compile_event(ipc::CompilePhase phase, const std::string& text = {}) {
    ipc::CompileEvent event;
    event.phase = phase;
    event.diag = phase == ipc::CompilePhase::Error ? 2200 : 0;
    event.module = "fixture.exe";
    event.text = text;
    return event;
}

// Waits until the hook log holds at least `count` entries (hooks of events that have no answer).
bool wait_for_log(std::size_t count, Agent* sync_agent = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (hooktest_user::log().size() < count && std::chrono::steady_clock::now() < deadline) {
        if (sync_agent != nullptr) {
            sync_agent->sync_point();
        }
        Sleep(10);
    }
    return hooktest_user::log().size() >= count;
}

}  // namespace

namespace {

// What the test hooks learned about their surroundings (set from user_hooks_probe's action callback).
struct HookProbe {
    Agent* agent = nullptr;
    std::atomic<int> guarded_in_pre{-1};
    std::atomic<int> guarded_in_compile{-1};
    std::atomic<int> wait_status{0};
    std::atomic<long long> wait_ms{-1};
    std::atomic<int> poll_status{0};
    std::atomic<std::int32_t> status_in_post{-1};
};

void hook_action(void* context, const char* where) {
    auto* probe = static_cast<HookProbe*>(context);
    if (std::strcmp(where, "pre") == 0) {
        probe->guarded_in_pre = Agent::in_guarded_call() ? 1 : 0;
        cml_reply reply;
        std::memset(&reply, 0, sizeof(reply));
        const auto t0 = std::chrono::steady_clock::now();
        probe->wait_status = probe->agent->wait_request(987654, 2000, &reply);  // blocking: refused on the agent thread
        probe->wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        probe->poll_status = probe->agent->wait_request(987654, 0, &reply);  // polling is fine
    } else if (std::strcmp(where, "post") == 0) {
        probe->status_in_post = Agent::last_patch_status();
    } else if (std::strcmp(where, "compile-start") == 0) {
        probe->guarded_in_compile = Agent::in_guarded_call() ? 1 : 0;
    }
}

}  // namespace

TEST_CASE("application hooks, immediate mode: pre-patch, then the install, then post-patch - all on the agent thread") {
    Rig rig;
    const std::string pipe = unique_pipe_name();
    FakeBroker broker(pipe);
    REQUIRE(broker.listening());
    ScopedPipeEnv env(pipe);
    hooktest_user::reset(rig.old_fn.as_int());
    set_window_probe(true);

    Agent agent(CML_MODE_IMMEDIATE);
    REQUIRE(agent.start().has_value());
    REQUIRE(broker.handshake());
    REQUIRE(enable_this_module(agent, broker));

    SUBCASE("install, then a revert that runs no patch hooks") {
        REQUIRE(broker.send(request(31, 0, {{rig.old_fn.entry, rig.new_1.entry}})));
        const std::optional<ipc::HooksInstalled> answer = broker.wait_for_answer(15000);
        REQUIRE(answer.has_value());
        CHECK_MESSAGE(answer->status == 0, answer->message);
        const std::vector<std::string> log = hooktest_user::log();
        // pre saw the original function (1), post saw the replacement (101): the order is pre, install, post.
        REQUIRE(log.size() == 3);
        CHECK(log[0] == "pre:1");
        CHECK(index_of(log, "post-a:101") > 0);
        CHECK(index_of(log, "post-b:101") > 0);
        for (const unsigned long thread : hooktest_user::threads()) {
            CHECK(thread != GetCurrentThreadId());
            CHECK(thread == g_window_thread.load());  // the thread that wrote the hooks
        }
        // The answer left only after the post-patch hooks ran: nothing is added behind it.
        Sleep(100);
        CHECK(hooktest_user::log().size() == 3);

        REQUIRE(broker.send(request(32, ipc::kInstallHooksRevert, {{rig.old_fn.entry, nullptr}})));
        const std::optional<ipc::HooksInstalled> reverted = broker.wait_for_answer(15000);
        REQUIRE(reverted.has_value());
        CHECK(hooktest_user::log().size() == 3);
    }
    SUBCASE("once the pre-patch hooks ran the post-patch hooks run too, also after a failed install") {
        HookProbe probe;
        probe.agent = &agent;
        hooktest_user::set_action(&hook_action, &probe);
        REQUIRE(broker.send(request(33, 0, {{rig.old_fn.entry, rig.new_1.entry}, {rig.tiny.entry, rig.new_1.entry}})));
        const std::optional<ipc::HooksInstalled> answer = broker.wait_for_answer(15000);
        REQUIRE(answer.has_value());
        CHECK(answer->status == hook::ids::kFunctionTooShort);
        const std::vector<std::string> log = hooktest_user::log();
        // Nothing was installed: the post-patch hooks still see the original function (1) ...
        REQUIRE(log.size() == 3);
        CHECK(log[0] == "pre:1");
        CHECK(index_of(log, "post-a:1") > 0);
        CHECK(index_of(log, "post-b:1") > 0);
        // ... and can ask what happened.
        CHECK(probe.status_in_post.load() == static_cast<std::int32_t>(hook::ids::kFunctionTooShort));
        CHECK(Agent::last_patch_status() == static_cast<std::int32_t>(hook::ids::kFunctionTooShort));
        CHECK(rig.old_fn.as_int()(1) == 1);
    }
    SUBCASE("a successful install reports status 0 to the post-patch hooks") {
        HookProbe probe;
        probe.agent = &agent;
        hooktest_user::set_action(&hook_action, &probe);
        REQUIRE(broker.send(request(36, 0, {{rig.old_fn.entry, rig.new_1.entry}})));
        const std::optional<ipc::HooksInstalled> answer = broker.wait_for_answer(15000);
        REQUIRE(answer.has_value());
        CHECK(answer->status == 0);
        CHECK(probe.status_in_post.load() == 0);
        CHECK(Agent::last_patch_status() == 0);
    }
    SUBCASE("a hook on the agent thread cannot destroy the agent or block waiting for it") {
        HookProbe probe;
        probe.agent = &agent;
        hooktest_user::set_action(&hook_action, &probe);
        REQUIRE(broker.send(request(37, 0, {{rig.old_fn.entry, rig.new_1.entry}})));
        const std::optional<ipc::HooksInstalled> answer = broker.wait_for_answer(15000);
        REQUIRE(answer.has_value());
        CHECK(answer->status == 0);
        CHECK(probe.guarded_in_pre.load() == 1);  // DestroyAgent would be refused (CML5655)
        CHECK(probe.wait_status.load() == static_cast<int>(cml::agent::user_hooks::ids::kWaitInHook));  // CML5656, at once
        CHECK(probe.wait_ms.load() < 1000);  // it asked for 2 s and did not wait
        // Polling is still allowed.
        CHECK(probe.poll_status.load() == CML_STATUS_UNKNOWN_REQUEST);
    }
    SUBCASE("a hook that overflows the stack is contained and the guard page is restored, every time") {
        hooktest_user::set_fault(2);
        for (std::uint64_t round = 0; round < 2; ++round) {
            REQUIRE(broker.send(request(50 + round * 2, 0, {{rig.old_fn.entry, rig.new_1.entry}})));
            const std::optional<ipc::HooksInstalled> answer = broker.wait_for_answer(30000);
            REQUIRE(answer.has_value());
            CHECK(answer->status == 0);
            REQUIRE(broker.send(request(51 + round * 2, ipc::kInstallHooksRevert, {{rig.old_fn.entry, nullptr}})));
            REQUIRE(broker.wait_for_answer(15000).has_value());
        }
        const std::vector<std::string> log = hooktest_user::log();
        int overflows = 0;
        int posts = 0;
        for (const std::string& entry : log) {
            overflows += entry == "post-overflow" ? 1 : 0;
            posts += entry.rfind("post-b:", 0) == 0 ? 1 : 0;
        }
        CHECK(overflows == 2);
        CHECK(posts == 2);  // the hook after the overflowing one ran both times
    }
    SUBCASE("a slow compile hook does not stall the agent: an InstallHooks behind it is answered at once") {
        hooktest_user::set_compile_start_delay_ms(2500);
        REQUIRE(broker.send(compile_event(ipc::CompilePhase::Start)));
        REQUIRE(wait_for_log(1));  // the hook started (and is asleep on its worker thread)
        const auto t0 = std::chrono::steady_clock::now();
        REQUIRE(broker.send(request(38, 0, {{rig.old_fn.entry, rig.new_1.entry}})));
        const std::optional<ipc::HooksInstalled> answer = broker.wait_for_answer(15000);
        REQUIRE(answer.has_value());
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(answer->status == 0);
        CHECK(took < 2000);  // not behind the 2.5 s hook
        CHECK(index_of(hooktest_user::log(), "compile-start-done") < 0);  // it is still sleeping
        REQUIRE(wait_for_log(5));
        CHECK(index_of(hooktest_user::log(), "compile-start-done") >= 0);
    }
    SUBCASE("a compile hook cannot destroy the agent either") {
        HookProbe probe;
        probe.agent = &agent;
        hooktest_user::set_action(&hook_action, &probe);
        REQUIRE(broker.send(compile_event(ipc::CompilePhase::Start)));
        REQUIRE(wait_for_log(1));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (probe.guarded_in_compile.load() < 0 && std::chrono::steady_clock::now() < deadline) {
            Sleep(10);
        }
        CHECK(probe.guarded_in_compile.load() == 1);
    }
    SUBCASE("an empty request runs no hooks") {
        REQUIRE(broker.send(request(34, 0, {})));
        const std::optional<ipc::HooksInstalled> answer = broker.wait_for_answer(15000);
        REQUIRE(answer.has_value());
        CHECK(hooktest_user::log().empty());
    }
    SUBCASE("a hook that raises an exception is reported and the next hook still runs") {
        hooktest_user::set_fault(1);
        REQUIRE(broker.send(request(35, 0, {{rig.old_fn.entry, rig.new_1.entry}})));
        const std::optional<ipc::HooksInstalled> answer = broker.wait_for_answer(15000);
        REQUIRE(answer.has_value());
        CHECK(answer->status == 0);
        const std::vector<std::string> log = hooktest_user::log();
        CHECK(index_of(log, "post-fault") >= 0);
        CHECK(index_of(log, "post-a:101") >= 0);
        CHECK(index_of(log, "post-b:101") >= 0);
        CHECK(rig.old_fn.as_int()(1) == 101);
    }
    SUBCASE("compile notifications run the compile hooks, with the error text") {
        REQUIRE(broker.send(compile_event(ipc::CompilePhase::Start)));
        REQUIRE(broker.send(compile_event(ipc::CompilePhase::Error, "logic.cpp(3): error C2065")));
        REQUIRE(broker.send(compile_event(ipc::CompilePhase::Start)));
        REQUIRE(broker.send(compile_event(ipc::CompilePhase::Success)));
        REQUIRE(wait_for_log(4));
        const std::vector<std::string> log = hooktest_user::log();
        REQUIRE(log.size() == 4);
        CHECK(log[0] == "compile-start");
        CHECK(log[1] == "compile-error:logic.cpp(3): error C2065");
        CHECK(log[2] == "compile-start");
        CHECK(log[3] == "compile-success");
        for (const unsigned long thread : hooktest_user::threads()) {
            CHECK(thread != GetCurrentThreadId());
        }
    }
    // The installer's registry is process-wide and the next Rig may reuse these addresses: take the hook off
    // again (a revert of a function without a hook is answered with CML4311 and harmless).
    REQUIRE(broker.send(request(99, ipc::kInstallHooksRevert, {{rig.old_fn.entry, nullptr}})));
    REQUIRE(broker.wait_for_answer(15000).has_value());
    agent.stop_and_wait_for_calls();
    set_window_probe(false);
    hooktest_user::reset(nullptr);
}

TEST_CASE("application hooks, synchronised mode: nothing runs before SyncPoint, then pre, install, post on that thread") {
    Rig rig;
    const std::string pipe = unique_pipe_name();
    FakeBroker broker(pipe);
    REQUIRE(broker.listening());
    ScopedPipeEnv env(pipe);
    hooktest_user::reset(rig.old_fn.as_int());
    set_window_probe(true);

    Agent agent(CML_MODE_SYNCHRONIZED);
    REQUIRE(agent.start().has_value());
    REQUIRE(broker.handshake());
    REQUIRE(enable_this_module(agent, broker));

    REQUIRE(broker.send(compile_event(ipc::CompilePhase::Start)));
    REQUIRE(broker.send(request(41, 0, {{rig.old_fn.entry, rig.new_1.entry}})));
    REQUIRE(broker.send(compile_event(ipc::CompilePhase::Success)));
    // Without a SyncPoint neither the hooks nor the compile notifications run.
    CHECK_FALSE(broker.wait_for_answer(400).has_value());
    CHECK(hooktest_user::log().empty());
    CHECK(rig.old_fn.as_int()(1) == 1);
    CHECK(g_window_thread.load() == 0);

    std::optional<ipc::HooksInstalled> answer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!answer && std::chrono::steady_clock::now() < deadline) {
        agent.sync_point();
        answer = broker.wait_for_answer(100);
    }
    REQUIRE(answer.has_value());
    CHECK_MESSAGE(answer->status == 0, answer->message);
    REQUIRE(wait_for_log(5, &agent));
    const std::vector<std::string> log = hooktest_user::log();
    // Posting order is kept: compile-start, pre, (install), post x2, compile-success.
    REQUIRE(log.size() == 5);
    CHECK(log[0] == "compile-start");
    CHECK(log[1] == "pre:1");
    // (the order of two hooks of one kind is the order the linker placed them in)
    CHECK((index_of(log, "post-a:101") == 2 || index_of(log, "post-a:101") == 3));
    CHECK((index_of(log, "post-b:101") == 2 || index_of(log, "post-b:101") == 3));
    CHECK(log[4] == "compile-success");
    CHECK(g_window_thread.load() == GetCurrentThreadId());
    for (const unsigned long thread : hooktest_user::threads()) {
        CHECK(thread == GetCurrentThreadId());  // hooks and the install ran on the thread that called SyncPoint
    }
    REQUIRE(broker.send(request(98, ipc::kInstallHooksRevert, {{rig.old_fn.entry, nullptr}})));
    answer.reset();
    while (!answer && std::chrono::steady_clock::now() < deadline) {
        agent.sync_point();
        answer = broker.wait_for_answer(100);
    }
    REQUIRE(answer.has_value());
    agent.stop_and_wait_for_calls();
    set_window_probe(false);
    hooktest_user::reset(nullptr);
}

TEST_CASE("application hooks: a module the application did not enable contributes no hooks") {
    Rig rig;
    const std::string pipe = unique_pipe_name();
    FakeBroker broker(pipe);
    REQUIRE(broker.listening());
    ScopedPipeEnv env(pipe);
    hooktest_user::reset(rig.old_fn.as_int());

    Agent agent(CML_MODE_IMMEDIATE);
    REQUIRE(agent.start().has_value());
    REQUIRE(broker.handshake());
    // No EnableModule: the test executable's hooks are not the agent's business.
    REQUIRE(broker.send(compile_event(ipc::CompilePhase::Start)));
    Sleep(300);
    CHECK(hooktest_user::log().empty());
    agent.stop_and_wait_for_calls();
    hooktest_user::reset(nullptr);
}


// --- hot-restart (WP-21) -----------------------------------------------------------------------------------

namespace {

// Sets an environment variable of the test process for the lifetime of the object.
class ScopedVar {
public:
    ScopedVar(const wchar_t* name, const wchar_t* value) : name_(name) { SetEnvironmentVariableW(name, value); }
    ~ScopedVar() { SetEnvironmentVariableW(name_, nullptr); }
    ScopedVar(const ScopedVar&) = delete;
    ScopedVar& operator=(const ScopedVar&) = delete;
    void set(const wchar_t* value) { SetEnvironmentVariableW(name_, value); }

private:
    const wchar_t* name_;
};

std::u16string u16_of(const std::wstring& text) {
    return std::u16string(reinterpret_cast<const char16_t*>(text.data()), text.size());
}

bool contains_entry(const std::u16string& block, const std::u16string& entry) {
    const std::u16string needle = entry + std::u16string(1, u'\0');
    const std::size_t at = block.find(needle);
    return at != std::u16string::npos && (at == 0 || block[at - 1] == u'\0');
}

std::uint64_t handle_value(DWORD which) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(GetStdHandle(which)));
}

}  // namespace

TEST_CASE("hot-restart, immediate mode: the pre-restart hooks run on the agent thread and RestartInfo carries what the agent captured at start") {
    const std::string pipe = unique_pipe_name();
    FakeBroker broker(pipe);
    REQUIRE(broker.listening());
    ScopedPipeEnv env(pipe);
    ScopedVar probe(L"CML_WP21_PROBE", L"at-start");
    ScopedVar token(L"CML_RESTART_TOKEN", L"tok123");
    hooktest_user::reset(nullptr);

    Agent agent(CML_MODE_IMMEDIATE);
    REQUIRE(agent.start().has_value());
    probe.set(L"changed-later");  // after start-up: not what a restart must hand on
    token.set(nullptr);
    REQUIRE(broker.handshake());
    CHECK(broker.hello().restart_token == "tok123");  // the broker recognises a relaunched process by it
    REQUIRE(enable_this_module(agent, broker));

    ipc::Restart restart;
    restart.request_id = 71;
    REQUIRE(broker.send(restart));
    const std::optional<ipc::RestartInfo> info = broker.wait_for<ipc::RestartInfo>(15000);
    REQUIRE(info.has_value());
    CHECK(info->request_id == 71);
    CHECK_MESSAGE(info->status == 0, info->message);
    CHECK(info->hooks_run == 1);
    const std::vector<std::string> log = hooktest_user::log();
    REQUIRE(log.size() == 1);
    CHECK(log[0] == "pre-restart");
    REQUIRE(hooktest_user::threads().size() == 1);
    CHECK(hooktest_user::threads()[0] != GetCurrentThreadId());  // the agent thread, not the test's

    CHECK(info->command_line == u16_of(GetCommandLineW()));
    wchar_t image[32768];
    const DWORD image_size = GetModuleFileNameW(nullptr, image, 32768);
    CHECK(info->image_path == u16_of(std::wstring(image, image_size)));
    wchar_t cwd[32768];
    const DWORD cwd_size = GetCurrentDirectoryW(32768, cwd);
    CHECK(info->working_directory == u16_of(std::wstring(cwd, cwd_size)));
    CHECK(contains_entry(info->environment, u"CML_WP21_PROBE=at-start"));
    CHECK_FALSE(contains_entry(info->environment, u"CML_WP21_PROBE=changed-later"));
    CHECK(contains_entry(info->environment, u"CML_RESTART_TOKEN=tok123"));  // the broker replaces it
    REQUIRE(info->environment.size() >= 2);
    CHECK(info->environment.substr(info->environment.size() - 2) == std::u16string(2, u'\0'));
    CHECK(info->std_input == handle_value(STD_INPUT_HANDLE));
    CHECK(info->std_output == handle_value(STD_OUTPUT_HANDLE));
    CHECK(info->std_error == handle_value(STD_ERROR_HANDLE));
    DWORD console_processes[1] = {0};
    const bool has_console = GetConsoleProcessList(console_processes, 1) != 0;
    CHECK(((info->flags & ipc::kRestartInfoHadConsole) != 0) == has_console);  // the new process gets a console only if this one had one
    REQUIRE(info->modules.size() == 1);
    CHECK(info->modules[0].path == hooktest_user::module_path_utf8());
    agent.stop_and_wait_for_calls();
    hooktest_user::reset(nullptr);
}

TEST_CASE("hot-restart, synchronised mode: nothing happens before SyncPoint, then the hooks and the answer come from that thread") {
    const std::string pipe = unique_pipe_name();
    FakeBroker broker(pipe);
    REQUIRE(broker.listening());
    ScopedPipeEnv env(pipe);
    hooktest_user::reset(nullptr);

    Agent agent(CML_MODE_SYNCHRONIZED);
    REQUIRE(agent.start().has_value());
    REQUIRE(broker.handshake());
    CHECK(broker.hello().restart_token.empty());
    REQUIRE(enable_this_module(agent, broker));

    ipc::Restart restart;
    restart.request_id = 72;
    REQUIRE(broker.send(restart));
    CHECK_FALSE(broker.wait_for<ipc::RestartInfo>(400).has_value());  // the application has not reached its sync point
    CHECK(hooktest_user::log().empty());

    std::optional<ipc::RestartInfo> info;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!info && std::chrono::steady_clock::now() < deadline) {
        agent.sync_point();
        info = broker.wait_for<ipc::RestartInfo>(100);
    }
    REQUIRE(info.has_value());
    CHECK(info->request_id == 72);
    CHECK(info->status == 0);
    CHECK(info->hooks_run == 1);
    REQUIRE(hooktest_user::threads().size() == 1);
    CHECK(hooktest_user::threads()[0] == GetCurrentThreadId());
    agent.stop_and_wait_for_calls();
    hooktest_user::reset(nullptr);
}

TEST_CASE("hot-restart: a module that was not enabled contributes no pre-restart hook, and the answer names no module") {
    const std::string pipe = unique_pipe_name();
    FakeBroker broker(pipe);
    REQUIRE(broker.listening());
    ScopedPipeEnv env(pipe);
    hooktest_user::reset(nullptr);

    Agent agent(CML_MODE_IMMEDIATE);
    REQUIRE(agent.start().has_value());
    REQUIRE(broker.handshake());
    ipc::Restart restart;
    restart.request_id = 73;
    REQUIRE(broker.send(restart));
    const std::optional<ipc::RestartInfo> info = broker.wait_for<ipc::RestartInfo>(15000);
    REQUIRE(info.has_value());
    CHECK(info->status == 0);
    CHECK(info->hooks_run == 0);
    CHECK(info->modules.empty());
    CHECK(hooktest_user::log().empty());
    agent.stop_and_wait_for_calls();
}

TEST_CASE("hot-restart: ReenableModules becomes EnableModule requests like the application's own calls") {
    const std::string pipe = unique_pipe_name();
    FakeBroker broker(pipe);
    REQUIRE(broker.listening());
    ScopedPipeEnv env(pipe);

    Agent agent(CML_MODE_IMMEDIATE);
    REQUIRE(agent.start().has_value());
    REQUIRE(broker.handshake());

    ipc::ReenableModules ask;
    ask.request_id = 81;
    ask.modules.push_back(ipc::RestartModule{ipc::kModuleOptionAllImports, "C:\\app\\a.dll"});
    ask.modules.push_back(ipc::RestartModule{0, "b.dll"});
    REQUIRE(broker.send(ask));

    std::vector<ipc::EnableModule> enables;
    std::optional<ipc::Reply> answer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while ((enables.size() < 2 || !answer) && std::chrono::steady_clock::now() < deadline) {
        std::optional<ipc::Message> message = broker.receive(100);
        if (!message) {
            continue;
        }
        if (const auto* enable = std::get_if<ipc::EnableModule>(&*message)) {
            enables.push_back(*enable);
            ipc::Reply reply;
            reply.request_id = enable->request_id;
            broker.send(reply);
        } else if (const auto* list = std::get_if<ipc::ModuleList>(&*message)) {
            ipc::Reply reply;
            reply.request_id = list->request_id;
            broker.send(reply);
        } else if (const auto* reply = std::get_if<ipc::Reply>(&*message)) {
            if (reply->request_id == 81) {
                answer = *reply;
            }
        }
    }
    REQUIRE(enables.size() == 2);
    CHECK(enables[0].path == "C:\\app\\a.dll");
    CHECK(enables[0].options == ipc::kModuleOptionAllImports);
    CHECK(enables[1].path == "b.dll");
    CHECK(enables[1].options == 0);
    REQUIRE(answer.has_value());
    CHECK(answer->status == 0);
    // Once the broker accepted them the agent tracks them as enabled (they survive a reconnect like any other).
    const auto tracked_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (agent.tracked_modules_for_testing() < 2 && std::chrono::steady_clock::now() < tracked_deadline) {
        Sleep(10);
    }
    CHECK(agent.tracked_modules_for_testing() == 2);
    agent.stop_and_wait_for_calls();
}


TEST_CASE("hot-restart: a Restart the broker cancelled never runs its hooks, and only the newest of several runs") {
    const std::string pipe = unique_pipe_name();
    FakeBroker broker(pipe);
    REQUIRE(broker.listening());
    ScopedPipeEnv env(pipe);
    hooktest_user::reset(nullptr);

    Agent agent(CML_MODE_SYNCHRONIZED);
    REQUIRE(agent.start().has_value());
    REQUIRE(broker.handshake());
    REQUIRE(enable_this_module(agent, broker));

    // The broker gave up on request 91 while the job still waits for the application's SyncPoint().
    ipc::Restart first;
    first.request_id = 91;
    REQUIRE(broker.send(first));
    ipc::RestartCancel cancel;
    cancel.request_id = 91;
    REQUIRE(broker.send(cancel));
    Sleep(200);  // let the agent thread handle both messages
    for (int i = 0; i < 10; ++i) {
        agent.sync_point();
        Sleep(20);
    }
    CHECK_FALSE(broker.wait_for<ipc::RestartInfo>(200).has_value());
    CHECK(hooktest_user::log().empty());  // the pre-restart hook did not run for a restart nobody carries out

    // Two requests queued before the next SyncPoint(): one job, the newest request, one run of the hooks.
    ipc::Restart second;
    second.request_id = 92;
    ipc::Restart third;
    third.request_id = 93;
    REQUIRE(broker.send(second));
    REQUIRE(broker.send(third));
    Sleep(200);
    std::optional<ipc::RestartInfo> info;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!info && std::chrono::steady_clock::now() < deadline) {
        agent.sync_point();
        info = broker.wait_for<ipc::RestartInfo>(100);
    }
    REQUIRE(info.has_value());
    CHECK(info->request_id == 93);
    CHECK(info->status == 0);
    for (int i = 0; i < 10; ++i) {
        agent.sync_point();
        Sleep(20);
    }
    CHECK_FALSE(broker.wait_for<ipc::RestartInfo>(200).has_value());  // nothing for request 92
    CHECK(hooktest_user::log().size() == 1);                           // the hooks ran once
    agent.stop_and_wait_for_calls();
    hooktest_user::reset(nullptr);
}

TEST_CASE("hot-restart: launch parameters that do not fit one message are refused with an answer, before any hook runs") {
    const std::string pipe = unique_pipe_name();
    FakeBroker broker(pipe);
    REQUIRE(broker.listening());
    ScopedPipeEnv env(pipe);
    hooktest_user::reset(nullptr);
    // More than 300 000 UTF-16 units of environment (every variable is limited to 32 767 characters).
    std::vector<std::unique_ptr<ScopedVar>> big;
    std::vector<std::wstring> names;
    for (int i = 0; i < 12; ++i) {
        names.push_back(std::wstring(L"CML_WP21_BIG") + static_cast<wchar_t>(L'A' + i));
    }
    const std::wstring value(30000, L'x');
    for (const std::wstring& name : names) {
        big.push_back(std::make_unique<ScopedVar>(name.c_str(), value.c_str()));
    }

    Agent agent(CML_MODE_IMMEDIATE);
    REQUIRE(agent.start().has_value());
    REQUIRE(broker.handshake());
    REQUIRE(enable_this_module(agent, broker));
    ipc::Restart restart;
    restart.request_id = 95;
    REQUIRE(broker.send(restart));
    const std::optional<ipc::RestartInfo> info = broker.wait_for<ipc::RestartInfo>(15000);
    REQUIRE(info.has_value());  // an answer, not a dropped connection
    CHECK(info->status == 4451);
    CHECK(info->hooks_run == 0);
    CHECK(info->command_line.empty());
    CHECK(hooktest_user::log().empty());  // nothing ran for a restart that cannot happen

    // The connection is still good: a later request is served.
    ipc::ReenableModules ask;
    ask.request_id = 96;
    ask.modules.push_back(ipc::RestartModule{0, "b.dll"});
    REQUIRE(broker.send(ask));
    std::optional<ipc::Reply> reply;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!reply && std::chrono::steady_clock::now() < deadline) {
        std::optional<ipc::Message> message = broker.receive(100);
        if (!message) {
            continue;
        }
        if (const auto* enable = std::get_if<ipc::EnableModule>(&*message)) {
            ipc::Reply accepted;
            accepted.request_id = enable->request_id;
            broker.send(accepted);
        } else if (const auto* answer = std::get_if<ipc::Reply>(&*message)) {
            if (answer->request_id == 96) {
                reply = *answer;
            }
        }
    }
    REQUIRE(reply.has_value());
    CHECK(reply->status == 0);
    agent.stop_and_wait_for_calls();
}

#endif  // _WIN32
