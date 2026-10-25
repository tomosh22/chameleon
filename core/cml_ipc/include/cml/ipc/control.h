#pragma once

// Control protocol: how tools (cmltest, IDE extensions, scripts) talk to the broker. Specified in
// docs/wp/WP-13.md, section "Control protocol".
//
// Transport: a named pipe (default `\\.\pipe\cml_control_<logon-session SID>`, overridden by
// CML_CONTROL_PIPE or `cml_broker --control-pipe NAME`), same DACL rules as the agent pipe.
// Messages are UTF-8 JSON objects, one per line (newline-delimited):
//
//   request   {"id":N,"cmd":"<name>","args":{...}}
//   response  {"id":N,"ok":true|false,"result":{...},"diag":{"id":"CMLnnnn","message":"...","action":"..."}}
//   event     {"event":"<name>",...}     unsolicited; recognised by its "event" member (other members
//                                        are free, e.g. the diag event carries "id":"CML3101")
//
// The codec half of this header is portable (no Win32) and never throws; the pipe half (channel,
// client, name resolution) is Windows-only.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "cml/ipc/control_ids.h"
#include "cml/ipc/json.h"
#include "cml/util/result.h"

namespace cml::ipc {

inline constexpr std::size_t kMaxControlLineBytes = 1u << 20;  // 1 MiB per message

// Capability names reported by `status`. A broker provides exactly the commands/features it lists.
namespace capability {
inline constexpr const char* kStatus = "status";
inline constexpr const char* kReload = "reload";
inline constexpr const char* kWait = "wait";
inline constexpr const char* kSetOption = "set-option";
inline constexpr const char* kSubscribe = "subscribe";
inline constexpr const char* kDiagEvents = "diag-events";  // `diag` events are delivered to subscribers
}  // namespace capability

struct ControlDiag {
    std::string id;       // "CML5402"
    std::string message;  // one line
    std::string action;   // what to do
};

struct ControlRequest {
    std::int64_t id = 0;
    std::string cmd;
    json::Value args = json::Value::object();
};

struct ControlResponse {
    std::optional<std::int64_t> id;  // null when the request was too broken to read its id
    bool ok = false;
    json::Value result = json::Value::object();
    std::optional<ControlDiag> diag;  // present when !ok
};

struct ControlEvent {
    std::string name;                        // value of "event"
    json::Value body = json::Value::object();  // the whole object, including "event"
};

ControlDiag to_control_diag(const Diag& diag);
// The Diag a ControlDiag stands for. An id that is not CMLnnnn (a peer bug) maps to CML5407.
Diag to_diag(const ControlDiag& diag);

// Encoders return the message WITHOUT the trailing newline. They fail (CML5403) when the message
// would exceed kMaxControlLineBytes.
Result<std::string> encode_request(const ControlRequest& request);
Result<std::string> encode_response(const ControlResponse& response);
Result<std::string> encode_event(const ControlEvent& event);

// Decoders take one line without its newline. Malformed JSON gives CML5400; JSON that is not a
// well-formed message gives CML5401 (requests) or CML5407 (messages a client receives).
Result<ControlRequest> decode_request(std::string_view line);
Result<std::variant<ControlResponse, ControlEvent>> decode_server_message(std::string_view line);

// Best-effort id of a request line that failed to decode (for the error response); nullopt when
// the line is not JSON or has no integer "id".
std::optional<std::int64_t> peek_control_request_id(std::string_view line);

ControlResponse make_ok_response(std::int64_t id, json::Value result);
ControlResponse make_error_response(std::optional<std::int64_t> id, const Diag& diag);

// --- status ---------------------------------------------------------------------------------

struct StatusModule {
    std::string path;
    bool enabled = false;
};

struct StatusProcess {
    std::uint32_t pid = 0;
    std::vector<StatusModule> modules;
};

struct StatusReport {
    std::string version;
    std::vector<std::string> capabilities;
    std::vector<StatusProcess> processes;

    bool has_capability(std::string_view name) const;
    // The process with this pid, or nullptr.
    const StatusProcess* find_process(std::uint32_t pid) const;
};

json::Value status_to_json(const StatusReport& report);
Result<StatusReport> status_from_json(const json::Value& result);

}  // namespace cml::ipc

#if defined(_WIN32)

#include <deque>

#include "cml/ipc/pipe.h"

namespace cml::ipc {

// `explicit_name` (e.g. --control-pipe) if non-empty, else CML_CONTROL_PIPE, else
// `\\.\pipe\cml_control_<logon session SID>`. CML5208 for a name without the pipe prefix.
Result<std::string> resolve_control_pipe_name(const std::string& explicit_name);

// Line-oriented view of a PipeConnection (server and client side). Same threading rules as
// PipeConnection: one reader and one writer at a time; cancel() from any thread.
class ControlChannel {
public:
    ControlChannel() = default;
    explicit ControlChannel(PipeConnection connection) : pipe_(std::move(connection)) {}

    bool is_open() const noexcept { return pipe_.is_open(); }

    // Sends `line` plus '\n'. Ok / Timeout / Closed / Cancelled; a Diag for I/O errors, and CML5403
    // when `line` is over the limit or contains a newline.
    Result<IoStatus> send_line(std::string_view line, std::uint32_t timeout_ms);

    struct Line {
        IoStatus status = IoStatus::Closed;  // Ok, Timeout, Woken, Closed or Cancelled
        std::string text;                    // valid when status == Ok (without '\n' / '\r')
    };
    // Waits for one complete line. CML5403 (and the connection must be closed) when more than
    // kMaxControlLineBytes arrive without a newline.
    Result<Line> receive_line(std::uint32_t timeout_ms, void* wake_event = nullptr);

    void cancel() noexcept { pipe_.cancel(); }
    void close() noexcept { pipe_.close(); }
    std::uint32_t peer_process_id() const noexcept { return pipe_.peer_process_id(); }

private:
    PipeConnection pipe_;
    std::string buffer_;  // bytes received but not yet returned as lines
};

// Client used by cmltest and by tools: connects, sends requests, matches responses by id and
// queues events that arrive in between.
class ControlClient {
public:
    static Result<ControlClient> connect(const std::string& pipe_name, std::uint32_t timeout_ms);

    // Sends `cmd` with `args` and waits for its response. A response with ok=false is NOT an error
    // here (it is returned for the caller to inspect); transport errors, timeouts (CML5408) and
    // unreadable answers (CML5407) are.
    Result<ControlResponse> call(const std::string& cmd, json::Value args, std::uint32_t timeout_ms);

    // `status` command, decoded. A failing response is returned as its Diag.
    Result<StatusReport> status(std::uint32_t timeout_ms);

    // Next queued event, or waits up to timeout_ms for one. nullopt on timeout.
    Result<std::optional<ControlEvent>> next_event(std::uint32_t timeout_ms);

    // Events received so far and not yet consumed by next_event().
    std::size_t queued_events() const noexcept { return events_.size(); }

    void close() noexcept { channel_.close(); }

private:
    ControlChannel channel_;
    std::int64_t next_id_ = 1;
    std::deque<ControlEvent> events_;
};

}  // namespace cml::ipc

#endif  // _WIN32
