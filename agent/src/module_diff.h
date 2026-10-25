#pragma once

// Module-list change detection for the agent. The agent polls the process module list on its own
// thread (see docs/wp/WP-11.md for why polling rather than LdrRegisterDllNotification) and turns
// the difference between two snapshots into ModuleLoaded / ModuleUnloaded events.
//
// Portable so it can be unit-tested anywhere.

#include <vector>

#include "cml/ipc/messages.h"

namespace cml::agent {

struct ModuleDelta {
    std::vector<ipc::ModuleEntry> loaded;    // in `after` only (or at the same base with another image)
    std::vector<ipc::ModuleEntry> unloaded;  // in `before` only
    bool empty() const noexcept { return loaded.empty() && unloaded.empty(); }
};

// Two entries are the same module when base, size and path are all equal; a different image
// mapped at the same base counts as unload + load. Order of the results follows the inputs.
ModuleDelta diff_modules(const std::vector<ipc::ModuleEntry>& before,
                         const std::vector<ipc::ModuleEntry>& after);

}  // namespace cml::agent
