#include "module_diff.h"

#include <map>

namespace cml::agent {

namespace {

using ByBase = std::map<std::uint64_t, const ipc::ModuleEntry*>;

ByBase index_by_base(const std::vector<ipc::ModuleEntry>& modules) {
    ByBase index;
    for (const ipc::ModuleEntry& module : modules) {
        index[module.base] = &module;
    }
    return index;
}

bool same_module(const ipc::ModuleEntry& a, const ipc::ModuleEntry& b) {
    return a.base == b.base && a.size == b.size && a.path == b.path;
}

}  // namespace

ModuleDelta diff_modules(const std::vector<ipc::ModuleEntry>& before,
                         const std::vector<ipc::ModuleEntry>& after) {
    const ByBase old_index = index_by_base(before);
    const ByBase new_index = index_by_base(after);
    ModuleDelta delta;
    for (const ipc::ModuleEntry& module : before) {
        const auto it = new_index.find(module.base);
        if (it == new_index.end() || !same_module(module, *it->second)) {
            delta.unloaded.push_back(module);
        }
    }
    for (const ipc::ModuleEntry& module : after) {
        const auto it = old_index.find(module.base);
        if (it == old_index.end() || !same_module(module, *it->second)) {
            delta.loaded.push_back(module);
        }
    }
    return delta;
}

}  // namespace cml::agent
