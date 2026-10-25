#include "agent.h"

#if defined(_WIN32)

#include <process.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <utility>

#include "agent_log.h"
#include "broker_launch.h"
#include "cml/proc/diag_ids.h"
#include "cml/proc/proc.h"
#include "cml/util/log.h"
#include "cml/util/path.h"
#include "module_diff.h"

namespace cml::agent {

namespace {

constexpr DWORD kSendTimeoutMs = 5000;
constexpr DWORD kHandshakeTimeoutMs = 5000;
constexpr ULONGLONG kModulePollMs = 500;
constexpr DWORD kReceiveSliceMs = 250;       // longest the agent thread sleeps without re-checking
// The request table. Answers nobody has collected are kept only up to kMaxUnconsumedAnswers (the
// oldest are dropped first) and are evicted when the table is full, so they can never keep new
// requests out; kReenableReserve is extra room for the internal re-enables after a reconnect.
constexpr std::size_t kMaxOutstanding = 1024;
constexpr std::size_t kMaxUnconsumedAnswers = 512;
constexpr std::size_t kMaxTrackedModules = 256;
constexpr std::size_t kReenableReserve = kMaxTrackedModules;
constexpr std::size_t kMaxLogsPerPass = 16;
// Requests sent but not yet answered. Bounded so that the agent keeps reading replies while it
// writes: with an unbounded burst both pipe buffers could fill up and stall both ends.
constexpr std::size_t kMaxInFlight = 16;
constexpr std::uint32_t kMaxBackoffMs = 1000;
constexpr DWORD kReconnectPauseMs = 200;   // pause after a session that was up, before reconnecting

// Any address inside this module: used to find the path of cml_agent.dll.
int g_module_anchor = 0;

std::wstring module_file_name(HMODULE module) {
    return proc::read_growing_string([module](wchar_t* buffer, DWORD capacity) {
        return GetModuleFileNameW(module, buffer, capacity);
    });
}

std::wstring directory_of(const std::wstring& path) {
    return std::wstring(path::parent_directory(std::wstring_view(path)));
}

std::vector<ipc::ModuleEntry> to_entries(const std::vector<proc::ModuleInfo>& modules) {
    std::vector<ipc::ModuleEntry> entries;
    entries.reserve(modules.size());
    for (const proc::ModuleInfo& module : modules) {
        entries.push_back(ipc::ModuleEntry{module.base, module.size, module.path});
    }
    return entries;
}

// Copies `text` into the fixed reply buffer, cutting at a UTF-8 boundary if it has to truncate.
void fill_reply(cml_reply* reply, std::uint32_t status, const std::string& text) {
    const std::size_t n = ipc::utf8_cut_point(text, CML_REPLY_MESSAGE_CAPACITY - 1);
    std::memcpy(reply->message, text.data(), n);
    reply->message[n] = '\0';
    reply->message_length = static_cast<std::uint32_t>(n);
    reply->status = static_cast<cml_status>(status);
}

}  // namespace

Agent::Agent(std::uint32_t mode) : mode_(mode) {}

Agent::~Agent() {
    stop();
}

Result<void> Agent::start() {
    install_logging();

    Result<std::string> pipe = ipc::resolve_pipe_name(std::string());
    if (!pipe) {
        return pipe.error();
    }
    pipe_name_ = *pipe;

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&g_module_anchor), &self);
    const std::wstring agent_file = self != nullptr ? module_file_name(self) : std::wstring();
    agent_path_ = proc::utf8_from_wide(agent_file);
    std::wstring override_path;
    if (const std::optional<std::string> env = proc::get_env("CML_BROKER_PATH"); env && !env->empty()) {
        override_path = proc::wide_from_utf8(*env);
    }
    broker_path_ = broker_path_for(override_path, directory_of(agent_file));
    if (const std::optional<std::string> env = proc::get_env("CML_BROKER_ARGS"); env) {
        broker_args_ = *env;
    }
    command_line_ = proc::current_command_line();

    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (stop_event_ == nullptr || wake_event_ == nullptr) {
        return Diag::make(ids::kAgentCreateFailed, "cannot create the agent's synchronisation events",
                          "Close other programs to free system resources and retry.");
    }

    set_log_forwarding(true);
    logs_forwarded_ = true;
    CML_LOG(Info, "agent starting: mode=%s pipe=%s broker=%s", synchronized() ? "synchronized" : "immediate",
            pipe_name_.c_str(), proc::utf8_from_wide(broker_path_).c_str());

    const std::uintptr_t handle = _beginthreadex(nullptr, 0, &Agent::thread_entry, this, 0, nullptr);
    if (handle == 0) {
        return Diag::make(ids::kAgentCreateFailed, "cannot create the agent thread",
                          "Close other programs to free system resources and retry.");
    }
    thread_ = reinterpret_cast<HANDLE>(handle);
    return {};
}

void Agent::stop() {
    std::lock_guard<std::mutex> serialise(stop_mutex_);
    stopping_.store(true, std::memory_order_release);
    if (stop_event_ != nullptr) {
        SetEvent(stop_event_);
    }
    {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (active_connection_ != nullptr) {
            active_connection_->cancel();
        }
    }
    if (thread_ != nullptr) {
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (logs_forwarded_) {
        set_log_forwarding(false);
        logs_forwarded_ = false;
    }
    // Wake every waiter: whatever was still open will never be answered.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::uint64_t> open;
        for (const auto& [id, entry] : entries_) {
            if (entry.state != State::Done) {
                open.push_back(id);
            }
        }
        for (const std::uint64_t id : open) {
            const auto it = entries_.find(id);
            finish_locked(it, ids::kRequestCancelled, "the agent was destroyed before the broker answered", false);
        }
        outgoing_.clear();
    }
    done_.notify_all();
    for (HANDLE* event : {&stop_event_, &wake_event_}) {
        if (*event != nullptr) {
            CloseHandle(*event);
            *event = nullptr;
        }
    }
}

// --- application-facing calls -------------------------------------------------------------------

cml_status Agent::enqueue(std::uint64_t id, ipc::Message message, bool wanted) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_.load(std::memory_order_acquire)) {
            return static_cast<cml_status>(ids::kRequestCancelled);
        }
        // Answers nobody collected must never lock new requests out: make room by evicting the
        // oldest of them. Only genuinely open requests can fill the table.
        while (entries_.size() >= kMaxOutstanding + kReenableReserve && !done_order_.empty()) {
            entries_.erase(done_order_.front());
            done_order_.pop_front();
        }
        if (entries_.size() >= kMaxOutstanding + kReenableReserve) {
            return static_cast<cml_status>(ids::kRequestQueueFull);
        }
        Entry entry;
        entry.message = std::move(message);
        entry.wanted = wanted;
        entry.seq = queue_seq_.fetch_add(1, std::memory_order_acq_rel) + 1;
        entries_.emplace(id, std::move(entry));
        outgoing_.push_back(id);
    }
    SetEvent(wake_event_);
    return CML_STATUS_OK;
}

template <class Build>
cml_status Agent::submit_path_request(const wchar_t* path, std::uint64_t* request_id, Build build) {
    std::string utf8 = proc::utf8_from_wide(path, std::wcslen(path));
    if (utf8.empty() || utf8.size() > ipc::kMaxStringBytes) {
        return CML_STATUS_INVALID_ARGUMENT;
    }
    const std::uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    // Without a request_id out-parameter nobody can ask for the answer, so none is kept.
    const cml_status status = enqueue(id, build(id, std::move(utf8)), request_id != nullptr);
    if (status == CML_STATUS_OK && request_id != nullptr) {
        *request_id = id;
    }
    return status;
}

cml_status Agent::enable_module(const wchar_t* path, std::uint32_t options, std::uint64_t* request_id) {
    return submit_path_request(path, request_id, [options](std::uint64_t id, std::string utf8) {
        ipc::EnableModule request;
        request.request_id = id;
        request.options = options;
        request.path = std::move(utf8);
        return ipc::Message(std::move(request));
    });
}

cml_status Agent::disable_module(const wchar_t* path, std::uint64_t* request_id) {
    return submit_path_request(path, request_id, [](std::uint64_t id, std::string utf8) {
        ipc::DisableModule request;
        request.request_id = id;
        request.path = std::move(utf8);
        return ipc::Message(std::move(request));
    });
}

void Agent::erase_done_locked(std::uint64_t id) {
    entries_.erase(id);
    const auto it = std::find(done_order_.begin(), done_order_.end(), id);
    if (it != done_order_.end()) {
        done_order_.erase(it);
    }
}

cml_status Agent::wait_request(std::uint64_t request_id, std::uint32_t timeout_ms, cml_reply* reply) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto lookup = [&]() -> std::map<std::uint64_t, Entry>::iterator {
        const auto it = entries_.find(request_id);
        return (it == entries_.end() || it->second.internal) ? entries_.end() : it;
    };
    auto is_done = [&] {
        const auto it = lookup();
        return it == entries_.end() || it->second.state == State::Done;
    };
    if (lookup() == entries_.end()) {
        return CML_STATUS_UNKNOWN_REQUEST;
    }
    if (!is_done()) {
        if (timeout_ms == 0) {
            return CML_STATUS_TIMEOUT;
        }
        if (timeout_ms == 0xFFFFFFFFu) {
            done_.wait(lock, is_done);
        } else if (!done_.wait_for(lock, std::chrono::milliseconds(timeout_ms), is_done)) {
            return CML_STATUS_TIMEOUT;
        }
    }
    const auto it = lookup();
    if (it == entries_.end()) {
        return CML_STATUS_UNKNOWN_REQUEST;  // consumed by another waiter or evicted meanwhile
    }
    fill_reply(reply, it->second.status, it->second.text);
    erase_done_locked(request_id);
    return CML_STATUS_OK;
}

void Agent::sync_point() {
    // Hot path of the application's frame loop: one atomic load when idle.
    if (mode_ == CML_MODE_SYNCHRONIZED && sync_queue_.has_work()) {
        sync_queue_.run_pending();
    }
}

bool Agent::post_main_thread_work(SyncQueue::WorkFn work, void* context) {
    if (work == nullptr) {
        return false;
    }
    if (synchronized()) {
        return sync_queue_.post(work, context);
    }
    work(context);
    return true;
}

// --- agent thread -------------------------------------------------------------------------------

unsigned __stdcall Agent::thread_entry(void* self) {
    static_cast<Agent*>(self)->run();
    return 0;
}

void Agent::register_connection(ipc::PipeConnection* connection) {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    active_connection_ = connection;
}

bool Agent::try_connect(ipc::PipeConnection& out, ConnectState& state) {
    if (ipc::pipe_exists(pipe_name_)) {
        Result<ipc::PipeConnection> connection = ipc::connect_pipe(pipe_name_, 1000);
        if (connection) {
            out = std::move(*connection);
            return true;
        }
        if (state.failures++ == 0) {
            CML_LOG(Warn, "%s", connection.error().format().c_str());
        } else {
            CML_LOG(Debug, "%s", connection.error().format().c_str());
        }
    } else {
        if (!state.launched) {
            state.launched = true;
            CML_LOG(Info, "no broker is listening on %s; starting %s", pipe_name_.c_str(),
                    proc::utf8_from_wide(broker_path_).c_str());
            Result<void> launched = launch_broker(broker_path_, pipe_name_, broker_args_);
            if (!launched) {
                CML_LOG(Warn, "%s", launched.error().format().c_str());
            }
        } else if (state.failures++ == 0) {
            CML_LOG(Warn, "still no broker on %s; will keep retrying in the background",
                    pipe_name_.c_str());
        }
    }
    WaitForSingleObject(stop_event_, state.backoff_ms);
    state.backoff_ms = std::min(state.backoff_ms * 2, kMaxBackoffMs);
    return false;
}

void Agent::run() {
    CML_LOG(Debug, "agent thread running");
    ConnectState state;
    while (!stopping_.load(std::memory_order_acquire)) {
        ipc::PipeConnection connection;
        if (!try_connect(connection, state)) {
            continue;
        }
        CML_LOG(Info, "connected to the broker on %s (pid %u)", pipe_name_.c_str(),
                connection.peer_process_id());
        const bool established = run_session(connection);
        if (established) {
            state = ConnectState();  // the next connection cycle may start the broker again
        } else {
            state.backoff_ms = std::min(state.backoff_ms * 2, kMaxBackoffMs);
        }
        // Never hammer a broker that refuses or drops us: always pause before the next attempt.
        WaitForSingleObject(stop_event_, established ? kReconnectPauseMs : state.backoff_ms);
    }
    CML_LOG(Debug, "agent thread exiting");
}

bool Agent::run_session(ipc::PipeConnection& connection) {
    register_connection(&connection);
    if (stopping_.load(std::memory_order_acquire)) {
        connection.cancel();  // stop() ran before the connection was registered
    }
    known_modules_.clear();
    last_module_poll_ = GetTickCount64();
    const bool established = handshake(connection) && send_module_list(connection);
    if (established) {
        queue_reenables();
        connected_.store(true, std::memory_order_release);
        serve(connection);
    }
    connected_.store(false, std::memory_order_release);
    register_connection(nullptr);
    connection.close();
    if (!stopping_.load(std::memory_order_acquire)) {
        CML_LOG(Info, "disconnected from the broker; will reconnect");
    }
    fail_in_flight(ids::kConnectionLost, "the connection to the broker was lost before it answered");
    return established;
}

bool Agent::send_message(ipc::PipeConnection& connection, const ipc::Message& message) {
    CML_LOG(Debug, "-> %s", ipc::describe(message).c_str());
    Result<ipc::IoStatus> sent = connection.send(message, kSendTimeoutMs);
    if (!sent) {
        CML_LOG(Warn, "cannot send %s: %s", ipc::to_string(ipc::type_of(message)),
                sent.error().format().c_str());
        return false;
    }
    switch (*sent) {
        case ipc::IoStatus::Ok:
            return true;
        case ipc::IoStatus::Timeout:
            CML_LOG(Warn, "the broker did not accept %s within %lu ms; dropping the connection",
                    ipc::to_string(ipc::type_of(message)), static_cast<unsigned long>(kSendTimeoutMs));
            return false;
        case ipc::IoStatus::Closed:
            CML_LOG(Info, "the broker closed the connection");
            return false;
        case ipc::IoStatus::Cancelled:
        case ipc::IoStatus::Woken:
        case ipc::IoStatus::Rejected:
            return false;
    }
    return false;
}

bool Agent::handshake(ipc::PipeConnection& connection) {
    ipc::Hello hello;
    hello.request_id = next_id_.fetch_add(1, std::memory_order_relaxed);
    hello.agent_abi_version = CML_AGENT_ABI_VERSION;
    hello.pid = proc::current_process_id();
    hello.mode = synchronized() ? ipc::AgentMode::Synchronized : ipc::AgentMode::Immediate;
    hello.agent_path = agent_path_;
    hello.command_line = command_line_;
    if (!send_message(connection, hello)) {
        return false;
    }

    const ULONGLONG deadline = GetTickCount64() + kHandshakeTimeoutMs;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            CML_LOG(Warn, "CML%u: the broker did not answer Hello within %lu ms",
                    ids::kHandshakeFailed, static_cast<unsigned long>(kHandshakeTimeoutMs));
            return false;
        }
        Result<ipc::PipeConnection::Received> received =
            connection.receive(static_cast<std::uint32_t>(deadline - now));
        if (!received) {
            CML_LOG(Warn, "%s", received.error().format().c_str());
            return false;
        }
        switch (received->status) {
            case ipc::IoStatus::Ok:
                break;
            case ipc::IoStatus::Timeout:
            case ipc::IoStatus::Woken:
                continue;
            case ipc::IoStatus::Closed:
                CML_LOG(Warn, "CML%u: the broker closed the connection during the handshake",
                        ids::kHandshakeFailed);
                return false;
            case ipc::IoStatus::Rejected:
                CML_LOG(Warn, "CML%u: unusable answer to Hello: %s", ids::kHandshakeFailed,
                        received->error.format().c_str());
                return false;
            case ipc::IoStatus::Cancelled:
                return false;
        }
        if (const auto* welcome = std::get_if<ipc::Welcome>(&received->message)) {
            if (welcome->request_id != hello.request_id ||
                welcome->protocol_version != ipc::kProtocolVersion) {
                CML_LOG(Warn, "CML%u: unexpected Welcome (request %llu, protocol %u)",
                        ids::kHandshakeFailed, static_cast<unsigned long long>(welcome->request_id),
                        welcome->protocol_version);
                return false;
            }
            CML_LOG(Info, "registered with broker %s as agent #%llu", welcome->broker_version.c_str(),
                    static_cast<unsigned long long>(welcome->agent_id));
            return true;
        }
        if (const auto* reply = std::get_if<ipc::Reply>(&received->message)) {
            CML_LOG(Warn, "CML%u: the broker refused Hello: CML%u %s", ids::kHandshakeFailed,
                    reply->status, reply->message.c_str());
            return false;
        }
        CML_LOG(Warn, "CML%u: expected Welcome, got %s", ids::kHandshakeFailed,
                ipc::to_string(ipc::type_of(received->message)));
        return false;
    }
}

bool Agent::send_module_list(ipc::PipeConnection& connection) {
    ipc::ModuleList list;
    list.request_id = next_id_.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t seq_mark = queue_seq_.load(std::memory_order_acquire);
    Result<std::vector<proc::ModuleInfo>> modules = proc::enumerate_modules(0);
    if (!modules) {
        CML_LOG(Warn, "%s", modules.error().format().c_str());
    } else {
        list.modules = to_entries(*modules);
    }
    {
        // Register before sending so the Reply cannot race the registration.
        Entry entry;
        entry.internal = true;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto inserted = entries_.emplace(list.request_id, std::move(entry));
        note_sent_locked(inserted.first->second);
    }
    if (!send_message(connection, list)) {
        return false;
    }
    known_modules_ = list.modules;
    last_module_poll_ = GetTickCount64();
    polled_seq_ = seq_mark;
    return true;
}

bool Agent::flush_outgoing(ipc::PipeConnection& connection) {
    for (;;) {
        std::uint64_t id = 0;
        std::uint64_t seq = 0;
        ipc::Message message;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (outgoing_.empty()) {
                return true;
            }
            // Leave the rest queued while a full window of requests awaits answers; serve() reads
            // replies and calls us again.
            if (sent_count_ >= kMaxInFlight) {
                return true;
            }
            id = outgoing_.front();
            outgoing_.pop_front();
            const auto it = entries_.find(id);
            if (it == entries_.end()) {
                continue;
            }
            message = it->second.message;
            seq = it->second.seq;
        }
        // The broker checks EnableModule against the module list it has, so it must have seen
        // every module that was loaded before the request was made. One poll serves all the
        // requests queued before it started; a request queued after the last poll needs a new one.
        bool ok = true;
        if (seq > polled_seq_) {
            ok = poll_modules(connection);
        }
        ok = ok && send_message(connection, message);
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(id);
        if (!ok) {
            if (it != entries_.end()) {
                outgoing_.push_front(id);  // still Queued: goes out again after reconnecting
            }
            return false;
        }
        if (it != entries_.end()) {
            note_sent_locked(it->second);
        }
    }
}

bool Agent::flush_logs(ipc::PipeConnection& connection) {
    for (;;) {
        std::vector<ipc::Log> logs = take_forwarded_logs(kMaxLogsPerPass);
        if (logs.empty()) {
            return true;
        }
        for (const ipc::Log& log : logs) {
            if (!send_message(connection, log)) {
                return false;
            }
        }
    }
}

bool Agent::poll_modules(ipc::PipeConnection& connection) {
    last_module_poll_ = GetTickCount64();
    // Everything queued before this point was queued after the modules it refers to were loaded,
    // so the snapshot taken now covers it.
    const std::uint64_t seq_mark = queue_seq_.load(std::memory_order_acquire);
    Result<std::vector<proc::ModuleInfo>> modules = proc::enumerate_modules(0);
    if (!modules) {
        CML_LOG(Debug, "%s", modules.error().format().c_str());
        return true;  // a failed snapshot is not a connection problem; try again next time
    }
    std::vector<ipc::ModuleEntry> current = to_entries(*modules);
    const ModuleDelta delta = diff_modules(known_modules_, current);
    for (const ipc::ModuleEntry& module : delta.unloaded) {
        if (!send_message(connection, ipc::ModuleUnloaded{module})) {
            return false;
        }
    }
    for (const ipc::ModuleEntry& module : delta.loaded) {
        if (!send_message(connection, ipc::ModuleLoaded{module})) {
            return false;
        }
    }
    known_modules_ = std::move(current);
    polled_seq_ = seq_mark;
    return true;
}

bool Agent::same_module(const std::string& a, const std::string& b) {
    return path::matches_module(a, b) || path::matches_module(b, a);
}

void Agent::note_sent_locked(Entry& entry) {
    entry.state = State::Sent;
    ++sent_count_;
}

void Agent::track_enabled_locked(const Entry& entry, std::uint32_t status) {
    if (const auto* enable = std::get_if<ipc::EnableModule>(&entry.message)) {
        const auto same = [enable](const EnabledModule& m) { return same_module(m.path, enable->path); };
        if (status == 0) {
            const auto existing = std::find_if(enabled_.begin(), enabled_.end(), same);
            if (existing != enabled_.end()) {
                existing->options = enable->options;  // a later enable may change the options
            } else if (enabled_.size() < kMaxTrackedModules) {
                enabled_.push_back(EnabledModule{enable->path, enable->options});
            }
        } else if (entry.internal) {
            // Re-enabling after a reconnect failed (the module is gone): stop tracking it.
            enabled_.erase(std::remove_if(enabled_.begin(), enabled_.end(), same), enabled_.end());
        }
    } else if (const auto* disable = std::get_if<ipc::DisableModule>(&entry.message)) {
        if (status == 0) {
            enabled_.erase(std::remove_if(enabled_.begin(), enabled_.end(),
                                          [disable](const EnabledModule& m) { return same_module(m.path, disable->path); }),
                           enabled_.end());
        }
    }
}

// Ends an open request. A request whose answer is wanted becomes Done and waits to be collected
// (bounded; the oldest uncollected answers are dropped); internal ones and ones created without a
// request id are simply forgotten.
void Agent::finish_locked(std::map<std::uint64_t, Entry>::iterator it, std::uint32_t status, const char* text,
                          bool answered_by_broker) {
    Entry& entry = it->second;
    if (entry.state == State::Sent) {
        --sent_count_;
    }
    if (answered_by_broker) {
        track_enabled_locked(entry, status);  // a lost connection says nothing about the module
    }
    if (entry.internal || !entry.wanted) {
        entries_.erase(it);
        return;
    }
    entry.state = State::Done;
    entry.status = status;
    entry.text = text;
    done_order_.push_back(it->first);
    while (done_order_.size() > kMaxUnconsumedAnswers) {
        entries_.erase(done_order_.front());
        done_order_.pop_front();
    }
}

// After a reconnect the new broker session knows nothing of what the application enabled. Queue
// an EnableModule for each tracked module ahead of everything else that is waiting. Re-enables
// left over from the previous session (still queued, or sent and unanswered) are dropped first, so
// reconnecting repeatedly never piles up duplicates.
void Agent::queue_reenables() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.internal && std::holds_alternative<ipc::EnableModule>(it->second.message)) {
            if (it->second.state == State::Sent) {
                --sent_count_;
            }
            outgoing_.erase(std::remove(outgoing_.begin(), outgoing_.end(), it->first), outgoing_.end());
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = enabled_.rbegin(); it != enabled_.rend(); ++it) {
        ipc::EnableModule request;
        request.request_id = next_id_.fetch_add(1, std::memory_order_relaxed);
        request.options = it->options;
        request.path = it->path;
        Entry entry;
        entry.internal = true;  // seq stays 0: no module poll needed, they refer to known modules
        entry.message = request;
        entries_.emplace(request.request_id, std::move(entry));
        outgoing_.push_front(request.request_id);
    }
    if (!enabled_.empty()) {
        CML_LOG(Info, "re-enabling %llu module(s) in the new broker session",
                static_cast<unsigned long long>(enabled_.size()));
    }
}

void Agent::complete(std::uint64_t request_id, std::uint32_t status, std::string text) {
    bool known = false;
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(request_id);
        if (it != entries_.end() && it->second.state != State::Done) {
            known = true;
            wake = !it->second.internal && it->second.wanted;
            finish_locked(it, status, text.c_str(), true);
        }
    }
    if (!known) {
        CML_LOG(Warn, "reply for unknown request %llu ignored", static_cast<unsigned long long>(request_id));
        return;
    }
    if (status != 0) {
        CML_LOG(Warn, "request %llu failed: CML%u %s", static_cast<unsigned long long>(request_id), status,
                text.c_str());
    } else {
        CML_LOG(Debug, "request %llu answered: %s", static_cast<unsigned long long>(request_id), text.c_str());
    }
    if (wake) {
        done_.notify_all();
    }
}

void Agent::fail_in_flight(std::uint32_t status, const char* text) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::uint64_t> sent;
        for (const auto& [id, entry] : entries_) {
            if (entry.state == State::Sent) {
                sent.push_back(id);
            }
        }
        for (const std::uint64_t id : sent) {
            finish_locked(entries_.find(id), status, text, false);
        }
    }
    done_.notify_all();
}

void Agent::handle_message(const ipc::Message& message) {
    CML_LOG(Debug, "<- %s", ipc::describe(message).c_str());
    if (const auto* reply = std::get_if<ipc::Reply>(&message)) {
        complete(reply->request_id, reply->status, reply->message);
        return;
    }
    CML_LOG(Warn, "ignoring unexpected %s from the broker", ipc::to_string(ipc::type_of(message)));
}

void Agent::serve(ipc::PipeConnection& connection) {
    while (!stopping_.load(std::memory_order_acquire)) {
        if (!flush_outgoing(connection) || !flush_logs(connection)) {
            return;
        }
        if (GetTickCount64() - last_module_poll_ >= kModulePollMs && !poll_modules(connection)) {
            return;
        }
        Result<ipc::PipeConnection::Received> received = connection.receive(kReceiveSliceMs, wake_event_);
        if (!received) {
            CML_LOG(Warn, "%s", received.error().format().c_str());
            return;
        }
        switch (received->status) {
            case ipc::IoStatus::Ok:
                handle_message(received->message);
                break;
            case ipc::IoStatus::Rejected:
                // An answer we cannot decode would leave its request waiting for ever (and hold an
                // in-flight slot): drop the connection, which fails everything in flight and
                // reconnects. Anything else (a stray event) can be skipped.
                if (received->raw_type == static_cast<std::uint16_t>(ipc::MessageType::Reply) ||
                    received->raw_type == static_cast<std::uint16_t>(ipc::MessageType::Welcome)) {
                    CML_LOG(Warn, "an undecodable answer from the broker (%s); dropping the connection",
                            received->error.format().c_str());
                    return;
                }
                CML_LOG(Warn, "dropped one message from the broker: %s", received->error.format().c_str());
                break;
            case ipc::IoStatus::Timeout:
            case ipc::IoStatus::Woken:
                break;
            case ipc::IoStatus::Closed:
                CML_LOG(Info, "the broker closed the connection");
                return;
            case ipc::IoStatus::Cancelled:
                return;
        }
    }
}

}  // namespace cml::agent

#endif  // _WIN32
