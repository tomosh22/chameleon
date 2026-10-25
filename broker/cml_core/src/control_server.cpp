#include "cml/core/control_server.h"

#include <algorithm>
#include <utility>

#include "cml/proc/diag_ids.h"
#include "cml/util/log.h"

namespace cml::core {

std::vector<std::string> broker_capabilities() {
    // WP-12 adds "reload", "wait", "set-option", "subscribe", "diag-events" here as it implements them.
    return {ipc::capability::kStatus};
}

namespace {

ipc::json::Value status_result(const ControlContext& context) {
    ipc::StatusReport report;
    report.version = context.version;
    report.capabilities = broker_capabilities();
    bool truncated = false;
    if (context.registry != nullptr) {
        for (const RegisteredProcess& process : context.registry->snapshot()) {
            ipc::StatusProcess out;
            out.pid = process.pid;
            // Enabled modules first so that the interesting ones survive the cap.
            for (const bool want_enabled : {true, false}) {
                for (const RegisteredModule& module : process.modules) {
                    if (module.enabled != want_enabled) {
                        continue;
                    }
                    if (out.modules.size() >= kMaxStatusModulesPerProcess) {
                        truncated = true;
                        break;
                    }
                    out.modules.push_back(ipc::StatusModule{module.path, module.enabled});
                }
            }
            report.processes.push_back(std::move(out));
        }
    }
    ipc::json::Value result = ipc::status_to_json(report);
    if (truncated) {
        result.set("modules_truncated", ipc::json::Value::boolean(true));
    }
    return result;
}

}  // namespace

ipc::ControlResponse handle_control_request(const ipc::ControlRequest& request, const ControlContext& context) {
    if (request.cmd == "status") {
        return ipc::make_ok_response(request.id, status_result(context));
    }
    std::string provided;
    for (const std::string& capability : broker_capabilities()) {
        provided += (provided.empty() ? "" : ", ") + capability;
    }
    return ipc::make_error_response(
        request.id,
        Diag::make(ids::kControlUnknownCommand,
                   "control command '" + request.cmd + "' is not provided by this broker (it provides: " + provided + ")",
                   "Query `status` for the broker's capabilities and only send commands it lists; reload and the other "
                   "commands arrive with the full broker (WP-12)."));
}

std::string handle_control_line(std::string_view line, const ControlContext& context) {
    ipc::ControlResponse response;
    Result<ipc::ControlRequest> request = ipc::decode_request(line);
    if (!request) {
        response = ipc::make_error_response(ipc::peek_control_request_id(line), request.error());
    } else {
        CML_LOG(Debug, "control request %lld: %s", static_cast<long long>(request->id), request->cmd.c_str());
        response = handle_control_request(*request, context);
    }
    Result<std::string> encoded = ipc::encode_response(response);
    if (!encoded) {
        // Only possible when the answer is above 1 MiB.
        encoded = ipc::encode_response(ipc::make_error_response(response.id, encoded.error()));
        if (!encoded) {
            return "{\"id\":null,\"ok\":false,\"diag\":{\"id\":\"CML5403\",\"message\":\"response too large\",\"action\":\"\"}}";
        }
    }
    return *encoded;
}

}  // namespace cml::core

#if defined(_WIN32)

#include <windows.h>

#include <memory>

namespace cml::core {

namespace {
constexpr std::uint32_t kAcceptPollMs = 100;
constexpr std::uint32_t kSendTimeoutMs = 5000;
}  // namespace

struct ControlServer::Connection {
    ipc::ControlChannel channel;
    std::thread thread;
    std::atomic<bool> finished{false};
};

ControlServer::ControlServer(std::string name, ControlContext context_in)
    : pipe_name_(std::move(name)), context_(std::move(context_in)) {}

ControlServer::~ControlServer() {
    stop();
}

Result<void> ControlServer::start() {
    Result<ipc::PipeServer> created = ipc::PipeServer::create(pipe_name_);
    if (!created) {
        return created.error();
    }
    pipe_ = std::move(*created);
    started_ = true;
    accept_thread_ = std::thread([this] { accept_loop(); });
    CML_LOG(Info, "control pipe listening on %s", pipe_name_.c_str());
    return Result<void>();
}

void ControlServer::stop() noexcept {
    if (!started_) {
        return;
    }
    stopping_.store(true, std::memory_order_release);
    pipe_.cancel();
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    for (auto& connection : connections_) {
        connection->channel.cancel();
    }
    for (auto& connection : connections_) {
        if (connection->thread.joinable()) {
            connection->thread.join();
        }
    }
    connections_.clear();
    started_ = false;
}

void ControlServer::accept_loop() {
    int consecutive_failures = 0;
    while (!stopping_.load(std::memory_order_acquire)) {
        Result<ipc::PipeServer::Accepted> accepted = pipe_.accept(kAcceptPollMs);
        if (!accepted) {
            CML_LOG(Error, "control pipe: %s", accepted.error().format().c_str());
            if (++consecutive_failures >= 50) {
                CML_LOG(Error, "control pipe: giving up after repeated accept failures");
                return;
            }
            Sleep(100);
        } else if (accepted->status == ipc::IoStatus::Ok) {
            consecutive_failures = 0;
            auto connection = std::make_unique<Connection>();
            connection->channel = ipc::ControlChannel(std::move(accepted->connection));
            CML_LOG(Info, "control client connected (pid %u)", connection->channel.peer_process_id());
            Connection* raw = connection.get();
            raw->thread = std::thread([this, raw] { serve(*raw); });
            connections_.push_back(std::move(connection));
        } else if (accepted->status == ipc::IoStatus::Cancelled) {
            return;
        }
        // Reap finished connections.
        for (auto it = connections_.begin(); it != connections_.end();) {
            if ((*it)->finished.load(std::memory_order_acquire)) {
                (*it)->thread.join();
                it = connections_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void ControlServer::serve(Connection& connection) {
    while (!stopping_.load(std::memory_order_acquire)) {
        Result<ipc::ControlChannel::Line> received = connection.channel.receive_line(ipc::kInfiniteTimeout);
        if (!received) {
            // Over-long line or I/O failure: tell the client if we still can, then drop the connection
            // (the stream can no longer be resynchronised).
            CML_LOG(Warn, "control connection: %s", received.error().format().c_str());
            Result<std::string> reply = ipc::encode_response(ipc::make_error_response(std::nullopt, received.error()));
            if (reply) {
                (void)connection.channel.send_line(*reply, kSendTimeoutMs);
            }
            break;
        }
        if (received->status != ipc::IoStatus::Ok) {
            break;  // closed or cancelled
        }
        if (received->text.empty()) {
            continue;
        }
        const std::string answer = handle_control_line(received->text, context_);
        Result<ipc::IoStatus> sent = connection.channel.send_line(answer, kSendTimeoutMs);
        if (!sent || *sent != ipc::IoStatus::Ok) {
            break;
        }
    }
    connection.finished.store(true, std::memory_order_release);
}

}  // namespace cml::core

#endif  // _WIN32
