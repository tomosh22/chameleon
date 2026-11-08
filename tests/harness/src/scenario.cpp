#include "scenario.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

#include "cml/ipc/control.h"
#include "fault_step.h"
#include "restart_step.h"
#include "rx.h"
#include "toml.h"

namespace cmltest {

const char* to_string(StepKind kind) {
    switch (kind) {
        case StepKind::Launch: return "launch";
        case StepKind::ExpectStdout: return "expect_stdout";
        case StepKind::ExpectModuleEnabled: return "expect_module_enabled";
        case StepKind::Edit: return "edit";
        case StepKind::Reload: return "reload";
        case StepKind::ExpectDiag: return "expect_diag";
        case StepKind::ExpectMonotonic: return "expect_monotonic";
        case StepKind::SetOption: return "set_option";
        case StepKind::Wait: return "wait";
        case StepKind::ExpectEvent: return "expect_event";
        case StepKind::SignalFixture: return "signal_fixture";
        case StepKind::ExpectReloadResult: return "expect_reload_result";
        case StepKind::Restart: return "restart";
        case StepKind::Revert: return "revert";
        case StepKind::InjectFailure: return "inject_failure";
        case StepKind::WatchExceptions: return "watch_exceptions";
        case StepKind::ExpectException: return "expect_exception";
        case StepKind::ExceptionAction: return "exception_action";
        case StepKind::ExpectPendingExceptions: return "expect_pending_exceptions";
    }
    return "?";
}

std::string Step::describe() const {
    std::string out = to_string(kind);
    switch (kind) {
        case StepKind::Launch:
            if (!exe.empty()) {
                out += " " + exe + (label.empty() ? std::string() : " as " + label);
            }
            if (!args.empty()) {
                out += " [" + args + "]";
            }
            break;
        case StepKind::ExpectStdout:
            out += " /" + regex + "/ within " + std::to_string(within_s) + " s";
            if (!process.empty()) {
                out += " (" + process + ")";
            }
            break;
        case StepKind::ExpectModuleEnabled:
            out += " " + module + " within " + std::to_string(within_s) + " s";
            if (!process.empty()) {
                out += " (" + process + ")";
            }
            break;
        case StepKind::ExpectReloadResult:
            out += " " + key + "=" + std::to_string(result_value);
            break;
        case StepKind::Edit:
            out += " " + patch;
            break;
        case StepKind::Reload:
            out += " within " + std::to_string(within_s) + " s";
            if (has_result_value) {
                out += " expecting " + result_key + "=" + std::to_string(result_value);
            }
            if (!expect_error.empty()) {
                out += " expecting the refusal " + expect_error;
            }
            break;
        case StepKind::SignalFixture:
            break;
        case StepKind::Revert:
            out += " " + revert_scope + " within " + std::to_string(within_s) + " s";
            if (!expect_error.empty()) {
                out += " expecting the refusal " + expect_error;
            }
            break;
        case StepKind::ExpectMonotonic:
            out += " " + key + " (at least " + std::to_string(min_samples) + " samples)";
            break;
        case StepKind::ExpectDiag:
            out += " " + id + " within " + std::to_string(within_s) + " s";
            break;
        case StepKind::SetOption:
            out += " " + option + " = " +
                   (option_kind == OptionKind::Bool ? (option_bool ? "true" : "false")
                    : option_kind == OptionKind::Integer ? std::to_string(option_integer)
                                                         : "\"" + option_string + "\"");
            break;
        case StepKind::Wait:
            out += " for " + wait_for + " within " + std::to_string(within_s) + " s";
            break;
        case StepKind::ExpectEvent:
            out += " " + event + " within " + std::to_string(within_s) + " s";
            break;
        case StepKind::Restart:
            out += " within " + std::to_string(within_s) + " s";
            if (has_result_value) {
                out += " expecting " + result_key + "=" + std::to_string(result_value);
            }
        case StepKind::WatchExceptions:
            break;
        case StepKind::ExpectException:
            out += (exception_code.empty() ? std::string() : " " + exception_code) + (exception_function.empty() ? std::string() : " in " + exception_function) +
                   " within " + std::to_string(within_s) + " s";
            break;
        case StepKind::ExceptionAction:
            out += " " + exception_action + (expect_error.empty() ? std::string() : " expecting the refusal " + expect_error);
            break;
        case StepKind::ExpectPendingExceptions:
            out += " " + std::to_string(result_value);
            break;
        case StepKind::InjectFailure:
            out += " " + fault_stage + (fault_count != 1 ? " x" + std::to_string(fault_count) : std::string()) +
                   (expect_error.empty() ? std::string() : " expecting the refusal " + expect_error);
            break;
    }
    return out;
}

const char* Step::required_capability() const {
    switch (kind) {
        case StepKind::Reload: return cml::ipc::capability::kReload;
        case StepKind::Revert: return cml::ipc::capability::kRevert;
        case StepKind::ExpectDiag: return cml::ipc::capability::kDiagEvents;
        case StepKind::SetOption: return cml::ipc::capability::kSetOption;
        case StepKind::Wait: return cml::ipc::capability::kWait;
        case StepKind::ExpectEvent: return cml::ipc::capability::kSubscribe;
        case StepKind::Restart: return cml::ipc::capability::kRestart;
        case StepKind::InjectFailure: return cml::ipc::capability::kFaultInjection;
        case StepKind::WatchExceptions:
        case StepKind::ExpectException:
        case StepKind::ExceptionAction:
        case StepKind::ExpectPendingExceptions: return cml::ipc::capability::kExceptions;
        default: return nullptr;
    }
}

namespace {

cml::Diag invalid(int line, const std::string& what) {
    return cml::Diag::make(cml::ids::kScenarioInvalid,
                           "scenario line " + std::to_string(line) + ": " + what,
                           "Fix the scenario file (tests/harness/README.md lists the keys of every step kind).");
}

bool name_chars_ok(const std::string& s, bool allow_dot) {
    return !s.empty() && s.size() <= 64 &&
           std::all_of(s.begin(), s.end(), [&](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                      (allow_dot && c == '.');
           });
}

cml::Result<std::string> require_string(const toml::Table& table, const char* key, const char* context) {
    const toml::Value* v = table.find(key);
    if (v == nullptr) {
        return invalid(table.line, std::string(context) + " needs the key '" + key + "'");
    }
    if (v->kind != toml::Value::Kind::String) {
        return invalid(v->line, std::string("'") + key + "' must be a string");
    }
    return v->text;
}

cml::Result<int> int_in_range(const toml::Table& table, const char* key, int fallback, int lo, int hi) {
    const toml::Value* v = table.find(key);
    if (v == nullptr) {
        return fallback;
    }
    if (v->kind != toml::Value::Kind::Integer || v->integer < lo || v->integer > hi) {
        return invalid(v->line, std::string("'") + key + "' must be an integer between " + std::to_string(lo) + " and " +
                                    std::to_string(hi));
    }
    return static_cast<int>(v->integer);
}

cml::Result<void> only_keys(const toml::Table& table, std::initializer_list<const char*> allowed, const char* context) {
    for (const auto& entry : table.entries) {
        const bool known = std::any_of(allowed.begin(), allowed.end(), [&](const char* k) { return entry.first == k; });
        if (!known) {
            return invalid(entry.second.line, "unknown key '" + entry.first + "' in " + context);
        }
    }
    return cml::Result<void>();
}

// A launched exe's default label: its file name without a (case-insensitive) ".exe" suffix, like the runner's lookup.
std::string default_label(const std::string& exe) {
    if (exe.size() > 4) {
        std::string tail = exe.substr(exe.size() - 4);
        std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (tail == ".exe") {
            return exe.substr(0, exe.size() - 4);
        }
    }
    return exe;
}

cml::Result<Step> parse_step_kind(const toml::Table& table);

cml::Result<Step> parse_step(const toml::Table& table) {
    cml::Result<Step> step = parse_step_kind(table);
    if (!step) {
        return step;
    }
    if (const toml::Value* p = table.find("process")) {
        if (p->kind != toml::Value::Kind::String || !name_chars_ok(p->text, false)) {
            return invalid(p->line, "'process' must be a process label (letters, digits, '_', '-')");
        }
        step->process = p->text;
    }
    return step;
}

cml::Result<Step> parse_step_kind(const toml::Table& table) {
    Step step;
    step.line = table.line;
    cml::Result<std::string> kind = require_string(table, "kind", "[[step]]");
    if (!kind) {
        return kind.error();
    }
    const char* context = "this step";
    if (*kind == "launch") {
        step.kind = StepKind::Launch;
        cml::Result<void> keys = only_keys(table, {"kind", "exe", "label", "args"}, "a launch step");
        if (!keys) return keys.error();
        if (table.find("exe") != nullptr) {
            cml::Result<std::string> exe = require_string(table, "exe", "a launch step");
            if (!exe) return exe.error();
            if (!name_chars_ok(*exe, true)) return invalid(table.line, "'exe' must be an executable file name of the fixture build");
            step.exe = *exe;
        }
        if (table.find("args") != nullptr) {
            cml::Result<std::string> args = require_string(table, "args", "a launch step");
            if (!args) return args.error();
            if (args->size() > 512 || args->find('"') != std::string::npos) return invalid(table.line, "'args' must be at most 512 characters and hold no double quote");
            step.args = *args;
        }
        if (table.find("label") != nullptr) {
            cml::Result<std::string> label = require_string(table, "label", "a launch step");
            if (!label) return label.error();
            if (!name_chars_ok(*label, false)) return invalid(table.line, "'label' must be 1-64 characters of a-z, A-Z, 0-9, '_', '-'");
            step.label = *label;
        }
        return step;
    }
    if (*kind == "expect_stdout") {
        step.kind = StepKind::ExpectStdout;
        cml::Result<void> keys = only_keys(table, {"kind", "regex", "within_s", "process"}, "an expect_stdout step");
        if (!keys) return keys.error();
        cml::Result<std::string> regex = require_string(table, "regex", "an expect_stdout step");
        if (!regex) return regex.error();
        cml::Result<Regex> compiled = Regex::compile(*regex);
        if (!compiled) {
            return compiled.error();
        }
        step.regex = *regex;
        cml::Result<int> within = int_in_range(table, "within_s", 10, 1, 3600);
        if (!within) return within.error();
        step.within_s = *within;
        return step;
    }
    if (*kind == "expect_module_enabled") {
        step.kind = StepKind::ExpectModuleEnabled;
        cml::Result<void> keys = only_keys(table, {"kind", "module", "within_s", "process"}, "an expect_module_enabled step");
        if (!keys) return keys.error();
        cml::Result<std::string> module = require_string(table, "module", "an expect_module_enabled step");
        if (!module) return module.error();
        if (module->empty()) return invalid(table.line, "'module' must not be empty");
        step.module = *module;
        cml::Result<int> within = int_in_range(table, "within_s", 10, 1, 3600);
        if (!within) return within.error();
        step.within_s = *within;
        return step;
    }
    if (*kind == "edit") {
        step.kind = StepKind::Edit;
        cml::Result<void> keys = only_keys(table, {"kind", "patch"}, "an edit step");
        if (!keys) return keys.error();
        cml::Result<std::string> patch = require_string(table, "patch", "an edit step");
        if (!patch) return patch.error();
        const std::filesystem::path p(*patch);
        if (patch->empty() || p.is_absolute() || p.has_root_name() || patch->find("..") != std::string::npos ||
            patch->find(':') != std::string::npos || (*patch)[0] == '/' || (*patch)[0] == '\\') {
            return invalid(table.line, "'patch' must be a relative path below the fixture directory");
        }
        step.patch = *patch;
        return step;
    }
    if (*kind == "reload") {
        step.kind = StepKind::Reload;
        cml::Result<void> keys = only_keys(table, {"kind", "within_s", "expect_result_key", "expect_result_value", "expect_error"}, "a reload step");
        if (!keys) return keys.error();
        cml::Result<int> within = int_in_range(table, "within_s", 60, 1, 3600);
        if (!within) return within.error();
        step.within_s = *within;
        const toml::Value* rk = table.find("expect_result_key");
        const toml::Value* rv = table.find("expect_result_value");
        if ((rk == nullptr) != (rv == nullptr)) {
            return invalid(table.line, "'expect_result_key' and 'expect_result_value' go together");
        }
        if (rk != nullptr) {
            if (rk->kind != toml::Value::Kind::String || !name_chars_ok(rk->text, false)) {
                return invalid(rk->line, "'expect_result_key' must be a member name (letters, digits, '_', '-')");
            }
            if (rv->kind != toml::Value::Kind::Integer) {
                return invalid(rv->line, "'expect_result_value' must be an integer");
            }
            step.result_key = rk->text;
            step.result_value = rv->integer;
            step.has_result_value = true;
        }
        if (const toml::Value* ee = table.find("expect_error")) {
            const bool shape = ee->kind == toml::Value::Kind::String && ee->text.size() == 7 && ee->text.compare(0, 3, "CML") == 0 &&
                               std::all_of(ee->text.begin() + 3, ee->text.end(), [](char c) { return c >= '0' && c <= '9'; });
            if (!shape) return invalid(ee->line, "'expect_error' must look like CML5550");
            if (rk != nullptr) return invalid(ee->line, "'expect_error' and 'expect_result_key' exclude each other");
            step.expect_error = ee->text;
        }
        return step;
    }
    if (*kind == "expect_diag") {
        step.kind = StepKind::ExpectDiag;
        cml::Result<void> keys = only_keys(table, {"kind", "id", "within_s"}, "an expect_diag step");
        if (!keys) return keys.error();
        cml::Result<std::string> id = require_string(table, "id", "an expect_diag step");
        if (!id) return id.error();
        const bool shape = id->size() == 7 && id->compare(0, 3, "CML") == 0 &&
                           std::all_of(id->begin() + 3, id->end(), [](char c) { return c >= '0' && c <= '9'; });
        if (!shape) return invalid(table.line, "'id' must look like CML3101");
        step.id = *id;
        cml::Result<int> within = int_in_range(table, "within_s", 10, 1, 3600);
        if (!within) return within.error();
        step.within_s = *within;
        return step;
    }
    if (*kind == "expect_monotonic") {
        step.kind = StepKind::ExpectMonotonic;
        cml::Result<void> keys = only_keys(table, {"kind", "key", "min_samples", "process"}, "an expect_monotonic step");
        if (!keys) return keys.error();
        cml::Result<std::string> key = require_string(table, "key", "an expect_monotonic step");
        if (!key) return key.error();
        if (!name_chars_ok(*key, false)) return invalid(table.line, "'key' must be a TICK key (letters, digits, '_', '-')");
        step.key = *key;
        cml::Result<int> samples = int_in_range(table, "min_samples", 10, 2, 100000);
        if (!samples) return samples.error();
        step.min_samples = *samples;
        return step;
    }
    if (*kind == "set_option") {
        step.kind = StepKind::SetOption;
        cml::Result<void> keys = only_keys(table, {"kind", "option", "value"}, "a set_option step");
        if (!keys) return keys.error();
        cml::Result<std::string> option = require_string(table, "option", "a set_option step");
        if (!option) return option.error();
        if (!name_chars_ok(*option, false)) return invalid(table.line, "'option' must be an option name (letters, digits, '_', '-')");
        step.option = *option;
        const toml::Value* value = table.find("value");
        if (value == nullptr) return invalid(table.line, "a set_option step needs the key 'value'");
        switch (value->kind) {
            case toml::Value::Kind::Boolean:
                step.option_kind = Step::OptionKind::Bool;
                step.option_bool = value->boolean;
                break;
            case toml::Value::Kind::Integer:
                step.option_kind = Step::OptionKind::Integer;
                step.option_integer = value->integer;
                break;
            case toml::Value::Kind::String:
                step.option_kind = Step::OptionKind::String;
                step.option_string = value->text;
                break;
        }
        return step;
    }
    if (*kind == "wait") {
        step.kind = StepKind::Wait;
        cml::Result<void> keys = only_keys(table, {"kind", "for", "within_s"}, "a wait step");
        if (!keys) return keys.error();
        cml::Result<std::string> what = require_string(table, "for", "a wait step");
        if (!what) return what.error();
        if (*what != "index" && *what != "idle") return invalid(table.line, "'for' must be \"index\" or \"idle\"");
        step.wait_for = *what;
        cml::Result<int> within = int_in_range(table, "within_s", 60, 1, 3600);
        if (!within) return within.error();
        step.within_s = *within;
        return step;
    }
    if (*kind == "expect_event") {
        step.kind = StepKind::ExpectEvent;
        cml::Result<void> keys = only_keys(table, {"kind", "event", "within_s"}, "an expect_event step");
        if (!keys) return keys.error();
        cml::Result<std::string> event = require_string(table, "event", "an expect_event step");
        if (!event) return event.error();
        if (!name_chars_ok(*event, false)) return invalid(table.line, "'event' must be an event name (letters, digits, '_', '-')");
        step.event = *event;
        cml::Result<int> within = int_in_range(table, "within_s", 10, 1, 3600);
        if (!within) return within.error();
        step.within_s = *within;
        return step;
    }
    if (*kind == "restart") {
        return parse_restart_step(table);  // restart_step.cpp
    }
    if (*kind == "inject_failure") {
        return parse_inject_step(table);  // fault_step.cpp (WP-28)
    }
    if (*kind == "revert") {
        step.kind = StepKind::Revert;
        cml::Result<void> keys = only_keys(table, {"kind", "scope", "within_s", "expect_error"}, "a revert step");
        if (!keys) return keys.error();
        if (const toml::Value* scope = table.find("scope")) {
            if (scope->kind != toml::Value::Kind::String || (scope->text != "last" && scope->text != "all")) {
                return invalid(scope->line, "'scope' must be \"last\" or \"all\"");
            }
            step.revert_scope = scope->text;
        }
        cml::Result<int> within = int_in_range(table, "within_s", 30, 1, 3600);
        if (!within) return within.error();
        step.within_s = *within;
        if (const toml::Value* ee = table.find("expect_error")) {
            const bool shape = ee->kind == toml::Value::Kind::String && ee->text.size() == 7 && ee->text.compare(0, 3, "CML") == 0 &&
                               std::all_of(ee->text.begin() + 3, ee->text.end(), [](char c) { return c >= '0' && c <= '9'; });
            if (!shape) return invalid(ee->line, "'expect_error' must look like CML5850");
            step.expect_error = ee->text;
        }
        return step;
    }
    if (*kind == "signal_fixture") {
        step.kind = StepKind::SignalFixture;
        cml::Result<void> keys = only_keys(table, {"kind"}, "a signal_fixture step");
        if (!keys) return keys.error();
        return step;
    }
    if (*kind == "expect_reload_result") {
        step.kind = StepKind::ExpectReloadResult;
        cml::Result<void> keys = only_keys(table, {"kind", "key", "value"}, "an expect_reload_result step");
        if (!keys) return keys.error();
        cml::Result<std::string> key = require_string(table, "key", "an expect_reload_result step");
        if (!key) return key.error();
        if (!name_chars_ok(*key, false)) return invalid(table.line, "'key' must be a member name (letters, digits, '_', '-')");
        const toml::Value* value = table.find("value");
        if (value == nullptr || value->kind != toml::Value::Kind::Integer) {
            return invalid(table.line, "an expect_reload_result step needs an integer 'value'");
        }
        step.key = *key;
        step.result_value = value->integer;
        step.has_result_value = true;
        return step;
    }
    if (*kind == "watch_exceptions") {
        step.kind = StepKind::WatchExceptions;
        cml::Result<void> keys = only_keys(table, {"kind"}, "a watch_exceptions step");
        if (!keys) return keys.error();
        return step;
    }
    if (*kind == "expect_exception") {
        step.kind = StepKind::ExpectException;
        cml::Result<void> keys = only_keys(table, {"kind", "code", "function", "within_s"}, "an expect_exception step");
        if (!keys) return keys.error();
        if (const toml::Value* code = table.find("code")) {
            const bool shape = code->kind == toml::Value::Kind::String && code->text.size() == 10 && code->text.compare(0, 2, "0x") == 0 &&
                               std::all_of(code->text.begin() + 2, code->text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
            if (!shape) return invalid(code->line, "'code' must look like \"0xC0000005\"");
            step.exception_code = code->text;
        }
        if (const toml::Value* function = table.find("function")) {
            if (function->kind != toml::Value::Kind::String || function->text.empty() || function->text.size() > 200) {
                return invalid(function->line, "'function' must be a non-empty string (a substring of the function name)");
            }
            step.exception_function = function->text;
        }
        cml::Result<int> within = int_in_range(table, "within_s", 30, 1, 3600);
        if (!within) return within.error();
        step.within_s = *within;
        return step;
    }
    if (*kind == "exception_action") {
        step.kind = StepKind::ExceptionAction;
        cml::Result<void> keys = only_keys(table, {"kind", "action", "expect_error"}, "an exception_action step");
        if (!keys) return keys.error();
        cml::Result<std::string> action = require_string(table, "action", "an exception_action step");
        if (!action) return action.error();
        if (*action != "continue" && *action != "leave" && *action != "ignore") {
            return invalid(table.line, "'action' must be \"continue\", \"leave\" or \"ignore\"");
        }
        step.exception_action = *action;
        if (const toml::Value* ee = table.find("expect_error")) {
            const bool shape = ee->kind == toml::Value::Kind::String && ee->text.size() == 7 && ee->text.compare(0, 3, "CML") == 0 &&
                               std::all_of(ee->text.begin() + 3, ee->text.end(), [](char c) { return c >= '0' && c <= '9'; });
            if (!shape) return invalid(ee->line, "'expect_error' must look like CML4401");
            step.expect_error = ee->text;
        }
        return step;
    }
    if (*kind == "expect_pending_exceptions") {
        step.kind = StepKind::ExpectPendingExceptions;
        cml::Result<void> keys = only_keys(table, {"kind", "count"}, "an expect_pending_exceptions step");
        if (!keys) return keys.error();
        const toml::Value* count = table.find("count");
        if (count == nullptr || count->kind != toml::Value::Kind::Integer || count->integer < 0 || count->integer > 64) {
            return invalid(table.line, "an expect_pending_exceptions step needs an integer 'count' from 0 to 64");
        }
        step.result_value = count->integer;
        return step;
    }
    (void)context;
    return invalid(table.line, "unknown step kind '" + *kind +
                                   "' (launch, expect_stdout, expect_module_enabled, edit, reload, expect_diag, expect_monotonic, set_option, wait, "
                                   "expect_event, expect_reload_result, signal_fixture, revert, watch_exceptions, expect_exception, exception_action, "
                                   "expect_pending_exceptions, restart, inject_failure)");
}

}  // namespace

cml::Result<Scenario> parse_scenario(std::string_view text) {
    cml::Result<toml::Document> doc = toml::parse(text);
    if (!doc) {
        return doc.error();
    }
    Scenario scenario;
    cml::Result<void> root_keys = only_keys(doc->root, {"name", "description", "fixture", "fixture_args", "timeout_s"}, "the scenario header");
    if (!root_keys) return root_keys.error();
    if (!doc->tables.empty()) {
        return invalid(doc->tables[0].line, "plain tables are not used in scenarios (only [[step]])");
    }
    for (const toml::Table& t : doc->array_tables) {
        if (t.name != "step") {
            return invalid(t.line, "unknown array of tables [[" + t.name + "]] (only [[step]] exists)");
        }
    }
    cml::Result<std::string> name = require_string(doc->root, "name", "the scenario");
    if (!name) return name.error();
    if (!name_chars_ok(*name, true)) return invalid(doc->root.find("name")->line, "'name' must be 1-64 characters of a-z, A-Z, 0-9, '_', '-' or '.'");
    scenario.name = *name;
    cml::Result<std::string> fixture = require_string(doc->root, "fixture", "the scenario");
    if (!fixture) return fixture.error();
    if (!name_chars_ok(*fixture, false)) return invalid(doc->root.find("fixture")->line, "'fixture' must be a fixture directory name (tests/fixtures/<name>)");
    scenario.fixture = *fixture;
    if (const toml::Value* args = doc->root.find("fixture_args")) {
        const bool plain = args->kind == toml::Value::Kind::String && args->text.size() <= 200 &&
                           std::all_of(args->text.begin(), args->text.end(), [](char ch) {
                               return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
                                      ch == '_' || ch == '=' || ch == '.' || ch == ' ';
                           });
        if (!plain) {
            return invalid(args->line, "'fixture_args' must be a string of letters, digits, spaces and - _ = .");
        }
        scenario.fixture_args = args->text;
    }
    if (const toml::Value* d = doc->root.find("description")) {
        if (d->kind != toml::Value::Kind::String) return invalid(d->line, "'description' must be a string");
        scenario.description = d->text;
    }
    cml::Result<int> timeout = int_in_range(doc->root, "timeout_s", 120, 10, 7200);
    if (!timeout) return timeout.error();
    scenario.timeout_s = *timeout;

    bool launched = false;
    std::vector<std::string> labels;  // processes launched so far (WP-16)
    for (const toml::Table& t : doc->array_tables) {
        cml::Result<Step> step = parse_step(t);
        if (!step) {
            return step.error();
        }
        if (step->kind == StepKind::Launch) {
            if (launched && step->exe.empty()) {
                return invalid(t.line, "a scenario can launch the primary fixture only once; further launches need 'exe' (another executable of the fixture)");
            }
            const std::string label = !step->label.empty() ? step->label : step->exe.empty() ? std::string("fixture") : default_label(step->exe);
            if (std::find(labels.begin(), labels.end(), label) != labels.end()) {
                return invalid(t.line, "the process label '" + label + "' is used twice (give the launch a 'label')");
            }
            step->label = label;
            labels.push_back(label);
            launched = true;
        } else if (!step->process.empty() && std::find(labels.begin(), labels.end(), step->process) == labels.end()) {
            return invalid(t.line, "the process '" + step->process + "' is not launched before this step");
        } else if (step->kind != StepKind::Edit && !launched) {
            return invalid(t.line, std::string(to_string(step->kind)) + " needs a launch step before it");
        }
        scenario.steps.push_back(std::move(*step));
    }
    if (scenario.steps.empty()) {
        return invalid(1, "the scenario has no [[step]]");
    }
    return scenario;
}

cml::Result<Scenario> load_scenario(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return cml::Diag::make(cml::ids::kHarnessUsage, "cannot read the scenario file '" + path.string() + "'",
                               "Pass the path of an existing scenario.toml with --scenario.");
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    cml::Result<Scenario> scenario = parse_scenario(buffer.str());
    if (!scenario) {
        return cml::Diag::make(scenario.error().code, path.string() + ": " + scenario.error().message,
                               scenario.error().what_to_do);
    }
    scenario->path = path;
    return scenario;
}

}  // namespace cmltest
