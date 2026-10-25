#include "cml/ipc/pipe.h"

#if defined(_WIN32)

#include <windows.h>

#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <utility>
#include <vector>

#include "cml/proc/diag_ids.h"
#include "cml/proc/proc.h"
#include "cml/util/log.h"

namespace cml::ipc {

namespace {

constexpr DWORD kPipeBufferBytes = 64 * 1024;
constexpr char kPipePrefix[] = "\\\\.\\pipe\\";

std::string os_error_text(DWORD error) {
    wchar_t buffer[256];
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, error, 0, buffer,
                                   static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])), nullptr);
    std::string text = "Win32 error " + std::to_string(error);
    if (n > 0) {
        std::string detail = proc::utf8_from_wide(buffer, n);
        while (!detail.empty() &&
               (detail.back() == '\r' || detail.back() == '\n' || detail.back() == ' ' ||
                detail.back() == '.')) {
            detail.pop_back();
        }
        text += ": " + detail;
    }
    return text;
}

bool is_peer_gone(DWORD error) {
    return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
           error == ERROR_NO_DATA || error == ERROR_NETNAME_DELETED;
}

Diag io_diag(const char* what, DWORD error) {
    return Diag::make(ids::kPipeIoFailed, std::string(what) + " failed (" + os_error_text(error) + ")",
                      "Restart the application and the broker; if it persists, report it with the "
                      "broker log.");
}

bool has_pipe_prefix(const std::string& name) {
    const std::size_t n = sizeof(kPipePrefix) - 1;
    if (name.size() <= n) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != kPipePrefix[i]) {
            return false;
        }
    }
    return true;
}

Diag bad_name_diag(const std::string& name) {
    return Diag::make(ids::kPipeNameInvalid,
                      "pipe name '" + name + "' does not start with \\\\.\\pipe\\ or has no name part",
                      "Set CML_PIPE_NAME (or --pipe) to a full name such as \\\\.\\pipe\\cml_test_1.");
}

// Deadline arithmetic on GetTickCount64 so that waits are bounded across loop iterations.
constexpr DWORD kReplaceRetryMs = 50;  // pause between attempts to create the next listening instance

ULONGLONG deadline_after(std::uint32_t timeout_ms) {
    return timeout_ms == kInfiniteTimeout ? 0 : GetTickCount64() + timeout_ms;
}

DWORD remaining_ms(std::uint32_t timeout_ms, ULONGLONG deadline) {
    if (timeout_ms == kInfiniteTimeout) {
        return INFINITE;
    }
    const ULONGLONG now = GetTickCount64();
    return now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
}

bool signalled(HANDLE event) {
    return WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
}

}  // namespace

// --- PipeConnection -----------------------------------------------------------------------------

struct PipeConnection::Impl {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE read_event = nullptr;
    HANDLE write_event = nullptr;
    HANDLE cancel_event = nullptr;
    OVERLAPPED read_ov;
    OVERLAPPED write_ov;
    bool read_pending = false;
    bool peer_closed = false;
    bool write_broken = false;
    DWORD peer_pid = 0;

    // Receive state, kept across timeouts so that a half-received frame is never lost.
    std::uint8_t header[kFrameHeaderSize];
    std::size_t header_have = 0;
    bool header_done = false;
    FrameHeader parsed;
    std::vector<std::uint8_t> payload;
    std::size_t payload_have = 0;

    Impl() {
        std::memset(&read_ov, 0, sizeof(read_ov));
        std::memset(&write_ov, 0, sizeof(write_ov));
        std::memset(header, 0, sizeof(header));
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() { close_all(); }

    bool create_events() {
        read_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        write_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return read_event != nullptr && write_event != nullptr && cancel_event != nullptr;
    }

    void close_all() {
        if (pipe != INVALID_HANDLE_VALUE) {
            if (read_pending) {
                // The kernel still owns read_ov and the receive buffer until the cancelled read
                // has completed.
                CancelIoEx(pipe, &read_ov);
                DWORD ignored = 0;
                GetOverlappedResult(pipe, &read_ov, &ignored, TRUE);
                read_pending = false;
            }
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
        for (HANDLE* event : {&read_event, &write_event, &cancel_event}) {
            if (*event != nullptr) {
                CloseHandle(*event);
                *event = nullptr;
            }
        }
    }

    void reset_receive_state() {
        header_have = 0;
        header_done = false;
        payload.clear();
        payload_have = 0;
    }
};

PipeConnection::PipeConnection() noexcept = default;
PipeConnection::~PipeConnection() = default;
PipeConnection::PipeConnection(PipeConnection&&) noexcept = default;
PipeConnection& PipeConnection::operator=(PipeConnection&&) noexcept = default;
PipeConnection::PipeConnection(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

bool PipeConnection::is_open() const noexcept {
    return impl_ != nullptr && impl_->pipe != INVALID_HANDLE_VALUE;
}

std::uint32_t PipeConnection::peer_process_id() const noexcept {
    return impl_ ? impl_->peer_pid : 0;
}

void PipeConnection::cancel() noexcept {
    if (impl_ && impl_->cancel_event != nullptr) {
        SetEvent(impl_->cancel_event);
    }
}

void PipeConnection::close() noexcept {
    if (impl_) {
        impl_->close_all();
    }
}

Result<IoStatus> PipeConnection::send(const Message& message, std::uint32_t timeout_ms) {
    if (!is_open()) {
        return IoStatus::Closed;
    }
    Impl& s = *impl_;
    if (s.write_broken) {
        return IoStatus::Closed;
    }
    if (signalled(s.cancel_event)) {
        return IoStatus::Cancelled;
    }
    Result<std::vector<std::uint8_t>> frame = encode_frame(message);
    if (!frame) {
        return frame.error();
    }
    const std::vector<std::uint8_t>& bytes = *frame;
    const ULONGLONG deadline = deadline_after(timeout_ms);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        ResetEvent(s.write_event);
        std::memset(&s.write_ov, 0, sizeof(s.write_ov));
        s.write_ov.hEvent = s.write_event;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
        const BOOL started = WriteFile(s.pipe, bytes.data() + offset, chunk, nullptr, &s.write_ov);
        if (!started) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                s.write_broken = true;
                if (is_peer_gone(error)) {
                    return IoStatus::Closed;
                }
                return io_diag("writing to the pipe", error);
            }
        }
        const HANDLE waits[2] = {s.write_event, s.cancel_event};
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, remaining_ms(timeout_ms, deadline));
        if (w == WAIT_OBJECT_0) {
            DWORD written = 0;
            if (!GetOverlappedResult(s.pipe, &s.write_ov, &written, FALSE)) {
                const DWORD error = GetLastError();
                s.write_broken = true;
                if (is_peer_gone(error)) {
                    return IoStatus::Closed;
                }
                return io_diag("writing to the pipe", error);
            }
            offset += written;
            continue;
        }
        // Timeout, cancel or wait failure: stop the write; it may have partly happened, so the
        // stream is no longer frame-aligned unless the write finished in the meantime.
        const DWORD wait_error = GetLastError();
        CancelIoEx(s.pipe, &s.write_ov);
        DWORD written = 0;
        if (GetOverlappedResult(s.pipe, &s.write_ov, &written, TRUE)) {
            offset += written;
            if (offset == bytes.size()) {
                return IoStatus::Ok;
            }
        }
        s.write_broken = true;
        if (w == WAIT_OBJECT_0 + 1) {
            return IoStatus::Cancelled;
        }
        if (w == WAIT_TIMEOUT) {
            return IoStatus::Timeout;
        }
        return io_diag("waiting to write to the pipe", wait_error);
    }
    return IoStatus::Ok;
}

Result<PipeConnection::Received> PipeConnection::receive(std::uint32_t timeout_ms, void* wake_event) {
    Received out;
    if (!is_open()) {
        out.status = IoStatus::Closed;
        return out;
    }
    Impl& s = *impl_;
    const ULONGLONG deadline = deadline_after(timeout_ms);
    for (;;) {
        if (s.peer_closed) {
            out.status = IoStatus::Closed;
            return out;
        }
        if (signalled(s.cancel_event)) {
            out.status = IoStatus::Cancelled;
            return out;
        }
        if (!s.read_pending) {
            std::uint8_t* destination = nullptr;
            std::size_t wanted = 0;
            if (!s.header_done) {
                destination = s.header + s.header_have;
                wanted = kFrameHeaderSize - s.header_have;
            } else {
                destination = s.payload.data() + s.payload_have;
                wanted = s.payload.size() - s.payload_have;
            }
            ResetEvent(s.read_event);
            std::memset(&s.read_ov, 0, sizeof(s.read_ov));
            s.read_ov.hEvent = s.read_event;
            // Whether ReadFile completes inline or later, completion is signalled on read_event.
            const BOOL started = ReadFile(s.pipe, destination, static_cast<DWORD>(wanted), nullptr, &s.read_ov);
            if (!started) {
                const DWORD error = GetLastError();
                if (error != ERROR_IO_PENDING) {
                    if (is_peer_gone(error)) {
                        s.peer_closed = true;
                        out.status = IoStatus::Closed;
                        return out;
                    }
                    return io_diag("reading from the pipe", error);
                }
            }
            s.read_pending = true;
        }

        HANDLE waits[3] = {s.read_event, s.cancel_event, static_cast<HANDLE>(wake_event)};
        const DWORD count = wake_event != nullptr ? 3 : 2;
        const DWORD w = WaitForMultipleObjects(count, waits, FALSE, remaining_ms(timeout_ms, deadline));
        if (w == WAIT_TIMEOUT) {
            out.status = IoStatus::Timeout;
            return out;
        }
        if (w == WAIT_OBJECT_0 + 1) {
            out.status = IoStatus::Cancelled;
            return out;
        }
        if (w == WAIT_OBJECT_0 + 2) {
            out.status = IoStatus::Woken;
            return out;
        }
        if (w != WAIT_OBJECT_0) {
            return io_diag("waiting for pipe data", GetLastError());
        }

        DWORD got = 0;
        const BOOL done = GetOverlappedResult(s.pipe, &s.read_ov, &got, FALSE);
        s.read_pending = false;
        if (!done) {
            const DWORD error = GetLastError();
            if (is_peer_gone(error)) {
                s.peer_closed = true;
                out.status = IoStatus::Closed;
                return out;
            }
            return io_diag("reading from the pipe", error);
        }

        if (!s.header_done) {
            s.header_have += got;
            if (s.header_have < kFrameHeaderSize) {
                continue;
            }
            Result<FrameHeader> header = parse_frame_header(s.header, kFrameHeaderSize);
            if (!header) {
                s.reset_receive_state();
                return header.error();
            }
            s.parsed = *header;
            s.header_done = true;
            s.payload.assign(s.parsed.payload_size, 0);
            s.payload_have = 0;
        } else {
            s.payload_have += got;
        }
        if (s.header_done && s.payload_have == s.payload.size()) {
            const FrameHeader header = s.parsed;
            Result<Message> message =
                decode_message(header.type, header.version, s.payload.data(), s.payload.size());
            std::vector<std::uint8_t> rejected_payload;
            if (!message) {
                rejected_payload.assign(s.payload.begin(),
                                        s.payload.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(s.payload.size(), 8)));
            }
            s.reset_receive_state();
            if (!message) {
                out.status = IoStatus::Rejected;  // the stream is still frame-aligned
                out.error = message.error();
                out.raw_type = header.type;
                out.is_request = is_request_type(header.type);
                out.request_id = peek_request_id(header.type, rejected_payload.data(), rejected_payload.size());
                return out;
            }
            out.status = IoStatus::Ok;
            out.message = std::move(*message);
            return out;
        }
    }
}

namespace {

// Builds a PipeConnection implementation around an open overlapped pipe handle. Takes ownership
// of `pipe` (closed on failure).
std::unique_ptr<PipeConnection::Impl> make_impl(HANDLE pipe, DWORD peer_pid) {
    auto impl = std::make_unique<PipeConnection::Impl>();
    impl->pipe = pipe;
    impl->peer_pid = peer_pid;
    if (!impl->create_events()) {
        return nullptr;  // ~Impl closes the pipe and any event that was created
    }
    return impl;
}

}  // namespace

// --- client -------------------------------------------------------------------------------------

bool pipe_exists(const std::string& pipe_name) {
    const std::wstring wide = proc::wide_from_utf8(pipe_name);
    if (WaitNamedPipeW(wide.c_str(), 1)) {
        return true;
    }
    const DWORD error = GetLastError();
    return error == ERROR_SEM_TIMEOUT || error == ERROR_PIPE_BUSY;
}

Result<PipeConnection> connect_pipe(const std::string& pipe_name, std::uint32_t timeout_ms) {
    if (!has_pipe_prefix(pipe_name)) {
        return bad_name_diag(pipe_name);
    }
    const std::wstring wide = proc::wide_from_utf8(pipe_name);
    const ULONGLONG deadline = deadline_after(timeout_ms);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (;;) {
        // SECURITY_IDENTIFICATION: the server may learn who we are but cannot impersonate us.
        pipe = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                           nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            break;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_BUSY) {
            const DWORD left = remaining_ms(timeout_ms, deadline);
            if (left > 0) {
                WaitNamedPipeW(wide.c_str(), std::min<DWORD>(left, 250));
                continue;
            }
        }
        const char* reason = error == ERROR_FILE_NOT_FOUND ? "no broker is listening there"
                             : error == ERROR_ACCESS_DENIED ? "access denied (the pipe belongs to another user)"
                             : error == ERROR_PIPE_BUSY    ? "all pipe instances stayed busy"
                                                           : "the OS refused the connection";
        return Diag::make(ids::kBrokerUnreachable,
                          "cannot connect to broker pipe '" + pipe_name + "': " + reason + " (" +
                              os_error_text(error) + ")",
                          "Start cml_broker, or set CML_PIPE_NAME to the pipe the broker listens on.");
    }

    ULONG server_pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &server_pid)) {
        const DWORD error = GetLastError();
        CloseHandle(pipe);
        return Diag::make(ids::kPipeServerUntrusted,
                          "cannot identify the process serving '" + pipe_name + "' (" +
                              os_error_text(error) + ")",
                          "Make sure the broker runs on this machine as your user.");
    }
    // Do not talk to a pipe that another user created under our name.
    Result<bool> same_user = proc::process_runs_as_current_user(server_pid);
    if (!same_user || !*same_user) {
        CloseHandle(pipe);
        return Diag::make(ids::kPipeServerUntrusted,
                          "the process serving '" + pipe_name + "' (pid " + std::to_string(server_pid) +
                              ") " + (same_user ? "runs as a different user" : "cannot be verified"),
                          "Stop that process or choose another pipe name with CML_PIPE_NAME.");
    }
    std::unique_ptr<PipeConnection::Impl> impl = make_impl(pipe, server_pid);
    if (!impl) {
        return Diag::make(ids::kPipeIoFailed, "cannot create synchronisation events for the pipe",
                          "Close other programs to free system resources and retry.");
    }
    return PipeConnection(std::move(impl));
}

// --- server -------------------------------------------------------------------------------------

std::atomic<int> g_injected_create_failures{0};

void set_instance_create_failures_for_testing(int count) {
    g_injected_create_failures.store(count);
}

struct PipeServer::Impl {
    std::string name;
    std::wstring wide_name;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes;
    HANDLE listening = INVALID_HANDLE_VALUE;  // an instance waiting for the next client
    // A connected instance that accept() has not handed to the caller yet because the replacement
    // listening instance could not be created. accept() creates the replacement BEFORE handing the
    // connected instance over; while that keeps failing it keeps retrying and keeps owning this
    // handle, so the pipe name is never without an instance and cannot be taken by another process.
    HANDLE pending = INVALID_HANDLE_VALUE;
    HANDLE connect_event = nullptr;
    HANDLE cancel_event = nullptr;
    OVERLAPPED ov;
    bool first_instance_created = false;

    Impl() {
        std::memset(&attributes, 0, sizeof(attributes));
        std::memset(&ov, 0, sizeof(ov));
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
        if (listening != INVALID_HANDLE_VALUE) {
            CloseHandle(listening);
        }
        if (pending != INVALID_HANDLE_VALUE) {
            CloseHandle(pending);
        }
        if (connect_event != nullptr) {
            CloseHandle(connect_event);
        }
        if (cancel_event != nullptr) {
            CloseHandle(cancel_event);
        }
        if (descriptor != nullptr) {
            LocalFree(descriptor);
        }
    }

    // Creates one pipe instance. The very first one carries FILE_FLAG_FIRST_PIPE_INSTANCE so that
    // creation fails if any other process already serves this name (no squatting, no two brokers).
    Result<HANDLE> create_instance() {
        if (g_injected_create_failures.load() > 0 && first_instance_created &&
            g_injected_create_failures.fetch_sub(1) > 0) {
            return Diag::make(ids::kPipeCreateFailed, "cannot create pipe '" + name + "' (injected failure)",
                              "Test hook: set_instance_create_failures_for_testing.");
        }
        DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
        if (!first_instance_created) {
            open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
        }
        HANDLE h = CreateNamedPipeW(
            wide_name.c_str(), open_mode,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, kPipeBufferBytes, kPipeBufferBytes, 0, &attributes);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            const bool taken = !first_instance_created && error == ERROR_ACCESS_DENIED;
            return Diag::make(ids::kPipeCreateFailed,
                              "cannot create pipe '" + name + "'" +
                                  (taken ? ": another process already serves this name" : "") + " (" +
                                  os_error_text(error) + ")",
                              taken ? "Stop the other broker or pass a different --pipe name."
                                    : "Check the pipe name and that the process may create pipes.");
        }
        first_instance_created = true;
        return h;
    }
};

PipeServer::PipeServer() noexcept = default;
PipeServer::~PipeServer() = default;
PipeServer::PipeServer(PipeServer&&) noexcept = default;
PipeServer& PipeServer::operator=(PipeServer&&) noexcept = default;
PipeServer::PipeServer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

const std::string& PipeServer::name() const noexcept {
    static const std::string kEmpty;
    return impl_ ? impl_->name : kEmpty;
}

Result<PipeServer> PipeServer::create(const std::string& pipe_name) {
    if (!has_pipe_prefix(pipe_name)) {
        return bad_name_diag(pipe_name);
    }
    auto impl = std::make_unique<Impl>();
    impl->name = pipe_name;
    impl->wide_name = proc::wide_from_utf8(pipe_name);

    // Protected DACL with a single ACE: full access for the current user only. No inherited
    // entries, so neither other users nor (unless they are this user) services get access.
    Result<std::string> sid = proc::current_user_sid();
    if (!sid) {
        return sid.error();
    }
    const std::wstring sddl = L"D:P(A;;GA;;;" + proc::wide_from_utf8(*sid) + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &impl->descriptor, nullptr)) {
        return Diag::make(ids::kPipeCreateFailed,
                          "cannot build the pipe security descriptor (" + os_error_text(GetLastError()) + ")",
                          "Report this as a bug; Chameleon refuses to create an unrestricted pipe.");
    }
    impl->attributes.nLength = sizeof(impl->attributes);
    impl->attributes.lpSecurityDescriptor = impl->descriptor;
    impl->attributes.bInheritHandle = FALSE;

    impl->connect_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl->cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (impl->connect_event == nullptr || impl->cancel_event == nullptr) {
        return Diag::make(ids::kPipeCreateFailed, "cannot create synchronisation events for the pipe",
                          "Close other programs to free system resources and retry.");
    }
    Result<HANDLE> first = impl->create_instance();
    if (!first) {
        return first.error();
    }
    impl->listening = *first;
    return PipeServer(std::move(impl));
}

namespace {
void replace_listening_instance(PipeServer::Impl& s);
}  // namespace

void PipeServer::refresh_instance() {
    if (impl_) {
        replace_listening_instance(*impl_);
    }
}

bool PipeServer::has_pending_connection() const noexcept {
    return impl_ && impl_->pending != INVALID_HANDLE_VALUE;
}

void PipeServer::cancel() noexcept {
    if (impl_ && impl_->cancel_event != nullptr) {
        SetEvent(impl_->cancel_event);
    }
}

namespace {

// After an unexpected error the listening instance may be unusable. Replace it, but create the new
// one BEFORE closing the old: the pipe name must never be left without an instance, or another
// process could take it over. If no replacement can be made the old instance stays (the next
// accept() reports the problem again), so the server keeps owning the name.
void replace_listening_instance(PipeServer::Impl& s) {
    Result<HANDLE> fresh = s.create_instance();
    if (!fresh) {
        CML_LOG(Warn, "%s", fresh.error().format().c_str());
        return;
    }
    if (s.listening != INVALID_HANDLE_VALUE) {
        CloseHandle(s.listening);
    }
    s.listening = *fresh;
}

}  // namespace

Result<PipeServer::Accepted> PipeServer::accept(std::uint32_t timeout_ms, void* wake_event) {
    Accepted out;
    if (!impl_) {
        out.status = IoStatus::Closed;
        return out;
    }
    Impl& s = *impl_;
    if (signalled(s.cancel_event)) {
        out.status = IoStatus::Cancelled;
        return out;
    }
    // The whole call (waiting for a client and creating the replacement instance) takes at most
    // timeout_ms.
    const ULONGLONG accept_deadline = deadline_after(timeout_ms);
    // A client that connects and vanishes before we get to it (ERROR_NO_DATA) is not an error of
    // ours: drop that instance's state and listen again. One bad client must never stop the
    // broker, so only a run of such events (a pathological flood) gives up for this call.
    constexpr int kMaxDroppedClients = 64;
    bool connected = s.pending != INVALID_HANDLE_VALUE;  // a connection from an earlier call, see Impl::pending
    for (int dropped = 0; !connected; ++dropped) {
        if (dropped >= kMaxDroppedClients) {
            out.status = IoStatus::Timeout;  // the caller's loop calls accept() again
            return out;
        }
        if (s.listening == INVALID_HANDLE_VALUE) {
            Result<HANDLE> instance = s.create_instance();
            if (!instance) {
                return instance.error();
            }
            s.listening = *instance;
        }

        ResetEvent(s.connect_event);
        std::memset(&s.ov, 0, sizeof(s.ov));
        s.ov.hEvent = s.connect_event;
        if (ConnectNamedPipe(s.listening, &s.ov)) {
            connected = true;
            break;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            connected = true;  // the client connected before we started waiting
            break;
        }
        if (error == ERROR_NO_DATA) {
            DisconnectNamedPipe(s.listening);  // the client already left; reuse the instance
            continue;
        }
        if (error != ERROR_IO_PENDING) {
            replace_listening_instance(s);  // do not keep retrying on a handle that is broken
            return io_diag("waiting for a client", error);
        }

        HANDLE waits[3] = {s.connect_event, s.cancel_event, static_cast<HANDLE>(wake_event)};
        const DWORD count = wake_event != nullptr ? 3 : 2;
        const DWORD w = WaitForMultipleObjects(count, waits, FALSE, remaining_ms(timeout_ms, accept_deadline));
        DWORD transferred = 0;
        if (w == WAIT_OBJECT_0) {
            if (GetOverlappedResult(s.listening, &s.ov, &transferred, FALSE)) {
                connected = true;
            } else if (GetLastError() == ERROR_NO_DATA) {
                DisconnectNamedPipe(s.listening);
            } else {
                const DWORD accept_error = GetLastError();
                replace_listening_instance(s);
                return io_diag("accepting a client", accept_error);
            }
        } else {
            // Stop waiting. A client may connect in the instant before the cancel takes effect;
            // then the connection is real and must not be dropped.
            const DWORD wait_error = GetLastError();
            CancelIoEx(s.listening, &s.ov);
            if (GetOverlappedResult(s.listening, &s.ov, &transferred, TRUE)) {
                connected = true;
            } else if (w == WAIT_TIMEOUT) {
                out.status = IoStatus::Timeout;
                return out;
            } else if (w == WAIT_OBJECT_0 + 1) {
                out.status = IoStatus::Cancelled;
                return out;
            } else if (w == WAIT_OBJECT_0 + 2) {
                out.status = IoStatus::Woken;
                return out;
            } else {
                replace_listening_instance(s);
                return io_diag("waiting for a client", wait_error);
            }
        }
    }

    // The next listening instance must exist before the connected one leaves our hands: until then
    // the connected instance (owned by s.pending) is what keeps the name alive. If the replacement
    // cannot be created, keep trying until it works, the wait is cancelled, woken or out of time;
    // the connection stays pending and is handed over by a later accept() call.
    if (s.pending == INVALID_HANDLE_VALUE) {
        s.pending = s.listening;
        s.listening = INVALID_HANDLE_VALUE;
    }
    bool warned = false;
    while (s.listening == INVALID_HANDLE_VALUE) {
        Result<HANDLE> next = s.create_instance();
        if (next) {
            s.listening = *next;
            break;
        }
        if (!warned) {
            CML_LOG(Warn, "%s; retrying", next.error().format().c_str());
            warned = true;
        }
        const DWORD left = remaining_ms(timeout_ms, accept_deadline);
        if (left == 0) {
            out.status = IoStatus::Timeout;
            return out;
        }
        HANDLE waits[2] = {s.cancel_event, static_cast<HANDLE>(wake_event)};
        const DWORD count = wake_event != nullptr ? 2 : 1;
        const DWORD w = WaitForMultipleObjects(count, waits, FALSE, std::min<DWORD>(left, kReplaceRetryMs));
        if (w == WAIT_OBJECT_0) {
            out.status = IoStatus::Cancelled;
            return out;
        }
        if (w == WAIT_OBJECT_0 + 1) {
            out.status = IoStatus::Woken;
            return out;
        }
    }

    HANDLE connected_pipe = s.pending;
    s.pending = INVALID_HANDLE_VALUE;
    ULONG client_pid = 0;
    GetNamedPipeClientProcessId(connected_pipe, &client_pid);
    std::unique_ptr<PipeConnection::Impl> impl = make_impl(connected_pipe, client_pid);
    if (!impl) {
        return Diag::make(ids::kPipeIoFailed, "cannot create synchronisation events for the pipe",
                          "Close other programs to free system resources and retry.");
    }
    out.status = IoStatus::Ok;
    out.connection = PipeConnection(std::move(impl));
    return out;
}

// --- naming -------------------------------------------------------------------------------------

Result<std::string> resolve_pipe_name(const std::string& explicit_name) {
    std::string name = explicit_name;
    if (name.empty()) {
        const std::optional<std::string> from_env = proc::get_env("CML_PIPE_NAME");
        if (from_env && !from_env->empty()) {
            name = *from_env;
        }
    }
    if (name.empty()) {
        Result<std::string> sid = proc::logon_session_sid();
        if (!sid) {
            return sid.error();
        }
        name = std::string(kPipePrefix) + "cml_broker_" + *sid;
    }
    if (!has_pipe_prefix(name)) {
        return bad_name_diag(name);
    }
    return name;
}

}  // namespace cml::ipc

#endif  // _WIN32
