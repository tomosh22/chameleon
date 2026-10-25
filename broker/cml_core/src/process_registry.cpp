#include "cml/core/process_registry.h"

#include <unordered_set>
#include <utility>

namespace cml::core {

std::string normalized_path_key(std::string_view path) {
    constexpr std::string_view kExtended = "\\\\?\\";
    if (path.size() > kExtended.size() && path.substr(0, kExtended.size()) == kExtended) {
        path.remove_prefix(kExtended.size());
    }
    std::string key(path);
    for (char& c : key) {
        if (c == '/') {
            c = '\\';
        } else if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return key;
}

namespace {

RegisteredProcess build_entry(const Session& session) {
    RegisteredProcess entry;
    entry.agent_id = session.agent_id();
    entry.pid = session.pid();
    entry.revision = session.revision();
    // O(modules + enabled): one hashed set of the enabled paths instead of a pairwise scan.
    std::unordered_set<std::string> enabled;
    enabled.reserve(session.enabled_modules().size());
    for (const std::string& path : session.enabled_modules()) {
        enabled.insert(normalized_path_key(path));
    }
    entry.modules.reserve(session.modules().size());
    for (const ipc::ModuleEntry& module : session.modules()) {
        RegisteredModule m;
        m.path = module.path;
        m.enabled = !enabled.empty() && enabled.count(normalized_path_key(module.path)) != 0;
        entry.modules.push_back(std::move(m));
    }
    return entry;
}

}  // namespace

bool ProcessRegistry::update_if_changed(const Session& session) {
    if (!session.handshaken()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = processes_.find(session.agent_id());
        if (it != processes_.end() && it->second.revision == session.revision()) {
            return false;
        }
    }
    RegisteredProcess entry = build_entry(session);  // built outside the lock
    std::lock_guard<std::mutex> lock(mutex_);
    processes_[entry.agent_id] = std::move(entry);
    ++rebuilds_;
    return true;
}

void ProcessRegistry::update(const Session& session) {
    if (!session.handshaken()) {
        return;
    }
    RegisteredProcess entry = build_entry(session);
    std::lock_guard<std::mutex> lock(mutex_);
    processes_[entry.agent_id] = std::move(entry);
    ++rebuilds_;
}

std::uint64_t ProcessRegistry::rebuilds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rebuilds_;
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
