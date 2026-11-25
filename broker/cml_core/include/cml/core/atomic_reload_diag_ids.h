#pragma once

#include <cstdint>

// Diagnostic ids of the all-or-nothing reload of a module's compilands (WP-47, ADR-0044: CML3650..CML3669).
//
// A reload plans, links and verifies every recompiled compiland of a module before it applies any (pipeline.cpp). These ids say
// what became of the compilands that were NOT refused or failed themselves; the refusal or failure keeps its own id (CML32xx plan,
// CML33xx link, CML3400 thread_local, CML36xx proofs, the agent's CML42xx/43xx/46xx, ...). docs/wp/WP-47.md has the table.
namespace cml::core::atomic_reload::diag {

// (warning) One compiland of a multi-compiland reload was refused before anything was applied: none of the reload's compilands was
// applied (the others had been planned and linked, never loaded); the process runs what it ran before.
inline constexpr std::uint32_t kRefusedNothingApplied = 3650;
// (warning) Applying a compiland failed after earlier compilands of the same reload had been applied: those were rolled back (the
// WP-22 revert of the reload's batch); the process runs what it ran before.
inline constexpr std::uint32_t kRolledBack = 3651;
// (error) Applying a compiland failed and rolling back the compilands the reload had already applied failed as well: the process
// runs a part of the edit. The reload's batch stays recorded, so `revert` can try again.
inline constexpr std::uint32_t kRollbackFailed = 3652;
// (warning) The agent did not confirm the hooks of a compiland in time (CML5309), or could not take them out again (CML4656): that
// compiland may still become live and cannot be rolled back, so the reload stopped there and the compilands after it were not
// applied; the process may run a part of the edit.
inline constexpr std::uint32_t kStoppedPartly = 3653;

}  // namespace cml::core::atomic_reload::diag
