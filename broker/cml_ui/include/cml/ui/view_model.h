#pragma once

// The window-free state of the broker UI: what the panels show, and how control-pipe answers and events change it.
// Everything here is portable and unit-tested (tests/unit/ui); drawing code only reads it.
//
// Threading: all mutating members lock an internal recursive mutex. A reader that iterates the lists (the UI
// thread, once per frame) takes `lock()` for the duration of the frame; the lists are plain std::deque/vector and
// the references returned by the accessors are valid while that lock is held.

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cml/ipc/control.h"
#include "cml/ipc/json.h"
#include "cml/ui/commands.h"
#include "cml/ui/compile_output.h"
#include "cml/util/diag.h"

namespace cml::ui {

enum class LogLevel { Trace, Debug, Info, Warn, Error };
const char* to_string(LogLevel level) noexcept;
bool parse_log_level(std::string_view text, LogLevel* level) noexcept;  // "trace|debug|info|warn|warning|error"

enum class ConnectionState { Disconnected, Connecting, Connected, Failed };
const char* to_string(ConnectionState state) noexcept;

// UI features that depend on what the broker provides (capability names: commands.h).
enum class Feature { Reload, Wait, SetOption, Exceptions, Restart, Revert };
const char* capability_of(Feature feature) noexcept;

struct ModuleRow {
    std::string path;
    bool enabled = false;
    std::string index_state;  // "loading" | "ready" | "failed" | "pending" | "" (not reported)
    std::uint32_t index_percent = 0;
    std::string index_message;
};

struct ProcessRow {
    std::uint32_t pid = 0;
    std::vector<ModuleRow> modules;
    bool modules_truncated = false;
};

struct OutputLine {
    std::uint64_t seq = 0;
    OutputSeverity severity = OutputSeverity::Info;
    std::string text;
    SourceLocation location;  // clickable() = the line names a file and a line
    std::string diag_id;      // "CML2101" on the first line of a diagnostic
    std::string full_message; // the whole diagnostic message, on that first line only (duplicate detection)
    std::uint32_t pid = 0;
    std::string stage;        // "compile", "link", "reload", "watch", "ui", ...
};

struct PatchRecord {
    std::uint64_t seq = 0;
    std::int64_t when_ms = 0;  // wall clock (ms since the Unix epoch) when the UI learned about the patch
    std::uint32_t pid = 0;
    std::string module;
    std::uint64_t generation = 0;
    std::uint64_t hooks = 0;
    std::uint64_t new_functions = 0;
    std::uint64_t new_globals = 0;
    PatchTiming timing;        // present once a `reload` answer of this UI carried the numbers
    bool reverted = false;
};

struct ChangeRecord {
    std::uint64_t seq = 0;
    std::int64_t when_ms = 0;
    std::string path;    // file, or a description for events that name no file
    std::string state;   // "patched" | "no patch" | "watch" | "module"
    std::string detail;
};

struct LogLine {
    std::uint64_t seq = 0;
    std::int64_t when_ms = 0;
    LogLevel level = LogLevel::Info;
    std::string message;
};

struct LogFilter {
    LogLevel min_level = LogLevel::Info;
    std::string text;  // case-insensitive substring; empty = everything
};

struct ExceptionRow {
    std::uint32_t pid = 0;
    std::uint32_t tid = 0;
    std::uint64_t exception_id = 0;
    std::string code;      // "0xC0000005"
    std::string address;
    std::string module;
    std::string function;
    std::string offset;
    std::vector<std::string> actions;  // "continue" | "leave" | "ignore"
};

struct ConfirmPrompt {
    std::string message;
    std::uint32_t pid = 0;
    std::string module;
};

struct CompileProgress {
    std::string stage;
    std::uint32_t pid = 0;
    std::string module;
    std::uint64_t done = 0;
    std::uint64_t total = 0;
};

enum class StatusSeverity { Info, Warning, Error };

class ViewModel {
public:
    ViewModel();

    using Clock = std::function<std::int64_t()>;
    // Wall-clock milliseconds since the Unix epoch; tests replace it.
    void set_clock(Clock clock);

    std::unique_lock<std::recursive_mutex> lock() const { return std::unique_lock<std::recursive_mutex>(mutex_); }

    // --- connection ---------------------------------------------------------------------------------------------
    void set_connection(ConnectionState state, const std::string& text);
    ConnectionState connection() const;
    const std::string& connection_text() const { return connection_text_; }

    // --- capabilities -------------------------------------------------------------------------------------------
    void set_capabilities(const std::vector<std::string>& capabilities);
    const std::vector<std::string>& capabilities() const { return capabilities_; }
    bool has_capability(std::string_view name) const;
    // The broker advertises the capability and has not answered CML5402 for one of its commands.
    bool feature_available(Feature feature) const;
    bool capability_usable(std::string_view capability) const;
    // A command was answered with CML5402: its capability is treated as missing from now on.
    void mark_command_unavailable(const std::string& cmd);

    // --- status -------------------------------------------------------------------------------------------------
    void apply_status(const ipc::StatusReport& report);
    const std::vector<ProcessRow>& processes() const { return processes_; }
    const std::string& broker_version() const { return broker_version_; }
    std::uint64_t generation() const { return generation_; }
    bool broker_reload_running() const { return reload_running_; }
    bool status_truncated() const { return status_truncated_; }

    // --- events -------------------------------------------------------------------------------------------------
    // Returns false for events the UI does not know (they are counted, not shown).
    bool apply_event(const ipc::ControlEvent& event);
    bool apply_event_body(const ipc::json::Value& body);
    std::uint64_t ignored_events() const { return ignored_events_; }

    // --- command results ----------------------------------------------------------------------------------------
    void apply_reload_result(const ReloadSummary& summary);
    void apply_restart_result(const RestartSummary& summary);
    void apply_revert_result(const RevertSummary& summary);
    // A command failed with `diag`; shown in the status line and, unless a diag event with the same id and text was
    // just shown, in the output panel. CML5551 on reload arms the confirm prompt; CML5402 disables the feature.
    // `request_serial` is the number begin_reload_request() gave the failed reload (0 = not a reload).
    void apply_command_failure(const std::string& cmd, const Diag& diag, std::uint32_t pid = 0, const std::string& module = std::string(),
                               std::uint64_t request_serial = 0);
    void set_reload_in_flight(bool in_flight);
    // The one guarded entry for every reload trigger (button, hotkey, tray, menu, confirm prompt): refuses unless the
    // broker is connected, advertises `reload` and no reload runs. On success the reload counts as running at once, so a
    // second trigger before the first reaches the broker is refused too; `why` receives the reason otherwise (it is also
    // shown in the status line).
    bool try_begin_reload(std::string* why = nullptr);
    // Called by Controller::reload: the number of this reload request (one confirm prompt per request).
    std::uint64_t begin_reload_request();
    std::uint64_t reload_serial() const;
    bool reload_in_flight() const { return reload_in_flight_; }
    // True while this UI or the broker is reloading.
    bool busy() const { return reload_in_flight_ || reload_running_; }

    // --- status line --------------------------------------------------------------------------------------------
    void set_status_message(const std::string& text, StatusSeverity severity);
    const std::string& status_message() const { return status_message_; }
    StatusSeverity status_severity() const { return status_severity_; }

    // --- compile output -----------------------------------------------------------------------------------------
    const std::deque<OutputLine>& output() const { return output_; }
    void clear_output();
    std::size_t error_count() const;
    std::size_t warning_count() const;
    // Adds text as output lines (each line parsed for a source location). Used for UI messages.
    void add_output_text(const std::string& text, OutputSeverity severity, const std::string& stage, std::uint32_t pid = 0);

    // --- patch history, changes, log ----------------------------------------------------------------------------
    const std::deque<PatchRecord>& patches() const { return patches_; }
    const std::deque<ChangeRecord>& changes() const { return changes_; }
    void clear_changes();
    const std::deque<LogLine>& log() const { return log_; }
    void clear_log();
    void add_log(LogLevel level, const std::string& message);
    // Indices into log() of the lines that pass the filter, oldest first.
    std::vector<std::size_t> filtered_log(const LogFilter& filter) const;

    // --- compile progress ---------------------------------------------------------------------------------------
    const std::optional<CompileProgress>& progress() const { return progress_; }

    // --- exceptions (ADR-0032, WP-20) ---------------------------------------------------------------------------
    const std::deque<ExceptionRow>& pending_exceptions() const { return exceptions_; }
    // The exception the modal is about (the oldest pending one), or nullptr.
    const ExceptionRow* current_exception() const;
    // The row for an `exception` event body or an entry of the `exceptions` answer.
    static ExceptionRow parse_exception(const ipc::json::Value& body);
    void resolve_exception(std::uint32_t pid, std::uint64_t exception_id);
    // The answer of the `exceptions` command is authoritative: it replaces the list (rows the broker no longer holds go away).
    void set_pending_exceptions(const std::vector<ExceptionRow>& rows);

    // --- confirm prompt (CML5551) -------------------------------------------------------------------------------
    const std::optional<ConfirmPrompt>& confirm_prompt() const { return confirm_; }
    void clear_confirm_prompt();

    static constexpr std::size_t kMaxOutput = 10000;
    static constexpr std::size_t kMaxPatches = 500;
    static constexpr std::size_t kMaxChanges = 500;
    static constexpr std::size_t kMaxLog = 5000;
    static constexpr std::size_t kMaxExceptions = 100;

private:
    std::int64_t now() const;
    void push_output(OutputLine line);
    void count_line(const OutputLine& line, int delta);
    void add_diag_lines(const std::string& id, const std::string& message, const std::string& action, OutputSeverity severity,
                        const std::string& stage, std::uint32_t pid);
    void arm_confirm(const std::string& message, std::uint32_t pid, const std::string& module, std::uint64_t request_serial);
    void add_change(const std::string& path, const std::string& state, const std::string& detail);
    PatchRecord& upsert_patch(std::uint32_t pid, const std::string& module, std::uint64_t generation);
    void on_diag(const ipc::json::Value& body);
    void on_patch_applied(const ipc::json::Value& body);
    void on_progress(const ipc::json::Value& body);
    void on_log(const ipc::json::Value& body);
    void on_exception(const ipc::json::Value& body);
    void on_restarted(const ipc::json::Value& body);
    void on_patch_reverted(const ipc::json::Value& body);
    // A header line of this diagnostic (same id and full message) among the latest lines; `from_command` selects the
    // lines made by a command failure instead of by a `diag` event.
    bool recently_shown(const std::string& diag_id, const std::string& message, bool from_command) const;

    mutable std::recursive_mutex mutex_;
    Clock clock_;
    ConnectionState connection_ = ConnectionState::Disconnected;
    std::string connection_text_ = "not connected";
    std::vector<std::string> capabilities_;
    std::set<std::string> unavailable_cmds_;
    std::vector<ProcessRow> processes_;
    std::string broker_version_;
    std::uint64_t generation_ = 0;
    bool reload_running_ = false;
    bool reload_in_flight_ = false;
    bool status_truncated_ = false;
    std::string status_message_;
    StatusSeverity status_severity_ = StatusSeverity::Info;
    std::deque<OutputLine> output_;
    std::deque<PatchRecord> patches_;
    std::deque<ChangeRecord> changes_;
    std::deque<LogLine> log_;
    std::deque<ExceptionRow> exceptions_;
    std::optional<ConfirmPrompt> confirm_;
    std::uint64_t reload_serial_ = 1;          // number of the latest reload request
    std::uint64_t confirm_armed_serial_ = 0;   // request for which a CML5551 prompt was already armed (and maybe answered)
    bool reload_reserved_ = false;             // try_begin_reload() ran, Controller::reload() has not started yet
    std::size_t error_lines_ = 0;
    std::size_t warning_lines_ = 0;
    std::optional<CompileProgress> progress_;
    std::uint64_t next_seq_ = 1;
    std::uint64_t ignored_events_ = 0;
};

}  // namespace cml::ui
