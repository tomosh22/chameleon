#pragma once

// Failure injection, agent side (WP-28, ADR-0035; stages and ids in cml/ipc/fault_stages.h).
//
// The broker arms one of the agent's stages with ArmFault (control command `inject-failure`); the stage's code asks
// `consume(stage)` at the point that is made to fail. Rules that make it safe to ask anywhere, including inside the window in
// which the hook installer has every other thread suspended:
//   * the state is one lock-free atomic counter per stage with constant initialisation: no allocation, no lock, no CRT call;
//   * `consume` is noexcept and wait-free;
//   * the diagnostic text is built by the caller AFTER the threads run again.
//
// Without CML_FAULT_INJECTION `consume` is a constant false and ArmFault is answered with CML4655.

#include <cstdint>

#include "cml/ipc/fault_stages.h"
#include "cml/ipc/messages.h"

namespace cml::agent::fault {

#if defined(CML_FAULT_INJECTION)
inline constexpr bool kBuilt = true;
#else
inline constexpr bool kBuilt = false;
#endif

// True once per armed pass of `stage` (and uses it up). Any thread, any lock state.
bool consume(ipc::fault::Stage stage) noexcept;

// Arms `stage` for its next `count` passes (replacing the previous arming); count 0 disarms it.
void arm(ipc::fault::Stage stage, std::uint32_t count) noexcept;
void disarm_all() noexcept;
std::uint32_t armed(ipc::fault::Stage stage) noexcept;

// ArmFault: true for the message handled here.
bool is_arm_request(const ipc::Message& message) noexcept;
// The Reply to an ArmFault: status 0 when armed (or disarmed by stage "none"), CML4654 for an unknown stage, a stage of the
// broker, or a bad count, CML4655 when the agent is built without injection.
ipc::Message handle_arm_request(const ipc::Message& message);

}  // namespace cml::agent::fault
