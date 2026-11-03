#include "cml/core/event_bus.h"

#include <algorithm>
#include <utility>

namespace cml::core {

Event make_diag_event(const Diag& diag, const char* severity, const std::string& stage, std::uint32_t pid) {
    Event e;
    e.name = "diag";
    e.body = ipc::json::Value::object();
    e.body.set("event", ipc::json::Value::string("diag"));
    e.body.set("id", ipc::json::Value::string(diag.id()));
    e.body.set("message", ipc::json::Value::string(diag.message));
    e.body.set("action", ipc::json::Value::string(diag.what_to_do));
    e.body.set("severity", ipc::json::Value::string(severity));
    if (!stage.empty()) {
        e.body.set("stage", ipc::json::Value::string(stage));
    }
    if (pid != 0) {
        e.body.set("pid", ipc::json::Value::integer(static_cast<std::int64_t>(pid)));
    }
    return e;
}

Event make_patch_applied_event(std::uint32_t pid, const std::string& module, std::uint64_t generation, std::size_t hooks,
                               std::size_t new_functions, std::size_t new_globals) {
    Event e;
    e.name = "patch_applied";
    e.body = ipc::json::Value::object();
    e.body.set("event", ipc::json::Value::string("patch_applied"));
    e.body.set("pid", ipc::json::Value::integer(static_cast<std::int64_t>(pid)));
    e.body.set("module", ipc::json::Value::string(module));
    e.body.set("generation", ipc::json::Value::integer(static_cast<std::int64_t>(generation)));
    e.body.set("hooks", ipc::json::Value::integer(static_cast<std::int64_t>(hooks)));
    e.body.set("new_functions", ipc::json::Value::integer(static_cast<std::int64_t>(new_functions)));
    e.body.set("new_globals", ipc::json::Value::integer(static_cast<std::int64_t>(new_globals)));
    return e;
}

Event make_patch_reverted_event(std::uint32_t pid, const std::string& module, std::uint64_t generation) {
    Event e;
    e.name = "patch_reverted";
    e.body = ipc::json::Value::object();
    e.body.set("event", ipc::json::Value::string("patch_reverted"));
    e.body.set("pid", ipc::json::Value::integer(static_cast<std::int64_t>(pid)));
    e.body.set("module", ipc::json::Value::string(module));
    e.body.set("generation", ipc::json::Value::integer(static_cast<std::int64_t>(generation)));
    return e;
}

Event make_progress_event(const std::string& stage, std::uint32_t pid, const std::string& module, std::size_t done, std::size_t total) {
    Event e;
    e.name = "progress";
    e.body = ipc::json::Value::object();
    e.body.set("event", ipc::json::Value::string("progress"));
    e.body.set("stage", ipc::json::Value::string(stage));
    e.body.set("pid", ipc::json::Value::integer(static_cast<std::int64_t>(pid)));
    e.body.set("module", ipc::json::Value::string(module));
    e.body.set("done", ipc::json::Value::integer(static_cast<std::int64_t>(done)));
    e.body.set("total", ipc::json::Value::integer(static_cast<std::int64_t>(total)));
    return e;
}

Event make_log_event(const char* level, const std::string& message) {
    Event e;
    e.name = "log";
    e.body = ipc::json::Value::object();
    e.body.set("event", ipc::json::Value::string("log"));
    e.body.set("level", ipc::json::Value::string(level));
    e.body.set("message", ipc::json::Value::string(message));
    return e;
}

EventBus::Id EventBus::subscribe(std::shared_ptr<EventListener> listener, std::set<std::string> names) {
    std::lock_guard<std::mutex> lock(mutex_);
    Subscription s;
    s.id = next_++;
    s.listener = std::move(listener);
    s.names = std::move(names);
    subs_.push_back(std::move(s));
    return subs_.back().id;
}

bool EventBus::update(Id id, std::set<std::string> names) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Subscription& s : subs_) {
        if (s.id == id) {
            s.names = std::move(names);
            return true;
        }
    }
    return false;
}

void EventBus::unsubscribe(Id id) {
    std::lock_guard<std::mutex> lock(mutex_);
    subs_.erase(std::remove_if(subs_.begin(), subs_.end(), [id](const Subscription& s) { return s.id == id; }), subs_.end());
}

void EventBus::publish(const Event& event) {
    std::vector<std::shared_ptr<EventListener>> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Subscription& s : subs_) {
            if (s.names.empty() || s.names.count(event.name) != 0) {
                targets.push_back(s.listener);
            }
        }
    }
    for (const std::shared_ptr<EventListener>& l : targets) {
        l->on_event(event);
    }
}

bool EventBus::wanted(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Subscription& s : subs_) {
        if (s.names.empty() || s.names.count(name) != 0) {
            return true;
        }
    }
    return false;
}

std::size_t EventBus::subscribers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return subs_.size();
}

}  // namespace cml::core
