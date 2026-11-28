#include "cml/core/reload_service.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <set>
#include <thread>

#include "cml/core/control_args.h"
#include "cml/core/header_diag_ids.h"
#include "cml/core/pipeline_diag_ids.h"
#include "cml/core/process_reload.h"
#include "cml/core/process_registry.h"
#include "cml/core/reenable_diag_ids.h"
#include "cml/core/restart.h"
#include "cml/core/restart_diag_ids.h"
#include "cml/core/revert.h"
#include "cml/core/revert_diag_ids.h"
#include "cml/util/log.h"
#include "cml/util/path.h"
#include "cml/watch/watch_diag_ids.h"

namespace cml::core {

namespace fs = std::filesystem;
namespace pd = pipeline::diag;
using Clock = std::chrono::steady_clock;
using ipc::json::Value;

namespace {

Diag bad_request(const std::string& message) {
    return Diag::make(pd::kBadRequest, message, "Check the command's arguments (docs/wp/WP-12.md, \"Control commands\").");
}

// Reads an optional unsigned integer argument; false (and *error set) when present with a wrong type or range.
bool read_uint(const Value& args, const char* key, std::uint32_t max, std::uint32_t* out, std::optional<Diag>* error) {
    return read_uint_arg(args, key, max, out, error, bad_request);
}

std::string normalized_key(const std::string& p) { return normalized_path_key(p); }

// Value of an environment variable ("" when unset); _dupenv_s on Windows (getenv is deprecated there, /WX).
std::string env_value(const char* name) {
#if defined(_WIN32)
    char* buffer = nullptr;
    std::size_t size = 0;
    std::string out;
    if (_dupenv_s(&buffer, &size, name) == 0 && buffer != nullptr) {
        out = buffer;
        std::free(buffer);
    }
    return out;
#else
    const char* v = std::getenv(name);
    return v != nullptr ? v : "";
#endif
}

// ---- WP-45 helpers ----

// `older` (what an earlier instance was given) under `newer` (what this instance's index holds now): the older originals win (they
// are what the module's binary was built from), the newer stamps stay. The rule of the restart's unfinished records (restart.cpp).
void merge_older(BaselineSnapshot& newer, const BaselineSnapshot& older) {
    for (const auto& original : older.originals) {
        newer.originals[original.first] = original.second;
    }
    for (const auto& stamp : older.stamps) {
        newer.stamps.emplace(stamp.first, stamp.second);
    }
}

// "a.cpp, b.cpp" (base names; at most five, then "and N more").
std::string file_list(const std::vector<std::string>& files) {
    std::string out;
    std::size_t shown = 0;
    for (const std::string& f : files) {
        if (shown == 5) {
            out += ", and " + std::to_string(files.size() - shown) + " more";
            break;
        }
        out += (shown == 0 ? "" : ", ") + std::string(path::base_name(std::string_view(f)));
        ++shown;
    }
    return out.empty() ? std::string("(none)") : out;
}

// Which process an agent is: its restart token, its command line and its creation time (read by the broker through the process host;
// empty when it cannot be read). With the pid, the creation time is what tells a process from a later one that reuses the pid; the
// token and the command line only add to it.
std::string process_identity(const LinkSnapshot& snap, const std::optional<std::uint64_t>& created) {
    return snap.restart_token + "\n" + snap.command_line + "\n" + (created ? std::to_string(*created) : std::string());
}

}  // namespace

ReloadService::ReloadService(PipelineEnv env, EventBus* events, std::string session_dir)
    : pipeline_(std::move(env)), events_(events), session_dir_(std::move(session_dir)) {
    // A directory the user chose (--session-dir, $CML_SESSION_DIR) is theirs: it is never removed. Only the
    // temporary default is.
    keep_session_dir_ = !session_dir_.empty();
    set_process_host(native_process_host());  // WP-21: null on hosts that cannot restart processes
}

ReloadService::~ReloadService() {
    {
        // WP-45: first the re-apply worker's stop flag (it starts no reload once it is set), then cancel() aborts a reload it runs.
        std::lock_guard<std::mutex> lock(reapply_mutex_);
        reapply_stop_ = true;
    }
    reapply_changed_.notify_all();
    {
        std::lock_guard<std::mutex> lock(prefetch_mutex_);  // WP-50: no new background read; one in flight finishes
        prefetch_stop_ = true;
        prefetch_queue_.clear();
    }
    prefetch_changed_.notify_all();
    cancel();
    if (reapply_thread_.joinable()) {
        reapply_thread_.join();
    }
    if (prefetch_thread_.joinable()) {
        prefetch_thread_.join();
    }
    {
        std::shared_ptr<SaveWatcher> watcher;
        {
            std::lock_guard<std::mutex> lock(watch_mutex_);
            watcher.swap(save_watcher_);
        }
        if (watcher) {
            watcher->stop();
        }
        watcher.reset();  // joins its thread (a running reload was cancelled above)
        std::vector<std::future<void>> retiring;
        {
            std::lock_guard<std::mutex> lock(watch_mutex_);
            retiring.swap(retiring_);
        }
        for (std::future<void>& f : retiring) {
            f.wait();
        }
    }
    std::map<std::uint64_t, AgentEntry> agents;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        agents.swap(agents_);
    }
    agents.clear();  // joins the index threads outside the lock
    if (session_dir_made_ && !keep_session_dir_ && env_value("CML_KEEP_SESSION").empty()) {
        std::error_code ec;
        fs::remove_all(fs::path(session_dir_), ec);  // patch DLLs still loaded in a process cannot be removed: ignored
    }
}

Result<std::string> ReloadService::ensure_session_dir() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_dir_made_) {
        return session_dir_;
    }
    std::error_code ec;
    if (session_dir_.empty()) {
        if (const std::string base = env_value("CML_SESSION_DIR"); !base.empty()) {
            session_dir_ = base;
            keep_session_dir_ = true;  // the user asked for a place to look at: leave it
        } else {
            const auto stamp = static_cast<unsigned long long>(Clock::now().time_since_epoch().count());
            session_dir_ = (fs::temp_directory_path(ec) / ("chameleon-" + std::to_string(stamp % 1000000007ull))).string();
        }
    }
    fs::create_directories(fs::path(session_dir_), ec);
    if (ec) {
        return Diag::make(pd::kWorkDir, "cannot create the session directory '" + session_dir_ + "': " + ec.message(), "Check the TEMP directory (free space, permissions).");
    }
    session_dir_made_ = true;
    CML_LOG(Info, "session directory: %s", session_dir_.c_str());
    return session_dir_;
}

// ---- connections ---------------------------------------------------------------------------------------

void ReloadService::attach(std::shared_ptr<AgentLink> link) {
    const std::uint64_t id = link->agent_id();
    std::lock_guard<std::mutex> lock(mutex_);
    agents_[id].link = std::move(link);
}

void ReloadService::detach(std::uint64_t agent_id) {
    AgentEntry entry;
    std::shared_ptr<RestartCoordinator> coordinator;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = agents_.find(agent_id);
        if (it == agents_.end()) {
            return;
        }
        entry = std::move(it->second);
        agents_.erase(it);
        coordinator = restart_;
    }
    // One exit check for everything below (RestartCoordinator::process_exited: WP-44's short, bounded re-check, because the pipe can break
    // a moment before the process handle is signalled), asked only when something depends on it, so a disconnect waits at most once.
    // Not while the broker shuts down (cancel()): then nothing is dropped.
    if (entry.link != nullptr) {
        const LinkSnapshot snap = entry.link->snapshot();
        if (snap.handshaken && snap.pid != 0) {
            std::optional<bool> exited;
            const auto has_exited = [&]() {
                if (!exited) {
                    exited = coordinator != nullptr && coordinator->process_exited(snap.pid);
                }
                return *exited;
            };
            // WP-45: what this process's module instances left is for later instances in the same process, so it goes with the process,
            // not with the connection: an agent that reconnects to the same live process keeps it (on_session_changed then keeps it only
            // for a proven identity). Dropped here when the process is known to have exited.
            bool has_carries = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                has_carries = std::any_of(carries_.begin(), carries_.end(), [&](const auto& c) { return c.first.first == snap.pid; });
            }
            if (has_carries && !cancel_.cancelled()) {
                const bool gone = has_exited();
                std::lock_guard<std::mutex> lock(mutex_);
                if (gone) {
                    drop_carries_locked(snap.pid, "the process has exited");
                } else {
                    CML_LOG(Trace, "reenable: decision=keep-carries pid=%u reason=%s", snap.pid, coordinator == nullptr ? "exit-unknown" : "process-running");
                }
            }
            // WP-43: an unfinished restart's record for this process must not outlive it (its pid may be reused). R-51: neither must what
            // inject-failure armed in its agent (a new process with the same pid would report it as `injection_disarmed`), but only once the
            // process has really exited: a pipe that broke, or a broker that shuts down (cancel()), leaves the agent armed, so the records
            // (and the persisted ones) stay.
#if defined(CML_FAULT_INJECTION)
            if (coordinator != nullptr && !cancel_.cancelled() && faults_.has_process_records(snap.pid)) {
                if (has_exited()) {
                    faults_.forget_process(snap.pid);
                }
            }
#endif
            if (coordinator != nullptr) {
                coordinator->process_gone(snap.pid, exited);
            }
        }
    }
    for (auto& module : entry.modules) {
        module.second->release_image();  // WP-50
    }
    entry.modules.clear();  // index threads are joined here, outside the lock
    rearm_save_watcher();
}

#if defined(CML_FAULT_INJECTION)
bool ReloadService::process_exited(std::uint32_t pid) {
    std::shared_ptr<RestartCoordinator> coordinator;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        coordinator = restart_;
    }
    return coordinator != nullptr && coordinator->process_exited(pid);
}
#endif

void ReloadService::on_session_changed(std::uint64_t agent_id) {
    std::shared_ptr<AgentLink> link;
    ReloadOptions options;
    std::shared_ptr<ProcessHost> host;
    bool registered = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = agents_.find(agent_id);
        if (it == agents_.end()) {
            return;
        }
        link = it->second.link;
        options = options_;
        host = process_host_;
        registered = it->second.registered;
    }
    const LinkSnapshot snap = link->snapshot();
    // WP-45: which process this agent is (restart token, command line, creation time), determined once, when it has registered. Without
    // a creation time the identity is not proven, and nothing kept for its pid by another agent can be given to it.
    std::string identity;
    bool identity_known = false;
    if (!registered && snap.handshaken) {
        const std::optional<std::uint64_t> created = host != nullptr && snap.pid != 0 ? host->creation_time(snap.pid) : std::nullopt;
        identity = process_identity(snap, created);
        identity_known = created.has_value();
    }
    // The modules that should have state: enabled and loaded, with the address the agent reports.
    std::map<std::string, LoadedModule> desired;
    for (const std::string& enabled : snap.enabled) {
        for (const LoadedModule& m : snap.modules) {
            if (path::same_path(m.path, enabled)) {
                desired[normalized_key(m.path)] = m;
                break;
            }
        }
    }
    // WP-45: whether the image a state was made for is still loaded (same path, same base).
    const auto image_loaded = [&snap](const std::string& module_path, std::uint64_t base) {
        return std::any_of(snap.modules.begin(), snap.modules.end(),
                           [&](const LoadedModule& m) { return m.base == base && path::same_path(m.path, module_path); });
    };
    std::vector<std::shared_ptr<ModuleState>> dropped;
    std::vector<std::pair<std::string, LoadedModule>> created;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = agents_.find(agent_id);
        if (it == agents_.end()) {
            return;
        }
        AgentEntry& entry = it->second;
        // WP-45: when the agent registers, what other agents kept for its pid stays only if this is provably the same process (an agent
        // that reconnected to it); a process that reuses the pid gets none of it.
        if (!entry.registered && snap.handshaken) {
            entry.registered = true;
            entry.identity = identity;
            entry.identity_known = identity_known;
            identities_[agent_id] = AgentIdentity{snap.pid, identity, identity_known};
            registered_by_pid_[snap.pid] = agent_id;
            check_carries_on_register_locked(snap.pid, agent_id, identity, identity_known);
        }
        identity = entry.identity;
        identity_known = entry.identity_known;
        // WP-45: a kept baseline of a disabled module whose image has gone since is not "live" any more: the next instance is a fresh
        // image that runs the original code, so the edits must be re-applied there. (Only this process's carries: this module list.)
        for (auto& c : carries_) {
            if (c.first.first == snap.pid && c.second.live_base != 0 && proven_same_locked(c.second.agent_id, agent_id) &&
                !image_loaded(c.second.path, c.second.live_base)) {
                CML_LOG(Trace, "reenable: decision=image-gone module=%s pid=%u base=0x%llx", c.second.path.c_str(), snap.pid,
                        static_cast<unsigned long long>(c.second.live_base));
                c.second.live_base = 0;
                c.second.origin = "reload";
            }
        }
        for (auto m = entry.modules.begin(); m != entry.modules.end();) {
            const auto want = desired.find(m->first);
            if (want == desired.end() || want->second.base != m->second->base) {
                dropped.push_back(std::move(m->second));  // disabled, unloaded, or reloaded at another address
                m = entry.modules.erase(m);
            } else {
                ++m;
            }
        }
        for (const auto& want : desired) {
            if (entry.modules.count(want.first) == 0) {
                auto state = std::make_shared<ModuleState>();
                state->path = want.second.path;
                state->name = std::string(path::base_name(std::string_view(want.second.path)));
                state->base = want.second.base;
                state->size = want.second.size;
                state->instance = ++instances_[InstanceKey{snap.pid, want.first}];  // WP-45: names its patch work directory
                entry.modules[want.first] = state;
                created.emplace_back(want.first, want.second);
            }
        }
    }
    // WP-45: what each dropped instance leaves for the next one of the same module in this process. Only a baseline with accepted
    // edits (originals) matters; without one the next instance starts from the files as they are, as before.
    if (!dropped.empty()) {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hook = drop_hook_;
        }
        if (hook) {
            hook();
        }
    }
    for (const std::shared_ptr<ModuleState>& state : dropped) {
        state->release_image();  // WP-50: the copy goes with the state; a later instance reads the file afresh
        std::optional<BaselineSnapshot> left = leaving_baseline(*state);
        bool live = image_loaded(state->path, state->base);
        if (!left || left->originals.empty()) {
            CML_LOG(Trace, "reenable: decision=nothing-to-carry module=%s pid=%u instance=%llu", state->path.c_str(), snap.pid,
                    static_cast<unsigned long long>(state->instance));
            continue;
        }
        CML_LOG(Info, "module %s %s in process %u: its %llu edited file(s) are kept for the next time it is enabled", state->name.c_str(),
                live ? "disabled" : "unloaded (or loaded at another address)", snap.pid, static_cast<unsigned long long>(left->originals.size()));
        CML_LOG(Trace, "reenable: decision=carry module=%s pid=%u instance=%llu live=%d base=0x%llx originals=%zu", state->path.c_str(), snap.pid,
                static_cast<unsigned long long>(state->instance), live ? 1 : 0, static_cast<unsigned long long>(state->base), left->originals.size());
        std::lock_guard<std::mutex> lock(mutex_);
        // A late message of an agent that is no longer the one registered for this pid (the pid was reused, or the process reconnected
        // and this connection is being torn down): what it leaves is not stored.
        if (const auto reg = registered_by_pid_.find(snap.pid); reg == registered_by_pid_.end() || reg->second != agent_id) {
            std::vector<std::string> names;
            for (const auto& original : left->originals) {
                names.push_back(original.first);
            }
            CML_LOG(Info, "module %s in process %u: the session's edits of %s are not kept: agent#%llu is no longer the agent of that process",
                    state->name.c_str(), snap.pid, file_list(names).c_str(), static_cast<unsigned long long>(agent_id));
            CML_LOG(Trace, "reenable: decision=drop-carry module=%s pid=%u reason=stale-agent agent=%llu", state->path.c_str(), snap.pid,
                    static_cast<unsigned long long>(agent_id));
            continue;
        }
        const InstanceKey key{snap.pid, normalized_key(state->path)};
        std::string origin = live ? "disable" : "reload";
        if (const auto stored = carries_.find(key); stored != carries_.end()) {
            if (carry_usable_locked(stored->second, snap.pid, agent_id, "merged into")) {
                // A carry arrived since the state was dropped (carry_to_late_module of a restart, in the moment mutex_ was free): it is
                // the older one (its originals win, this instance's stamps stay), and its edits are not in this image, so it is not live.
                CML_LOG(Trace, "reenable: decision=carry-merged module=%s pid=%u origin=%s", state->path.c_str(), snap.pid, stored->second.origin.c_str());
                merge_older(*left, stored->second.snapshot);
                origin = stored->second.origin + "+" + origin;
                live = false;
            }
            carries_.erase(stored);  // merged into the new one, or not this process's: dropped (logged)
        }
        Carry& carry = carries_[key];
        carry.path = state->path;
        carry.snapshot = std::move(*left);
        carry.live_base = live ? state->base : 0;
        carry.origin = origin;
        carry.identity = identity;
        carry.identity_known = identity_known;
        carry.agent_id = agent_id;
    }
    const bool any_dropped = !dropped.empty();
    dropped.clear();  // joins their threads
    if (any_dropped) {
        rearm_save_watcher();
    }
    for (const auto& c : created) {
        std::shared_ptr<ModuleState> state;
        std::optional<Carry> carry;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = agents_.find(agent_id);
            if (it == agents_.end()) {
                return;
            }
            const auto m = it->second.modules.find(c.first);
            if (m == it->second.modules.end()) {
                continue;
            }
            state = m->second;
            // WP-45: the baseline an earlier instance of this module (or a restart) left for this process.
            // Only one this process stored (this agent, or one provably the same process); any other is dropped, never taken.
            if (const auto kept = carries_.find(InstanceKey{snap.pid, c.first}); kept != carries_.end()) {
                if (carry_usable_locked(kept->second, snap.pid, agent_id, "taken by the enable of")) {
                    carry = std::move(kept->second);
                }
                carries_.erase(kept);
            }
        }
        ModuleIndexSpec spec;
        spec.module_path = state->path;
        spec.baseline.track_system = options.track_system_headers;
        if (options.cache_include_scan) {
            spec.scan_cache = scan_cache_;  // WP-50 (R-55)
        }
        std::optional<ReapplyItem> reapply;
        if (carry) {
            // The same image enabled again (it never went away) still runs the patches: nothing to recompile. Any other instance runs
            // the original code: its index restores the original sources' baseline and the worker reloads it.
            const bool same_image = carry->live_base != 0 && carry->live_base == state->base;
            spec.carried = carry->snapshot;
            spec.carried_live = same_image;
            {
                std::lock_guard<std::mutex> state_lock(state->mutex);
                state->carried_in = carry->snapshot;
            }
            CML_LOG(Trace, "reenable: decision=%s module=%s pid=%u instance=%llu origin=%s originals=%zu", same_image ? "adopt" : "reapply",
                    state->path.c_str(), snap.pid, static_cast<unsigned long long>(state->instance), carry->origin.c_str(), carry->snapshot.originals.size());
            if (!same_image) {
                ReapplyItem item;
                item.agent_id = agent_id;
                item.pid = snap.pid;
                item.state = state;
                for (const auto& original : carry->snapshot.originals) {
                    item.files.push_back(original.first);
                }
                item.origin = carry->origin;
                reapply = std::move(item);
            }
        }
        const std::string name = state->name;
        const std::weak_ptr<ModuleState> weak_state = state;
        const bool prefetch = options.prefetch_module_image;
        // WP-50: the read is queued once, on the change into Ready (a Ready index reports again on later status changes).
        const auto prefetch_queued = std::make_shared<std::atomic<bool>>(false);
        spec.on_change = [this, name, weak_state, prefetch, prefetch_queued](const IndexStatus& s) {
            if (s.state == IndexState::Ready) {
                rearm_save_watcher();  // its tracked files are known now (compile-on-save)
                if (prefetch && !prefetch_queued->exchange(true)) {
                    queue_image_prefetch(weak_state);  // WP-50 (R-47): only once the index is Ready
                }
            }
            if (s.state == IndexState::Failed) {
                CML_LOG(Warn, "index of %s failed: %s", name.c_str(), s.error ? s.error->format().c_str() : "unknown error");
            } else {
                CML_LOG(Debug, "index of %s: %s %u%% (%s)", name.c_str(), to_string(s.state), s.percent, s.phase.c_str());
            }
        };
        // WP-18: the build requirements (the same audit as `cml_broker check`). Warnings and failures are published as
        // diag events (stage "enable"); a failure also fails the index, so a reload names it.
        spec.audit = true;
        if (events_ != nullptr) {
            EventBus* bus = events_;
            const std::uint32_t pid = snap.pid;
            // WP-26b: archive members that cannot be reloaded (CML5920), once per module.
            spec.on_skipped_archive_members = [bus, pid](const Diag& d) { bus->publish(make_diag_event(d, "warning", "enable", pid)); };
            spec.on_audit = [bus, pid](const ModuleAudit& audit) {
                for (const AuditFinding& f : audit.findings) {
                    if (f.verdict != Verdict::Pass) {
                        bus->publish(make_diag_event(f.diag(), f.verdict == Verdict::Fail ? "error" : "warning", "enable", pid));
                    }
                }
            };
        }
        CML_LOG(Info, "module %s enabled in process %u: loading its PDB index", state->name.c_str(), snap.pid);
        {
            std::shared_ptr<ModuleIndex> index = ModuleIndex::start(std::move(spec));
            std::lock_guard<std::mutex> state_lock(state->mutex);  // tracked_files() reads it from the watcher's threads
            state->index = std::move(index);
        }
        if (reapply) {
            queue_reapply(std::move(*reapply));
        }
        rearm_save_watcher();  // the index may have become ready before it was stored
    }
}

// ---- WP-45: re-applying the session's edits to a module enabled later --------------------------------------------------------

std::optional<BaselineSnapshot> ReloadService::leaving_baseline(ModuleState& state) const {
    std::shared_ptr<ModuleIndex> index;
    std::optional<BaselineSnapshot> carried_in;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        index = state.index;
        carried_in = state.carried_in;
    }
    std::optional<BaselineSnapshot> out;
    if (index && index->status().state == IndexState::Ready) {
        out = index->baseline_snapshot();  // the accepted edits of this instance, and what it was given (restored before Ready)
        if (carried_in) {
            merge_older(*out, *carried_in);  // what it was given but had not restored yet (the worker restores a late carry)
        }
    } else if (carried_in) {
        out = std::move(carried_in);  // the index never got ready: hand on what this instance was given
    }
    return out;
}

bool ReloadService::carry_to_late_module(std::uint64_t agent_id, std::uint32_t pid, const std::string& module_path, const BaselineSnapshot& snapshot,
                                         const std::string& origin) {
    if (snapshot.originals.empty()) {
        return false;
    }
    const std::string key = normalized_key(module_path);
    std::vector<std::string> names;
    for (const auto& original : snapshot.originals) {
        names.push_back(original.first);
    }
    std::shared_ptr<ModuleState> existing;
    std::uint64_t target = 0;  // the agent the baseline goes to
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The agent registered for the pid now: `agent_id`, or one that reconnected to the same process since (proven identity).
        const auto reg = registered_by_pid_.find(pid);
        const char* why = nullptr;
        if (reg == registered_by_pid_.end()) {
            why = "no agent of that process has registered";
        } else if (reg->second != agent_id && !proven_same_locked(reg->second, agent_id)) {
            why = "the agent registered for that pid now is not provably the same process";
        } else {
            target = reg->second;
        }
        const auto agent = target != 0 ? agents_.find(target) : agents_.end();
        if (why == nullptr && (agent == agents_.end() || !agent->second.registered)) {
            why = "its agent is gone";
        }
        if (why != nullptr) {
            CML_LOG(Info, "module %s in process %u: %s, so the session's edits of %s are not kept for it (they come back with the next edit of "
                          "those files, or a restart)",
                    module_path.c_str(), pid, why, file_list(names).c_str());
            CML_LOG(Trace, "reenable: decision=drop-carry module=%s pid=%u reason=no-proven-agent agent=%llu origin=%s", module_path.c_str(), pid,
                    static_cast<unsigned long long>(agent_id), origin.c_str());
            return false;
        }
        if (const auto m = agent->second.modules.find(key); m != agent->second.modules.end()) {
            existing = m->second;
        }
        if (!existing) {
            const InstanceKey k{pid, key};
            Carry fresh;
            fresh.path = module_path;
            fresh.snapshot = snapshot;
            if (const auto stored = carries_.find(k); stored != carries_.end()) {
                if (carry_usable_locked(stored->second, pid, target, "merged into")) {
                    fresh.snapshot = stored->second.snapshot;   // this process's own carry: the incoming one is the older baseline
                    merge_older(fresh.snapshot, snapshot);
                }
                carries_.erase(stored);  // merged, or another process's: dropped (logged)
            }
            fresh.live_base = 0;
            fresh.origin = origin;
            fresh.identity = agent->second.identity;
            fresh.identity_known = agent->second.identity_known;
            fresh.agent_id = target;
            carries_[k] = std::move(fresh);
        }
    }
    if (!existing) {
        CML_LOG(Info, "module %s is not enabled in process %u yet: its %llu edited file(s) are re-applied when it is", module_path.c_str(), pid,
                static_cast<unsigned long long>(snapshot.originals.size()));
        CML_LOG(Trace, "reenable: decision=carry module=%s pid=%u agent=%llu origin=%s originals=%zu", module_path.c_str(), pid,
                static_cast<unsigned long long>(target), origin.c_str(), snapshot.originals.size());
        return true;
    }
    // It was enabled just now, without the baseline: the worker restores it once the index is ready, then reloads.
    CML_LOG(Trace, "reenable: decision=reapply-existing module=%s pid=%u instance=%llu origin=%s originals=%zu", module_path.c_str(), pid,
            static_cast<unsigned long long>(existing->instance), origin.c_str(), snapshot.originals.size());
    {
        std::lock_guard<std::mutex> state_lock(existing->mutex);
        if (existing->carried_in) {
            merge_older(*existing->carried_in, snapshot);
        } else {
            existing->carried_in = snapshot;
        }
    }
    ReapplyItem item;
    item.agent_id = target;
    item.pid = pid;
    item.state = existing;
    item.restore = snapshot;
    item.files = std::move(names);
    item.origin = origin;
    queue_reapply(std::move(item));
    return true;
}

std::vector<std::pair<std::string, BaselineSnapshot>> ReloadService::kept_baselines(std::uint64_t agent_id, std::uint32_t pid) {
    std::vector<std::pair<std::string, BaselineSnapshot>> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto c = carries_.begin(); c != carries_.end();) {
        if (c->first.first != pid) {
            ++c;
            continue;
        }
        if (!carry_usable_locked(c->second, pid, agent_id, "handed to the restart of")) {
            c = carries_.erase(c);
            continue;
        }
        out.emplace_back(c->second.path, c->second.snapshot);
        ++c;
    }
    return out;
}

void ReloadService::set_drop_hook_for_testing(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    drop_hook_ = std::move(hook);
}

std::vector<std::pair<std::string, BaselineSnapshot>> ReloadService::kept_baselines_for_testing(std::uint32_t pid) const {
    std::vector<std::pair<std::string, BaselineSnapshot>> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& c : carries_) {
        if (c.first.first == pid) {
            out.emplace_back(c.second.path, c.second.snapshot);
        }
    }
    return out;
}

bool ReloadService::proven_same_locked(std::uint64_t a, std::uint64_t b) const {
    if (a == b) {
        return true;
    }
    const auto x = identities_.find(a);
    const auto y = identities_.find(b);
    return x != identities_.end() && y != identities_.end() && x->second.known && y->second.known && x->second.pid == y->second.pid &&
           x->second.identity == y->second.identity;
}

bool ReloadService::carry_usable_locked(const Carry& carry, std::uint32_t pid, std::uint64_t agent_id, const char* what) const {
    if (proven_same_locked(carry.agent_id, agent_id)) {
        return true;
    }
    std::vector<std::string> names;
    for (const auto& original : carry.snapshot.originals) {
        names.push_back(original.first);
    }
    CML_LOG(Info, "module %s: the session's edits of %s kept for process %u by agent#%llu are dropped, not %s agent#%llu: not provably the same process",
            carry.path.c_str(), file_list(names).c_str(), pid, static_cast<unsigned long long>(carry.agent_id), what,
            static_cast<unsigned long long>(agent_id));
    CML_LOG(Trace, "reenable: decision=drop-carry module=%s pid=%u reason=identity-unproven stored_by=%llu asked_by=%llu", carry.path.c_str(), pid,
            static_cast<unsigned long long>(carry.agent_id), static_cast<unsigned long long>(agent_id));
    return false;
}
void ReloadService::drop_carries_locked(std::uint32_t pid, const char* why) {
    for (auto c = carries_.begin(); c != carries_.end();) {
        if (c->first.first == pid) {
            CML_LOG(Trace, "reenable: decision=drop-carry module=%s pid=%u reason=%s", c->second.path.c_str(), pid, why);
            c = carries_.erase(c);
        } else {
            ++c;
        }
    }
}

void ReloadService::check_carries_on_register_locked(std::uint32_t pid, std::uint64_t agent_id, const std::string& identity, bool identity_known) {
    for (auto c = carries_.begin(); c != carries_.end();) {
        if (c->first.first != pid || c->second.agent_id == agent_id) {
            ++c;
            continue;
        }
        // Kept only on a proven match: both creation times known and equal (with the token and the command line). An unproven match
        // may be another program run with the same pid, whose image runs the original code: an adopted baseline would hide the edits.
        const bool same = identity_known && c->second.identity_known && c->second.identity == identity;
        if (same) {
            CML_LOG(Trace, "reenable: decision=keep-carry module=%s pid=%u reason=same-process agent=%llu", c->second.path.c_str(), pid,
                    static_cast<unsigned long long>(agent_id));
            c->second.agent_id = agent_id;
            ++c;
            continue;
        }
        std::vector<std::string> names;
        for (const auto& original : c->second.snapshot.originals) {
            names.push_back(original.first);
        }
        CML_LOG(Info, "module %s: the session's edits of %s kept for process %u are dropped: the agent that registered with that pid is %s", c->second.path.c_str(),
                file_list(names).c_str(), pid, identity_known && c->second.identity_known ? "another process" : "not provably the same process");
        CML_LOG(Trace, "reenable: decision=drop-carry module=%s pid=%u reason=%s", c->second.path.c_str(), pid, identity_known && c->second.identity_known ? "other-process" : "identity-unproven");
        c = carries_.erase(c);
    }
}

bool ReloadService::reapply_stopping() const {
    std::lock_guard<std::mutex> lock(reapply_mutex_);
    return reapply_stop_;
}

bool ReloadService::has_carry_for_testing(std::uint32_t pid, const std::string& module_path, bool* live) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = carries_.find(InstanceKey{pid, normalized_key(module_path)});
    if (it == carries_.end()) {
        return false;
    }
    if (live != nullptr) {
        *live = it->second.live_base != 0;
    }
    return true;
}

// ---- WP-50 (R-47): the module file read in the background ----------------------------------------------------------------------

void ReloadService::queue_image_prefetch(std::weak_ptr<ModuleState> state) {
    std::lock_guard<std::mutex> lock(prefetch_mutex_);
    if (prefetch_stop_) {
        return;
    }
    prefetch_queue_.push_back(std::move(state));
    if (!prefetch_thread_.joinable()) {
        prefetch_thread_ = std::thread([this] { prefetch_loop(); });
    }
    prefetch_changed_.notify_all();
}

void ReloadService::prefetch_loop() {
    for (;;) {
        std::weak_ptr<ModuleState> next;
        {
            std::unique_lock<std::mutex> lock(prefetch_mutex_);
            prefetch_changed_.wait(lock, [this] { return prefetch_stop_ || !prefetch_queue_.empty(); });
            if (prefetch_stop_) {
                return;
            }
            next = std::move(prefetch_queue_.front());
            prefetch_queue_.pop_front();
        }
        std::shared_ptr<ModuleImageSlot> slot;
        std::string path;
        std::string name;
        {
            const std::shared_ptr<ModuleState> state = next.lock();
            if (!state) {
                continue;  // dropped before its turn
            }
            slot = state->image_slot;
            path = state->path;
            name = state->name;
        }
        // The state is not held from here on: a disable that drops it meanwhile destroys it on the disabling thread at once
        // (its index joined, its PDB released), and it released the slot first, so this read stores nothing.
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(prefetch_mutex_);
            hook = prefetch_hook_;
        }
        if (hook) {
            hook();
        }
        const ModuleState::ImageRead read = slot->ensure(path, true);
        const bool gone = next.expired();
        if (gone) {
            slot->release();  // a state dropped without a release (none does today): the copy goes with this slot
        }
        if (read.read_now) {
            CML_LOG(Info, "WP50-ONEOFF what=module-image image=background module=%s ms=%.1f mib=%.1f ok=%d kept=%d", name.c_str(), read.ms, read.mib,
                    read.image ? 1 : 0, !gone && read.image && !slot->released() ? 1 : 0);
            if (read.error) {
                // Not a failure: the first reload reads the file itself and warns (CML5313) if it still cannot.
                CML_LOG(Debug, "reload: background read of %s: %s", path.c_str(), read.error->format().c_str());
            }
        }
    }
}

void ReloadService::set_prefetch_hook_for_testing(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(prefetch_mutex_);
    prefetch_hook_ = std::move(hook);
}

void ReloadService::queue_reapply(ReapplyItem item) {
    if (const std::shared_ptr<ModuleState> state = item.state.lock()) {
        std::lock_guard<std::mutex> state_lock(state->mutex);
        item.generation = state->generation;  // a reload that patches the module before the worker gets to it has re-applied the edits
    }
    std::lock_guard<std::mutex> lock(reapply_mutex_);
    if (reapply_stop_) {
        return;
    }
    ++reapply_pending_;
    reapply_queue_.push_back(std::move(item));
    if (!reapply_thread_.joinable()) {
        reapply_thread_ = std::thread([this] { reapply_loop(); });
    }
    reapply_changed_.notify_all();
}

void ReloadService::reapply_loop() {
    for (;;) {
        ReapplyItem item;
        {
            std::unique_lock<std::mutex> lock(reapply_mutex_);
            reapply_changed_.wait(lock, [this] { return reapply_stop_ || !reapply_queue_.empty(); });
            if (reapply_stop_) {
                reapply_pending_ -= reapply_queue_.size();
                reapply_queue_.clear();
                return;
            }
            item = std::move(reapply_queue_.front());
            reapply_queue_.pop_front();
        }
        reapply_one(item);
        --reapply_pending_;
    }
}

void ReloadService::reapply_one(const ReapplyItem& item) {
    const std::shared_ptr<ModuleState> state = item.state.lock();
    if (!state) {
        return;  // dropped already; it handed its carry on (leaving_baseline)
    }
    const auto current = [&] { return module_state(item.agent_id, state->path) == state; };
    // Whether to give up: the broker stops, or the instance went away (it handed its carry on). Logged once, at trace.
    const auto abandoned = [&](const char* where) {
        const bool stop = reapply_stopping();
        if (!stop && current()) {
            return false;
        }
        CML_LOG(Trace, "reenable: decision=abandoned module=%s pid=%u instance=%llu reason=%s at=%s", state->path.c_str(), item.pid,
                static_cast<unsigned long long>(state->instance), stop ? "shutdown" : "module-gone", where);
        return true;
    };
    const std::string files = file_list(item.files);
    const auto report = [&](const Diag& d) {
        CML_LOG(Warn, "reenable: %s", d.format().c_str());
        if (events_ != nullptr) {
            events_->publish(make_diag_event(d, "warning", "enable", item.pid));
        }
    };
    // 1. Its index (started right after the state; the carried baseline is applied in it before Ready).
    std::shared_ptr<ModuleIndex> index;
    for (;;) {
        if (abandoned("index")) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            index = state->index;
        }
        if (index && index->wait(100)) {
            break;
        }
        if (!index) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    // The instance may have gone (or the broker begun to stop) while its index loaded: that is not a failed re-apply.
    if (abandoned("index-done")) {
        return;
    }
    if (index->status().state != IndexState::Ready) {
        const IndexStatus st = index->status();
        report(Diag::make(reenable::diag::kReapplyIndexFailed,
                          "module '" + state->name + "' (process " + std::to_string(item.pid) + ") runs its original code: the session's edits of " + files +
                              " were not re-applied because its PDB index failed" + (st.error ? ": " + st.error->format() : std::string()),
                          "Fix what the index diagnostic says, then run `reload` (or load the module again)."));
        return;
    }
    // 2. The run lock: one reload at a time. A `reload` that arrives meanwhile waits for this one (run_reload).
    std::unique_ptr<ExclusiveRun> run;
    for (;;) {
        if (abandoned("run-lock")) {
            return;
        }
        run = lock_reloads();
        if (run) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // 3. With the lock held: still wanted? A reload (the user's, or compile-on-save) that ran since this item was queued swept the
    // instance with the carried baseline in it (the index applied it before Ready), so it re-applied the edits already.
    if (abandoned("before-reload")) {
        return;
    }
    if (!item.restore) {
        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            generation = state->generation;
        }
        if (generation != item.generation) {
            CML_LOG(Trace, "reenable: decision=skip-applied module=%s pid=%u instance=%llu generation_queued=%llu generation_now=%llu", state->path.c_str(),
                    item.pid, static_cast<unsigned long long>(state->instance), static_cast<unsigned long long>(item.generation),
                    static_cast<unsigned long long>(generation));
            return;
        }
    }
    // The baseline (when the index did not apply it), then one reload of this module.
    const std::size_t restored = item.restore ? index->restore_session_baseline(*item.restore) : index->carried_files();
    if (restored == 0) {
        CML_LOG(Info, "module %s (process %u): none of the edited files (%s) is part of this instance; nothing to re-apply", state->name.c_str(), item.pid, files.c_str());
        return;
    }
    CML_LOG(Info, "module %s enabled again in process %u (instance %llu, after %s): re-applying the session's edits of %llu file(s): %s", state->name.c_str(),
            item.pid, static_cast<unsigned long long>(state->instance), item.origin.c_str(), static_cast<unsigned long long>(restored), files.c_str());
    Value args = Value::object();
    args.set("pid", Value::integer(item.pid));
    args.set("module", Value::string(state->path));
    args.set("confirm", Value::boolean(true));  // the user confirmed these edits when they were first reloaded
    args.set("timeout_ms", Value::integer(300000));
    Result<Value> reloaded = run_reload(args, false, true);
    if (!reloaded) {
        // A reload that ended because the broker stops, or because the instance went away meanwhile, is not a failed re-apply.
        if (abandoned("reload")) {
            return;
        }
        report(Diag::make(reenable::diag::kReapplyFailed,
                          "module '" + state->name + "' (process " + std::to_string(item.pid) + ") runs its original code: re-applying the session's edits of " + files +
                              " failed: " + reloaded.error().format(),
                          "Fix the error and run `reload`; the edits stay pending until a reload applies them."));
        return;
    }
    // Success, with patches or without (nothing changed any more: a reload before this one applied them, or the files are back to
    // their original content). Never a CML5600.
    CML_LOG(Info, "module %s (process %u): the session's edits were re-applied (%lld patch(es), %lld hook(s))", state->name.c_str(), item.pid,
            static_cast<long long>(reloaded->get_integer("patched")), static_cast<long long>(reloaded->get_integer("hooks_installed")));
    CML_LOG(Trace, "reenable: decision=reapplied module=%s pid=%u instance=%llu patched=%lld", state->path.c_str(), item.pid,
            static_cast<unsigned long long>(state->instance), static_cast<long long>(reloaded->get_integer("patched")));
}

std::optional<IndexStatus> ReloadService::index_status(std::uint64_t agent_id, const std::string& module_path) const {
    std::shared_ptr<ModuleState> state = module_state(agent_id, module_path);
    if (!state || !state->index) {
        return std::nullopt;
    }
    return state->index->status();
}

std::shared_ptr<ModuleState> ReloadService::module_state(std::uint64_t agent_id, const std::string& module_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = agents_.find(agent_id);
    if (it == agents_.end()) {
        return nullptr;
    }
    const auto m = it->second.modules.find(normalized_key(module_path));
    return m == it->second.modules.end() ? nullptr : m->second;
}

// ---- hot-restart (WP-21) --------------------------------------------------------------------------------------

void ReloadService::set_process_host(std::shared_ptr<ProcessHost> host) {
    // WP-43 (R-40): the coordinator is made once and keeps its holds, its unfinished restarts and its shutdown flag; only its host is
    // replaced. A restart that runs (or holds a process) meanwhile keeps the host it started with, and `restart-resume` and cancel() still
    // reach it. (Replacing the coordinator stranded a held process in the old one until hold_ms.)
    std::lock_guard<std::mutex> lock(mutex_);
    process_host_ = host;  // WP-45: detach asks it whether a process has exited
    if (restart_ == nullptr) {
        if (!host) {
            return;
        }
        restart_ = std::make_shared<RestartCoordinator>(*this, events_, std::move(host));
        return;
    }
    restart_->set_host(std::move(host));
}

void ReloadService::set_restart_trace_for_testing(std::function<void(const std::string&)> trace) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (restart_) {
        restart_->set_trace_for_testing(std::move(trace));
    }
}

std::shared_ptr<RestartCoordinator> ReloadService::restart_coordinator_for_testing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return restart_;
}

bool ReloadService::restart_available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return restart_ != nullptr && restart_->has_host();
}

void ReloadService::cancel() {
    cancel_.cancel();
    std::shared_ptr<RestartCoordinator> coordinator;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        coordinator = restart_;
    }
    if (coordinator != nullptr) {
        coordinator->abort_holds();  // WP-34: a process held suspended is resumed before the broker goes
    }
}

Result<Value> ReloadService::restart(const Value& args, const std::function<bool()>& client_gone) {
    std::shared_ptr<RestartCoordinator> coordinator;  // held for the whole call: set_process_host may replace it meanwhile
    {
        std::lock_guard<std::mutex> lock(mutex_);
        coordinator = restart_;
    }
    if (coordinator == nullptr || !coordinator->has_host()) {
        return Diag::make(restart::diag::kUnavailable, "this broker cannot restart processes on this system",
                          "Restart the application yourself; hot-restart needs the Windows broker.");
    }
    return coordinator->restart(args, client_gone);
}

Result<Value> ReloadService::restart_resume(const Value& args) {
    std::shared_ptr<RestartCoordinator> coordinator;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        coordinator = restart_;
    }
    if (coordinator == nullptr) {
        return Diag::make(restart::diag::kUnavailable, "this broker cannot restart processes on this system",
                          "Restart the application yourself; hot-restart needs the Windows broker.");
    }
    return coordinator->resume(args);
}

bool ReloadService::restart_hold_available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return restart_ != nullptr && restart_->hold_supported();
}

bool ReloadService::restart_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return restart_ != nullptr && restart_->active();
}

std::vector<std::shared_ptr<AgentLink>> ReloadService::links() const {
    std::vector<std::shared_ptr<AgentLink>> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& agent : agents_) {
        if (agent.second.link) {
            out.push_back(agent.second.link);
        }
    }
    return out;
}

std::shared_ptr<AgentLink> ReloadService::link_for_pid(std::uint32_t pid) const {
    for (const std::shared_ptr<AgentLink>& link : links()) {
        const LinkSnapshot snap = link->snapshot();
        if (snap.handshaken && snap.pid == pid && !link->closed()) {
            return link;
        }
    }
    return nullptr;
}

std::shared_ptr<AgentLink> ReloadService::link_for_restart_token(const std::string& token, std::uint32_t pid) const {
    if (token.empty()) {
        return nullptr;
    }
    for (const std::shared_ptr<AgentLink>& link : links()) {
        const LinkSnapshot snap = link->snapshot();
        if (snap.handshaken && snap.restart_token == token && snap.pid == pid && !link->closed()) {
            return link;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<ModuleState>> ReloadService::module_states(std::uint64_t agent_id) const {
    std::vector<std::shared_ptr<ModuleState>> out;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = agents_.find(agent_id);
    if (it != agents_.end()) {
        for (const auto& m : it->second.modules) {
            out.push_back(m.second);
        }
    }
    return out;
}

ReloadOptions ReloadService::options() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return options_;
}

std::vector<ReloadService::Target> ReloadService::targets(std::uint32_t pid, const std::string& module) const {
    std::vector<Target> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& agent : agents_) {
        if (!agent.second.link) {
            continue;
        }
        if (pid != 0 && agent.second.link->snapshot().pid != pid) {
            continue;
        }
        for (const auto& m : agent.second.modules) {
            if (module.empty() || path::matches_module(module, m.second->path)) {
                out.push_back(Target{agent.second.link, m.second});
            }
        }
    }
    return out;
}

// ---- reload -------------------------------------------------------------------------------------------------

Result<Value> ReloadService::reload(const Value& args) {
    return run_reload(args, false);
}

std::unique_ptr<ExclusiveRun> ReloadService::lock_reloads() {
    auto run = std::make_unique<ExclusiveRun>(reload_mutex_, running_);
    if (!run->acquired()) {
        return nullptr;
    }
    return run;
}

Result<Value> ReloadService::reload_locked(const Value& args) {
    return run_reload(args, false, true);
}

Result<Value> ReloadService::run_reload(const Value& args, bool automatic, bool lock_held) {
    std::uint32_t pid = 0;
    std::uint32_t timeout_ms = 60000;
    std::optional<Diag> error;
    if (!read_uint(args, "pid", 0xFFFFFFFFu, &pid, &error) || !read_uint(args, "timeout_ms", 3600u * 1000u, &timeout_ms, &error)) {
        return *error;
    }
    const std::string module = args.get_string("module");
    if (const Value* m = args.find("module"); m != nullptr && !m->is_null() && !m->is_string()) {
        return bad_request("argument 'module' must be a string");
    }
    if (timeout_ms == 0) {
        timeout_ms = 60000;
    }
    bool confirm = false;
    if (const Value* c = args.find("confirm"); c != nullptr && !c->is_null()) {
        if (!c->is_bool()) {
            return bad_request("argument 'confirm' must be a boolean");
        }
        confirm = c->as_bool();
    }

    std::optional<ExclusiveRun> exclusive;  // WP-21: a restart holds the run lock itself (lock_reloads) and calls reload_locked()
    if (!lock_held) {
        exclusive.emplace(reload_mutex_, running_);
        // WP-45: re-applying the edits of a module that was enabled again is part of its enable: a reload the user asks for meanwhile
        // waits for it (within its timeout) instead of being refused busy. A save's reload stays quiet and retries.
        const Clock::time_point give_up = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!automatic && !exclusive->acquired() && reapply_pending_.load() != 0 && Clock::now() < give_up) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            exclusive.reset();
            exclusive.emplace(reload_mutex_, running_);
        }
    }
    if (!lock_held && !exclusive->acquired()) {
        const Diag d = Diag::make(pd::kBusy, "another reload is still running", "Wait for it to finish (control command `wait`) and try again.");
        if (automatic) {
            return d;  // the caller retries quietly
        }
        if (events_ != nullptr) {
            events_->publish(make_diag_event(d, "error", "reload"));
        }
        return d;
    }

    const Clock::time_point start = Clock::now();
    std::vector<Target> list = targets(pid, module);
    if (automatic) {
        // A save while a module is still indexing (or its index failed) is not a failed reload: that module is
        // skipped here, and picked up when its index becomes ready (the watcher is re-armed then).
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [](const Target& t) {
                                      std::shared_ptr<ModuleIndex> index;
                                      {
                                          std::lock_guard<std::mutex> lock(t.module->mutex);
                                          index = t.module->index;
                                      }
                                      return !index || index->status().state != IndexState::Ready;
                                  }),
                   list.end());
        if (list.empty()) {
            CML_LOG(Debug, "compile-on-save: no module with a ready index; nothing to reload");
            Value none = Value::object();
            none.set("patched", Value::integer(0));
            return none;
        }
    }
    if (list.empty()) {
        const Diag d = Diag::make(pd::kNoTarget,
                                  std::string("no enabled module to reload") + (pid != 0 ? " in process " + std::to_string(pid) : "") +
                                      (module.empty() ? "" : " matching '" + module + "'"),
                                  "Enable a module with cml::Agent::EnableModule (see `status`), then reload.");
        CML_LOG(Warn, "reload: %s", d.format().c_str());
        if (events_ != nullptr) {
            events_->publish(make_diag_event(d, "error", "reload"));
        }
        return d;
    }
    Result<std::string> dir = ensure_session_dir();
    if (!dir) {
        if (events_ != nullptr) {
            events_->publish(make_diag_event(dir.error(), "error", "reload"));
        }
        return dir.error();
    }
    const ReloadOptions opts = options();
    PipelineContext ctx;
    ctx.session_dir = *dir;
    ctx.options = &opts;
    ctx.events = events_;
    ctx.exports = exports_;
    ctx.deadline = start + std::chrono::milliseconds(timeout_ms);
    ctx.cancel = &cancel_;
    ctx.faults = &faults_;  // WP-28
    ctx.confirm = confirm;
    ctx.reload_id = ++reload_counter_;  // WP-22: `revert last` undoes the batches of one reload id
    // WP-16: one compile per distinct command line for the whole reload; every process still links and loads its own patch.
    ctx.compile_cache = std::make_shared<CompileCache>();

    std::vector<PatchReport> reports;
    std::optional<Diag> first_error;
    std::size_t failed = 0;
    StageTimes sum;
    // WP-54 (ADR-0045): the modules of one process are prepared together and applied all or none (process_reload.h); processes
    // stay independent. The groups keep the order of `list`, and so does the answer's `patches`.
    std::vector<std::vector<std::size_t>> processes;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto same = std::find_if(processes.begin(), processes.end(), [&](const std::vector<std::size_t>& g) { return list[g.front()].link == list[i].link; });
        if (same != processes.end()) {
            same->push_back(i);
        } else {
            processes.push_back({i});
        }
    }
    for (const std::vector<std::size_t>& group : processes) {
        std::vector<ModuleState*> modules;
        std::vector<std::uint64_t> generations_before;
        for (const std::size_t i : group) {
            modules.push_back(list[i].module.get());
            std::lock_guard<std::mutex> lock(list[i].module->mutex);
            generations_before.push_back(list[i].module->generation);
        }
        ProcessReload done = reload_process(pipeline_, *list[group.front()].link, modules, ctx);
        // Patches that stay applied count even when the reload failed (WP-47: an unconfirmed install); a roll-back puts the module's
        // generation back. The broker-wide generation follows the modules' own.
        for (std::size_t g = 0; g < group.size(); ++g) {
            std::lock_guard<std::mutex> lock(modules[g]->mutex);
            generation_.fetch_add(modules[g]->generation - generations_before[g]);
        }
        if (done.error && !first_error) {
            first_error = done.error;
        }
        for (std::size_t g = 0; g < group.size(); ++g) {
            Result<PatchReport>& r = done.modules[g];
            const Target& t = list[group[g]];
            if (!r) {
                ++failed;
                keep_session_dir_ = true;  // objects, DLLs and PDBs of a failed reload are evidence
                if (!first_error) {
                    first_error = r.error();
                }
                continue;
            }
            sum.add(r->times);
            if (r->patched) {
                const std::string stem(path::base_name(std::string_view(t.module->name)));
                const std::size_t dot = stem.find_last_of('.');
                CML_LOG(Info, "%s", timing_line(dot == std::string::npos ? stem : stem.substr(0, dot), r->times).c_str());
            }
            reports.push_back(std::move(*r));
        }
    }
    const double wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    if (first_error) {
        Diag d = *first_error;
        if (failed > 1 || reports.size() > 0) {
            d.message += " (" + std::to_string(failed) + " of " + std::to_string(list.size()) + " modules failed)";
        }
        return d;
    }

    Value result = Value::object();
    result.set("generation", Value::integer(static_cast<std::int64_t>(generation_.load())));
    std::size_t hooks = 0, run = 0, patched = 0, compiled = 0, reused = 0, attempts = 0, modules_patched = 0;
    std::size_t hooks_planned = 0, hooks_dropped = 0, modules_unchanged = 0;  // WP-25
    std::size_t imgrel_sites = 0, imgrel_verified = 0;  // WP-36
    std::size_t objects_held_peak = 0;                  // WP-54: a module's prepare drops its objects before the next module's starts
    std::set<std::uint32_t> patched_pids;
    Value patches = Value::array();
    Value changed = Value::array();
    for (const PatchReport& p : reports) {
        hooks += p.hooks_installed;
        hooks_planned += p.hooks_planned;
        hooks_dropped += p.hooks_dropped;
        modules_unchanged += p.no_functional_change ? 1 : 0;
        imgrel_sites += p.imgrel_sites;
        imgrel_verified += p.imgrel_verified;
        objects_held_peak = std::max(objects_held_peak, p.objects_held_peak);
        run += p.initializers_run;
        patched += p.patches;
        compiled += p.compiled;
        reused += p.compile_reused;
        attempts += p.load_attempts;
        if (p.patches != 0) {
            ++modules_patched;
            patched_pids.insert(p.pid);
        }
        for (const std::string& f : p.changed_files) {
            changed.push(Value::string(f));
        }
        patches.push(to_json(p));
    }
    result.set("patched", Value::integer(static_cast<std::int64_t>(patched)));
    result.set("hooks_installed", Value::integer(static_cast<std::int64_t>(hooks)));
    // WP-25: "N functions changed" = hooks_planned - hooks_dropped (hooks_installed is what the agent installed).
    // `no_functional_change`: the edit compiled to identical code in every module that was recompiled and nothing was
    // loaded (CML3450 is in patches[].warnings and was published as a diag event).
    result.set("hooks_planned", Value::integer(static_cast<std::int64_t>(hooks_planned)));
    result.set("hooks_dropped", Value::integer(static_cast<std::int64_t>(hooks_dropped)));
    result.set("modules_unchanged", Value::integer(static_cast<std::int64_t>(modules_unchanged)));
    result.set("no_functional_change", Value::boolean(modules_unchanged != 0 && patched == 0));  // the warnings are in patches[].warnings
    // WP-16: compiler runs of this reload, results taken from another module's identical command line, and the
    // modules / processes that received at least one patch.
    result.set("compiled", Value::integer(static_cast<std::int64_t>(compiled)));
    result.set("compile_reused", Value::integer(static_cast<std::int64_t>(reused)));
    result.set("load_attempts", Value::integer(static_cast<std::int64_t>(attempts)));  // LoadPatch requests; more than `patched` = relinks at another base
    result.set("modules_patched", Value::integer(static_cast<std::int64_t>(modules_patched)));
    result.set("processes_patched", Value::integer(static_cast<std::int64_t>(patched_pids.size())));
    result.set("initializers_run", Value::integer(static_cast<std::int64_t>(run)));
    // WP-36: image-relative references to old data in the recompiled objects and how many were retargeted (proved to be
    // address displacements). cmltest prints them as `WP36-IMGREL sites=N verified=N`.
    result.set("imgrel_sites", Value::integer(static_cast<std::int64_t>(imgrel_sites)));
    result.set("imgrel_verified", Value::integer(static_cast<std::int64_t>(imgrel_verified)));
    // WP-54: the most COFF objects of recompiled compilands the reload held at once (1 for any number of compilands and modules).
    result.set("objects_held_peak", Value::integer(static_cast<std::int64_t>(objects_held_peak)));
    result.set("changed_files", std::move(changed));
    result.set("patches", std::move(patches));
    sum.total = wall_ms;
    result.set("timing_ms", to_json(sum));
    std::string fixture = reports.empty() ? std::string() : reports.front().module;
    if (const std::size_t dot = fixture.find_last_of('.'); dot != std::string::npos) {
        fixture.resize(dot);
    }
    result.set("timing", Value::string(timing_line(fixture, sum)));
    return result;
}

// ---- revert (WP-22) -------------------------------------------------------------------------------------------

Result<Value> ReloadService::revert(const Value& args) {
    // A revert and a reload never overlap: both change the hooks and the broker's per-module patch list. R-51: the run lock is taken
    // here, under the failure-injection gate, and held until finish_injection below has run (an inject-failure sees the revert running
    // until then); revert_impl refuses busy after checking its arguments when it could not be taken.
#if defined(CML_FAULT_INJECTION)
    std::unique_lock<std::mutex> gate = faults_.lock_gate();  // WP-62: waits for an inject-failure that is arming agents
#endif
    std::optional<ExclusiveRun> exclusive;
    exclusive.emplace(reload_mutex_, running_);
#if defined(CML_FAULT_INJECTION)
    gate.unlock();
#endif
    std::vector<std::uint32_t> pids;
    std::map<std::uint32_t, std::string> outcomes;
    Result<Value> result = revert_impl(args, exclusive->acquired(), &pids, &outcomes);
    // WP-43: an armed revert stage applies to this revert only, whether it fired or not. R-51: each process by what happened in it.
    std::vector<std::string> unused;
    for (const std::uint32_t p : pids) {
        for (std::string& n : finish_injection(*this, "revert", p, outcomes[p])) {
            unused.push_back(std::move(n));
        }
    }
    if (result) {
        set_injection_disarmed(*result, unused);
    }
    return result;
}

Result<Value> ReloadService::revert_impl(const Value& args, bool acquired, std::vector<std::uint32_t>* pids, std::map<std::uint32_t, std::string>* outcomes) {
    std::uint32_t pid = 0;
    std::uint32_t timeout_ms = 60000;
    std::optional<Diag> error;
    if (!read_uint(args, "pid", 0xFFFFFFFFu, &pid, &error) || !read_uint(args, "timeout_ms", 3600u * 1000u, &timeout_ms, &error)) {
        return *error;
    }
    if (timeout_ms == 0) {
        timeout_ms = 60000;
    }
    const std::string module = args.get_string("module");
    if (const Value* m = args.find("module"); m != nullptr && !m->is_null() && !m->is_string()) {
        return bad_request("argument 'module' must be a string");
    }
    RevertScope scope = RevertScope::Last;
    if (const Value* sc = args.find("scope"); sc != nullptr && !sc->is_null()) {
        if (!sc->is_string() || (sc->as_string() != "last" && sc->as_string() != "all")) {
            return Diag::make(revert::diag::kBadScope, "argument 'scope' must be \"last\" or \"all\"",
                              "Send {\"scope\":\"last\"} to undo the newest reload, {\"scope\":\"all\"} to undo every reload.");
        }
        scope = sc->as_string() == "all" ? RevertScope::All : RevertScope::Last;
    }
    if (!acquired) {  // revert() could not take the run lock: a reload (or a restart) runs
        const Diag d = Diag::make(pd::kBusy, "a reload is still running", "Wait for it to finish (control command `wait`) and try again.");
        if (events_ != nullptr) {
            events_->publish(make_diag_event(d, "error", "revert"));
        }
        return d;
    }

    const std::vector<Target> list = targets(pid, module);
    for (const Target& t : list) {
        const std::uint32_t p = t.link->snapshot().pid;
        if (std::find(pids->begin(), pids->end(), p) == pids->end()) {
            pids->push_back(p);
        }
    }
    if (pid != 0 && pids->empty()) {
        pids->push_back(pid);
    }
    RevertOptions revert_options;
    revert_options.timeout_ms = options().hooks_timeout_ms;
    revert_options.deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);  // one deadline for every module and step
    revert_options.events = events_;
    if (scope == RevertScope::Last) {
        // "last" is the newest reload: the modules that reload did not touch keep their older ones.
        for (const Target& t : list) {
            std::lock_guard<std::mutex> lock(t.module->mutex);
            if (!t.module->batches.empty()) {
                revert_options.only_reload = std::max(revert_options.only_reload, t.module->batches.back().reload_id);
            }
        }
    }
    Value reverted = Value::array();
    Value failed = Value::array();
    std::size_t reverted_count = 0;
    std::size_t failed_count = 0;
    std::optional<Diag> first_error;
    for (const Target& t : list) {
        const std::uint32_t target_pid = t.link->snapshot().pid;
        Result<RevertReport> r = revert_module(*t.link, *t.module, scope, revert_options);
        if (!r) {
            (*outcomes)[target_pid] += r.error().format() + "\n";
            CML_LOG(Warn, "revert: %s", r.error().format().c_str());
            if (events_ != nullptr) {
                events_->publish(make_diag_event(r.error(), "error", "revert", target_pid));
            }
            Value entry = Value::object();
            entry.set("pid", Value::integer(static_cast<std::int64_t>(target_pid)));
            entry.set("module", Value::string(t.module->name));
            entry.set("id", Value::string(r.error().id()));
            entry.set("message", Value::string(r.error().message));
            failed.push(std::move(entry));
            ++failed_count;
            if (!first_error) {
                first_error = r.error();
            }
            continue;
        }
        for (const Diag& w : r->warnings) {
            (*outcomes)[target_pid] += w.format() + "\n";
            CML_LOG(Warn, "revert: %s", w.format().c_str());
            if (events_ != nullptr) {
                events_->publish(make_diag_event(w, "warning", "revert", r->pid));
            }
        }
        if (r->batches == 0) {
            continue;
        }
        // The broker-wide generation counts the live patches: it goes back with the module's.
        generation_.fetch_sub(std::min<std::uint64_t>(generation_.load(), r->generation_before - r->generation));
        Value entry = Value::object();
        entry.set("pid", Value::integer(static_cast<std::int64_t>(r->pid)));
        entry.set("module", Value::string(r->module));
        entry.set("generation", Value::integer(static_cast<std::int64_t>(r->generation)));
        entry.set("batches", Value::integer(static_cast<std::int64_t>(r->batches)));
        entry.set("patches", Value::integer(static_cast<std::int64_t>(r->patches)));
        entry.set("hooks_restored", Value::integer(static_cast<std::int64_t>(r->hooks_restored)));
        entry.set("hooks_repointed", Value::integer(static_cast<std::int64_t>(r->hooks_repointed)));
        reverted.push(std::move(entry));
        ++reverted_count;
    }
    if (reverted_count == 0 && first_error) {
        Diag d = *first_error;
        if (failed_count > 1) {
            d.message += " (" + std::to_string(failed_count) + " of " + std::to_string(list.size()) + " modules failed)";
        }
        return d;
    }
    if (reverted_count == 0) {
        const Diag d = Diag::make(revert::diag::kNothingToRevert,
                                  std::string("no patch to revert") + (list.empty() ? ": no enabled module" : "") + (pid != 0 ? " in process " + std::to_string(pid) : "") +
                                      (module.empty() ? "" : " matching '" + module + "'"),
                                  "A revert undoes patches that a `reload` applied in this session; reload first.");
        if (events_ != nullptr) {
            events_->publish(make_diag_event(d, "error", "revert"));
        }
        return d;
    }
    Value result = Value::object();
    result.set("reverted", std::move(reverted));
    result.set("generation", Value::integer(static_cast<std::int64_t>(generation_.load())));
    if (failed_count != 0) {
        // Some modules were reverted and others were not: the answer says which, so a retry does not undo the first ones again.
        const Diag d = Diag::make(revert::diag::kRevertPartial,
                                  std::to_string(failed_count) + " of " + std::to_string(reverted_count + failed_count) + " modules could not be reverted: " + first_error->message,
                                  "The modules in `reverted` are done; fix the cause for those in `failed` and send `revert` again (it only touches what is still applied).");
        CML_LOG(Warn, "revert: %s", d.format().c_str());
        if (events_ != nullptr) {
            events_->publish(make_diag_event(d, "error", "revert"));
        }
        result.set("partial", Value::boolean(true));
        result.set("failed", std::move(failed));
        result.set("diag", Value::string(d.id()));
    }
    return result;
}

// ---- compile on save (WP-14) -----------------------------------------------------------------------------------

std::vector<std::string> ReloadService::tracked_files() const {
    std::vector<std::shared_ptr<ModuleState>> modules;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& a : agents_) {
            for (const auto& m : a.second.modules) {
                modules.push_back(m.second);
            }
        }
    }
    std::set<std::string> files;
    for (const std::shared_ptr<ModuleState>& m : modules) {
        std::shared_ptr<ModuleIndex> index;
        {
            std::lock_guard<std::mutex> lock(m->mutex);
            index = m->index;
        }
        if (index && index->status().state == IndexState::Ready) {
            for (std::string& f : index->tracked_files()) {
                files.insert(std::move(f));
            }
        }
    }
    return std::vector<std::string>(files.begin(), files.end());
}

SaveWatcherHooks::ReloadResult ReloadService::reload_for_save() {
    Result<Value> r = run_reload(Value::object(), true);
    if (!r && r.error().code == header::diag::kTooManyCompilands) {
        // A save that touches more compilands than max_affected_compilands: the pipeline has published CML5551 (with
        // the way to confirm). Nobody is there to be asked, so nothing is compiled; the files stay "changed" and the
        // next explicit `reload` (with "confirm": true) or a smaller save picks them up.
        CML_LOG(Info, "compile-on-save: the change affects too many compilands; run `reload` with \"confirm\": true to recompile them");
        return SaveWatcherHooks::ReloadResult::Done;
    }
    if (r) {
        return SaveWatcherHooks::ReloadResult::Done;
    }
    return r.error().code == pd::kBusy ? SaveWatcherHooks::ReloadResult::Busy : SaveWatcherHooks::ReloadResult::Failed;
}

std::optional<Diag> ReloadService::apply_compile_on_save(bool on) {
    std::shared_ptr<SaveWatcher> old;
    std::shared_ptr<SaveWatcher> created;
    std::shared_ptr<SaveWatcher> existing;
    {
        std::lock_guard<std::mutex> lock(watch_mutex_);
        if (!on) {
            old.swap(save_watcher_);
        } else if (save_watcher_) {
            existing = save_watcher_;
        } else {
            SaveWatcherHooks hooks;
            hooks.tracked_files = [this] { return tracked_files(); };
            hooks.reload = [this] { return reload_for_save(); };
#if defined(CML_FAULT_INJECTION)
            hooks.faults = &faults_;  // WP-44: the group "watch"
#endif
            hooks.report = [this](const Diag& d, const char* severity) {
                if (events_ != nullptr) {
                    events_->publish(make_diag_event(d, severity, "watch"));
                }
            };
            SaveWatcherOptions options;
            options.factory = watcher_factory_;
            save_watcher_ = std::make_shared<SaveWatcher>(std::move(hooks), std::move(options));
            created = save_watcher_;
        }
    }
    if (old) {
        // Switching off returns at once: no new reload starts, and a reload a save already started finishes on its
        // own (it is joined when the service is destroyed).
        old->stop();
        std::lock_guard<std::mutex> lock(watch_mutex_);
        retiring_.erase(std::remove_if(retiring_.begin(), retiring_.end(),
                                       [](std::future<void>& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }),
                        retiring_.end());
        retiring_.push_back(std::async(std::launch::async, [w = std::move(old)]() mutable { w.reset(); }));
    }
    if (!on) {
        CML_LOG(Info, "compile on save: off");
        return std::nullopt;
    }
    if (created) {
        CML_LOG(Info, "compile on save: on");
        return created->rearm();
    }
    return existing->rearm();
}

void ReloadService::rearm_save_watcher() {
    std::shared_ptr<SaveWatcher> watcher;
    {
        std::lock_guard<std::mutex> lock(watch_mutex_);
        watcher = save_watcher_;
    }
    if (watcher) {
        (void)watcher->rearm();
    }
}

void ReloadService::set_watcher_factory_for_testing(WatcherFactory factory) {
    std::lock_guard<std::mutex> lock(watch_mutex_);
    watcher_factory_ = std::move(factory);
}

// ---- wait ---------------------------------------------------------------------------------------------------

Result<Value> ReloadService::wait(const Value& args) {
    std::uint32_t timeout_ms = 60000;
    std::optional<Diag> error;
    if (!read_uint(args, "timeout_ms", 3600u * 1000u, &timeout_ms, &error)) {
        return *error;
    }
    const std::string what = args.find("for") != nullptr ? args.get_string("for") : "idle";
    if (what != "idle" && what != "index") {
        return bad_request("argument 'for' must be \"idle\" or \"index\"");
    }
    const Clock::time_point start = Clock::now();
    const Clock::time_point deadline = start + std::chrono::milliseconds(timeout_ms);
    const auto remaining = [&] {
        return static_cast<std::uint32_t>(std::max<long long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count()));
    };
    std::vector<std::shared_ptr<ModuleState>> modules;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& a : agents_) {
            for (const auto& m : a.second.modules) {
                modules.push_back(m.second);
            }
        }
    }
    Value failed = Value::array();
    for (const std::shared_ptr<ModuleState>& m : modules) {
        if (m->index && !m->index->wait(remaining())) {
            return Diag::make(pd::kReloadTimeout, "the PDB index of '" + m->name + "' is still loading (" + m->index->status().phase + ")",
                              "Raise `timeout_ms`; a large PDB can take a few seconds.");
        }
        if (m->index && m->index->status().state == IndexState::Failed) {
            failed.push(Value::string(m->name));
        }
    }
    if (what == "idle") {
        // WP-45: a re-apply that is queued (its module's index still loading) or running counts as a reload.
        while (running_.load() || reapply_pending_.load() != 0) {
            if (remaining() == 0) {
                return Diag::make(pd::kReloadTimeout, "a reload is still running", "Raise `timeout_ms`.");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    Value result = Value::object();
    result.set("for", Value::string(what));
    result.set("modules", Value::integer(static_cast<std::int64_t>(modules.size())));
    result.set("failed", std::move(failed));
    result.set("waited_ms", Value::integer(static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count())));
    return result;
}

// ---- set-option ---------------------------------------------------------------------------------------------

Result<Value> ReloadService::set_option(const Value& args) {
    const Value* name_v = args.find("name");
    const Value* value = args.find("value");
    if (name_v == nullptr || !name_v->is_string() || value == nullptr) {
        return bad_request("set-option needs a string \"name\" and a \"value\"");
    }
    const std::string name = name_v->as_string();
    std::unique_lock<std::mutex> lock(mutex_);
    ReloadOptions& o = options_;
    const auto want_bool = [&](bool* field) -> std::optional<Diag> {
        if (!value->is_bool()) {
            return bad_request("option '" + name + "' takes a boolean");
        }
        *field = value->as_bool();
        return std::nullopt;
    };
    const auto want_uint = [&](std::uint32_t* field, std::uint32_t lo, std::uint32_t hi) -> std::optional<Diag> {
        if (!value->is_integer() || value->as_integer() < static_cast<std::int64_t>(lo) || value->as_integer() > static_cast<std::int64_t>(hi)) {
            return bad_request("option '" + name + "' takes an integer from " + std::to_string(lo) + " to " + std::to_string(hi));
        }
        *field = static_cast<std::uint32_t>(value->as_integer());
        return std::nullopt;
    };
    const auto want_string = [&](std::string* field) -> std::optional<Diag> {
        if (!value->is_string()) {
            return bad_request("option '" + name + "' takes a string");
        }
        *field = value->as_string();
        return std::nullopt;
    };
    Value previous;
    std::optional<Diag> error;
    bool toggle = false;
    if (name == "narrow_hooks") {
        previous = Value::boolean(o.narrow_hooks);
        error = want_bool(&o.narrow_hooks);
    } else if (name == "release_pdb_after_reload") {
        previous = Value::boolean(o.release_pdb_after_reload);
        error = want_bool(&o.release_pdb_after_reload);
    } else if (name == "track_system_headers") {
        previous = Value::boolean(o.track_system_headers);
        error = want_bool(&o.track_system_headers);
    } else if (name == "prefetch_module_image") {  // WP-50: applies to modules enabled from now on
        previous = Value::boolean(o.prefetch_module_image);
        error = want_bool(&o.prefetch_module_image);
    } else if (name == "cache_include_scan") {  // WP-50: applies to indexes started from now on
        previous = Value::boolean(o.cache_include_scan);
        error = want_bool(&o.cache_include_scan);
    } else if (name == "max_parallel_compiles") {
        previous = Value::integer(o.max_parallel_compiles);
        std::uint32_t v = o.max_parallel_compiles;
        error = want_uint(&v, 0, 256);
        o.max_parallel_compiles = v;
    } else if (name == "max_affected_compilands") {
        previous = Value::integer(o.max_affected_compilands);
        std::uint32_t v = o.max_affected_compilands;
        error = want_uint(&v, 0, 1000000);
        o.max_affected_compilands = v;
    } else if (name == "agent_timeout_ms") {
        previous = Value::integer(o.agent_timeout_ms);
        error = want_uint(&o.agent_timeout_ms, 100, 3600u * 1000u);
    } else if (name == "hooks_timeout_ms") {
        previous = Value::integer(o.hooks_timeout_ms);
        error = want_uint(&o.hooks_timeout_ms, 100, 3600u * 1000u);
    } else if (name == "reserve_block_mb") {
        previous = Value::integer(o.reserve_block_mb);
        // A patch needs room for at least 4 MiB in a block (PatchPipeline's kMinPatchRoom): smaller blocks could
        // never be used.
        if (value->is_integer() && value->as_integer() >= 1 && value->as_integer() < 4) {
            return bad_request("option 'reserve_block_mb' must be at least 4 (MiB): a smaller block cannot hold a patch");
        }
        error = want_uint(&o.reserve_block_mb, 4, 1024);
    } else if (name == "reserve_blocks") {
        previous = Value::integer(o.reserve_blocks);
        error = want_uint(&o.reserve_blocks, 1, 64);
    } else if (name == "max_load_attempts") {
        previous = Value::integer(o.max_load_attempts);
        std::uint32_t v = o.max_load_attempts;
        error = want_uint(&v, 1, 5);
        o.max_load_attempts = v;
    } else if (name == "linker") {
        previous = Value::string(o.linker_override);
        std::string v = o.linker_override;
        error = want_string(&v);
        if (!error && v != "" && v != "link" && v != "lld-link") {
            error = bad_request("option 'linker' takes \"\", \"link\" or \"lld-link\"");
        }
        if (!error) {
            o.linker_override = v;
        }
    } else if (name == "linker_path") {
        previous = Value::string(o.linker_path);
        error = want_string(&o.linker_path);
    } else if (name == "compile_on_save") {
        previous = Value::boolean(o.compile_on_save);
        error = want_bool(&o.compile_on_save);
        toggle = true;
    } else if (name == "crash_handler") {
        previous = Value::boolean(o.crash_handler);
        error = want_bool(&o.crash_handler);
    } else if (name == "break_with_debugger") {
        previous = Value::boolean(o.break_with_debugger);
        error = want_bool(&o.break_with_debugger);
    } else if (name == "log_level") {
        static const char* const kNames[] = {"trace", "debug", "info", "warn", "error", "off"};
        previous = Value::string(kNames[static_cast<int>(log_level())]);
        if (!value->is_string()) {
            error = bad_request("option 'log_level' takes a string");
        } else {
            error = bad_request("option 'log_level' takes trace, debug, info, warn, error or off");
            for (int i = 0; i < 6; ++i) {
                if (value->as_string() == kNames[i]) {
                    set_log_level(static_cast<LogLevel>(i));
                    error.reset();
                }
            }
        }
    } else {
        return bad_request("unknown option '" + name +
                           "'; options: narrow_hooks, release_pdb_after_reload, track_system_headers, prefetch_module_image, cache_include_scan, max_parallel_compiles, "
                           "max_affected_compilands, agent_timeout_ms, "
                           "hooks_timeout_ms, reserve_block_mb, reserve_blocks, max_load_attempts, linker, linker_path, compile_on_save, crash_handler, break_with_debugger, log_level");
    }
    if (error) {
        return *error;
    }
    CML_LOG(Info, "option %s changed", name.c_str());
    const bool compile_on_save = o.compile_on_save;
    lock.unlock();  // the watcher's thread takes mutex_ (options()) while it reloads
    if (toggle) {
        const std::optional<Diag> unavailable = apply_compile_on_save(compile_on_save);
        if (unavailable && unavailable->code == watch::diag::kUnsupported) {
            // No directory watcher on this host: the option cannot be active, so it must not look as if it were.
            {
                std::lock_guard<std::mutex> relock(mutex_);
                options_.compile_on_save = false;
            }
            (void)apply_compile_on_save(false);
            return *unavailable;
        }
    }
    OptionObserver observer;
    {
        std::lock_guard<std::mutex> observer_lock(observer_mutex_);
        observer = option_observer_;
    }
    if (observer) {
        observer(name);
    }
    Value result = Value::object();
    result.set("name", Value::string(name));
    result.set("previous", std::move(previous));
    return result;
}

void ReloadService::set_option_observer(OptionObserver observer) {
    std::lock_guard<std::mutex> lock(observer_mutex_);
    option_observer_ = std::move(observer);
}

}  // namespace cml::core
