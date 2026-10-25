#pragma once

// Thread-safe snapshot of the processes (agent sessions) the broker currently serves, for the
// control protocol's `status` command. Each connection thread publishes its Session after every
// message; the control server reads snapshots. Portable.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
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
    std::vector<RegisteredModule> modules;  // every module the agent reported, enabled flag set
};

class ProcessRegistry {
public:
    // Replaces the entry of the session's agent. Ignored until the session has completed the
    // handshake (before that it has no pid).
    void update(const Session& session);
    void remove(std::uint64_t agent_id);

    // Processes ordered by agent id (connection order).
    std::vector<RegisteredProcess> snapshot() const;

private:
    mutable std::mutex mutex_;
    std::map<std::uint64_t, RegisteredProcess> processes_;
};

}  // namespace cml::core
