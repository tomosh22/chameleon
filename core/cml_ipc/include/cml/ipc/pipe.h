#pragma once

// Overlapped named-pipe transport shared by the agent and the broker (plan section 4.13).
//
// Windows-only; no exceptions, no RTTI, no third-party code. Frames are those of messages.h on a
// byte-mode pipe.
//
// Threading rules for PipeConnection:
//   * one thread at a time may call receive() and one thread at a time may call send(); the two
//     may run concurrently with each other;
//   * cancel() may be called from any thread at any time and is sticky: every blocked or later
//     receive()/send() returns IoStatus::Cancelled;
//   * the destructor and close() must not race with receive()/send(): cancel(), join the user
//     threads, then destroy.
//
// Security: the server restricts the pipe to the current user with a protected DACL, rejects
// remote clients and refuses to start if another process already owns the pipe name; clients open
// the pipe at SECURITY_IDENTIFICATION level (the server cannot impersonate them) and check that
// the server process runs as the same user. See docs/wp/WP-11.md.

#if defined(_WIN32)

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "cml/ipc/messages.h"
#include "cml/util/result.h"

namespace cml::ipc {

// Result of a blocking operation that did not fail with an error.
enum class IoStatus {
    Ok,         // completed
    Timeout,    // the timeout elapsed first (a partially received frame is kept for next time)
    Woken,      // receive(): the caller's wake event was signalled
    Closed,     // the peer closed the pipe (or the connection was closed locally)
    Cancelled,  // cancel() was called
    Rejected,   // receive(): a whole frame arrived but its payload was invalid or unsupported
                // (CML5201..5203); framing is intact, so the connection stays usable
};

inline constexpr std::uint32_t kInfiniteTimeout = 0xFFFFFFFFu;

class PipeConnection {
public:
    PipeConnection() noexcept;
    ~PipeConnection();
    PipeConnection(PipeConnection&&) noexcept;
    PipeConnection& operator=(PipeConnection&&) noexcept;
    PipeConnection(const PipeConnection&) = delete;
    PipeConnection& operator=(const PipeConnection&) = delete;

    bool is_open() const noexcept;

    // Writes one complete frame. Ok, Timeout (the connection is unusable afterwards because a
    // partial frame may have been written), Closed or Cancelled. A Diag (CML5200..5206) for
    // encoding errors and unexpected OS errors.
    Result<IoStatus> send(const Message& message, std::uint32_t timeout_ms);

    struct Received {
        IoStatus status = IoStatus::Closed;
        Message message;  // valid only when status == Ok
        // Valid only when status == Rejected: why, and what is known about the frame.
        Diag error;
        std::uint16_t raw_type = 0;       // wire type value (may not be a known MessageType)
        bool is_request = false;          // a type that expects an answer
        std::optional<std::uint64_t> request_id;  // readable even if the rest of the payload is bad
    };

    // Waits for one complete frame. `wake_event` (a Win32 event HANDLE, may be null) makes the call
    // return Woken as soon as it is signalled, so one thread can wait for both pipe traffic and
    // its own work queue. A frame whose header is broken (bad length) returns a Diag and the
    // connection must be closed; a frame with a well-formed header but an invalid payload is
    // consumed and reported as IoStatus::Rejected so the caller can log it and carry on.
    Result<Received> receive(std::uint32_t timeout_ms, void* wake_event = nullptr);

    void cancel() noexcept;
    void close() noexcept;

    // Process id of the other end of the pipe (0 if unknown).
    std::uint32_t peer_process_id() const noexcept;

    struct Impl;  // opaque; defined in pipe.cpp

private:
    friend class PipeServer;
    friend Result<PipeConnection> connect_pipe(const std::string&, std::uint32_t);
    explicit PipeConnection(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// Opens a client connection to `pipe_name` ("\\.\pipe\..."). Waits up to `timeout_ms` when all
// pipe instances are busy; returns immediately with CML5205 when the pipe does not exist. Also
// CML5205 (access denied) and CML5209 (server runs as another user).
Result<PipeConnection> connect_pipe(const std::string& pipe_name, std::uint32_t timeout_ms);

// True if a pipe with this name exists (without connecting to it).
bool pipe_exists(const std::string& pipe_name);

// Test hook: the next `count` attempts to create a further pipe instance (after the server exists)
// fail, as if the system were out of resources.
void set_instance_create_failures_for_testing(int count);

class PipeServer {
public:
    PipeServer() noexcept;
    ~PipeServer();
    PipeServer(PipeServer&&) noexcept;
    PipeServer& operator=(PipeServer&&) noexcept;
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    // Creates the first pipe instance. Fails with CML5207 when the name is taken by another
    // process or the security descriptor cannot be built, CML5208 for a malformed name.
    static Result<PipeServer> create(const std::string& pipe_name);

    struct Accepted {
        IoStatus status = IoStatus::Closed;  // Ok, Timeout, Woken or Cancelled
        PipeConnection connection;           // valid only when status == Ok
    };

    // Waits for the next client. Any number of connections may be open at once. The pipe name is
    // never left without an instance: when a client has connected, the replacement listening
    // instance is created before the connection is returned. If that creation fails, accept()
    // keeps retrying (and keeps owning the connected instance) until it works or the wait times
    // out / is cancelled / is woken; the connection is then returned by a later call.
    Result<Accepted> accept(std::uint32_t timeout_ms, void* wake_event = nullptr);

    // Replaces the instance that waits for the next client with a fresh one (accept() does this by
    // itself after an unexpected error). The new instance is created before the old one is closed,
    // so the pipe name is never without an instance and cannot be taken by another process.
    void refresh_instance();

    // Any thread; sticky. Makes accept() return Cancelled.
    void cancel() noexcept;

    const std::string& name() const noexcept;

    struct Impl;  // opaque; defined in pipe.cpp

private:
    explicit PipeServer(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// Pipe name resolution (plan section 4.13): `explicit_name` (e.g. --pipe) if non-empty, else the
// CML_PIPE_NAME environment variable, else `\\.\pipe\cml_broker_<logon session SID>`. Names must
// start with `\\.\pipe\` (CML5208).
Result<std::string> resolve_pipe_name(const std::string& explicit_name);

}  // namespace cml::ipc

#endif  // _WIN32
