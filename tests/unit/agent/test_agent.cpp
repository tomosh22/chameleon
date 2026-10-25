// End-to-end tests of the agent through the public header, exactly as an application uses it:
// Chameleon_API.h LoadLibrary()s cml_agent.dll from the test exe's directory. The broker is
// cml_core's ConnectionServer running on a thread of this process, on a pipe name that is unique
// per test, so parallel CI jobs and reruns never collide. Every wait is bounded; nothing sleeps
// for a fixed time to "let things happen".
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "broker_launch.h"

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
        const cml::Request first_enable = agent.EnableModule(cml::GetCurrentModulePath());
        REQUIRE(first_enable.status == CML_STATUS_OK);
        REQUIRE(agent.WaitForRequest(first_enable, 15000, &reply) == CML_STATUS_OK);
        REQUIRE(reply.status == 0);
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
    CHECK(reply.message.find("already enabled") != std::string::npos);  // re-enabled on reconnect
    // The module the application had enabled is enabled in the new session too, so disabling it
    // works (it would be CML5213 if the agent had forgotten it).
    const cml::Request disable = agent.DisableModule(cml::GetCurrentModulePath());
    REQUIRE(disable.status == CML_STATUS_OK);
    REQUIRE(agent.WaitForRequest(disable, 15000, &reply) == CML_STATUS_OK);
    CHECK(reply.status == 0);
    cml::DestroyAgent(agent);
}

TEST_CASE("hundreds of requests queued before the connection are all answered") {
    const std::string pipe = unique_pipe_name("flood");
    ScopedEnv pipe_env(L"CML_PIPE_NAME", widen(pipe));
    ScopedEnv broker_env(L"CML_BROKER_PATH", exe_directory() + L"\\no_such_broker.exe");
    cml::Agent agent = cml::CreateDefaultAgent(exe_directory().c_str());
    REQUIRE(agent.IsValid());

    constexpr int kRequests = 400;  // far more than the pipe buffers could hold of replies unread
    const std::wstring self = cml::GetCurrentModulePath();
    std::vector<cml::Request> requests;
    for (int i = 0; i < kRequests; ++i) {
        requests.push_back(i % 2 == 0 ? agent.EnableModule(self) : agent.DisableModule(self));
        REQUIRE(requests.back().status == CML_STATUS_OK);
    }
    TestBroker broker(pipe);
    REQUIRE_MESSAGE(broker.ready(), broker.error());
    REQUIRE_MESSAGE(wait_until([&] { return agent.IsConnected(); }, 20000), "the agent never connected");
    int failed = 0;
    for (const cml::Request& request : requests) {
        cml::Reply reply;
        REQUIRE(agent.WaitForRequest(request, 30000, &reply) == CML_STATUS_OK);
        failed += reply.status == 0 ? 0 : 1;
    }
    CHECK(failed == 0);  // enable, disable, enable, ...: every one is valid in sequence
    cml::DestroyAgent(agent);
}

TEST_CASE("a failing create can be retried any number of times without growth") {
    // CML5208: the pipe name is not in the pipe namespace, so the agent cannot even start.
    ScopedEnv pipe_env(L"CML_PIPE_NAME", L"not_a_pipe_name");
    using LiveBlocksFn = uint64_t (*)(void);
    DWORD handles_after_first = 0;
    uint64_t blocks_after_first = 0;
    for (int attempt = 0; attempt < 6; ++attempt) {
        cml_status status = CML_STATUS_OK;
        cml::Agent agent = cml::CreateDefaultAgent(exe_directory().c_str(), &status);
        CHECK_FALSE(agent.IsValid());
        CHECK(status == 5208);
        // The DLL is deliberately still loaded after a failed create (see cml_create_agent()).
        const LiveBlocksFn live = agent_export<LiveBlocksFn>("cml_agent_testing_live_heap_blocks");
        DWORD handles = 0;
        REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &handles));
        if (attempt == 0) {
            handles_after_first = handles;
            blocks_after_first = live();
        } else {
            CHECK(handles <= handles_after_first);
            CHECK(live() == blocks_after_first);
        }
    }
}

TEST_CASE("an auto-started broker runs from its own directory and does not live forever") {
    const std::wstring broker = L"C:\\tools\\chameleon\\cml_broker.exe";
    const std::wstring plain = cml::agent::broker_command_line(broker, "\\\\.\\pipe\\x", "");
    CHECK(plain.find(L"\"C:\\tools\\chameleon\\cml_broker.exe\" --headless --pipe") == 0);
    CHECK(plain.find(L"--exit-when-idle 300000") != std::wstring::npos);
    CHECK(plain.find(L"--startup-timeout 60000") != std::wstring::npos);

    // A caller's own limits win; nothing is added twice.
    const std::wstring custom = cml::agent::broker_command_line(
        broker, "\\\\.\\pipe\\x", "--exit-when-idle 2000 --startup-timeout 9000 --log \"a b.log\"");
    CHECK(custom.find(L"--exit-when-idle 2000") != std::wstring::npos);
    CHECK(custom.find(L"--exit-when-idle", custom.find(L"--exit-when-idle") + 1) == std::wstring::npos);
    CHECK(custom.find(L"300000") == std::wstring::npos);
    CHECK(custom.find(L"--startup-timeout", custom.find(L"--startup-timeout") + 1) == std::wstring::npos);
}

namespace {

// Names of the DLLs a PE file imports (normal and delay-load), lower-cased.
std::vector<std::string> imported_dlls(const std::wstring& path) {
    std::vector<std::string> names;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    std::vector<unsigned char> image(GetFileSize(file, nullptr));
    DWORD read = 0;
    REQUIRE(ReadFile(file, image.data(), static_cast<DWORD>(image.size()), &read, nullptr));
    CloseHandle(file);

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    REQUIRE(dos->e_magic == IMAGE_DOS_SIGNATURE);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
    REQUIRE(nt->Signature == IMAGE_NT_SIGNATURE);
    REQUIRE(nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    const auto to_offset = [&](DWORD rva) -> std::size_t {
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const IMAGE_SECTION_HEADER& s = sections[i];
            if (rva >= s.VirtualAddress && rva < s.VirtualAddress + std::max(s.Misc.VirtualSize, s.SizeOfRawData)) {
                return s.PointerToRawData + (rva - s.VirtualAddress);
            }
        }
        return 0;
    };
    const auto add_name = [&](DWORD name_rva) {
        const std::size_t offset = to_offset(name_rva);
        if (offset == 0) {
            return;
        }
        std::string name(reinterpret_cast<const char*>(image.data() + offset));
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        names.push_back(name);
    };

    const IMAGE_DATA_DIRECTORY imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress != 0) {
        const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image.data() + to_offset(imports.VirtualAddress));
        for (; descriptor->Name != 0; ++descriptor) {
            add_name(descriptor->Name);
        }
    }
    const IMAGE_DATA_DIRECTORY delayed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayed.VirtualAddress != 0) {
        const auto* descriptor = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(image.data() + to_offset(delayed.VirtualAddress));
        for (; descriptor->DllNameRVA != 0; ++descriptor) {
            add_name(descriptor->DllNameRVA);
        }
    }
    return names;
}

}  // namespace

TEST_CASE("cml_agent.dll imports only the system DLLs it strictly needs") {
    // The agent is injected into arbitrary hosts: every extra import is a dependency of the host.
    // Allowed: kernel32 (everything), advapi32 (tokens, SIDs, security descriptors), ntdll, and the
    // api-ms-win-core-* API-set forwarders of kernel32 that the static runtime may use.
    const std::vector<std::string> imports = imported_dlls(exe_directory() + L"\\cml_agent.dll");
    std::string listing;
    for (const std::string& name : imports) {
        listing += name + " ";
    }
    INFO("cml_agent.dll imports: " << listing);
    CHECK_FALSE(imports.empty());
    for (const std::string& name : imports) {
        const bool allowed = name == "kernel32.dll" || name == "advapi32.dll" || name == "ntdll.dll" ||
                             name.rfind("api-ms-win-core-", 0) == 0;
        CHECK_MESSAGE(allowed, "unexpected import: " << name);
    }
    CHECK(std::find(imports.begin(), imports.end(), "shell32.dll") == imports.end());
    CHECK(std::find(imports.begin(), imports.end(), "user32.dll") == imports.end());
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
