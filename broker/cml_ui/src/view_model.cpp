#include "cml/ui/view_model.h"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace cml::ui {

namespace json = ipc::json;

namespace {

template <class Deque>
void trim_front(Deque& d, std::size_t max) {
    while (d.size() > max) {
        d.pop_front();
    }
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// A member that the broker may write as a string ("0xC0000005") or as a number.
std::string text_or_number(const json::Value& object, const char* key) {
    const json::Value* v = object.find(key);
    if (v == nullptr) {
        return {};
    }
    if (v->is_string()) {
        return v->as_string();
    }
    if (v->is_integer()) {
        return std::to_string(v->as_integer());
    }
    return {};
}

std::uint64_t unsigned_member(const json::Value& object, const char* key) {
    const std::int64_t v = object.get_integer(key, 0);
    return v < 0 ? 0 : static_cast<std::uint64_t>(v);
}

OutputSeverity severity_from_text(const std::string& text) {
    if (text == "warning" || text == "warn") {
        return OutputSeverity::Warning;
    }
    if (text == "info" || text == "note") {
        return OutputSeverity::Info;
    }
    return OutputSeverity::Error;
}

std::string first_line_of(const std::string& text) {
    const std::size_t nl = text.find('\n');
    std::string line = nl == std::string::npos ? text : text.substr(0, nl);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

}  // namespace

const char* to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
    }
    return "info";
}

bool parse_log_level(std::string_view text, LogLevel* level) noexcept {
    if (text == "trace") {
        *level = LogLevel::Trace;
    } else if (text == "debug") {
        *level = LogLevel::Debug;
    } else if (text == "info") {
        *level = LogLevel::Info;
    } else if (text == "warn" || text == "warning") {
        *level = LogLevel::Warn;
    } else if (text == "error" || text == "fatal" || text == "critical") {
        *level = LogLevel::Error;
    } else {
        return false;
    }
    return true;
}

const char* to_string(ConnectionState state) noexcept {
    switch (state) {
        case ConnectionState::Disconnected: return "disconnected";
        case ConnectionState::Connecting: return "connecting";
        case ConnectionState::Connected: return "connected";
        case ConnectionState::Failed: return "failed";
    }
    return "disconnected";
}

const char* capability_of(Feature feature) noexcept {
    switch (feature) {
        case Feature::Reload: return "reload";
        case Feature::Wait: return "wait";
        case Feature::SetOption: return "set-option";
        case Feature::Exceptions: return "exceptions";
        case Feature::Restart: return "restart";
        case Feature::Revert: return "revert";
    }
    return "";
}

ViewModel::ViewModel() {
    clock_ = [] {
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    };
}

void ViewModel::set_clock(Clock clock) {
    auto guard = lock();
    clock_ = std::move(clock);
}

std::int64_t ViewModel::now() const {
    return clock_();
}

// --- connection / capabilities ---------------------------------------------------------------------------------

void ViewModel::set_connection(ConnectionState state, const std::string& text) {
    auto guard = lock();
    connection_ = state;
    connection_text_ = text;
    if (state != ConnectionState::Connected) {
        reload_running_ = false;
    }
}

ConnectionState ViewModel::connection() const {
    auto guard = lock();
    return connection_;
}

void ViewModel::set_capabilities(const std::vector<std::string>& capabilities) {
    auto guard = lock();
    capabilities_ = capabilities;
    unavailable_cmds_.clear();
}

bool ViewModel::has_capability(std::string_view name) const {
    auto guard = lock();
    return std::find(capabilities_.begin(), capabilities_.end(), name) != capabilities_.end();
}

bool ViewModel::capability_usable(std::string_view capability) const {
    auto guard = lock();
    return has_capability(capability) && unavailable_cmds_.count(std::string(capability)) == 0;
}

bool ViewModel::feature_available(Feature feature) const {
    return capability_usable(capability_of(feature));
}

void ViewModel::mark_command_unavailable(const std::string& cmd) {
    auto guard = lock();
    for (const CommandInfo& c : command_table()) {
        if (cmd == c.cmd) {
            unavailable_cmds_.insert(c.capability);
        }
    }
}

// --- status ----------------------------------------------------------------------------------------------------

void ViewModel::apply_status(const ipc::StatusReport& report) {
    auto guard = lock();
    if (capabilities_ != report.capabilities) {
        capabilities_ = report.capabilities;
        unavailable_cmds_.clear();
    }
    broker_version_ = report.version;
    generation_ = report.generation;
    reload_running_ = report.reload_running;
    status_truncated_ = report.truncated;
    processes_.clear();
    for (const ipc::StatusProcess& p : report.processes) {
        ProcessRow row;
        row.pid = p.pid;
        row.modules_truncated = p.modules_truncated;
        for (const ipc::StatusModule& m : p.modules) {
            ModuleRow module;
            module.path = m.path;
            module.enabled = m.enabled;
            module.index_state = m.index_state;
            module.index_percent = m.index_percent;
            module.index_message = m.index_message;
            row.modules.push_back(std::move(module));
        }
        processes_.push_back(std::move(row));
    }
}

// --- events ----------------------------------------------------------------------------------------------------

bool ViewModel::apply_event(const ipc::ControlEvent& event) {
    return apply_event_body(event.body);
}

bool ViewModel::apply_event_body(const json::Value& body) {
    auto guard = lock();
    const std::string name = body.get_string("event");
    if (name == "diag") {
        on_diag(body);
    } else if (name == "patch_applied") {
        on_patch_applied(body);
    } else if (name == "progress") {
        on_progress(body);
    } else if (name == "log") {
        on_log(body);
    } else if (name == "exception") {
        on_exception(body);
    } else if (name == "restarted") {
        on_restarted(body);
    } else if (name == "patch_reverted") {
        on_patch_reverted(body);
    } else {
        ++ignored_events_;
        return false;
    }
    return true;
}

void ViewModel::push_output(OutputLine line) {
    line.seq = next_seq_++;
    output_.push_back(std::move(line));
    trim_front(output_, kMaxOutput);
}

void ViewModel::add_output_text(const std::string& text, OutputSeverity severity, const std::string& stage, std::uint32_t pid) {
    auto guard = lock();
    std::vector<std::string> lines = split_output_lines(text);
    if (lines.empty()) {
        lines.emplace_back();
    }
    for (const std::string& line : lines) {
        OutputLine o;
        o.severity = severity;
        o.text = line;
        o.stage = stage;
        o.pid = pid;
        if (std::optional<CompilerMessage> parsed = parse_compiler_line(line)) {
            o.severity = parsed->severity;
            if (parsed->location.clickable()) {
                o.location = parsed->location;
            }
        }
        push_output(std::move(o));
    }
}

bool ViewModel::recently_shown(const std::string& diag_id) const {
    const std::size_t count = std::min<std::size_t>(output_.size(), 20);
    for (std::size_t i = 0; i < count; ++i) {
        const OutputLine& line = output_[output_.size() - 1 - i];
        if (line.diag_id == diag_id && line.stage != "command") {
            return true;
        }
    }
    return false;
}

void ViewModel::on_diag(const json::Value& body) {
    const std::string id = body.get_string("id");
    const std::string message = body.get_string("message");
    const std::string action = body.get_string("action");
    const std::string stage = body.get_string("stage");
    const std::uint32_t pid = static_cast<std::uint32_t>(unsigned_member(body, "pid"));
    const OutputSeverity severity = severity_from_text(body.get_string("severity", "error"));

    progress_.reset();

    // The answer of the command that caused this diagnostic may already have shown it.
    bool already = false;
    for (std::size_t i = output_.size(); i > 0 && output_.size() - i < 20; --i) {
        const OutputLine& line = output_[i - 1];
        if (line.diag_id == id && line.stage == "command") {
            already = true;
            break;
        }
    }

    if (!already) {
        std::vector<std::string> lines = split_output_lines(message);
        if (lines.empty()) {
            lines.emplace_back();
        }
        for (std::size_t i = 0; i < lines.size(); ++i) {
            OutputLine o;
            o.severity = i == 0 ? severity : OutputSeverity::Info;
            o.text = lines[i];
            o.stage = stage;
            o.pid = pid;
            if (i == 0) {
                o.diag_id = id;
            }
            if (std::optional<CompilerMessage> parsed = parse_compiler_line(lines[i])) {
                o.severity = parsed->severity;
                if (parsed->location.clickable()) {
                    o.location = parsed->location;
                }
            }
            push_output(std::move(o));
        }
        if (!action.empty()) {
            OutputLine o;
            o.severity = OutputSeverity::Info;
            o.text = "-> " + action;
            o.stage = stage;
            o.pid = pid;
            push_output(std::move(o));
        }
    }

    if (stage == "watch") {
        add_change(first_line_of(message), "watch", id);
    }
    if (id == "CML5551") {
        confirm_ = ConfirmPrompt{first_line_of(message), pid, std::string()};
    }
}

PatchRecord& ViewModel::upsert_patch(std::uint32_t pid, const std::string& module, std::uint64_t generation) {
    for (auto it = patches_.rbegin(); it != patches_.rend(); ++it) {
        if (it->pid == pid && it->module == module && it->generation == generation) {
            return *it;
        }
    }
    PatchRecord record;
    record.seq = next_seq_++;
    record.when_ms = now();
    record.pid = pid;
    record.module = module;
    record.generation = generation;
    patches_.push_back(std::move(record));
    trim_front(patches_, kMaxPatches);
    return patches_.back();
}

void ViewModel::on_patch_applied(const json::Value& body) {
    const std::uint32_t pid = static_cast<std::uint32_t>(unsigned_member(body, "pid"));
    const std::string module = body.get_string("module");
    const std::uint64_t generation = unsigned_member(body, "generation");
    PatchRecord& record = upsert_patch(pid, module, generation);
    record.hooks = unsigned_member(body, "hooks");
    record.new_functions = unsigned_member(body, "new_functions");
    record.new_globals = unsigned_member(body, "new_globals");
    generation_ = std::max(generation_, generation);
    progress_.reset();
    confirm_.reset();
    add_change(module, "module", "patched (generation " + std::to_string(generation) + ", " + std::to_string(record.hooks) + " hooks)");
}

void ViewModel::on_progress(const json::Value& body) {
    CompileProgress p;
    p.stage = body.get_string("stage");
    p.pid = static_cast<std::uint32_t>(unsigned_member(body, "pid"));
    p.module = body.get_string("module");
    p.done = unsigned_member(body, "done");
    p.total = unsigned_member(body, "total");
    progress_ = std::move(p);
}

void ViewModel::on_log(const json::Value& body) {
    LogLevel level = LogLevel::Info;
    parse_log_level(lower(body.get_string("level", "info")), &level);
    add_log(level, body.get_string("message"));
}

ExceptionRow ViewModel::parse_exception(const json::Value& body) {
    ExceptionRow row;
    row.pid = static_cast<std::uint32_t>(unsigned_member(body, "pid"));
    row.tid = static_cast<std::uint32_t>(unsigned_member(body, "tid"));
    row.exception_id = unsigned_member(body, "exception_id");
    row.code = text_or_number(body, "code");
    row.address = text_or_number(body, "address");
    row.module = body.get_string("module");
    row.function = body.get_string("function");
    row.offset = text_or_number(body, "offset");
    if (const json::Value* actions = body.find("actions"); actions != nullptr && actions->is_array()) {
        for (const json::Value& a : actions->items()) {
            if (a.is_string()) {
                row.actions.push_back(a.as_string());
            }
        }
    }
    if (row.actions.empty()) {
        row.actions = {"continue", "leave", "ignore"};
    }
    return row;
}

void ViewModel::on_exception(const json::Value& body) {
    ExceptionRow row = parse_exception(body);
    for (const ExceptionRow& existing : exceptions_) {
        if (existing.pid == row.pid && existing.exception_id == row.exception_id) {
            return;  // already pending (the `exceptions` command and the event both report it)
        }
    }
    add_log(LogLevel::Warn, "exception " + row.code + " in " + (row.function.empty() ? row.module : row.function) + " (process " +
                                std::to_string(row.pid) + ")");
    exceptions_.push_back(std::move(row));
    trim_front(exceptions_, kMaxExceptions);
}

void ViewModel::on_restarted(const json::Value& body) {
    const std::uint32_t old_pid = static_cast<std::uint32_t>(unsigned_member(body, "old_pid"));
    const std::uint32_t new_pid = static_cast<std::uint32_t>(unsigned_member(body, "new_pid"));
    exceptions_.erase(std::remove_if(exceptions_.begin(), exceptions_.end(), [&](const ExceptionRow& e) { return e.pid == old_pid; }),
                      exceptions_.end());
    add_log(LogLevel::Info, "process " + std::to_string(old_pid) + " restarted as " + std::to_string(new_pid));
    set_status_message("Restarted process " + std::to_string(old_pid) + " as " + std::to_string(new_pid), StatusSeverity::Info);
}

void ViewModel::on_patch_reverted(const json::Value& body) {
    const std::uint32_t pid = static_cast<std::uint32_t>(unsigned_member(body, "pid"));
    const std::string module = body.get_string("module");
    const std::uint64_t generation = unsigned_member(body, "generation");
    for (PatchRecord& p : patches_) {
        if (p.pid == pid && p.module == module && p.generation == generation) {
            p.reverted = true;
        }
    }
    add_log(LogLevel::Info, "patch reverted: " + module + " (process " + std::to_string(pid) + ", generation " + std::to_string(generation) + ")");
    add_change(module, "module", "patch reverted (generation " + std::to_string(generation) + ")");
}

// --- command results -------------------------------------------------------------------------------------------

void ViewModel::apply_reload_result(const ReloadSummary& summary) {
    auto guard = lock();
    progress_.reset();
    confirm_.reset();
    generation_ = std::max(generation_, summary.generation);
    for (const ReloadPatch& patch : summary.patches) {
        PatchRecord& record = upsert_patch(patch.pid, patch.module, patch.generation);
        record.hooks = patch.hooks_installed;
        record.new_functions = patch.new_functions;
        record.new_globals = patch.new_globals;
        if (patch.timing.present) {
            record.timing = patch.timing;
        }
    }
    if (summary.patches.empty() && summary.patched > 0) {
        PatchRecord& record = upsert_patch(0, "(all modules)", summary.generation);
        record.hooks = summary.hooks_installed;
        record.timing = summary.timing;
    }
    const char* state = summary.patched > 0 ? "patched" : "no patch";
    for (const std::string& file : summary.changed_files) {
        add_change(file, state, summary.patched > 0 ? "generation " + std::to_string(summary.generation) : "nothing to compile");
    }
    if (summary.patched > 0) {
        set_status_message("Patched " + std::to_string(summary.patched) + " compiland(s)" +
                               (summary.timing.present ? " in " + std::to_string(static_cast<long long>(summary.timing.total + 0.5)) + " ms" : ""),
                           StatusSeverity::Info);
    } else {
        set_status_message("Reload finished: no changes to patch", StatusSeverity::Info);
    }
}

void ViewModel::apply_restart_result(const RestartSummary& summary) {
    auto guard = lock();
    exceptions_.erase(std::remove_if(exceptions_.begin(), exceptions_.end(), [&](const ExceptionRow& e) { return e.pid == summary.old_pid; }),
                      exceptions_.end());
    set_status_message("Restarted process " + std::to_string(summary.old_pid) + " as " + std::to_string(summary.new_pid) + " (" +
                           std::to_string(summary.modules.size()) + " module(s) re-enabled)",
                       StatusSeverity::Info);
}

void ViewModel::apply_revert_result(const RevertSummary& summary) {
    auto guard = lock();
    for (const RevertedPatch& r : summary.reverted) {
        for (PatchRecord& p : patches_) {
            if (p.pid == r.pid && p.module == r.module && p.generation == r.generation) {
                p.reverted = true;
            }
        }
    }
    set_status_message("Reverted " + std::to_string(summary.reverted.size()) + " patch(es)", StatusSeverity::Info);
}

void ViewModel::apply_command_failure(const std::string& cmd, const Diag& diag, std::uint32_t pid, const std::string& module) {
    auto guard = lock();
    progress_.reset();
    if (diag.code == 5402) {
        mark_command_unavailable(cmd);
        set_status_message("'" + cmd + "' is not available in this broker", StatusSeverity::Warning);
        return;
    }
    if (cmd == "reload" && diag.code == 5551) {
        confirm_ = ConfirmPrompt{diag.message, pid, module};
        set_status_message("This reload recompiles many files; confirm to continue", StatusSeverity::Warning);
    } else {
        set_status_message(diag.id() + ": " + diag.message, StatusSeverity::Error);
    }
    const std::string id = diag.id();
    if (!recently_shown(id)) {
        std::vector<std::string> lines = split_output_lines(diag.message);
        if (lines.empty()) {
            lines.emplace_back();
        }
        for (std::size_t i = 0; i < lines.size(); ++i) {
            OutputLine o;
            o.severity = i == 0 ? OutputSeverity::Error : OutputSeverity::Info;
            o.text = lines[i];
            o.stage = "command";
            o.pid = pid;
            if (i == 0) {
                o.diag_id = id;
            }
            if (std::optional<CompilerMessage> parsed = parse_compiler_line(lines[i])) {
                o.severity = parsed->severity;
                if (parsed->location.clickable()) {
                    o.location = parsed->location;
                }
            }
            push_output(std::move(o));
        }
        if (!diag.what_to_do.empty()) {
            OutputLine o;
            o.severity = OutputSeverity::Info;
            o.text = "-> " + diag.what_to_do;
            o.stage = "command";
            o.pid = pid;
            push_output(std::move(o));
        }
    }
}

void ViewModel::set_reload_in_flight(bool in_flight) {
    auto guard = lock();
    reload_in_flight_ = in_flight;
}

void ViewModel::set_status_message(const std::string& text, StatusSeverity severity) {
    auto guard = lock();
    status_message_ = text;
    status_severity_ = severity;
}

// --- lists -----------------------------------------------------------------------------------------------------

void ViewModel::clear_output() {
    auto guard = lock();
    output_.clear();
}

std::size_t ViewModel::error_count() const {
    auto guard = lock();
    return static_cast<std::size_t>(std::count_if(output_.begin(), output_.end(), [](const OutputLine& l) { return l.severity == OutputSeverity::Error; }));
}

std::size_t ViewModel::warning_count() const {
    auto guard = lock();
    return static_cast<std::size_t>(std::count_if(output_.begin(), output_.end(), [](const OutputLine& l) { return l.severity == OutputSeverity::Warning; }));
}

void ViewModel::add_change(const std::string& path, const std::string& state, const std::string& detail) {
    ChangeRecord record;
    record.seq = next_seq_++;
    record.when_ms = now();
    record.path = path;
    record.state = state;
    record.detail = detail;
    changes_.push_back(std::move(record));
    trim_front(changes_, kMaxChanges);
}

void ViewModel::clear_changes() {
    auto guard = lock();
    changes_.clear();
}

void ViewModel::clear_log() {
    auto guard = lock();
    log_.clear();
}

void ViewModel::add_log(LogLevel level, const std::string& message) {
    auto guard = lock();
    LogLine line;
    line.seq = next_seq_++;
    line.when_ms = now();
    line.level = level;
    line.message = message;
    log_.push_back(std::move(line));
    trim_front(log_, kMaxLog);
}

std::vector<std::size_t> ViewModel::filtered_log(const LogFilter& filter) const {
    auto guard = lock();
    const std::string needle = lower(filter.text);
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < log_.size(); ++i) {
        const LogLine& line = log_[i];
        if (static_cast<int>(line.level) < static_cast<int>(filter.min_level)) {
            continue;
        }
        if (!needle.empty() && lower(line.message).find(needle) == std::string::npos) {
            continue;
        }
        out.push_back(i);
    }
    return out;
}

// --- exceptions / confirm --------------------------------------------------------------------------------------

const ExceptionRow* ViewModel::current_exception() const {
    auto guard = lock();
    return exceptions_.empty() ? nullptr : &exceptions_.front();
}

void ViewModel::resolve_exception(std::uint32_t pid, std::uint64_t exception_id) {
    auto guard = lock();
    exceptions_.erase(std::remove_if(exceptions_.begin(), exceptions_.end(),
                                     [&](const ExceptionRow& e) { return e.pid == pid && e.exception_id == exception_id; }),
                      exceptions_.end());
}

void ViewModel::set_pending_exceptions(const std::vector<ExceptionRow>& rows) {
    auto guard = lock();
    for (const ExceptionRow& row : rows) {
        bool known = false;
        for (const ExceptionRow& e : exceptions_) {
            known = known || (e.pid == row.pid && e.exception_id == row.exception_id);
        }
        if (!known) {
            exceptions_.push_back(row);
        }
    }
    trim_front(exceptions_, kMaxExceptions);
}

void ViewModel::clear_confirm_prompt() {
    auto guard = lock();
    confirm_.reset();
}

}  // namespace cml::ui
