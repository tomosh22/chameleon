#pragma once

// Failure injection, broker side (WP-28, ADR-0035; stages and ids in cml/ipc/fault_stages.h).
//
// `FaultInjector` holds which of the broker's pipeline stages are armed. The pipeline asks it at the entry of a stage
// (`hit`); an armed stage fails with its own diagnostic through the stage's normal error path, `count` times, then disarms
// itself. The agent's stages are armed in the agent (ArmFault); `inject_failure` below does both for the control command.
//
// Everything is compiled in under CML_FAULT_INJECTION (CMake option, default ON). Without it `hit` is a constant "no" and
// the commands do not exist (the capability `fault-injection` is not advertised).
//
// WP-44 (ADR-0043):
//   * Persistent arming. `inject-failure {stage, persist:true}` also writes the arming to a small JSON file next to the broker's
//     log (set_state_file; ServerOptions::fault_state_file). A broker that starts with such a file arms those stages again
//     (load_state). The file always mirrors what is armed with persist: a stage that fires, is disarmed at the end of its operation
//     or by {"stage":"none"} leaves it ({"stage":"none"} removes the file). An agent stage is armed in the agent process, which
//     outlives a broker restart by itself; what is persisted for it is the broker's record that the end of its operation must
//     disarm it there.
//   * R-51: the operations with an end-of-operation cleanup (restart, revert, exception-action) take `gate()` while they start, and
//     inject_failure holds it from its busy check to its last ArmFault, so an arming can never fall between an operation's start
//     and its end. Each operation runs its cleanup (finish_injection) before it lets go of its run lock.

#include <array>
#include <cstdint>
#include <map>
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
    // WP-44: `persist` also keeps the arming in the state file (CML5984 when there is none or it cannot be written; nothing is
    // armed then).
    Result<void> arm(ipc::fault::Stage stage, std::uint32_t count, std::optional<std::uint32_t> pid, bool persist = false);
    void disarm_all();
    // WP-44: disarms one broker stage (the group "exception" keeps one arming at a time).
    void disarm(ipc::fault::Stage stage);

    // The pipeline calls this when it reaches `stage` for process `pid`. Returns the injected failure (and uses up one
    // count) or nullopt.
    std::optional<Diag> hit(ipc::fault::Stage stage, std::uint32_t pid);

    // Remaining armed passes of a stage (tests, `inject-failure` result).
    std::uint32_t armed(ipc::fault::Stage stage) const;

    // WP-43: disarms the broker stages of `group` ("restart", "revert") armed for process `pid` or for every process, whether they fired
    // or not; returns the names of those that had not fired.
    std::vector<std::string> disarm_group(const char* group, std::uint32_t pid);
    // WP-43: which agent stages inject-failure armed in which process, so that the end of an operation can disarm them there.
    // `created`: the process's creation time (ProcessHost::creation_time), kept with its records (and in the state file) so that a later
    // registration with the same pid can tell whether it is the same process (records_belong_to).
    void note_agent_armed(ipc::fault::Stage stage, std::uint32_t pid, bool persist = false, std::optional<std::uint64_t> created = std::nullopt);
    bool take_agent_armed(ipc::fault::Stage stage, std::uint32_t pid);
    // R-51: process `pid` has exited (ReloadService::detach asks the process host): its records, persisted ones included, are dropped,
    // so a new process that reuses the pid never reports them.
    void forget_process(std::uint32_t pid);
    bool has_process_records(std::uint32_t pid) const;
    // WP-44: whether the records of `pid` were made for the process created at `created`: false when either time is unknown or they
    // differ (a reused pid). ReloadService drops them then, when an agent registers with that pid.
    bool records_belong_to(std::uint32_t pid, std::optional<std::uint64_t> created) const;
    // WP-44: {"stage":"none","pid":N}: the broker stages armed for N and N's agent records; armings without a pid (they apply to every
    // process) and those of other processes stay, and so do their persisted entries.
    void disarm_process(std::uint32_t pid);
    // WP-44: a broker stage armed for `pid`, or a record of it.
    bool armed_for_process(std::uint32_t pid) const;

    // R-51: see the top of the file. Taken by inject_failure (for a stage with an end-of-operation cleanup) and by the operations
    // while they start; never held while an operation runs.
    std::mutex& gate() noexcept { return gate_; }
    // WP-44: operations of a group that the broker counts itself (the group "exception-action"); callers hold gate() for begin.
    void begin_operation(const char* group);
    void end_operation(const char* group);
    bool operation_running(const char* group) const;

    // WP-44: persistent arming. set_state_file names the file (empty: none, `persist` is refused); load_state arms what the file
    // holds (a broken file is renamed aside and reported, CML5984). Both are called once, at start.
    void set_state_file(std::string path);
    bool has_state_file() const;
    std::vector<Diag> load_state();

    // The diagnostic an injected failure of `stage` carries.
    static Diag make_diag(ipc::fault::Stage stage);

private:
    struct Arming {
        std::uint32_t remaining = 0;
        std::optional<std::uint32_t> pid;
        bool persist = false;  // WP-44
    };
    // WP-44: writes the persisted armings (mutex_ held); a failure is logged (CML5984) and returned.
    std::optional<Diag> save_locked();
    bool any_persisted_locked() const;

    mutable std::mutex mutex_;
    std::mutex gate_;  // R-51
    std::array<Arming, ipc::fault::kStageCount> armed_{};
    std::array<std::set<std::uint32_t>, ipc::fault::kStageCount> agent_armed_{};  // WP-43
    std::array<std::set<std::uint32_t>, ipc::fault::kStageCount> agent_persist_{};  // WP-44: the records of agent_armed_ that persist
    std::map<std::uint32_t, std::uint64_t> created_;  // WP-44: pid -> creation time of the process its records were made for
    std::vector<std::pair<std::string, int>> operations_;  // WP-44: running operations per group
    std::string state_file_;  // WP-44
    bool state_written_ = false;  // the file exists (written or loaded by this broker)
};

// Control command `inject-failure` {stage, pid?, count?, persist?} -> {stage, armed, count, side, processes, persist}; {"stage":"none"} disarms
// every stage in the broker and in the agents (all, or `pid`'s). CML5960 for a bad request, CML5961 when no agent has that
// pid, CML5962 when the agent refuses.
Result<ipc::json::Value> inject_failure(ReloadService& service, const ipc::json::Value& args);

// WP-43: the end of a restart (group "restart") or a revert (group "revert") of process `pid`. Every stage of the group still armed for it is
// disarmed, in the broker and in the agent, whether it fired or not, so an arming never carries over to a later operation. `outcome` is the
// text of the operation's diagnostics (an agent stage whose id appears in it fired). Returns the names of the stages disarmed unused; the
// operation's answer lists them as `injection_disarmed`.
// `agent_alive` false: the process is gone (a restart terminated it): its agent armings are only forgotten.
std::vector<std::string> finish_injection(ReloadService& service, const char* group, std::uint32_t pid, const std::string& outcome, bool agent_alive = true);

// WP-44: `injection_disarmed` for an answer: the names as a JSON array (nothing is set when there are none).
void set_injection_disarmed(ipc::json::Value& result, const std::vector<std::string>& unused);

// Control command `fault-stages` -> {stages:[name...], details:[{name, side, id, where, group}]} in pipeline order (the reload's stages,
// then WP-43's restart and revert stages, then WP-44's crash-handler and watcher stages).
ipc::json::Value fault_stages();

}  // namespace cml::core
