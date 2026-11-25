#include "cml/core/fault_injection.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "cml/core/agent_link.h"
#include "cml/core/module_state.h"
#include "cml/core/reload_service.h"
#include "cml/util/log.h"

namespace cml::core {

namespace fault = ipc::fault;
using ipc::json::Value;

namespace {

// WP-44: the groups whose operations disarm what is still armed for them at their end (finish_injection). An arming of such a group
// is recorded and its operation's start is ordered against it (FaultInjector::gate()).
bool has_cleanup(const std::string& group) {
    return group == "restart" || group == "revert" || group == "exception-action" || group == "watch";
}

}  // namespace

Diag FaultInjector::make_diag(fault::Stage stage) {
    const fault::StageInfo& info = fault::info(stage);
    const std::string group = info.group;
    const char* again = group == "restart"            ? "Restart again: without it the restart succeeds."
                        : group == "revert"           ? "Revert again: without it the revert succeeds."
                        : group == "exception"        ? "The next fault is held and reported as usual."
                        : group == "exception-action" ? "Send the action again: without it the action is carried out."
                        : group == "watch"            ? "Save again, or run `reload`: without it the save reloads."
                                                      : "Reload again: without it the reload succeeds.";
    return Diag::make(info.diag_id, std::string("injected failure at the stage '") + info.name + "' (" + info.where + ")",
                      std::string("This failure was requested with the control command inject-failure; it fires once per armed pass. ") + again);
}

Result<void> FaultInjector::arm(fault::Stage stage, std::uint32_t count, std::optional<std::uint32_t> pid, bool persist) {
    const fault::StageInfo& info = fault::info(stage);
#if !defined(CML_FAULT_INJECTION)
    (void)count;
    (void)pid;
    (void)persist;
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
    if (std::string(info.group) != "reload" && count != 1) {  // WP-43: armed for the next operation only
        return Diag::make(fault::ids::kBadRequest, std::string("the stage '") + info.name + "' applies to the next " + info.group + " only: count must be 1",
                          "Arm it again before each operation that should fail.");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (persist && state_file_.empty()) {
        return Diag::make(fault::ids::kPersist,
                          std::string("the arming of '") + info.name + "' cannot be kept across a broker restart: this broker writes no log file to keep it next to",
                          "Start the broker with --log FILE (the arming is kept next to it), or arm the stage without persist.");
    }
    Arming& slot = armed_[static_cast<std::size_t>(stage)];
    const Arming before = slot;
    slot = Arming{count, pid, persist};
    if (persist || before.persist) {
        if (std::optional<Diag> failed = save_locked()) {
            slot = before;
            (void)save_locked();
            return *failed;
        }
    }
    CML_LOG(Trace, "failure injection: broker stage '%s' armed for %u pass(es)%s%s", info.name, count,
            pid ? (" of process " + std::to_string(*pid)).c_str() : "", persist ? ", kept across a broker restart" : "");
    return Result<void>();
#endif
}

void FaultInjector::disarm_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    armed_ = {};
    for (std::set<std::uint32_t>& pids : agent_armed_) {
        pids.clear();
    }
    for (std::set<std::uint32_t>& pids : agent_persist_) {
        pids.clear();
    }
    (void)save_locked();  // nothing is persisted any more: the state file goes
}

void FaultInjector::disarm(fault::Stage stage) {
    std::lock_guard<std::mutex> lock(mutex_);
    Arming& a = armed_[static_cast<std::size_t>(stage)];
    const bool persisted = a.persist;
    a = Arming{};
    if (persisted) {
        (void)save_locked();
    }
}

std::vector<std::string> FaultInjector::disarm_group(const char* group, std::uint32_t pid) {
    std::vector<std::string> unused;
    std::lock_guard<std::mutex> lock(mutex_);
    bool persisted = false;
    for (const fault::StageInfo& s : fault::kStages) {
        Arming& a = armed_[static_cast<std::size_t>(s.stage)];
        if (std::string(s.group) == group && !s.agent_side && a.remaining != 0 && (!a.pid || *a.pid == pid)) {
            persisted = persisted || a.persist;
            a = Arming{};
            unused.emplace_back(s.name);
        }
    }
    if (persisted) {
        (void)save_locked();
    }
    return unused;
}

void FaultInjector::note_agent_armed(fault::Stage stage, std::uint32_t pid, bool persist) {
    std::lock_guard<std::mutex> lock(mutex_);
    agent_armed_[static_cast<std::size_t>(stage)].insert(pid);
    if (persist && agent_persist_[static_cast<std::size_t>(stage)].insert(pid).second) {
        (void)save_locked();
    }
}

bool FaultInjector::take_agent_armed(fault::Stage stage, std::uint32_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (agent_persist_[static_cast<std::size_t>(stage)].erase(pid) != 0) {
        (void)save_locked();
    }
    return agent_armed_[static_cast<std::size_t>(stage)].erase(pid) != 0;
}

void FaultInjector::forget_process(std::uint32_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool dropped = false;
    bool persisted = false;
    for (std::size_t i = 0; i < fault::kStageCount; ++i) {
        dropped = agent_armed_[i].erase(pid) != 0 || dropped;
        persisted = agent_persist_[i].erase(pid) != 0 || persisted;
    }
    if (dropped) {
        CML_LOG(Trace, "failure injection: the agent of process %u is gone; what was armed in it is forgotten", pid);
    }
    if (persisted) {
        (void)save_locked();
    }
}

bool FaultInjector::has_process_records(std::uint32_t pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < fault::kStageCount; ++i) {
        if (agent_armed_[i].count(pid) != 0 || agent_persist_[i].count(pid) != 0) {
            return true;
        }
    }
    return false;
}

void FaultInjector::disarm_process(std::uint32_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Arming& a : armed_) {
        if (a.remaining != 0 && (!a.pid || *a.pid == pid)) {
            a = Arming{};
        }
    }
    for (std::size_t i = 0; i < fault::kStageCount; ++i) {
        agent_armed_[i].erase(pid);
        agent_persist_[i].erase(pid);
    }
    CML_LOG(Trace, "failure injection: everything that could fire for process %u is disarmed; other processes' armings stay", pid);
    (void)save_locked();
}

void FaultInjector::begin_operation(const char* group) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& op : operations_) {
        if (op.first == group) {
            ++op.second;
            return;
        }
    }
    operations_.emplace_back(group, 1);
}

void FaultInjector::end_operation(const char* group) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& op : operations_) {
        if (op.first == group && op.second > 0) {
            --op.second;
            return;
        }
    }
}

bool FaultInjector::operation_running(const char* group) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& op : operations_) {
        if (op.first == group) {
            return op.second > 0;
        }
    }
    return false;
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
        if (a.persist) {
            a.persist = a.remaining != 0;
            (void)save_locked();
        }
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

// ---- WP-44: persistent arming ---------------------------------------------------------------------------------------------

void FaultInjector::set_state_file(std::string path) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_file_ = std::move(path);
}

bool FaultInjector::has_state_file() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !state_file_.empty();
}

bool FaultInjector::any_persisted_locked() const {
    for (std::size_t i = 0; i < fault::kStageCount; ++i) {
        if ((armed_[i].persist && armed_[i].remaining != 0) || !agent_persist_[i].empty()) {
            return true;
        }
    }
    return false;
}

std::optional<Diag> FaultInjector::save_locked() {
    namespace fs = std::filesystem;
    if (state_file_.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    if (!any_persisted_locked()) {
        if (state_written_ || fs::exists(fs::path(state_file_), ec)) {
            fs::remove(fs::path(state_file_), ec);
            state_written_ = false;
            CML_LOG(Trace, "failure injection: nothing is armed with persist any more; %s removed", state_file_.c_str());
        }
        return std::nullopt;
    }
    Value broker = Value::array();
    Value agent = Value::array();
    for (const fault::StageInfo& s : fault::kStages) {
        const std::size_t i = static_cast<std::size_t>(s.stage);
        const Arming& a = armed_[i];
        if (a.persist && a.remaining != 0) {
            Value entry = Value::object();
            entry.set("stage", Value::string(s.name));
            entry.set("count", Value::integer(a.remaining));
            if (a.pid) {
                entry.set("pid", Value::integer(*a.pid));
            }
            broker.push(std::move(entry));
        }
        for (const std::uint32_t pid : agent_persist_[i]) {
            Value entry = Value::object();
            entry.set("stage", Value::string(s.name));
            entry.set("pid", Value::integer(pid));
            agent.push(std::move(entry));
        }
    }
    Value root = Value::object();
    root.set("version", Value::integer(1));
    root.set("broker", std::move(broker));
    root.set("agent", std::move(agent));
    const std::string text = ipc::json::serialize(root) + "\n";
    const auto failure = [&](const std::string& what) {
        const Diag d = Diag::make(fault::ids::kPersist, what,
                                  "Check that the directory of the broker's log is writable; until then the arming is not kept across a broker restart.");
        CML_LOG(Warn, "failure injection: %s", d.format().c_str());
        return d;
    };
    // Written beside the file and renamed over it: a broker killed meanwhile leaves the previous state, never half of one.
    const std::string temp = state_file_ + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            return failure("cannot write the failure-injection state file '" + temp + "'");
        }
    }
    fs::rename(fs::path(temp), fs::path(state_file_), ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(fs::path(temp), ignored);
        return failure("cannot replace the failure-injection state file '" + state_file_ + "': " + ec.message());
    }
    state_written_ = true;
    CML_LOG(Trace, "failure injection: the persisted armings are written to %s", state_file_.c_str());
    return std::nullopt;
}

std::vector<Diag> FaultInjector::load_state() {
    std::vector<Diag> problems;
#if defined(CML_FAULT_INJECTION)
    namespace fs = std::filesystem;
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    if (state_file_.empty() || !fs::exists(fs::path(state_file_), ec)) {
        return problems;
    }
    std::string text;
    {
        std::ifstream in(state_file_, std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        text = buffer.str();
    }
    // A file this broker cannot use arms nothing; it is put aside so that the next start does not trip over it again.
    const auto broken = [&](const std::string& why) {
        armed_ = {};
        for (std::size_t i = 0; i < fault::kStageCount; ++i) {
            agent_armed_[i].clear();
            agent_persist_[i].clear();
        }
        const std::string aside = state_file_ + ".bad";
        std::error_code rename_ec;
        fs::remove(fs::path(aside), rename_ec);
        fs::rename(fs::path(state_file_), fs::path(aside), rename_ec);
        problems.push_back(Diag::make(fault::ids::kPersist,
                                      "the failure-injection state file '" + state_file_ + "' cannot be used (" + why + "); nothing was armed from it",
                                      "It was renamed to '" + aside + "'. Arm the stages again with inject-failure {\"persist\":true}."));
        CML_LOG(Warn, "failure injection: %s", problems.back().format().c_str());
    };
    Result<Value> parsed = ipc::json::parse(text);
    if (!parsed || !parsed->is_object() || parsed->get_integer("version") != 1) {
        broken(parsed ? std::string("it is not version 1 of the format") : parsed.error().message);
        return problems;
    }
    const Value* broker = parsed->find("broker");
    const Value* agent = parsed->find("agent");
    if ((broker != nullptr && !broker->is_array()) || (agent != nullptr && !agent->is_array())) {
        broken("'broker' and 'agent' must be arrays");
        return problems;
    }
    std::size_t restored = 0;
    if (broker != nullptr) {
        for (const Value& e : broker->items()) {
            const fault::StageInfo* info = e.is_object() ? fault::find_stage(e.get_string("stage")) : nullptr;
            const std::int64_t count = e.get_integer("count");
            const std::int64_t pid = e.get_integer("pid");
            if (info == nullptr || info->agent_side || count < 1 || count > static_cast<std::int64_t>(fault::kMaxCount) || pid < 0 || pid > 0xFFFFFFFFll ||
                (std::string(info->group) != "reload" && count != 1) || (std::string(info->group) == "watch" && pid != 0)) {
                // (A watch stage is not per process: with a pid it would never fire or be disarmed, as inject-failure says.)
                broken("a broker entry names no stage of the broker, or has a bad count or pid");
                return problems;
            }
            armed_[static_cast<std::size_t>(info->stage)] =
                Arming{static_cast<std::uint32_t>(count), pid == 0 ? std::nullopt : std::optional<std::uint32_t>(static_cast<std::uint32_t>(pid)), true};
            ++restored;
            CML_LOG(Info, "failure injection: the stage '%s' is armed again from %s (persist)", info->name, state_file_.c_str());
        }
    }
    if (agent != nullptr) {
        for (const Value& e : agent->items()) {
            const fault::StageInfo* info = e.is_object() ? fault::find_stage(e.get_string("stage")) : nullptr;
            const std::int64_t pid = e.get_integer("pid");
            if (info == nullptr || !info->agent_side || pid <= 0 || pid > 0xFFFFFFFFll) {
                broken("an agent entry names no stage of the agent, or has a bad pid");
                return problems;
            }
            agent_armed_[static_cast<std::size_t>(info->stage)].insert(static_cast<std::uint32_t>(pid));
            agent_persist_[static_cast<std::size_t>(info->stage)].insert(static_cast<std::uint32_t>(pid));
            ++restored;
            CML_LOG(Info, "failure injection: the arming of '%s' in process %lld is remembered again from %s (persist)", info->name,
                    static_cast<long long>(pid), state_file_.c_str());
        }
    }
    state_written_ = true;
    if (restored == 0) {
        (void)save_locked();  // an empty state: the file goes
    }
#endif
    return problems;
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
        d.set("group", Value::string(s.group));  // WP-43: reload, restart or revert; WP-44: exception, exception-action, watch
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

std::vector<std::string> finish_injection(ReloadService& service, const char* group, std::uint32_t pid, const std::string& outcome, bool agent_alive) {
#if !defined(CML_FAULT_INJECTION)
    (void)agent_alive;
    (void)service;
    (void)group;
    (void)pid;
    (void)outcome;
    return {};
#else
    std::vector<std::string> unused = service.faults().disarm_group(group, pid);
    for (const fault::StageInfo& s : fault::kStages) {
        if (!s.agent_side || std::string(s.group) != group || !service.faults().take_agent_armed(s.stage, pid)) {
            continue;
        }
        if (std::shared_ptr<AgentLink> link = agent_alive ? service.link_for_pid(pid) : nullptr) {
            (void)arm_agent(*link, s.name, 0);  // count 0: disarm this stage (a no-op when it fired)
        }
        if (outcome.find("CML" + std::to_string(s.diag_id)) == std::string::npos) {
            unused.emplace_back(s.name);
        }
    }
    if (!unused.empty()) {
        std::string names;
        for (const std::string& n : unused) {
            names += (names.empty() ? "" : ", ") + n;
        }
        CML_LOG(Warn, "failure injection: the %s of process %u did not reach the armed stage(s) %s; disarmed (an arming applies to one %s)", group, pid,
                names.c_str(), group);
    } else {
        CML_LOG(Trace, "failure injection: the %s of process %u leaves nothing armed for it", group, pid);
    }
    return unused;
#endif
}

void set_injection_disarmed(Value& result, const std::vector<std::string>& unused) {
    if (unused.empty()) {
        return;
    }
    Value names = Value::array();
    for (const std::string& n : unused) {
        names.push(Value::string(n));
    }
    result.set("injection_disarmed", std::move(names));
}

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
    // WP-44: keep the arming across a broker restart.
    bool persist = false;
    if (const Value* keep = args.find("persist"); keep != nullptr && !keep->is_null()) {
        if (!keep->is_bool()) {
            return Diag::make(fault::ids::kBadRequest, "'persist' must be true or false", "Send {\"persist\": true} to keep the arming across a broker restart.");
        }
        persist = keep->as_bool();
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
        // Disarm everything that can be disarmed, then say which agents could not be. WP-44: the persisted armings go as well; with a
        // pid only what can fire for that process (the other processes' armings and records stay).
        if (pid) {
            service.faults().disarm_process(*pid);
        } else {
            service.faults().disarm_all();
        }
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
    const std::string group = info->group;
    if (group != "reload" && count != 1) {
        return Diag::make(fault::ids::kBadRequest, "the stage '" + stage_name + "' applies to the next " + group + " only: count must be 1",
                          "Arm it again before each operation that should fail.");
    }
    if (group == "watch" && pid) {
        // WP-44: the save watcher reloads every process; its stages are not per process.
        return Diag::make(fault::ids::kBadRequest, "the stage '" + stage_name + "' belongs to the compile-on-save watcher, which is not per process: 'pid' cannot be given",
                          "Send the stage without 'pid'.");
    }
    if (persist && info->agent_side && !has_cleanup(group)) {
        // The arming lives in the agent process, which keeps it across a broker restart by itself; the broker keeps nothing for it.
        return Diag::make(fault::ids::kBadRequest,
                          "'persist' does not apply to the stage '" + stage_name + "': it is armed in the agent process, which keeps it across a broker restart by itself",
                          "Send the stage without 'persist'. It applies to the broker's stages and to the agent stages of the restart, the revert and exception-action.");
    }
    if (persist && !service.faults().has_state_file()) {
        return Diag::make(fault::ids::kPersist,
                          "the arming of '" + stage_name + "' cannot be kept across a broker restart: this broker writes no log file to keep it next to",
                          "Start the broker with --log FILE (the arming is kept next to it), or arm the stage without persist.");
    }
    // WP-43: a stage with an end-of-operation cleanup is armed for the next operation; arming it while one runs could fire in the middle of
    // it (the undo of a revert's re-pointing, say). R-51: the check and the arming are one step against the start of such an operation
    // (the gate, which the operations take while they start), and each operation disarms what is left while it still counts as running.
    const bool cleanup = has_cleanup(group);
    std::unique_lock<std::mutex> gate;
    if (cleanup) {
        gate = std::unique_lock<std::mutex>(service.faults().gate());
        const bool busy = (group == "restart" && service.restart_active()) || (group == "revert" && service.reload_running()) ||
                          (group == "exception-action" && service.faults().operation_running("exception-action")) ||
                          (group == "watch" && service.faults().operation_running("watch"));
        if (busy) {
            const char* what = group == "restart"            ? "a restart"
                               : group == "revert"           ? "a reload or revert"
                               : group == "exception-action" ? "an exception-action"
                                                             : "a batch of saves of the compile-on-save watcher";
            return Diag::make(fault::ids::kInjectBusy, "the stage '" + stage_name + "' cannot be armed while " + what + " runs",
                              "Wait for it to finish (control command `wait`), then arm the stage.");
        }
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
        // R-51: recorded before the agents are asked, so an operation that ends afterwards always finds the record.
        if (cleanup) {
            for (const std::shared_ptr<AgentLink>& link : targets) {
                service.faults().note_agent_armed(info->stage, link->snapshot().pid, persist);
            }
        }
        // All or nothing: the agents already armed are disarmed again when one of them refuses.
        std::vector<std::shared_ptr<AgentLink>> armed;
        for (const std::shared_ptr<AgentLink>& link : targets) {
            if (Result<void> r = arm_agent(*link, stage_name, count); !r) {
                for (const std::shared_ptr<AgentLink>& done : armed) {
                    (void)arm_agent(*done, stage_name, 0);  // count 0: disarm this stage only
                }
                if (cleanup) {
                    for (const std::shared_ptr<AgentLink>& noted : targets) {
                        (void)service.faults().take_agent_armed(info->stage, noted->snapshot().pid);
                    }
                }
                return r.error();  // names the pid that refused
            }
            armed.push_back(link);
            processes.push(Value::integer(link->snapshot().pid));
        }
        if (group == "exception") {
            service.faults().disarm(fault::Stage::ExceptionReport);  // WP-44: one arming of the group at a time (once this one stands)
        }
    } else {
        if (Result<void> r = service.faults().arm(info->stage, count, pid, persist); !r) {
            return r.error();
        }
        if (group == "exception") {
            // WP-44: one arming of the group at a time: once this one stands, exception-hold goes in the agents it applies to.
            for (const std::shared_ptr<AgentLink>& link : targets) {
                (void)arm_agent(*link, fault::info(fault::Stage::ExceptionHold).name, 0);
            }
        }
    }
    CML_LOG(Trace, "failure injection: inject-failure armed '%s' (group %s)%s", stage_name.c_str(), group.c_str(), persist ? " with persist" : "");
    result.set("stage", Value::string(stage_name));
    result.set("armed", Value::boolean(true));
    result.set("count", Value::integer(count));
    result.set("side", Value::string(info->agent_side ? "agent" : "broker"));
    result.set("id", Value::string("CML" + std::to_string(info->diag_id)));
    result.set("processes", std::move(processes));
    result.set("persist", Value::boolean(persist));
    return result;
}

}  // namespace cml::core
