#pragma once

// Windows glue between the view model and the broker: the control-pipe transport, starting cml_broker.exe, and the
// two connection threads of the running UI (one for commands and status polling, one for the event stream).

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "cml/ipc/control.h"
#include "cml/ui/controller.h"
#include "cml/ui/settings.h"
#include "cml/ui/view_model.h"

namespace cml::ui::win {

// Directory of the running executable (with no trailing separator).
std::wstring executable_directory();
// %APPDATA%\Chameleon\ui.json (UTF-8); next to the executable when APPDATA is not set.
std::string default_settings_path();

// Transport over a ControlClient.
class ClientTransport final : public Transport {
public:
    explicit ClientTransport(ipc::ControlClient client) : client_(std::move(client)) {}
    Result<ipc::ControlResponse> call(const std::string& cmd, ipc::json::Value args, std::uint32_t timeout_ms) override {
        return client_.call(cmd, std::move(args), timeout_ms);
    }
    ipc::ControlClient& client() { return client_; }

private:
    ipc::ControlClient client_;
};

// Starts `broker_path --headless [--control-pipe N] [--pipe N] [extra]`. When `kill_on_close` is non-null the process
// is put into a job object that kills it when the handle closes (tests); the job handle is returned there.
// CML5705 when the process cannot be created. `process_out` (optional) receives the process handle (caller closes).
Result<void> start_broker(const std::wstring& broker_path, const std::string& control_pipe, const std::string& agent_pipe,
                          const std::wstring& extra_arguments, HANDLE* kill_on_close, HANDLE* process_out);

// Connects to the control pipe; with `start_if_missing` and a broker path that exists, starts cml_broker.exe next to
// the UI when nothing answers and waits for it (up to `start_wait_ms`).
Result<ipc::ControlClient> connect_to_broker(const Settings& settings, bool start_if_missing, std::uint32_t start_wait_ms, bool* started);

// The two connection threads. Everything the threads learn goes into the view model.
class BrokerLink {
public:
    using Task = std::function<void(Controller&)>;

    BrokerLink(ViewModel& view_model, Settings settings);
    ~BrokerLink();
    BrokerLink(const BrokerLink&) = delete;
    BrokerLink& operator=(const BrokerLink&) = delete;

    void start();
    // Stops both threads. false when the command thread was still inside a long call after `wait_ms`; the object must
    // then never be destroyed (the process exits instead).
    bool stop(std::uint32_t wait_ms);

    // Runs `task` on the command thread, in order. Safe from any thread.
    void post(Task task);
    // New settings: the next connection and an immediate set-option round use them.
    void update_settings(const Settings& settings);
    // Asks the command thread to refresh `status` now.
    void poke();

private:
    void command_loop();
    void event_loop();
    Settings settings_copy();
    void sleep_interruptibly(std::uint32_t ms);

    ViewModel& vm_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> tasks_;
    Settings settings_;
    bool settings_dirty_ = false;
    bool poked_ = false;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> command_done_{false};
    std::atomic<bool> event_done_{false};
    std::atomic<std::uint64_t> connection_epoch_{0};  // bumped by the command thread after each (re)connect
    std::thread command_thread_;
    std::thread event_thread_;
    ULONGLONG last_broker_start_ = 0;
};

}  // namespace cml::ui::win
