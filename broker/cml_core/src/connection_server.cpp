#include "cml/core/connection_server.h"

#if defined(_WIN32)

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "cml/core/control_server.h"
#include "cml/core/process_registry.h"
#include "cml/core/session.h"
#include "cml/core/version.h"
#include "cml/proc/diag_ids.h"
#include "cml/ipc/pipe.h"
#include "cml/util/log.h"

namespace cml::core {

namespace {

constexpr std::uint32_t kAcceptPollMs = 100;  // how often run() re-checks stop and idle timers
constexpr std::uint32_t kSendTimeoutMs = 5000;
constexpr int kMaxConsecutiveAcceptFailures = 50;
constexpr DWORD kAcceptRetryPauseMs = 100;

struct Connection {
    ipc::PipeConnection pipe;
    std::thread thread;
    std::atomic<bool> finished{false};
};

// State shared between run() and the connection threads.
struct Shared {
    ProcessRegistry registry;  // what the control pipe's `status` reports
    std::atomic<int> active{0};
    std::atomic<unsigned long long> last_idle_tick{0};  // GetTickCount64 when active last hit 0
};

void serve_connection(Connection& connection, std::uint64_t agent_id, Shared& shared,
                      const std::atomic<bool>& stop_requested) {
    Session session(agent_id, version_string());
    bool running = true;
    while (running && !stop_requested.load(std::memory_order_acquire)) {
        Result<ipc::PipeConnection::Received> received = connection.pipe.receive(ipc::kInfiniteTimeout);
        if (!received) {
            CML_LOG(Warn, "agent#%llu: %s", static_cast<unsigned long long>(agent_id),
                    received.error().format().c_str());
            break;
        }
        switch (received->status) {
            case ipc::IoStatus::Ok: {
                SessionOutcome outcome = session.on_message(received->message);
                shared.registry.update(session);
                for (const ipc::Message& reply : outcome.replies) {
                    CML_LOG(Debug, "agent#%llu -> %s", static_cast<unsigned long long>(agent_id),
                            ipc::describe(reply).c_str());
                    Result<ipc::IoStatus> sent = connection.pipe.send(reply, kSendTimeoutMs);
                    if (!sent || *sent != ipc::IoStatus::Ok) {
                        if (!sent) {
                            CML_LOG(Warn, "agent#%llu: %s", static_cast<unsigned long long>(agent_id),
                                    sent.error().format().c_str());
                        }
                        running = false;
                        break;
                    }
                }
                if (outcome.close) {
                    running = false;
                }
                break;
            }
            case ipc::IoStatus::Rejected: {
                // Framing is intact, so the connection survives one bad message. But a request
                // that is silently skipped would leave its sender waiting forever: answer it with
                // the error when its id can be read, and give up on the connection when it cannot.
                const ipc::PipeConnection::Received& bad = *received;
                CML_LOG(Warn, "agent#%llu: rejected a %s message: %s", static_cast<unsigned long long>(agent_id),
                        ipc::to_string(static_cast<ipc::MessageType>(bad.raw_type)), bad.error.format().c_str());
                if (bad.is_request) {
                    if (!bad.request_id) {
                        CML_LOG(Warn, "agent#%llu: the request id is unreadable; closing the connection",
                                static_cast<unsigned long long>(agent_id));
                        running = false;
                        break;
                    }
                    ipc::Reply error_reply;
                    error_reply.request_id = *bad.request_id;
                    error_reply.status = bad.error.code;
                    error_reply.message = ipc::sanitize_utf8(bad.error.message + " " + bad.error.what_to_do, 1000);
                    Result<ipc::IoStatus> sent = connection.pipe.send(error_reply, kSendTimeoutMs);
                    if (!sent || *sent != ipc::IoStatus::Ok) {
                        running = false;
                    }
                }
                break;
            }
            case ipc::IoStatus::Timeout:
            case ipc::IoStatus::Woken:
                break;
            case ipc::IoStatus::Closed:
            case ipc::IoStatus::Cancelled:
                running = false;
                break;
        }
    }
    shared.registry.remove(agent_id);
    CML_LOG(Info, "agent#%llu disconnected (pid %u, %llu modules enabled)",
            static_cast<unsigned long long>(agent_id), session.pid(),
            static_cast<unsigned long long>(session.enabled_modules().size()));
    // The pipe handle is closed when run() reaps this connection (a few poll intervals at most);
    // closing it here would race with run() cancelling it.
    // Record the time before dropping the count so that run() never sees a zero count with a
    // stale timestamp.
    shared.last_idle_tick.store(GetTickCount64(), std::memory_order_release);
    shared.active.fetch_sub(1, std::memory_order_acq_rel);
    connection.finished.store(true, std::memory_order_release);
}

void reap_finished(std::vector<std::unique_ptr<Connection>>& connections) {
    for (auto it = connections.begin(); it != connections.end();) {
        if ((*it)->finished.load(std::memory_order_acquire)) {
            (*it)->thread.join();
            it = connections.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace

ConnectionServer::ConnectionServer(ServerOptions options) : options_(std::move(options)) {
    if (options_.stop_event != nullptr) {
        stop_event_ = options_.stop_event;
    } else {
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        owns_stop_event_ = stop_event_ != nullptr;
    }
}

ConnectionServer::~ConnectionServer() {
    if (owns_stop_event_) {
        CloseHandle(stop_event_);
    }
}

void ConnectionServer::stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
    if (stop_event_ != nullptr) {
        SetEvent(stop_event_);
    }
}

Result<ServerExit> ConnectionServer::run() {
    if (stop_event_ == nullptr) {
        return Diag::make(ids::kPipeCreateFailed, "cannot create the broker's stop event",
                          "Close other programs to free system resources and retry.");
    }
    Result<ipc::PipeServer> created = ipc::PipeServer::create(options_.pipe_name);
    if (!created) {
        return created.error();
    }
    ipc::PipeServer server = std::move(*created);
    CML_LOG(Info, "listening on %s", options_.pipe_name.c_str());

    Shared shared;
    std::unique_ptr<ControlServer> control;
    if (!options_.control_pipe_name.empty()) {
        ControlContext context;
        context.version = version_string();
        context.registry = &shared.registry;
        control = std::make_unique<ControlServer>(options_.control_pipe_name, std::move(context));
        Result<void> started = control->start();
        if (!started) {
            return started.error();
        }
    }
    std::vector<std::unique_ptr<Connection>> connections;
    std::uint64_t next_agent_id = 1;
    bool ever_connected = false;
    int consecutive_accept_failures = 0;
    const ULONGLONG started = GetTickCount64();
    ServerExit exit_reason = ServerExit::Stopped;
    std::optional<Diag> failure;

    const auto stop_signalled = [this] {
        return stop_requested_.load(std::memory_order_acquire) ||
               WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0;
    };
    while (!stop_signalled()) {
        // stop() signals stop_event_, which also ends a wait for a client at once.
        Result<ipc::PipeServer::Accepted> accepted = server.accept(kAcceptPollMs, stop_event_);
        if (!accepted) {
            // One failed accept (resource shortage, a client that misbehaved) must not take the
            // broker down; only a long unbroken run of failures means the pipe is really broken.
            // Pause (interruptibly), then fall through to the housekeeping below so that stop
            // requests, finished connections and the idle timers are still looked after.
            CML_LOG(Error, "%s", accepted.error().format().c_str());
            if (++consecutive_accept_failures >= kMaxConsecutiveAcceptFailures) {
                failure = accepted.error();
                break;
            }
            WaitForSingleObject(stop_event_, kAcceptRetryPauseMs);
        } else if (accepted->status == ipc::IoStatus::Ok) {
            consecutive_accept_failures = 0;
            const std::uint64_t agent_id = next_agent_id++;
            ever_connected = true;
            shared.active.fetch_add(1, std::memory_order_acq_rel);
            auto connection = std::make_unique<Connection>();
            connection->pipe = std::move(accepted->connection);
            CML_LOG(Info, "agent#%llu connected (pipe client pid %u)",
                    static_cast<unsigned long long>(agent_id), connection->pipe.peer_process_id());
            Connection* raw = connection.get();
            raw->thread = std::thread(serve_connection, std::ref(*raw), agent_id, std::ref(shared),
                                      std::cref(stop_requested_));
            connections.push_back(std::move(connection));
        } else {
            consecutive_accept_failures = 0;
        }
        reap_finished(connections);

        const ULONGLONG now = GetTickCount64();
        if (options_.exit_when_idle_ms != 0 && ever_connected &&
            shared.active.load(std::memory_order_acquire) == 0 && !server.has_pending_connection() &&
            now - shared.last_idle_tick.load(std::memory_order_acquire) >= options_.exit_when_idle_ms) {
            CML_LOG(Info, "no agents for %u ms; exiting", options_.exit_when_idle_ms);
            exit_reason = ServerExit::Idle;
            break;
        }
        if (options_.startup_timeout_ms != 0 && !ever_connected &&
            now - started >= options_.startup_timeout_ms) {
            CML_LOG(Warn, "no agent connected within %u ms; exiting", options_.startup_timeout_ms);
            exit_reason = ServerExit::StartupTimeout;
            break;
        }
    }

    stop_requested_.store(true, std::memory_order_release);  // the connection threads look at this too
    server.cancel();
    for (auto& connection : connections) {
        connection->pipe.cancel();
    }
    for (auto& connection : connections) {
        connection->thread.join();
    }
    if (control) {
        control->stop();
    }
    if (failure) {
        return *failure;
    }
    return exit_reason;
}

}  // namespace cml::core

#endif  // _WIN32
