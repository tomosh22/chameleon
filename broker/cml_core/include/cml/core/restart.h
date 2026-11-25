#pragma once

// Hot-restart (plan section 4.12, WP-21): the control command `restart`.
//
//   1. The broker asks the agent of the process to prepare (Restart). The agent runs the application's
//      CML_PRERESTART_HOOK functions and answers with what the broker needs to start the program again: the command
//      line, the working directory and the environment block captured when the agent started, the image path, the
//      values of its standard handles, and the modules the application enabled. A process that does not answer is not
//      touched.
//   2. The broker takes copies of the standard handles out of the old process, carries the source-hash baselines of
//      its modules over (see below), terminates it, and starts the same image with the same parameters, the
//      environment plus CML_RESTART_TOKEN=<token>, and the copied standard handles as the only inherited handles.
//   3. The new agent's Hello carries the token; that is how the broker recognises the process it started. The broker
//      then makes sure the same modules are enabled (those the application did not enable by itself are enabled by an
//      agent request, ReenableModules) and waits for their PDB indexes.
//   4. Restart keeps the session's changes: the relaunched process runs the original binary, but the edits that had
//      been patched into the old process are still on disk. The baseline of every module is set back to the content
//      the original binary was built from (SourceBaseline::restore), and one ordinary reload then recompiles and patches
//      every file that had been patched before. Edits that were saved but not reloaded stay pending.
//
// Early debugger re-attach (WP-34, ADR-0040): `restart {hold_new:true, hold_ms?}` creates the new process suspended and
// publishes `restarting {old_pid,new_pid}`. A debugger can attach to the process before its first instruction; the client
// then sends `restart-resume {pid}` (or the broker resumes the process itself after hold_ms, default 10 s, at most 60 s).
// The registration budget starts when the process is resumed. A suspended process is never left behind: it is resumed on
// timeout, on shutdown, when the requesting client disconnects, when the process host is destroyed and on every error path
// (docs/wp/WP-34.md lists them). Without hold_new nothing here changes.
//
// WP-43 (ADR-0043): failure injection reaches the sequence (stages pre-restart-hooks .. restart-reapply, cml/ipc/fault_stages.h), and
// a restart that could not finish its work in the new process (the registration failed, a module was not enabled again) is remembered
// for that process: its next restart enables those modules and re-applies those edits ("unfinished restart", docs/wp/WP-43.md).
// A broker that shuts down ends a running restart at its next wait (CML5821), whatever step it is in.
//
// The operating system part (open / terminate / start a process, duplicate handles) is behind ProcessHost so that the
// whole sequence is unit-tested with a scripted agent on any host.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cml/core/restart_diag_ids.h"
#include "cml/core/source_baseline.h"
#include "cml/ipc/json.h"
#include "cml/ipc/messages.h"
#include "cml/util/result.h"

namespace cml::core {

class EventBus;
class ReloadService;

// What the new process is started with.
struct RelaunchSpec {
    std::u16string image_path;
    std::u16string command_line;
    std::u16string working_directory;
    std::u16string environment;  // "A=b\0C=d\0\0"
    bool suspended = false;      // WP-34: create the process suspended (its first thread does not run until resume())
};

// The operating-system handle on one process that is about to be restarted (everything that needs the old process to
// still exist was done when it was opened).
class RestartableProcess {
public:
    virtual ~RestartableProcess() = default;
    // Terminates the old process and waits for it to be gone.
    virtual Result<void> terminate(std::uint32_t timeout_ms) = 0;
    // Starts the new process; returns its pid. The standard handles of the old process are handed on. WP-43: may be called again after
    // it failed (the broker tries twice); what it needs (the copied handles) is kept until it succeeds or the object is destroyed.
    // A failure must not leave a process behind: if the attempt created one and a later step of it failed, the host terminates that
    // process before it returns the failure, and relaunch_created_process() says so; the broker then does NOT try again (it never starts
    // a second copy of the application).
    virtual Result<std::uint32_t> relaunch(const RelaunchSpec& spec) = 0;
    // WP-43: after a failed relaunch(): whether that attempt had created a process (which it terminated). False: no process existed.
    virtual bool relaunch_created_process() const { return false; }
    // The exit code of the relaunched process once it has exited.
    virtual std::optional<std::uint32_t> relaunched_exit_code() = 0;
    // WP-34: lets a process that relaunch() created suspended (spec.suspended) run. Calling it again, or for a process that was
    // not suspended, succeeds without doing anything. A host that cannot resume a process it could not resume terminates it
    // instead of leaving it suspended, and says so in the diagnostic (CML5817). The destructor of a host process that is still
    // suspended resumes it too.
    virtual Result<void> resume() { return Result<void>(); }
};

class ProcessHost {
public:
    virtual ~ProcessHost() = default;
    // Opens process `pid` and takes copies of its standard handles (values in that process). A handle that cannot be
    // copied is reported as a warning in `warnings` and the new process gets none for it. `info_flags` are the
    // kRestartInfo* flags the agent reported (whether the process had a console: the new one gets the same, see
    // docs/wp/WP-21.md "Console"). CML5805 when the process cannot be opened at all.
    virtual Result<std::unique_ptr<RestartableProcess>> open(std::uint32_t pid, const std::uint64_t std_handles[3], std::uint32_t info_flags,
                                                             std::vector<Diag>* warnings) = 0;
    // WP-34: whether relaunch() honours RelaunchSpec::suspended and resume() works. Gates the capability `restart-hold`.
    virtual bool supports_hold() const { return false; }
};

// The real thing on Windows; null elsewhere.
std::shared_ptr<ProcessHost> native_process_host();

#if defined(_WIN32)
// WP-43 (R-40): the executable the hold watchdog runs: this process's own image when it is cml_broker.exe. `module_file_name` stands for
// GetModuleFileNameW(nullptr, buffer, capacity) (tests pass a fake). When there is none it says why in the log: at warn level when a call
// failed or the path does not fit (a broker that is killed while holding would then leave the process suspended), at debug level when this
// executable simply is not cml_broker.exe (a test host or a tool embedding the library).
std::optional<std::wstring> hold_watchdog_executable(const std::function<unsigned long(wchar_t* buffer, unsigned long capacity)>& module_file_name);

// WP-34: the hidden mode `cml_broker.exe --hold-watchdog <broker> <thread> <ended> <done> <max_ms>` (handle values inherited from the broker).
// Resumes the held process's thread when the broker dies while holding it; see restart_win.cpp. `args` are the arguments after the flag.
int run_hold_watchdog(const std::vector<std::string>& args);
#endif

// `block` with any CML_RESTART_TOKEN entry removed and CML_RESTART_TOKEN=<token> added in sorted position.
std::u16string environment_with_token(const std::u16string& block, const std::string& token);

class RestartCoordinator {
public:
    RestartCoordinator(ReloadService& service, EventBus* events, std::shared_ptr<ProcessHost> host);

    // {"pid"?:N, "timeout_ms"?:N, "registration_timeout_ms"?:N} -> {"old_pid","new_pid","modules":[...], ...}. pid may be
    // omitted when exactly one process is connected. `timeout_ms` (default 60 s) bounds everything before the old process is
    // terminated (the agent's pre-restart hooks included); once it is gone the new process gets its own budget,
    // `registration_timeout_ms` (default 30 s), for registering, enabling its modules, building its indexes and re-applying the
    // session's edits. Reloads are refused busy for the whole restart.
    //
    // WP-34: `hold_new` (bool) and `hold_ms` (1..60000, default 10000) make the new process start suspended; see the top of this
    // file. `client_gone`, polled while a process is held, tells the restart that the client that asked for it has disconnected
    // (the process is resumed at once and the restart continues). Both are optional. `token` (a string of at most 64 characters) in
    // the arguments is echoed in the `restarting` event, so a client can tell its own held restart from another client's.
    Result<ipc::json::Value> restart(const ipc::json::Value& args, const std::function<bool()>& client_gone = {});

    // WP-34, command `restart-resume {pid}` -> {pid, resumed:true, held_ms}. Answers once the process runs. CML5814 when no process
    // with that pid is held (it never was, or it was resumed already: the message says which), CML5818 for bad arguments. WP-43: CML5820
    // when the restart did not confirm the resume in time (the process may still be held; the broker resumes it when hold_ms passes).
    Result<ipc::json::Value> resume(const ipc::json::Value& args);
    // Whether this coordinator can hold a new process (its host can).
    bool hold_supported() const;
    // WP-43 (R-40): the process host can be replaced (ReloadService::set_process_host) while a restart runs or holds a process. The
    // coordinator stays, so `restart-resume` and a shutdown still reach the restart that holds it; a running restart keeps the host it
    // started with. Null: no restart can start.
    void set_host(std::shared_ptr<ProcessHost> host);
    bool has_host() const;
    // Broker shutdown: every process that is held is resumed at once and its restart ends with CML5815; later holds end at once.
    void abort_holds();
    // A restart is running (also while it holds a process). The broker does not exit as idle meanwhile.
    bool active() const { return active_.load() != 0; }

    // How long the broker waits for the application to enable its modules by itself before it asks the agent to
    // (tests shorten it).
    void set_reenable_grace_for_testing(std::chrono::milliseconds grace) { grace_ = grace; }
    // Called with "prepared", "snapshot" and "terminate" as the sequence passes them (tests check their order). WP-43 adds "hold-ending"
    // (a held restart: the hold's end was decided by the restart thread, before it is recorded).
    void set_trace_for_testing(std::function<void(const std::string&)> trace) { trace_ = std::move(trace); }
    // WP-43: how long `restart-resume` waits for the restart thread to confirm that the process runs (default 10 s), and the pause before
    // the second attempt to start the new process (default 250 ms).
    void set_resume_confirm_wait_for_testing(std::chrono::milliseconds wait) { confirm_wait_ = wait; }
    void set_relaunch_retry_delay_for_testing(std::chrono::milliseconds delay) { relaunch_retry_delay_ = delay; }
    // WP-43: whether a restart that did not finish its work in process `pid` is remembered for it (tests).
    bool has_unfinished_for_testing(std::uint32_t pid);
    // WP-43: the agent of process `pid` disconnected (the process exited, or its agent was destroyed): what an unfinished restart left
    // for that pid is dropped, so it can never apply to another process that gets the same pid later.
    void process_gone(std::uint32_t pid);
    // WP-43: a `restart-resume` for held process `pid` has been received and is waiting for the restart thread (tests).
    bool resume_requested_for_testing(std::uint32_t pid);

private:
    // A module's source baseline as the old process left it (restart.cpp, "Restart keeps the session's changes").
    struct Carried {
        std::string path;
        BaselineSnapshot baseline;
    };
    // WP-43: what a restart could not do in its new process: modules it did not enable again, baselines it did not restore.
    struct Unfinished {
        std::string token;  // CML_RESTART_TOKEN of the process it is for: a process with the same pid but another token is another process
        std::vector<ipc::RestartModule> modules;
        std::vector<Carried> carried;
    };
    void remember_unfinished(std::uint32_t pid, Unfinished unfinished);
    std::optional<Unfinished> peek_unfinished(std::uint32_t pid);
    void forget_unfinished(std::uint32_t pid);
    std::shared_ptr<ProcessHost> host() const;

    ReloadService& service_;
    EventBus* events_;
    mutable std::mutex host_mutex_;
    std::shared_ptr<ProcessHost> host_;  // guarded by host_mutex_; restart() works on a copy
    std::chrono::milliseconds grace_{300};
    std::function<void(const std::string&)> trace_;
    std::mutex mutex_;  // one restart at a time
    std::uint64_t counter_ = 0;
    std::atomic<int> active_{0};
    std::atomic<bool> aborting_{false};

    // WP-34: the processes held now, by pid. The restart thread owns the process and resumes it; `resume()` (any thread) only asks.
    struct Hold {
        std::mutex mutex;
        std::condition_variable changed;
        bool release_requested = false;
        std::string ended_by;                 // set (under `mutex`) when the hold ended for a reason other than a requested release
        bool resumed = false;                 // the restart thread resumed the process (or tried to)
        std::optional<Diag> resume_error;
        std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    };
    std::mutex holds_mutex_;
    std::map<std::uint32_t, std::shared_ptr<Hold>> holds_;
    std::map<std::uint32_t, std::string> ended_holds_;  // pid -> how the hold ended (the last few; for the CML5814 message)
    std::deque<std::uint32_t> ended_order_;             // the same pids, oldest first (eviction)
    std::chrono::milliseconds confirm_wait_{10000};
    std::chrono::milliseconds relaunch_retry_delay_{250};

    // WP-43: unfinished restarts by the pid of the process they started (the last 16).
    std::mutex unfinished_mutex_;
    std::map<std::uint32_t, Unfinished> unfinished_;
    std::deque<std::uint32_t> unfinished_order_;
};

}  // namespace cml::core
