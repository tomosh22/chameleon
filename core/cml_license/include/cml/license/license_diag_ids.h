#pragma once

#include <cstdint>

// Diagnostic ids of the licensing hook (WP-32). ADR-0038 reserves CML5998..CML5999 for it. Chameleon ships with a
// no-op implementation that always allows, so neither id is raised by the open build; they exist for a commercial
// build that plugs in its own cml::license::Provider.
namespace cml::license::diag {

inline constexpr std::uint32_t kLicenseRefused = 5998;  // the licence check completed and says this installation may not run
inline constexpr std::uint32_t kLicenseCheckFailed = 5999;  // the licence check could not be completed (no answer, unreadable licence file)

}  // namespace cml::license::diag
