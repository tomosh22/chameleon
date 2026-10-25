#pragma once

// The two lookup interfaces the bind planner (WP-07) needs from the running process, with in-memory
// implementations for tests and for the broker until WP-12 wires the real ones.
//
//   SymbolRegistry  symbols introduced by earlier patches (new functions, new globals): name -> VA.
//                   Resolution step (1); also consulted for DEFINED symbols so that a second patch of
//                   the same TU hooks / binds the first patch's definition.
//   ProcessModules  what the process offers besides the module being patched: IAT slots of that
//                   module (step 4, "parse the import directory in memory") and the exports of every
//                   other loaded module (step 5).
//
// Both are queried synchronously and must be thread-safe if shared; the in-memory versions are not.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace cml::core {

struct SymbolAddress {
    std::uint64_t va = 0;
    bool is_function = false;
    std::string origin;  // "patch 2", "kernel32.dll", ... (diagnostics and traces only)
};

class SymbolRegistry {
public:
    virtual ~SymbolRegistry() = default;
    // `decorated_name` is the linker name exactly as the object spells it.
    virtual std::optional<SymbolAddress> find(std::string_view decorated_name) const = 0;
};

class ProcessModules {
public:
    virtual ~ProcessModules() = default;
    // Address of the IAT slot that holds the address of `import_name` ("X" of "__imp_X") in
    // `module_name` (the module being patched), by parsing its import directory.
    virtual std::optional<SymbolAddress> iat_slot(std::string_view module_name, std::string_view import_name) const = 0;
    // Export `name` of any loaded module other than the one being patched.
    virtual std::optional<SymbolAddress> find_export(std::string_view name) const = 0;
};

class InMemorySymbolRegistry final : public SymbolRegistry {
public:
    void add(std::string name, SymbolAddress address) { symbols_[std::move(name)] = std::move(address); }
    bool remove(std::string_view name);
    std::size_t size() const noexcept { return symbols_.size(); }
    std::optional<SymbolAddress> find(std::string_view decorated_name) const override;

private:
    std::map<std::string, SymbolAddress, std::less<>> symbols_;
};

class InMemoryProcessModules final : public ProcessModules {
public:
    void add_iat_slot(std::string module_name, std::string import_name, std::uint64_t va, std::string origin = {});
    void add_export(std::string module_name, std::string name, std::uint64_t va, bool is_function);
    std::optional<SymbolAddress> iat_slot(std::string_view module_name, std::string_view import_name) const override;
    std::optional<SymbolAddress> find_export(std::string_view name) const override;

private:
    std::map<std::string, SymbolAddress, std::less<>> iat_;      // "module|import", module lower-cased
    std::map<std::string, SymbolAddress, std::less<>> exports_;  // name
};

}  // namespace cml::core
