#include "agent.h"

#if defined(_WIN32)

#include <process.h>

#include <algorithm>
#include <atomic>
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
#include "hook/installer.h"
#include "module_diff.h"

namespace cml::agent {

namespace {

constexpr DWORD kSendTimeoutMs = 5000;
constexpr DWORD kHandshakeTimeoutMs = 5000;
constexpr ULONGLONG kModulePollMs = 500;
constexpr DWORD kReceiveSliceMs = 250;       // longest the agent thread sleeps without re-checking
// The request table (see the invariants in agent.h):
//  * at most kMaxOutstanding application requests are open (not yet answered) at a time;
//  * answers nobody has collected yet are kept, oldest evicted first, only up to
//    kMaxUnconsumedAnswers and never while a thread is waiting for them;
//  * the internal re-enables after a reconnect are outside both limits (one per enabled module).
constexpr std::size_t kMaxOutstanding = 1024;
constexpr std::size_t kMaxUnconsumedAnswers = 512;
constexpr std::size_t kMaxLogsPerPass = 16;
// Requests sent but not yet answered. Bounded so that the agent keeps reading replies while it
// writes: with an unbounded burst both pipe buffers could fill up and stall both ends.
constexpr std::size_t kMaxInFlight = 16;
constexpr std::uint32_t kMaxBackoffMs = 1000;
constexpr int kEnumerateAttempts = 3;        // tries to list the loaded modules before sending what we have
constexpr DWORD kEnumerateRetryMs = 20;
constexpr DWORD kReconnectPauseMs = 200;   // pause after a session that was up, before reconnecting

std::atomic<int> g_enumeration_failures{0};

Result<std::vector<proc::ModuleInfo>> enumerate_loaded_modules() {
    if (g_enumeration_failures.load() > 0 && g_enumeration_failures.fetch_sub(1) > 0) {
        return Diag::make(ids::kModuleEnumFailed, "module enumeration failed (injected failure)",
                          "Test hook: set_enumeration_failures_for_testing.");
    }
    return proc::enumerate_modules(0);
}

constexpr DWORD kMaxLaunchBackoffMs = 60000;   // broker auto-start back-off cap
constexpr DWORD kMaxListBackoffMs = 30000;     // full module list resend back-off cap

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

void set_enumeration_failures_for_testing(int count) {
    g_enumeration_failures.store(count);
}

int& Agent::CallGuard::depth() noexcept {
    thread_local int depth = 0;
    return depth;
}

bool Agent::in_guarded_call() noexcept {
    return CallGuard::depth() > 0;
}

Agent::Agent(std::uint32_t mode) : mode_(mode) {}

Agent::~Agent() {
    stop();
    // The events are closed only here, never in stop(): enqueue() signals wake_event_ after it has
    // released the lock, and the public-call guard keeps this destructor from running until every
    // such caller has left.
    for (HANDLE* event : {&stop_event_, &wake_event_}) {
        if (*event != nullptr) {
            CloseHandle(*event);
            *event = nullptr;
        }
    }
}

void Agent::stop_and_wait_for_calls() {
    stop();  // wakes every waiter; later calls see stopping_ and return at once
    while (active_calls_.load(std::memory_order_acquire) != 0) {
        Sleep(1);
    }
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
            // Everything open becomes "cancelled" and stays collectable: no eviction here, so every
            // waiter, and every later wait_request, sees kRequestCancelled.
            finish_locked(it, ids::kRequestCancelled, "the agent was destroyed before the broker answered",
                          /*answered_by_broker=*/false, /*evict=*/false);
        }
        outgoing_.clear();
    }
    done_.notify_all();
}

// --- application-facing calls -------------------------------------------------------------------

cml_status Agent::enqueue(std::uint64_t id, ipc::Message message, bool wanted) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_.load(std::memory_order_acquire)) {
            return static_cast<cml_status>(ids::kRequestCancelled);
        }
        // The limit counts open application requests only. Answers waiting to be collected and the
        // internal re-enables never use it up.
        if (open_app_ >= kMaxOutstanding) {
            return static_cast<cml_status>(ids::kRequestQueueFull);
        }
        Entry entry;
        entry.message = std::move(message);
        entry.wanted = wanted;
        entry.seq = queue_seq_.fetch_add(1, std::memory_order_acq_rel) + 1;
        entries_.emplace(id, std::move(entry));
        ++open_app_;
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
    // While this thread is inside wait_request the entry has a waiter and is never evicted, even
    // after it has been answered and before this thread has picked the answer up.
    ++lookup()->second.waiters;
    struct WaiterGuard {
        Agent& agent;
        std::uint64_t id;
        ~WaiterGuard() {  // runs with the lock still held (declared after it)
            const auto it = agent.entries_.find(id);
            if (it != agent.entries_.end() && it->second.waiters > 0) {
                --it->second.waiters;
            }
        }
    } guard{*this, request_id};
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
        return CML_STATUS_UNKNOWN_REQUEST;  // consumed by another waiter meanwhile
    }
    fill_reply(reply, it->second.status, it->second.text);
    erase_done_locked(request_id);
    return CML_STATUS_OK;
}

void Agent::complete_for_testing(std::uint64_t request_id, std::uint32_t status, const std::string& text) {
    complete(request_id, status, text);
}

std::size_t Agent::waiters_for_testing() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t total = 0;
    for (const auto& [id, entry] : entries_) {
        total += static_cast<std::size_t>(entry.waiters);
    }
    return total;
}

std::size_t Agent::tracked_modules_for_testing() {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_.size();
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
        if (!state.launched && GetTickCount64() >= next_launch_at_) {
            state.launched = true;
            // Launches back off exponentially (1 s, 2 s, 4 s ... 60 s); the back-off is reset only
            // when a session got its ModuleList accepted (note_list_accepted()).
            next_launch_at_ = GetTickCount64() + launch_backoff_ms_;
            launch_backoff_ms_ = std::min(launch_backoff_ms_ * 2, kMaxLaunchBackoffMs);
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
        // Whatever ended the session, a broker that is gone afterwards gets a fresh auto-start.
        state.launched = false;
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
    polled_seq_ = 0;  // a new broker knows no modules: every queued request polls first
    full_list_pending_ = false;
    const bool established = handshake(connection) && send_module_list(connection);
    if (established) {
        queue_reenables(false);
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
    Result<std::vector<proc::ModuleInfo>> modules = enumerate_loaded_modules();
    for (int attempt = 1; !modules && attempt < kEnumerateAttempts; ++attempt) {
        CML_LOG(Debug, "%s", modules.error().format().c_str());
        if (WaitForSingleObject(stop_event_, kEnumerateRetryMs) == WAIT_OBJECT_0) {
            break;
        }
        modules = enumerate_loaded_modules();
    }
    if (modules) {
        list.modules = to_entries(*modules);
    } else {
        // Send what we have (nothing, at the start of a session) and keep the session: polled_seq_
        // stays as it is, so the next flush polls again, and the next poll sends the difference.
        CML_LOG(Warn, "cannot enumerate the loaded modules (%s); sending the list we have",
                modules.error().format().c_str());
        list.modules = known_modules_;
    }
    {
        // Register before sending so the Reply cannot race the registration.
        Entry entry;
        entry.internal = true;
        entry.module_list = true;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto inserted = entries_.emplace(list.request_id, std::move(entry));
        note_sent_locked(inserted.first->second);
    }
    full_list_pending_ = false;
    if (!modules) {
        // Ask for a full list again later (rate-limited like a refusal), so that the broker ends up
        // with the real module list instead of only the differences.
        full_list_pending_ = true;
        full_list_not_before_ = GetTickCount64() + list_backoff_ms_;
        list_backoff_ms_ = std::min(list_backoff_ms_ * 2, kMaxListBackoffMs);
    }
    if (!send_message(connection, list)) {
        return false;
    }
    known_modules_ = list.modules;
    last_module_poll_ = GetTickCount64();
    if (modules) {
        polled_seq_ = seq_mark;
    }
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
    if (full_list_pending_ && GetTickCount64() >= full_list_not_before_) {
        return send_module_list(connection);  // the broker refused (or we could not build) the last full list
    }
    // Everything queued before this point was queued after the modules it refers to were loaded,
    // so the snapshot taken now covers it.
    const std::uint64_t seq_mark = queue_seq_.load(std::memory_order_acquire);
    Result<std::vector<proc::ModuleInfo>> modules = enumerate_loaded_modules();
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
                existing->needs_reenable = false;
            } else {
                // No cap: the set is bounded by the modules the broker accepted, i.e. by the
                // modules that are loaded in this process.
                enabled_.push_back(EnabledModule{enable->path, enable->options});
            }
        } else if (entry.internal) {
            // Re-enabling after a reconnect failed (the module is gone): stop tracking it.
            // Only the application's own DisableModule removes a module from the set. A refused
            // re-enable marks it; it is sent again after the next accepted ModuleList.
            const auto existing = std::find_if(enabled_.begin(), enabled_.end(), same);
            if (existing != enabled_.end()) {
                existing->needs_reenable = true;
            }
        }
    } else if (const auto* disable = std::get_if<ipc::DisableModule>(&entry.message)) {
        if (status == 0) {
            enabled_.erase(std::remove_if(enabled_.begin(), enabled_.end(),
                                          [disable](const EnabledModule& m) { return same_module(m.path, disable->path); }),
                           enabled_.end());
        }
    }
}

// Ends an open request. A request whose answer is wanted becomes Done and waits to be collected;
// internal ones and ones created without a request id are simply forgotten. `evict` lets the
// oldest uncollected answers (Done, nobody waiting) go when more than kMaxUnconsumedAnswers pile
// up; stop() passes false so that cancellation never loses an answer.
void Agent::finish_locked(std::map<std::uint64_t, Entry>::iterator it, std::uint32_t status, const char* text,
                          bool answered_by_broker, bool evict) {
    Entry& entry = it->second;
    if (entry.state == State::Sent) {
        --sent_count_;
    }
    if (!entry.internal) {
        --open_app_;
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
    if (evict) {
        evict_oldest_answers_locked();
    }
}

void Agent::evict_oldest_answers_locked() {
    for (auto candidate = done_order_.begin(); done_order_.size() > kMaxUnconsumedAnswers && candidate != done_order_.end();) {
        const auto entry = entries_.find(*candidate);
        if (entry != entries_.end() && entry->second.waiters > 0) {
            ++candidate;  // someone is about to collect it: never evict
            continue;
        }
        entries_.erase(*candidate);
        candidate = done_order_.erase(candidate);
    }
}

// After a reconnect the new broker session knows nothing of what the application enabled. Queue
// an EnableModule for each tracked module ahead of everything else that is waiting. Re-enables
// left over from the previous session (still queued, or sent and unanswered) are dropped first, so
// reconnecting repeatedly never piles up duplicates.
void Agent::queue_reenables(bool only_flagged) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); !only_flagged && it != entries_.end();) {
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
    std::size_t queued = 0;
    for (auto it = enabled_.rbegin(); it != enabled_.rend(); ++it) {
        if (only_flagged && !it->needs_reenable) {
            continue;
        }
        it->needs_reenable = false;
        ++queued;
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
    if (queued != 0) {
        CML_LOG(Info, "re-enabling %llu module(s)%s", static_cast<unsigned long long>(queued),
                only_flagged ? " that were refused earlier" : " in the new broker session");
    }
}

// The broker accepted a ModuleList: it knows our modules now. Resets the back-offs and sends the
// re-enables it refused while it did not.
void Agent::note_list_accepted() {
    launch_backoff_ms_ = 1000;
    next_launch_at_ = 0;
    list_backoff_ms_ = 1000;
    full_list_not_before_ = 0;
    queue_reenables(true);
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
            finish_locked(it, status, text.c_str(), true, true);
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
            finish_locked(entries_.find(id), status, text, false, true);
        }
    }
    done_.notify_all();
}

void Agent::handle_message(const ipc::Message& message) {
    CML_LOG(Debug, "<- %s", ipc::describe(message).c_str());
    if (const auto* reply = std::get_if<ipc::Reply>(&message)) {
        bool is_list = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = entries_.find(reply->request_id);
            is_list = it != entries_.end() && it->second.module_list;
        }
        if (is_list && reply->status != 0) {
            // Keep the session; the next full list goes out after a growing pause. Requests queued
            // meanwhile go out normally and the broker answers them on their own merits.
            CML_LOG(Warn, "the broker refused the module list: CML%u %s; will send it again in %lu ms",
                    reply->status, reply->message.c_str(), static_cast<unsigned long>(list_backoff_ms_));
            full_list_pending_ = true;
            full_list_not_before_ = GetTickCount64() + list_backoff_ms_;
            list_backoff_ms_ = std::min(list_backoff_ms_ * 2, kMaxListBackoffMs);
            polled_seq_ = 0;
        }
        complete(reply->request_id, reply->status, reply->message);
        if (is_list && reply->status == 0) {
            note_list_accepted();
        }
        return;
    }
    if (const auto* install = std::get_if<ipc::InstallHooks>(&message)) {
        handle_install_hooks(*install);
        return;
    }
    CML_LOG(Warn, "ignoring unexpected %s from the broker", ipc::to_string(ipc::type_of(message)));
}

void Agent::serve(ipc::PipeConnection& connection) {
    while (!stopping_.load(std::memory_order_acquire)) {
        if (!flush_outgoing(connection) || !flush_replies(connection) || !flush_logs(connection)) {
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
                if (received->raw_type == static_cast<std::uint16_t>(ipc::MessageType::InstallHooks) &&
                    received->request_id.has_value()) {
                    // The broker waits for an answer: tell it the request was unusable.
                    ipc::HooksInstalled refusal;
                    refusal.request_id = *received->request_id;
                    refusal.status = received->error.code;
                    refusal.message = ipc::sanitize_utf8(received->error.format(), 1000);
                    queue_reply(std::move(refusal));
                    break;
                }
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
