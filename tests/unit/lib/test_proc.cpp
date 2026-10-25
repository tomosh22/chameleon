// cml_proc: module enumeration, session SIDs, environment and UTF-8 helpers (Windows only).
#include "cml/proc/proc.h"

#include "doctest.h"

#if defined(_WIN32)

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <string>

#include "cml/proc/diag_ids.h"

using namespace cml;

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string self_path() {
    wchar_t buffer[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, 32768);
    return proc::utf8_from_wide(buffer, n);
}

}  // namespace

TEST_CASE("enumerate_modules lists this exe and kernel32") {
    Result<std::vector<proc::ModuleInfo>> modules = proc::enumerate_modules();
    REQUIRE_MESSAGE(modules.has_value(), (modules.has_value() ? "" : modules.error().format()));
    CHECK(modules->size() >= 3);

    const std::string self = lower(self_path());
    bool found_self = false;
    bool found_kernel32 = false;
    for (const proc::ModuleInfo& module : *modules) {
        CHECK(module.base != 0);
        CHECK(module.size != 0);
        const std::string path = lower(module.path);
        found_self = found_self || path == self;
        found_kernel32 =
            found_kernel32 || (path.size() >= 12 && path.substr(path.size() - 12) == "kernel32.dll");
    }
    CHECK(found_self);
    CHECK(found_kernel32);

    // pid 0 means "this process".
    Result<std::vector<proc::ModuleInfo>> explicit_pid = proc::enumerate_modules(proc::current_process_id());
    REQUIRE(explicit_pid.has_value());
    CHECK(explicit_pid->size() >= 3);
}

TEST_CASE("enumerate_modules reports CML5220 for a process that does not exist") {
    Result<std::vector<proc::ModuleInfo>> modules = proc::enumerate_modules(0xFFFFFF00u);
    REQUIRE_FALSE(modules.has_value());
    CHECK(modules.error().code == ids::kModuleEnumFailed);
    CHECK(modules.error().valid());
    CHECK_FALSE(modules.error().what_to_do.empty());
}

TEST_CASE("session and user SIDs") {
    Result<std::string> logon = proc::logon_session_sid();
    REQUIRE(logon.has_value());
    CHECK(logon->rfind("S-1-", 0) == 0);
    Result<std::string> user = proc::current_user_sid();
    REQUIRE(user.has_value());
    CHECK(user->rfind("S-1-", 0) == 0);
    // Stable within a process.
    CHECK(*proc::logon_session_sid() == *logon);
}

TEST_CASE("process_runs_as_current_user") {
    Result<bool> self = proc::process_runs_as_current_user(proc::current_process_id());
    REQUIRE(self.has_value());
    CHECK(*self);
    Result<bool> missing = proc::process_runs_as_current_user(0xFFFFFF00u);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ids::kSidQueryFailed);
}

TEST_CASE("UTF-8 and UTF-16 conversion round-trips") {
    const std::wstring wide = L"C:\\Users\\J\u00fcrgen\\caf\u00e9 \U0001F600\\game.dll";
    const std::string utf8 = proc::utf8_from_wide(wide);
    CHECK(utf8.find("J\xC3\xBCrgen") != std::string::npos);
    CHECK(proc::wide_from_utf8(utf8) == wide);
    CHECK(proc::utf8_from_wide(L"", 0).empty());
    CHECK(proc::utf8_from_wide(nullptr, 0).empty());
    CHECK(proc::wide_from_utf8("").empty());
    // Invalid UTF-8 becomes U+FFFD instead of failing.
    const std::wstring repaired = proc::wide_from_utf8("a\xFF" "b");
    CHECK(repaired.size() == 3);
    CHECK(repaired[1] == 0xFFFD);
}

TEST_CASE("get_env distinguishes unset, empty and long values") {
    CHECK_FALSE(proc::get_env("CML_UT_PROC_DEFINITELY_UNSET").has_value());

    SetEnvironmentVariableW(L"CML_UT_PROC_VAR", L"");
    std::optional<std::string> empty = proc::get_env("CML_UT_PROC_VAR");
    REQUIRE(empty.has_value());
    CHECK(empty->empty());

    SetEnvironmentVariableW(L"CML_UT_PROC_VAR", L"value \u00e9");
    CHECK(proc::get_env("CML_UT_PROC_VAR") == std::string("value \xC3\xA9"));

    const std::wstring long_value(5000, L'x');
    SetEnvironmentVariableW(L"CML_UT_PROC_VAR", long_value.c_str());
    std::optional<std::string> big = proc::get_env("CML_UT_PROC_VAR");
    REQUIRE(big.has_value());
    CHECK(big->size() == 5000);
    SetEnvironmentVariableW(L"CML_UT_PROC_VAR", nullptr);
    CHECK_FALSE(proc::get_env("CML_UT_PROC_VAR").has_value());
}

TEST_CASE("command line access") {
    const std::string line = proc::current_command_line();
    CHECK(lower(line).find("cml_test_proc") != std::string::npos);
    const std::vector<std::string> args = proc::command_line_arguments();
    REQUIRE_FALSE(args.empty());
    CHECK(lower(args[0]).find("cml_test_proc") != std::string::npos);
}

#endif  // _WIN32
