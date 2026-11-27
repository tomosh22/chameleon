#pragma once

// The broker's reload service (WP-12): the process/module registry behind the control pipe's `reload`, `wait`
// and `set-option` commands.
//
//   * attach()/on_session_changed()/detach() follow the agent connections. When a session enables a module the
//     service creates a ModuleState for it and starts its asynchronous PDB index (ModuleIndex); when the module
//     is disabled or the agent goes away the state is dropped (its index thread is joined, the PDB mapping is
//     released: ADR-0017).
//   * reload() runs the PatchPipeline for the enabled modules (all, or those of `pid` / `module`), one reload at
//     a time (a second one is refused with CML5304), and answers with the per-stage timing (also logged as one
//     `RELOAD-TIMING` line per patched module).
//   * wait() blocks until the indexes are loaded and no reload runs.
//   * set_option() changes ReloadOptions at run time and returns the previous value.
//   * The option compile_on_save (WP-14) starts a SaveWatcher: a save of a tracked source runs a reload.
//   * WP-45 (ADR-0043): a module instance that is enabled after the session accepted edits for its sources gets them re-applied
//     as part of its enable, the way a hot-restart re-applies them: a module freed and loaded again (FreeLibrary + LoadLibrary,
//     at the same base or another), and a module a hot-restarted process enables only after the restart finished (CML5809).
//     When a module's state is dropped because its image went away, its source baseline (BaselineSnapshot) is kept for that
//     process and path; the next instance's index starts from it (ModuleIndexSpec::carried), and a re-apply worker runs one
//     reload of that module once its index is ready. A failure is a diagnostic naming the module and the files (CML5600,
//     CML5601), never silence. A module that is disabled and enabled again while its image stays loaded takes the baseline over
//     as it is (the image still runs the patches): nothing is recompiled. Each instance has its own patch work directory.
//
// Portable: the agent transport, compiler and linker are behind AgentLink and PipelineEnv.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cml/core/agent_link.h"
#include "cml/core/event_bus.h"
#include "cml/core/fault_injection.h"
#include "cml/core/module_state.h"
#include "cml/core/pipeline.h"
#include "cml/core/save_watcher.h"
#include "cml/ipc/json.h"
#include "cml/util/result.h"

namespace cml::core {

class ProcessHost;
class RestartCoordinator;

// One reload or revert at a time (WP-12, WP-22): takes the lock without waiting and, while held, shows the service as running
// (`wait idle` waits for it).
class ExclusiveRun {
public:
    ExclusiveRun(std::mutex& mutex, std::atomic<bool>& running) : lock_(mutex, std::try_to_lock), running_(running) {
        if (lock_.owns_lock()) {
            running_.store(true);
        }
    }
    ~ExclusiveRun() {
        if (lock_.owns_lock()) {
            running_.store(false);
        }
    }
    ExclusiveRun(const ExclusiveRun&) = delete;
    ExclusiveRun& operator=(const ExclusiveRun&) = delete;
    bool acquired() const { return lock_.owns_lock(); }

private:
    std::unique_lock<std::mutex> lock_;
    std::atomic<bool>& running_;
};

class ReloadService {
public:
    // `session_dir`: base directory for objects, patch DLLs and PDBs ("" = a new directory under the system
    // temporary directory, or $CML_SESSION_DIR). `events` may be null.
    ReloadService(PipelineEnv env, EventBus* events, std::string session_dir = {});
    ~ReloadService();
    ReloadService(const ReloadService&) = delete;
    ReloadService& operator=(const ReloadService&) = delete;

    // ---- connections ----
    void attach(std::shared_ptr<AgentLink> link);
    void on_session_changed(std::uint64_t agent_id);
    void detach(std::uint64_t agent_id);

    // ---- control commands (args are the request's "args" object) ----
    // {"pid"?:N, "module"?:"name or path", "timeout_ms"?:N, "confirm"?:bool}
    Result<ipc::json::Value> reload(const ipc::json::Value& args);
    // WP-22: {"pid"?:N, "module"?:"name or path", "scope"?:"last"|"all", "timeout_ms"?:N}   (`last`: the newest reload)
    //   -> {"reverted":[{"pid","module","generation","batches","patches","hooks_restored","hooks_repointed"}],"generation":N}
    //   plus, when only some modules could be reverted, "partial":true and "failed":[{"pid","module","id","message"}] (CML5854)
    Result<ipc::json::Value> revert(const ipc::json::Value& args);
    // (WP-43: at the end of every revert the "revert" stages still armed for its processes are disarmed; the answer lists those that did
    // not fire as `injection_disarmed`.)
    // {"for"?:"idle"|"index", "timeout_ms"?:N}
    Result<ipc::json::Value> wait(const ipc::json::Value& args);
    // {"name":s, "value":s|n|b}
    Result<ipc::json::Value> set_option(const ipc::json::Value& args);

    // ---- status ----
    std::optional<IndexStatus> index_status(std::uint64_t agent_id, const std::string& module_path) const;
    bool reload_running() const noexcept { return running_.load(); }
    std::uint64_t generation() const noexcept { return generation_.load(); }
    ReloadOptions options() const;
    const std::string& session_dir() const noexcept { return session_dir_; }
    EventBus* events() const noexcept { return events_; }
    // Re-derives the watched directories from the enabled modules' indices (no-op unless compile_on_save is on).
    void rearm_save_watcher();
    // The reload a save starts (compile on save): only modules whose index is ready, nothing published when a reload
    // is running (Busy: try again) or no module is ready (Done).
    SaveWatcherHooks::ReloadResult reload_for_save();
    // Test seam: the directory watcher factory used by the next compile_on_save activation.
    void set_watcher_factory_for_testing(WatcherFactory factory);
    // WP-20: called (on the control connection's thread, without a lock held) after set-option changed an option; the name is
    // the option's. The exception service re-evaluates whether the agents should hold faulting threads.
    using OptionObserver = std::function<void(const std::string& name)>;
    void set_option_observer(OptionObserver observer);
    // Aborts running compiles and links (shutdown); sticky. Also ends every restart that holds a new process (it is resumed first, WP-34).
    void cancel();
    // WP-28: the broker-side failure injection state (control command `inject-failure`).
    FaultInjector& faults() noexcept { return faults_; }
#if defined(CML_FAULT_INJECTION)
    // WP-44: whether the process host confirms that process `pid` has exited (RestartCoordinator::process_exited; false without a host).
    bool process_exited(std::uint32_t pid);
#endif

    // The state of a module (tests, diagnostics); null when it is not enabled.
    std::shared_ptr<ModuleState> module_state(std::uint64_t agent_id, const std::string& module_path) const;

    // ---- hot-restart (WP-21, restart.h) ----
    // Gives the service the means to start and stop processes. The real host is installed on Windows; tests install a
    // scripted one. Without a host `restart` is refused (CML5800) and the capability is not advertised.
    void set_process_host(std::shared_ptr<ProcessHost> host);
    bool restart_available() const;
    void set_restart_trace_for_testing(std::function<void(const std::string&)> trace);
    // WP-43: the restart coordinator (null without a process host ever set), for the knobs of its tests.
    std::shared_ptr<RestartCoordinator> restart_coordinator_for_testing() const;
    // {"pid"?:N, "timeout_ms"?:N, "hold_new"?:bool, "hold_ms"?:N}. `client_gone` (optional) reports that the requesting client has
    // disconnected; a restart that holds a process then resumes it at once (WP-34).
    Result<ipc::json::Value> restart(const ipc::json::Value& args, const std::function<bool()>& client_gone = {});
    // WP-34: `restart-resume {pid}`, and the capability `restart-hold` (restart is available and its host can create suspended processes).
    Result<ipc::json::Value> restart_resume(const ipc::json::Value& args);
    bool restart_hold_available() const;
    // A restart is running now (the broker's idle exit waits for it: the old process is gone and the new one may be held).
    bool restart_active() const;
    // The agent connections (handshaken or not), their modules' states, and a lookup by pid / restart token.
    std::vector<std::shared_ptr<AgentLink>> links() const;
    std::shared_ptr<AgentLink> link_for_pid(std::uint32_t pid) const;
    // The connection whose Hello carried `token` and came from process `pid` (the pid the broker started; a child that
    // inherited the token cannot be mistaken for the restarted process).
    std::shared_ptr<AgentLink> link_for_restart_token(const std::string& token, std::uint32_t pid) const;
    // Holds off every reload (they are refused busy, CML5304) for as long as the returned lock lives. Not owned when a
    // reload is running. A restart calls reload_locked() meanwhile.
    std::unique_ptr<ExclusiveRun> lock_reloads();
    Result<ipc::json::Value> reload_locked(const ipc::json::Value& args);
    std::vector<std::shared_ptr<ModuleState>> module_states(std::uint64_t agent_id) const;

    // ---- WP-45: re-applying the session's edits to a module enabled later ----
    // The restart hands over the baseline of a module the new process did not enable in time (CML5809): when process `pid` (agent
    // `agent_id`) enables `module_path` later, its edits are re-applied. When the module has a state already (it was enabled just
    // now), the re-apply starts at once. `origin` names the cause in the log and in a failure ("restart").
    // The baseline goes to the agent registered for `pid` now: `agent_id` itself, or an agent that reconnected to the same process (proven
    // identity, creation time included). Returns whether it was kept (or queued for a state that exists); false: dropped, logged at info
    // with the module and the files (no such agent, or not provably the same process). The caller says so in its diagnostic.
    bool carry_to_late_module(std::uint64_t agent_id, std::uint32_t pid, const std::string& module_path, const BaselineSnapshot& snapshot,
                              const std::string& origin);
    // Re-applies queued or running (part of `wait idle`). A `reload` that arrives meanwhile waits for them instead of being busy.
    std::size_t reapply_pending() const noexcept { return reapply_pending_.load(); }
    // The baselines kept for process `pid` (path, baseline) of modules that are not enabled now; a restart of the process that agent
    // `agent_id` is carries them along. Only carries stored by that process (that agent, or one provably the same process) are returned;
    // the others are dropped (logged at info).
    std::vector<std::pair<std::string, BaselineSnapshot>> kept_baselines(std::uint64_t agent_id, std::uint32_t pid);
    // Tests: called in on_session_changed after a dropped state left the service's map and before its baseline is kept (the moment
    // another thread can hand over a carry for the same module).
    void set_drop_hook_for_testing(std::function<void()> hook);
    // Tests: every baseline kept for `pid`, whoever stored it (nothing is dropped).
    std::vector<std::pair<std::string, BaselineSnapshot>> kept_baselines_for_testing(std::uint32_t pid) const;
    // What a module's state hands on (to its next instance, or through a restart): the index's baseline (Ready) with the baseline it was
    // given and has not restored yet (ModuleState::carried_in) under it; only carried_in while its index is not Ready; nullopt without
    // either.
    std::optional<BaselineSnapshot> leaving_baseline(ModuleState& state) const;
    // Tests: whether a baseline is kept for (pid, path), and whether that one is for an image that is still loaded.
    bool has_carry_for_testing(std::uint32_t pid, const std::string& module_path, bool* live = nullptr) const;

private:
    struct AgentEntry {
        std::shared_ptr<AgentLink> link;
        std::map<std::string, std::shared_ptr<ModuleState>> modules;  // normalised path -> state
        // WP-45: set once, when the agent has registered (Hello): which process it is (restart token, command line, creation time read
        // through the process host) and whether the creation time could be read.
        bool registered = false;
        std::string identity;
        bool identity_known = false;
    };
    struct Target {
        std::shared_ptr<AgentLink> link;
        std::shared_ptr<ModuleState> module;
    };

    Result<std::string> ensure_session_dir();
    // Starts or stops the save watcher. Stopping does not wait for a reload that a save started: the old watcher is
    // retired on its own thread (joined by the destructor). Returns the error when no watcher can be created.
    std::optional<Diag> apply_compile_on_save(bool on);
    // The reload; `automatic` is a reload started by a save: it only touches modules whose index is ready, finds a
    // running reload or no module without publishing an error, and returns quietly (kBusy for "try again").
    Result<ipc::json::Value> run_reload(const ipc::json::Value& args, bool automatic, bool lock_held = false);
    std::vector<std::string> tracked_files() const;
    std::vector<Target> targets(std::uint32_t pid, const std::string& module) const;

    PatchPipeline pipeline_;
    EventBus* events_;
    std::string session_dir_;
    bool session_dir_made_ = false;
    bool keep_session_dir_ = false;
    std::shared_ptr<ExportCache> exports_ = std::make_shared<ExportCache>();
    toolchain::CancelToken cancel_;
    FaultInjector faults_;  // WP-28

    mutable std::mutex mutex_;  // agents_, options_
    std::map<std::uint64_t, AgentEntry> agents_;
    ReloadOptions options_;

    std::mutex watch_mutex_;  // save_watcher_, watcher_factory_
    std::shared_ptr<SaveWatcher> save_watcher_;
    std::vector<std::future<void>> retiring_;  // stopped save watchers finishing a reload
    WatcherFactory watcher_factory_;

    // `acquired`: revert() holds the run lock. `outcomes`: the diagnostics of each process (R-51: an agent stage's id in one process's
    // outcome says it fired there, not in the others).
    Result<ipc::json::Value> revert_impl(const ipc::json::Value& args, bool acquired, std::vector<std::uint32_t>* pids,
                                         std::map<std::uint32_t, std::string>* outcomes);
    std::shared_ptr<RestartCoordinator> restart_;  // guarded by mutex_; restart() works on a copy
    std::mutex observer_mutex_;
    OptionObserver option_observer_;

    // ---- WP-45 ----
    // The baseline an instance of a module left, for its next instance in the same process. `live_base` is the base of an image
    // that is still loaded (the module was disabled, not freed); 0 once that image is gone (or it never was: a restart's carry).
    struct Carry {
        std::string path;
        BaselineSnapshot snapshot;
        std::uint64_t live_base = 0;
        std::string origin;  // "reload" (the module was loaded again), "disable", "restart"
        // The process it is for: restart token, command line and creation time (AgentEntry::identity of the agent that stored it), and
        // whether the creation time was known. Another agent that registers with the pid keeps it only on a proven match.
        std::string identity;
        bool identity_known = false;
        std::uint64_t agent_id = 0;  // the agent that stored it (or took it over on a proven match)
    };
    // Drops every carry of `pid`. Needs mutex_.
    void drop_carries_locked(std::uint32_t pid, const char* why);
    // Which process each agent that ever registered is (kept after it disconnects, so a later agent can be compared with it), and the
    // agent that registered last for each pid. Guarded by mutex_.
    struct AgentIdentity {
        std::uint32_t pid = 0;
        std::string identity;
        bool known = false;  // the creation time could be read
    };
    std::map<std::uint64_t, AgentIdentity> identities_;
    std::map<std::uint32_t, std::uint64_t> registered_by_pid_;
    std::function<void()> drop_hook_;  // guarded by mutex_ (set_drop_hook_for_testing)
    // Agents `a` and `b` are provably the same process: the same agent, or both identities known (creation time read) and equal.
    bool proven_same_locked(std::uint64_t a, std::uint64_t b) const;
    // A carry may be consumed, merged into or handed to a restart by `agent_id` only if the agent that stored it is provably the same
    // process. False (and logged at info, with the module and the files: the caller drops the carry) otherwise. Needs mutex_.
    bool carry_usable_locked(const Carry& carry, std::uint32_t pid, std::uint64_t agent_id, const char* what) const;
    // An agent registered with `pid`: the carries other agents stored for it stay only when both identities are known and equal (the
    // same process, reconnected); the others are dropped (logged at info, with the module and the files). Needs mutex_.
    void check_carries_on_register_locked(std::uint32_t pid, std::uint64_t agent_id, const std::string& identity, bool identity_known);
    std::shared_ptr<ProcessHost> process_host_;  // guarded by mutex_: whether a process has exited (detach), its creation time
    using InstanceKey = std::pair<std::uint32_t, std::string>;  // pid, normalised path
    std::map<InstanceKey, Carry> carries_;                       // guarded by mutex_
    std::map<InstanceKey, std::uint64_t> instances_;             // guarded by mutex_: instances made so far (ModuleState::instance)
    struct ReapplyItem {
        std::uint64_t agent_id = 0;
        std::uint32_t pid = 0;
        std::weak_ptr<ModuleState> state;
        std::optional<BaselineSnapshot> restore;  // restored by the worker (the state existed before the carry arrived); else the index applied it
        std::vector<std::string> files;           // the files with accepted edits, for the log and a failure
        std::string origin;
        std::uint64_t generation = 0;             // the module's generation when it was queued (a reload since may have applied it)
    };
    void queue_reapply(ReapplyItem item);
    void reapply_loop();
    void reapply_one(const ReapplyItem& item);
    bool reapply_stopping() const;
    mutable std::mutex reapply_mutex_;  // guards the members below
    std::condition_variable reapply_changed_;
    std::deque<ReapplyItem> reapply_queue_;
    std::thread reapply_thread_;
    bool reapply_stop_ = false;
    std::atomic<std::size_t> reapply_pending_{0};  // queued + running

    // WP-50 (R-55): the scan results of the module files this session indexed (ModuleIndexSpec::scan_cache).
    std::shared_ptr<ScanCache> scan_cache_ = std::make_shared<ScanCache>();
    // WP-50 (R-47): background reads of the module files the reloads compare with (ModuleState::ensure_image), queued when a
    // module's index becomes Ready (ReloadOptions::prefetch_module_image). One worker thread, started on first use, joined by the
    // destructor. It holds a state only while it reads that state's file.
    void queue_image_prefetch(std::weak_ptr<ModuleState> state);
    void prefetch_loop();
    std::mutex prefetch_mutex_;  // guards the members below
    std::condition_variable prefetch_changed_;
    std::deque<std::weak_ptr<ModuleState>> prefetch_queue_;
    std::thread prefetch_thread_;
    bool prefetch_stop_ = false;

    std::mutex reload_mutex_;  // one reload at a time
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::uint64_t> reload_counter_{0};  // WP-22: numbers the reload commands (PatchBatch::reload_id)
};

}  // namespace cml::core
