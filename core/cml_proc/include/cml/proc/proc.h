#pragma once

// cml_proc: process, module and session information shared by the agent and the broker.
//
// Everything here talks to Win32, so the whole header is Windows-only. Failures are cml::Diag
// values (CML52xx, see diag_ids.h), never exceptions.

#if defined(_WIN32)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cml/util/result.h"

namespace cml::proc {

// One loaded module of a process, as reported by the toolhelp module snapshot.
struct ModuleInfo {
    std::uint64_t base = 0;  // load address
    std::uint64_t size = 0;  // size of the image in memory
    std::string path;        // full path, UTF-8
};

// Modules loaded in process `pid` (0 means the calling process). Retries a few times when the
// loader changes the module list while the snapshot is taken. Diag CML5220 on failure.
Result<std::vector<ModuleInfo>> enumerate_modules(std::uint32_t pid = 0);

// The user's logon-session SID as a string ("S-1-5-5-0-1234567"), used to name the broker pipe so
// that every logon session (and every user) gets its own broker. Falls back to the user SID when
// the token has no logon-id group. Diag CML5221 on failure.
Result<std::string> logon_session_sid();

// SID string of the user the calling process runs as (used in the pipe ACL). Diag CML5221.
Result<std::string> current_user_sid();

// True when process `pid` runs as the same user (SID) as the calling process. An agent uses it to
// verify that the owner of the broker pipe is not a different user. Diag CML5221 when the process
// cannot be inspected.
Result<bool> process_runs_as_current_user(std::uint32_t pid);

std::uint32_t current_process_id() noexcept;

// Reads a Win32 path-like string through `fetch(wchar_t* buffer, DWORD capacity) -> DWORD`
// (GetModuleFileNameW and friends): starts small, and while the call reports truncation (returns
// `capacity`) quadruples the buffer up to 32768 characters. Returns an empty string when the call
// fails (returns 0) or the string does not fit even then.
template <class Fetch>
std::wstring read_growing_string(Fetch fetch, std::size_t initial_capacity = 260) {
    constexpr std::size_t kLimit = 32768;
    std::wstring buffer(initial_capacity, L'\0');
    for (;;) {
        const std::size_t n = fetch(buffer.data(), static_cast<unsigned long>(buffer.size()));
        if (n == 0) {
            return {};
        }
        if (n < buffer.size()) {
            buffer.resize(n);
            return buffer;
        }
        if (buffer.size() >= kLimit) {
            return {};
        }
        buffer.assign(std::min(buffer.size() * 4, kLimit), L'\0');
    }
}

// Full command line of the calling process, UTF-8.
std::string current_command_line();

// Value of environment variable `name` (UTF-8 names and values), or nullopt when unset.
std::optional<std::string> get_env(std::string_view name);

// UTF-8 <-> UTF-16 conversion. Invalid UTF-8 becomes U+FFFD; an OS conversion failure (only
// possible for absurdly large inputs) yields an empty string.
std::string utf8_from_wide(const wchar_t* text, std::size_t length);
std::string utf8_from_wide(std::wstring_view text);
std::wstring wide_from_utf8(std::string_view text);

}  // namespace cml::proc

#endif  // _WIN32
