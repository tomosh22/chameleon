#pragma once

// Auto-start of the headless broker (plan section 4.13 / WP-11 lifecycle step 3).

#if defined(_WIN32)

#include <functional>
#include <string>
#include <vector>

#include "cml/util/result.h"

namespace cml::agent {

// Starts `broker_path --headless --pipe <pipe_name> [extra_args]` as a detached process (no
// console, no inherited handles, its own process group, so it outlives the application and is
// not closed by Ctrl+C aimed at it) and in the broker's own directory. Returns at once; the caller retries connecting. CML5211 when
// the executable does not exist or cannot be started.
// Idle time after which an auto-started broker exits when the caller did not pass its own
// --exit-when-idle: five minutes.
inline constexpr unsigned kDefaultIdleExitMs = 5 * 60 * 1000;
// Likewise --startup-timeout: an auto-started broker no agent ever connects to exits after a minute.
inline constexpr unsigned kDefaultStartupTimeoutMs = 60 * 1000;

// The command line launch_broker uses: `"<broker>" --headless --pipe "<name>" <extra args>`, plus
// `--exit-when-idle <kDefaultIdleExitMs>` and `--startup-timeout <kDefaultStartupTimeoutMs>` unless
// extra_args already contains that option.
std::wstring broker_command_line(const std::wstring& broker_path, const std::string& pipe_name,
                                 const std::string& extra_args);

// The working directory for the broker: its own directory. Empty (meaning: inherit the host's) when
// that directory is too long for CreateProcess's lpCurrentDirectory (MAX_PATH) or cannot be told.
// A drive root keeps its trailing separator ("C:\\", not "C:", which means "the current directory
// of drive C").
std::wstring broker_working_directory(const std::wstring& broker_path);

// Splits a command-line tail into arguments with the rules of CommandLineToArgvW (quotes,
// backslashes before quotes, "" inside quotes). The agent cannot use that function itself: it lives
// in shell32.dll, which the agent must not import.
std::vector<std::wstring> split_command_line(const std::wstring& line);

// The broker runs in its own directory, so a relative path in CML_BROKER_ARGS would be resolved
// against that directory, not against the host's. For every `--log <path>` pair the agent therefore
// resolves <path> through `resolve` (production: resolve_against_host_directory) before launch; an
// empty result leaves the value alone. Arguments without `--log` are returned untouched; otherwise
// the tail is re-quoted from the parsed tokens. Other options are never rewritten.
std::string absolutize_log_arguments(const std::string& args,
                                     const std::function<std::wstring(const std::wstring&)>& resolve);

// GetFullPathNameW against the host's current directory; handles drive-relative forms ("C:log")
// and ".." too. Empty on failure.
std::wstring resolve_against_host_directory(const std::wstring& path);

Result<void> launch_broker(const std::wstring& broker_path, const std::string& pipe_name,
                           const std::string& extra_args);

// Where to look for the broker: `override_path` (CML_BROKER_PATH) if non-empty, else
// cml_broker.exe in `agent_dir` (the directory of cml_agent.dll).
std::wstring broker_path_for(const std::wstring& override_path, const std::wstring& agent_dir);

}  // namespace cml::agent

#endif  // _WIN32
