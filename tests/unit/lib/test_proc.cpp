// cml_proc: module enumeration, session SIDs, environment and UTF-8 helpers (Windows only).
#include "cml/proc/proc.h"

#include "doctest.h"

#if defined(_WIN32)

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

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
}

TEST_CASE("read_growing_string grows the buffer until the string fits") {
    const std::wstring wanted(1000, L'p');
    std::vector<unsigned long> capacities;
    // Mimics GetModuleFileNameW: copies what fits and returns the capacity when it had to cut.
    const auto fetch = [&](wchar_t* buffer, unsigned long capacity) -> std::size_t {
        capacities.push_back(capacity);
        const std::size_t n = std::min<std::size_t>(wanted.size(), capacity);
        std::copy(wanted.begin(), wanted.begin() + static_cast<std::ptrdiff_t>(n), buffer);
        return n == wanted.size() && n < capacity ? n : capacity;
    };
    CHECK(proc::read_growing_string(fetch) == wanted);
    REQUIRE(capacities.size() >= 2);
    CHECK(capacities.front() == 260);
    CHECK(capacities.back() > 1000);

    // Exactly capacity-1 characters fit; exactly `capacity` is reported as truncated.
    const std::wstring edge(259, L'e');
    const auto fetch_edge = [&](wchar_t* buffer, unsigned long capacity) -> std::size_t {
        if (capacity > edge.size()) {
            std::copy(edge.begin(), edge.end(), buffer);
            return edge.size();
        }
        return capacity;
    };
    CHECK(proc::read_growing_string(fetch_edge) == edge);
}

TEST_CASE("read_growing_string gives up on failure and on absurd lengths") {
    CHECK(proc::read_growing_string([](wchar_t*, unsigned long) -> std::size_t { return 0; }).empty());
    CHECK(proc::read_growing_string([](wchar_t*, unsigned long capacity) -> std::size_t { return capacity; }).empty());
}

TEST_CASE("module paths longer than MAX_PATH are not truncated") {
    // Copy a DLL to a deeply nested directory (> 260 characters in total) and load it.
    wchar_t temp[MAX_PATH];
    REQUIRE(GetTempPathW(MAX_PATH, temp) > 0);
    std::wstring dir = L"\\\\?\\" + std::wstring(temp) + L"cml_ut_long_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    std::vector<std::wstring> created = {dir};
    for (int i = 0; i < 6; ++i) {
        dir += L"\\" + std::wstring(45, static_cast<wchar_t>(L'a' + i));
        CreateDirectoryW(dir.c_str(), nullptr);
        created.push_back(dir);
    }
    wchar_t exe[MAX_PATH];
    REQUIRE(GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0);
    const std::wstring exe_dir = std::wstring(exe).substr(0, std::wstring(exe).find_last_of(L'\\'));
    const std::wstring source = exe_dir + L"\\cml_test_dummy_module.dll";
    const std::wstring target = dir + L"\\cml_test_dummy_module.dll";
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
        for (auto it = created.rbegin(); it != created.rend(); ++it) RemoveDirectoryW(it->c_str());
        return;  // the dummy DLL is only built with the agent tests (unit.proc can run without it)
    }
    REQUIRE(CopyFileW(source.c_str(), target.c_str(), FALSE));
    HMODULE module = LoadLibraryW(target.c_str());
    if (module != nullptr) {
        Result<std::vector<proc::ModuleInfo>> modules = proc::enumerate_modules();
        REQUIRE(modules.has_value());
        std::size_t longest = 0;
        for (const proc::ModuleInfo& m : *modules) {
            longest = std::max(longest, m.path.size());
        }
        CHECK(longest > 260);
        FreeLibrary(module);
    } else {
        INFO("the long path could not be loaded on this system; skipping the check");
    }
    DeleteFileW(target.c_str());
    for (auto it = created.rbegin(); it != created.rend(); ++it) RemoveDirectoryW(it->c_str());
}

TEST_CASE("read_growing_string also grows when truncation is reported as capacity - 1") {
    // GetModuleFileNameExW style: writes the terminator and returns the cut length (capacity - 1).
    const std::wstring wanted(600, L'q');
    const auto fetch = [&](wchar_t* buffer, unsigned long capacity) -> std::size_t {
        const std::size_t n = std::min<std::size_t>(wanted.size(), capacity - 1);
        std::copy(wanted.begin(), wanted.begin() + static_cast<std::ptrdiff_t>(n), buffer);
        return n;
    };
    CHECK(proc::read_growing_string(fetch) == wanted);

    // A string that is exactly capacity - 1 long is indistinguishable from a cut one: still correct.
    const std::wstring exact(259, L'x');
    const auto fetch_exact = [&](wchar_t* buffer, unsigned long capacity) -> std::size_t {
        const std::size_t n = std::min<std::size_t>(exact.size(), capacity - 1);
        std::copy(exact.begin(), exact.begin() + static_cast<std::ptrdiff_t>(n), buffer);
        return n;
    };
    CHECK(proc::read_growing_string(fetch_exact) == exact);
}

#endif  // _WIN32
