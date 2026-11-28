#pragma once

#include <cstdint>

// Diagnostic ids of the broker's handling of the agent's module events (WP-48, ADR-0044 allocates CML5620..CML5629). Both are
// warnings: the connection thread logs them, and publishes CML5620 as a `diag` event (stage "modules"). Message and "what to do"
// text live next to the code that raises each id (session.cpp); docs/TROUBLESHOOTING.md has the entries.
namespace cml::core::module_events::diag {

// The agent reported (some of) its module changes from a module-list diff instead of the loader's notifications (its ring
// overflowed, a path could not be resolved, or its list and the process's disagreed): a module of the process freed and loaded
// again at the same base in that time is not seen, and runs its original code.
inline constexpr std::uint32_t kModuleEventsResync = 5620;
// The agent announced a wire ABI before 3 (agent_abi_version in Hello): it reports module changes from a module-list diff every
// 500 ms, so a module freed and loaded again in between is not seen. Everything else works as before.
inline constexpr std::uint32_t kAgentWithoutModuleEvents = 5621;

}  // namespace cml::core::module_events::diag
