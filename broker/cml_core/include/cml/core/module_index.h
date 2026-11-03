#pragma once

// The PDB index of one enabled module, loaded asynchronously (plan section 3 step 2, WP-12).
//
// When a module is enabled the broker starts a ModuleIndex: a worker thread opens the module's PDB (cml_pdb,
// publics preloaded), lists the files of every compiland and takes the source-hash baseline (SourceBaseline).
// Progress is observable at any time (`status()`: state, percent, phase) and is what the control pipe's
// `status` and `wait` report. `reload` waits for it.
//
// ADR-0017: the PDB index maps the file read-only for as long as it lives, and a linker cannot rewrite a mapped
// PDB. `release_pdb()` drops the broker's reference (the next `acquire_pdb()` maps it again); the caller must
// call it before anything relinks the module. The patch pipeline holds a reference only while it plans.
//
// Portable; thread-safe.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cml/core/source_baseline.h"
#include "cml/pdb/index.h"
#include "cml/util/result.h"

namespace cml::core {

enum class IndexState : std::uint8_t { Pending, Loading, Ready, Failed };
const char* to_string(IndexState state) noexcept;  // "pending" "loading" "ready" "failed"

struct IndexStatus {
    IndexState state = IndexState::Pending;
    std::uint32_t percent = 0;  // 0..100
    std::string phase;          // "opening the PDB", "reading file lists", "hashing sources (n/m)", ...
    std::optional<Diag> error;  // Failed
    std::size_t compilands = 0;
    std::size_t files = 0;      // tracked source files
    double pdb_ms = 0;
    double baseline_ms = 0;
};

struct ModuleIndexSpec {
    std::string module_path;  // the module's file; its PDB is located from the debug directory
    std::string pdb_path;     // when non-empty the PDB is opened directly (tests)
    pdb::OpenOptions pdb_options;  // use_cache is forced off: the index must be releasable
    BaselineOptions baseline;      // progress / cancelled are set by the index itself
    // Called from the worker thread on every state or phase change (after the status was updated).
    std::function<void(const IndexStatus&)> on_change;
};

class ModuleIndex {
public:
    static std::shared_ptr<ModuleIndex> start(ModuleIndexSpec spec);
    ~ModuleIndex();
    ModuleIndex(const ModuleIndex&) = delete;
    ModuleIndex& operator=(const ModuleIndex&) = delete;

    IndexStatus status() const;
    // Waits until Ready or Failed; false on timeout. timeout 0 polls.
    bool wait(std::uint32_t timeout_ms) const;

    // The mapped index (null while loading, after release_pdb(), or when the load failed).
    std::shared_ptr<const pdb::PdbIndex> pdb() const;
    // The index, mapped again if it was released. CML53xx/CML14xx when the PDB cannot be opened.
    Result<std::shared_ptr<const pdb::PdbIndex>> acquire_pdb();
    // ADR-0017: drops the broker's reference. Planning in progress keeps its own until it ends.
    void release_pdb();

    const std::string& module_path() const noexcept { return spec_.module_path; }

    // Compilands that have at least one changed file (indices ascending), the changed files, and changed
    // files that no compiland lists. `stamps` goes to accept() once the files were patched.
    struct Sweep {
        std::vector<std::string> changed;
        std::vector<std::uint32_t> compilands;
        std::vector<std::string> unmapped;
        std::vector<std::string> missing;
        std::map<std::string, std::vector<std::uint32_t>> file_compilands;  // changed file -> the compilands that list it
        std::map<std::string, FileStamp> stamps;
        std::size_t hashed = 0;
        std::size_t checked = 0;
    };
    // Requires Ready and a mapped index (acquire_pdb() first).
    Result<Sweep> sweep();
    void accept(const std::map<std::string, FileStamp>& stamps);
    // The files the baseline tracks (empty until Ready). WP-14 watches their directories.
    std::vector<std::string> tracked_files() const { return baseline_.files(); }
    // Test seam: replaces the content of sweep() (the synthetic PDBs of the unit tests have no real source
    // files behind them). The state check still applies. accept() calls are recorded for the tests.
    void set_sweep_override_for_testing(std::function<Result<Sweep>()> sweep);
    std::vector<std::map<std::string, FileStamp>> accepted_for_testing() const;

    // True for compilands that can be recompiled at all (not a library member, not an import or linker record).
    static bool is_source_compiland(const pdb::PdbIndex& index, std::uint32_t compiland);

private:
    explicit ModuleIndex(ModuleIndexSpec spec);
    void run();
    void set_status(IndexState state, std::uint32_t percent, std::string phase, std::optional<Diag> error = std::nullopt);
    Result<std::shared_ptr<const pdb::PdbIndex>> open_pdb();

    ModuleIndexSpec spec_;
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    IndexStatus status_;
    std::shared_ptr<const pdb::PdbIndex> pdb_;
    SourceBaseline baseline_;
    std::function<Result<Sweep>()> sweep_override_;                       // guarded by mutex_
    std::vector<std::map<std::string, FileStamp>> accepted_for_testing_;  // guarded by mutex_
    std::atomic<bool> cancel_{false};
    std::thread worker_;
};

}  // namespace cml::core
