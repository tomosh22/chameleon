// The out-of-process hang watchdog (tests/unit/hang_watch) fires on a process whose threads are all
// suspended: the situation in which the in-process watchdog of doctest_main.cpp is frozen too.
//
// A victim process (the helper in --victim mode) starts three threads, suspends them like the hook
// installer would, prints READY and hangs. A second helper process watches the victim with a 2 s
// timeout. The test requires: the watcher printed one HANGWATCH line per thread of the victim with its
// suspend count and RIP (module+offset), the minidump exists, and the victim was terminated with exit
// code 3.
#if defined(_WIN32)

#include <windows.h>

#include <cstdio>
#include <string>

#include "doctest.h"

namespace {

struct Pipe {
    HANDLE read = nullptr;
    HANDLE write = nullptr;
};

Pipe make_pipe() {
    Pipe pipe;
    SECURITY_ATTRIBUTES attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    CreatePipe(&pipe.read, &pipe.write, &attributes, 0);
    SetHandleInformation(pipe.read, HANDLE_FLAG_INHERIT, 0);  // only the write end is inherited
    return pipe;
}

struct Child {
    HANDLE process = nullptr;
    DWORD pid = 0;
    Pipe output;  // the child's stdout and stderr
};

bool start(const std::string& command_line, Child* child) {
    child->output = make_pipe();
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = INVALID_HANDLE_VALUE;
    startup.hStdOutput = child->output.write;
    startup.hStdError = child->output.write;
    PROCESS_INFORMATION info = {};
    std::string command = command_line;
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &info)) {
        return false;
    }
    CloseHandle(info.hThread);
    child->process = info.hProcess;
    child->pid = info.dwProcessId;
    CloseHandle(child->output.write);  // the parent keeps only the read end
    child->output.write = nullptr;
    return true;
}

// Reads what the child has written so far (and, with `until_exit`, everything up to its end).
std::string read_available(Child& child, bool until_exit) {
    std::string text;
    for (;;) {
        DWORD available = 0;
        if (!until_exit && (!PeekNamedPipe(child.output.read, nullptr, 0, nullptr, &available, nullptr) || available == 0)) {
            break;
        }
        char buffer[4096];
        DWORD got = 0;
        if (!ReadFile(child.output.read, buffer, sizeof(buffer), &got, nullptr) || got == 0) {
            break;
        }
        text.append(buffer, got);
    }
    return text;
}

std::size_t count_of(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

}  // namespace

TEST_CASE("the out-of-process hang watchdog reports every thread of a process whose threads are all suspended") {
    const std::string helper = CML_HANG_WATCH_PATH;
    char temp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, temp);
    const std::string directory = std::string(temp) + "cml_hang_watch_test";

    Child victim;
    REQUIRE(start("\"" + helper + "\" --victim", &victim));
    // Wait for READY (the sleepers are suspended by then).
    std::string victim_output;
    for (int i = 0; i < 200 && victim_output.find("READY") == std::string::npos; ++i) {
        Sleep(50);
        victim_output += read_available(victim, false);
    }
    REQUIRE_MESSAGE(victim_output.find("READY") != std::string::npos, "the victim did not become ready: " << victim_output);

    Child watcher;
    REQUIRE(start("\"" + helper + "\" " + std::to_string(victim.pid) + " 2 \"" + directory + "\"", &watcher));
    const std::string output = read_available(watcher, true);  // until the watcher exits
    REQUIRE(WaitForSingleObject(watcher.process, 60000) == WAIT_OBJECT_0);
    DWORD watcher_exit = 0;
    GetExitCodeProcess(watcher.process, &watcher_exit);

    // The victim was terminated by the watcher.
    REQUIRE(WaitForSingleObject(victim.process, 30000) == WAIT_OBJECT_0);
    DWORD victim_exit = 0;
    GetExitCodeProcess(victim.process, &victim_exit);

    INFO("watcher output:\n" << output);
    CHECK(watcher_exit == 3u);
    CHECK(victim_exit == 3u);
    CHECK(output.find("is still running after 2 s") != std::string::npos);
    // main thread + 3 sleepers (+ whatever the runtime adds): one line each
    CHECK(count_of(output, "HANGWATCH: thread ") >= 4);
    CHECK(count_of(output, "suspend_count=1 ") >= 3);  // the three threads the victim suspended
    CHECK(count_of(output, "suspend_count=0 ") >= 1);  // its main thread
    CHECK(output.find("ntdll.dll+0x") != std::string::npos);  // RIP resolved to module+offset
    CHECK(output.find("minidump written to") != std::string::npos);

    const std::string dump = directory + "\\cml_hang_" + std::to_string(victim.pid) + ".dmp";
    const DWORD attributes = GetFileAttributesA(dump.c_str());
    CHECK_MESSAGE(attributes != INVALID_FILE_ATTRIBUTES, "no dump at " << dump);
    HANDLE file = CreateFileA(dump.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size = {};
        GetFileSizeEx(file, &size);
        CHECK(size.QuadPart > 0);
        CloseHandle(file);
        DeleteFileA(dump.c_str());
    }

    CloseHandle(victim.process);
    CloseHandle(victim.output.read);
    CloseHandle(watcher.process);
    CloseHandle(watcher.output.read);
}

TEST_CASE("the out-of-process hang watchdog leaves a process that ends in time alone") {
    const std::string helper = CML_HANG_WATCH_PATH;
    // The victim in --victim mode never ends by itself, so end it ourselves right away.
    Child victim;
    REQUIRE(start("\"" + helper + "\" --victim", &victim));
    Child watcher;
    REQUIRE(start("\"" + helper + "\" " + std::to_string(victim.pid) + " 120 \".\"", &watcher));
    Sleep(300);
    TerminateProcess(victim.process, 7);
    REQUIRE(WaitForSingleObject(watcher.process, 30000) == WAIT_OBJECT_0);  // it noticed
    DWORD watcher_exit = 99;
    GetExitCodeProcess(watcher.process, &watcher_exit);
    CHECK(watcher_exit == 0u);
    const std::string output = read_available(watcher, false);
    CHECK(output.find("HANGWATCH") == std::string::npos);
    CloseHandle(victim.process);
    CloseHandle(victim.output.read);
    CloseHandle(watcher.process);
    CloseHandle(watcher.output.read);
}

#endif  // _WIN32
