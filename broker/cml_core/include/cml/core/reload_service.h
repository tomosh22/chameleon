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
//
// Portable: the agent transport, compiler and linker are behind AgentLink and PipelineEnv.

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cml/core/agent_link.h"
#include "cml/core/event_bus.h"
#include "cml/core/module_state.h"
#include "cml/core/pipeline.h"
#include "cml/core/save_watcher.h"
#include "cml/ipc/json.h"
#include "cml/util/result.h"

namespace cml::core {

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
    // Aborts running compiles and links (shutdown); sticky.
    void cancel() { cancel_.cancel(); }

    // The state of a module (tests, diagnostics); null when it is not enabled.
    std::shared_ptr<ModuleState> module_state(std::uint64_t agent_id, const std::string& module_path) const;

private:
    struct AgentEntry {
        std::shared_ptr<AgentLink> link;
        std::map<std::string, std::shared_ptr<ModuleState>> modules;  // normalised path -> state
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
    Result<ipc::json::Value> run_reload(const ipc::json::Value& args, bool automatic);
    std::vector<std::string> tracked_files() const;
    std::vector<Target> targets(std::uint32_t pid, const std::string& module) const;

    PatchPipeline pipeline_;
    EventBus* events_;
    std::string session_dir_;
    bool session_dir_made_ = false;
    bool keep_session_dir_ = false;
    std::shared_ptr<ExportCache> exports_ = std::make_shared<ExportCache>();
    toolchain::CancelToken cancel_;

    mutable std::mutex mutex_;  // agents_, options_
    std::map<std::uint64_t, AgentEntry> agents_;
    ReloadOptions options_;

    std::mutex watch_mutex_;  // save_watcher_, watcher_factory_
    std::shared_ptr<SaveWatcher> save_watcher_;
    std::vector<std::future<void>> retiring_;  // stopped save watchers finishing a reload
    WatcherFactory watcher_factory_;

    std::mutex observer_mutex_;
    OptionObserver option_observer_;

    std::mutex reload_mutex_;  // one reload at a time
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> generation_{0};
};

}  // namespace cml::core
