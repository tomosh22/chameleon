#pragma once

// Auto-start of the headless broker (plan section 4.13 / WP-11 lifecycle step 3).

#if defined(_WIN32)

#include <string>

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

// The broker runs in its own directory, so a relative path in CML_BROKER_ARGS would be resolved
// against that directory, not against the host's. For `--log` (the one option that takes a path)
// the agent therefore makes a relative path absolute against `host_directory` first. Other options
// are passed through untouched.
std::string absolutize_log_argument(const std::string& args, const std::wstring& host_directory);

Result<void> launch_broker(const std::wstring& broker_path, const std::string& pipe_name,
                           const std::string& extra_args);

// Where to look for the broker: `override_path` (CML_BROKER_PATH) if non-empty, else
// cml_broker.exe in `agent_dir` (the directory of cml_agent.dll).
std::wstring broker_path_for(const std::wstring& override_path, const std::wstring& agent_dir);

}  // namespace cml::agent

#endif  // _WIN32
