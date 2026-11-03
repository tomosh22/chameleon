#include "cml/core/restart.h"

#include <algorithm>
#include <thread>
#include <utility>

#include "cml/core/agent_link.h"
#include "cml/core/event_bus.h"
#include "cml/core/module_state.h"
#include "cml/core/pipeline_diag_ids.h"
#include "cml/core/reload_service.h"
#include "cml/core/restart_diag_ids.h"
#include "cml/ipc/messages.h"
#include "cml/util/hash.h"
#include "cml/util/log.h"
#include "cml/util/path.h"

namespace cml::core {

namespace rd = restart::diag;
using Clock = std::chrono::steady_clock;
using ipc::json::Value;

namespace {

constexpr const char* kTokenName = "CML_RESTART_TOKEN";
constexpr std::uint32_t kDefaultTimeoutMs = 60000;
constexpr std::uint32_t kTerminateTimeoutMs = 10000;
constexpr std::uint32_t kPollMs = 20;
constexpr std::chrono::seconds kEnableWait{3};  // how long enabled modules may take to show up after the broker asked

// ASCII upper case; the environment names this code looks at are ASCII.
char16_t fold(char16_t c) {
    return (c >= u'a' && c <= u'z') ? static_cast<char16_t>(c - u'a' + u'A') : c;
}

// The name part of "NAME=value". Entries like "=C:=C:\dir" start with '=': the name begins after it.
std::u16string name_of(const std::u16string& entry) {
    const std::size_t eq = entry.find(u'=', entry.empty() || entry[0] != u'=' ? 0 : 1);
    return eq == std::u16string::npos ? entry : entry.substr(0, eq);
}

int compare_names(const std::u16string& a, const std::u16string& b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const char16_t x = fold(a[i]);
        const char16_t y = fold(b[i]);
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

std::u16string widen_ascii(const std::string& text) {
    std::u16string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char16_t>(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string hex64(std::uint64_t v) {
    static const char* const kDigits = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[v & 0xF];
        v >>= 4;
    }
    return out;
}

bool read_uint(const Value& args, const char* key, std::uint32_t max, std::uint32_t* out, std::optional<Diag>* error) {
    const Value* v = args.find(key);
    if (v == nullptr || v->is_null()) {
        return true;
    }
    if (!v->is_integer() || v->as_integer() < 0 || v->as_integer() > static_cast<std::int64_t>(max)) {
        *error = Diag::make(rd::kBadRequest, std::string("argument '") + key + "' must be an integer from 0 to " + std::to_string(max),
                            "Use `{\"pid\":N, \"timeout_ms\":N}` (docs/wp/WP-21.md).");
        return false;
    }
    *out = static_cast<std::uint32_t>(v->as_integer());
    return true;
}

}  // namespace

std::u16string environment_with_token(const std::u16string& block, const std::string& token) {
    std::vector<std::u16string> entries;
    std::size_t pos = 0;
    while (pos < block.size() && block[pos] != u'\0') {
        std::size_t end = block.find(u'\0', pos);
        if (end == std::u16string::npos) {
            end = block.size();
        }
        entries.push_back(block.substr(pos, end - pos));
        pos = end + 1;
    }
    const std::u16string token_name = widen_ascii(kTokenName);
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const std::u16string& e) { return !e.empty() && e[0] != u'=' && compare_names(name_of(e), token_name) == 0; }),
                  entries.end());
    const std::u16string mine = token_name + u"=" + widen_ascii(token);
    // The block is sorted by name (CreateProcess documents that); keep it so. Entries that start with '=' sort first.
    auto where = entries.begin();
    while (where != entries.end() && ((*where)[0] == u'=' || compare_names(name_of(*where), token_name) < 0)) {
        ++where;
    }
    entries.insert(where, mine);
    std::u16string out;
    for (const std::u16string& e : entries) {
        out += e;
        out.push_back(u'\0');
    }
    out.push_back(u'\0');
    return out;
}

RestartCoordinator::RestartCoordinator(ReloadService& service, EventBus* events, std::shared_ptr<ProcessHost> host)
    : service_(service), events_(events), host_(std::move(host)) {}

Result<Value> RestartCoordinator::restart(const Value& args) {
    std::uint32_t pid = 0;
    std::uint32_t timeout_ms = kDefaultTimeoutMs;
    std::optional<Diag> arg_error;
    if (!read_uint(args, "pid", 0xFFFFFFFFu, &pid, &arg_error) || !read_uint(args, "timeout_ms", 3600u * 1000u, &timeout_ms, &arg_error)) {
        return *arg_error;
    }
    if (timeout_ms == 0) {
        timeout_ms = kDefaultTimeoutMs;
    }
    const Clock::time_point start = Clock::now();
    const Clock::time_point deadline = start + std::chrono::milliseconds(timeout_ms);
    const auto remaining_ms = [&]() -> std::uint32_t {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        return left <= 0 ? 0u : static_cast<std::uint32_t>(left);
    };
    const auto pause_ms = [](std::uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    // Every failure is logged and published once, then returned.
    const auto fail = [&](Diag d, std::uint32_t for_pid) -> Diag {
        CML_LOG(Warn, "restart: %s", d.format().c_str());
        if (events_ != nullptr) {
            events_->publish(make_diag_event(d, "error", "restart", for_pid));
        }
        return d;
    };
    const auto warn = [&](Diag d, std::uint32_t for_pid) {
        CML_LOG(Warn, "restart: %s", d.format().c_str());
        if (events_ != nullptr) {
            events_->publish(make_diag_event(d, "warning", "restart", for_pid));
        }
    };

    std::unique_lock<std::mutex> only_one(mutex_, std::try_to_lock);
    if (!only_one.owns_lock() || service_.reload_running()) {
        return fail(Diag::make(rd::kBusy, "another restart or a reload is still running", "Wait for it to finish (control command `wait`) and try again."), pid);
    }

    // ---- which process ----
    std::shared_ptr<AgentLink> old_link;
    if (pid != 0) {
        old_link = service_.link_for_pid(pid);
        if (!old_link) {
            return fail(Diag::make(rd::kNoProcess, "no connected process with pid " + std::to_string(pid),
                                   "Query `status` for the processes whose agent is connected."),
                        pid);
        }
    } else {
        std::vector<std::shared_ptr<AgentLink>> candidates;
        for (const std::shared_ptr<AgentLink>& link : service_.links()) {
            if (link->snapshot().handshaken && !link->closed()) {
                candidates.push_back(link);
            }
        }
        if (candidates.size() != 1) {
            return fail(Diag::make(rd::kBadRequest,
                                   candidates.empty() ? std::string("no process is connected, nothing to restart")
                                                      : std::to_string(candidates.size()) + " processes are connected and no `pid` was given",
                                   "Pass the `pid` of the process to restart (see `status`)."),
                        0);
        }
        old_link = candidates.front();
    }
    const LinkSnapshot old_snap = old_link->snapshot();
    const std::uint32_t old_pid = old_snap.pid;

    // ---- what to carry over: the baselines of the enabled modules (their indexes die with the connection) ----
    struct Carried {
        std::string path;
        BaselineSnapshot baseline;
    };
    std::vector<Carried> carried;
    for (const std::shared_ptr<ModuleState>& state : service_.module_states(old_snap.agent_id)) {
        std::shared_ptr<ModuleIndex> index;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            index = state->index;
        }
        if (index && index->status().state == IndexState::Ready) {
            carried.push_back(Carried{state->path, index->baseline_snapshot()});
        }
    }

    // ---- 1. the agent prepares: pre-restart hooks, launch parameters ----
    CML_LOG(Info, "restart: asking process %u to prepare", old_pid);
    ipc::Restart prepare;
    prepare.request_id = old_link->next_request_id();
    Result<ipc::Message> answer = old_link->request(prepare, std::max<std::uint32_t>(remaining_ms(), 1));
    if (!answer) {
        return fail(Diag::make(rd::kPrepareFailed,
                               "process " + std::to_string(old_pid) + " did not prepare for the restart: " + answer.error().message +
                                   " (the process was not touched)",
                               "In synchronised mode the application must reach SyncPoint() for the pre-restart hooks to run; try again."),
                    old_pid);
    }
    const ipc::RestartInfo* info = std::get_if<ipc::RestartInfo>(&*answer);
    if (info == nullptr) {
        const ipc::Reply* reply = std::get_if<ipc::Reply>(&*answer);
        return fail(Diag::make(rd::kPrepareFailed,
                               "process " + std::to_string(old_pid) + " refused the restart" +
                                   (reply != nullptr ? ": CML" + std::to_string(reply->status) + " " + reply->message : std::string()) +
                                   " (the process was not touched)",
                               "Use an agent from the same Chameleon build as the broker."),
                    old_pid);
    }
    if (info->status != 0) {
        return fail(Diag::make(rd::kPrepareFailed,
                               "process " + std::to_string(old_pid) + " cannot be restarted: CML" + std::to_string(info->status) + " " + info->message +
                                   " (the process was not touched)",
                               "See the agent's diagnostic for what to change."),
                    old_pid);
    }
    if (info->image_path.empty() || info->command_line.empty()) {
        return fail(Diag::make(rd::kPrepareFailed, "process " + std::to_string(old_pid) + " reported no image path or command line (the process was not touched)",
                               "Use an agent from the same Chameleon build as the broker."),
                    old_pid);
    }
    CML_LOG(Info, "restart: process %u prepared (%u pre-restart hook(s) ran, %llu module(s) enabled)", old_pid, info->hooks_run,
            static_cast<unsigned long long>(info->modules.size()));

    // The modules to have enabled again: what the application enabled (with its options); the broker's own list when the
    // agent reports none.
    std::vector<ipc::RestartModule> modules = info->modules;
    if (modules.empty()) {
        for (const std::string& path : old_snap.enabled) {
            modules.push_back(ipc::RestartModule{0, path});
        }
    }

    // ---- 2. the old process goes, the new one starts ----
    const std::uint64_t std_handles[3] = {info->std_input, info->std_output, info->std_error};
    std::vector<Diag> open_warnings;
    Result<std::unique_ptr<RestartableProcess>> opened = host_->open(old_pid, std_handles, &open_warnings);
    for (const Diag& w : open_warnings) {
        warn(w, old_pid);
    }
    if (!opened) {
        return fail(Diag::make(rd::kOpenFailed, "cannot take over process " + std::to_string(old_pid) + ": " + opened.error().message + " (the process was not touched)",
                               opened.error().what_to_do),
                    old_pid);
    }
    std::unique_ptr<RestartableProcess> process = std::move(*opened);

    const std::string token = hex64(hash64(std::to_string(old_pid) + ":" + std::to_string(Clock::now().time_since_epoch().count()) + ":" +
                                           std::to_string(++counter_))) +
                              hex64(static_cast<std::uint64_t>(Clock::now().time_since_epoch().count()));
    RelaunchSpec spec;
    spec.image_path = info->image_path;
    spec.command_line = info->command_line;
    spec.working_directory = info->working_directory;
    spec.environment = environment_with_token(info->environment, token);

    CML_LOG(Info, "restart: terminating process %u", old_pid);
    Result<void> terminated = process->terminate(kTerminateTimeoutMs);
    if (!terminated) {
        return fail(Diag::make(rd::kTerminateFailed, "process " + std::to_string(old_pid) + " could not be terminated: " + terminated.error().message,
                               "Close it yourself and start the application again."),
                    old_pid);
    }
    Result<std::uint32_t> launched = process->relaunch(spec);
    if (!launched) {
        return fail(Diag::make(rd::kLaunchFailed,
                               "process " + std::to_string(old_pid) + " was terminated but the new one could not be started: " + launched.error().message,
                               "Start the application yourself; its agent registers again by itself."),
                    old_pid);
    }
    CML_LOG(Info, "restart: started process %u (token %s), waiting for its agent", *launched, token.c_str());

    // ---- 3. the new agent registers ----
    std::shared_ptr<AgentLink> new_link;
    for (;;) {
        new_link = service_.link_for_restart_token(token);
        if (new_link) {
            break;
        }
        if (const std::optional<std::uint32_t> code = process->relaunched_exit_code()) {
            return fail(Diag::make(rd::kNewProcessExited,
                                   "the new process (pid " + std::to_string(*launched) + ") exited with code " + std::to_string(*code) + " before its agent registered",
                                   "Run the application by hand to see why it stops; the old process is already gone."),
                        *launched);
        }
        if (remaining_ms() == 0) {
            return fail(Diag::make(rd::kNoRegistration,
                                   "the new process (pid " + std::to_string(*launched) + ") did not register within " + std::to_string(timeout_ms) +
                                       " ms; it was left running",
                                   "Check that the application creates its agent at start-up (and that it can reach the broker); raise `timeout_ms` if it starts slowly."),
                        *launched);
        }
        pause_ms(kPollMs);
    }
    const std::uint32_t new_pid = new_link->snapshot().pid;
    const std::uint64_t new_agent = new_link->agent_id();
    CML_LOG(Info, "restart: process %u registered as agent#%llu", new_pid, static_cast<unsigned long long>(new_agent));

    // ---- 4. the same modules, enabled again ----
    // The module list arrives right after the handshake; EnableModule needs it.
    while (new_link->snapshot().modules.empty() && remaining_ms() > 0) {
        pause_ms(kPollMs);
    }
    const auto missing_in = [&](const LinkSnapshot& snap) {
        std::vector<ipc::RestartModule> missing;
        for (const ipc::RestartModule& m : modules) {
            const bool have = std::any_of(snap.enabled.begin(), snap.enabled.end(),
                                          [&](const std::string& full) { return path::matches_module(m.path, full); });
            if (!have) {
                missing.push_back(m);
            }
        }
        return missing;
    };
    // The application usually enables its modules by itself within a moment: give it that, then ask.
    const Clock::time_point grace_end = std::min(deadline, Clock::now() + grace_);
    while (!missing_in(new_link->snapshot()).empty() && Clock::now() < grace_end) {
        pause_ms(kPollMs);
    }
    std::vector<ipc::RestartModule> missing = missing_in(new_link->snapshot());
    if (!missing.empty()) {
        ipc::ReenableModules ask;
        ask.request_id = new_link->next_request_id();
        ask.modules = missing;
        CML_LOG(Info, "restart: asking process %u to enable %llu module(s) again", new_pid, static_cast<unsigned long long>(missing.size()));
        Result<ipc::Message> queued = new_link->request(ask, std::max<std::uint32_t>(std::min<std::uint32_t>(remaining_ms(), 10000), 1));
        if (!queued) {
            warn(Diag::make(rd::kModuleNotEnabled, "process " + std::to_string(new_pid) + " did not take the request to enable its modules again: " + queued.error().message,
                            "Enable the modules from the application."),
                 new_pid);
        } else if (const ipc::Reply* reply = std::get_if<ipc::Reply>(&*queued); reply != nullptr && reply->status != 0) {
            warn(Diag::make(rd::kModuleNotEnabled, "process " + std::to_string(new_pid) + " refused to enable its modules again: CML" + std::to_string(reply->status) + " " + reply->message,
                            "Enable the modules from the application."),
                 new_pid);
        }
        const Clock::time_point enable_end = std::min(deadline, Clock::now() + kEnableWait);
        while (!missing_in(new_link->snapshot()).empty() && Clock::now() < enable_end) {
            pause_ms(kPollMs);
        }
        missing = missing_in(new_link->snapshot());
    }
    Value not_enabled = Value::array();
    for (const ipc::RestartModule& m : missing) {
        not_enabled.push(Value::string(m.path));
        warn(Diag::make(rd::kModuleNotEnabled, "module '" + m.path + "' is not enabled in the new process (pid " + std::to_string(new_pid) + ")",
                        "It may not be loaded yet; enable it from the application once it is."),
             new_pid);
    }

    // The enabled modules of the new process, and their PDB indexes.
    const LinkSnapshot new_snap = new_link->snapshot();
    Value enabled_json = Value::array();
    std::size_t restored = 0;
    for (const std::string& full : new_snap.enabled) {
        enabled_json.push(Value::string(full));
        std::shared_ptr<ModuleState> state;
        std::shared_ptr<ModuleIndex> index;
        while (remaining_ms() > 0) {
            state = service_.module_state(new_agent, full);
            if (state) {
                std::lock_guard<std::mutex> lock(state->mutex);
                index = state->index;
            }
            if (index) {
                break;
            }
            pause_ms(kPollMs);
        }
        if (!index || !index->wait(remaining_ms()) || index->status().state != IndexState::Ready) {
            warn(Diag::make(rd::kReapplyFailed, "the PDB index of '" + full + "' in the new process is not ready, so its edits cannot be re-applied",
                            "Run `reload` once its index is ready."),
                 new_pid);
            continue;
        }
        // ---- 5. the edits of the session: the baseline goes back to the original binary's sources ----
        for (const Carried& c : carried) {
            if (path::same_path(c.path, full)) {
                restored += index->restore_baseline(c.baseline);
            }
        }
    }

    // ---- 6. one reload applies every file that had been patched before ----
    Value result = Value::object();
    if (restored > 0) {
        CML_LOG(Info, "restart: re-applying the session's edits (%llu file(s) patched in the previous process)", static_cast<unsigned long long>(restored));
        Value reload_args = Value::object();
        reload_args.set("pid", Value::integer(new_pid));
        reload_args.set("confirm", Value::boolean(true));  // the user confirmed these edits when they were first reloaded
        reload_args.set("timeout_ms", Value::integer(std::max<std::uint32_t>(remaining_ms(), 1000)));
        for (;;) {
            Result<Value> reloaded = service_.reload(reload_args);
            if (reloaded) {
                result.set("reapplied_files", Value::integer(static_cast<std::int64_t>(restored)));
                result.set("patched", Value::integer(reloaded->get_integer("patched")));
                if (const Value* changed = reloaded->find("changed_files")) {
                    result.set("changed_files", *changed);
                }
                break;
            }
            if (reloaded.error().code == pipeline::diag::kBusy && remaining_ms() > 0) {
                pause_ms(50);  // a save-triggered reload is running; ours finds what it leaves
                continue;
            }
            Value error = Value::object();
            error.set("id", Value::string(reloaded.error().id()));
            error.set("message", Value::string(reloaded.error().message));
            result.set("reapply_error", std::move(error));
            warn(Diag::make(rd::kReapplyFailed, "the new process (pid " + std::to_string(new_pid) + ") runs the original code: re-applying the session's edits failed: " +
                                                    reloaded.error().format(),
                            "Fix the error and run `reload`."),
                 new_pid);
            break;
        }
    }

    result.set("old_pid", Value::integer(old_pid));
    result.set("new_pid", Value::integer(new_pid));
    result.set("modules", std::move(enabled_json));
    result.set("not_enabled", std::move(not_enabled));
    result.set("hooks_run", Value::integer(info->hooks_run));
    result.set("restart_ms", Value::integer(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count()));

    if (events_ != nullptr) {
        Value body = Value::object();
        body.set("event", Value::string("restarted"));
        body.set("old_pid", Value::integer(old_pid));
        body.set("new_pid", Value::integer(new_pid));
        Event event;
        event.name = "restarted";
        event.body = std::move(body);
        events_->publish(event);
    }
    CML_LOG(Info, "restart: process %u is now process %u (%lld ms)", old_pid, new_pid,
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count()));
    return result;
}

#if !defined(_WIN32)
std::shared_ptr<ProcessHost> native_process_host() {
    return nullptr;
}
#endif

}  // namespace cml::core
