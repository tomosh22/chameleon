#include "self_test.h"

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "broker_link.h"
#include "cml/ipc/control.h"
#include "cml/proc/proc.h"
#include "cml/ui/commands.h"
#include "cml/ui/compile_output.h"
#include "cml/ui/controller.h"
#include "cml/ui/editor.h"
#include "cml/ui/settings.h"
#include "cml/ui/view_model.h"

namespace cml::ui::win {

void write_stdout(const std::string& text) {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) {
        OutputDebugStringA(text.c_str());
        return;
    }
    DWORD written = 0;
    WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

namespace {

class Checker {
public:
    void check(bool ok, const std::string& what) {
        ++checks_;
        if (ok) {
            write_stdout("  ok    " + what + "\n");
        } else {
            ++failures_;
            write_stdout("  FAIL  " + what + "\n");
        }
    }
    void note(const std::string& text) { write_stdout("  note  " + text + "\n"); }
    int checks() const { return checks_; }
    int failures() const { return failures_; }

private:
    int checks_ = 0;
    int failures_ = 0;
};

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& s : items) {
        out += (out.empty() ? "" : ", ") + s;
    }
    return out.empty() ? "(none)" : out;
}

}  // namespace

int run_self_test(const SelfTestOptions& options) {
    Checker t;
    write_stdout("cml_ui --self-test\n");

    // Private pipes, so the test never meets a broker the developer runs.
    const std::string unique = std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64());
    const std::string control_pipe = "\\\\.\\pipe\\cml_ui_selftest_c_" + unique;
    const std::string agent_pipe = "\\\\.\\pipe\\cml_ui_selftest_a_" + unique;

    std::error_code ec;
    std::filesystem::path work = options.work_dir.empty() ? std::filesystem::temp_directory_path(ec) / ("cml_ui_selftest_" + unique)
                                                          : std::filesystem::path(options.work_dir);
    std::filesystem::create_directories(work, ec);
    t.check(!ec, "work directory " + work.string());

    // --- the broker ------------------------------------------------------------------------------------------------
    HANDLE job = nullptr;
    HANDLE process = nullptr;
    const std::wstring log_argument = L"--log \"" + (work / "broker.log").wstring() + L"\"";
    Result<void> started = start_broker(options.broker_path, control_pipe, agent_pipe, log_argument, &job, &process);
    t.check(started.has_value(), "start cml_broker --headless on a private control pipe");
    if (!started) {
        write_stdout(started.error().format() + "\n");
        write_stdout("CMLUI_SELFTEST result=fail checks=" + std::to_string(t.checks()) + " failures=" + std::to_string(t.failures()) + "\n");
        return 1;
    }

    const auto finish = [&]() {
        if (process != nullptr) {
            TerminateProcess(process, 0);
            WaitForSingleObject(process, 5000);
            CloseHandle(process);
        }
        if (job != nullptr) {
            CloseHandle(job);
        }
        const bool ok = t.failures() == 0;
        write_stdout(std::string("CMLUI_SELFTEST result=") + (ok ? "ok" : "fail") + " checks=" + std::to_string(t.checks()) +
                     " failures=" + std::to_string(t.failures()) + "\n");
        return ok ? 0 : 1;
    };

    // --- connect ---------------------------------------------------------------------------------------------------
    std::optional<ipc::ControlClient> first;
    Diag first_error = Diag::make(5404, "no connection attempt was made", "n/a");
    for (int attempt = 0; attempt < 60 && !first; ++attempt) {
        Result<ipc::ControlClient> connected = ipc::ControlClient::connect(control_pipe, 500);
        if (connected) {
            first.emplace(std::move(*connected));
        } else {
            first_error = connected.error();
            Sleep(250);
        }
    }
    t.check(first.has_value(), "connect to the broker's control pipe");
    if (!first) {
        write_stdout(first_error.format() + "\n");
        return finish();
    }
    ClientTransport commands(std::move(*first));
    ViewModel vm;
    Controller controller(vm, commands);

    // --- status ----------------------------------------------------------------------------------------------------
    Result<void> status = controller.refresh_status();
    t.check(status.has_value(), "status");
    if (!status) {
        write_stdout(status.error().format() + "\n");
        return finish();
    }
    t.check(!vm.broker_version().empty(), "status reports a broker version (" + vm.broker_version() + ")");
    t.check(vm.has_capability("status"), "status lists the 'status' capability");
    t.note("capabilities: " + join(vm.capabilities()));

    // --- the command table covers what the broker advertises -------------------------------------------------------
    const std::vector<std::string> uncovered = uncovered_capabilities(vm.capabilities());
    t.check(uncovered.empty(), "the UI command table covers every advertised capability (uncovered: " + join(uncovered) + ")");
    t.note("UI features disabled for this broker: " + join(unavailable_commands(vm.capabilities())));
    for (const char* required : {"reload", "wait", "set-option", "subscribe"}) {
        t.check(vm.has_capability(required), std::string("the broker advertises '") + required + "'");
    }

    // --- events: a second connection, subscribed, receives the diag the failing reload publishes ---------------------
    Result<ipc::ControlClient> second = ipc::ControlClient::connect(control_pipe, 5000);
    t.check(second.has_value(), "second control connection for events");
    if (!second) {
        return finish();
    }
    ClientTransport events(std::move(*second));
    Controller event_controller(vm, events);
    t.check(event_controller.subscribe().has_value(), "subscribe");

    if (vm.feature_available(Feature::Reload)) {
        // No process is attached to this broker: reload fails with CML5300 and publishes a diag event.
        Result<ReloadSummary> reload = controller.reload(0, "", false);
        t.check(!reload.has_value() && reload.error().code == 5300, "reload without a target fails with CML5300");
        bool saw_event = false;
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (GetTickCount64() < deadline) {
            Result<std::optional<ipc::ControlEvent>> event = events.client().next_event(250);
            if (!event) {
                break;
            }
            if (*event) {
                vm.apply_event(**event);
                if ((*event)->name == "diag" && (*event)->body.get_string("id") == "CML5300") {
                    saw_event = true;
                    break;
                }
            }
        }
        t.check(saw_event, "the diag event arrives on the subscribed connection");
        std::size_t shown = 0;
        for (const OutputLine& line : vm.output()) {
            shown += line.diag_id == "CML5300" ? 1 : 0;
        }
        t.check(shown == 1, "CML5300 is in the output panel exactly once (response and event are merged)");
        t.check(vm.status_severity() == StatusSeverity::Error, "the status line shows the error");
        t.check(!vm.reload_in_flight(), "the in-flight flag is cleared after the failure");
    }

    // --- settings reach the broker ---------------------------------------------------------------------------------
    if (vm.feature_available(Feature::SetOption)) {
        Result<void> jobs = controller.set_option("max_parallel_compiles", ipc::json::Value::integer(2));
        t.check(jobs.has_value(), "set-option max_parallel_compiles");
        Settings defaults;  // a default ui.json sends nothing: the broker's own values stay
        t.check(controller.apply_settings(defaults).has_value() && settings_broker_options(defaults).empty(), "apply_settings with the defaults sends no option");
        Settings settings;
        settings.compile_on_save = false;
        Result<void> applied = controller.apply_settings(settings);
        t.check(applied.has_value(), "apply_settings (set-option compile_on_save false)");
        if (!applied) {
            write_stdout(applied.error().format() + "\n");
        }
    }
    if (vm.feature_available(Feature::Wait)) {
        t.check(controller.wait("idle", 2000).has_value(), "wait for idle");
    }
    if (vm.feature_available(Feature::Exceptions)) {
        // WP-20: the real `exceptions` command; nothing is held, so the authoritative answer is empty.
        t.check(controller.refresh_exceptions().has_value() && vm.pending_exceptions().empty(), "exceptions: the broker holds no thread");
    }
    if (vm.feature_available(Feature::Revert)) {
        // WP-22: the real `revert` command with nothing to revert must answer (a result or a diagnostic), not hang or disappear.
        Result<RevertSummary> reverted = controller.revert(0, "", "all");
        t.check(reverted.has_value() || reverted.error().code != 5402, "revert: the broker answers (" +
                    (reverted ? std::string("ok") : reverted.error().id()) + ")");
    }
    t.check(controller.refresh_status().has_value(), "status again after the commands");

    // --- the view model, fed with the event shapes of ADR-0032 and WP-12 -------------------------------------------
    {
        ViewModel offline;
        const auto feed = [&](const char* json_text) {
            Result<ipc::json::Value> body = ipc::json::parse(json_text);
            return body.has_value() && offline.apply_event_body(*body);
        };
        t.check(feed(R"({"event":"diag","id":"CML2101","severity":"error","stage":"compile","message":"compile failed\nC:\\src\\a.cpp(3,4): error C2065: 'x': undeclared identifier","action":"Fix it."})"),
                "view model: diag event");
        bool clickable = false;
        for (const OutputLine& line : offline.output()) {
            clickable = clickable || (line.location.clickable() && line.location.line == 3 && line.location.column == 4);
        }
        t.check(clickable, "view model: compiler error line is clickable");
        t.check(feed(R"({"event":"patch_applied","pid":1,"module":"a.exe","generation":1,"hooks":2})") && offline.patches().size() == 1,
                "view model: patch_applied");
        t.check(feed(R"({"event":"log","level":"warn","message":"hello"})") && offline.log().size() == 1, "view model: log event");
        t.check(feed(R"({"event":"exception","pid":1,"tid":2,"exception_id":3,"code":"0xC0000005"})") && offline.current_exception() != nullptr,
                "view model: exception event opens the modal state");
        t.check(feed(R"({"event":"restarted","old_pid":1,"new_pid":2})") && offline.pending_exceptions().empty(), "view model: restarted event");
        t.check(feed(R"({"event":"patch_reverted","pid":1,"module":"a.exe","generation":1})") && offline.patches()[0].reverted,
                "view model: patch_reverted event");
    }

    // --- editor commands and settings ------------------------------------------------------------------------------
    {
        SourceLocation location;
        location.file = R"(C:\src\a b.cpp)";
        location.line = 12;
        location.column = 3;
        EditorConfig code;
        t.check(make_editor_launch(code, location).has_value() && join_command_line(make_editor_launch(code, location)->argv) == R"(code -g "C:\src\a b.cpp:12:3")",
                "editor: code -g file:line:col");
        EditorConfig devenv;
        devenv.kind = EditorKind::VisualStudio;
        t.check(make_editor_launch(devenv, location).has_value() &&
                    join_command_line(make_editor_launch(devenv, location)->argv) == R"(devenv /edit "C:\src\a b.cpp" /command "Edit.GoTo 12")",
                "editor: devenv /edit file /command \"Edit.GoTo line\"");
        Settings settings;
        settings.compile_on_save = true;
        settings.jobs = 3;
        const std::string settings_path = (work / "ui.json").string();
        t.check(save_settings(settings_path, settings).has_value(), "settings: save");
        Result<Settings> loaded = load_settings(settings_path);
        t.check(loaded.has_value() && loaded->compile_on_save.value_or(false) && loaded->jobs == 3, "settings: load");
    }

    return finish();
}

}  // namespace cml::ui::win
