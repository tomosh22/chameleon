#pragma once

#include <cstdint>

// Diagnostic ids of the agent's side of the hot-restart (WP-21, ADR-0032: CML4450-4499). The broker's ids (CML5800-5849)
// are in broker/cml_core/include/cml/core/restart_diag_ids.h. docs/wp/WP-21.md repeats every id with its text.
namespace cml::agent::restart::ids {

inline constexpr std::uint32_t kNoLaunchInfo = 4450;     // the command line, directory or environment could not be captured at start-up
inline constexpr std::uint32_t kLaunchInfoTooLarge = 4451;  // command line + environment do not fit one message
inline constexpr std::uint32_t kOutOfMemory = 4452;      // no memory to prepare the restart
inline constexpr std::uint32_t kReenableRefused = 4453;  // a module could not be queued for enabling again after a restart

}  // namespace cml::agent::restart::ids
