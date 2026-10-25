#pragma once

// One agent connection as seen by the broker: a small, transport-independent state machine.
//
// WP-11 scope: handshake, module bookkeeping and enable/disable. WP-12 grows this into the real
// session (process registry, bind planner, patch pipeline); the shape - messages in, replies out,
// all state private to the session - is meant to survive that.
//
// Portable (no Win32) so it can be unit-tested without pipes.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cml/ipc/messages.h"

namespace cml::core {

struct SessionOutcome {
    std::vector<ipc::Message> replies;  // to send back, in order
    bool close = false;                 // protocol violation: drop the connection after sending
};

class Session {
public:
    Session(std::uint64_t agent_id, std::string broker_version);

    // Handles one message from the agent. Every incoming message is logged at info level; a
    // request gets exactly one reply (Welcome for Hello, Reply for the others).
    SessionOutcome on_message(const ipc::Message& message);

    std::uint64_t agent_id() const noexcept { return agent_id_; }
    bool handshaken() const noexcept { return handshaken_; }
    std::uint32_t pid() const noexcept { return pid_; }
    const std::string& agent_path() const noexcept { return agent_path_; }
    const std::string& command_line() const noexcept { return command_line_; }
    const std::vector<ipc::ModuleEntry>& modules() const noexcept { return modules_; }
    const std::vector<std::string>& enabled_modules() const noexcept { return enabled_; }

    // Compares two module paths the way Windows would for display purposes: '/' equals '\',
    // a leading "\\?\" is ignored and ASCII letters compare case-insensitively.
    static bool same_path(std::string_view a, std::string_view b);

private:
    const ipc::ModuleEntry* find_module(std::string_view path) const;
    bool is_enabled(std::string_view path) const;

    SessionOutcome handle_hello(const ipc::Hello& hello);
    ipc::Reply handle_enable(const ipc::EnableModule& request);
    ipc::Reply handle_disable(const ipc::DisableModule& request);
    SessionOutcome violation(std::uint64_t request_id, std::string message);

    std::uint64_t agent_id_;
    std::string broker_version_;
    bool handshaken_ = false;
    std::uint32_t pid_ = 0;
    std::string agent_path_;
    std::string command_line_;
    std::vector<ipc::ModuleEntry> modules_;
    std::vector<std::string> enabled_;  // full paths of enabled modules
};

}  // namespace cml::core
