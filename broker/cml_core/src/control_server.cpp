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

std::size_t module_json_size(const RegisteredModule& module) {
    ipc::json::Value value = ipc::json::Value::object();
    value.set("path", ipc::json::Value::string(module.path));
    value.set("enabled", ipc::json::Value::boolean(module.enabled));
    return ipc::json::serialize(value).size() + 1;  // + the comma
}

ipc::json::Value status_result(const ControlContext& context) {
    ipc::StatusReport report;
    report.version = context.version;
    report.capabilities = broker_capabilities();
    report.control_pipe_healthy = context.pipe_healthy == nullptr || context.pipe_healthy->load();
    // Fixed part of the response, with some slack for the per-process framing.
    std::size_t used = 256 + report.version.size();
    if (context.registry != nullptr) {
        constexpr std::size_t kProcessOverhead = 64;
        bool full = false;
        for (const RegisteredProcess& process : context.registry->snapshot()) {
            if (full || used + kProcessOverhead > kStatusByteBudget) {
                report.truncated = true;  // this and the remaining processes do not fit
                break;
            }
            used += kProcessOverhead;
            ipc::StatusProcess out;
            out.pid = process.pid;
            // Enabled modules first so that the interesting ones survive the cut.
            for (const bool want_enabled : {true, false}) {
                for (const RegisteredModule& module : process.modules) {
                    if (module.enabled != want_enabled || out.modules_truncated) {
                        continue;
                    }
                    const std::size_t size = module_json_size(module);
                    if (out.modules.size() >= kMaxStatusModulesPerProcess || used + size > kStatusByteBudget) {
                        out.modules_truncated = true;
                        report.truncated = true;
                        full = used + size > kStatusByteBudget;
                        continue;
                    }
                    used += size;
                    out.modules.push_back(ipc::StatusModule{module.path, module.enabled});
                }
            }
            report.processes.push_back(std::move(out));
        }
    }
    return ipc::status_to_json(report);
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
    context_.pipe_healthy = &healthy_;
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

namespace {
std::atomic<int> g_injected_accept_failures{0};

bool take_injected_failure() {
    int n = g_injected_accept_failures.load();
    while (n > 0) {
        if (g_injected_accept_failures.compare_exchange_weak(n, n - 1)) {
            return true;
        }
    }
    return false;
}
}  // namespace

void ControlServer::set_accept_failures_for_testing(int count) {
    g_injected_accept_failures.store(count);
}

// Never leaves the loop while the broker runs: a failed accept is logged at warn, the listening
// instance is recreated, and accepting is retried with a growing pause (100 ms .. 2 s). `status`
// reports control_pipe_healthy = false until an accept works again.
void ControlServer::accept_loop() {
    constexpr std::uint32_t kMinBackoffMs = 100;
    constexpr std::uint32_t kMaxBackoffMs = 2000;
    std::uint32_t backoff_ms = kMinBackoffMs;
    while (!stopping_.load(std::memory_order_acquire)) {
        Result<ipc::PipeServer::Accepted> accepted =
            take_injected_failure()
                ? Result<ipc::PipeServer::Accepted>(Diag::make(ids::kPipeIoFailed, "accepting a control client failed (injected failure)",
                                                               "Test hook: ControlServer::set_accept_failures_for_testing."))
                : pipe_.accept(kAcceptPollMs);
        if (!accepted) {
            healthy_.store(false);
            CML_LOG(Warn, "control pipe: %s; recreating the pipe instance and retrying in %u ms", accepted.error().format().c_str(), backoff_ms);
            pipe_.refresh_instance();
            for (std::uint32_t waited = 0; waited < backoff_ms && !stopping_.load(std::memory_order_acquire); waited += 20) {
                Sleep(20);
            }
            backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
        } else if (accepted->status == ipc::IoStatus::Ok) {
            if (!healthy_.exchange(true)) {
                CML_LOG(Info, "control pipe: accepting clients again");
            }
            backoff_ms = kMinBackoffMs;
            auto connection = std::make_unique<Connection>();
            connection->channel = ipc::ControlChannel(std::move(accepted->connection));
            CML_LOG(Info, "control client connected (pid %u)", connection->channel.peer_process_id());
            Connection* raw = connection.get();
            raw->thread = std::thread([this, raw] { serve(*raw); });
            connections_.push_back(std::move(connection));
        } else if (accepted->status == ipc::IoStatus::Cancelled) {
            return;
        } else {
            healthy_.store(true);  // a quiet poll timeout: the pipe works
            backoff_ms = kMinBackoffMs;
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
