#pragma once

// Unsolicited control-pipe events (WP-12): `diag`, `patch_applied`, `log` (docs/wp/WP-13.md, "Events").
//
// The pipeline publishes events; each subscribed control connection receives them as one JSON line. The bus is
// transport independent (portable): a listener is any object with on_event(). Listeners are called on the
// publishing thread and must not block for long (the control server's listener gives up on a stalled client).
// Publishing never holds the bus lock while calling a listener, so a listener may unsubscribe itself.

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "cml/ipc/json.h"
#include "cml/util/diag.h"

namespace cml::core {

struct Event {
    std::string name;       // "diag" | "patch_applied" | "progress" | "log"
    ipc::json::Value body;  // the whole JSON object, including "event": name
};

// {"event":"diag","id":"CML3200","message":..,"action":..,"severity":"error|warning|info","stage":..,"pid"?:N}
Event make_diag_event(const Diag& diag, const char* severity, const std::string& stage, std::uint32_t pid = 0);
// {"event":"patch_applied","pid":N,"module":..,"generation":N,"hooks":N,"new_functions":N,"new_globals":N}
Event make_patch_applied_event(std::uint32_t pid, const std::string& module, std::uint64_t generation, std::size_t hooks,
                               std::size_t new_functions, std::size_t new_globals);
// {"event":"progress","stage":"compile","pid":N,"module":..,"done":N,"total":N}  (WP-15: one per finished compile)
Event make_progress_event(const std::string& stage, std::uint32_t pid, const std::string& module, std::size_t done, std::size_t total);
// {"event":"log","level":"info","message":..}
Event make_log_event(const char* level, const std::string& message);

class EventListener {
public:
    virtual ~EventListener() = default;
    virtual void on_event(const Event& event) = 0;
};

class EventBus {
public:
    using Id = std::uint64_t;

    // `names` empty = every event. Returns the subscription id.
    Id subscribe(std::shared_ptr<EventListener> listener, std::set<std::string> names = {});
    // Replaces the names of an existing subscription (a second `subscribe` on the same connection).
    bool update(Id id, std::set<std::string> names);
    void unsubscribe(Id id);

    void publish(const Event& event);
    bool wanted(const std::string& name) const;  // does anybody listen for this event?
    std::size_t subscribers() const;

private:
    struct Subscription {
        Id id = 0;
        std::shared_ptr<EventListener> listener;
        std::set<std::string> names;
    };
    mutable std::mutex mutex_;
    std::vector<Subscription> subs_;
    Id next_ = 1;
};

}  // namespace cml::core
