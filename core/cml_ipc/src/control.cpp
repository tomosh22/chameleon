#include "cml/ipc/control.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "cml/proc/diag_ids.h"

#if defined(_WIN32)
#include <windows.h>

#include "cml/proc/proc.h"
#endif

namespace cml::ipc {

namespace {

constexpr std::size_t kMaxCommandLength = 64;

bool valid_command_name(const std::string& cmd) {
    if (cmd.empty() || cmd.size() > kMaxCommandLength) {
        return false;
    }
    return std::all_of(cmd.begin(), cmd.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

Diag request_invalid(const std::string& what) {
    return Diag::make(ids::kControlRequestInvalid, "invalid control request: " + what,
                      "Send {\"id\":N,\"cmd\":\"<name>\",\"args\":{...}} with an integer id and a lowercase command name.");
}

Diag response_invalid(const std::string& what) {
    return Diag::make(ids::kControlResponseInvalid, "invalid control message from the broker: " + what,
                      "Use a cmltest/client and broker from the same build; report this as a bug otherwise.");
}

Diag too_long(const char* what) {
    return Diag::make(ids::kControlLineTooLong, std::string(what) + " exceeds the 1 MiB control message limit",
                      "Send less data per message.");
}

}  // namespace

// --- codec --------------------------------------------------------------------------------------

ControlDiag to_control_diag(const Diag& diag) {
    ControlDiag out;
    out.id = diag.id();
    out.message = diag.message;
    out.action = diag.what_to_do;
    return out;
}

Diag to_diag(const ControlDiag& diag) {
    // "CMLnnnn" -> nnnn
    std::uint32_t code = 0;
    if (diag.id.size() == 7 && diag.id.compare(0, 3, "CML") == 0 &&
        std::all_of(diag.id.begin() + 3, diag.id.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        code = static_cast<std::uint32_t>(std::strtoul(diag.id.c_str() + 3, nullptr, 10));
    }
    Diag candidate = Diag::make(code, diag.message, diag.action);
    if (!candidate.valid()) {
        return Diag::make(ids::kControlResponseInvalid,
                          "the broker reported an error with an unusable id '" + diag.id + "': " + diag.message,
                          diag.action.empty() ? "Report this as a bug." : diag.action);
    }
    return candidate;
}

Result<std::string> encode_request(const ControlRequest& request) {
    json::Value object = json::Value::object();
    object.set("id", json::Value::integer(request.id));
    object.set("cmd", json::Value::string(request.cmd));
    object.set("args", request.args.is_object() ? request.args : json::Value::object());
    std::string line = json::serialize(object);
    if (line.size() > kMaxControlLineBytes) {
        return too_long("the request");
    }
    return line;
}

Result<std::string> encode_response(const ControlResponse& response) {
    json::Value object = json::Value::object();
    object.set("id", response.id ? json::Value::integer(*response.id) : json::Value());
    object.set("ok", json::Value::boolean(response.ok));
    if (response.ok) {
        object.set("result", response.result.is_object() ? response.result : json::Value::object());
    }
    if (response.diag) {
        json::Value diag = json::Value::object();
        diag.set("id", json::Value::string(response.diag->id));
        diag.set("message", json::Value::string(response.diag->message));
        diag.set("action", json::Value::string(response.diag->action));
        object.set("diag", std::move(diag));
    }
    std::string line = json::serialize(object);
    if (line.size() > kMaxControlLineBytes) {
        return too_long("the response");
    }
    return line;
}

Result<std::string> encode_event(const ControlEvent& event) {
    json::Value object = json::Value::object();
    object.set("event", json::Value::string(event.name));
    for (std::size_t i = 0; i < event.body.size(); ++i) {
        if (event.body.is_object() && event.body.key_at(i) != "event") {
            object.set(event.body.key_at(i), event.body.value_at(i));
        }
    }
    std::string line = json::serialize(object);
    if (line.size() > kMaxControlLineBytes) {
        return too_long("the event");
    }
    return line;
}

std::optional<std::int64_t> peek_control_request_id(std::string_view line) {
    Result<json::Value> parsed = json::parse(line);
    if (!parsed || !parsed->is_object()) {
        return std::nullopt;
    }
    const json::Value* id = parsed->find("id");
    if (id == nullptr || !id->is_integer()) {
        return std::nullopt;
    }
    return id->as_integer();
}

Result<ControlRequest> decode_request(std::string_view line) {
    if (line.size() > kMaxControlLineBytes) {
        return too_long("the request");
    }
    Result<json::Value> parsed = json::parse(line);
    if (!parsed) {
        return parsed.error();
    }
    if (!parsed->is_object()) {
        return request_invalid("the message is not a JSON object");
    }
    const json::Value* id = parsed->find("id");
    if (id == nullptr || !id->is_integer()) {
        return request_invalid("\"id\" must be an integer");
    }
    const json::Value* cmd = parsed->find("cmd");
    if (cmd == nullptr || !cmd->is_string()) {
        return request_invalid("\"cmd\" must be a string");
    }
    if (!valid_command_name(cmd->as_string())) {
        return request_invalid("\"cmd\" must be 1-64 characters of a-z, 0-9, '-' or '_'");
    }
    ControlRequest request;
    request.id = id->as_integer();
    request.cmd = cmd->as_string();
    const json::Value* args = parsed->find("args");
    if (args != nullptr) {
        if (!args->is_object()) {
            return request_invalid("\"args\" must be an object");
        }
        request.args = *args;
    }
    return request;
}

Result<std::variant<ControlResponse, ControlEvent>> decode_server_message(std::string_view line) {
    using Out = std::variant<ControlResponse, ControlEvent>;
    if (line.size() > kMaxControlLineBytes) {
        return too_long("the message");
    }
    Result<json::Value> parsed = json::parse(line);
    if (!parsed) {
        return Diag::make(ids::kControlResponseInvalid, "the broker sent malformed JSON: " + parsed.error().message,
                          "Use a client and broker from the same build.");
    }
    if (!parsed->is_object()) {
        return response_invalid("the message is not a JSON object");
    }
    if (const json::Value* event = parsed->find("event")) {
        if (!event->is_string() || event->as_string().empty()) {
            return response_invalid("\"event\" must be a non-empty string");
        }
        ControlEvent out;
        out.name = event->as_string();
        out.body = *parsed;
        return Out(std::move(out));
    }
    ControlResponse response;
    const json::Value* id = parsed->find("id");
    if (id == nullptr) {
        return response_invalid("a response needs \"id\" (an integer or null)");
    }
    if (id->is_integer()) {
        response.id = id->as_integer();
    } else if (!id->is_null()) {
        return response_invalid("\"id\" must be an integer or null");
    }
    const json::Value* ok = parsed->find("ok");
    if (ok == nullptr || !ok->is_bool()) {
        return response_invalid("\"ok\" must be a boolean");
    }
    response.ok = ok->as_bool();
    if (const json::Value* result = parsed->find("result")) {
        if (!result->is_object()) {
            return response_invalid("\"result\" must be an object");
        }
        response.result = *result;
    }
    if (const json::Value* diag = parsed->find("diag")) {
        if (!diag->is_object() || diag->find("id") == nullptr || !diag->find("id")->is_string() ||
            diag->find("message") == nullptr || !diag->find("message")->is_string()) {
            return response_invalid("\"diag\" must be an object with string \"id\" and \"message\"");
        }
        ControlDiag d;
        d.id = diag->get_string("id");
        d.message = diag->get_string("message");
        d.action = diag->get_string("action");
        response.diag = std::move(d);
    }
    if (!response.ok && !response.diag) {
        return response_invalid("a failed response needs a \"diag\"");
    }
    return Out(std::move(response));
}

ControlResponse make_ok_response(std::int64_t id, json::Value result) {
    ControlResponse response;
    response.id = id;
    response.ok = true;
    response.result = std::move(result);
    return response;
}

ControlResponse make_error_response(std::optional<std::int64_t> id, const Diag& diag) {
    ControlResponse response;
    response.id = id;
    response.ok = false;
    response.diag = to_control_diag(diag);
    return response;
}

// --- status -------------------------------------------------------------------------------------

bool StatusReport::has_capability(std::string_view name) const {
    return std::find(capabilities.begin(), capabilities.end(), name) != capabilities.end();
}

const StatusProcess* StatusReport::find_process(std::uint32_t pid) const {
    for (const StatusProcess& process : processes) {
        if (process.pid == pid) {
            return &process;
        }
    }
    return nullptr;
}

json::Value status_to_json(const StatusReport& report) {
    json::Value out = json::Value::object();
    out.set("version", json::Value::string(report.version));
    json::Value caps = json::Value::array();
    for (const std::string& c : report.capabilities) {
        caps.push(json::Value::string(c));
    }
    out.set("capabilities", std::move(caps));
    json::Value processes = json::Value::array();
    for (const StatusProcess& p : report.processes) {
        json::Value process = json::Value::object();
        process.set("pid", json::Value::integer(static_cast<std::int64_t>(p.pid)));
        json::Value modules = json::Value::array();
        for (const StatusModule& m : p.modules) {
            json::Value module = json::Value::object();
            module.set("path", json::Value::string(m.path));
            module.set("enabled", json::Value::boolean(m.enabled));
            modules.push(std::move(module));
        }
        process.set("modules", std::move(modules));
        processes.push(std::move(process));
    }
    out.set("processes", std::move(processes));
    return out;
}

Result<StatusReport> status_from_json(const json::Value& result) {
    if (!result.is_object()) {
        return response_invalid("status result is not an object");
    }
    StatusReport report;
    report.version = result.get_string("version");
    const json::Value* caps = result.find("capabilities");
    if (caps == nullptr || !caps->is_array()) {
        return response_invalid("status result needs a \"capabilities\" array");
    }
    for (const json::Value& c : caps->items()) {
        if (!c.is_string()) {
            return response_invalid("\"capabilities\" must contain strings");
        }
        report.capabilities.push_back(c.as_string());
    }
    const json::Value* processes = result.find("processes");
    if (processes == nullptr || !processes->is_array()) {
        return response_invalid("status result needs a \"processes\" array");
    }
    for (const json::Value& p : processes->items()) {
        const json::Value* pid = p.is_object() ? p.find("pid") : nullptr;
        const json::Value* modules = p.is_object() ? p.find("modules") : nullptr;
        if (pid == nullptr || !pid->is_integer() || pid->as_integer() < 0 || pid->as_integer() > 0xFFFFFFFFll ||
            modules == nullptr || !modules->is_array()) {
            return response_invalid("each process needs an integer \"pid\" and a \"modules\" array");
        }
        StatusProcess process;
        process.pid = static_cast<std::uint32_t>(pid->as_integer());
        for (const json::Value& m : modules->items()) {
            const json::Value* path = m.is_object() ? m.find("path") : nullptr;
            const json::Value* enabled = m.is_object() ? m.find("enabled") : nullptr;
            if (path == nullptr || !path->is_string() || enabled == nullptr || !enabled->is_bool()) {
                return response_invalid("each module needs a string \"path\" and a boolean \"enabled\"");
            }
            process.modules.push_back(StatusModule{path->as_string(), enabled->as_bool()});
        }
        report.processes.push_back(std::move(process));
    }
    return report;
}

// --- pipe side (Windows) ------------------------------------------------------------------------

#if defined(_WIN32)

namespace {

bool has_pipe_prefix(const std::string& name) {
    static const char kPrefix[] = "\\\\.\\pipe\\";
    const std::size_t n = sizeof(kPrefix) - 1;
    if (name.size() <= n) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != kPrefix[i]) {
            return false;
        }
    }
    return true;
}

ULONGLONG deadline_after(std::uint32_t timeout_ms) {
    return timeout_ms == kInfiniteTimeout ? 0 : GetTickCount64() + timeout_ms;
}

std::uint32_t remaining(std::uint32_t timeout_ms, ULONGLONG deadline) {
    if (timeout_ms == kInfiniteTimeout) {
        return kInfiniteTimeout;
    }
    const ULONGLONG now = GetTickCount64();
    return now >= deadline ? 0 : static_cast<std::uint32_t>(deadline - now);
}

Diag io_failed(const std::string& what) {
    return Diag::make(ids::kControlIoFailed, "the control pipe failed: " + what,
                      "Check that the broker is still running and reconnect.");
}

}  // namespace

Result<std::string> resolve_control_pipe_name(const std::string& explicit_name) {
    std::string name = explicit_name;
    if (name.empty()) {
        const std::optional<std::string> from_env = proc::get_env("CML_CONTROL_PIPE");
        if (from_env && !from_env->empty()) {
            name = *from_env;
        }
    }
    if (name.empty()) {
        Result<std::string> sid = proc::logon_session_sid();
        if (!sid) {
            return sid.error();
        }
        name = "\\\\.\\pipe\\cml_control_" + *sid;
    }
    if (!has_pipe_prefix(name)) {
        return Diag::make(ids::kPipeNameInvalid,
                          "control pipe name '" + name + "' does not start with \\\\.\\pipe\\ or has no name part",
                          "Set CML_CONTROL_PIPE (or --control-pipe) to a full name such as \\\\.\\pipe\\cml_control_test.");
    }
    return name;
}

Result<IoStatus> ControlChannel::send_line(std::string_view line, std::uint32_t timeout_ms) {
    if (line.size() > kMaxControlLineBytes || line.find('\n') != std::string_view::npos) {
        return Diag::make(ids::kControlLineTooLong,
                          "a control message is over 1 MiB or contains a newline and cannot be sent as one line",
                          "Send less data per message.");
    }
    std::string bytes(line);
    bytes.push_back('\n');
    return pipe_.send_bytes(bytes.data(), bytes.size(), timeout_ms);
}

Result<ControlChannel::Line> ControlChannel::receive_line(std::uint32_t timeout_ms, void* wake_event) {
    const ULONGLONG deadline = deadline_after(timeout_ms);
    for (;;) {
        const std::size_t newline = buffer_.find('\n');
        if (newline != std::string::npos) {
            Line out;
            out.status = IoStatus::Ok;
            out.text = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            if (!out.text.empty() && out.text.back() == '\r') {
                out.text.pop_back();
            }
            return out;
        }
        if (buffer_.size() > kMaxControlLineBytes) {
            buffer_.clear();
            return Diag::make(ids::kControlLineTooLong, "a control message is longer than 1 MiB without a newline",
                              "Send one JSON object per line, each under 1 MiB.");
        }
        Result<IoStatus> got = pipe_.receive_some(buffer_, remaining(timeout_ms, deadline), wake_event);
        if (!got) {
            return got.error();
        }
        if (*got != IoStatus::Ok) {
            Line out;
            out.status = *got;
            return out;
        }
    }
}

Result<ControlClient> ControlClient::connect(const std::string& pipe_name, std::uint32_t timeout_ms) {
    Result<PipeConnection> connection = connect_pipe(pipe_name, timeout_ms);
    if (!connection) {
        const Diag& e = connection.error();
        if (e.code == ids::kBrokerUnreachable) {
            return Diag::make(ids::kControlUnreachable,
                              "cannot connect to the broker control pipe '" + pipe_name + "': " + e.message,
                              "Start cml_broker with --control-pipe, or set CML_CONTROL_PIPE to the pipe it serves.");
        }
        return e;
    }
    ControlClient client;
    client.channel_ = ControlChannel(std::move(*connection));
    return client;
}

Result<ControlResponse> ControlClient::call(const std::string& cmd, json::Value args, std::uint32_t timeout_ms) {
    ControlRequest request;
    request.id = next_id_++;
    request.cmd = cmd;
    request.args = std::move(args);
    Result<std::string> line = encode_request(request);
    if (!line) {
        return line.error();
    }
    const ULONGLONG deadline = deadline_after(timeout_ms);
    Result<IoStatus> sent = channel_.send_line(*line, remaining(timeout_ms, deadline));
    if (!sent) {
        return sent.error();
    }
    if (*sent != IoStatus::Ok) {
        return io_failed(*sent == IoStatus::Timeout ? "timed out sending '" + cmd + "'" : "the broker closed the pipe");
    }
    for (;;) {
        Result<ControlChannel::Line> received = channel_.receive_line(remaining(timeout_ms, deadline));
        if (!received) {
            return received.error();
        }
        if (received->status == IoStatus::Timeout) {
            return Diag::make(ids::kControlTimeout,
                              "no answer to control command '" + cmd + "' within " + std::to_string(timeout_ms) + " ms",
                              "Check that the broker is responsive (see its log).");
        }
        if (received->status != IoStatus::Ok) {
            return io_failed("the broker closed the pipe before answering '" + cmd + "'");
        }
        Result<std::variant<ControlResponse, ControlEvent>> message = decode_server_message(received->text);
        if (!message) {
            return message.error();
        }
        if (std::holds_alternative<ControlEvent>(*message)) {
            constexpr std::size_t kMaxQueuedEvents = 4096;
            if (events_.size() >= kMaxQueuedEvents) {
                events_.pop_front();  // keep the newest
            }
            events_.push_back(std::move(std::get<ControlEvent>(*message)));
            continue;
        }
        ControlResponse& response = std::get<ControlResponse>(*message);
        // A null id means the broker could not even read our request; it is the answer to it.
        if (response.id && *response.id != request.id) {
            continue;  // stale answer of an earlier, timed-out call
        }
        return std::move(response);
    }
}

Result<StatusReport> ControlClient::status(std::uint32_t timeout_ms) {
    Result<ControlResponse> response = call("status", json::Value::object(), timeout_ms);
    if (!response) {
        return response.error();
    }
    if (!response->ok) {
        return to_diag(*response->diag);
    }
    return status_from_json(response->result);
}

Result<std::optional<ControlEvent>> ControlClient::next_event(std::uint32_t timeout_ms) {
    if (!events_.empty()) {
        ControlEvent event = std::move(events_.front());
        events_.pop_front();
        return std::optional<ControlEvent>(std::move(event));
    }
    const ULONGLONG deadline = deadline_after(timeout_ms);
    for (;;) {
        Result<ControlChannel::Line> received = channel_.receive_line(remaining(timeout_ms, deadline));
        if (!received) {
            return received.error();
        }
        if (received->status == IoStatus::Timeout) {
            return std::optional<ControlEvent>();
        }
        if (received->status != IoStatus::Ok) {
            return io_failed("the broker closed the pipe while waiting for an event");
        }
        Result<std::variant<ControlResponse, ControlEvent>> message = decode_server_message(received->text);
        if (!message) {
            return message.error();
        }
        if (std::holds_alternative<ControlEvent>(*message)) {
            return std::optional<ControlEvent>(std::move(std::get<ControlEvent>(*message)));
        }
        // A response nobody waits for (late answer): ignore.
    }
}

#endif  // _WIN32

}  // namespace cml::ipc
