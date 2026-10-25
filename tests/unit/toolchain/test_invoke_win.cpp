// CreateProcess wrapper: captured output, exit codes, cwd, environment, cancellation, timeouts, @files.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "cml/toolchain/toolchain.h"
#include "doctest.h"

using namespace cml::toolchain;

namespace {

std::string cmd_exe() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"ComSpec", buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    REQUIRE(n > 0);
    return std::filesystem::path(std::wstring(buf, n)).string();
}

ProcessSpec cmd(const std::string& command) {
    ProcessSpec spec;
    spec.exe = cmd_exe();
    spec.args = {"/c", command};
    spec.timeout_ms = 60000;
    return spec;
}

constexpr const char* kSleepForever = "ping -n 40 127.0.0.1 >nul";

}  // namespace

TEST_CASE("run_process: output, exit code, stderr merged") {
    auto r = run_process(cmd("echo hello world"));
    REQUIRE(r);
    CHECK(r->exit_code == 0);
    CHECK(r->output.find("hello world") != std::string::npos);
    CHECK_FALSE(r->timed_out);
    CHECK_FALSE(r->cancelled);

    auto fail = run_process(cmd("exit 7"));
    REQUIRE(fail);
    CHECK(fail->exit_code == 7);

    auto both = run_process(cmd("echo to-stdout & echo to-stderr 1>&2"));
    REQUIRE(both);
    CHECK(both->output.find("to-stdout") != std::string::npos);
    CHECK(both->output.find("to-stderr") != std::string::npos);
    CHECK(both->output.find("to-stdout") < both->output.find("to-stderr"));  // arrival order

    // Large output does not deadlock the pipe.
    auto big = run_process(cmd("for /L %i in (1,1,5000) do @echo line %i of padding padding padding padding padding"));
    REQUIRE(big);
    CHECK(big->output.size() > 200000);
}

TEST_CASE("run_process: cwd, environment, stdin is not inherited") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    ProcessSpec spec = cmd("cd");
    spec.cwd = dir.string();
    auto r = run_process(spec);
    REQUIRE(r);
    CHECK(r->output.find(dir.string().substr(0, dir.string().size() - 1)) != std::string::npos);

    ProcessSpec env = cmd("echo [%CML_TEST_VAR%][%PATH:~0,1%]");
    env.env_overrides = {{"CML_TEST_VAR", "value with spaces"}};
    auto e = run_process(env);
    REQUIRE(e);
    CHECK(e->output.find("[value with spaces]") != std::string::npos);
    CHECK(e->output.find("[%PATH") == std::string::npos);  // the inherited environment is kept

    // `set /p` would block on a console stdin; with NUL it returns at once.
    auto in = run_process(cmd("set /p x=prompt"));
    REQUIRE(in);
}

TEST_CASE("run_process: a missing program is CML2202") {
    ProcessSpec spec;
    spec.exe = R"(C:\no\such\program.exe)";
    auto r = run_process(spec);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == diag::kCannotStartCompiler);
}

TEST_CASE("run_process: timeout kills the process tree") {
    ProcessSpec spec = cmd(kSleepForever);
    spec.timeout_ms = 400;
    const auto start = std::chrono::steady_clock::now();
    auto r = run_process(spec);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    REQUIRE(r);
    CHECK(r->timed_out);
    CHECK(r->exit_code != 0);
    CHECK(secs < 20.0);
}

TEST_CASE("run_process: cancellation through the token (before, during)") {
    CancelToken pre;
    pre.cancel();
    auto none = run_process(cmd("echo should not run"), &pre);
    REQUIRE(none);
    CHECK(none->cancelled);
    CHECK(none->output.empty());

    CancelToken token;
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        token.cancel();
    });
    const auto start = std::chrono::steady_clock::now();
    auto r = run_process(cmd(kSleepForever), &token);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    canceller.join();
    REQUIRE(r);
    CHECK(r->cancelled);
    CHECK(r->exit_code != 0);
    CHECK(secs < 20.0);
}

TEST_CASE("run_process: one token cancels several parallel processes") {
    CancelToken token;
    std::vector<std::thread> threads;
    std::atomic<int> cancelled{0};
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] {
            auto r = run_process(cmd(kSleepForever), &token);
            if (r && r->cancelled) ++cancelled;
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    token.cancel();
    for (auto& t : threads) t.join();
    CHECK(cancelled.load() == 4);
}

TEST_CASE("run_process: long command lines and force_response_file go through an @file that is cleaned up") {
    ProcessSpec spec;
    spec.exe = cmd_exe();
    spec.args = {"/c", "echo", "x"};
    spec.force_response_file = true;
    spec.timeout_ms = 20000;
    spec.response_file_dir = std::filesystem::temp_directory_path().string();
    auto r = run_process(spec);
    REQUIRE(r);
    CHECK_FALSE(r->response_file.empty());
    CHECK(r->command_line.find('@') != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(r->response_file));

    ProcessSpec longer = cmd("echo ok");
    longer.max_command_line = 8;  // anything is "too long"
    longer.timeout_ms = 20000;
    auto l = run_process(longer);
    REQUIRE(l);
    CHECK_FALSE(l->response_file.empty());
}

TEST_CASE("detect_tool on a real binary: cmd.exe has a version resource, the banner of a non-tool is rejected") {
    auto v = read_file_version(cmd_exe());
    REQUIRE_MESSAGE(v, v.error().format());
    CHECK((*v)[0] >= 6);
    auto info = detect_tool(cmd_exe());
    CHECK_FALSE(info);  // not a compiler: no banner we know and an unknown family
    auto missing = detect_tool(R"(C:\no\such\cl.exe)");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == diag::kToolNotFound);
}

TEST_CASE("detect_tool finds the compiler this test was built with") {
#ifdef CML_TEST_CXX_COMPILER
    const std::string path = CML_TEST_CXX_COMPILER;
#else
    const std::string path;
#endif
    if (path.empty() || !std::filesystem::exists(path)) {
        return;
    }
    auto info = detect_tool(path);
    REQUIRE_MESSAGE(info, info.error().format());
    CHECK((info->family == ToolFamily::Cl || info->family == ToolFamily::ClangCl));
    CHECK(info->major() >= 17);
    std::printf("TOOL %s family=%s version=%s source=%s\n", path.c_str(), to_string(info->family), info->version_text.c_str(),
                info->from_version_resource ? "version-resource" : "banner");
}
