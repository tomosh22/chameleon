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

#include "cml/core/include_scan.h"
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
    // WP-50 (R-55): the other phases of the build, and what the include scan read (IncludeScanner::Stats). Set when each ends.
    double audit_ms = 0;       // the build-requirement checks (spec.audit)
    double scan_ms = 0;        // the file lists of the compilands and the include scan (ADR-0013)
    double scan_read_ms = 0;   //   of scan_ms: in the file reader
    double scan_parse_ms = 0;  //   of scan_ms: parsing #include lines
    std::size_t scan_found = 0;
    std::size_t scan_missing = 0;
    std::size_t scan_lookups = 0;
    std::size_t baseline_files = 0;  // files the baseline hashed (= files listed)
    // WP-50: where the file lists and the include scan came from: "cold" (scanned now), "warm" (ScanCache, the same module file
    // scanned before in this session), "off" (no cache given). WP-55: "partial" when only the compilands whose inputs changed
    // were scanned and the others came from the cache.
    std::string scan_cache = "off";
    std::size_t scan_reused = 0;     // WP-55: compilands whose part came from the cache
    std::size_t scan_rescanned = 0;  //   ... and compilands scanned
};

class ScanCache;

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
    // WP-50 (R-55): the file lists and the include graph of a module file this session indexed before are taken from here when
    // the file is the same (ScanCache); null: always scanned.
    std::shared_ptr<ScanCache> scan_cache;
    // Tests (WP-50): called on the worker thread after the scan of the file lists and includes, before its inputs are compared
    // with the stamps taken while scanning (an edit made "during the scan").
    std::function<void()> after_scan_for_testing;
};

// WP-26b: a compiland that cannot be reloaded although its object is real (no build environment, missing source file).
struct SkippedCompiland {
    std::uint32_t compiland = 0;
    std::string name;     // the object's file name
    std::string library;  // the archive it came from; empty for a plain object
    std::string reason;
};

// WP-50 (R-55): the identity of a module file for the scan cache: its size and last-write time, the GUID and age of the PDB it
// was indexed with, and whether includes were scanned (ModuleIndexSpec::scan_includes).
struct ModuleFileStamp {
    std::uint64_t size = 0;
    long long mtime = 0;
    pdb::PdbId pdb;
    bool scan_includes = true;
};
// "" when equal, else the first difference: "size", "mtime", "pdb" or "options".
std::string stamp_difference(const ModuleFileStamp& cached, const ModuleFileStamp& now);
// The stamp of `module_path` indexed as `index`; nullopt when the file cannot be stat'ed.
std::optional<ModuleFileStamp> module_file_stamp(const std::string& module_path, const pdb::PdbIndex& index, bool scan_includes);

// WP-50 (review): one file or directory a scan result depends on, as it was when the scan ended. `exists` false records a path
// whose absence the result depends on; a directory records only its last-write time (a file created in or removed from it).
struct ScanInput {
    std::string path;
    bool exists = false;
    bool directory = false;
    std::uint64_t size = 0;
    long long mtime = 0;
};
// What `path` is now.
ScanInput probe_scan_input(const std::string& path);
// Whether two stamps of the same path agree (existence, kind, size, time).
bool same_scan_input(const ScanInput& a, const ScanInput& b);
// "" when every input is still as recorded, else the first path that differs (changed, created or removed).
std::string changed_scan_input(const std::vector<ScanInput>& inputs);

// WP-50 (second review): the stamps of a scan's inputs, each taken before the scan used it, so that a result built from files
// that changed while it was built, or that met a file it could not read, is never cached. One per scan; not thread-safe.
class ScanInputRecorder {
public:
    // The stamp of `path` from the first time it was asked for (taken now if this is the first time).
    const ScanInput& before(const std::string& path);
    bool exists(const std::string& path) { return before(path).exists; }
    // The stamp before() took for `path`, or null when it was never asked for.
    const ScanInput* stamped(const std::string& path) const;
    // `disk` with every read stamped first: the file's directory (a file created there later changes its stamp), then the file
    // (not when its directory does not exist). A read that fails for a file that exists is remembered as unreadable, unless the
    // stamp taken before the read says the file is over kScanMaxFileBytes (WP-55, R-63): disk_file_reader() never reads such a
    // file, so every read gives the same answer until the file's stamp changes, and the file is an input like any other. A
    // directory is not a failed read either. The reader refers to this recorder, which must outlive it.
    ScanFileReader wrap(ScanFileReader disk);
    // Why a result that used `path` must not be cached: "unreadable:<path>" when a read of it failed although it existed (a
    // sharing violation, a read error: it may succeed next time), "changed-during-scan:<path>" when its stamp now differs from
    // the one taken before it was used, "" when neither. The path is stamped again on the first call for it.
    std::string not_cacheable(const std::string& path);

private:
    std::map<std::string, ScanInput> before_;
    std::set<std::string> unreadable_;
    std::map<std::string, bool> rechecked_;  // path -> its stamp now differs from before_
};

// WP-55 (R-58): one compiland's part of a scan. Paths are indices into ScanResult::inputs.
struct CompilandScan {
    std::uint32_t compiland = 0;
    bool toolchain = false;              // a member of a toolchain archive: skipped, nothing tracked (is_toolchain_archive)
    std::optional<SkippedCompiland> skipped;
    std::vector<std::uint32_t> files;    // the files it makes tracked (unique)
    std::vector<std::uint32_t> deps;     // the headers its include scan found (ScanResult::header_deps)
    std::vector<std::uint32_t> pch;      // what it could determine of the precompiled header it builds or uses
    bool uses_pch = false;               // a /Yu compiland (ScanResult::pch_unverified is decided over the whole module)
    // Every file and directory its part depends on, with the stamp taken before the scan used it: the files its PDB list names,
    // an archive member's source, every file its include walks read or were answered about from the scanner's memory (one over
    // the size limit too), and the directory of every path they asked about that did not exist (a file created there would be
    // found). Unique.
    std::vector<std::uint32_t> inputs;
    // Why this part must not be reused ("" when it may): ScanInputRecorder::not_cacheable() of one of its inputs.
    std::string not_cacheable;
};

// What the file-list and include-scan phase of an index build produces (all of it depends only on the PDB, the compile
// commands it records and the files on disk).
struct ScanResult {
    std::set<std::string> files;                                     // tracked files (the baseline hashes them)
    std::map<std::string, std::vector<std::uint32_t>> header_deps;   // scanned header -> compilands
    std::set<std::string> pch_files;
    std::set<std::uint32_t> pch_unverified;
    std::vector<SkippedCompiland> skipped;
    std::set<std::uint32_t> skipped_set;
    // WP-55 (R-58): the part of every object compiland (ascending), from which the members above are assembled
    // (assemble_scan_result). A cache entry is reused compiland by compiland.
    std::vector<CompilandScan> compilands;
    // Every path a compiland refers to, once, with the stamp taken before the scan used it (a lookup checks each of them again).
    // Without a cache only the paths are set.
    std::vector<ScanInput> inputs;
    IncludeScanner::Stats stats;  // of the scan that produced it (a partial scan: of the compilands it scanned)
    std::size_t files_read = 0;
    std::size_t reused = 0;     // compilands taken from a cache entry
    std::size_t rescanned = 0;  // compilands scanned
    double scan_ms = 0;
};

// Fills files, header_deps, pch_files, pch_unverified, skipped and skipped_set from `compilands` and `inputs`.
void assemble_scan_result(ScanResult& result);

// WP-50 (R-55): the scan results of the module files this session indexed, so that a module indexed again (the new process of a
// hot-restart, a module freed and loaded again, a second process of the same executable) is not scanned again while its file is
// unchanged. WP-55 (R-58): a compiland's part is reused while every input of that part is as it was. Only the compilands with a
// changed input (a source edited since, a header created where an include could now resolve, a skipped member's source that
// appeared) or with a part that could not be cached are scanned again, and the merged result replaces the entry. Thread-safe;
// at most kMaxEntries module files (the least recently used goes).
class ScanCache {
public:
    static constexpr std::size_t kMaxEntries = 32;
    struct Lookup {
        std::shared_ptr<const ScanResult> result;  // the entry of the same module stamp (null: none, or another stamp)
        // Per element of result->compilands: "" when it can be reused, else why not ("file-changed:<path>",
        // "not-cached:<reason>").
        std::vector<std::string> stale;
        std::size_t reusable = 0;
        // "" when the whole entry can be used as it is; else "absent", stamp_difference(), "file-changed:<path>" (the first input
        // that differs) or "not-cached:<reason>" (the first part that could not be cached).
        std::string reason;
    };
    // The entry of `module_path` and what of it is still true. The inputs are stamped again outside the lock, each once.
    Lookup lookup(const std::string& module_path, const ModuleFileStamp& stamp);
    // The entry when all of it can be used as it is; otherwise null, and `*reason` is Lookup::reason.
    std::shared_ptr<const ScanResult> find(const std::string& module_path, const ModuleFileStamp& stamp, std::string* reason);
    void store(const std::string& module_path, const ModuleFileStamp& stamp, std::shared_ptr<const ScanResult> result);
    std::size_t size() const;

private:
    struct Entry {
        ModuleFileStamp stamp;
        std::shared_ptr<const ScanResult> result;
        std::uint64_t used = 0;
    };
    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;  // by normalised path
    std::uint64_t clock_ = 0;
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
    // The file lists and the include scan of every compiland (WP-50: cached as a ScanResult). WP-55: the parts of `previous`
    // that can be reused are taken from it, the others are scanned. nullopt when cancelled.
    std::optional<ScanResult> scan_files(const pdb::PdbIndex& index, const ScanCache::Lookup& previous);

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
