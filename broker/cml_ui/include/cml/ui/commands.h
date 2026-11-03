#pragma once

// The control commands the UI drives (ADR-0033: "every control-pipe operation reachable from the UI"), the
// request builders and the result parsers. Shapes: docs/wp/WP-13.md (status, reload, wait, set-option, subscribe),
// ADR-0032 (exceptions, restart, revert). Portable.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cml/ipc/json.h"

namespace cml::ui {

// One row per control command. `capability` is the name the broker advertises for it; `feature` names the part of
// the UI that drives it (shown by the self-test and in docs/wp/WP-19.md).
struct CommandInfo {
    const char* cmd;
    const char* capability;
    const char* feature;
};

const std::vector<CommandInfo>& command_table();

// Capabilities that are not commands but that the UI knows and handles (events and the like).
const std::vector<std::string>& passive_capabilities();

// Advertised capabilities that neither a table row nor a passive entry covers: a command the broker gained that the
// UI cannot drive yet.
std::vector<std::string> uncovered_capabilities(const std::vector<std::string>& advertised);

// Table rows whose capability the broker does not advertise (their UI is disabled).
std::vector<std::string> unavailable_commands(const std::vector<std::string>& advertised);

struct Request {
    std::string cmd;
    ipc::json::Value args = ipc::json::Value::object();
};

Request make_status_request();
Request make_reload_request(std::uint32_t pid, const std::string& module, bool confirm, std::uint32_t timeout_ms);
Request make_wait_request(const std::string& what, std::uint32_t timeout_ms);
Request make_set_option_request(const std::string& name, ipc::json::Value value);
Request make_subscribe_request(const std::vector<std::string>& events);
Request make_exception_action_request(std::uint32_t pid, std::uint64_t exception_id, const std::string& action);
Request make_exceptions_request();
Request make_restart_request(std::uint32_t pid, std::uint32_t timeout_ms);
Request make_revert_request(std::uint32_t pid, const std::string& module, const std::string& scope);

// The event names to subscribe to, given the capabilities: the WP-12 events always, the ADR-0032 events only when
// the broker advertises the capability (an older broker rejects unknown event names).
std::vector<std::string> subscribe_events_for(const std::vector<std::string>& capabilities);

// --- results --------------------------------------------------------------------------------------------------

struct PatchTiming {
    double sweep = 0, compile = 0, plan = 0, anchor = 0, link = 0, load = 0, fixups = 0, init = 0, hooks = 0, total = 0;  // ms
    bool present = false;
};

struct ReloadPatch {
    std::uint32_t pid = 0;
    std::string module;
    std::uint64_t generation = 0;
    std::uint64_t hooks_installed = 0;
    std::uint64_t new_functions = 0;
    std::uint64_t new_globals = 0;
    std::vector<std::string> changed_files;
    PatchTiming timing;
};

struct ReloadSummary {
    std::uint64_t generation = 0;
    std::uint64_t patched = 0;
    std::uint64_t hooks_installed = 0;
    std::vector<std::string> changed_files;
    std::vector<ReloadPatch> patches;
    PatchTiming timing;  // the sum over all patches
};

ReloadSummary parse_reload_result(const ipc::json::Value& result);
PatchTiming parse_timing(const ipc::json::Value* timing_ms);

struct RestartSummary {
    std::uint32_t old_pid = 0;
    std::uint32_t new_pid = 0;
    std::vector<std::string> modules;
};
RestartSummary parse_restart_result(const ipc::json::Value& result);

struct RevertedPatch {
    std::uint32_t pid = 0;
    std::string module;
    std::uint64_t generation = 0;
};
struct RevertSummary {
    std::vector<RevertedPatch> reverted;
    std::uint64_t generation = 0;
};
RevertSummary parse_revert_result(const ipc::json::Value& result);

}  // namespace cml::ui
