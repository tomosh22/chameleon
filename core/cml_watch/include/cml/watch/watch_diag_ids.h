#pragma once

#include <cstdint>

// Diagnostic ids of change detection (WP-14). ADR-0008 allocates CML5500..CML5549 to it.
namespace cml::watch::diag {

inline constexpr std::uint32_t kRootNotWatched = 5500;    // (warning) a directory cannot be watched
inline constexpr std::uint32_t kOverflow = 5501;          // (warning) events were lost; every tracked file is re-checked
inline constexpr std::uint32_t kUnsupported = 5502;       // directory watching is not available on this host
inline constexpr std::uint32_t kWatchFailed = 5503;       // the watcher stopped working (the directory went away, I/O error)
inline constexpr std::uint32_t kNothingToWatch = 5504;    // (info) compile-on-save is on but no source directory is known yet
inline constexpr std::uint32_t kAutoReloadFailed = 5505;  // (warning) the reload started by a save failed (its own diag was published)

}  // namespace cml::watch::diag
