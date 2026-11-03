#pragma once

// Patch revert (WP-22): put the previous code back without unloading anything.
//
// A reload applies one patch per recompiled translation unit; the patches of one reload form a batch (ModuleState::
// batches). `revert` undoes batches, newest first: scope `last` the newest one, `all` every live batch.
//
// What happens for a batch:
//   * Hooks. Every function the batch hooked goes back to what the remaining (older) patches say: when an older
//     patch also hooked it, the jump is re-pointed at that patch's function (an InstallHooks request); when none did,
//     the original bytes come back (an InstallHooks request with the revert flag). Both use the agent's stop-the-world
//     installer, so nothing here allocates or locks while threads are stopped. The requests carry kInstallHooksQuiet /
//     kInstallHooksRevert: the application's pre/post-patch hooks do not run for a revert (docs/wp/WP-22.md).
//   * The patch DLLs stay loaded (a thread may still be inside their code) and keep their globals; globals the
//     reverted patch created stay alive and unused.
//   * Broker state goes back: the module's generation, the batch list, the symbols the patches added to the registry
//     (so a later reload does not bind to them), and the source baseline (the files stay edited on disk, so the next
//     reload recompiles them).
//
// Portable: the agent is behind AgentLink.

#include <cstdint>
#include <string>
#include <vector>

#include "cml/core/agent_link.h"
#include "cml/core/event_bus.h"
#include "cml/core/module_state.h"
#include "cml/util/result.h"

namespace cml::core {

enum class RevertScope { Last, All };

struct RevertOptions {
    std::uint32_t timeout_ms = 30000;  // per request to the agent (synchronised mode: until SyncPoint())
    EventBus* events = nullptr;        // `patch_reverted` is published here; may be null
};

struct RevertReport {
    std::uint32_t pid = 0;
    std::string module;
    std::uint64_t generation = 0;         // the module's generation after the revert
    std::uint64_t generation_before = 0;  // ... and before it
    std::size_t batches = 0;              // 0: the module had nothing to revert
    std::size_t patches = 0;
    std::size_t hooks_restored = 0;       // original bytes put back
    std::size_t hooks_repointed = 0;      // jump re-pointed at an older patch
    std::vector<Diag> warnings;
};

// Reverts the newest batch (Last) or all (All) of `module`. The caller serialises with reloads. On failure nothing
// has changed (a half-done revert is undone first, CML5854 when that is not possible).
Result<RevertReport> revert_module(AgentLink& link, ModuleState& module, RevertScope scope, const RevertOptions& options);

}  // namespace cml::core
