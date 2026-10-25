#pragma once

// Accepts agent connections on the broker pipe and runs one Session per connection.
//
// WP-11 scope: a thread per connection, enough for the headless broker. WP-12 may replace the
// threading model; the Session (session.h) is the part that carries over.

#if defined(_WIN32)

#include <atomic>
#include <cstdint>
#include <string>

#include "cml/util/result.h"

namespace cml::core {

struct ServerOptions {
    std::string pipe_name;                    // full \\.\pipe\ name
    std::uint32_t exit_when_idle_ms = 0;      // 0 = never; else exit this long after the last agent left
    std::uint32_t startup_timeout_ms = 0;     // 0 = never; else exit if no agent ever connected in time
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

private:
    ServerOptions options_;
    std::atomic<bool> stop_requested_{false};
    void* stop_event_ = nullptr;  // manual-reset event, set by stop()
};

}  // namespace cml::core

#endif  // _WIN32
