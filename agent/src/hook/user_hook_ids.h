#pragma once

#include <cstdint>

// Diagnostic ids of the application hooks (WP-17, ADR-0008: CML5650-5699). docs/wp/WP-17.md repeats every
// id with its message and "what to do" text.
namespace cml::agent::user_hooks::ids {

inline constexpr std::uint32_t kEntryRejected = 5650;      // a hook record points outside its image: skipped
inline constexpr std::uint32_t kSectionUnreadable = 5651;  // the hook section of an image cannot be read: skipped
inline constexpr std::uint32_t kHookFaulted = 5652;        // an application hook raised a processor exception
inline constexpr std::uint32_t kTooManyHooks = 5653;       // more hook records than the per-image limit: the rest are skipped
inline constexpr std::uint32_t kDestroyInHook = 5655;     // DestroyAgent called from inside an application hook: refused
inline constexpr std::uint32_t kWaitInHook = 5656;        // a blocking wait called from a hook on the agent thread: refused
inline constexpr std::uint32_t kStackNotRestored = 5657;  // a hook overflowed the stack and the guard page could not be restored
inline constexpr std::uint32_t kOutOfMemory = 5654;        // no memory to prepare the hook run: the hooks of this event are skipped

}  // namespace cml::agent::user_hooks::ids
