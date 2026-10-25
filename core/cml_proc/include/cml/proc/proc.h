#pragma once

// cml_proc: process, module and session information shared by the agent and the broker.
//
// Everything here talks to Win32, so the whole header is Windows-only. Failures are cml::Diag
// values (CML52xx, see diag_ids.h), never exceptions.

#if defined(_WIN32)

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

// Full command line of the calling process, UTF-8.
std::string current_command_line();

// Value of environment variable `name` (UTF-8 names and values), or nullopt when unset.
std::optional<std::string> get_env(std::string_view name);

// UTF-8 <-> UTF-16 conversion. Invalid UTF-8 becomes U+FFFD; an OS conversion failure (only
// possible for absurdly large inputs) yields an empty string.
std::string utf8_from_wide(const wchar_t* text, std::size_t length);
std::string utf8_from_wide(std::wstring_view text);
std::wstring wide_from_utf8(std::string_view text);

// Command-line arguments of the calling process as UTF-8 (argv[0] included).
std::vector<std::string> command_line_arguments();

}  // namespace cml::proc

#endif  // _WIN32
