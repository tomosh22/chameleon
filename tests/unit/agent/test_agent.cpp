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
#include <cstddef>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <variant>
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

// Names of the DLLs a PE file imports (normal and delay-load), lower-cased. Every RVA, structure
// and name is bounds-checked against the file image; anything that cannot be mapped or read fails
// the test instead of being read from some default place.
std::vector<std::string> imported_dlls(const std::wstring& path) {
    std::vector<std::string> names;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    const DWORD file_size = GetFileSize(file, nullptr);
    REQUIRE(file_size != INVALID_FILE_SIZE);
    std::vector<unsigned char> image(file_size);
    DWORD read = 0;
    REQUIRE(ReadFile(file, image.data(), file_size, &read, nullptr));
    REQUIRE(read == file_size);
    CloseHandle(file);

    // A pointer to `length` bytes at `offset` in the file, or a test failure.
    const auto at = [&](std::size_t offset, std::size_t length) -> const unsigned char* {
        REQUIRE_MESSAGE((offset <= image.size() && length <= image.size() - offset),
                        "read of " << length << " bytes at offset " << offset << " is outside the file (" << image.size() << ")");
        return image.data() + offset;
    };

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(at(0, sizeof(IMAGE_DOS_HEADER)));
    REQUIRE(dos->e_magic == IMAGE_DOS_SIGNATURE);
    REQUIRE(dos->e_lfanew > 0);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        at(static_cast<std::size_t>(dos->e_lfanew), sizeof(IMAGE_NT_HEADERS64)));
    REQUIRE(nt->Signature == IMAGE_NT_SIGNATURE);
    REQUIRE_MESSAGE(nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC, "not a PE32+ image (x86 is out of scope)");
    const std::size_t sections_offset = static_cast<std::size_t>(dos->e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                                        nt->FileHeader.SizeOfOptionalHeader;
    const std::size_t section_count = nt->FileHeader.NumberOfSections;
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        at(sections_offset, section_count * sizeof(IMAGE_SECTION_HEADER)));

    // Maps an RVA to a file offset through the section table; fails the test if no section holds it.
    const auto to_offset = [&](std::size_t rva) -> std::size_t {
        for (std::size_t i = 0; i < section_count; ++i) {
            const IMAGE_SECTION_HEADER& section = sections[i];
            const std::size_t extent = std::min<std::size_t>(section.Misc.VirtualSize, section.SizeOfRawData);
            if (rva >= section.VirtualAddress && rva - section.VirtualAddress < extent) {
                return static_cast<std::size_t>(section.PointerToRawData) + (rva - section.VirtualAddress);
            }
        }
        FAIL("RVA " << rva << " is not inside any section");
        return 0;
    };
    // A NUL-terminated name at `rva`, not running past the end of the file.
    const auto add_name = [&](std::size_t name_rva) {
        const std::size_t offset = to_offset(name_rva);
        const unsigned char* start = at(offset, 1);
        const std::size_t room = image.size() - offset;
        const void* nul = std::memchr(start, 0, room);
        REQUIRE_MESSAGE(nul != nullptr, "an import name is not terminated inside the file");
        std::string name(reinterpret_cast<const char*>(start), static_cast<const unsigned char*>(nul) - start);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        names.push_back(name);
    };

    constexpr std::size_t kMaxDescriptors = 1024;  // a corrupt table must not loop for ever
    const IMAGE_DATA_DIRECTORY imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress != 0) {
        const std::size_t table = to_offset(imports.VirtualAddress);
        bool terminated = false;
        for (std::size_t i = 0; i < kMaxDescriptors && !terminated; ++i) {
            const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
                at(table + i * sizeof(IMAGE_IMPORT_DESCRIPTOR), sizeof(IMAGE_IMPORT_DESCRIPTOR)));
            if (descriptor->Name == 0) {
                terminated = true;
            } else {
                add_name(descriptor->Name);
            }
        }
        REQUIRE_MESSAGE(terminated, "the import table has no terminating entry");
    }
    const IMAGE_DATA_DIRECTORY delayed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayed.VirtualAddress != 0) {
        const std::size_t table = to_offset(delayed.VirtualAddress);
        bool terminated = false;
        for (std::size_t i = 0; i < kMaxDescriptors && !terminated; ++i) {
            const auto* descriptor = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(
                at(table + i * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR), sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)));
            if (descriptor->DllNameRVA == 0) {
                terminated = true;
            } else {
                add_name(descriptor->DllNameRVA);
            }
        }
        REQUIRE_MESSAGE(terminated, "the delay-import table has no terminating entry");
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

// ---- a scripted broker on raw pipe handles, for protocol-level behaviour ------------------------

namespace {

// A broker that speaks the protocol by hand over a blocking pipe, records everything it receives
// (per connection) and can be told to misbehave: answer with a corrupt frame, or hang up.
class ScriptedBroker {
public:
    enum class Action { Answer, CorruptAnswer, Reject, Hangup };
    using Hook = std::function<Action(const cml::ipc::Message&)>;

    ScriptedBroker(std::string pipe, Hook hook = nullptr) : pipe_(std::move(pipe)), hook_(std::move(hook)) {
        thread_ = std::thread([this] { run(); });
        ready_ = wait_until([this] { return cml::ipc::pipe_exists(pipe_); }, 10000);
    }
    ~ScriptedBroker() {
        stopping_.store(true);
        // Unblock a pending ConnectNamedPipe with a throw-away client.
        const std::wstring wide = widen(pipe_);
        HANDLE poke = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (poke != INVALID_HANDLE_VALUE) {
            CloseHandle(poke);
        }
        SetEvent(hangup_event_);
        thread_.join();
        CloseHandle(hangup_event_);
    }
    bool ready() const { return ready_; }

    // Closes the current connection (the agent will reconnect).
    void hang_up() { SetEvent(hangup_event_); }

    std::size_t connections() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sessions_.size();
    }
    // Messages received on connection `index` (0 = first), a copy.
    std::vector<cml::ipc::Message> messages(std::size_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return index < sessions_.size() ? sessions_[index] : std::vector<cml::ipc::Message>();
    }
    std::size_t count(std::size_t index, bool (*predicate)(const cml::ipc::Message&)) const {
        std::size_t n = 0;
        for (const cml::ipc::Message& m : messages(index)) {
            n += predicate(m) ? 1 : 0;
        }
        return n;
    }

private:
    // Reads exactly `size` bytes by polling (PeekNamedPipe), so the wait can be abandoned when the
    // test hangs up or stops the broker. False on end of stream, error or abandonment.
    bool read_exactly(HANDLE pipe, void* buffer, DWORD size) {
        auto* bytes = static_cast<unsigned char*>(buffer);
        DWORD done = 0;
        while (done < size) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                return false;  // the client closed its end
            }
            if (available == 0) {
                if (stopping_.load() || WaitForSingleObject(hangup_event_, 0) == WAIT_OBJECT_0) {
                    return false;
                }
                Sleep(1);
                continue;
            }
            DWORD n = 0;
            if (!ReadFile(pipe, bytes + done, std::min<DWORD>(available, size - done), &n, nullptr) || n == 0) {
                return false;
            }
            done += n;
        }
        return true;
    }
    static bool write_all(HANDLE pipe, const std::vector<std::uint8_t>& bytes) {
        DWORD written = 0;
        return WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    }
    static bool send(HANDLE pipe, const cml::ipc::Message& message) {
        cml::Result<std::vector<std::uint8_t>> frame = cml::ipc::encode_frame(message);
        return frame.has_value() && write_all(pipe, *frame);
    }

    void run() {
        const std::wstring wide = widen(pipe_);
        while (!stopping_.load()) {
            HANDLE pipe = CreateNamedPipeW(wide.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                           PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) {
                return;
            }
            if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
                if (!stopping_.load()) {
                    serve(pipe);
                }
            }
            CloseHandle(pipe);
        }
    }

    void serve(HANDLE pipe) {
        std::size_t index = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sessions_.emplace_back();
            index = sessions_.size() - 1;
        }
        for (;;) {
            std::vector<std::uint8_t> header(cml::ipc::kFrameHeaderSize);
            if (!read_exactly(pipe, header.data(), static_cast<DWORD>(header.size()))) {
                break;
            }
            cml::Result<cml::ipc::FrameHeader> parsed = cml::ipc::parse_frame_header(header.data(), header.size());
            if (!parsed) {
                break;
            }
            std::vector<std::uint8_t> payload(parsed->payload_size);
            if (!payload.empty() && !read_exactly(pipe, payload.data(), static_cast<DWORD>(payload.size()))) {
                break;
            }
            cml::Result<cml::ipc::Message> message =
                cml::ipc::decode_message(parsed->type, parsed->version, payload.data(), payload.size());
            if (!message) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                sessions_[index].push_back(*message);
            }
            if (!answer(pipe, *message)) {
                break;
            }
        }
        DisconnectNamedPipe(pipe);  // run() closes the handle
        ResetEvent(hangup_event_);  // consumed: the next session starts clean
    }

    bool answer(HANDLE pipe, const cml::ipc::Message& message) {
        namespace ipc = cml::ipc;
        if (const auto* hello = std::get_if<ipc::Hello>(&message)) {
            ipc::Welcome welcome;
            welcome.request_id = hello->request_id;
            welcome.agent_id = 1;
            welcome.broker_version = "scripted";
            return send(pipe, welcome);
        }
        std::uint64_t id = 0;
        if (const auto* list = std::get_if<ipc::ModuleList>(&message)) {
            id = list->request_id;
        } else if (const auto* enable = std::get_if<ipc::EnableModule>(&message)) {
            id = enable->request_id;
        } else if (const auto* disable = std::get_if<ipc::DisableModule>(&message)) {
            id = disable->request_id;
        } else {
            return true;  // events: no answer
        }
        const Action action = hook_ ? hook_(message) : Action::Answer;
        if (action == Action::Hangup) {
            return false;
        }
        if (action == Action::CorruptAnswer) {
            // A Reply with the right id whose text is not UTF-8.
            std::vector<std::uint8_t> frame = {18, 0, 0, 0, 0x03, 0x00, 0x01, 0x00};
            for (int i = 0; i < 8; ++i) {
                frame.push_back(static_cast<std::uint8_t>(id >> (8 * i)));
            }
            const std::uint8_t tail[] = {0, 0, 0, 0, 2, 0, 0, 0, 0xFF, 0xFE};
            frame.insert(frame.end(), tail, tail + sizeof(tail));
            return write_all(pipe, frame);
        }
        ipc::Reply reply;
        reply.request_id = id;
        if (action == Action::Reject) {
            reply.status = 5201;
            reply.message = "scripted rejection";
        } else {
            reply.message = "scripted ok";
        }
        return send(pipe, reply);
    }

    std::string pipe_;
    Hook hook_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    bool ready_ = false;
    HANDLE hangup_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual-reset: reset per session
    mutable std::mutex mutex_;
    std::vector<std::vector<cml::ipc::Message>> sessions_;
};

bool is_enable(const cml::ipc::Message& m) { return std::holds_alternative<cml::ipc::EnableModule>(m); }

}  // namespace

TEST_CASE("an answer the agent cannot decode drops the connection instead of hanging the request") {
    const std::string pipe = unique_pipe_name("corrupt");
    ScopedEnv pipe_env(L"CML_PIPE_NAME", widen(pipe));
    ScopedEnv broker_env(L"CML_BROKER_PATH", exe_directory() + L"\\no_such_broker.exe");
    std::atomic<int> enables{0};
    ScriptedBroker broker(pipe, [&](const cml::ipc::Message& m) {
        return is_enable(m) && enables.fetch_add(1) == 0 ? ScriptedBroker::Action::CorruptAnswer
                                                         : ScriptedBroker::Action::Answer;
    });
    REQUIRE(broker.ready());
    cml::Agent agent = cml::CreateDefaultAgent(exe_directory().c_str());
    REQUIRE(agent.IsValid());
    REQUIRE(wait_until([&] { return agent.IsConnected(); }, 20000));

    const cml::Request first = agent.EnableModule(L"corrupt_target.dll");
    REQUIRE(first.status == CML_STATUS_OK);
    cml::Reply reply;
    REQUIRE(agent.WaitForRequest(first, 15000, &reply) == CML_STATUS_OK);
    CHECK(reply.status == 5216);  // connection lost before a usable answer
    // The agent reconnects and works again.
    REQUIRE(wait_until([&] { return broker.connections() >= 2; }, 20000));
    REQUIRE(wait_until([&] { return agent.IsConnected(); }, 20000));
    const cml::Request second = agent.EnableModule(L"corrupt_target.dll");
    REQUIRE(agent.WaitForRequest(second, 15000, &reply) == CML_STATUS_OK);
    CHECK(reply.status == 0);
    cml::DestroyAgent(agent);
}

TEST_CASE("repeated reconnects re-enable each module exactly once and keep accepting requests") {
    const std::string pipe = unique_pipe_name("reconnects");
    ScopedEnv pipe_env(L"CML_PIPE_NAME", widen(pipe));
    ScopedEnv broker_env(L"CML_BROKER_PATH", exe_directory() + L"\\no_such_broker.exe");
    ScriptedBroker broker(pipe);
    REQUIRE(broker.ready());
    cml::Agent agent = cml::CreateDefaultAgent(exe_directory().c_str());
    REQUIRE(agent.IsValid());
    REQUIRE(wait_until([&] { return agent.IsConnected(); }, 20000));

    constexpr int kModules = 40;  // more than the 16-request in-flight window
    cml::Reply reply;
    for (int i = 0; i < kModules; ++i) {
        const cml::Request r = agent.EnableModule(L"module_" + std::to_wstring(i) + L".dll");
        REQUIRE(agent.WaitForRequest(r, 15000, &reply) == CML_STATUS_OK);
        REQUIRE(reply.status == 0);
    }
    // Option changes of an already enabled module are remembered: first no options, then all imports.
    REQUIRE(agent.WaitForRequest(agent.EnableModule(L"module_0.dll", cml::ModuleOptions::None), 15000, &reply) == CML_STATUS_OK);
    REQUIRE(agent.WaitForRequest(agent.EnableModule(L"module_0.dll", cml::ModuleOptions::AllImports), 15000, &reply) == CML_STATUS_OK);

    // Reconnect twice in quick succession: the second hang-up comes as soon as the second session is
    // registered, while the first re-enables are still on their way.
    broker.hang_up();
    REQUIRE(wait_until([&] { return broker.connections() >= 2 && broker.count(1, [](const cml::ipc::Message& m) { return std::holds_alternative<cml::ipc::ModuleList>(m); }) == 1; }, 20000));
    broker.hang_up();
    REQUIRE(wait_until([&] { return broker.connections() >= 3 && agent.IsConnected(); }, 30000));

    // The app can still make requests, and they are answered.
    const cml::Request extra = agent.EnableModule(L"after_reconnects.dll");
    REQUIRE(extra.status == CML_STATUS_OK);
    REQUIRE(agent.WaitForRequest(extra, 20000, &reply) == CML_STATUS_OK);
    CHECK(reply.status == 0);

    // On the last connection every module was enabled once (plus the app's new request), none twice.
    const std::vector<cml::ipc::Message> last = broker.messages(2);
    std::map<std::string, int> seen;
    std::uint32_t options_of_module_0 = 99;
    for (const cml::ipc::Message& m : last) {
        if (const auto* enable = std::get_if<cml::ipc::EnableModule>(&m)) {
            ++seen[enable->path];
            if (enable->path == "module_0.dll") {
                options_of_module_0 = enable->options;
            }
        }
    }
    CHECK(seen.size() == static_cast<std::size_t>(kModules) + 1);
    for (const auto& [path, count] : seen) {
        CHECK_MESSAGE(count == 1, path << " was enabled " << count << " times");
    }
    CHECK(options_of_module_0 == 1);  // the later, AllImports enable won
    cml::DestroyAgent(agent);
}

TEST_CASE("fire-and-forget and uncollected requests never lock new requests out") {
    const std::string pipe = unique_pipe_name("forget");
    ScopedEnv pipe_env(L"CML_PIPE_NAME", widen(pipe));
    ScopedEnv broker_env(L"CML_BROKER_PATH", exe_directory() + L"\\no_such_broker.exe");
    ScriptedBroker broker(pipe);
    REQUIRE(broker.ready());
    cml::Agent agent = cml::CreateDefaultAgent(exe_directory().c_str());
    REQUIRE(agent.IsValid());
    REQUIRE(wait_until([&] { return agent.IsConnected(); }, 20000));

    const auto push = [&](bool keep_id, int n) {
        for (int i = 0; i < n; ++i) {
            for (;;) {  // a full queue is back-pressure, not failure: wait for room
                cml_status status;
                if (keep_id) {
                    uint64_t id = 0;
                    status = cml_enable_module(agent.NativeHandle(), L"forget.dll", CML_MODULE_NONE, &id);
                } else {
                    status = cml_enable_module(agent.NativeHandle(), L"forget.dll", CML_MODULE_NONE, nullptr);
                }
                if (status == CML_STATUS_OK) {
                    break;
                }
                REQUIRE(status == 5217);
                Sleep(2);
            }
        }
    };
    push(false, 5000);  // no request id: nothing is kept
    push(true, 1500);   // ids nobody ever asks for: kept only up to a bounded number
    cml::Reply reply;
    // Queued requests are real work and may still fill the table right after the burst: that is
    // back-pressure (CML5217), so wait for room. What must not happen is room never coming back.
    cml::Request waited = agent.EnableModule(L"waited.dll");
    for (int attempt = 0; waited.status == 5217 && attempt < 15000; ++attempt) {
        Sleep(2);
        waited = agent.EnableModule(L"waited.dll");
    }
    REQUIRE(waited.status == CML_STATUS_OK);
    REQUIRE(agent.WaitForRequest(waited, 30000, &reply) == CML_STATUS_OK);
    CHECK(reply.status == 0);
    cml::DestroyAgent(agent);
}

TEST_CASE("a module loaded just before an EnableModule is known to the broker, even in a busy pass") {
    const std::string pipe = unique_pipe_name("busy");
    ScopedEnv pipe_env(L"CML_PIPE_NAME", widen(pipe));
    TestBroker broker(pipe);
    REQUIRE_MESSAGE(broker.ready(), broker.error());
    cml::Agent agent = cml::CreateDefaultAgent(exe_directory().c_str());
    REQUIRE(agent.IsValid());
    REQUIRE(wait_until([&] { return agent.IsConnected(); }, 20000));

    std::atomic<bool> stop{false};
    std::thread noise([&] {  // keeps the agent's flush passes busy with unrelated requests
        const std::wstring self = cml::GetCurrentModulePath();
        while (!stop.load()) {
            cml_enable_module(agent.NativeHandle(), self.c_str(), CML_MODULE_NONE, nullptr);
            Sleep(1);
        }
    });
    const std::wstring dummy = exe_directory() + L"\\cml_test_dummy_module.dll";
    int failures = 0;
    for (int i = 0; i < 15; ++i) {
        HMODULE module = LoadLibraryW(dummy.c_str());
        REQUIRE(module != nullptr);
        cml::Reply reply;
        const cml::Request r = agent.EnableModule(dummy);
        REQUIRE(r.status == CML_STATUS_OK);
        REQUIRE(agent.WaitForRequest(r, 20000, &reply) == CML_STATUS_OK);
        failures += reply.status == 0 ? 0 : 1;
        FreeLibrary(module);
    }
    stop.store(true);
    noise.join();
    CHECK(failures == 0);
    cml::DestroyAgent(agent);
}

TEST_CASE("the broker's working directory") {
    using cml::agent::broker_working_directory;
    CHECK(broker_working_directory(L"C:\\tools\\cml_broker.exe") == L"C:\\tools");
    CHECK(broker_working_directory(L"C:\\cml_broker.exe") == L"C:\\");  // a drive root keeps its backslash
    CHECK(broker_working_directory(L"\\\\server\\share\\cml_broker.exe") == L"\\\\server\\share");
    CHECK(broker_working_directory(L"cml_broker.exe").empty());           // no directory: inherit
    const std::wstring too_long = L"C:\\" + std::wstring(300, L'd') + L"\\cml_broker.exe";
    CHECK(broker_working_directory(too_long).empty());                    // CreateProcess could not take it
}

TEST_CASE("CML_BROKER_ARGS is split with the CommandLineToArgvW rules") {
    using cml::agent::split_command_line;
    using Args = std::vector<std::wstring>;
    CHECK(split_command_line(L"") == Args{});
    CHECK(split_command_line(L"   \t ") == Args{});
    CHECK(split_command_line(L"--log run.log") == Args{L"--log", L"run.log"});
    CHECK(split_command_line(L"a \"b c\"   d") == Args{L"a", L"b c", L"d"});
    CHECK(split_command_line(L"\"C:\\x y\\\\\" z") == Args{L"C:\\x y\\", L"z"});  // 2 backslashes + quote
    CHECK(split_command_line(L"x\\\"y") == Args{L"x\"y"});                        // backslash-quote is a quote
    CHECK(split_command_line(L"\\\\srv\\share\\f") == Args{L"\\\\srv\\share\\f"});  // not before a quote: literal
    CHECK(split_command_line(L"\"a\"\"b\"") == Args{L"a\"b"});                     // "" inside quotes
    CHECK(split_command_line(L"\"unterminated tail") == Args{L"unterminated tail"});
}

TEST_CASE("every --log value is resolved against the host directory") {
    using cml::agent::absolutize_log_arguments;
    const auto fake = [](const std::wstring& path) {
        return path.size() >= 2 && path[1] == L':' ? path : L"C:\\work\\" + path;
    };
    CHECK(absolutize_log_arguments("--log run.log", fake) == "--log C:\\work\\run.log");
    CHECK(absolutize_log_arguments("--exit-when-idle 5 --log \"sub dir\\x.log\" --verbose", fake) ==
          "--exit-when-idle 5 --log \"C:\\work\\sub dir\\x.log\" --verbose");
    CHECK(absolutize_log_arguments("--log a.log --log b.log", fake) == "--log C:\\work\\a.log --log C:\\work\\b.log");
    // Nothing to change: returned exactly as given.
    CHECK(absolutize_log_arguments("--log C:\\abs.log", fake) == "--log C:\\abs.log");
    CHECK(absolutize_log_arguments("--verbose", fake) == "--verbose");
    CHECK(absolutize_log_arguments("--logfile x.log", fake) == "--logfile x.log");  // a different option
    CHECK(absolutize_log_arguments("--log", fake) == "--log");
    CHECK(absolutize_log_arguments("--log run.log", [](const std::wstring&) { return std::wstring(); }) == "--log run.log");
}

TEST_CASE("the real resolver uses the host directory and understands drive-relative paths") {
    using cml::agent::resolve_against_host_directory;
    wchar_t cwd[MAX_PATH];
    REQUIRE(GetCurrentDirectoryW(MAX_PATH, cwd) > 0);
    std::wstring expected = cwd;
    if (expected.back() != L'\\') {
        expected.push_back(L'\\');
    }
    CHECK(resolve_against_host_directory(L"rel.log") == expected + L"rel.log");
    CHECK(resolve_against_host_directory(L"C:\\a\\..\\b.log") == L"C:\\b.log");
    const std::wstring drive_relative = resolve_against_host_directory(L"Q:rel.log");
    CHECK(drive_relative.rfind(L"Q:\\", 0) == 0);  // resolved on drive Q, not glued to the cwd
    CHECK(drive_relative.find(expected) == std::wstring::npos);
}

TEST_CASE("a refused module list keeps the session and the next poll sends the full list again") {
    const std::string pipe = unique_pipe_name("badlist");
    ScopedEnv pipe_env(L"CML_PIPE_NAME", widen(pipe));
    ScopedEnv broker_env(L"CML_BROKER_PATH", exe_directory() + L"\\no_such_broker.exe");
    std::atomic<int> lists{0};
    ScriptedBroker broker(pipe, [&](const cml::ipc::Message& m) {
        return std::holds_alternative<cml::ipc::ModuleList>(m) && lists.fetch_add(1) == 0
                   ? ScriptedBroker::Action::Reject
                   : ScriptedBroker::Action::Answer;
    });
    REQUIRE(broker.ready());
    cml::Agent agent = cml::CreateDefaultAgent(exe_directory().c_str());
    REQUIRE(agent.IsValid());
    REQUIRE(wait_until([&] { return agent.IsConnected(); }, 20000));
    constexpr auto is_list = [](const cml::ipc::Message& m) { return std::holds_alternative<cml::ipc::ModuleList>(m); };
    // The agent sends the whole list again on its own, on the same connection.
    REQUIRE(wait_until([&] { return broker.count(0, is_list) >= 2; }, 20000));
    CHECK(broker.connections() == 1);
    CHECK(agent.IsConnected());
    cml::Reply reply;
    const cml::Request r = agent.EnableModule(L"after_refusal.dll");
    REQUIRE(agent.WaitForRequest(r, 15000, &reply) == CML_STATUS_OK);
    CHECK(reply.status == 0);
    CHECK(broker.connections() == 1);
    cml::DestroyAgent(agent);
}

TEST_CASE("destroying the agent while threads wait on it neither crashes nor leaves a waiter behind") {
    using WaitFn = cml_status (*)(void*, std::uint64_t, std::uint32_t, cml_reply*);
    using EnableFn = cml_status (*)(void*, const wchar_t*, std::uint32_t, std::uint64_t*);
    using WaitersFn = std::uint64_t (*)(void*);
    constexpr int kThreads = 8;
    for (int round = 0; round < 50; ++round) {
        NoBrokerEnv env("destroy_race");
        cml::Agent agent = cml::CreateDefaultAgent(exe_directory().c_str());
        REQUIRE(agent.IsValid());
        const WaitFn wait = agent_export<WaitFn>("cml_agent_wait_request");
        const EnableFn enable = agent_export<EnableFn>("cml_agent_enable_module");
        const WaitersFn waiters = agent_export<WaitersFn>("cml_agent_testing_waiters");
        void* const impl = agent.NativeHandle()->impl;

        std::atomic<int> returned{0};
        std::atomic<int> cancelled{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&, i] {
                std::uint64_t id = 0;
                const std::wstring path = L"destroy_race_" + std::to_wstring(i) + L".dll";
                if (enable(impl, path.c_str(), 0, &id) == CML_STATUS_OK) {
                    cml_reply reply{};
                    if (wait(impl, id, 0xFFFFFFFFu, &reply) == CML_STATUS_OK && reply.status == 5215) {
                        ++cancelled;
                    }
                }
                ++returned;
            });
        }
        REQUIRE(wait_until([&] { return waiters(impl) == static_cast<std::uint64_t>(kThreads); }, 20000));
        std::thread destroyer([&] { agent.NativeHandle()->destroy(impl); });
        destroyer.join();
        for (std::thread& t : threads) {
            t.join();
        }
        CHECK(returned.load() == kThreads);
        CHECK(cancelled.load() == kThreads);
        HeapFree(GetProcessHeap(), 0, agent.NativeHandle());  // the wrapper; the agent object is gone
    }
}

#ifdef CML_TEST_BROKER_EXE
TEST_CASE("when the auto-started broker exits the agent starts a new one") {
    const std::string pipe = unique_pipe_name("relaunch");
    ScopedEnv pipe_env(L"CML_PIPE_NAME", widen(pipe));
    std::wstring broker_exe = L"" CML_TEST_BROKER_EXE;
    std::replace(broker_exe.begin(), broker_exe.end(), L'/', L'\\');
    ScopedEnv broker_env(L"CML_BROKER_PATH", broker_exe);
    ScopedEnv args_env(L"CML_BROKER_ARGS", L"--exit-when-idle 20000");
    const auto broker_pid = [&]() -> DWORD {
        cml::Result<cml::ipc::PipeConnection> probe = cml::ipc::connect_pipe(pipe, 2000);
        return probe ? probe->peer_process_id() : 0;
    };
    const auto kill = [](DWORD pid) {
        HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        REQUIRE(process != nullptr);
        TerminateProcess(process, 1);
        WaitForSingleObject(process, 10000);
        CloseHandle(process);
    };

    cml::Agent agent = cml::CreateDefaultAgent(exe_directory().c_str());
    REQUIRE(agent.IsValid());
    REQUIRE_MESSAGE(wait_until([&] { return agent.IsConnected(); }, 60000), "the agent did not start a broker");
    const DWORD first = broker_pid();
    REQUIRE(first != 0);
    kill(first);
    REQUIRE(wait_until([&] { return !agent.IsConnected(); }, 20000));
    REQUIRE_MESSAGE(wait_until([&] { return agent.IsConnected(); }, 60000), "the agent did not start a second broker");
    const DWORD second = broker_pid();
    REQUIRE(second != 0);
    CHECK(second != first);
    cml::DestroyAgent(agent);
    kill(second);
}
#endif

TEST_CASE("300 enabled modules are all re-enabled after a reconnect") {
    const std::string pipe = unique_pipe_name("many300");
    ScopedEnv pipe_env(L"CML_PIPE_NAME", widen(pipe));
    ScopedEnv broker_env(L"CML_BROKER_PATH", exe_directory() + L"\\no_such_broker.exe");
    ScriptedBroker broker(pipe);
    REQUIRE(broker.ready());
    cml::Agent agent = cml::CreateDefaultAgent(exe_directory().c_str());
    REQUIRE(agent.IsValid());
    REQUIRE(wait_until([&] { return agent.IsConnected(); }, 20000));
    constexpr int kModules = 300;
    cml::Reply reply;
    for (int i = 0; i < kModules; ++i) {
        const cml::Request r = agent.EnableModule(L"bulk_" + std::to_wstring(i) + L".dll");
        REQUIRE(agent.WaitForRequest(r, 15000, &reply) == CML_STATUS_OK);
        REQUIRE(reply.status == 0);
    }
    broker.hang_up();
    REQUIRE(wait_until([&] { return broker.connections() >= 2; }, 20000));
    REQUIRE(wait_until([&] { return broker.count(1, is_enable) >= static_cast<std::size_t>(kModules); }, 60000));
    std::map<std::string, int> seen;
    for (const cml::ipc::Message& m : broker.messages(1)) {
        if (const auto* enable = std::get_if<cml::ipc::EnableModule>(&m)) {
            ++seen[enable->path];
        }
    }
    CHECK(seen.size() == static_cast<std::size_t>(kModules));
    cml::DestroyAgent(agent);
}
