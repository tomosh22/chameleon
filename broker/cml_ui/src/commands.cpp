#include "cml/ui/commands.h"

#include <algorithm>

#include "cml/ipc/control.h"

namespace cml::ui {

namespace json = ipc::json;

const std::vector<CommandInfo>& command_table() {
    static const std::vector<CommandInfo> table = {
        {"status", ipc::capability::kStatus, "process and module list, connection state"},
        {"reload", ipc::capability::kReload, "Reload button, global hotkey, per-process reload, confirm prompt (CML5551)"},
        {"wait", ipc::capability::kWait, "Wait for idle / index"},
        {"set-option", ipc::capability::kSetOption, "settings: compile on save, jobs, max affected compilands, log level"},
        {"subscribe", ipc::capability::kSubscribe, "compile output, change list, patch history, log view"},
        {"exceptions", "exceptions", "pending exceptions list"},
        {"exception-action", "exceptions", "exception dialog: Continue / Leave function / Ignore"},
        {"exception-action", "reexecute", "exception dialog: Re-execute function button (offered when `actions` has \"reexecute\"; `exception_updated` events)"},
        {"restart", "restart", "Restart process button"},
        {"revert", "revert", "Revert last / Revert all buttons"},
    };
    return table;
}

const std::vector<std::string>& passive_capabilities() {
    static const std::vector<std::string> passive = {
        ipc::capability::kDiagEvents,  // `diag` events: the compile output panel
    };
    return passive;
}

std::vector<std::string> uncovered_capabilities(const std::vector<std::string>& advertised) {
    std::vector<std::string> out;
    for (const std::string& capability : advertised) {
        bool covered = std::find(passive_capabilities().begin(), passive_capabilities().end(), capability) != passive_capabilities().end();
        for (const CommandInfo& c : command_table()) {
            covered = covered || capability == c.capability;
        }
        if (!covered) {
            out.push_back(capability);
        }
    }
    return out;
}

std::vector<std::string> unavailable_commands(const std::vector<std::string>& advertised) {
    std::vector<std::string> out;
    for (const CommandInfo& c : command_table()) {
        if (std::find(advertised.begin(), advertised.end(), c.capability) == advertised.end()) {
            out.emplace_back(c.cmd);
        }
    }
    return out;
}

Request make_status_request() {
    return Request{"status", json::Value::object()};
}

Request make_reload_request(std::uint32_t pid, const std::string& module, bool confirm, std::uint32_t timeout_ms) {
    Request r{"reload", json::Value::object()};
    if (pid != 0) {
        r.args.set("pid", json::Value::integer(pid));
    }
    if (!module.empty()) {
        r.args.set("module", json::Value::string(module));
    }
    if (confirm) {
        r.args.set("confirm", json::Value::boolean(true));
    }
    if (timeout_ms != 0) {
        r.args.set("timeout_ms", json::Value::integer(timeout_ms));
    }
    return r;
}

Request make_wait_request(const std::string& what, std::uint32_t timeout_ms) {
    Request r{"wait", json::Value::object()};
    r.args.set("for", json::Value::string(what));
    r.args.set("timeout_ms", json::Value::integer(timeout_ms));
    return r;
}

Request make_set_option_request(const std::string& name, json::Value value) {
    Request r{"set-option", json::Value::object()};
    r.args.set("name", json::Value::string(name));
    r.args.set("value", std::move(value));
    return r;
}

Request make_subscribe_request(const std::vector<std::string>& events) {
    Request r{"subscribe", json::Value::object()};
    json::Value list = json::Value::array();
    for (const std::string& e : events) {
        list.push(json::Value::string(e));
    }
    r.args.set("events", std::move(list));
    return r;
}

Request make_exception_action_request(std::uint32_t pid, std::uint64_t exception_id, const std::string& action) {
    Request r{"exception-action", json::Value::object()};
    r.args.set("pid", json::Value::integer(pid));
    r.args.set("exception_id", json::Value::integer(static_cast<std::int64_t>(exception_id)));
    r.args.set("action", json::Value::string(action));
    return r;
}

Request make_exceptions_request() {
    return Request{"exceptions", json::Value::object()};
}

Request make_restart_request(std::uint32_t pid, std::uint32_t timeout_ms) {
    Request r{"restart", json::Value::object()};
    r.args.set("pid", json::Value::integer(pid));
    if (timeout_ms != 0) {
        r.args.set("timeout_ms", json::Value::integer(timeout_ms));
    }
    return r;
}

Request make_revert_request(std::uint32_t pid, const std::string& module, const std::string& scope) {
    Request r{"revert", json::Value::object()};
    if (pid != 0) {
        r.args.set("pid", json::Value::integer(pid));
    }
    if (!module.empty()) {
        r.args.set("module", json::Value::string(module));
    }
    r.args.set("scope", json::Value::string(scope));
    return r;
}

std::vector<std::string> subscribe_events_for(const std::vector<std::string>& capabilities) {
    std::vector<std::string> events = {"diag", "patch_applied", "progress", "log"};
    const auto has = [&](const char* name) { return std::find(capabilities.begin(), capabilities.end(), name) != capabilities.end(); };
    if (has("exceptions")) {
        events.emplace_back("exception");
    }
    if (has("restart")) {
        events.emplace_back("restarted");
    }
    if (has("revert")) {
        events.emplace_back("patch_reverted");
    }
    return events;
}

PatchTiming parse_timing(const json::Value* t) {
    PatchTiming out;
    if (t == nullptr || !t->is_object()) {
        return out;
    }
    const auto read = [&](const char* key) {
        const json::Value* v = t->find(key);
        return v != nullptr && v->is_number() ? v->as_double() : 0.0;
    };
    out.sweep = read("sweep");
    out.compile = read("compile");
    out.plan = read("plan");
    out.anchor = read("anchor");
    out.link = read("link");
    out.load = read("load");
    out.fixups = read("fixups");
    out.init = read("init");
    out.hooks = read("hooks");
    out.total = read("total");
    out.present = t->find("total") != nullptr;
    return out;
}

namespace {

std::vector<std::string> string_list(const json::Value* v) {
    std::vector<std::string> out;
    if (v != nullptr && v->is_array()) {
        for (const json::Value& item : v->items()) {
            if (item.is_string()) {
                out.push_back(item.as_string());
            }
        }
    }
    return out;
}

std::uint64_t unsigned_of(const json::Value& object, const char* key) {
    const std::int64_t v = object.get_integer(key, 0);
    return v < 0 ? 0 : static_cast<std::uint64_t>(v);
}

}  // namespace

ReloadSummary parse_reload_result(const json::Value& result) {
    ReloadSummary out;
    if (!result.is_object()) {
        return out;
    }
    out.generation = unsigned_of(result, "generation");
    // `patched` is a count in the broker's answer; a bool in an older shape. Both mean "something was patched" when nonzero.
    if (const json::Value* p = result.find("patched")) {
        out.patched = p->is_bool() ? (p->as_bool() ? 1 : 0) : static_cast<std::uint64_t>(std::max<std::int64_t>(0, p->as_integer()));
    }
    out.hooks_installed = unsigned_of(result, "hooks_installed");
    out.changed_files = string_list(result.find("changed_files"));
    out.timing = parse_timing(result.find("timing_ms"));
    if (const json::Value* patches = result.find("patches"); patches != nullptr && patches->is_array()) {
        for (const json::Value& p : patches->items()) {
            if (!p.is_object()) {
                continue;
            }
            ReloadPatch patch;
            patch.pid = static_cast<std::uint32_t>(unsigned_of(p, "pid"));
            patch.module = p.get_string("module");
            patch.generation = unsigned_of(p, "generation");
            patch.hooks_installed = unsigned_of(p, "hooks_installed");
            patch.new_functions = unsigned_of(p, "new_functions");
            patch.new_globals = unsigned_of(p, "new_globals");
            patch.changed_files = string_list(p.find("changed_files"));
            patch.timing = parse_timing(p.find("timing_ms"));
            out.patches.push_back(std::move(patch));
        }
    }
    return out;
}

RestartSummary parse_restart_result(const json::Value& result) {
    RestartSummary out;
    if (!result.is_object()) {
        return out;
    }
    out.old_pid = static_cast<std::uint32_t>(unsigned_of(result, "old_pid"));
    out.new_pid = static_cast<std::uint32_t>(unsigned_of(result, "new_pid"));
    if (const json::Value* modules = result.find("modules"); modules != nullptr && modules->is_array()) {
        for (const json::Value& m : modules->items()) {
            if (m.is_string()) {
                out.modules.push_back(m.as_string());
            } else if (m.is_object()) {
                out.modules.push_back(m.get_string("path", m.get_string("module")));
            }
        }
    }
    return out;
}

RevertSummary parse_revert_result(const json::Value& result) {
    RevertSummary out;
    if (!result.is_object()) {
        return out;
    }
    out.generation = unsigned_of(result, "generation");
    if (const json::Value* list = result.find("reverted"); list != nullptr && list->is_array()) {
        for (const json::Value& r : list->items()) {
            if (!r.is_object()) {
                continue;
            }
            RevertedPatch patch;
            patch.pid = static_cast<std::uint32_t>(unsigned_of(r, "pid"));
            patch.module = r.get_string("module");
            patch.generation = unsigned_of(r, "generation");
            out.reverted.push_back(std::move(patch));
        }
    }
    return out;
}

}  // namespace cml::ui
