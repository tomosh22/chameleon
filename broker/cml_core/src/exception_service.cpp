#include "cml/core/exception_service.h"

#include <cstdio>
#include <iterator>
#include <thread>
#include <utility>

#include "cml/core/bind_names.h"
#include "cml/core/exception_diag_ids.h"
#include "cml/core/module_state.h"
#include "cml/util/log.h"

namespace cml::core {

namespace ed = exceptions::diag;
using ipc::json::Value;

namespace {

constexpr std::uint32_t kActionTimeoutMs = 10000;
constexpr std::uint32_t kConfigTimeoutMs = 3000;

Diag bad_arguments(const std::string& message) {
    return Diag::make(ed::kBadArguments, message, "Send {\"pid\": N, \"exception_id\": N, \"action\": \"continue\"|\"leave\"|\"ignore\"}.");
}

std::string hex(std::uint64_t value, int digits) {
    char text[32];
    std::snprintf(text, sizeof text, "0x%0*llX", digits, static_cast<unsigned long long>(value));
    return text;
}

std::string file_name_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

const char* access_name(std::uint32_t access) {
    switch (access) {
        case 0: return "read";
        case 1: return "write";
        case 8: return "execute";
        default: return "";
    }
}

Value actions_array() {
    Value actions = Value::array();
    for (const char* a : {"continue", "leave", "ignore"}) {
        actions.push(Value::string(a));
    }
    return actions;
}

}  // namespace

ExceptionService::ExceptionService(ReloadService* service, EventBus* events) : service_(service), events_(events) {}

bool ExceptionService::active() const {
    return service_->options().crash_handler && events_ != nullptr && events_->wanted("exception");
}

std::size_t ExceptionService::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

ipc::ExceptionConfig ExceptionService::make_config() const {
    ipc::ExceptionConfig config;
    const bool on = active();
    config.flags = (on ? ipc::kExceptionConfigEnabled : 0u) |
                   (on && service_->options().break_with_debugger ? ipc::kExceptionConfigBreakWithDebugger : 0u);
    return config;
}

void ExceptionService::agent_ready(const std::shared_ptr<AgentLink>& link) {
    // Called after the agent's Welcome was sent (the agent expects nothing else before it). Registered and told under the same
    // mutex refresh() uses, so a concurrent refresh either includes this agent (and told it first) or runs entirely before.
    std::lock_guard<std::mutex> config_lock(config_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        links_[link->agent_id()] = link;
    }
    ipc::ExceptionConfig config = make_config();
    if ((config.flags & ipc::kExceptionConfigEnabled) == 0) {
        return;  // the agent starts with holding off
    }
    config.request_id = link->next_request_id();
    const Result<void> sent = link->send(config);  // never wait on the connection thread: it reads the answers
    if (!sent) {
        CML_LOG(Warn, "agent#%llu: cannot send the crash-handler configuration: %s", static_cast<unsigned long long>(link->agent_id()),
                sent.error().format().c_str());
    }
}

void ExceptionService::agent_gone(std::uint64_t agent_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    links_.erase(agent_id);
    for (auto it = pending_.begin(); it != pending_.end();) {
        it = it->first.agent_id == agent_id ? pending_.erase(it) : std::next(it);
    }
}

void ExceptionService::send_ignore(const std::shared_ptr<AgentLink>& link, std::uint32_t exception_id) {
    ipc::ExceptionAction action;
    action.request_id = link->next_request_id();
    action.exception_id = exception_id;
    action.action = ipc::ExceptionActionKind::Ignore;
    (void)link->send(action);  // the answer finds no waiter and is dropped
}

Value ExceptionService::describe(const std::shared_ptr<AgentLink>& link, std::uint32_t pid, const ipc::Exception& exception) const {
    const bool in_patch = (exception.flags & ipc::kExceptionFlagInPatch) != 0;
    std::string function;
    std::uint64_t offset = exception.address - exception.module_base;
    if (!in_patch && !exception.module_path.empty()) {
        if (const std::shared_ptr<ModuleState> state = service_->module_state(link->agent_id(), exception.module_path)) {
            if (state->index != nullptr) {
                const std::shared_ptr<const pdb::PdbIndex> index = state->index->pdb();
                if (index != nullptr && exception.address >= exception.module_base) {
                    const std::uint64_t rva = exception.address - exception.module_base;
                    if (rva <= 0xFFFFFFFFull) {
                        Result<std::optional<pdb::SymbolView>> proc = index->find_proc_by_rva(static_cast<std::uint32_t>(rva));
                        if (proc && *proc) {
                            function = bind::display_name((*proc)->name);
                            offset = rva - (*proc)->rva;
                        }
                    }
                }
            }
        }
    }
    Value body = Value::object();
    body.set("event", Value::string("exception"));
    body.set("pid", Value::integer(static_cast<std::int64_t>(pid)));
    body.set("tid", Value::integer(static_cast<std::int64_t>(exception.thread_id)));
    body.set("exception_id", Value::integer(static_cast<std::int64_t>(exception.exception_id)));
    body.set("code", Value::string(hex(exception.code, 8)));
    body.set("address", Value::string(hex(exception.address, 16)));
    body.set("module", Value::string(in_patch ? std::string("(patch)") : file_name_of(exception.module_path)));
    body.set("function", Value::string(function));
    body.set("offset", Value::integer(static_cast<std::int64_t>(offset)));
    body.set("actions", actions_array());
    // Additions (ADR-0032 allows fields to be added).
    body.set("module_path", Value::string(exception.module_path));
    body.set("in_patch", Value::boolean(in_patch));
    body.set("access", Value::string(access_name(exception.access)));
    body.set("data_address", Value::string(hex(exception.data_address, 16)));
    body.set("stack_pointer", Value::string(hex(exception.stack_pointer, 16)));
    return body;
}

void ExceptionService::on_exception(const std::shared_ptr<AgentLink>& link, std::uint32_t pid, const ipc::Exception& exception) {
    Event event;
    event.name = "exception";
    event.body = describe(link, pid, exception);
    bool release = false;
    {
        // Check and insert under the mutex refresh() clears the list under: an entry can never survive a switch-off.
        std::lock_guard<std::mutex> lock(mutex_);
        if (holding_) {
            Pending pending;
            pending.pid = pid;
            pending.link = link;
            pending.body = event.body;
            pending_[Key{link->agent_id(), exception.exception_id}] = std::move(pending);
        } else {
            release = true;
        }
    }
    if (release) {
        // The listener left between the agent's decision and now: nobody can answer, so do not keep the thread waiting.
        // (Sent after the lock is dropped: a write to the agent can block.)
        CML_LOG(Info, "CML%u: exception %u of process %u released as Ignore: no client listens for exception events", ed::kIgnoredNoClient,
                exception.exception_id, pid);
        send_ignore(link, exception.exception_id);
        return;
    }
    CML_LOG(Info, "process %u: thread %u faulted with %s at %s (%s); waiting for a decision", pid, exception.thread_id,
            event.body.get_string("code").c_str(), event.body.get_string("address").c_str(), event.body.get_string("function").c_str());
    if (events_ != nullptr) {
        events_->publish(event);
    }
}

Result<Value> ExceptionService::action(const Value& args) {
    const Value* id_value = args.find("exception_id");
    const Value* action_value = args.find("action");
    const Value* pid_value = args.find("pid");
    if (id_value == nullptr || !id_value->is_integer() || id_value->as_integer() < 0 || id_value->as_integer() > 0xFFFFFFFFll) {
        return bad_arguments("argument 'exception_id' must be a non-negative integer");
    }
    if (action_value == nullptr || !action_value->is_string()) {
        return bad_arguments("argument 'action' must be \"continue\", \"leave\" or \"ignore\"");
    }
    if (pid_value != nullptr && !pid_value->is_null() && (!pid_value->is_integer() || pid_value->as_integer() < 0)) {
        return bad_arguments("argument 'pid' must be a non-negative integer");
    }
    const std::string& name = action_value->as_string();
    ipc::ExceptionActionKind kind = ipc::ExceptionActionKind::Ignore;
    if (name == "continue") {
        kind = ipc::ExceptionActionKind::Continue;
    } else if (name == "leave") {
        kind = ipc::ExceptionActionKind::Leave;
    } else if (name != "ignore") {
        return bad_arguments("unknown action '" + name + "'; use continue, leave or ignore");
    }
    const auto exception_id = static_cast<std::uint32_t>(id_value->as_integer());
    const bool has_pid = pid_value != nullptr && pid_value->is_integer();

    std::shared_ptr<AgentLink> link;
    std::uint32_t pid = 0;
    bool ambiguous = false;
    Key key;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [k, p] : pending_) {
            if (k.exception_id == exception_id && (!has_pid || p.pid == static_cast<std::uint32_t>(pid_value->as_integer()))) {
                if (link != nullptr) {
                    ambiguous = true;
                }
                key = k;
                link = p.link;
                pid = p.pid;
            }
        }
    }
    if (ambiguous) {
        return Diag::make(ed::kAmbiguousException,
                          "exception_id " + std::to_string(exception_id) + " is held in more than one process",
                          "Add \"pid\" to the request; `exceptions` lists the processes.");
    }
    if (link == nullptr) {
        return Diag::make(ed::kNoSuchException,
                          "no held thread with exception_id " + std::to_string(exception_id) + (has_pid ? " in process " + std::to_string(pid_value->as_integer()) : std::string()),
                          "Query `exceptions` for the threads that wait for a decision; one that was answered or whose process exited is gone.");
    }
    ipc::ExceptionAction request;
    request.request_id = link->next_request_id();
    request.exception_id = exception_id;
    request.action = kind;
    Result<ipc::Message> answer = link->request(request, kActionTimeoutMs);
    if (!answer) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(key);  // the connection is gone (or silent): the held thread cannot be reached from here
        return Diag::make(ed::kAgentUnreachable, "the agent of process " + std::to_string(pid) + " did not answer: " + answer.error().message,
                          "The application may have exited; the thread was dropped from the pending list.");
    }
    const auto* reply = std::get_if<ipc::Reply>(&*answer);
    if (reply == nullptr) {
        return Diag::make(ed::kAgentUnreachable, "the agent answered ExceptionAction with something else than Reply", "This is a bug; report it.");
    }
    if (reply->status != 0) {
        // A refusal ("cannot unwind this frame") leaves the thread held so that the client can choose another action;
        // "unknown exception" means it is gone.
        if (reply->status == ipc::kStatusUnknownException) {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.erase(key);
        }
        return Diag::make(reply->status, reply->message, "Choose another action, or ignore the fault.");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(key);
    }
    Value result = Value::object();
    result.set("pid", Value::integer(static_cast<std::int64_t>(pid)));
    result.set("exception_id", Value::integer(static_cast<std::int64_t>(exception_id)));
    result.set("action", Value::string(name));
    return result;
}

Result<Value> ExceptionService::pending(const Value& args) const {
    const Value* pid_value = args.find("pid");
    if (pid_value != nullptr && !pid_value->is_null() && (!pid_value->is_integer() || pid_value->as_integer() < 0)) {
        return bad_arguments("argument 'pid' must be a non-negative integer");
    }
    Value list = Value::array();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [key, p] : pending_) {
            if (pid_value != nullptr && pid_value->is_integer() && static_cast<std::int64_t>(p.pid) != pid_value->as_integer()) {
                continue;
            }
            list.push(p.body);
        }
    }
    Value result = Value::object();
    result.set("pending", std::move(list));
    return result;
}

void ExceptionService::refresh(bool wait) {
    std::lock_guard<std::mutex> config_lock(config_mutex_);
    ipc::ExceptionConfig config = make_config();
    const bool enabled = (config.flags & ipc::kExceptionConfigEnabled) != 0;
    if (config.flags == applied_flags_) {
        return;  // nothing the agents do not know already
    }
    applied_flags_ = config.flags;
    std::vector<std::shared_ptr<AgentLink>> links;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, link] : links_) {
            links.push_back(link);
        }
        holding_ = enabled;
        if (!enabled) {
            pending_.clear();  // the agents release every held thread as Ignore
        }
    }
    CML_LOG(Info, "crash handling %s for %llu agents", enabled ? "on" : "off", static_cast<unsigned long long>(links.size()));
    const auto tell = [&config](const std::shared_ptr<AgentLink>& link, bool wait_for_answer) {
        ipc::ExceptionConfig mine = config;
        mine.request_id = link->next_request_id();
        if (!wait_for_answer) {
            (void)link->send(mine);
            return;
        }
        Result<ipc::Message> answer = link->request(mine, kConfigTimeoutMs);
        const auto* reply = answer ? std::get_if<ipc::Reply>(&*answer) : nullptr;
        if (!answer || reply == nullptr || reply->status != 0) {
            CML_LOG(Warn, "CML%u: agent#%llu did not accept the crash-handler configuration: %s", ed::kConfigRefused,
                    static_cast<unsigned long long>(link->agent_id()),
                    !answer ? answer.error().format().c_str() : (reply != nullptr ? reply->message.c_str() : "unexpected answer"));
        }
    };
    if (!wait || links.size() < 2) {
        for (const std::shared_ptr<AgentLink>& link : links) {
            tell(link, wait);
        }
        return;
    }
    // Several agents: ask them all at once, so one that does not answer costs one timeout, not one per agent.
    std::vector<std::thread> askers;
    askers.reserve(links.size());
    for (const std::shared_ptr<AgentLink>& link : links) {
        askers.emplace_back([&tell, link] { tell(link, true); });
    }
    for (std::thread& t : askers) {
        t.join();
    }
}

}  // namespace cml::core
