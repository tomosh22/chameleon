#pragma once

// A scripted broker for the UI tests: speaks the WP-13 / WP-12 shapes and the ADR-0032 shapes of the commands that
// land in parallel (exceptions, restart, revert), without any pipe.

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "cml/ipc/control.h"
#include "cml/ui/controller.h"

namespace cml::ui::test {

class FakeBroker final : public Transport {
public:
    using Handler = std::function<ipc::ControlResponse(const ipc::json::Value& args)>;

    std::vector<std::string> capabilities;
    std::vector<std::pair<std::string, ipc::json::Value>> calls;  // every request, in order
    std::map<std::string, Handler> handlers;                      // by command
    bool disconnected = false;

    Result<ipc::ControlResponse> call(const std::string& cmd, ipc::json::Value args, std::uint32_t) override {
        calls.emplace_back(cmd, args);
        if (disconnected) {
            return Diag::make(5405, "control pipe closed", "restart the broker");
        }
        if (cmd == "status" && handlers.find("status") == handlers.end()) {
            return ipc::make_ok_response(1, ipc::status_to_json(status));
        }
        const auto it = handlers.find(cmd);
        const bool advertised = [&] {
            for (const CommandInfo& c : command_table()) {
                if (cmd == c.cmd) {
                    for (const std::string& cap : capabilities) {
                        if (cap == c.capability) return true;
                    }
                    return false;
                }
            }
            return cmd == "status";
        }();
        if (it == handlers.end() || !advertised) {
            return ipc::make_error_response(1, Diag::make(5402, "control command '" + cmd + "' is not provided by this broker",
                                                           "Query `status` for the broker's capabilities."));
        }
        return it->second(args);
    }

    ipc::StatusReport status;

    // The commands of WP-12 plus whatever `extra` adds; `status` carries the same capabilities.
    explicit FakeBroker(std::vector<std::string> caps = {"status", "reload", "wait", "set-option", "subscribe", "diag-events"})
        : capabilities(std::move(caps)) {
        status.version = "0.1.0-test";
        status.capabilities = capabilities;
        ipc::StatusProcess p;
        p.pid = 4242;
        ipc::StatusModule m;
        m.path = R"(C:\game\game.exe)";
        m.enabled = true;
        m.index_state = "ready";
        p.modules.push_back(m);
        ipc::StatusModule other;
        other.path = R"(C:\game\plugin.dll)";
        other.enabled = false;
        p.modules.push_back(other);
        status.processes.push_back(p);
    }

    void sync_status_capabilities() { status.capabilities = capabilities; }
};

inline ipc::json::Value parse(const char* text) {
    return *ipc::json::parse(text);
}

inline ipc::ControlEvent event(const char* text) {
    ipc::ControlEvent e;
    e.body = parse(text);
    e.name = e.body.get_string("event");
    return e;
}

}  // namespace cml::ui::test
