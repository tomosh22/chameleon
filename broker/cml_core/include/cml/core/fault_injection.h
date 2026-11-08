#pragma once

// Failure injection, broker side (WP-28, ADR-0035; stages and ids in cml/ipc/fault_stages.h).
//
// `FaultInjector` holds which of the broker's pipeline stages are armed. The pipeline asks it at the entry of a stage
// (`hit`); an armed stage fails with its own diagnostic through the stage's normal error path, `count` times, then disarms
// itself. The agent's stages are armed in the agent (ArmFault); `inject_failure` below does both for the control command.
//
// Everything is compiled in under CML_FAULT_INJECTION (CMake option, default ON). Without it `hit` is a constant "no" and
// the commands do not exist (the capability `fault-injection` is not advertised).

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>

#include "cml/ipc/fault_stages.h"
#include "cml/ipc/json.h"
#include "cml/util/diag.h"
#include "cml/util/result.h"

namespace cml::core {

#if defined(CML_FAULT_INJECTION)
inline constexpr bool kFaultInjectionBuilt = true;
#else
inline constexpr bool kFaultInjectionBuilt = false;
#endif

class ReloadService;

class FaultInjector {
public:
    // Arms a broker-side stage for its next `count` passes (of process `pid` only when given). Re-arming replaces the previous
    // arming of that stage. A stage of the agent is a bad argument here (arm it with ArmFault).
    Result<void> arm(ipc::fault::Stage stage, std::uint32_t count, std::optional<std::uint32_t> pid);
    void disarm_all();

    // The pipeline calls this when it reaches `stage` for process `pid`. Returns the injected failure (and uses up one
    // count) or nullopt.
    std::optional<Diag> hit(ipc::fault::Stage stage, std::uint32_t pid);

    // Remaining armed passes of a stage (tests, `inject-failure` result).
    std::uint32_t armed(ipc::fault::Stage stage) const;

    // The diagnostic an injected failure of `stage` carries.
    static Diag make_diag(ipc::fault::Stage stage);

private:
    struct Arming {
        std::uint32_t remaining = 0;
        std::optional<std::uint32_t> pid;
    };
    mutable std::mutex mutex_;
    std::array<Arming, ipc::fault::kStageCount> armed_{};
};

// Control command `inject-failure` {stage, pid?, count?} -> {stage, armed, count, side, processes}; {"stage":"none"} disarms
// every stage in the broker and in the agents (all, or `pid`'s). CML5960 for a bad request, CML5961 when no agent has that
// pid, CML5962 when the agent refuses.
Result<ipc::json::Value> inject_failure(ReloadService& service, const ipc::json::Value& args);

// Control command `fault-stages` -> {stages:[name...], details:[{name, side, id, where}]} in pipeline order.
ipc::json::Value fault_stages();

}  // namespace cml::core
