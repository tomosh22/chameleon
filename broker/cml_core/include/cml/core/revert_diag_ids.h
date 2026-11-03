#pragma once

#include <cstdint>

// Diagnostic ids of the patch revert (WP-22). ADR-0032 allocates CML5850..CML5899 to the broker side. Message and
// "what to do" text live next to the code that raises each id; docs/wp/WP-22.md lists them.
namespace cml::core::revert::diag {

inline constexpr std::uint32_t kNothingToRevert = 5850;    // no live patch in the targeted modules
inline constexpr std::uint32_t kRevertRefused = 5851;      // the agent refused the hook changes (its id inside); nothing changed
inline constexpr std::uint32_t kRevertUnconfirmed = 5852;  // (warning) no HooksInstalled in time; the agent may still apply the change
inline constexpr std::uint32_t kBadScope = 5853;           // `scope` is neither "last" nor "all"
inline constexpr std::uint32_t kRevertPartial = 5854;      // the second half of a revert failed; the first could not be undone

}  // namespace cml::core::revert::diag
