#include "cml/core/symbol_registry.h"

#include <utility>

namespace cml::core {

namespace {
std::string iat_key(std::string_view module_name, std::string_view import_name) {
    std::string key;
    key.reserve(module_name.size() + 1 + import_name.size());
    for (const char c : module_name) {  // module names compare case-insensitively (ASCII)
        key.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    key.push_back('|');
    key.append(import_name);
    return key;
}
}  // namespace

bool InMemorySymbolRegistry::remove(std::string_view name) {
    const auto it = symbols_.find(name);
    if (it == symbols_.end()) {
        return false;
    }
    symbols_.erase(it);
    return true;
}

std::optional<SymbolAddress> InMemorySymbolRegistry::find(std::string_view decorated_name) const {
    const auto it = symbols_.find(decorated_name);
    if (it == symbols_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void InMemoryProcessModules::add_iat_slot(std::string module_name, std::string import_name, std::uint64_t va,
                                          std::string origin) {
    SymbolAddress a;
    a.va = va;
    a.is_function = false;
    a.origin = origin.empty() ? module_name : std::move(origin);
    iat_[iat_key(module_name, import_name)] = std::move(a);
}

void InMemoryProcessModules::add_export(std::string module_name, std::string name, std::uint64_t va, bool is_function) {
    SymbolAddress a;
    a.va = va;
    a.is_function = is_function;
    a.origin = std::move(module_name);
    exports_[std::move(name)] = std::move(a);
}

std::optional<SymbolAddress> InMemoryProcessModules::iat_slot(std::string_view module_name, std::string_view import_name) const {
    const auto it = iat_.find(iat_key(module_name, import_name));
    if (it == iat_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<SymbolAddress> InMemoryProcessModules::find_export(std::string_view name) const {
    const auto it = exports_.find(name);
    if (it == exports_.end()) {
        return std::nullopt;
    }
    return it->second;
}

}  // namespace cml::core
