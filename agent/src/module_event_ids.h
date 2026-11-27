#pragma once

#include <cstdint>

// Diagnostic ids of the agent's module events (WP-48, ADR-0044). ADR-0044 names CML4700..CML4719 for the agent, but WP-36
// (ADR-0042) already owns that block (CML4700 is its fix-up writer's id, agent/src/loader/fixup_diag_ids.h), so these use the
// next free twenty, CML4720..CML4739 (to be confirmed by the orchestrator). All of them are warnings the agent logs; warnings
// are forwarded to the broker log. Message and "what to do" text live next to the code that logs them (agent.cpp);
// docs/TROUBLESHOOTING.md has the entries.
namespace cml::agent::module_events::ids {

// LdrRegisterDllNotification is not available or failed (or CML_TEST_NO_LOADER_NOTIFICATIONS is set): module changes are seen by
// the 500 ms module-list diff only, as before WP-48.
inline constexpr std::uint32_t kNotificationsUnavailable = 4720;
// The ring the loader notifications are recorded in was full: notifications were lost, so this report is completed from a
// module-list diff.
inline constexpr std::uint32_t kRingOverflow = 4721;
// The events and the process's module list disagreed in two polls in a row with nothing loaded or unloaded in between: the list
// is taken as the truth.
inline constexpr std::uint32_t kListDisagrees = 4722;
// A loaded module whose path was longer than a ring slot holds is gone again before the poll could read its full path: this
// report is completed from a module-list diff.
inline constexpr std::uint32_t kPathUnresolved = 4723;
// A hooked function's bytes are neither the agent's jump nor the function's original bytes (a debugger's breakpoint, another tool's
// detour), or cannot be read: no proof that the module was replaced, so the hook record is kept (review round 4).
inline constexpr std::uint32_t kForeignHookBytes = 4724;

}  // namespace cml::agent::module_events::ids
