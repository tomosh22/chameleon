#include "broker_link.h"

#include <chrono>
#include <vector>

#include "cml/ipc/control_ids.h"
#include "cml/proc/proc.h"
#include "cml/ui/editor.h"
#include "cml/ui/ui_diag_ids.h"

namespace cml::ui::win {

namespace {

std::wstring quote(const std::wstring& text) {
    return proc::wide_from_utf8(quote_argument(proc::utf8_from_wide(text)));
}

bool file_exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Pipe and protocol errors that mean "this connection is gone" (as opposed to a refusal by the broker).
bool is_transport_error(std::uint32_t code) {
    return (code >= 5200 && code <= 5299) || code == ids::kControlUnreachable || code == ids::kControlIoFailed ||
           code == ids::kControlResponseInvalid || code == ids::kControlTimeout || code == ids::kControlLineTooLong;
}

}  // namespace

std::wstring executable_directory() {
    std::wstring buffer(512, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) {
            return L".";
        }
        if (n < buffer.size()) {
            buffer.resize(n);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    const std::size_t slash = buffer.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : buffer.substr(0, slash);
}

std::string default_settings_path() {
    wchar_t appdata[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, static_cast<DWORD>(sizeof appdata / sizeof appdata[0]));
    std::wstring dir;
    if (n > 0 && n < sizeof appdata / sizeof appdata[0]) {
        dir = std::wstring(appdata, n) + L"\\Chameleon";
    } else {
        dir = executable_directory();
    }
    return proc::utf8_from_wide(dir + L"\\ui.json");
}

Result<void> start_broker(const std::wstring& broker_path, const std::string& control_pipe, const std::string& agent_pipe,
                          const std::wstring& extra_arguments, HANDLE* kill_on_close, HANDLE* process_out) {
    const auto fail = [&](const std::string& why) {
        return Diag::make(ids::kUiBrokerStart, "cannot start the broker '" + proc::utf8_from_wide(broker_path) + "': " + why,
                          "Put cml_broker.exe next to cml_ui.exe, set the broker path in the settings, or start 'cml_broker --headless' yourself.");
    };
    if (!file_exists(broker_path)) {
        return fail("the file does not exist");
    }
    std::wstring command = quote(broker_path) + L" --headless";
    if (!control_pipe.empty()) {
        command += L" --control-pipe " + quote(proc::wide_from_utf8(control_pipe));
    }
    if (!agent_pipe.empty()) {
        command += L" --pipe " + quote(proc::wide_from_utf8(agent_pipe));
    }
    if (!extra_arguments.empty()) {
        command += L" " + extra_arguments;
    }
    std::wstring directory = broker_path;
    const std::size_t slash = directory.find_last_of(L"\\/");
    directory = slash == std::wstring::npos ? L"." : directory.substr(0, slash);

    HANDLE job = nullptr;
    if (kill_on_close != nullptr) {
        job = CreateJobObjectW(nullptr, nullptr);
        if (job != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        }
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION info = {};
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    if (!CreateProcessW(broker_path.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, flags, nullptr, directory.c_str(), &startup,
                        &info)) {
        const DWORD error = GetLastError();
        if (job != nullptr) {
            CloseHandle(job);
        }
        return fail("CreateProcess failed with error " + std::to_string(error));
    }
    if (job != nullptr) {
        AssignProcessToJobObject(job, info.hProcess);
        *kill_on_close = job;
    }
    CloseHandle(info.hThread);
    if (process_out != nullptr) {
        *process_out = info.hProcess;
    } else {
        CloseHandle(info.hProcess);
    }
    return {};
}

Result<ipc::ControlClient> connect_to_broker(const Settings& settings, bool start_if_missing, std::uint32_t start_wait_ms, bool* started) {
    Result<std::string> pipe = ipc::resolve_control_pipe_name(settings.control_pipe);
    if (!pipe) {
        return pipe.error();
    }
    Result<ipc::ControlClient> client = ipc::ControlClient::connect(*pipe, 300);
    if (client || !start_if_missing || !settings.start_broker) {
        return client;
    }
    const std::wstring path = settings.broker_path.empty() ? executable_directory() + L"\\cml_broker.exe" : proc::wide_from_utf8(settings.broker_path);
    Result<void> launched = start_broker(path, settings.control_pipe, std::string(), std::wstring(), nullptr, nullptr);
    if (!launched) {
        return launched.error();
    }
    if (started != nullptr) {
        *started = true;
    }
    const ULONGLONG deadline = GetTickCount64() + start_wait_ms;
    for (;;) {
        Sleep(250);
        client = ipc::ControlClient::connect(*pipe, 300);
        if (client || GetTickCount64() >= deadline) {
            return client;
        }
    }
}

// --- BrokerLink --------------------------------------------------------------------------------------------------

BrokerLink::BrokerLink(ViewModel& view_model, Settings settings) : vm_(view_model), settings_(std::move(settings)) {}

BrokerLink::~BrokerLink() {
    stop(5000);
}

void BrokerLink::start() {
    command_thread_ = std::thread([this] {
        command_loop();
        command_done_ = true;
    });
    event_thread_ = std::thread([this] {
        event_loop();
        event_done_ = true;
    });
}

bool BrokerLink::stop(std::uint32_t wait_ms) {
    stopping_ = true;
    cv_.notify_all();
    const ULONGLONG deadline = GetTickCount64() + wait_ms;
    while ((!command_done_ || !event_done_) && GetTickCount64() < deadline) {
        Sleep(20);
    }
    bool clean = true;
    for (std::thread* t : {&command_thread_, &event_thread_}) {
        if (!t->joinable()) {
            continue;
        }
        const bool done = t == &command_thread_ ? command_done_.load() : event_done_.load();
        if (done) {
            t->join();
        } else {
            t->detach();
            clean = false;
        }
    }
    return clean;
}

void BrokerLink::post(Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    cv_.notify_all();
}

void BrokerLink::update_settings(const Settings& settings) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings_ = settings;  // used by the next (re)connect; the UI posts the changed options itself
    }
    cv_.notify_all();
}

void BrokerLink::poke() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        poked_ = true;
    }
    cv_.notify_all();
}

Settings BrokerLink::settings_copy() {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}

void BrokerLink::sleep_interruptibly(std::uint32_t ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return stopping_.load(); });
}

void BrokerLink::command_loop() {
    bool reported_failure = false;
    while (!stopping_) {
        vm_.set_connection(ConnectionState::Connecting, "connecting to the broker");
        const Settings settings = settings_copy();
        bool started = false;
        const bool may_start = GetTickCount64() - last_broker_start_ > 20000 || last_broker_start_ == 0;
        Result<ipc::ControlClient> client = connect_to_broker(settings, may_start, 12000, &started);
        if (started) {
            last_broker_start_ = GetTickCount64();
            vm_.add_log(LogLevel::Info, "started cml_broker.exe");
        }
        const auto fail = [&](const Diag& diag) {
            vm_.set_connection(ConnectionState::Failed, diag.message);
            vm_.set_status_message(diag.id() + ": " + diag.message, StatusSeverity::Error);
            if (!reported_failure) {
                vm_.add_log(LogLevel::Error, diag.format());
                reported_failure = true;
            }
            sleep_interruptibly(2000);
        };
        if (!client) {
            fail(client.error());
            continue;
        }
        ClientTransport transport(std::move(*client));
        Controller controller(vm_, transport);
        if (Result<void> status = controller.refresh_status(); !status) {
            fail(status.error());
            continue;
        }
        reported_failure = false;
        ++connection_epoch_;  // before Connected is visible: the event thread then reads the epoch of this connection
        vm_.set_connection(ConnectionState::Connected, "broker " + vm_.broker_version());
        vm_.add_log(LogLevel::Info, "connected to broker " + vm_.broker_version());
        vm_.set_status_message("Connected to broker " + vm_.broker_version(), StatusSeverity::Info);
        if (vm_.feature_available(Feature::SetOption)) {
            if (Result<void> applied = controller.apply_settings(settings); !applied) {
                vm_.add_log(LogLevel::Warn, applied.error().format());
            }
        }
        if (vm_.feature_available(Feature::Exceptions)) {
            static_cast<void>(controller.refresh_exceptions());
        }

        bool broken = false;
        ULONGLONG next_poll = 0;
        while (!stopping_ && !broken) {
            Task task;
            bool have_task = false;
            bool dirty = false;
            bool poke_requested = false;
            Settings current;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(200),
                             [this] { return stopping_.load() || !tasks_.empty() || settings_dirty_ || poked_; });
                if (!tasks_.empty()) {
                    task = std::move(tasks_.front());
                    tasks_.pop_front();
                    have_task = true;
                }
                dirty = settings_dirty_;
                settings_dirty_ = false;
                poke_requested = poked_;
                poked_ = false;
                current = settings_;
            }
            if (stopping_) {
                break;
            }
            if (have_task) {
                task(controller);
                next_poll = 0;
                if (dirty) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    settings_dirty_ = true;  // handled on the next round
                }
                continue;
            }
            if (dirty && vm_.feature_available(Feature::SetOption)) {
                if (Result<void> applied = controller.apply_settings(current); !applied) {
                    vm_.add_log(LogLevel::Warn, applied.error().format());
                }
            }
            const ULONGLONG now = GetTickCount64();
            if (poke_requested || now >= next_poll) {
                next_poll = now + 1500;
                if (Result<void> status = controller.refresh_status(); !status && is_transport_error(status.error().code)) {
                    vm_.set_connection(ConnectionState::Failed, status.error().message);
                    vm_.add_log(LogLevel::Error, "lost the broker: " + status.error().format());
                    broken = true;
                }
            }
        }
        if (!broken && stopping_) {
            vm_.set_connection(ConnectionState::Disconnected, "closing");
        }
    }
}

void BrokerLink::event_loop() {
    while (!stopping_) {
        if (vm_.connection() != ConnectionState::Connected) {
            sleep_interruptibly(200);
            continue;
        }
        const std::uint64_t epoch = connection_epoch_.load();
        Result<ipc::ControlClient> client = connect_to_broker(settings_copy(), false, 0, nullptr);
        if (!client) {
            sleep_interruptibly(1000);
            continue;
        }
        ClientTransport transport(std::move(*client));
        Controller controller(vm_, transport);
        Result<void> subscribed = controller.subscribe();
        if (!subscribed) {
            // An older broker may reject an event name it does not know: retry with the WP-12 set.
            const Request base = make_subscribe_request({"diag", "patch_applied", "progress", "log"});
            Result<ipc::ControlResponse> again = transport.call(base.cmd, base.args, Controller::kQuickTimeoutMs);
            if (!again || !again->ok) {
                vm_.add_log(LogLevel::Warn, "cannot subscribe to broker events: " + subscribed.error().format());
                sleep_interruptibly(5000);
                continue;
            }
        }
        vm_.add_log(LogLevel::Debug, "event stream connected");
        while (!stopping_ && vm_.connection() == ConnectionState::Connected && epoch == connection_epoch_.load()) {
            Result<std::optional<ipc::ControlEvent>> event = transport.client().next_event(250);
            if (!event) {
                vm_.add_log(LogLevel::Warn, "event stream lost: " + event.error().format());
                break;
            }
            if (*event) {
                vm_.apply_event(**event);
            }
        }
    }
}

}  // namespace cml::ui::win
