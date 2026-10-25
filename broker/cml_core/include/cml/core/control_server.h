#pragma once

// Broker side of the control protocol (cml_ipc/control.h, docs/wp/WP-13.md).
//
// WP-13 scope: the minimal server. It implements `status` (capabilities: ["status"]) and answers
// every other command with a proper CML5402 diagnostic. WP-12 extends handle_control_request()
// with reload / wait / set-option / subscribe and the matching capabilities; the request/response
// plumbing and the pipe server below carry over unchanged.
//
// handle_control_line() is portable and unit-testable without pipes; ControlServer (Windows)
// serves it on a named pipe with a thread per connection.

#include <string>
#include <string_view>
#include <vector>

#include "cml/core/process_registry.h"
#include "cml/ipc/control.h"

namespace cml::core {

struct ControlContext {
    std::string version;                         // broker version string for `status`
    const ProcessRegistry* registry = nullptr;   // may be null (no processes)
};

// The capabilities this broker provides. A command is only available when its capability is here.
std::vector<std::string> broker_capabilities();

// Maximum modules per process listed by `status` (enabled ones first); the result carries
// "modules_truncated": true when the list was cut.
inline constexpr std::size_t kMaxStatusModulesPerProcess = 2000;

ipc::ControlResponse handle_control_request(const ipc::ControlRequest& request, const ControlContext& context);

// One request line in, one response line out (no newlines). Never fails: a line that is not JSON
// or not a valid request is answered with a CML5400 / CML5401 diagnostic response, so the
// connection stays usable.
std::string handle_control_line(std::string_view line, const ControlContext& context);

}  // namespace cml::core

#if defined(_WIN32)

#include <atomic>
#include <memory>
#include <thread>

#include "cml/ipc/pipe.h"

namespace cml::core {

class ControlServer {
public:
    ControlServer(std::string name, ControlContext context_in);
    ~ControlServer();
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // Creates the pipe (CML5207 when the name is already served, CML5208 for a bad name) and starts
    // serving on background threads. `context.registry` must outlive the server.
    Result<void> start();

    // Any thread, idempotent: stops accepting, closes every connection and joins all threads.
    void stop() noexcept;

    const std::string& pipe_name() const noexcept { return pipe_name_; }

private:
    struct Connection;
    void accept_loop();
    void serve(Connection& connection);

    std::string pipe_name_;
    ControlContext context_;
    ipc::PipeServer pipe_;
    std::atomic<bool> stopping_{false};
    std::thread accept_thread_;
    std::vector<std::unique_ptr<Connection>> connections_;  // touched by the accept thread only (and stop() after joining it)
    bool started_ = false;
};

}  // namespace cml::core

#endif  // _WIN32
