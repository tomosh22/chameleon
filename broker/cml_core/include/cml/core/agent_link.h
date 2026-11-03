#pragma once

// The broker's handle on one connected agent (WP-12): thread-safe request/answer exchange and a snapshot of
// what the agent reported.
//
// The connection thread owns the Session and the receive side of the pipe. The reload pipeline runs on another
// thread (the control connection's) and needs to send requests (ReserveRange, LoadPatch, ApplyFixups,
// RunInitializers, InstallHooks, ReleaseRange) and wait for their answers (ReserveRangeResult, PatchLoaded,
// Reply, HooksInstalled). AgentLink is the meeting point:
//   * send() serialises every write on the pipe (the connection thread's own replies included);
//   * request() registers a waiter under the request id, sends, and waits up to a timeout;
//   * deliver(), called by the connection thread through the Session's answer sink, completes the waiter; an
//     answer nobody waits for (the request timed out, or the id is unknown) returns false and is ignored;
//   * close() ends all waits with CML5317 when the connection goes away.
// Portable (the transport is a std::function), so the pipeline is unit-tested with a scripted fake agent.

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cml/core/module_symbols.h"
#include "cml/core/session.h"
#include "cml/ipc/messages.h"
#include "cml/util/result.h"

namespace cml::core {

struct LinkSnapshot {
    std::uint64_t agent_id = 0;
    std::uint32_t pid = 0;
    ipc::AgentMode mode = ipc::AgentMode::Immediate;
    std::string command_line;
    std::string restart_token;             // WP-21: CML_RESTART_TOKEN of the process
    std::vector<LoadedModule> modules;     // everything the agent reported, in order
    std::vector<std::string> enabled;      // full paths of the enabled modules
    std::uint64_t revision = 0;            // Session::revision()
    bool handshaken = false;
};

// Request id of a request or answer message (the types that carry one first); nullopt for events.
std::optional<std::uint64_t> request_id_of(const ipc::Message& message);
// Sets the request id of a request message built by the caller; false for types without one.
bool set_request_id(ipc::Message& message, std::uint64_t id);

class AgentLink {
public:
    using SendFn = std::function<Result<void>(const ipc::Message&)>;

    AgentLink(std::uint64_t agent_id, SendFn send);

    std::uint64_t agent_id() const noexcept { return agent_id_; }

    // ---- connection thread ----
    // Copies what the Session knows (call after every message; cheap when nothing changed).
    void publish(const Session& session);
    // The Session's answer sink: completes the waiter for the answer's request id. False when nobody waits.
    bool deliver(const ipc::Message& answer);
    // The connection ended: fails every pending and future request with CML5317.
    void close();

    // ---- any thread ----
    LinkSnapshot snapshot() const;
    bool closed() const;
    std::uint64_t next_request_id();
    // Writes one message (serialised with every other writer).
    Result<void> send(const ipc::Message& message);
    // Sends `request` (its request id must be set, see next_request_id/set_request_id) and waits for the
    // answer. CML5317 when the connection is closed or closes while waiting, CML5306 on timeout or send failure.
    // A late answer is dropped by deliver().
    Result<ipc::Message> request(ipc::Message request, std::uint32_t timeout_ms);

    // Number of answers that arrived with nobody waiting (tests).
    std::uint64_t unmatched_answers() const;

private:
    struct Waiter {
        std::condition_variable cv;
        std::optional<ipc::Message> answer;
        bool closed = false;
    };

    std::uint64_t agent_id_;
    SendFn send_;
    mutable std::mutex mutex_;
    LinkSnapshot snapshot_;
    std::map<std::uint64_t, std::shared_ptr<Waiter>> waiters_;
    std::uint64_t next_id_ = 1;
    bool closed_ = false;
    std::uint64_t unmatched_ = 0;
    std::mutex send_mutex_;
};

}  // namespace cml::core
