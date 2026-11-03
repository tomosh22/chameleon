#include "cml/core/agent_link.h"

#include <chrono>
#include <type_traits>
#include <utility>

#include "cml/core/pipeline_diag_ids.h"
#include "cml/util/log.h"

namespace cml::core {

std::optional<std::uint64_t> request_id_of(const ipc::Message& message) {
    return std::visit(
        [](const auto& m) -> std::optional<std::uint64_t> {
            if constexpr (requires { m.request_id; }) {
                return m.request_id;
            } else {
                return std::nullopt;
            }
        },
        message);
}

bool set_request_id(ipc::Message& message, std::uint64_t id) {
    return std::visit(
        [id](auto& m) -> bool {
            if constexpr (requires { m.request_id; }) {
                m.request_id = id;
                return true;
            } else {
                return false;
            }
        },
        message);
}

AgentLink::AgentLink(std::uint64_t agent_id, SendFn send) : agent_id_(agent_id), send_(std::move(send)) {
    snapshot_.agent_id = agent_id;
}

void AgentLink::publish(const Session& session) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.revision == session.revision() && snapshot_.handshaken == session.handshaken()) {
        return;
    }
    snapshot_.handshaken = session.handshaken();
    snapshot_.revision = session.revision();
    snapshot_.pid = session.pid();
    snapshot_.mode = session.mode();
    snapshot_.command_line = session.command_line();
    snapshot_.enabled = session.enabled_modules();
    snapshot_.modules.clear();
    snapshot_.modules.reserve(session.modules().size());
    for (const ipc::ModuleEntry& m : session.modules()) {
        snapshot_.modules.push_back(LoadedModule{m.path, m.base, m.size});
    }
}

bool AgentLink::deliver(const ipc::Message& answer) {
    const std::optional<std::uint64_t> id = request_id_of(answer);
    std::lock_guard<std::mutex> lock(mutex_);
    if (id) {
        const auto it = waiters_.find(*id);
        if (it != waiters_.end() && !it->second->answer) {
            it->second->answer = answer;
            it->second->cv.notify_all();
            return true;
        }
    }
    ++unmatched_;
    return false;
}

void AgentLink::close() {
    {
        // Waits for a send in flight, then drops the transport: nobody can use the pipe after this returns.
        std::lock_guard<std::mutex> send_lock(send_mutex_);
        send_ = nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    for (auto& w : waiters_) {
        w.second->closed = true;
        w.second->cv.notify_all();
    }
}

LinkSnapshot AgentLink::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

bool AgentLink::closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

std::uint64_t AgentLink::next_request_id() {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_id_++;
}

std::uint64_t AgentLink::unmatched_answers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return unmatched_;
}

Result<void> AgentLink::send(const ipc::Message& message) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (!send_) {
        return Diag::make(pipeline::diag::kAgentLost, "the agent is no longer connected", "Restart the application, or wait for its agent to reconnect.");
    }
    return send_(message);
}

Result<ipc::Message> AgentLink::request(ipc::Message request, std::uint32_t timeout_ms) {
    const std::optional<std::uint64_t> id = request_id_of(request);
    if (!id) {
        return Diag::make(pipeline::diag::kAgentRequestFailed, std::string(ipc::to_string(ipc::type_of(request))) + " carries no request id",
                          "This is a broker bug; report it.");
    }
    auto waiter = std::make_shared<Waiter>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return Diag::make(pipeline::diag::kAgentLost, "the agent of process is no longer connected",
                              "Restart the application, or wait for its agent to reconnect.");
        }
        waiters_[*id] = waiter;
    }
    const auto forget = [&] {
        std::lock_guard<std::mutex> lock(mutex_);
        waiters_.erase(*id);
    };
    Result<void> sent = send(request);
    if (!sent) {
        forget();
        if (sent.error().code == pipeline::diag::kAgentLost) {
            return sent.error();
        }
        return Diag::make(pipeline::diag::kAgentRequestFailed,
                          std::string("could not send ") + ipc::to_string(ipc::type_of(request)) + " to the agent: " + sent.error().message,
                          "Check that the application is still running.");
    }
    std::unique_lock<std::mutex> lock(mutex_);
    const bool done = waiter->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return waiter->answer.has_value() || waiter->closed; });
    waiters_.erase(*id);
    if (waiter->answer) {
        return std::move(*waiter->answer);
    }
    lock.unlock();
    if (waiter->closed) {
        return Diag::make(pipeline::diag::kAgentLost, std::string("the agent disconnected before it answered ") + ipc::to_string(ipc::type_of(request)),
                          "Restart the application, or wait for its agent to reconnect.");
    }
    (void)done;
    return Diag::make(pipeline::diag::kAgentRequestTimeout,
                      std::string("no answer to ") + ipc::to_string(ipc::type_of(request)) + " within " + std::to_string(timeout_ms) + " ms",
                      "The application may be stopped in a debugger or not reaching its sync point; try again.");
}

}  // namespace cml::core
