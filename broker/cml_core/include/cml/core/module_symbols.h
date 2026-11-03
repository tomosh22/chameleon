#pragma once

// The real ProcessModules for the bind planner (WP-12, review item 4, ADR-0029).
//
// The planner resolves an undefined symbol that neither the registry nor the module's PDB knows against
//   * the IAT slot the patched module has for `__imp_X` (resolution step 4), and
//   * the exports of every OTHER module loaded in the target process (step 5): ntdll, kernel32, ucrtbase, ...
//     (`new_global` needs `strlen`, which the old fixture never calls and ntdll exports).
// The broker is a different process, so both are read from the module FILES on disk (cml_pe) and turned into
// addresses with the load addresses the agent reported (ModuleList). Forwarded exports are skipped (they are
// not code in that module). Export tables are parsed once per file and cached; the cache checks size and
// modification time so that a rebuilt DLL is parsed again.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cml/core/symbol_registry.h"
#include "cml/util/result.h"

namespace cml::core {

struct LoadedModule {
    std::string path;        // full path of the file
    std::uint64_t base = 0;  // where it is loaded in the target process
    std::uint64_t size = 0;
};

struct ExportInfo {
    std::uint32_t rva = 0;
    bool is_function = false;  // the RVA lies in an executable section
};

struct ExportIndex {
    bool ok = false;
    Diag error;  // valid when !ok (only a diagnostic, never fatal: the module is skipped)
    std::unordered_map<std::string, ExportInfo> by_name;
};

class ExportCache {
public:
    // Parses `path` (once; again when its size or modification time changed). Never null.
    std::shared_ptr<const ExportIndex> get(const std::string& path);
    std::size_t parses() const;  // how many files were parsed so far (tests)

private:
    struct Entry {
        std::uint64_t size = 0;
        long long mtime = 0;
        std::shared_ptr<const ExportIndex> index;
    };
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    std::size_t parses_ = 0;
};

class FileProcessModules final : public ProcessModules {
public:
    // `modules`: the target process's modules in loader order. `patched`: the module being patched (its
    // IAT is read; its exports are not offered). `cache` may be shared between instances.
    FileProcessModules(std::vector<LoadedModule> modules, LoadedModule patched, std::shared_ptr<ExportCache> cache = nullptr);

    std::optional<SymbolAddress> iat_slot(std::string_view module_name, std::string_view import_name) const override;
    std::optional<SymbolAddress> find_export(std::string_view name) const override;

    // Modules whose file could not be parsed (diagnostics for the log).
    std::vector<Diag> skipped() const;

private:
    std::vector<LoadedModule> modules_;
    LoadedModule patched_;
    std::shared_ptr<ExportCache> cache_;
    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, std::uint32_t> iat_;  // import name -> IAT slot RVA of the patched module
    mutable bool iat_loaded_ = false;
    mutable std::vector<Diag> skipped_;
};

}  // namespace cml::core
