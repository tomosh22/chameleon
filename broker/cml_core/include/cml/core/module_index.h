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
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cml/core/module_audit.h"
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
    bool scan_includes = true;     // WP-15: follow #include through the files on disk (ADR-0013); off in tests that have no sources
    BaselineOptions baseline;      // progress / cancelled are set by the index itself
    // Called from the worker thread on every state or phase change (after the status was updated).
    std::function<void(const IndexStatus&)> on_change;
    // WP-18: audit the module against the build requirements (module_audit.h) once its PDB is open. A FAIL finding fails
    // the index with that finding's diagnostic (the PDB is released); `on_audit` hears every audit (worker thread).
    bool audit = false;
    std::function<void(const ModuleAudit&)> on_audit;
    // WP-26b: called once from the worker thread when the index is built and archive members were skipped (CML5920).
    std::function<void(const Diag&)> on_skipped_archive_members;
    // WP-45: the source baseline an earlier instance of the same module left (it was freed and loaded again, or a hot-restart
    // carried it to a module the new process enabled late). Applied right after the baseline is taken, before the index is Ready,
    // so every reload that sees the index sees it: SourceBaseline::restore (the files patched before count as changed again and
    // the next reload re-applies them), or SourceBaseline::adopt when `carried_live` (the same image was disabled and enabled
    // again; it still runs the patches). carried_files() says how many files it covered.
    std::optional<BaselineSnapshot> carried;
    bool carried_live = false;
};

// WP-26b: a compiland that cannot be reloaded although its object is real (no build environment, missing source file).
struct SkippedCompiland {
    std::uint32_t compiland = 0;
    std::string name;     // the object's file name
    std::string library;  // the archive it came from; empty for a plain object
    std::string reason;
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
    // The enable-time audit (spec.audit); nullopt before it ran or when it was not asked for.
    std::optional<ModuleAudit> audit() const;

    // WP-15: the include graph scanned when the index was built (IncludeScanner): number of headers with at least
    // one compiland that includes them, and whether a file is part of a precompiled header.
    std::size_t scanned_headers() const;
    bool is_pch_header(const std::string& normalised_path) const;

    // Compilands that have at least one changed file (indices ascending), the changed files, and changed
    // files that no compiland lists. `stamps` goes to accept() once the files were patched.
    struct Sweep {
        std::vector<std::string> changed;
        std::vector<std::uint32_t> compilands;
        std::vector<std::string> unmapped;
        // WP-26b: changed source files that only compilands which cannot be reloaded own. `detail` names them and says why.
        struct NotOwned {
            std::string file;
            std::string detail;
        };
        std::vector<NotOwned> not_owned;
        std::vector<std::string> missing;
        std::map<std::string, std::vector<std::uint32_t>> file_compilands;  // changed file -> the compilands that list or include it
        // Changed files that belong to a precompiled header (WP-15): the /Yc compiland's headers. A change to one
        // cannot be hot-reloaded (CML5550).
        std::vector<std::string> pch_headers;
        // Compilands to recompile that use a precompiled header whose files could not be determined (CML5552).
        std::vector<std::uint32_t> pch_unverified;
        std::map<std::string, FileStamp> stamps;
        std::size_t hashed = 0;
        std::size_t checked = 0;
    };
    // Requires Ready and a mapped index (acquire_pdb() first).
    Result<Sweep> sweep();
    void accept(const std::map<std::string, FileStamp>& stamps);
    // WP-21 (hot-restart): the baseline state to carry to the relaunched process, and its application there. Requires Ready.
    BaselineSnapshot baseline_snapshot() const { return baseline_.session_snapshot(); }
    std::size_t restore_session_baseline(const BaselineSnapshot& snapshot) { return baseline_.restore(snapshot); }
    // WP-45: the files of spec.carried this index applied (restored, or adopted when carried_live); 0 without a carried baseline.
    std::size_t carried_files() const { return carried_files_.load(std::memory_order_acquire); }
    // WP-22: the stamps the baseline holds for these files now, and the undo of an accept() (the stamps go back as they were).
    std::map<std::string, FileStamp> baseline_stamps(const std::vector<std::string>& files) const { return baseline_.stamps_of(files); }
    void restore_baseline(const std::map<std::string, FileStamp>& stamps) { baseline_.accept(stamps); }
    // The files the baseline tracks (empty until Ready). WP-14 watches their directories.
    std::vector<std::string> tracked_files() const { return baseline_.files(); }
    // Test seam: replaces the content of sweep() (the synthetic PDBs of the unit tests have no real source
    // files behind them). The state check still applies. accept() calls are recorded for the tests.
    void set_sweep_override_for_testing(std::function<Result<Sweep>()> sweep);
    std::vector<std::map<std::string, FileStamp>> accepted_for_testing() const;

    // WP-26b (ADR-0036): a compiland whose object is real (not `Import:`, not `* Linker *`), whether or not it came from an
    // archive. No build-environment check, so it is cheap.
    static bool is_object_compiland(const pdb::PdbIndex& index, std::uint32_t compiland);
    // True for compilands that can be recompiled: a real object whose build environment (cwd, compiler, command, source) is
    // recorded, whether or not it came from an archive. (The index additionally skips an archive member whose source file is not
    // on this machine, a library built elsewhere; that needs the file system, so it is not part of this predicate.) Reads the module's build info (lazily), so avoid it in hot loops.
    static bool is_source_compiland(const pdb::PdbIndex& index, std::uint32_t compiland);

    // WP-26b: archives that belong to the toolchain, not to the project (the CRT, the STL, the SDK, the compiler runtime). Their
    // members never record a build environment, so they are neither counted as skipped (CML5920) nor tracked, and the audit's
    // samplers leave them out. One rule for the index and the audit.
    static bool is_toolchain_archive(std::string_view library);

    // WP-26b: the real-object compilands that were skipped (empty until Ready), and the subset that comes from archives.
    std::vector<SkippedCompiland> skipped_compilands() const;

private:
    explicit ModuleIndex(ModuleIndexSpec spec);
    void run();
    void set_status(IndexState state, std::uint32_t percent, std::string phase, std::optional<Diag> error = std::nullopt, bool drop_pdb = false);
    Result<std::shared_ptr<const pdb::PdbIndex>> open_pdb();

    ModuleIndexSpec spec_;
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    IndexStatus status_;
    std::optional<ModuleAudit> audit_;
    std::shared_ptr<const pdb::PdbIndex> pdb_;
    SourceBaseline baseline_;
    std::function<Result<Sweep>()> sweep_override_;                       // guarded by mutex_
    std::vector<std::map<std::string, FileStamp>> accepted_for_testing_;  // guarded by mutex_
    std::map<std::string, std::vector<std::uint32_t>> header_deps_;       // guarded by mutex_: scanned header -> compilands
    std::set<std::string> pch_files_;                                     // guarded by mutex_: files of the precompiled header(s)
    std::set<std::uint32_t> pch_unverified_;                              // guarded by mutex_: /Yu compilands with an unknown PCH
    std::vector<SkippedCompiland> skipped_;                               // guarded by mutex_
    std::set<std::uint32_t> skipped_set_;                                 // guarded by mutex_: their indices
    std::atomic<bool> cancel_{false};
    std::atomic<std::size_t> carried_files_{0};  // WP-45
    std::thread worker_;
};

}  // namespace cml::core
