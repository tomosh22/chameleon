#pragma once

// Accepts agent connections on the broker pipe and runs one Session per connection.
//
// WP-11 scope: a thread per connection, enough for the headless broker. WP-12 may replace the
// threading model; the Session (session.h) is the part that carries over.

#if defined(_WIN32)

#include <atomic>
#include <cstdint>
#include <string>

#include "cml/core/event_bus.h"
#include "cml/util/result.h"

namespace cml::core {

struct PipelineEnv;

struct ServerOptions {
    std::string pipe_name;                    // full \\.\pipe\ name
    // Full name of the control pipe (cml_ipc/control.h). Empty = no control pipe. When set, the pipe
    // must be creatable: run() fails with the pipe's Diag otherwise. Control clients do not count
    // as agents for the idle and startup timers.
    std::string control_pipe_name;
    // true: a control pipe that cannot be created (the default name is served by another broker) is a warning,
    // not a failure of run(). Set when the name was not asked for explicitly.
    bool control_pipe_optional = false;
    // WP-12: where the reload pipeline keeps objects and patch DLLs ("" = $CML_SESSION_DIR, else a new directory
    // under the temporary directory) and the compiler/linker it uses (null = the real ones).
    std::string session_dir;
    const PipelineEnv* pipeline_env = nullptr;
    std::uint32_t exit_when_idle_ms = 0;     // 0 = never; else exit this long after the last agent left
    std::uint32_t startup_timeout_ms = 0;     // 0 = never; else exit if no agent ever connected in time
    // Optional manual-reset event handle owned by the caller (never closed by the server). Setting
    // it stops the server, exactly like stop(); meant for signal handlers (Ctrl+C), which must not
    // touch a server object that may be torn down at the same time.
    void* stop_event = nullptr;
};

enum class ServerExit {
    Stopped,         // stop() was called
    Idle,            // exit_when_idle_ms elapsed after the last agent disconnected
    StartupTimeout,  // no agent connected within startup_timeout_ms
};

class ConnectionServer {
public:
    explicit ConnectionServer(ServerOptions options);
    ~ConnectionServer();
    ConnectionServer(const ConnectionServer&) = delete;
    ConnectionServer& operator=(const ConnectionServer&) = delete;

    // Serves until stop(), the idle timeout or the startup timeout. Blocks the calling thread.
    // Fails (CML5207 and friends) only if the pipe cannot be created.
    Result<ServerExit> run();

    // Any thread. Idempotent. Wakes run() at once, even while it waits for a client.
    void stop() noexcept;

    // Control-pipe events (diag, patch_applied, log). The structured log sink attaches itself here.
    EventBus& event_bus() noexcept { return events_; }

private:
    ServerOptions options_;
    EventBus events_;
    std::atomic<bool> stop_requested_{false};
    void* stop_event_ = nullptr;  // manual-reset event, set by stop() (options_.stop_event or our own)
    bool owns_stop_event_ = false;
};

}  // namespace cml::core

#endif  // _WIN32
