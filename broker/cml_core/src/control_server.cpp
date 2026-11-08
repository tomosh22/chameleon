#include "cml/core/control_server.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <set>
#include <utility>

#include "cml/core/pipeline_diag_ids.h"
#include "cml/proc/diag_ids.h"
#include "cml/util/log.h"

namespace cml::core {

std::vector<std::string> broker_capabilities() {
    return {ipc::capability::kStatus};
}

std::vector<std::string> broker_capabilities(const ControlContext& context) {
    std::vector<std::string> caps = broker_capabilities();
    if (context.service != nullptr) {
        for (const char* c : {ipc::capability::kReload, ipc::capability::kWait, ipc::capability::kSetOption, ipc::capability::kSubscribe,
                              ipc::capability::kDiagEvents}) {
            caps.emplace_back(c);
        }
        if (context.service->restart_available()) {
            caps.emplace_back(ipc::capability::kRestart);  // WP-21
        }
        caps.emplace_back(ipc::capability::kRevert);  // WP-22
    }
    if (context.exceptions != nullptr) {
        caps.emplace_back(ipc::capability::kExceptions);
        caps.emplace_back(ipc::capability::kReexecute);  // WP-23
    }
    return caps;
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
    report.capabilities = broker_capabilities(context);
    if (context.service != nullptr) {
        report.reload_running = context.service->reload_running();
        report.generation = context.service->generation();
    }
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
                    ipc::StatusModule entry;
                    entry.path = module.path;
                    entry.enabled = module.enabled;
                    if (module.enabled && context.service != nullptr) {
                        if (const std::optional<IndexStatus> index = context.service->index_status(process.agent_id, module.path)) {
                            entry.index_state = to_string(index->state);
                            entry.index_percent = index->percent;
                            entry.index_message = index->state == IndexState::Failed && index->error ? index->error->format() : index->phase;
                        } else {
                            // Enabled, but the service has not created the index (yet, or the module just went away):
                            // never "ready".
                            entry.index_state = "pending";
                        }
                    }
                    out.modules.push_back(std::move(entry));
                }
            }
            report.processes.push_back(std::move(out));
        }
    }
    return ipc::status_to_json(report);
}

}  // namespace

namespace {

ipc::ControlResponse answer(const ipc::ControlRequest& request, Result<ipc::json::Value> result) {
    if (!result) {
        return ipc::make_error_response(request.id, result.error());
    }
    return ipc::make_ok_response(request.id, std::move(*result));
}

ipc::ControlResponse subscribe_response(const ipc::ControlRequest& request, const ControlContext& context) {
    static const char* const kEvents[] = {"diag", "patch_applied", "patch_reverted", "progress", "log", "exception", "restarted", "exception_updated"};
    std::set<std::string> names;
    const ipc::json::Value* events = request.args.find("events");
    // An empty array is the same as no argument: the default set. (The bus reads an empty set as "every event", which would switch
    // on `exception` without the client naming it.)
    if (events == nullptr || events->is_null() || (events->is_array() && events->items().empty())) {
        // `progress` (WP-15) is opt-in: it can be chatty on a big reload. `exception` (WP-20) is opt-in because listening for it
        // makes the agents hold faulting threads until somebody decides.
        for (const char* k : kEvents) {
            if (std::string(k) != "progress" && std::string(k) != "exception" && std::string(k) != "exception_updated" && std::string(k) != "restarted") names.insert(k);
        }
    } else if (!events->is_array()) {
        return ipc::make_error_response(request.id, Diag::make(pipeline::diag::kBadRequest, "argument 'events' must be an array of event names", "Use [\"diag\", \"patch_applied\", \"patch_reverted\", \"progress\", \"log\", \"exception\", \"restarted\"] or a subset."));
    } else {
        for (const ipc::json::Value& e : events->items()) {
            bool known = false;
            for (const char* k : kEvents) {
                known = known || (e.is_string() && e.as_string() == k);
            }
            if (!known) {
                return ipc::make_error_response(request.id, Diag::make(pipeline::diag::kBadRequest, "unknown event name in 'events'", "Known events: diag, patch_applied, patch_reverted, progress, log, exception, restarted."));
            }
            names.insert(e.as_string());
        }
    }
    if (context.subscribe) {
        context.subscribe(names);
    }
    ipc::json::Value result = ipc::json::Value::object();
    ipc::json::Value list = ipc::json::Value::array();
    for (const std::string& n : names) {
        list.push(ipc::json::Value::string(n));
    }
    result.set("events", std::move(list));
    return ipc::make_ok_response(request.id, std::move(result));
}

}  // namespace

ipc::ControlResponse handle_control_request(const ipc::ControlRequest& request, const ControlContext& context) {
    if (request.cmd == "status") {
        return ipc::make_ok_response(request.id, status_result(context));
    }
    if (context.service != nullptr) {
        if (request.cmd == "reload") {
            return answer(request, context.service->reload(request.args));
        }
        if (request.cmd == "revert") {
            return answer(request, context.service->revert(request.args));
        }
        if (request.cmd == "wait") {
            return answer(request, context.service->wait(request.args));
        }
        if (request.cmd == "set-option") {
            return answer(request, context.service->set_option(request.args));
        }
        if (request.cmd == "subscribe") {
            return subscribe_response(request, context);
        }
        if (request.cmd == "restart" && context.service->restart_available()) {
            return answer(request, context.service->restart(request.args));  // WP-21
        }
    }
    if (context.exceptions != nullptr) {
        if (request.cmd == "exception-action") {
            return answer(request, context.exceptions->action(request.args));
        }
        if (request.cmd == "exceptions") {
            return answer(request, context.exceptions->pending(request.args));
        }
    }
    std::string provided;
    for (const std::string& capability : broker_capabilities(context)) {
        provided += (provided.empty() ? "" : ", ") + capability;
    }
    return ipc::make_error_response(
        request.id,
        Diag::make(ids::kControlUnknownCommand,
                   "control command '" + request.cmd + "' is not provided by this broker (it provides: " + provided + ")",
                   "Query `status` for the broker's capabilities and only send commands it lists."));
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

#include <condition_variable>
#include <deque>
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

namespace {

// The one writer of a control connection: responses and events share the write lock, so lines never interleave.
//
// Events must never break the response path and must never block the pipeline:
//   * on_event() (called on whichever thread publishes, e.g. the reload pipeline) only appends to a bounded queue
//     (kMaxQueued; further events are dropped and counted) and returns at once;
//   * a writer thread sends the queued lines with the full response timeout (kWriteTimeoutMs). A client that stops
//     reading therefore delays only its own events; once a single write times out the connection is unusable
//     (a partial line may be on the pipe), so the writer marks it broken: no more events and no more responses,
//     and the connection thread ends. A stalled subscriber thus costs the pipeline nothing and is dropped after at
//     most kWriteTimeoutMs.
class ConnectionSender final : public EventListener {
public:
    explicit ConnectionSender(ipc::ControlChannel* channel) : channel_(channel) {}
    ~ConnectionSender() override { detach(); }

    Result<ipc::IoStatus> send_line(const std::string& line, std::uint32_t timeout_ms) {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (channel_ == nullptr) {
            return ipc::IoStatus::Closed;
        }
        Result<ipc::IoStatus> sent = channel_->send_line(line, timeout_ms);
        if (!sent || *sent != ipc::IoStatus::Ok) {
            channel_ = nullptr;  // a timed-out or failed write may have left a partial line: the stream is finished
        }
        return sent;
    }

    void on_event(const Event& event) override {
        ipc::ControlEvent out;
        out.name = event.name;
        out.body = event.body;
        Result<std::string> line = ipc::encode_event(out);
        if (!line) {
            return;  // too large for one control line: dropped
        }
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (stopping_) {
            return;
        }
        if (queue_.size() >= kMaxQueued) {
            ++dropped_;
            return;
        }
        queue_.push_back(std::move(*line));
        if (!writer_.joinable()) {
            writer_ = std::thread([this] { run(); });
        }
        cv_.notify_one();
    }

    void detach() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stopping_ = true;
            cv_.notify_all();
        }
        if (writer_.joinable()) {
            writer_.join();
        }
        std::lock_guard<std::mutex> lock(write_mutex_);
        channel_ = nullptr;
    }

private:
    static constexpr std::size_t kMaxQueued = 512;
    static constexpr std::uint32_t kWriteTimeoutMs = 5000;

    void run() {
        for (;;) {
            std::string line;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_) {
                    return;
                }
                line = std::move(queue_.front());
                queue_.pop_front();
            }
            const Result<ipc::IoStatus> sent = send_line(line, kWriteTimeoutMs);
            if (!sent || *sent != ipc::IoStatus::Ok) {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                stopping_ = true;  // broken connection: nothing more to send
                queue_.clear();
                return;
            }
        }
    }

    std::mutex write_mutex_;
    ipc::ControlChannel* channel_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    bool stopping_ = false;
    std::size_t dropped_ = 0;
    std::thread writer_;
};

}  // namespace

void ControlServer::serve(Connection& connection) {
    auto sender = std::make_shared<ConnectionSender>(&connection.channel);
    EventBus* bus = context_.service != nullptr ? context_.service->events() : nullptr;
    EventBus::Id subscription = 0;
    ControlContext local = context_;  // per connection: `subscribe` belongs to this connection
    local.subscribe = [&](const std::set<std::string>& requested) {
        if (bus == nullptr) {
            return;
        }
        // WP-23: a client that listens for `exception` also gets `exception_updated` (what is offered changed after a patch).
        std::set<std::string> names = requested;
        if (names.count("exception") != 0) {
            names.insert("exception_updated");
        }
        if (subscription == 0) {
            subscription = bus->subscribe(sender, names);
        } else {
            bus->update(subscription, names);
        }
        if (context_.exceptions != nullptr) {
            context_.exceptions->refresh(true);  // WP-20: the agents hold faulting threads only while somebody listens for `exception`
        }
    };
    while (!stopping_.load(std::memory_order_acquire)) {
        Result<ipc::ControlChannel::Line> received = connection.channel.receive_line(ipc::kInfiniteTimeout);
        if (!received) {
            // Over-long line or I/O failure: tell the client if we still can, then drop the connection
            // (the stream can no longer be resynchronised).
            CML_LOG(Warn, "control connection: %s", received.error().format().c_str());
            Result<std::string> reply = ipc::encode_response(ipc::make_error_response(std::nullopt, received.error()));
            if (reply) {
                (void)sender->send_line(*reply, kSendTimeoutMs);
            }
            break;
        }
        if (received->status != ipc::IoStatus::Ok) {
            break;  // closed or cancelled
        }
        if (received->text.empty()) {
            continue;
        }
        const std::string answer = handle_control_line(received->text, local);
        Result<ipc::IoStatus> sent = sender->send_line(answer, kSendTimeoutMs);
        if (!sent || *sent != ipc::IoStatus::Ok) {
            break;
        }
    }
    if (bus != nullptr && subscription != 0) {
        bus->unsubscribe(subscription);
        if (context_.exceptions != nullptr) {
            context_.exceptions->refresh(false);  // the last listener may be gone: release what the agents hold
        }
    }
    sender->detach();
    connection.finished.store(true, std::memory_order_release);
}

}  // namespace cml::core

#endif  // _WIN32
