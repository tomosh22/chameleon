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
// The operating system part (open / terminate / start a process, duplicate handles) is behind ProcessHost so that the
// whole sequence is unit-tested with a scripted agent on any host.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cml/ipc/json.h"
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
};

// The operating-system handle on one process that is about to be restarted (everything that needs the old process to
// still exist was done when it was opened).
class RestartableProcess {
public:
    virtual ~RestartableProcess() = default;
    // Terminates the old process and waits for it to be gone.
    virtual Result<void> terminate(std::uint32_t timeout_ms) = 0;
    // Starts the new process; returns its pid. The standard handles of the old process are handed on.
    virtual Result<std::uint32_t> relaunch(const RelaunchSpec& spec) = 0;
    // The exit code of the relaunched process once it has exited.
    virtual std::optional<std::uint32_t> relaunched_exit_code() = 0;
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
};

// The real thing on Windows; null elsewhere.
std::shared_ptr<ProcessHost> native_process_host();

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
    Result<ipc::json::Value> restart(const ipc::json::Value& args);

    // How long the broker waits for the application to enable its modules by itself before it asks the agent to
    // (tests shorten it).
    void set_reenable_grace_for_testing(std::chrono::milliseconds grace) { grace_ = grace; }
    // Called with "prepared", "snapshot" and "terminate" as the sequence passes them (tests check their order).
    void set_trace_for_testing(std::function<void(const std::string&)> trace) { trace_ = std::move(trace); }

private:
    ReloadService& service_;
    EventBus* events_;
    std::shared_ptr<ProcessHost> host_;
    std::chrono::milliseconds grace_{300};
    std::function<void(const std::string&)> trace_;
    std::mutex mutex_;  // one restart at a time
    std::uint64_t counter_ = 0;
};

}  // namespace cml::core
