#pragma once

#include <cstdint>

// Diagnostic ids of the patch revert (WP-22). ADR-0032 allocates CML5850..CML5899 to the broker side. Message and
// "what to do" text live next to the code that raises each id; docs/wp/WP-22.md lists them.
namespace cml::core::revert::diag {

inline constexpr std::uint32_t kNothingToRevert = 5850;    // no live patch in the targeted modules
inline constexpr std::uint32_t kRevertRefused = 5851;      // the agent refused the hook changes (its id inside); nothing changed
inline constexpr std::uint32_t kRevertUnconfirmed = 5852;  // (warning) no HooksInstalled in time; the agent may still apply the change
inline constexpr std::uint32_t kBadScope = 5853;           // `scope` is neither "last" nor "all"
inline constexpr std::uint32_t kRevertPartial = 5854;      // several modules: some were reverted, others failed (n of m)
inline constexpr std::uint32_t kUncommitFailed = 5855;     // (warning) the agent did not un-commit a reverted patch (older agent?): its application hooks may keep running
inline constexpr std::uint32_t kModuleGone = 5856;         // the module is no longer loaded where the patches were made; the stale patch list was dropped
inline constexpr std::uint32_t kRevertMixed = 5857;        // the second half of a revert failed and the first could not be undone
inline constexpr std::uint32_t kRevertTimeout = 5858;      // the revert ran out of time before a step

}  // namespace cml::core::revert::diag
