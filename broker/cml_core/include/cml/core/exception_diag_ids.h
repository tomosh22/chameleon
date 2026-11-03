#pragma once

#include <cstdint>

// Diagnostic ids of the broker side of crash -> patch -> continue (WP-20). ADR-0032 allocates CML5750..CML5799 to it (the agent side,
// CML4400..4449, is in agent/src/crash/diag_ids.h). docs/wp/WP-20.md repeats every id with its message and "what to do" text.
namespace cml::core::exceptions::diag {

inline constexpr std::uint32_t kBadArguments = 5750;      // exception-action / exceptions: a missing or mistyped argument
inline constexpr std::uint32_t kNoSuchException = 5751;   // exception-action: no such thread is held (answered already, or its process is gone)
inline constexpr std::uint32_t kAgentUnreachable = 5752;  // exception-action: the agent did not answer / the connection is gone
inline constexpr std::uint32_t kConfigRefused = 5753;     // the agent refused (or did not answer) the crash-handler configuration
inline constexpr std::uint32_t kAmbiguousException = 5755; // exception-action without `pid`: several processes hold a thread with that exception_id
inline constexpr std::uint32_t kIgnoredNoClient = 5754;   // (info) a held thread was released as Ignore because no client listens for exception events

}  // namespace cml::core::exceptions::diag
