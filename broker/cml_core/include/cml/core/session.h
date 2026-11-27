#pragma once

// One agent connection as seen by the broker: a small, transport-independent state machine.
//
// WP-11 scope: handshake, module bookkeeping and enable/disable. WP-12 grows this into the real
// session (process registry, bind planner, patch pipeline); the shape - messages in, replies out,
// all state private to the session - is meant to survive that.
//
// Portable (no Win32) so it can be unit-tested without pipes.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "cml/ipc/messages.h"
#include "cml/util/diag.h"

namespace cml::core {

struct SessionOutcome {
    std::vector<ipc::Message> replies;  // to send back, in order
    bool close = false;                 // protocol violation: drop the connection after sending
    // WP-48: warnings for the connection thread to log (and to publish as `diag` events where module_event_diag_ids.h says so):
    // CML5620 (module changes reported from a module-list diff), CML5621 (an agent without module events).
    std::vector<Diag> warnings;
};

// The broker's own requests (ReserveRange, LoadPatch, ApplyFixups, RunInitializers, InstallHooks, ReleaseRange) are
// answered by the agent with Reply, ReserveRangeResult, PatchLoaded or HooksInstalled. A Session does not interpret
// those answers; it hands them to the sink (see AgentLink::deliver) and, like WP-10 asked, ignores answers whose
// request id nobody waits for any more (a reply that arrives after its request timed out).
using AnswerSink = std::function<bool(const ipc::Message& answer)>;  // true: somebody was waiting for it

class Session {
public:
    Session(std::uint64_t agent_id, std::string broker_version);

    void set_answer_sink(AnswerSink sink) { answer_sink_ = std::move(sink); }
    // WP-20: the agent reports a thread it holds after a fault in an enabled module (Exception, an event nobody answers).
    using ExceptionSink = std::function<void(const ipc::Exception& exception)>;
    void set_exception_sink(ExceptionSink sink) { exception_sink_ = std::move(sink); }
    // Answers that no sink consumed (unknown request id, or no sink): counted for tests and diagnostics.
    std::uint64_t ignored_answers() const noexcept { return ignored_answers_; }
    ipc::AgentMode mode() const noexcept { return mode_; }

    // Handles one message from the agent. Every incoming message is logged at info level; a
    // request gets exactly one reply (Welcome for Hello, Reply for the others).
    // WP-48: a ModuleEvents is applied one event at a time, and `after_step` (when set) runs after each event whose base comes up
    // again later in the same message, with the module list as it is after that event: the connection thread publishes the session
    // there and lets the reload service see it, so an unload and a load of the same image at the same base are two changes (the
    // WP-45 re-enable path), never none. The caller observes once more after the message, as for every message.
    SessionOutcome on_message(const ipc::Message& message, const std::function<void()>& after_step = {});

    std::uint64_t agent_id() const noexcept { return agent_id_; }
    bool handshaken() const noexcept { return handshaken_; }
    std::uint32_t pid() const noexcept { return pid_; }
    const std::string& agent_path() const noexcept { return agent_path_; }
    const std::string& command_line() const noexcept { return command_line_; }
    // WP-21: CML_RESTART_TOKEN of the process ("" unless the broker relaunched it).
    const std::string& restart_token() const noexcept { return restart_token_; }
    // WP-48: the wire ABI the agent announced (Hello), and whether it reports the loader's module events (ABI 3 or more).
    std::uint32_t agent_abi_version() const noexcept { return agent_abi_version_; }
    bool module_events() const noexcept { return agent_abi_version_ >= ipc::kAgentAbiModuleEvents; }
    // WP-48: module events applied so far, and ModuleEvents that were resyncs (tests, status).
    std::uint64_t module_events_applied() const noexcept { return module_events_applied_; }
    std::uint64_t module_event_resyncs() const noexcept { return module_event_resyncs_; }
    const std::vector<ipc::ModuleEntry>& modules() const noexcept { return modules_; }
    const std::vector<std::string>& enabled_modules() const noexcept { return enabled_; }
    // Incremented whenever what a status report shows changes (handshake, module list, loaded/unloaded
    // modules, enabled set). Messages that change none of that (Log, ...) leave it alone.
    std::uint64_t revision() const noexcept { return revision_; }

    // Compares two module paths the way Windows would for display purposes: '/' equals '\',
    // a leading "\\?\" is ignored and ASCII letters compare case-insensitively.
    static bool same_path(std::string_view a, std::string_view b);

private:
    const ipc::ModuleEntry* find_module(std::string_view path) const;
    bool is_enabled(std::string_view path) const;

    SessionOutcome handle_hello(const ipc::Hello& hello);
    ipc::Reply handle_enable(const ipc::EnableModule& request);
    ipc::Reply handle_disable(const ipc::DisableModule& request);
    void handle_module_events(const ipc::ModuleEvents& events, const std::function<void()>& after_step, SessionOutcome& outcome);
    void apply_loaded(const ipc::ModuleEntry& module);
    void apply_unloaded(const ipc::ModuleEntry& module);
    SessionOutcome violation(std::uint64_t request_id, std::string message);

    std::uint64_t agent_id_;
    std::string broker_version_;
    std::uint64_t revision_ = 0;
    bool handshaken_ = false;
    std::uint32_t pid_ = 0;
    ipc::AgentMode mode_ = ipc::AgentMode::Immediate;
    AnswerSink answer_sink_;
    ExceptionSink exception_sink_;
    std::uint64_t ignored_answers_ = 0;
    std::string agent_path_;
    std::string command_line_;
    std::string restart_token_;
    std::uint32_t agent_abi_version_ = 0;
    std::uint64_t module_events_applied_ = 0;
    std::uint64_t module_event_resyncs_ = 0;
    std::vector<ipc::ModuleEntry> modules_;
    std::vector<std::string> enabled_;  // full paths of enabled modules
};

}  // namespace cml::core
