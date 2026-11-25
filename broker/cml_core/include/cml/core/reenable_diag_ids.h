#pragma once

#include <cstdint>

// Diagnostic ids of re-applying the session's edits to a module instance that is enabled after the session accepted edits for
// its sources (WP-45, ADR-0043): a module freed and loaded again (FreeLibrary + LoadLibrary), or a module a hot-restarted process
// enables only after the restart finished (CML5809). ADR-0043 allocates CML5600..CML5619. Message and "what to do" text live next
// to the code that raises each id (reload_service.cpp); docs/wp/WP-45.md lists them.
namespace cml::core::reenable::diag {

inline constexpr std::uint32_t kReapplyFailed = 5600;       // the reload that re-applies the edits failed (its diagnostic inside); the instance runs its original code
inline constexpr std::uint32_t kReapplyIndexFailed = 5601;  // the instance's PDB index failed, so its edits cannot be re-applied

}  // namespace cml::core::reenable::diag
