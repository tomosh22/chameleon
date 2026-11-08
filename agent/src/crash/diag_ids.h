#pragma once

#include <cstdint>

// Registry of the CML44xx crash-handler diagnostic ids (ADR-0032: CML4400-4449 belong to WP-20 in the agent).
// docs/wp/WP-20.md repeats every id with its message and "what to do" text.
namespace cml::agent::crash::ids {

inline constexpr std::uint32_t kUnknownException = 4400;   // ExceptionAction for a thread that is not held (answered already, or never held)
inline constexpr std::uint32_t kLeaveUnwindFailed = 4401;  // Leave function: the frame cannot be unwound (no unwind info and no readable return address)
inline constexpr std::uint32_t kLeaveBadFrame = 4402;      // Leave function: the unwound frame does not look like a caller
inline constexpr std::uint32_t kSlotsFull = 4403;          // more threads faulted at once than the handler has slots: the extra faults were passed on
inline constexpr std::uint32_t kHandlerInstallFailed = 4404;  // AddVectoredExceptionHandler failed: crash handling is off
inline constexpr std::uint32_t kNoSlotEvent = 4405;        // the handler's wait events could not be created: crash handling is off

// WP-23 (ADR-0035: CML4600-4649): re-execute function after a crash.
inline constexpr std::uint32_t kReexecuteBadFrame = 4600;        // the stack of the held thread cannot be unwound to the chosen frame
inline constexpr std::uint32_t kReexecuteStackMismatch = 4601;   // the chosen frame was entered with another RSP than the broker saw
inline constexpr std::uint32_t kReexecuteBadTarget = 4602;       // the replacement function is not executable code
inline constexpr std::uint32_t kReexecuteBadLocation = 4603;     // a parameter's location cannot be read (register not supported, memory unreadable)

}  // namespace cml::agent::crash::ids
