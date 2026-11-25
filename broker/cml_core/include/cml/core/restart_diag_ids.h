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

// WP-34 (ADR-0040): `restart {hold_new:true}` holds the new process suspended until `restart-resume` or `hold_ms`. CML5813-5819.
inline constexpr std::uint32_t kHoldExpired = 5813;       // (warning) nobody resumed the held process within hold_ms: the broker did
inline constexpr std::uint32_t kNotHeld = 5814;           // `restart-resume` for a pid that is not held (never held, or already resumed)
inline constexpr std::uint32_t kHoldAborted = 5815;       // the broker shut down while holding: the process was resumed, the restart ended
inline constexpr std::uint32_t kHoldClientGone = 5816;    // (warning) the client that asked for the restart went away while holding: resumed, the restart goes on
inline constexpr std::uint32_t kResumeFailed = 5817;      // the held process could not be resumed (it was terminated rather than left suspended)
inline constexpr std::uint32_t kBadHoldArgs = 5818;       // `hold_new` / `hold_ms` / `restart-resume` arguments invalid
inline constexpr std::uint32_t kHoldUnavailable = 5819;   // this broker or its process host cannot hold a new process; nothing was touched

// WP-43 (ADR-0043, R-40): CML5820-5839.
inline constexpr std::uint32_t kResumeUnconfirmed = 5820; // `restart-resume`: the restart did not confirm in time that the process runs (it may still be held)
inline constexpr std::uint32_t kShutdown = 5821;          // the broker shut down during the restart: before the termination nothing was touched, after it the new process was left running
inline constexpr std::uint32_t kRelaunchRetried = 5822;   // (warning) the first attempt to start the new process failed; the broker tries once more

// The harness's restart step uses CML5840 and CML5841 (cml_ipc/control_ids.h: kRestartStepFailed, kRestartStepMismatch).

}  // namespace cml::core::restart::diag
