#pragma once

// Thread-safe snapshot of the processes (agent sessions) the broker currently serves, for the
// control protocol's `status` command. Each connection thread publishes its Session after every
// message; the control server reads snapshots. Portable.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "cml/core/session.h"

namespace cml::core {

struct RegisteredModule {
    std::string path;
    bool enabled = false;
};

struct RegisteredProcess {
    std::uint64_t agent_id = 0;
    std::uint32_t pid = 0;
    std::uint64_t revision = 0;             // Session::revision() this entry was built from
    std::vector<RegisteredModule> modules;  // every module the agent reported, enabled flag set
};

// Key used to compare module paths: ASCII-case-insensitive, '/' equals '\\', a leading "\\\\?\\" is ignored
// (the rules of Session::same_path), so that a hash set can replace pairwise comparison.
std::string normalized_path_key(std::string_view path);

class ProcessRegistry {
public:
    // Rebuilds the entry of the session's agent, but only when the session's revision() differs from
    // the one the entry was built from (module list or enabled set changed). Ignored until the session
    // has completed the handshake. Returns true when the entry was rebuilt.
    bool update_if_changed(const Session& session);
    // Unconditional rebuild (same matching rules).
    void update(const Session& session);

    // How many entries were rebuilt so far (tests, diagnostics).
    std::uint64_t rebuilds() const;
    void remove(std::uint64_t agent_id);

    // Processes ordered by agent id (connection order).
    std::vector<RegisteredProcess> snapshot() const;

private:
    mutable std::mutex mutex_;
    std::uint64_t rebuilds_ = 0;
    std::map<std::uint64_t, RegisteredProcess> processes_;
};

}  // namespace cml::core
