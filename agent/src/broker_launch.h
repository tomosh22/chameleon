#pragma once

// Auto-start of the headless broker (plan section 4.13 / WP-11 lifecycle step 3).

#if defined(_WIN32)

#include <string>

#include "cml/util/result.h"

namespace cml::agent {

// Starts `broker_path --headless --pipe <pipe_name> [extra_args]` as a detached process (no
// console, no inherited handles, its own process group, so it outlives the application and is
// not closed by Ctrl+C aimed at it). Returns at once; the caller retries connecting. CML5211 when
// the executable does not exist or cannot be started.
Result<void> launch_broker(const std::wstring& broker_path, const std::string& pipe_name,
                           const std::string& extra_args);

// Where to look for the broker: `override_path` (CML_BROKER_PATH) if non-empty, else
// cml_broker.exe in `agent_dir` (the directory of cml_agent.dll).
std::wstring broker_path_for(const std::wstring& override_path, const std::wstring& agent_dir);

}  // namespace cml::agent

#endif  // _WIN32
