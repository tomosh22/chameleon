// unit.agent_exception (WP-20): the agent's first-chance exception handler (agent/src/crash/exception_gate.*).
//
// Every scenario faults on purpose, so each one runs in a CHILD process: this executable starts itself with
// `--test-case=exc_child` and the scenario name in CML_TEST_EXC_CHILD, and the parent checks the exit code and the result file
// the child writes. In the child, the test thread faults and a "decider" thread plays the agent thread: it takes the held
// thread from the gate, optionally repairs state, and resolves it with Continue, Leave or Ignore.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "crash/exception_gate.h"
#include "doctest.h"
#include "heap.h"

using cml::agent::crash::Action;
using cml::agent::crash::ExceptionGate;
using cml::agent::crash::Held;
using cml::agent::crash::Range;

#if defined(_MSC_VER)
#define CML_TEST_NOINLINE __declspec(noinline)
#else
#define CML_TEST_NOINLINE __attribute__((noinline))
#endif

namespace {

// Assertions made on the decider thread are recorded here and checked by the scenario's thread (doctest assertions belong to
// the thread that runs the test case).
std::atomic<int> g_problems{0};

void expect(bool ok, const char* what) {
    if (!ok) {
        g_problems.fetch_add(1);
        std::fprintf(stderr, "PROBLEM: %s\n", what);
    }
}

// A null pointer the compiler cannot see through.
volatile int* volatile g_null = nullptr;

// ---- code that faults ------------------------------------------------------------------------------------------------

CML_TEST_NOINLINE void write_page(volatile char* page, int value) {
    *page = static_cast<char>(value);
}

CML_TEST_NOINLINE int read_through(volatile int* p) {
    return *p + 5;
}

CML_TEST_NOINLINE int inner_reads(volatile int* p) {
    return *p;
}

// Keeps values in registers that the callee may use and the unwinder must restore.
CML_TEST_NOINLINE int outer_calls(volatile int* p, volatile int* a, volatile int* b) {
    const int x = *a;
    const int y = *b;
    const int r = inner_reads(p);
    return r + x + y;
}

// mov eax,100 ; xor edx,edx ; idiv dword ptr [rcx] ; ret   (the divisor is read from memory, so a fix of that memory makes a retry safe)
const unsigned char kDivideStub[] = {0xB8, 0x64, 0x00, 0x00, 0x00, 0x31, 0xD2, 0xF7, 0x39, 0xC3};
using DivideFn = int (*)(volatile int*);

// ---- the process under test ---------------------------------------------------------------------------------------------

std::vector<Range> image_ranges() {
    const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return {Range{reinterpret_cast<std::uint64_t>(base), nt->OptionalHeader.SizeOfImage, false}};
}

// Plays the agent thread: waits for the gate's notify event, hands every held thread to `on_held`.
class Decider {
public:
    explicit Decider(std::function<void(const Held&)> on_held) : on_held_(std::move(on_held)) {
        notify_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ExceptionGate::shared().set_notify_event(notify_);
        thread_ = std::thread([this] {
            ExceptionGate& gate = ExceptionGate::shared();
            gate.add_ignored_thread(GetCurrentThreadId());
            while (!stop_.load()) {
                WaitForSingleObject(notify_, 20);
                Held held;
                while (gate.take_new(&held)) {
                    on_held_(held);
                    handled_.fetch_add(1);
                }
            }
        });
    }
    ~Decider() {
        stop_.store(true);
        thread_.join();
        ExceptionGate::shared().set_notify_event(nullptr);
        CloseHandle(notify_);
    }
    Decider(const Decider&) = delete;
    Decider& operator=(const Decider&) = delete;
    int handled() const { return handled_.load(); }
    // The decider counts a thread after it released it, so the released thread can get here first.
    bool wait_for_handled(int count) const {
        for (int i = 0; i < 2000 && handled_.load() < count; ++i) {
            Sleep(1);
        }
        return handled_.load() == count;
    }

private:
    std::function<void(const Held&)> on_held_;
    HANDLE notify_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<int> handled_{0};
    std::thread thread_;
};

// A handler behind the gate in the chain: it sees a fault only when the gate passed it on (Ignore, not acted on).
std::atomic<int> g_next_handler_saw{0};
std::atomic<void*> g_repair_page{nullptr};

LONG CALLBACK last_handler(PEXCEPTION_POINTERS info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_next_handler_saw.fetch_add(1);
    if (void* page = g_repair_page.load()) {
        DWORD old = 0;
        VirtualProtect(page, 4096, PAGE_READWRITE, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

struct ChildProcess {
    ChildProcess() {
        ExceptionGate& gate = ExceptionGate::shared();
        const cml::Result<void> installed = gate.install();
        REQUIRE_MESSAGE(installed.has_value(), "installing the handler");
        cookie_ = AddVectoredExceptionHandler(0, &last_handler);  // behind the gate's
        g_next_handler_saw.store(0);
        g_repair_page.store(nullptr);
        const std::vector<Range> ranges = image_ranges();
        gate.set_ranges(ranges.data(), ranges.size());
        gate.configure(true, false);
    }
    ~ChildProcess() {
        ExceptionGate::shared().configure(false, false);
        if (cookie_ != nullptr) {
            RemoveVectoredExceptionHandler(cookie_);
        }
    }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    void* cookie_ = nullptr;
};

char* make_noaccess_page() {
    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    REQUIRE(page != nullptr);
    DWORD old = 0;
    REQUIRE(VirtualProtect(page, 4096, PAGE_NOACCESS, &old) != 0);
    return static_cast<char*>(page);
}

void* make_divide_stub() {
    void* code = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    REQUIRE(code != nullptr);
    std::memcpy(code, kDivideStub, sizeof kDivideStub);
    FlushInstructionCache(GetCurrentProcess(), code, sizeof kDivideStub);
    std::vector<Range> ranges = image_ranges();
    ranges.push_back(Range{reinterpret_cast<std::uint64_t>(code), 4096, true});  // a "patch image"
    ExceptionGate::shared().set_ranges(ranges.data(), ranges.size());
    return code;
}

// ---- the scenarios (each runs in the child) -------------------------------------------------------------------------------

void scenario_continue_after_fix() {
    ChildProcess process;
    char* page = make_noaccess_page();
    const DWORD main_thread = GetCurrentThreadId();
    Decider decider([&](const Held& held) {
        // The "patch": make the memory the faulting instruction writes accessible, then resume at that instruction.
        expect(held.code == EXCEPTION_ACCESS_VIOLATION, "code is an access violation");
        expect(held.access == 1, "the access is a write");
        expect(held.data_address == reinterpret_cast<std::uint64_t>(page), "data address is the page");
        expect(!held.in_patch, "not in a patch");
        expect(held.thread_id == main_thread, "thread id is the faulting thread's");
        DWORD old = 0;
        VirtualProtect(page, 4096, PAGE_READWRITE, &old);
        expect(ExceptionGate::shared().resolve(held.id, Action::Continue).has_value(), "continue accepted");
    });
    write_page(page, 42);
    CHECK(page[0] == 42);
    CHECK(decider.wait_for_handled(1));
    CHECK(g_next_handler_saw.load() == 0);
}

void scenario_leave_function() {
    ChildProcess process;
    Decider decider([&](const Held& held) {
        expect(held.code == EXCEPTION_ACCESS_VIOLATION, "code is an access violation");
        expect(held.access == 0, "the access is a read");
        expect(held.data_address == 0, "null was read");
        expect(ExceptionGate::shared().resolve(held.id, Action::Leave).has_value(), "leave accepted");
    });
    // The function faults reading null; Leave returns 0 from it, so the caller sees 0 (not 5).
    CHECK(read_through(g_null) == 0);
    CHECK(decider.wait_for_handled(1));
}

void scenario_leave_restores_the_callers_registers() {
    ChildProcess process;
    Decider decider([&](const Held& held) {
        expect(ExceptionGate::shared().resolve(held.id, Action::Leave).has_value(), "leave accepted");
    });
    volatile int a = 7;
    volatile int b = 11;
    // inner_reads() faults and returns 0; outer_calls() carries on with its own locals: 0 + 7 + 11.
    CHECK(outer_calls(g_null, &a, &b) == 18);
    CHECK(a == 7);
    CHECK(b == 11);
    CHECK(decider.wait_for_handled(1));
}

void scenario_ignore_passes_the_fault_on() {
    ChildProcess process;
    char* page = make_noaccess_page();
    g_repair_page.store(page);
    Decider decider([&](const Held& held) {
        expect(ExceptionGate::shared().resolve(held.id, Action::Ignore).has_value(), "ignore accepted");
    });
    write_page(page, 9);  // the handler behind the gate repairs the page and resumes
    CHECK(page[0] == 9);
    CHECK(decider.wait_for_handled(1));
    CHECK(g_next_handler_saw.load() == 1);
}

void scenario_divide_by_zero_continue() {
    ChildProcess process;
    void* code = make_divide_stub();
    volatile int divisor = 0;
    Decider decider([&](const Held& held) {
        expect(held.code == EXCEPTION_INT_DIVIDE_BY_ZERO, "code is a division by zero");
        expect(held.access == 0xFFFFFFFFu, "no access kind");
        expect(held.in_patch, "in a patch range");
        expect(held.address == reinterpret_cast<std::uint64_t>(code) + 7, "the idiv faulted");
        divisor = 5;  // the "patch" fixes the state the instruction reads
        expect(ExceptionGate::shared().resolve(held.id, Action::Continue).has_value(), "continue accepted");
    });
    const int quotient = reinterpret_cast<DivideFn>(code)(&divisor);
    CHECK(quotient == 20);
    CHECK(decider.wait_for_handled(1));
}

void scenario_divide_by_zero_leave_leaf() {
    ChildProcess process;
    void* code = make_divide_stub();
    volatile int divisor = 0;
    Decider decider([&](const Held& held) {
        // The stub has no unwind information: it is a leaf, and the return address is at RSP.
        expect(ExceptionGate::shared().resolve(held.id, Action::Leave).has_value(), "leave accepted");
    });
    CHECK(reinterpret_cast<DivideFn>(code)(&divisor) == 0);
    CHECK(decider.wait_for_handled(1));
}

void scenario_not_in_range_is_not_held() {
    ChildProcess process;
    ExceptionGate::shared().set_ranges(nullptr, 0);  // no application range at all
    char* page = make_noaccess_page();
    g_repair_page.store(page);
    Decider decider([&](const Held&) { expect(false, "a fault outside the application's ranges must not be held"); });
    write_page(page, 3);
    CHECK(page[0] == 3);
    CHECK(decider.handled() == 0);
    CHECK(g_next_handler_saw.load() == 1);
}

void scenario_disabled_is_not_held() {
    ChildProcess process;
    ExceptionGate::shared().configure(false, false);
    char* page = make_noaccess_page();
    g_repair_page.store(page);
    Decider decider([&](const Held&) { expect(false, "a disabled gate must not hold"); });
    write_page(page, 4);
    CHECK(page[0] == 4);
    CHECK(decider.handled() == 0);
    CHECK(g_next_handler_saw.load() == 1);
}

void scenario_ignored_thread_is_not_held() {
    ChildProcess process;
    ExceptionGate::shared().add_ignored_thread(GetCurrentThreadId());
    char* page = make_noaccess_page();
    g_repair_page.store(page);
    Decider decider([&](const Held&) { expect(false, "an ignored thread must not be held"); });
    write_page(page, 5);
    CHECK(page[0] == 5);
    CHECK(g_next_handler_saw.load() == 1);
    ExceptionGate::shared().remove_ignored_thread(GetCurrentThreadId());
}

void scenario_stopped_world_is_not_held() {
    ChildProcess process;
    char* page = make_noaccess_page();
    g_repair_page.store(page);
    Decider decider([&](const Held&) { expect(false, "no thread is held while the hook installer has the others stopped"); });
    cml::agent::heap::set_alloc_forbidden(true);  // what the installer sets around its stopped window
    write_page(page, 6);
    cml::agent::heap::set_alloc_forbidden(false);
    CHECK(page[0] == 6);
    CHECK(g_next_handler_saw.load() == 1);
}

void scenario_switching_off_releases_the_thread_as_ignore() {
    ChildProcess process;
    char* page = make_noaccess_page();
    g_repair_page.store(page);
    Decider decider([&](const Held&) {
        // The last subscriber went away: the agent answers everything held with Ignore.
        ExceptionGate::shared().configure(false, false);
    });
    write_page(page, 8);
    CHECK(page[0] == 8);
    CHECK(g_next_handler_saw.load() == 1);
}

void scenario_resolve_unknown_id() {
    ChildProcess process;
    const cml::Result<void> resolved = ExceptionGate::shared().resolve(12345, Action::Continue);
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().code == 4400);
}

// Nine threads fault at once; the gate has eight slots, so one fault is passed on and counted.
void scenario_slot_overflow_is_passed_on() {
    ChildProcess process;
    char* page = make_noaccess_page();
    constexpr int kThreads = 9;
    std::atomic<int> seen{0};
    std::vector<Held> held_threads;
    Decider decider([&](const Held& held) {
        held_threads.push_back(held);
        seen.fetch_add(1);
    });
    // The overflowing thread reaches the handler behind the gate: that handler repairs the page, which lets the held
    // threads' retry succeed too once they are released with Continue.
    g_repair_page.store(page);
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] { write_page(page + i, i + 1); });
    }
    // Wait until the gate holds eight and the ninth went by.
    for (int i = 0; i < 3000 && (seen.load() < 8 || g_next_handler_saw.load() < 1); ++i) {
        Sleep(10);
    }
    CHECK(seen.load() == 8);
    CHECK(g_next_handler_saw.load() >= 1);
    CHECK(ExceptionGate::shared().take_overflow_count() >= 1);
    CHECK(ExceptionGate::shared().held_count() == 8);
    for (const Held& held : held_threads) {
        CHECK(ExceptionGate::shared().resolve(held.id, Action::Continue).has_value());
    }
    for (std::thread& t : threads) {
        t.join();
    }
    for (int i = 0; i < kThreads; ++i) {
        CHECK(page[i] == i + 1);
    }
    CHECK(ExceptionGate::shared().held_count() == 0);
}

struct Scenario {
    const char* name;
    void (*run)();
};

const Scenario kScenarios[] = {
    {"continue_after_fix", &scenario_continue_after_fix},
    {"leave_function", &scenario_leave_function},
    {"leave_restores_registers", &scenario_leave_restores_the_callers_registers},
    {"ignore_passes_on", &scenario_ignore_passes_the_fault_on},
    {"divide_by_zero_continue", &scenario_divide_by_zero_continue},
    {"divide_by_zero_leave_leaf", &scenario_divide_by_zero_leave_leaf},
    {"not_in_range", &scenario_not_in_range_is_not_held},
    {"disabled", &scenario_disabled_is_not_held},
    {"ignored_thread", &scenario_ignored_thread_is_not_held},
    {"stopped_world", &scenario_stopped_world_is_not_held},
    {"switch_off_releases", &scenario_switching_off_releases_the_thread_as_ignore},
    {"resolve_unknown", &scenario_resolve_unknown_id},
    {"slot_overflow", &scenario_slot_overflow_is_passed_on},
};

// ---- the parent side -------------------------------------------------------------------------------------------------------

std::wstring self_path() {
    wchar_t buffer[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(sizeof buffer / sizeof buffer[0]));
    return buffer;
}

std::wstring widen(const char* text) {
    std::wstring out;
    for (; *text != '\0'; ++text) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text)));
    }
    return out;
}

// Starts this executable running only the `exc_child` test case with CML_TEST_EXC_CHILD=name.
// Returns the exit code, or -1 when it could not be started or did not finish in time.
long run_child(const char* name, std::string* result_text) {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring result_file = std::wstring(temp) + L"cml_exc_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                                     std::to_wstring(GetTickCount64()) + L".txt";
    DeleteFileW(result_file.c_str());
    SetEnvironmentVariableW(L"CML_TEST_EXC_CHILD", widen(name).c_str());
    SetEnvironmentVariableW(L"CML_TEST_EXC_RESULT", result_file.c_str());
    std::wstring command = L"\"" + self_path() + L"\" --test-case=exc_child";
    STARTUPINFOW startup = {};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION info = {};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info) == 0) {
        return -1;
    }
    long code = -1;
    if (WaitForSingleObject(info.hProcess, 90000) == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        GetExitCodeProcess(info.hProcess, &exit_code);
        code = static_cast<long>(exit_code);
    } else {
        TerminateProcess(info.hProcess, 99);
    }
    CloseHandle(info.hProcess);
    CloseHandle(info.hThread);
    SetEnvironmentVariableW(L"CML_TEST_EXC_CHILD", nullptr);
    SetEnvironmentVariableW(L"CML_TEST_EXC_RESULT", nullptr);
    if (result_text != nullptr) {
        result_text->clear();
        if (FILE* f = _wfopen(result_file.c_str(), L"rb")) {
            char buffer[256];
            const std::size_t n = std::fread(buffer, 1, sizeof buffer - 1, f);
            buffer[n] = 0;
            *result_text = buffer;
            std::fclose(f);
        }
    }
    DeleteFileW(result_file.c_str());
    return code;
}

}  // namespace

// The child body: runs the scenario named by CML_TEST_EXC_CHILD and writes "ok <name>" to the result file when it got to the
// end (a failed check also makes the exit code non-zero). In the parent (and in a normal run of this executable) the variable
// is absent and the test case does nothing.
TEST_CASE("exc_child") {
    wchar_t name_wide[128] = {};
    if (GetEnvironmentVariableW(L"CML_TEST_EXC_CHILD", name_wide, 128) == 0) {
        return;
    }
    std::string name;
    for (const wchar_t* c = name_wide; *c != L'\0'; ++c) {
        name.push_back(static_cast<char>(*c));
    }
    bool found = false;
    for (const Scenario& scenario : kScenarios) {
        if (name == scenario.name) {
            found = true;
            scenario.run();
        }
    }
    REQUIRE_MESSAGE(found, "unknown scenario " << name);
    CHECK(g_problems.load() == 0);
    wchar_t result_file[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"CML_TEST_EXC_RESULT", result_file, MAX_PATH) != 0) {
        if (FILE* f = _wfopen(result_file, L"wb")) {
            std::fputs(("ok " + name).c_str(), f);
            std::fclose(f);
        }
    }
}

TEST_CASE("crash handler: each scenario behaves, in a child process") {
    for (const Scenario& scenario : kScenarios) {
        SUBCASE(scenario.name) {
            std::string text;
            const long code = run_child(scenario.name, &text);
            CHECK_MESSAGE(code == 0, "the child exited with " << code << " (scenario " << scenario.name << ")");
            CHECK_MESSAGE(text == std::string("ok ") + scenario.name, "result file said '" << text << "'");
        }
    }
}
