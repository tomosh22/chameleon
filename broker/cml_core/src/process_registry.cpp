#include "cml/core/process_registry.h"

#include <algorithm>

namespace cml::core {

void ProcessRegistry::update(const Session& session) {
    if (!session.handshaken()) {
        return;
    }
    RegisteredProcess entry;
    entry.agent_id = session.agent_id();
    entry.pid = session.pid();
    entry.modules.reserve(session.modules().size());
    for (const ipc::ModuleEntry& module : session.modules()) {
        RegisteredModule m;
        m.path = module.path;
        const std::vector<std::string>& enabled = session.enabled_modules();
        m.enabled = std::any_of(enabled.begin(), enabled.end(), [&](const std::string& path) {
            return Session::same_path(path, module.path);
        });
        entry.modules.push_back(std::move(m));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    processes_[entry.agent_id] = std::move(entry);
}

void ProcessRegistry::remove(std::uint64_t agent_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    processes_.erase(agent_id);
}

std::vector<RegisteredProcess> ProcessRegistry::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RegisteredProcess> out;
    out.reserve(processes_.size());
    for (const auto& entry : processes_) {
        out.push_back(entry.second);
    }
    return out;
}

}  // namespace cml::core
