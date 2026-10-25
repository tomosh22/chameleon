#pragma once

// The agent: one object per cml_create_*_agent() call, owning the agent thread that talks to the
// broker. See docs/wp/WP-11.md for the lifecycle; in short, the thread
//   1. connects to the broker pipe (auto-starting cml_broker.exe --headless when it is not
//      running), retrying with bounded back-off for as long as the agent lives;
//   2. sends Hello, receives Welcome, sends the module list;
//   3. serves queued requests (EnableModule / DisableModule) and reports module changes by
//      polling the module list;
//   4. on connection loss fails the requests that were in flight and starts over at 1.
// Application threads never wait for any of this: their calls only touch the request table.

#if defined(_WIN32)

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#if !defined(CML_AGENT_BUILD)
#error "agent sources are compiled with CML_AGENT_BUILD so Chameleon_API.h only provides the shared types"
#endif
#include "Chameleon_API.h"
#include "cml/ipc/messages.h"
#include "cml/ipc/pipe.h"
#include "cml/util/result.h"
#include "hook/installer.h"
#include "sync_queue.h"

namespace cml::agent {

// Test hook: the next `count` module enumerations made by the agent fail.
void set_enumeration_failures_for_testing(int count);

class Agent {
public:
    // `mode` is CML_MODE_IMMEDIATE or CML_MODE_SYNCHRONIZED.
    explicit Agent(std::uint32_t mode);
    ~Agent();  // calls stop()
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    // Reads the environment, creates the agent thread and returns. Does not wait for the broker.
    Result<void> start();

    // Stops the agent thread, disconnects, and fails every open request with CML5215. Safe from
    // any thread, idempotent; must not be called from DllMain (it joins the thread).
    void stop();

    // Queue a request for the agent thread. CML_STATUS_OK means queued; *request_id identifies it.
    cml_status enable_module(const wchar_t* path, std::uint32_t options, std::uint64_t* request_id);
    cml_status disable_module(const wchar_t* path, std::uint64_t* request_id);

    // See cml_wait_request() in Chameleon_API.h.
    cml_status wait_request(std::uint64_t request_id, std::uint32_t timeout_ms, cml_reply* reply);

    // See cml_sync_point(): runs work queued for the application's thread (synchronised mode).
    void sync_point();

    // Test hook (also the future entry point of the patch pipeline): queue `work` for the
    // application thread. Synchronised mode: it runs inside the next sync_point(). Immediate mode:
    // it runs right now on the calling thread. Returns false when `work` is null.
    bool post_main_thread_work(SyncQueue::WorkFn work, void* context);

    // Public-call guard. Every exported function that touches the Agent holds one for its whole
    // duration; destroy() waits until none is left, so the object is never freed under a thread
    // that is still inside it (e.g. a waiter being woken by stop()).
    class CallGuard {
    public:
        explicit CallGuard(Agent& agent) noexcept : agent_(agent) {
            agent_.active_calls_.fetch_add(1, std::memory_order_acq_rel);
            ++depth();
        }
        ~CallGuard() {
            --depth();
            agent_.active_calls_.fetch_sub(1, std::memory_order_acq_rel);
        }
        CallGuard(const CallGuard&) = delete;
        CallGuard& operator=(const CallGuard&) = delete;

        static int& depth() noexcept;  // guards held by the calling thread (all agents)

    private:
        Agent& agent_;
    };

    // True on a thread that is inside a guarded call (e.g. work run by SyncPoint): destroying the
    // agent from there is refused.
    static bool in_guarded_call() noexcept;

    // stop(), then wait until every thread has left every public call. Call this (not just the
    // destructor) before freeing an Agent that other threads may still be using.
    void stop_and_wait_for_calls();

    bool is_connected() const noexcept { return connected_.load(std::memory_order_acquire); }
    bool synchronized() const noexcept { return mode_ == CML_MODE_SYNCHRONIZED; }

    // Test hooks: inject an answer as if the broker had sent it, and count the threads currently
    // inside wait_request.
    void complete_for_testing(std::uint64_t request_id, std::uint32_t status, const std::string& text);
    std::size_t waiters_for_testing();
    std::size_t tracked_modules_for_testing();
    std::size_t active_calls_for_testing() const noexcept { return static_cast<std::size_t>(active_calls_.load()); }

private:
    // Request table invariants (all under mutex_):
    //  I1. entries_ holds every open request (Queued or Sent), every answered request whose answer
    //      has not been collected (Done), and the internal ones (ModuleList, re-enables).
    //  I2. open_app_ == number of non-Done application entries; enqueue() refuses (CML5217) at
    //      kMaxOutstanding. Done and internal entries do not count.
    //  I3. sent_count_ == number of entries in State::Sent; the flush window is kMaxInFlight.
    //  I4. done_order_ lists exactly the Done entries, oldest first. Only a Done entry with
    //      waiters == 0 may be evicted (oldest first, beyond kMaxUnconsumedAnswers); an entry with a
    //      thread inside wait_request is never evicted, nor is anything while stop() cancels.
    //  I5. Internal entries and entries created without a request id are erased when they finish.
    //  I6. A lost connection fails Sent entries but never changes the enabled-module set.
    enum class State { Queued, Sent, Done };

    struct Entry {
        State state = State::Queued;
        bool internal = false;  // Hello / ModuleList / re-enables: answered on the agent thread, never waited on
        bool module_list = false;  // the internal ModuleList request (its refusal is handled, see handle_message)
        bool wanted = true;     // the caller asked for the answer (passed a request id)
        std::uint64_t seq = 0;  // queue order; compared with the last module poll
        int waiters = 0;        // threads currently inside wait_request for this entry
        ipc::Message message;
        std::uint32_t status = 0;  // valid when Done
        std::string text;          // valid when Done
    };

    // Agent thread -------------------------------------------------------------------------
    static unsigned __stdcall thread_entry(void* self);
    void run();
    struct ConnectState {
        bool launched = false;       // broker auto-start already attempted in this cycle
        std::uint32_t failures = 0;  // consecutive failed attempts (for log noise control)
        std::uint32_t backoff_ms = 50;
    };
    bool try_connect(ipc::PipeConnection& out, ConnectState& state);
    bool run_session(ipc::PipeConnection& connection);  // true if it got as far as being registered
    bool handshake(ipc::PipeConnection& connection);
    bool send_module_list(ipc::PipeConnection& connection);
    void serve(ipc::PipeConnection& connection);
    bool flush_outgoing(ipc::PipeConnection& connection);
    bool flush_logs(ipc::PipeConnection& connection);
    bool poll_modules(ipc::PipeConnection& connection);
    bool send_message(ipc::PipeConnection& connection, const ipc::Message& message);
    void handle_message(const ipc::Message& message);
    void complete(std::uint64_t request_id, std::uint32_t status, std::string text);
    void fail_in_flight(std::uint32_t status, const char* text);
    void register_connection(ipc::PipeConnection* connection);

    // WP-10: broker -> agent InstallHooks. handle_install_hooks (agent thread) hands the request to
    // post_main_thread_work, so in synchronised mode the hooks are applied inside SyncPoint() on the
    // application's thread and in immediate mode right here; the answer (HooksInstalled) is queued
    // in replies_ and sent by the agent thread. See docs/wp/WP-10.md.
    void handle_install_hooks(const ipc::InstallHooks& request);
    static void run_hook_job(void* context);
    void apply_install_hooks(const ipc::InstallHooks& request);
    void queue_reply(ipc::Message message);
    bool flush_replies(ipc::PipeConnection& connection);

    // Any thread ------------------------------------------------------------------------------
    cml_status enqueue(std::uint64_t id, ipc::Message message, bool wanted);
    template <class Build>
    cml_status submit_path_request(const wchar_t* path, std::uint64_t* request_id, Build build);
    struct EnabledModule {
        std::string path;  // as the application spelled it
        std::uint32_t options;
        bool needs_reenable = false;  // a re-enable was refused: send it again after the next accepted ModuleList
    };
    static bool same_module(const std::string& a, const std::string& b);
    void track_enabled_locked(const Entry& entry, std::uint32_t status);
    void note_sent_locked(Entry& entry);
    void finish_locked(std::map<std::uint64_t, Entry>::iterator it, std::uint32_t status, const char* text,
                       bool answered_by_broker, bool evict);
    void evict_oldest_answers_locked();
    void queue_reenables(bool only_flagged);
    void note_list_accepted();
    void erase_done_locked(std::uint64_t id);

    const std::uint32_t mode_;
    SyncQueue sync_queue_;

    // Configuration, fixed by start().
    std::string pipe_name_;
    std::wstring broker_path_;
    std::string broker_args_;
    std::string agent_path_;     // UTF-8, for Hello
    std::string command_line_;   // UTF-8, for Hello

    HANDLE thread_ = nullptr;
    HANDLE stop_event_ = nullptr;  // manual-reset: stop() was called
    HANDLE wake_event_ = nullptr;  // auto-reset: new work for the agent thread
    std::mutex stop_mutex_;        // serialises stop()
    std::atomic<bool> stopping_{false};
    std::atomic<int> active_calls_{0};  // threads inside a public call (see CallGuard)
    std::atomic<bool> connected_{false};
    bool logs_forwarded_ = false;

    std::atomic<std::uint64_t> next_id_{1};
    std::atomic<std::uint64_t> queue_seq_{0};  // sequence number of the last queued request
    std::mutex mutex_;  // guards entries_, outgoing_, done_order_
    std::condition_variable done_;
    std::map<std::uint64_t, Entry> entries_;
    std::deque<std::uint64_t> outgoing_;    // ids in State::Queued, in send order
    std::deque<std::uint64_t> done_order_;  // ids in State::Done, oldest first (bounded)
    std::size_t sent_count_ = 0;            // entries in State::Sent (the in-flight window)
    std::size_t open_app_ = 0;              // application entries not yet Done (the 1024 limit)
    std::vector<EnabledModule> enabled_;    // modules the broker accepted and that were not disabled
    std::deque<ipc::Message> replies_;      // answers to broker requests (HooksInstalled), oldest first

    std::mutex connection_mutex_;  // guards active_connection_
    ipc::PipeConnection* active_connection_ = nullptr;

    // Agent thread only.
    std::vector<ipc::ModuleEntry> known_modules_;
    ULONGLONG last_module_poll_ = 0;
    std::uint64_t polled_seq_ = 0;  // queue_seq_ as of the last module poll
    // Back-off state, agent thread only (see run() and poll_modules()).
    ULONGLONG next_launch_at_ = 0;          // earliest tick for the next broker auto-start
    DWORD launch_backoff_ms_ = 1000;        // 1 s, 2 s, 4 s ... 60 s; reset when a session's list is accepted
    ULONGLONG full_list_not_before_ = 0;    // earliest tick for the next full-list resend
    DWORD list_backoff_ms_ = 1000;          // 1 s, 2 s ... 30 s; reset when a list is accepted
    bool full_list_pending_ = false;         // the broker refused a ModuleList: send a full one on the next poll
};

}  // namespace cml::agent

#endif  // _WIN32
