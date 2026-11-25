#include "cml/core/restart.h"

#include <algorithm>
#include <thread>
#include <utility>

#include "cml/core/agent_link.h"
#include "cml/core/control_args.h"
#include "cml/core/event_bus.h"
#include "cml/core/module_state.h"
#include "cml/core/pipeline_diag_ids.h"
#include "cml/core/reload_service.h"
#include "cml/core/restart_diag_ids.h"
#include "cml/ipc/control.h"
#include "cml/ipc/messages.h"
#include "cml/util/format.h"
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
constexpr std::uint32_t kDefaultRegistrationMs = 30000;  // after the old process is gone: registration, modules, indexes, re-applying the edits
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

// The token names are ASCII: widen byte by byte.
std::u16string widen_ascii(const std::string& text) {
    return std::u16string(text.begin(), text.end());
}

Diag bad_args(const std::string& message) {
    return Diag::make(rd::kBadRequest, message, "Use `{\"pid\":N, \"timeout_ms\":N, \"registration_timeout_ms\":N}` (docs/wp/WP-21.md).");
}

bool read_uint(const Value& args, const char* key, std::uint32_t max, std::uint32_t* out, std::optional<Diag>* error) {
    return read_uint_arg(args, key, max, out, error, bad_args);
}

// "A=b\0C=d\0\0": at least one entry, every entry ends with a NUL, and a second NUL ends the block.
bool environment_block_ok(const std::u16string& block) {
    return block.size() >= 4 && block[block.size() - 1] == u'\0' && block[block.size() - 2] == u'\0' && block[0] != u'\0';
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

std::shared_ptr<ProcessHost> RestartCoordinator::host() const {
    std::lock_guard<std::mutex> lock(host_mutex_);
    return host_;
}

void RestartCoordinator::set_host(std::shared_ptr<ProcessHost> host) {
    std::lock_guard<std::mutex> lock(host_mutex_);
    host_ = std::move(host);
}

bool RestartCoordinator::has_host() const {
    return host() != nullptr;
}

bool RestartCoordinator::hold_supported() const {
    const std::shared_ptr<ProcessHost> h = host();
    return h != nullptr && h->supports_hold();
}

void RestartCoordinator::remember_unfinished(std::uint32_t pid, Unfinished unfinished) {
    if (unfinished.modules.empty() && unfinished.carried.empty()) {
        return;
    }
    CML_LOG(Info, "restart: remembering for process %u what its restart could not do (%llu module(s) to enable, %llu baseline(s) to restore); its next restart does it",
            pid, static_cast<unsigned long long>(unfinished.modules.size()), static_cast<unsigned long long>(unfinished.carried.size()));
    std::lock_guard<std::mutex> lock(unfinished_mutex_);
    if (unfinished_.find(pid) == unfinished_.end()) {
        unfinished_order_.push_back(pid);
    }
    unfinished_[pid] = std::move(unfinished);
    while (unfinished_order_.size() > 16) {
        unfinished_.erase(unfinished_order_.front());
        unfinished_order_.pop_front();
    }
}

std::optional<RestartCoordinator::Unfinished> RestartCoordinator::peek_unfinished(std::uint32_t pid) {
    std::lock_guard<std::mutex> lock(unfinished_mutex_);
    const auto it = unfinished_.find(pid);
    if (it == unfinished_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void RestartCoordinator::forget_unfinished(std::uint32_t pid) {
    std::lock_guard<std::mutex> lock(unfinished_mutex_);
    if (unfinished_.erase(pid) != 0) {
        unfinished_order_.erase(std::remove(unfinished_order_.begin(), unfinished_order_.end(), pid), unfinished_order_.end());
    }
}

bool RestartCoordinator::resume_requested_for_testing(std::uint32_t pid) {
    std::shared_ptr<Hold> hold;
    {
        std::lock_guard<std::mutex> lock(holds_mutex_);
        const auto it = holds_.find(pid);
        if (it == holds_.end()) {
            return false;
        }
        hold = it->second;
    }
    std::lock_guard<std::mutex> lock(hold->mutex);
    return hold->release_requested;
}

bool RestartCoordinator::has_unfinished(std::uint32_t pid) {
    std::lock_guard<std::mutex> lock(unfinished_mutex_);
    return unfinished_.find(pid) != unfinished_.end();
}

void RestartCoordinator::process_gone(std::uint32_t pid) {
    if (!has_unfinished(pid)) {
        return;
    }
    const std::shared_ptr<ProcessHost> h = host();
    if (h == nullptr || !h->has_exited(pid)) {
        CML_LOG(Info, "restart: the agent of process %u disconnected, the process has not exited; what its unfinished restart left is kept", pid);
        return;
    }
    CML_LOG(Info, "restart: process %u has exited; what its unfinished restart left is dropped", pid);
    forget_unfinished(pid);
}

bool RestartCoordinator::has_unfinished_for_testing(std::uint32_t pid) {
    return has_unfinished(pid);
}

Result<Value> RestartCoordinator::restart(const Value& args, const std::function<bool()>& client_gone) {
    std::uint32_t ran_for = 0;
    bool terminated = false;
    Result<Value> result = restart_impl(args, client_gone, &ran_for, &terminated);
    if (ran_for != 0) {
        // WP-43: an armed restart stage applies to this restart only, whether it fired or not.
        const std::vector<std::string> unused = finish_injection(service_, "restart", ran_for, result ? std::string() : result.error().format(), !terminated);
        if (result && !unused.empty()) {
            Value names = Value::array();
            for (const std::string& n : unused) {
                names.push(Value::string(n));
            }
            result->set("injection_disarmed", std::move(names));
        }
    }
    return result;
}

Result<Value> RestartCoordinator::restart_impl(const Value& args, const std::function<bool()>& client_gone, std::uint32_t* ran_for, bool* old_terminated) {
    std::uint32_t pid = 0;
    std::uint32_t timeout_ms = kDefaultTimeoutMs;
    std::optional<Diag> arg_error;
    if (!read_uint(args, "pid", 0xFFFFFFFFu, &pid, &arg_error) || !read_uint(args, "timeout_ms", 3600u * 1000u, &timeout_ms, &arg_error)) {
        return *arg_error;
    }
    std::uint32_t registration_ms = kDefaultRegistrationMs;
    if (!read_uint(args, "registration_timeout_ms", 3600u * 1000u, &registration_ms, &arg_error)) {
        return *arg_error;
    }
    // WP-34: hold the new process suspended (docs/wp/WP-34.md). Absent or false: today's behaviour, nothing below changes.
    bool hold_new = false;
    std::uint32_t hold_ms = ipc::restart_hold::kDefaultHoldMs;
    const auto bad_hold = [](const std::string& message) {
        return Diag::make(rd::kBadHoldArgs, message, "Use `{\"hold_new\":true, \"hold_ms\":N}` with hold_ms from 1 to 60000 (docs/wp/WP-34.md).");
    };
    if (const Value* held = args.find("hold_new"); held != nullptr && !held->is_null()) {
        if (!held->is_bool()) {
            return bad_hold("argument 'hold_new' must be true or false");
        }
        hold_new = held->as_bool();
    }
    std::string hold_token;  // echoed in `restarting`
    if (hold_new) {
        // `hold_ms` and `token` mean something only with hold_new (without it they are not looked at, as before WP-34).
        if (!read_uint_arg(args, "hold_ms", ipc::restart_hold::kMaxHoldMs, &hold_ms, &arg_error, bad_hold)) {
            return *arg_error;
        }
        if (const Value* given = args.find("hold_ms"); given != nullptr && !given->is_null() && hold_ms == 0) {
            return bad_hold("argument 'hold_ms' must be an integer from 1 to " + std::to_string(ipc::restart_hold::kMaxHoldMs));
        }
        if (const Value* token = args.find("token"); token != nullptr && !token->is_null()) {
            if (!token->is_string() || token->as_string().size() > 64) {
                return bad_hold("argument 'token' must be a string of at most 64 characters");
            }
            hold_token = token->as_string();
        }
    }
    if (timeout_ms == 0) {
        timeout_ms = kDefaultTimeoutMs;
    }
    if (registration_ms == 0) {
        registration_ms = kDefaultRegistrationMs;
    }
    const Clock::time_point start = Clock::now();
    // `timeout_ms` bounds everything before the old process is terminated; the budget after it is separate (reassigned there), so
    // a slow preparation cannot leave a killed process with no time to register.
    Clock::time_point deadline = start + std::chrono::milliseconds(timeout_ms);
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

    // The host this restart works with, even if it is replaced meanwhile (set_host).
    const std::shared_ptr<ProcessHost> host = this->host();
    if (host == nullptr) {
        return fail(Diag::make(rd::kUnavailable, "this broker cannot restart processes on this system", "Restart the application yourself; hot-restart needs the Windows broker."),
                    pid);
    }
    if (hold_new && !host->supports_hold()) {
        return fail(Diag::make(rd::kHoldUnavailable, "this broker cannot hold a new process (the process host cannot create it suspended)",
                               "Send `restart` without hold_new, or attach the debugger after `restarted`."),
                    pid);
    }

    // Reloads are held off for the whole restart: a patch applied to the old process after its baseline was taken would be lost in
    // the new one. A reload that is running now makes this restart busy; one that arrives meanwhile is refused busy (CML5304).
    std::unique_lock<std::mutex> only_one(mutex_, std::try_to_lock);
    std::unique_ptr<ExclusiveRun> no_reloads = service_.lock_reloads();  // the run lock of reloads and reverts (shows as running)
    if (!only_one.owns_lock() || !no_reloads) {
        return fail(Diag::make(rd::kBusy, "another restart or a reload is still running", "Wait for it to finish (control command `wait`) and try again."), pid);
    }

    struct ActiveScope {  // the broker does not exit as idle while a restart runs (the old process is gone, the new one not registered yet)
        explicit ActiveScope(std::atomic<int>& n) : n_(n) { ++n_; }
        ~ActiveScope() { --n_; }
        std::atomic<int>& n_;
    } active_scope(active_);

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
    *ran_for = old_pid;

    // ---- 1. the agent prepares: pre-restart hooks, launch parameters ----
    CML_LOG(Info, "restart: asking process %u to prepare", old_pid);
    ipc::Restart prepare;
    prepare.request_id = old_link->next_request_id();
    Result<ipc::Message> answer = old_link->request(prepare, std::max<std::uint32_t>(remaining_ms(), 1));
    if (!answer) {
        // Nobody will carry this restart out: the agent must not run its pre-restart hooks for it later (a job may still wait for
        // the application's next SyncPoint()).
        ipc::RestartCancel cancel;
        cancel.request_id = prepare.request_id;
        (void)old_link->send(cancel);
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
        // WP-43: a preparation that failed after the pre-restart hooks ran (the stage pre-restart-hooks) says how many ran.
        return fail(Diag::make(rd::kPrepareFailed,
                               "process " + std::to_string(old_pid) + " cannot be restarted: CML" + std::to_string(info->status) + " " + info->message +
                                   " (the process was not touched)" +
                                   (info->hooks_run == 0 || info->message.find("pre-restart hook(s) ran") != std::string::npos
                                        ? std::string()
                                        : "; " + std::to_string(info->hooks_run) + " pre-restart hook(s) ran"),
                               "See the agent's diagnostic for what to change."),
                    old_pid);
    }
    const std::string hooks_note = info->hooks_run == 0 ? std::string() : " " + std::to_string(info->hooks_run) + " pre-restart hook(s) ran; the process was not restarted.";
    if (info->image_path.empty() || info->command_line.empty() || !environment_block_ok(info->environment)) {
        // An environment that was not captured must not be replaced by "nothing" (no SystemRoot, no PATH): refuse before anything is killed.
        return fail(Diag::make(rd::kPrepareFailed, "process " + std::to_string(old_pid) + " reported no usable image path, command line or environment (the process was not touched)." + hooks_note,
                               "Use an agent from the same Chameleon build as the broker."),
                    old_pid);
    }
    CML_LOG(Info, "restart: process %u prepared (%u pre-restart hook(s) ran, %llu module(s) enabled)", old_pid, info->hooks_run,
            static_cast<unsigned long long>(info->modules.size()));

    if (trace_) {
        trace_("prepared");
    }
    // The modules to have enabled again: what the application enabled (with its options); the broker's own list when the
    // agent reports none.
    std::vector<ipc::RestartModule> modules = info->modules;
    if (modules.empty()) {
        for (const std::string& path : old_snap.enabled) {
            modules.push_back(ipc::RestartModule{0, path});
        }
    }
    // WP-43: what the restart that started this process could not do (its registration failed, or a module was not enabled again) is
    // done now: those modules are enabled again in the new process and their baselines go along with this restart's.
    // A copy: it is forgotten only once the old process is gone (a restart refused before that leaves it for the next one).
    std::optional<Unfinished> unfinished = peek_unfinished(old_pid);
    if (unfinished && unfinished->token != old_snap.restart_token) {
        // The pid was reused by another process (the record is dropped when its agent disconnects; this catches a process that exited
        // before it ever registered).
        CML_LOG(Info, "restart: process %u is not the one an unfinished restart started (another restart token); its record is dropped", old_pid);
        forget_unfinished(old_pid);
        unfinished.reset();
    }
    if (unfinished) {
        for (const ipc::RestartModule& m : unfinished->modules) {
            const bool listed = std::any_of(modules.begin(), modules.end(), [&](const ipc::RestartModule& have) { return path::same_path(have.path, m.path); });
            if (!listed) {
                modules.push_back(m);
            }
        }
        CML_LOG(Info, "restart: process %u was started by a restart that did not finish; its %llu module(s) and %llu baseline(s) are carried over again", old_pid,
                static_cast<unsigned long long>(unfinished->modules.size()), static_cast<unsigned long long>(unfinished->carried.size()));
    }

    // ---- 2. the old process goes, the new one starts ----
    const std::uint64_t std_handles[3] = {info->std_input, info->std_output, info->std_error};
    std::vector<Diag> open_warnings;
    Result<std::unique_ptr<RestartableProcess>> opened = host->open(old_pid, std_handles, info->flags, &open_warnings);
    for (const Diag& w : open_warnings) {
        warn(w, old_pid);
    }
    if (!opened) {
        return fail(Diag::make(rd::kOpenFailed, "cannot take over process " + std::to_string(old_pid) + ": " + opened.error().message + " (the process was not touched)." + hooks_note,
                               opened.error().what_to_do),
                    old_pid);
    }
    std::unique_ptr<RestartableProcess> process = std::move(*opened);

    // Unique per restart (the token only matches a Hello to the restart that started it; it is not a secret).
    const std::string token = hex(hash64(std::to_string(old_pid) + ":" + std::to_string(Clock::now().time_since_epoch().count()) + ":" +
                                         std::to_string(++counter_)))
                                  .substr(2) +
                              hex(static_cast<std::uint64_t>(Clock::now().time_since_epoch().count())).substr(2);
    RelaunchSpec spec;
    spec.image_path = info->image_path;
    spec.command_line = info->command_line;
    spec.working_directory = info->working_directory;
    spec.environment = environment_with_token(info->environment, token);
    spec.suspended = hold_new;

    // What to carry over: the baselines of the enabled modules (their indexes die with the connection). Taken now, after the
    // preparation and with reloads held off, so every patch applied to the old process is in it.
    std::vector<Carried> carried;
    for (const std::shared_ptr<ModuleState>& state : service_.module_states(old_snap.agent_id)) {
        // WP-45: the index's baseline (Ready) with what the state was given to re-apply and has not restored yet (a re-apply that is
        // queued behind this restart's run lock), or only the latter while its index is not ready.
        if (std::optional<BaselineSnapshot> baseline = service_.leaving_baseline(*state)) {
            carried.push_back(Carried{state->path, std::move(*baseline)});
        }
    }
    // WP-45: a module of the old process that is not enabled at the moment (it was freed, or disabled) left its baseline in the
    // service; it travels too, so the new process gets those edits when it enables the module.
    for (auto& kept : service_.kept_baselines(old_pid)) {
        const bool have = std::any_of(carried.begin(), carried.end(), [&](const Carried& c) { return path::same_path(c.path, kept.first); });
        CML_LOG(Trace, "restart: decision=%s module=%s pid=%u originals=%zu", have ? "kept-baseline-ignored" : "kept-baseline-carried", kept.first.c_str(), old_pid,
                kept.second.originals.size());
        if (!have) {
            carried.push_back(Carried{kept.first, std::move(kept.second)});
        }
    }
    if (unfinished) {
        // The older baseline knows what the original binary was built from (its originals); the newer one, of a process that never got the
        // old baseline restored, took the edited files as its originals. The older originals win; the newer stamps stay.
        for (Carried& older : unfinished->carried) {
            const auto same = std::find_if(carried.begin(), carried.end(), [&](const Carried& c) { return path::same_path(c.path, older.path); });
            if (same == carried.end()) {
                carried.push_back(std::move(older));
                continue;
            }
            for (const auto& original : older.baseline.originals) {
                same->baseline.originals[original.first] = original.second;
            }
            for (const auto& stamp : older.baseline.stamps) {
                same->baseline.stamps.emplace(stamp.first, stamp.second);  // only where the newer baseline has none
            }
        }
    }
    // What a restart that ends after the old process is gone hands to the next restart of the new one.
    const auto unfinished_for = [&](const std::vector<ipc::RestartModule>& not_done) {
        Unfinished u;
        u.token = token;
        u.modules = not_done;
        for (const Carried& c : carried) {
            const bool affected = std::any_of(not_done.begin(), not_done.end(), [&](const ipc::RestartModule& m) {
                return path::same_path(m.path, c.path) || path::matches_module(m.path, c.path) || path::matches_module(c.path, m.path);
            });
            if (affected) {
                u.carried.push_back(c);
            }
        }
        return u;
    };


    if (trace_) {
        trace_("snapshot");
        trace_("terminate");
    }
    // WP-43 (R-40): a broker that is shutting down does not terminate anything any more.
    if (aborting_.load()) {
        return fail(Diag::make(rd::kShutdown, "the broker is shutting down; process " + std::to_string(old_pid) + " was not restarted (it was not touched)." + hooks_note,
                               "Start the broker again and send `restart` again."),
                    old_pid);
    }
    if (std::optional<Diag> injected = service_.faults().hit(ipc::fault::Stage::RestartTerminate, old_pid)) {
        injected->message += "; process " + std::to_string(old_pid) + " was not terminated." + hooks_note;
        return fail(*injected, old_pid);
    }
    CML_LOG(Info, "restart: terminating process %u", old_pid);
    Result<void> terminated = process->terminate(kTerminateTimeoutMs);
    if (!terminated) {
        return fail(Diag::make(rd::kTerminateFailed, "process " + std::to_string(old_pid) + " could not be terminated: " + terminated.error().message,
                               "Close it yourself and start the application again."),
                    old_pid);
    }
    *old_terminated = true;
    if (unfinished) {
        forget_unfinished(old_pid);  // the old process is gone: what was remembered for it travels with this restart now
    }
    deadline = Clock::now() + std::chrono::milliseconds(registration_ms);  // the post-termination budget starts here
    // WP-43: the old process is gone, so a failed start is tried once more (after a short pause: a file of the image may still be held
    // by the process that just ended, or by a scanner). The injected failure (stage restart-relaunch) takes the place of the first attempt.
    std::optional<Diag> first_attempt;
    std::optional<std::uint32_t> launched;
    if (std::optional<Diag> injected = service_.faults().hit(ipc::fault::Stage::RestartRelaunch, old_pid)) {
        first_attempt = std::move(*injected);
    } else {
        Result<std::uint32_t> attempt = process->relaunch(spec);
        if (attempt) {
            launched = *attempt;
        } else {
            first_attempt = attempt.error();
        }
    }
    if (!launched && process->relaunch_created_process()) {
        // The attempt had created a process and a later step of it failed; the host terminated it. Never start a second copy.
        return fail(Diag::make(rd::kLaunchFailed,
                               "process " + std::to_string(old_pid) + " was terminated; the new one was created but could not be started (" + first_attempt->message +
                                   "), so it was terminated and no second attempt is made",
                               "Start the application yourself; its agent registers again by itself."),
                    old_pid);
    }
    if (!launched) {
        // No process exists (CreateProcess failed, or the injected failure took the place of the attempt): try once more.
        if (first_attempt->code == ipc::fault::ids::kRestartRelaunch) {
            first_attempt->message += "; process " + std::to_string(old_pid) + " is gone, trying once more to start the new one";
            warn(*first_attempt, old_pid);
        } else {
            warn(Diag::make(rd::kRelaunchRetried,
                            "process " + std::to_string(old_pid) + " was terminated and the first attempt to start the new one failed (" + first_attempt->format() +
                                "); trying once more",
                            "Nothing to do if the next attempt works; otherwise start the application yourself."),
                 old_pid);
        }
        std::this_thread::sleep_for(relaunch_retry_delay_);
        Result<std::uint32_t> second = process->relaunch(spec);
        if (!second) {
            return fail(Diag::make(rd::kLaunchFailed,
                                   "process " + std::to_string(old_pid) + " was terminated but the new one could not be started (2 attempts): " + second.error().message,
                                   "Start the application yourself; its agent registers again by itself."),
                        old_pid);
        }
        launched = *second;
        CML_LOG(Info, "restart: the second attempt started the new process (pid %u)", *launched);
    }
    CML_LOG(Info, "restart: started process %u (token %s)%s, waiting for its agent", *launched, token.c_str(), hold_new ? " suspended" : "");

    // ---- 2b. WP-34: the new process is suspended until the client resumes it, hold_ms passes, or something goes wrong ----
    // It has not run a single instruction, so its agent does not exist yet and cannot register: the registration budget starts at
    // the resume. Every way out of this block resumes the process first (`end_hold`).
    if (hold_new) {
        const std::uint32_t held_pid = *launched;
        const auto hold = std::make_shared<Hold>();
        {
            std::lock_guard<std::mutex> lock(holds_mutex_);
            holds_[held_pid] = hold;
            if (ended_holds_.erase(held_pid) != 0) {
                ended_order_.erase(std::remove(ended_order_.begin(), ended_order_.end(), held_pid), ended_order_.end());
            }
        }
        const auto end_hold = [&](const char* how) -> Result<void> {
            Result<void> resumed = process->resume();
            {
                std::lock_guard<std::mutex> lock(holds_mutex_);
                holds_.erase(held_pid);
                if (ended_holds_.find(held_pid) == ended_holds_.end()) {
                    ended_order_.push_back(held_pid);
                } else {
                    ended_order_.erase(std::remove(ended_order_.begin(), ended_order_.end(), held_pid), ended_order_.end());
                    ended_order_.push_back(held_pid);
                }
                ended_holds_[held_pid] = how;
                while (ended_order_.size() > 16) {  // the oldest goes first, not the lowest pid
                    ended_holds_.erase(ended_order_.front());
                    ended_order_.pop_front();
                }
            }
            {
                std::lock_guard<std::mutex> lock(hold->mutex);
                hold->resumed = true;
                if (!resumed) {
                    hold->resume_error = resumed.error();
                }
            }
            hold->changed.notify_all();
            return resumed;
        };
        // WP-43 (stage restart-hold): the hold cannot be kept. The process is resumed at once, without a `restarting` event (nobody is asked
        // to attach to a process that is not held), and the restart goes on; a debugger attaches after `restarted`.
        std::optional<Diag> not_kept = service_.faults().hit(ipc::fault::Stage::RestartHold, old_pid);
        if (!not_kept && events_ != nullptr) {
            Value body = Value::object();
            body.set("event", Value::string(ipc::restart_hold::kRestartingEvent));
            body.set("old_pid", Value::integer(old_pid));
            body.set("new_pid", Value::integer(held_pid));
            body.set("hold_ms", Value::integer(hold_ms));
            if (!hold_token.empty()) {
                body.set("token", Value::string(hold_token));
            }
            Event event;
            event.name = ipc::restart_hold::kRestartingEvent;
            event.body = std::move(body);
            events_->publish(event);
        }
        if (!not_kept) {
            if (trace_) {
                trace_("held");
            }
            CML_LOG(Info, "restart: process %u is held suspended for at most %u ms", held_pid, hold_ms);
        }
        enum class End { Requested, Expired, ClientGone, Shutdown, Exited, NotKept };
        End end = not_kept ? End::NotKept : End::Expired;
        std::optional<std::uint32_t> exit_code;
        const Clock::time_point hold_end = Clock::now() + std::chrono::milliseconds(hold_ms);
        while (!not_kept) {
            {
                std::unique_lock<std::mutex> lock(hold->mutex);
                hold->changed.wait_for(lock, std::chrono::milliseconds(kPollMs), [&] { return hold->release_requested; });
                if (hold->release_requested) {
                    end = End::Requested;
                    break;
                }
            }
            if (aborting_.load()) {
                end = End::Shutdown;
                break;
            }
            if (client_gone && client_gone()) {
                end = End::ClientGone;
                break;
            }
            exit_code = process->relaunched_exit_code();
            if (exit_code.has_value()) {
                end = End::Exited;
                break;
            }
            if (Clock::now() >= hold_end) {
                end = End::Expired;
                break;
            }
        }
        // A `restart-resume` that races with the end of the hold: whichever gets the hold's mutex first decides. If the release was requested in
        // time it counts (the answer `resumed:true` is then true); otherwise the reason is recorded *before* the resume, so `restart-resume`
        // answers CML5814 with it instead of claiming it resumed the process. WP-43 (R-40): the check and the record are one critical section;
        // a `restart-resume` that came between two sections used to answer `resumed:true` next to a CML5813.
        if (trace_) {
            trace_("hold-ending");
        }
        const char* how = nullptr;
        {
            std::lock_guard<std::mutex> lock(hold->mutex);
            if (hold->release_requested) {
                end = End::Requested;
            }
            how = end == End::Requested ? "restart-resume" : end == End::Expired ? "hold_ms expired" : end == End::ClientGone ? "client disconnected"
                  : end == End::Shutdown ? "broker shutdown" : end == End::NotKept ? "the hold could not be kept" : "process exited";
            if (end != End::Requested) {
                hold->ended_by = how;
            }
        }
        Result<void> resumed = end_hold(how);
        if (end == End::Exited) {
            return fail(Diag::make(rd::kNewProcessExited,
                                   "the new process (pid " + std::to_string(held_pid) + ") exited with code " + std::to_string(*exit_code) + " while it was held, before its agent registered",
                                   "A debugger or another tool ended it; run the application by hand to see why it stops. The old process is already gone."),
                        held_pid);
        }
        if (end == End::Shutdown) {
            return fail(Diag::make(rd::kHoldAborted, "the broker is shutting down; process " + std::to_string(held_pid) + " was resumed and the restart ended before its agent registered",
                                   "Nothing to do: the process runs without Chameleon until a broker is started and its agent connects."),
                        held_pid);
        }
        if (!resumed) {
            return fail(Diag::make(rd::kResumeFailed, "process " + std::to_string(held_pid) + " could not be resumed: " + resumed.error().message,
                                   "The old process is gone and the new one was terminated rather than left suspended; start the application yourself."),
                        held_pid);
        }
        if (end == End::Expired) {
            warn(Diag::make(rd::kHoldExpired, "nobody resumed process " + std::to_string(held_pid) + " within " + std::to_string(hold_ms) + " ms; the broker resumed it",
                            "Send `restart-resume` as soon as the debugger is attached, or raise `hold_ms` (at most 60000)."),
                 held_pid);
        } else if (end == End::ClientGone) {
            warn(Diag::make(rd::kHoldClientGone, "the client that asked for the restart disconnected while process " + std::to_string(held_pid) + " was held; it was resumed and the restart continues",
                            "Nothing to do; reconnect and use `status`. A debugger the client meant to attach has to be attached by hand."),
                 held_pid);
        } else if (end == End::NotKept) {
            not_kept->message += "; process " + std::to_string(held_pid) + " was resumed at once, without waiting for the debugger, and the restart goes on";
            not_kept->what_to_do = "Attach the debugger after `restarted`, or by hand. " + not_kept->what_to_do;
            warn(*not_kept, held_pid);
        }
        if (trace_) {
            trace_("resumed");
        }
        CML_LOG(Info, "restart: process %u resumed (%s)", held_pid, how);
        deadline = Clock::now() + std::chrono::milliseconds(registration_ms);  // the time held does not count against the registration budget
    }

    // ---- 3. the new agent registers ----
    // A process left running without its restart finished (no registration in time, an injected failure, the broker stopping) has its
    // modules and baselines remembered: when it registers by itself later, its next `restart` enables them and re-applies the edits (WP-43).
    const std::string left_running = "; its next `restart` enables its modules and re-applies the session's edits";
    if (std::optional<Diag> injected = service_.faults().hit(ipc::fault::Stage::RestartRegistration, old_pid)) {
        remember_unfinished(*launched, unfinished_for(modules));
        injected->message += "; the new process (pid " + std::to_string(*launched) + ") was left running, not taken over by the restart" + left_running;
        return fail(*injected, *launched);
    }
    std::shared_ptr<AgentLink> new_link;
    for (;;) {
        new_link = service_.link_for_restart_token(token, *launched);
        if (new_link) {
            break;
        }
        if (const std::optional<std::uint32_t> code = process->relaunched_exit_code()) {
            return fail(Diag::make(rd::kNewProcessExited,
                                   "the new process (pid " + std::to_string(*launched) + ") exited with code " + std::to_string(*code) + " before its agent registered",
                                   "Run the application by hand to see why it stops; the old process is already gone."),
                        *launched);
        }
        if (aborting_.load()) {  // WP-43 (R-40): shutdown does not wait for the registration
            return fail(Diag::make(rd::kShutdown, "the broker is shutting down; the new process (pid " + std::to_string(*launched) + ") was left running before its agent registered",
                                   "Nothing to do: the process runs without Chameleon until a broker is started and its agent connects."),
                        *launched);
        }
        if (remaining_ms() == 0) {
            remember_unfinished(*launched, unfinished_for(modules));
            return fail(Diag::make(rd::kNoRegistration,
                                   "the new process (pid " + std::to_string(*launched) + ") did not register within " + std::to_string(registration_ms) +
                                       " ms; it was left running" + left_running + " once it has registered",
                                   "Check that the application creates its agent at start-up (and that it can reach the broker); raise `registration_timeout_ms` if it starts slowly."),
                        *launched);
        }
        pause_ms(kPollMs);
    }
    const auto shutting_down = [&](const std::uint32_t for_pid) {
        return Diag::make(rd::kShutdown,
                          "the broker is shutting down; the new process (pid " + std::to_string(for_pid) + ") runs, but its restart ended before its modules were enabled and its edits re-applied",
                          "Nothing to do: the process runs without Chameleon's patches until a broker is started and it is reloaded.");
    };
    const std::uint32_t new_pid = new_link->snapshot().pid;
    const std::uint64_t new_agent = new_link->agent_id();
    CML_LOG(Info, "restart: process %u registered as agent#%llu", new_pid, static_cast<unsigned long long>(new_agent));

    // ---- 4. the same modules, enabled again ----
    // The module list arrives right after the handshake; EnableModule needs it.
    while (new_link->snapshot().modules.empty() && remaining_ms() > 0 && !aborting_.load()) {
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
    while (!missing_in(new_link->snapshot()).empty() && Clock::now() < grace_end && !aborting_.load()) {
        pause_ms(kPollMs);
    }
    if (aborting_.load()) {
        return fail(shutting_down(new_pid), new_pid);
    }
    std::vector<ipc::RestartModule> missing = missing_in(new_link->snapshot());
    std::optional<Diag> reenable_injected;
    if (!missing.empty()) {
        reenable_injected = service_.faults().hit(ipc::fault::Stage::RestartReenable, old_pid);  // WP-43: the request is not sent
    }
    if (reenable_injected) {
        reenable_injected->message += "; the request to enable " + std::to_string(missing.size()) + " module(s) again was not sent to process " + std::to_string(new_pid);
        warn(*reenable_injected, new_pid);
    } else if (!missing.empty()) {
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
        while (!missing_in(new_link->snapshot()).empty() && Clock::now() < enable_end && !aborting_.load()) {
            pause_ms(kPollMs);
        }
        missing = missing_in(new_link->snapshot());
    }
    // WP-45: which missing module each carried baseline belongs to, so that its edited files are counted once (in the CML5809 of the
    // first module it matches: the matching is by name in both directions, so one baseline can match several entries).
    std::vector<std::size_t> owner(carried.size(), missing.size());
    for (std::size_t k = 0; k < carried.size(); ++k) {
        for (std::size_t i = 0; i < missing.size() && owner[k] == missing.size(); ++i) {
            const std::string& m = missing[i].path;
            if (path::same_path(m, carried[k].path) || path::matches_module(m, carried[k].path) || path::matches_module(carried[k].path, m)) {
                owner[k] = i;
            }
        }
    }
    Value not_enabled = Value::array();
    for (std::size_t i = 0; i < missing.size(); ++i) {
        const ipc::RestartModule& m = missing[i];
        not_enabled.push(Value::string(m.path));
        std::size_t edited = 0;
        for (std::size_t k = 0; k < carried.size(); ++k) {
            edited += owner[k] == i ? carried[k].baseline.originals.size() : 0;
        }
        warn(Diag::make(rd::kModuleNotEnabled, "module '" + m.path + "' is not enabled in the new process (pid " + std::to_string(new_pid) + ")" +
                            (reenable_injected ? left_running : std::string()) +
                            (edited != 0 ? "; the session's edits of its " + std::to_string(edited) + " file(s) are re-applied when it is enabled" : std::string()),
                        "It may not be loaded yet; enable it from the application once it is."),
             new_pid);
    }
    if (!missing.empty()) {
        remember_unfinished(new_pid, unfinished_for(missing));  // WP-43: the next restart of the new process asks for them again
    }

    // The enabled modules of the new process, and their PDB indexes.
    const LinkSnapshot new_snap = new_link->snapshot();
    // WP-45 (F-42.3): every carried baseline of a module the new process has not enabled (a module it loads lazily, one the restart
    // could not enable, and one the old process had freed or disabled, whose baseline the service kept) goes to the new process: its
    // enable re-applies the edits. Each once. A module enabled since `missing` was computed is in new_snap and is restored below.
    for (const Carried& c : carried) {
        const bool enabled_now = std::any_of(new_snap.enabled.begin(), new_snap.enabled.end(), [&](const std::string& full) { return path::same_path(full, c.path); });
        CML_LOG(Trace, "restart: decision=%s module=%s pid=%u originals=%zu", enabled_now ? "restore-now" : "carry-to-late-enable", c.path.c_str(), new_pid,
                c.baseline.originals.size());
        if (!enabled_now) {
            service_.carry_to_late_module(new_agent, new_pid, c.path, c.baseline, "restart");
        }
    }
    Value enabled_json = Value::array();
    std::size_t restored = 0;
    for (const std::string& full : new_snap.enabled) {
        enabled_json.push(Value::string(full));
        std::shared_ptr<ModuleState> state;
        std::shared_ptr<ModuleIndex> index;
        while (remaining_ms() > 0 && !aborting_.load()) {
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
        // In slices, so that a broker that shuts down does not wait for an index build (WP-43, R-40).
        bool ready = false;
        while (index != nullptr) {
            ready = index->wait(std::min<std::uint32_t>(remaining_ms(), 100));
            if (ready || remaining_ms() == 0 || aborting_.load()) {
                break;
            }
        }
        if (aborting_.load()) {
            return fail(shutting_down(new_pid), new_pid);
        }
        if (!index || !ready || index->status().state != IndexState::Ready) {
            warn(Diag::make(rd::kReapplyFailed, "the PDB index of '" + full + "' in the new process is not ready, so its edits cannot be re-applied",
                            "Run `reload` once its index is ready."),
                 new_pid);
            continue;
        }
        // ---- 5. the edits of the session: the baseline goes back to the original binary's sources ----
        for (const Carried& c : carried) {
            if (path::same_path(c.path, full)) {
                restored += index->restore_session_baseline(c.baseline);
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
        {
            // WP-43 (stage restart-reapply): the reload fails before it compiles; the new process runs the original code, its baseline is
            // restored, so the next `reload` (or `restart`) applies the edits.
            std::optional<Diag> injected = service_.faults().hit(ipc::fault::Stage::RestartReapply, old_pid);
            Result<Value> reloaded = injected ? Result<Value>(*injected) : service_.reload_locked(reload_args);  // this restart holds the reload lock
            if (reloaded) {
                result.set("reapplied_files", Value::integer(static_cast<std::int64_t>(restored)));
                result.set("patched", Value::integer(reloaded->get_integer("patched")));
                if (const Value* changed = reloaded->find("changed_files")) {
                    result.set("changed_files", *changed);
                }
            } else {
            Value error = Value::object();
            error.set("id", Value::string(reloaded.error().id()));
            error.set("message", Value::string(reloaded.error().message));
            result.set("reapply_error", std::move(error));
            warn(Diag::make(rd::kReapplyFailed, "the new process (pid " + std::to_string(new_pid) + ") runs the original code: re-applying the session's edits failed: " +
                                                    reloaded.error().format(),
                            "Fix the error and run `reload`."),
                 new_pid);
            }
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

Result<Value> RestartCoordinator::resume(const Value& args) {
    const Value* pid_value = args.find("pid");
    if (pid_value == nullptr || !pid_value->is_integer() || pid_value->as_integer() <= 0 || pid_value->as_integer() > 0xFFFFFFFFll) {
        return Diag::make(rd::kBadHoldArgs, "`restart-resume` needs the integer argument 'pid' (the new_pid of the `restarting` event)",
                          "Send `{\"pid\":N}` with the new_pid of the `restarting` event.");
    }
    const auto pid = static_cast<std::uint32_t>(pid_value->as_integer());
    std::shared_ptr<Hold> hold;
    std::string ended;
    {
        std::lock_guard<std::mutex> lock(holds_mutex_);
        const auto it = holds_.find(pid);
        if (it != holds_.end()) {
            hold = it->second;
        } else if (const auto done = ended_holds_.find(pid); done != ended_holds_.end()) {
            ended = done->second;
        }
    }
    if (!hold) {
        return Diag::make(rd::kNotHeld,
                          ended.empty() ? "process " + std::to_string(pid) + " is not held by a restart"
                                        : "process " + std::to_string(pid) + " is not held any more (it was resumed: " + ended + ")",
                          "Send `restart-resume` with the new_pid of a `restarting` event, while its restart is waiting; a process resumed by the broker needs nothing more.");
    }
    std::unique_lock<std::mutex> lock(hold->mutex);
    if (!hold->ended_by.empty()) {
        return Diag::make(rd::kNotHeld, "process " + std::to_string(pid) + " is not held any more (it was resumed: " + hold->ended_by + ")",
                          "A process resumed by the broker needs nothing more; this answer is harmless.");
    }
    hold->release_requested = true;
    hold->changed.notify_all();
    // WP-43 (R-40): no confirmation in time is its own id (CML5820), not CML5817: nothing is known to have failed, the resume is still asked
    // for (the restart thread acts on it when it gets to it), and the broker resumes the process by itself when hold_ms passes.
    if (!hold->changed.wait_for(lock, confirm_wait_, [&] { return hold->resumed; })) {
        return Diag::make(rd::kResumeUnconfirmed,
                          "the restart did not confirm within " + std::to_string(confirm_wait_.count()) + " ms that process " + std::to_string(pid) +
                              " runs; the resume is still requested, and the broker resumes the process by itself when hold_ms passes",
                          "Query `status` (or wait for `restarted`); send `restart-resume` again if the process is still held.");
    }
    if (hold->resume_error) {
        return *hold->resume_error;
    }
    Value result = Value::object();
    result.set("pid", Value::integer(pid));
    result.set("resumed", Value::boolean(true));
    result.set("held_ms", Value::integer(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - hold->since).count()));
    return result;
}

void RestartCoordinator::abort_holds() {
    aborting_.store(true);
}

#if !defined(_WIN32)
std::shared_ptr<ProcessHost> native_process_host() {
    return nullptr;
}
#endif

}  // namespace cml::core
