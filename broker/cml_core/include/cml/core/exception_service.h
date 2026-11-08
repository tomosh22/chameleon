#pragma once

// Crash -> patch -> continue, broker side (plan section 4.11, WP-20, docs/wp/WP-20.md).
//
// The agent holds a thread that faulted in an enabled module (or a patch) and tells the broker with `Exception`. This service
//   * turns that into the control-pipe event
//       {"event":"exception","pid","tid","exception_id","code":"0xC0000005","address","module","function","offset",
//        "actions":["continue","leave","ignore"], ...}            (function name resolved from the module's PDB index)
//     and remembers the held thread as pending. WP-23: `actions` holds "reexecute" only while it is offered; `reasons.reexecute`
//     says why not otherwise (CML59xx). When a patch (or a revert) changes what is offered, the event
//       {"event":"exception_updated","pid","exception_id","actions":[...],"reasons":{...}}
//     follows (published under the subscription name `exception`);
//   * answers the control commands `exception-action` {pid, exception_id, action} (sends ExceptionAction to the agent and waits for
//     its verdict; a refusal such as "cannot unwind" keeps the thread pending so the client can choose another action) and
//     `exceptions` (the pending threads, each in the event's shape);
//   * decides whether agents hold faulting threads at all (ExceptionConfig): only while a control client listens for the
//     `exception` event AND the option `crash_handler` is on (default on). When the last listener goes away or the option is
//     turned off, the agents release every held thread as Ignore and the pending list is cleared. Without a listener a fault is
//     therefore never held: an application that handles faults itself is not disturbed unless a client asked to see them.
//
// Threads: on_exception / agent_ready / agent_gone run on an agent's connection thread and never wait for the agent;
// action / pending / refresh run on control connection threads (refresh may wait for the agents' answers).

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "cml/core/agent_link.h"
#include "cml/core/event_bus.h"
#include "cml/core/reexecute.h"
#include "cml/core/reload_service.h"
#include "cml/ipc/json.h"
#include "cml/ipc/messages.h"
#include "cml/util/result.h"

namespace cml::core {

class ExceptionService {
public:
    // `service` and `events` must outlive this object.
    ExceptionService(ReloadService* service, EventBus* events);
    ~ExceptionService();
    ExceptionService(const ExceptionService&) = delete;
    ExceptionService& operator=(const ExceptionService&) = delete;

    // ---- agent connections (connection thread) ----
    // The agent finished its handshake: remember its link; if holding is on, tell the agent so (fire and forget).
    void agent_ready(const std::shared_ptr<AgentLink>& link);
    // The connection ended: forget the link and its pending threads.
    void agent_gone(std::uint64_t agent_id);
    // The agent holds a thread. `pid` is the process id the agent announced in Hello.
    void on_exception(const std::shared_ptr<AgentLink>& link, std::uint32_t pid, const ipc::Exception& exception);

    // ---- control commands (control thread) ----
    // {"pid"?:N, "exception_id":N, "action":"continue"|"leave"|"ignore"|"reexecute"}  ->  {pid, exception_id, action}
    // (WP-23: "reexecute" is refused with CML5908 while it is not offered.)
    Result<ipc::json::Value> action(const ipc::json::Value& args);
    // {"pid"?:N}  ->  {"pending":[<exception event bodies>]}
    Result<ipc::json::Value> pending(const ipc::json::Value& args) const;

    // Re-evaluates "should the agents hold faulting threads" (a client subscribed / left, an option changed) and, only when the
    // answer differs from what the agents were last told, tells every agent (in parallel). `wait`: wait for the agents'
    // acknowledgements (so the caller knows the setting is in effect); otherwise fire and forget (teardown paths).
    //
    // Ordering: refresh() and agent_ready() hold `config_mutex_` from computing the configuration to sending it, so two
    // configurations can never cross on the wire and the last one sent is the one computed last, i.e. from the newest state of the
    // event bus and the options. An agent is registered (and told) only under that mutex, after its Welcome went out.
    void refresh(bool wait);

    // Is holding on right now (option on and somebody listens)?
    bool active() const;

    std::size_t pending_count() const;

    // WP-23 test seam: replaces reexecute::evaluate (which reads PDBs of enabled modules).
    using Evaluator = std::function<reexecute::Offer(const std::vector<std::shared_ptr<ModuleState>>&, const ipc::Exception&, const reexecute::HookMap&)>;
    void set_evaluator_for_testing(Evaluator evaluator);

private:
    struct Key {
        std::uint64_t agent_id = 0;
        std::uint32_t exception_id = 0;
        bool operator<(const Key& other) const {
            return agent_id != other.agent_id ? agent_id < other.agent_id : exception_id < other.exception_id;
        }
    };
    struct Pending {
        std::uint32_t pid = 0;
        std::shared_ptr<AgentLink> link;
        ipc::json::Value body;  // the event body
        // WP-23
        ipc::Exception raw;                 // what the agent reported (the stack)
        reexecute::HookMap hooks_at_fault;  // the hook table when the fault was reported
        reexecute::Offer offer;             // what is offered now
    };
    class PatchListener;

    ipc::ExceptionConfig make_config() const;
    ipc::json::Value describe(const std::shared_ptr<AgentLink>& link, std::uint32_t pid, const ipc::Exception& exception, const reexecute::Offer& offer) const;
    // WP-23: the enabled modules of the agent's process.
    std::vector<std::shared_ptr<ModuleState>> modules_of(const std::shared_ptr<AgentLink>& link) const;
    // WP-23: a patch was applied or reverted in process `pid`: re-evaluate what is offered for its held threads.
    void on_patch_event(const Event& event);
    static void send_ignore(const std::shared_ptr<AgentLink>& link, std::uint32_t exception_id);

    ReloadService* service_;
    EventBus* events_;
    std::mutex config_mutex_;  // serialises "compute the configuration + send it" (refresh, agent_ready); taken before mutex_
    std::uint32_t applied_flags_ = 0;  // config_mutex_: what the agents were last told
    mutable std::mutex mutex_;  // links_, pending_, holding_
    bool holding_ = false;      // the agents may hold threads; set together with clearing pending_ (under mutex_)
    std::map<std::uint64_t, std::shared_ptr<AgentLink>> links_;
    std::map<Key, Pending> pending_;
    reexecute::Offer evaluate(const std::vector<std::shared_ptr<ModuleState>>& modules, const ipc::Exception& exception, const reexecute::HookMap& at_fault) const;
    mutable std::mutex evaluator_mutex_;
    Evaluator evaluator_;
    std::shared_ptr<PatchListener> patch_listener_;
    EventBus::Id patch_subscription_ = 0;
};

}  // namespace cml::core
