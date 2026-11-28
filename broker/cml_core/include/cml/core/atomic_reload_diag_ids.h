#pragma once

#include <cstdint>

// Diagnostic ids of the all-or-nothing reload of a module's compilands (WP-47, ADR-0044: CML3650..CML3655) and of the modules of one
// process (WP-54, ADR-0045: CML3656..CML3669).
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
// (error) Applying a compiland failed and the roll-back of the compilands applied before it was sent but not confirmed: the agent did
// not confirm the hook changes in time (CML5852, synchronised mode) or did not un-commit the rolled-back images (CML5855). The broker's
// state follows the roll-back (as `revert` does in that case); the process may run a part of the edit until the agent applies it.
inline constexpr std::uint32_t kRollbackUnconfirmed = 3654;
// (warning) The agent disconnected while a reload of several compilands was being applied: the compilands applied before stay
// recorded (nothing could be rolled back), the others were not applied, and none of their files is re-baselined.
inline constexpr std::uint32_t kAgentLostPartly = 3655;

// ---- WP-54 (ADR-0045, R-56): the modules of one process (process_reload.cpp) ----------------------------------------------------
// A reload of several modules of one process (an exe and its DLLs) prepares every module before it applies any. These ids say what
// became of the OTHER modules of the process; the refusal or failure keeps its own id (and its CML365x above, if any). Different
// processes stay independent.

// (warning) A module of the process was refused while the reload was being prepared: no module of that process was applied (the
// modules before it were planned and linked, never loaded; the ones after it were not prepared); the process runs what it ran before.
inline constexpr std::uint32_t kModuleRefusedNothingApplied = 3656;
// (warning) Applying a module failed after other modules of the same process had been applied by this reload: those were rolled back
// (the WP-22 revert of this reload's batch, per module, newest first); the process runs what it ran before.
inline constexpr std::uint32_t kModulesRolledBack = 3657;
// (error) Applying a module failed and rolling back a module of the same process that this reload had applied failed too: the process
// runs a part of the edit. That module's batch stays recorded, so `revert` can try again.
inline constexpr std::uint32_t kModulesRollbackFailed = 3658;
// (error) Applying a module failed and the roll-back of a module applied before it was sent but not confirmed (CML5852, CML5855): the
// broker's state follows the roll-back; the process may run a part of the edit until the agent applies it.
inline constexpr std::uint32_t kModulesRollbackUnconfirmed = 3659;
// (warning) A module's patch may still become live (CML5309 / CML4656, see CML3653): nothing is rolled back, the modules applied before
// it stay applied and the ones after it are not applied; the process may run a part of the edit.
inline constexpr std::uint32_t kModulesStoppedPartly = 3660;
// (warning) The agent disconnected while the modules of the process were being applied: the modules applied before stay recorded
// (nothing could be rolled back), none of their files is re-baselined and nothing of them is announced; the others were not applied.
inline constexpr std::uint32_t kModulesAgentLost = 3661;

}  // namespace cml::core::atomic_reload::diag
