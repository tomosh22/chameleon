#include "cml/ui/controller.h"

#include <algorithm>

#include "cml/ipc/control_ids.h"
#include "cml/ui/ui_diag_ids.h"

namespace cml::ui {

namespace json = ipc::json;

namespace {

const char* capability_for_command(const std::string& cmd) {
    if (cmd == "status") {
        return nullptr;  // the first call, before any capability is known
    }
    for (const CommandInfo& c : command_table()) {
        if (cmd == c.cmd) {
            return c.capability;
        }
    }
    return nullptr;
}

struct InFlight {
    ViewModel& vm;
    explicit InFlight(ViewModel& v) : vm(v) { vm.set_reload_in_flight(true); }
    ~InFlight() { vm.set_reload_in_flight(false); }
};

}  // namespace

Result<json::Value> Controller::invoke(const Request& request, std::uint32_t timeout_ms, std::uint32_t pid, const std::string& module,
                                       std::uint64_t request_serial) {
    if (const char* capability = capability_for_command(request.cmd); capability != nullptr && !vm_.capability_usable(capability)) {
        Diag diag = Diag::make(ids::kUiFeatureUnavailable, "this broker does not provide '" + request.cmd + "'",
                               std::string("Use a broker build that advertises the '") + capability + "' capability; the matching UI stays disabled.");
        vm_.set_status_message(diag.message, StatusSeverity::Warning);
        return diag;
    }
    Result<ipc::ControlResponse> response = transport_.call(request.cmd, request.args, timeout_ms);
    if (!response) {
        vm_.set_status_message(response.error().id() + ": " + response.error().message, StatusSeverity::Error);
        return response.error();
    }
    if (!response->ok) {
        Diag diag;
        if (response->diag) {
            diag = to_diag(*response->diag);
        } else {
            diag = Diag::make(ids::kControlResponseInvalid, "the broker answered '" + request.cmd + "' with an error and no diagnostic", "Use matching versions of cml_ui and cml_broker.");
        }
        vm_.apply_command_failure(request.cmd, diag, pid, module, request_serial);
        return diag;
    }
    return std::move(response->result);
}

Result<void> Controller::refresh_status() {
    Result<json::Value> result = invoke(make_status_request(), kQuickTimeoutMs);
    if (!result) {
        return result.error();
    }
    Result<ipc::StatusReport> report = ipc::status_from_json(*result);
    if (!report) {
        vm_.set_status_message(report.error().id() + ": " + report.error().message, StatusSeverity::Error);
        return report.error();
    }
    vm_.apply_status(*report);
    return {};
}

Result<void> Controller::subscribe() {
    Request request = make_subscribe_request(subscribe_events_for(vm_.capabilities()));
    Result<json::Value> result = invoke(request, kQuickTimeoutMs);
    if (!result) {
        return result.error();
    }
    return {};
}

Result<ReloadSummary> Controller::reload(std::uint32_t pid, const std::string& module, bool confirm) {
    const std::uint64_t serial = vm_.begin_reload_request();
    InFlight in_flight(vm_);
    vm_.clear_confirm_prompt();
    Result<json::Value> result = invoke(make_reload_request(pid, module, confirm, kReloadTimeoutMs), kReloadTimeoutMs + 10000, pid, module, serial);
    if (!result) {
        return result.error();
    }
    ReloadSummary summary = parse_reload_result(*result);
    vm_.apply_reload_result(summary);
    return summary;
}

Result<void> Controller::set_option(const std::string& name, json::Value value) {
    Result<json::Value> result = invoke(make_set_option_request(name, std::move(value)), kQuickTimeoutMs);
    if (!result) {
        return result.error();
    }
    return {};
}

Result<void> Controller::wait(const std::string& what, std::uint32_t timeout_ms) {
    Result<json::Value> result = invoke(make_wait_request(what, timeout_ms), timeout_ms + 10000);
    if (!result) {
        return result.error();
    }
    vm_.set_status_message("Wait for " + what + " finished", StatusSeverity::Info);
    return {};
}

Result<RestartSummary> Controller::restart(std::uint32_t pid) {
    Result<json::Value> result = invoke(make_restart_request(pid, 0), 130000, pid);
    if (!result) {
        return result.error();
    }
    RestartSummary summary = parse_restart_result(*result);
    vm_.apply_restart_result(summary);
    return summary;
}

Result<RevertSummary> Controller::revert(std::uint32_t pid, const std::string& module, const std::string& scope) {
    Result<json::Value> result = invoke(make_revert_request(pid, module, scope), 60000, pid, module);
    if (!result) {
        return result.error();
    }
    RevertSummary summary = parse_revert_result(*result);
    vm_.apply_revert_result(summary);
    return summary;
}

Result<void> Controller::exception_action(std::uint32_t pid, std::uint64_t exception_id, const std::string& action) {
    Result<json::Value> result = invoke(make_exception_action_request(pid, exception_id, action), kQuickTimeoutMs, pid);
    if (!result) {
        const std::uint32_t code = result.error().code;
        if (code == 4400 || code == 5751) {
            vm_.resolve_exception(pid, exception_id);  // not held any more (process gone, or answered elsewhere): stop asking
        }
        return result.error();
    }
    vm_.resolve_exception(pid, exception_id);
    vm_.set_status_message("Exception " + std::to_string(exception_id) + ": " + action, StatusSeverity::Info);
    return {};
}

Result<void> Controller::refresh_exceptions() {
    Result<json::Value> result = invoke(make_exceptions_request(), kQuickTimeoutMs);
    if (!result) {
        return result.error();
    }
    std::vector<ExceptionRow> rows;
    if (const json::Value* pending = result->find("pending"); pending != nullptr && pending->is_array()) {
        for (const json::Value& item : pending->items()) {
            if (item.is_object()) {
                rows.push_back(ViewModel::parse_exception(item));
            }
        }
    }
    vm_.set_pending_exceptions(rows);
    return {};
}

Result<void> Controller::apply_settings(const Settings& settings) {
    std::optional<Diag> first;
    for (OptionSetting& option : settings_broker_options(settings)) {
        Result<void> r = set_option(option.name, std::move(option.value));
        if (!r && !first) {
            first = r.error();
        }
    }
    if (first) {
        return *first;
    }
    return {};
}

}  // namespace cml::ui
