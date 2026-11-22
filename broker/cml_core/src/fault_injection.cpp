#include "cml/core/fault_injection.h"

#include <mutex>
#include <string>
#include <vector>

#include "cml/core/agent_link.h"
#include "cml/core/module_state.h"
#include "cml/core/reload_service.h"
#include "cml/util/log.h"

namespace cml::core {

namespace fault = ipc::fault;
using ipc::json::Value;

Diag FaultInjector::make_diag(fault::Stage stage) {
    const fault::StageInfo& info = fault::info(stage);
    return Diag::make(info.diag_id, std::string("injected failure at the stage '") + info.name + "' (" + info.where + ")",
                      "This failure was requested with the control command inject-failure; it fires once per armed pass. Reload again: without it the reload succeeds.");
}

Result<void> FaultInjector::arm(fault::Stage stage, std::uint32_t count, std::optional<std::uint32_t> pid) {
    const fault::StageInfo& info = fault::info(stage);
#if !defined(CML_FAULT_INJECTION)
    (void)count;
    (void)pid;
    return Diag::make(fault::ids::kBadRequest, std::string("this broker was built without CML_FAULT_INJECTION (stage '") + info.name + "')",
                      "Use a build with failure injection (the default).");
#else
    if (info.agent_side) {
        return Diag::make(fault::ids::kBadRequest, std::string("the stage '") + info.name + "' belongs to the agent",
                          "Arm it through the control command inject-failure, which sends it to the agent.");
    }
    if (count == 0 || count > fault::kMaxCount) {
        return Diag::make(fault::ids::kBadRequest, "count must be 1 to " + std::to_string(fault::kMaxCount), "Send a smaller count.");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    armed_[static_cast<std::size_t>(stage)] = Arming{count, pid};
    return Result<void>();
#endif
}

void FaultInjector::disarm_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    armed_ = {};
}

std::optional<Diag> FaultInjector::hit(fault::Stage stage, std::uint32_t pid) {
#if !defined(CML_FAULT_INJECTION)
    (void)stage;
    (void)pid;
    return std::nullopt;
#else
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Arming& a = armed_[static_cast<std::size_t>(stage)];
        if (a.remaining == 0 || (a.pid && *a.pid != pid)) {
            return std::nullopt;
        }
        --a.remaining;
    }
    const Diag d = make_diag(stage);
    CML_LOG(Warn, "failure injection: %s", d.format().c_str());
    return d;
#endif
}

std::uint32_t FaultInjector::armed(fault::Stage stage) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return armed_[static_cast<std::size_t>(stage)].remaining;
}

Value fault_stages() {
    Value stages = Value::array();
    Value details = Value::array();
    for (const fault::StageInfo& s : fault::kStages) {
        stages.push(Value::string(s.name));
        Value d = Value::object();
        d.set("name", Value::string(s.name));
        d.set("side", Value::string(s.agent_side ? "agent" : "broker"));
        d.set("id", Value::string("CML" + std::to_string(s.diag_id)));
        d.set("where", Value::string(s.where));
        details.push(std::move(d));
    }
    Value result = Value::object();
    result.set("stages", std::move(stages));
    result.set("details", std::move(details));
    return result;
}

namespace {

// Sends ArmFault to one agent and turns a refusal into CML5962.
Result<void> arm_agent(AgentLink& link, const std::string& stage, std::uint32_t count) {
    ipc::ArmFault request;
    request.request_id = link.next_request_id();
    request.stage = stage;
    request.count = count;
    const std::uint32_t pid = link.snapshot().pid;
    Result<ipc::Message> answer = link.request(ipc::Message(std::move(request)), 10000);
    if (!answer) {
        return Diag::make(fault::ids::kArmRefused, "the agent of process " + std::to_string(pid) + " did not answer ArmFault: " + answer.error().message,
                          "Check that the process is still running.");
    }
    const auto* reply = std::get_if<ipc::Reply>(&*answer);
    if (reply == nullptr || reply->status != 0) {
        return Diag::make(fault::ids::kArmRefused,
                          "the agent of process " + std::to_string(pid) + " refused to arm '" + stage + "': " +
                              (reply != nullptr ? "CML" + std::to_string(reply->status) + " " + reply->message : std::string("unexpected answer")),
                          "Use an agent built with CML_FAULT_INJECTION from the same build as the broker.");
    }
    return Result<void>();
}

}  // namespace

namespace {

// True when one of the process's enabled modules has not reserved address space yet (the stage `reservation` can fire).
bool has_unreserved_module(ReloadService& service, const AgentLink& link) {
    for (const std::shared_ptr<ModuleState>& m : service.module_states(link.agent_id())) {
        std::lock_guard<std::mutex> lock(m->mutex);
        if (!m->reservation.reserved()) {
            return true;
        }
    }
    return false;
}

std::string pid_list(const std::vector<std::uint32_t>& pids) {
    std::string text;
    for (const std::uint32_t pid : pids) {
        text += (text.empty() ? "" : ", ") + std::to_string(pid);
    }
    return text;
}

}  // namespace

Result<Value> inject_failure(ReloadService& service, const Value& args) {
    const Value* stage_value = args.find("stage");
    if (stage_value == nullptr || !stage_value->is_string()) {
        return Diag::make(fault::ids::kBadRequest, "inject-failure needs a string 'stage'", "Send {\"stage\": <name>} with a name from fault-stages, or \"none\".");
    }
    const std::string& stage_name = stage_value->as_string();
    std::optional<std::uint32_t> pid;
    if (const Value* p = args.find("pid"); p != nullptr && !p->is_null()) {
        if (!p->is_integer() || p->as_integer() <= 0 || p->as_integer() > 0xFFFFFFFFll) {
            return Diag::make(fault::ids::kBadRequest, "'pid' must be a positive integer", "Send the process id of an application with an agent.");
        }
        pid = static_cast<std::uint32_t>(p->as_integer());
    }
    std::uint32_t count = 1;
    if (const Value* c = args.find("count"); c != nullptr && !c->is_null()) {
        if (!c->is_integer() || c->as_integer() < 1 || c->as_integer() > static_cast<std::int64_t>(fault::kMaxCount)) {
            return Diag::make(fault::ids::kBadRequest, "'count' must be an integer from 1 to " + std::to_string(fault::kMaxCount), "Send a smaller count.");
        }
        count = static_cast<std::uint32_t>(c->as_integer());
    }

    // The agents this command reaches.
    std::vector<std::shared_ptr<AgentLink>> targets;
    if (pid) {
        if (std::shared_ptr<AgentLink> link = service.link_for_pid(*pid)) {
            targets.push_back(std::move(link));
        }
    } else {
        for (std::shared_ptr<AgentLink>& link : service.links()) {
            if (link && !link->closed() && link->snapshot().handshaken) {
                targets.push_back(std::move(link));
            }
        }
    }
    // A pid that matches no agent is refused before anything is changed (also for "none" and for the broker's stages).
    if (pid && targets.empty()) {
        return Diag::make(fault::ids::kNoSuchProcess, "no agent is connected for process " + std::to_string(*pid), "Query `status` for the connected processes.");
    }

    Value result = Value::object();
    Value processes = Value::array();

    if (stage_name == fault::kDisarm) {
        // Disarm everything that can be disarmed, then say which agents could not be.
        service.faults().disarm_all();
        std::vector<std::uint32_t> failed;
        std::string reasons;
        for (const std::shared_ptr<AgentLink>& link : targets) {
            if (Result<void> r = arm_agent(*link, fault::kDisarm, 1); !r) {
                failed.push_back(link->snapshot().pid);
                reasons += (reasons.empty() ? "" : "; ") + r.error().message;
            } else {
                processes.push(Value::integer(link->snapshot().pid));
            }
        }
        if (!failed.empty()) {
            return Diag::make(fault::ids::kPartialDisarm,
                              "the broker's stages are disarmed, but the agents of process(es) " + pid_list(failed) + " could not be disarmed: " + reasons,
                              "Check that those processes are still running, then send {\"stage\":\"none\"} again.");
        }
        result.set("stage", Value::string(fault::kDisarm));
        result.set("armed", Value::boolean(false));
        result.set("processes", std::move(processes));
        return result;
    }

    const fault::StageInfo* info = fault::find_stage(stage_name);
    if (info == nullptr) {
        std::string known;
        for (const fault::StageInfo& s : fault::kStages) {
            known += (known.empty() ? "" : ", ") + std::string(s.name);
        }
        return Diag::make(fault::ids::kBadRequest, "unknown failure-injection stage '" + stage_name + "'",
                          "Use one of: " + known + " (see fault-stages), or \"none\" to disarm.");
    }
    if (info->agent_side) {
        if (targets.empty()) {
            return Diag::make(fault::ids::kNoSuchProcess, "no agent is connected", "Start the application with the agent and wait for `status` to list it.");
        }
        if (info->stage == fault::Stage::Reservation) {
            // A module reserves address space once, on its first patch: an armed counter would fire on an unrelated later reload.
            for (const std::shared_ptr<AgentLink>& link : targets) {
                if (!has_unreserved_module(service, *link)) {
                    return Diag::make(fault::ids::kNotApplicable,
                                      "the stage 'reservation' fires only on a module's first patch, and every enabled module of process " + std::to_string(link->snapshot().pid) +
                                          " has reserved its address space already (or none is enabled)",
                                      "Arm it right after the module is enabled and before its first reload, or start a new process.");
                }
            }
        }
        // All or nothing: the agents already armed are disarmed again when one of them refuses.
        std::vector<std::shared_ptr<AgentLink>> armed;
        for (const std::shared_ptr<AgentLink>& link : targets) {
            if (Result<void> r = arm_agent(*link, stage_name, count); !r) {
                for (const std::shared_ptr<AgentLink>& done : armed) {
                    (void)arm_agent(*done, stage_name, 0);  // count 0: disarm this stage only
                }
                return r.error();  // names the pid that refused
            }
            armed.push_back(link);
            processes.push(Value::integer(link->snapshot().pid));
        }
    } else {
        if (Result<void> r = service.faults().arm(info->stage, count, pid); !r) {
            return r.error();
        }
    }
    result.set("stage", Value::string(stage_name));
    result.set("armed", Value::boolean(true));
    result.set("count", Value::integer(count));
    result.set("side", Value::string(info->agent_side ? "agent" : "broker"));
    result.set("id", Value::string("CML" + std::to_string(info->diag_id)));
    result.set("processes", std::move(processes));
    return result;
}

}  // namespace cml::core
