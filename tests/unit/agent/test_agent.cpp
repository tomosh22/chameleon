// End-to-end tests of the agent through the public header, exactly as an application uses it:
// Chameleon_API.h LoadLibrary()s cml_agent.dll from the test exe's directory. The broker is
// cml_core's ConnectionServer running on a thread of this process, on a pipe name that is unique
// per test, so parallel CI jobs and reruns never collide. Every wait is bounded; nothing sleeps
// for a fixed time to "let things happen".
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "Chameleon_API.h"
#include "cml/core/connection_server.h"
#include "cml/ipc/pipe.h"
#include "doctest.h"

namespace {

std::wstring exe_directory() {
    const std::wstring self = cml::GetCurrentModulePath();
    return self.substr(0, self.find_last_of(L"\\/"));
}

std::wstring file_name_of(const std::wstring& path) {
    return path.substr(path.find_last_of(L"\\/") + 1);
}

std::string unique_pipe_name(const char* tag) {
    static std::atomic<int> counter{0};
    return std::string("\\\\.\\pipe\\cml_ut_agent_") + tag + "_" + std::to_string(GetCurrentProcessId()) + "_" +
           std::to_string(GetTickCount64()) + "_" + std::to_string(counter.fetch_add(1));
}

std::wstring widen(const std::string& text) {
    return std::wstring(text.begin(), text.end());  // pipe names are ASCII
}

template <class Predicate>
bool wait_until(Predicate predicate, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (!predicate()) {
        if (GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(5);
    }
    return true;
}

// Sets an environment variable for the lifetime of the object, then restores the old state.
class ScopedEnv {
public:
    ScopedEnv(const wchar_t* name, const std::wstring& value) : name_(name) {
        wchar_t buffer[2048];
        const DWORD n = GetEnvironmentVariableW(name, buffer, 2048);
        had_ = n > 0 && n < 2048;
        if (had_) {
            old_ = buffer;
        }
        SetEnvironmentVariableW(name, value.c_str());
    }
    ~ScopedEnv() { SetEnvironmentVariableW(name_.c_str(), had_ ? old_.c_str() : nullptr); }

private:
    std::wstring name_;
    std::wstring old_;
    bool had_ = false;
};

// A broker running in this process.
class TestBroker {
public:
    explicit TestBroker(std::string pipe) : pipe_(std::move(pipe)) {
        cml::core::ServerOptions options;
        options.pipe_name = pipe_;
        server_ = std::make_unique<cml::core::ConnectionServer>(options);
        thread_ = std::thread([this] {
            const cml::Result<cml::core::ServerExit> result = server_->run();
            if (!result) {
                error_ = result.error().format();
            }
            finished_.store(true);
        });
        ready_ = wait_until([this] { return cml::ipc::pipe_exists(pipe_) || finished_.load(); }, 10000) &&
                 !finished_.load();
    }
    ~TestBroker() {
        server_->stop();
        thread_.join();
    }
    bool ready() const { return ready_; }
    const std::string& error() const { return error_; }

private:
    std::string pipe_;
    std::unique_ptr<cml::core::ConnectionServer> server_;
    std::thread thread_;
    std::atomic<bool> finished_{false};
    std::string error_;
    bool ready_ = false;
};

// Resolves an export of the already loaded cml_agent.dll (the test-only ones are not in the header).
template <class Fn>
Fn agent_export(const char* name) {
    using Generic = void (*)(void);
    const HMODULE module = GetModuleHandleW(L"cml_agent.dll");
    REQUIRE(module != nullptr);
    return reinterpret_cast<Fn>(reinterpret_cast<Generic>(GetProcAddress(module, name)));
}

// Environment that makes the agent unable to find or start a broker.
struct NoBrokerEnv {
    ScopedEnv pipe;
    ScopedEnv broker;
    explicit NoBrokerEnv(const char* tag)
        : pipe(L"CML_PIPE_NAME", widen(unique_pipe_name(tag))),
          broker(L"CML_BROKER_PATH", exe_directory() + L"\\no_such_broker.exe") {}
};

}  // namespace

// Runs first on purpose: nothing else has loaded the real agent yet.
TEST_CASE("an agent DLL with another ABI version is refused gracefully") {
    cml_status status = CML_STATUS_OK;
    const std::wstring fake_dir = exe_directory() + L"\\fake_agent_abi";
    cml_agent agent = cml_create_agent(fake_dir.c_str(), CML_MODE_IMMEDIATE, &status);
    CHECK(agent == nullptr);
    CHECK(status == CML_STATUS_ABI_MISMATCH);
}

TEST_CASE("a directory without cml_agent.dll is reported, not crashed on") {
    cml_status status = CML_STATUS_OK;
    cml::Agent agent = cml::CreateDefaultAgent((exe_directory() + L"\\no_such_subdirectory").c_str(), &status);
    CHECK_FALSE(agent.IsValid());
    CHECK(status == CML_STATUS_AGENT_NOT_FOUND);
}

TEST_CASE("without a broker the agent keeps the application running and destroys promptly") {
    NoBrokerEnv env("nobroker");
    cml_status status = CML_STATUS_ABI_MISMATCH;
    cml::Agent agent = cml::CreateDefaultAgent(exe_directory().c_str(), &status);
    REQUIRE(agent.IsValid());
    CHECK(status == CML_STATUS_OK);
    CHECK_FALSE(agent.IsConnected());

    // Requests are queued without waiting for the broker, and stay open.
    const cml::Request enable = agent.EnableModule(cml::GetCurrentModulePath(), cml::ModuleOptions::AllImports);
    CHECK(enable.status == CML_STATUS_OK);
    CHECK(enable.id != 0);
    CHECK(agent.WaitForRequest(enable, 0) == CML_STATUS_TIMEOUT);
    CHECK(agent.WaitForRequest(enable, 100) == CML_STATUS_TIMEOUT);  // a bounded wait just elapses
    CHECK(agent.WaitForRequest(cml::Request{CML_STATUS_OK, 987654}, 0) == CML_STATUS_UNKNOWN_REQUEST);
    CHECK(agent.EnableModule(L"").status == CML_STATUS_INVALID_ARGUMENT);

    // Not-yet-implemented calls say so.
    cml_hotkeys hotkeys;
    hotkeys.struct_size = static_cast<uint32_t>(sizeof(hotkeys));
    hotkeys.reload.modifiers = CML_HOTKEY_CONTROL;
    hotkeys.reload.virtual_key = 'R';
    hotkeys.restart.modifiers = 0;
    hotkeys.restart.virtual_key = 0;
    CHECK(agent.ScheduleReload() == CML_STATUS_NOT_IMPLEMENTED);
    CHECK(agent.WaitForReload(10) == CML_STATUS_NOT_IMPLEMENTED);
    CHECK(agent.ScheduleRestart() == CML_STATUS_NOT_IMPLEMENTED);
    CHECK(agent.SetGlobalHotkeys(hotkeys) == CML_STATUS_NOT_IMPLEMENTED);
    hotkeys.struct_size = 1;
    CHECK(agent.SetGlobalHotkeys(hotkeys) == CML_STATUS_INVALID_ARGUMENT);

    const auto start = std::chrono::steady_clock::now();
    cml::DestroyAgent(agent);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::seconds(5));
    CHECK_FALSE(agent.IsValid());
}

TEST_CASE("the C API works end to end against a broker") {
    const std::string pipe = unique_pipe_name("capi");
    ScopedEnv pipe_env(L"CML_PIPE_NAME", widen(pipe));
    TestBroker broker(pipe);
    REQUIRE_MESSAGE(broker.ready(), broker.error());

    cml_status status = CML_STATUS_ABI_MISMATCH;
    cml_agent agent = cml_create_agent(exe_directory().c_str(), CML_MODE_IMMEDIATE, &status);
    REQUIRE(agent != nullptr);
    REQUIRE(status == CML_STATUS_OK);
    REQUIRE(wait_until([&] { return cml_is_connected(agent) != 0; }, 15000));

    wchar_t self[32768];
    uint32_t length = 0;
    REQUIRE(cml_get_current_module_path(self, 32768, &length) == CML_STATUS_OK);
    uint64_t request = 0;
    REQUIRE(cml_enable_module(agent, self, CML_MODULE_ALL_IMPORTS, &request) == CML_STATUS_OK);
    cml_reply reply;
    REQUIRE(cml_wait_request(agent, request, 10000, &reply) == CML_STATUS_OK);
    CHECK(reply.status == 0);
    CHECK(reply.message_length == strlen(reply.message));
    CHECK(std::string(reply.message).find("enabled") != std::string::npos);
    CHECK(cml_wait_request(agent, request, 0, &reply) == CML_STATUS_UNKNOWN_REQUEST);  // consumed

    REQUIRE(cml_disable_module(agent, self, &request) == CML_STATUS_OK);
    REQUIRE(cml_wait_request(agent, request, 10000, &reply) == CML_STATUS_OK);
    CHECK(reply.status == 0);
    cml_sync_point(agent);
    cml_destroy_agent(agent);
}

TEST_CASE("enable, disable and module change reports against a broker") {
    const std::string pipe = unique_pipe_name("e2e");
    ScopedEnv pipe_env(L"CML_PIPE_NAME", widen(pipe));
    TestBroker broker(pipe);
    REQUIRE_MESSAGE(broker.ready(), broker.error());

    cml::Agent agent = cml::CreateDefaultAgent(exe_directory().c_str());
    REQUIRE(agent.IsValid());
    REQUIRE_MESSAGE(wait_until([&] { return agent.IsConnected(); }, 15000), "the agent never connected");

    auto ask = [&](const cml::Request& request, cml::Reply* reply) {
        REQUIRE(request.status == CML_STATUS_OK);
        REQUIRE(agent.WaitForRequest(request, 15000, reply) == CML_STATUS_OK);
    };
    cml::Reply reply;

    SUBCASE("the host module can be enabled by full path, again, and by file name") {
        const std::wstring self = cml::GetCurrentModulePath();
        ask(agent.EnableModule(self), &reply);
        CHECK(reply.status == 0);
        CHECK(reply.message.find("module enabled") != std::string::npos);
        ask(agent.EnableModule(self), &reply);
        CHECK(reply.status == 0);
        CHECK(reply.message.find("already enabled") != std::string::npos);
        ask(agent.EnableModule(file_name_of(self), cml::ModuleOptions::None), &reply);
        CHECK(reply.status == 0);
    }
    SUBCASE("a module that is not loaded is refused with CML5212") {
        ask(agent.EnableModule(L"definitely_not_loaded_anywhere.dll"), &reply);
        CHECK(reply.status == 5212);
        CHECK(reply.message.find("definitely_not_loaded_anywhere.dll") != std::string::npos);
    }
    SUBCASE("disable works once, then CML5213") {
        const std::wstring self = cml::GetCurrentModulePath();
        ask(agent.DisableModule(self), &reply);
        CHECK(reply.status == 5213);  // never enabled
        ask(agent.EnableModule(self), &reply);
        REQUIRE(reply.status == 0);
        ask(agent.DisableModule(self), &reply);
        CHECK(reply.status == 0);
        ask(agent.DisableModule(self), &reply);
        CHECK(reply.status == 5213);
    }
    SUBCASE("modules loaded and unloaded after connecting are reported") {
        const std::wstring dummy = exe_directory() + L"\\cml_test_dummy_module.dll";
        ask(agent.EnableModule(dummy), &reply);
        CHECK(reply.status == 5212);  // not loaded yet

        HMODULE module = LoadLibraryW(dummy.c_str());
        REQUIRE(module != nullptr);
        // The agent refreshes the module list before sending a request, so this is deterministic.
        ask(agent.EnableModule(dummy), &reply);
        CHECK(reply.status == 0);

        REQUIRE(FreeLibrary(module));
        ask(agent.EnableModule(dummy), &reply);
        CHECK(reply.status == 5212);  // ModuleUnloaded reached the broker first
    }

    cml::DestroyAgent(agent);
}

TEST_CASE("a restarted broker is found again") {
    const std::string pipe = unique_pipe_name("restart");
    ScopedEnv pipe_env(L"CML_PIPE_NAME", widen(pipe));
    ScopedEnv broker_env(L"CML_BROKER_PATH", exe_directory() + L"\\no_such_broker.exe");

    cml::Agent agent = cml::CreateDefaultAgent(exe_directory().c_str());
    REQUIRE(agent.IsValid());
    // The agent starts before any broker exists and must keep retrying in the background.
    cml::Reply reply;
    {
        TestBroker first(pipe);
        REQUIRE_MESSAGE(first.ready(), first.error());
        REQUIRE_MESSAGE(wait_until([&] { return agent.IsConnected(); }, 20000), "no connection to the first broker");
    }
    // The first broker is gone; the agent notices and fails nothing it did not send.
    REQUIRE(wait_until([&] { return !agent.IsConnected(); }, 20000));
    TestBroker second(pipe);
    REQUIRE_MESSAGE(second.ready(), second.error());
    REQUIRE_MESSAGE(wait_until([&] { return agent.IsConnected(); }, 20000), "no reconnection to the second broker");
    const cml::Request enable = agent.EnableModule(cml::GetCurrentModulePath());
    REQUIRE(enable.status == CML_STATUS_OK);
    REQUIRE(agent.WaitForRequest(enable, 15000, &reply) == CML_STATUS_OK);
    CHECK(reply.status == 0);
    cml::DestroyAgent(agent);
}

namespace {

struct Flag {
    std::atomic<int> runs{0};
    std::atomic<DWORD> thread_id{0};
};

void mark(void* context) {
    auto* flag = static_cast<Flag*>(context);
    flag->thread_id.store(GetCurrentThreadId());
    flag->runs.fetch_add(1);
}

using PostWorkFn = cml_status (*)(void*, void (*)(void*), void*);

}  // namespace

TEST_CASE("synchronized mode applies queued work only inside SyncPoint, on the calling thread") {
    NoBrokerEnv env("sync");
    cml::Agent agent = cml::CreateSynchronizedAgent(exe_directory().c_str());
    REQUIRE(agent.IsValid());
    const PostWorkFn post = agent_export<PostWorkFn>("cml_agent_testing_post_work");
    REQUIRE(post != nullptr);

    Flag flag;
    agent.SyncPoint();  // empty queue: nothing happens
    CHECK(flag.runs.load() == 0);

    std::atomic<cml_status> post_status{-1};  // assertions stay on the main thread
    std::thread producer([&] { post_status.store(post(agent.NativeHandle()->impl, &mark, &flag)); });
    producer.join();
    CHECK(post_status.load() == CML_STATUS_OK);
    CHECK(flag.runs.load() == 0);  // posted, but the application thread has not reached its sync point
    agent.SyncPoint();
    CHECK(flag.runs.load() == 1);
    CHECK(flag.thread_id.load() == GetCurrentThreadId());
    agent.SyncPoint();
    CHECK(flag.runs.load() == 1);  // exactly once

    CHECK(post(agent.NativeHandle()->impl, nullptr, nullptr) == CML_STATUS_INVALID_ARGUMENT);
    cml::DestroyAgent(agent);
}

TEST_CASE("immediate mode: SyncPoint is a no-op and work is applied at once") {
    NoBrokerEnv env("immediate");
    cml::Agent agent = cml::CreateDefaultAgent(exe_directory().c_str());
    REQUIRE(agent.IsValid());
    const PostWorkFn post = agent_export<PostWorkFn>("cml_agent_testing_post_work");
    REQUIRE(post != nullptr);

    Flag flag;
    CHECK(post(agent.NativeHandle()->impl, &mark, &flag) == CML_STATUS_OK);
    CHECK(flag.runs.load() == 1);  // no SyncPoint needed
    agent.SyncPoint();
    CHECK(flag.runs.load() == 1);
    cml::DestroyAgent(agent);
}

TEST_CASE("creating and destroying agents leaves the private heap balanced") {
    NoBrokerEnv env("heap");
    using LiveBlocksFn = uint64_t (*)(void);
    {
        // Warm-up: first use creates process-lifetime state (log state, heap).
        cml::Agent warmup = cml::CreateDefaultAgent(exe_directory().c_str());
        REQUIRE(warmup.IsValid());
        cml::DestroyAgent(warmup);
    }
    const LiveBlocksFn live = agent_export<LiveBlocksFn>("cml_agent_testing_live_heap_blocks");
    REQUIRE(live != nullptr);
    const uint64_t baseline = live();

    for (int round = 0; round < 3; ++round) {
        cml::Agent agent = cml::CreateSynchronizedAgent(exe_directory().c_str());
        REQUIRE(agent.IsValid());
        for (int i = 0; i < 20; ++i) {
            agent.EnableModule(L"queued_but_never_sent.dll");
        }
        CHECK(live() > baseline);  // the agent really allocates from the private heap
        cml::DestroyAgent(agent);
    }
    CHECK(live() == baseline);
}
