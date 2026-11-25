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
#include <set>
#include <string>
#include <vector>

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

    // WP-43: disarms the broker stages of `group` ("restart", "revert") armed for process `pid` or for every process, whether they fired
    // or not; returns the names of those that had not fired.
    std::vector<std::string> disarm_group(const char* group, std::uint32_t pid);
    // WP-43: which agent stages inject-failure armed in which process, so that the end of an operation can disarm them there.
    void note_agent_armed(ipc::fault::Stage stage, std::uint32_t pid);
    bool take_agent_armed(ipc::fault::Stage stage, std::uint32_t pid);

    // The diagnostic an injected failure of `stage` carries.
    static Diag make_diag(ipc::fault::Stage stage);

private:
    struct Arming {
        std::uint32_t remaining = 0;
        std::optional<std::uint32_t> pid;
    };
    mutable std::mutex mutex_;
    std::array<Arming, ipc::fault::kStageCount> armed_{};
    std::array<std::set<std::uint32_t>, ipc::fault::kStageCount> agent_armed_{};  // WP-43
};

// Control command `inject-failure` {stage, pid?, count?} -> {stage, armed, count, side, processes}; {"stage":"none"} disarms
// every stage in the broker and in the agents (all, or `pid`'s). CML5960 for a bad request, CML5961 when no agent has that
// pid, CML5962 when the agent refuses.
Result<ipc::json::Value> inject_failure(ReloadService& service, const ipc::json::Value& args);

// WP-43: the end of a restart (group "restart") or a revert (group "revert") of process `pid`. Every stage of the group still armed for it is
// disarmed, in the broker and in the agent, whether it fired or not, so an arming never carries over to a later operation. `outcome` is the
// text of the operation's diagnostics (an agent stage whose id appears in it fired). Returns the names of the stages disarmed unused; the
// operation's answer lists them as `injection_disarmed`.
// `agent_alive` false: the process is gone (a restart terminated it): its agent armings are only forgotten.
std::vector<std::string> finish_injection(ReloadService& service, const char* group, std::uint32_t pid, const std::string& outcome, bool agent_alive = true);

// Control command `fault-stages` -> {stages:[name...], details:[{name, side, id, where, group}]} in pipeline order (the reload's stages,
// then WP-43's restart and revert stages).
ipc::json::Value fault_stages();

}  // namespace cml::core
