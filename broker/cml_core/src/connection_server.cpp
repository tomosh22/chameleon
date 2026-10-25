#include "cml/core/connection_server.h"

#if defined(_WIN32)

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "cml/core/session.h"
#include "cml/core/version.h"
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
            case ipc::IoStatus::Rejected:
                // One bad message (invalid UTF-8, unsupported version or type) is logged and
                // skipped; framing is intact, so the connection and the session carry on.
                CML_LOG(Warn, "agent#%llu: dropped one message: %s", static_cast<unsigned long long>(agent_id),
                        received->error.format().c_str());
                break;
            case ipc::IoStatus::Timeout:
            case ipc::IoStatus::Woken:
                break;
            case ipc::IoStatus::Closed:
            case ipc::IoStatus::Cancelled:
                running = false;
                break;
        }
    }
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

ConnectionServer::ConnectionServer(ServerOptions options) : options_(std::move(options)) {}

Result<ServerExit> ConnectionServer::run() {
    Result<ipc::PipeServer> created = ipc::PipeServer::create(options_.pipe_name);
    if (!created) {
        return created.error();
    }
    ipc::PipeServer server = std::move(*created);
    CML_LOG(Info, "listening on %s", options_.pipe_name.c_str());

    Shared shared;
    std::vector<std::unique_ptr<Connection>> connections;
    std::uint64_t next_agent_id = 1;
    bool ever_connected = false;
    int consecutive_accept_failures = 0;
    const ULONGLONG started = GetTickCount64();
    ServerExit exit_reason = ServerExit::Stopped;
    std::optional<Diag> failure;

    while (!stop_requested_.load(std::memory_order_acquire)) {
        Result<ipc::PipeServer::Accepted> accepted = server.accept(kAcceptPollMs);
        if (!accepted) {
            // One failed accept (resource shortage, a client that misbehaved) must not take the
            // broker down; only a long unbroken run of failures means the pipe is really broken.
            CML_LOG(Error, "%s", accepted.error().format().c_str());
            if (++consecutive_accept_failures >= kMaxConsecutiveAcceptFailures) {
                failure = accepted.error();
                break;
            }
            Sleep(kAcceptRetryPauseMs);
            continue;
        }
        consecutive_accept_failures = 0;
        if (accepted->status == ipc::IoStatus::Ok) {
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
        }
        reap_finished(connections);

        const ULONGLONG now = GetTickCount64();
        if (options_.exit_when_idle_ms != 0 && ever_connected &&
            shared.active.load(std::memory_order_acquire) == 0 &&
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

    server.cancel();
    for (auto& connection : connections) {
        connection->pipe.cancel();
    }
    for (auto& connection : connections) {
        connection->thread.join();
    }
    if (failure) {
        return *failure;
    }
    return exit_reason;
}

}  // namespace cml::core

#endif  // _WIN32
