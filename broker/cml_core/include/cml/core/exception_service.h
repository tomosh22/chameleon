#pragma once

// Crash -> patch -> continue, broker side (plan section 4.11, WP-20, docs/wp/WP-20.md).
//
// The agent holds a thread that faulted in an enabled module (or a patch) and tells the broker with `Exception`. This service
//   * turns that into the control-pipe event
//       {"event":"exception","pid","tid","exception_id","code":"0xC0000005","address","module","function","offset",
//        "actions":["continue","leave","ignore"], ...}            (function name resolved from the module's PDB index)
//     and remembers the held thread as pending;
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
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "cml/core/agent_link.h"
#include "cml/core/event_bus.h"
#include "cml/core/reload_service.h"
#include "cml/ipc/json.h"
#include "cml/ipc/messages.h"
#include "cml/util/result.h"

namespace cml::core {

class ExceptionService {
public:
    // `service` and `events` must outlive this object.
    ExceptionService(ReloadService* service, EventBus* events);

    // ---- agent connections (connection thread) ----
    // The agent finished its handshake: remember its link; if holding is on, tell the agent so (fire and forget).
    void agent_ready(const std::shared_ptr<AgentLink>& link);
    // The connection ended: forget the link and its pending threads.
    void agent_gone(std::uint64_t agent_id);
    // The agent holds a thread. `pid` is the process id the agent announced in Hello.
    void on_exception(const std::shared_ptr<AgentLink>& link, std::uint32_t pid, const ipc::Exception& exception);

    // ---- control commands (control thread) ----
    // {"pid"?:N, "exception_id":N, "action":"continue"|"leave"|"ignore"}  ->  {pid, exception_id, action}
    Result<ipc::json::Value> action(const ipc::json::Value& args);
    // {"pid"?:N}  ->  {"pending":[<exception event bodies>]}
    Result<ipc::json::Value> pending(const ipc::json::Value& args) const;

    // Re-evaluates "should the agents hold faulting threads" (a client subscribed / left, an option changed) and tells every
    // agent. `wait`: wait for each agent's acknowledgement (so the caller knows the setting is in effect); otherwise fire and
    // forget (teardown paths).
    void refresh(bool wait);

    // Is holding on right now (option on and somebody listens)?
    bool active() const;

    std::size_t pending_count() const;

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
    };

    ipc::ExceptionConfig make_config() const;
    ipc::json::Value describe(const std::shared_ptr<AgentLink>& link, std::uint32_t pid, const ipc::Exception& exception) const;
    static void send_ignore(const std::shared_ptr<AgentLink>& link, std::uint32_t exception_id);

    ReloadService* service_;
    EventBus* events_;
    mutable std::mutex mutex_;  // links_, pending_
    std::map<std::uint64_t, std::shared_ptr<AgentLink>> links_;
    std::map<Key, Pending> pending_;
};

}  // namespace cml::core
