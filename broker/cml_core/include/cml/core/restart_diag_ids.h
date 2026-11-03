#pragma once

#include <cstdint>

// Diagnostic ids of the hot-restart (WP-21, plan section 4.12, broker range CML5800-5849 of ADR-0032). The agent's ids
// (CML4450-4499) are in agent/src/restart_ids.h. docs/wp/WP-21.md lists every id with its message and "what to do" text.
namespace cml::core::restart::diag {

inline constexpr std::uint32_t kUnavailable = 5800;       // this broker cannot restart processes (no process host on this system)
inline constexpr std::uint32_t kBadRequest = 5801;        // `restart` arguments invalid, or no pid given and not exactly one process
inline constexpr std::uint32_t kNoProcess = 5802;         // no connected process with that pid
inline constexpr std::uint32_t kBusy = 5803;              // another restart or a reload is running
inline constexpr std::uint32_t kPrepareFailed = 5804;     // the agent refused or did not answer: the process was not touched
inline constexpr std::uint32_t kOpenFailed = 5805;        // the process cannot be opened for termination / handle duplication: not touched
inline constexpr std::uint32_t kTerminateFailed = 5806;   // the old process could not be terminated
inline constexpr std::uint32_t kLaunchFailed = 5807;      // the old process is gone, the new one could not be started
inline constexpr std::uint32_t kNoRegistration = 5808;    // the new process did not register in time (left running)
inline constexpr std::uint32_t kModuleNotEnabled = 5809;  // (warning) a module of the old process is not enabled in the new one
inline constexpr std::uint32_t kReapplyFailed = 5810;     // (warning) the session's edits could not be re-applied to the new process
inline constexpr std::uint32_t kStdHandle = 5811;         // (warning) a standard handle could not be handed on; the new process has none for it
inline constexpr std::uint32_t kNewProcessExited = 5812;  // the new process exited before it registered

// Harness (tests/harness restart step), same range.
inline constexpr std::uint32_t kStepFailed = 5840;    // the harness's restart step failed
inline constexpr std::uint32_t kStepMismatch = 5841;  // restart result or event differs from what the scenario expects

}  // namespace cml::core::restart::diag
