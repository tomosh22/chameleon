#pragma once

// Runs control commands for the UI and feeds the answers into the view model. The transport is an interface so
// that the unit tests can script a fake broker (including the ADR-0032 commands that land in parallel); the real
// one wraps cml::ipc::ControlClient (src/win/broker_link.cpp).
//
// A Controller is used from one thread at a time (each control connection has its own).

#include <cstdint>
#include <string>
#include <vector>

#include "cml/ipc/control.h"
#include "cml/ipc/json.h"
#include "cml/ui/commands.h"
#include "cml/ui/settings.h"
#include "cml/ui/view_model.h"
#include "cml/util/result.h"

namespace cml::ui {

class Transport {
public:
    virtual ~Transport() = default;
    // A response with ok=false is returned as a response; transport failures and timeouts are Diags.
    virtual Result<ipc::ControlResponse> call(const std::string& cmd, ipc::json::Value args, std::uint32_t timeout_ms) = 0;
};

class Controller {
public:
    Controller(ViewModel& view_model, Transport& transport) : vm_(view_model), transport_(transport) {}

    // `status`: refreshes processes, modules, capabilities, generation.
    Result<void> refresh_status();
    // `subscribe` with the event set the capabilities allow (needs refresh_status() first).
    Result<void> subscribe();
    // `reload`; pid 0 = all processes, empty module = all modules. `confirm` answers the CML5551 prompt.
    Result<ReloadSummary> reload(std::uint32_t pid, const std::string& module, bool confirm);
    Result<void> set_option(const std::string& name, ipc::json::Value value);
    Result<void> wait(const std::string& what, std::uint32_t timeout_ms);
    Result<RestartSummary> restart(std::uint32_t pid);
    Result<RevertSummary> revert(std::uint32_t pid, const std::string& module, const std::string& scope);  // scope "last" | "all"
    // Success resolves the exception. A failure because the thread is no longer held (CML4400 from the agent, CML5751
    // from the broker) resolves it too: nothing is left to answer. Any other refusal (a Leave that cannot unwind,
    // CML4401/4402, an unreachable agent) keeps it pending, as the thread stays held (WP-20).
    Result<void> exception_action(std::uint32_t pid, std::uint64_t exception_id, const std::string& action);
    Result<void> refresh_exceptions();
    // Makes the broker match the settings (set-option for each); returns the first failure after trying all.
    Result<void> apply_settings(const Settings& settings);

    static constexpr std::uint32_t kQuickTimeoutMs = 10000;
    static constexpr std::uint32_t kReloadTimeoutMs = 120000;  // sent to the broker; the call itself waits a little longer

private:
    // Gate on the capability, send, turn a failed response into a Diag (and tell the view model).
    Result<ipc::json::Value> invoke(const Request& request, std::uint32_t timeout_ms, std::uint32_t pid = 0, const std::string& module = std::string(),
                                    std::uint64_t request_serial = 0);

    ViewModel& vm_;
    Transport& transport_;
};

}  // namespace cml::ui
