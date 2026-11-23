#include "cml/core/module_index.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <set>

#include "cml/core/include_scan.h"
#include "cml/core/pipeline_diag_ids.h"
#include "cml/core/static_lib_diag_ids.h"
#include "cml/toolchain/record.h"
#include "cml/util/log.h"

namespace cml::core {

const char* to_string(IndexState state) noexcept {
    switch (state) {
        case IndexState::Pending: return "pending";
        case IndexState::Loading: return "loading";
        case IndexState::Ready: return "ready";
        case IndexState::Failed: return "failed";
    }
    return "?";
}

namespace {
using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::string lower_slashes(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string base_name_of(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

bool file_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec);
}
}  // namespace

std::shared_ptr<ModuleIndex> ModuleIndex::start(ModuleIndexSpec spec) {
    std::shared_ptr<ModuleIndex> index(new ModuleIndex(std::move(spec)));
    index->worker_ = std::thread([raw = index.get()] { raw->run(); });
    return index;
}

ModuleIndex::ModuleIndex(ModuleIndexSpec spec) : spec_(std::move(spec)) {}

ModuleIndex::~ModuleIndex() {
    cancel_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
}

IndexStatus ModuleIndex::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool ModuleIndex::wait(std::uint32_t timeout_ms) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
        return status_.state == IndexState::Ready || status_.state == IndexState::Failed;
    });
}

std::optional<ModuleAudit> ModuleIndex::audit() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return audit_;
}

std::shared_ptr<const pdb::PdbIndex> ModuleIndex::pdb() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pdb_;
}

void ModuleIndex::release_pdb() {
    std::shared_ptr<const pdb::PdbIndex> released;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        released = std::move(pdb_);
        pdb_.reset();
    }
    if (released) {
        CML_LOG(Debug, "index: released the PDB of %s (ADR-0017)", spec_.module_path.c_str());
    }
}

Result<std::shared_ptr<const pdb::PdbIndex>> ModuleIndex::open_pdb() {
    pdb::OpenOptions options = spec_.pdb_options;
    options.use_cache = false;  // the index must be releasable: nothing else may keep the mapping alive
    options.preload_publics = true;
    if (!spec_.pdb_path.empty()) {
        return pdb::PdbIndex::open_pdb(spec_.pdb_path, options);
    }
    return pdb::PdbIndex::open_for_pe(spec_.module_path, options);
}

Result<std::shared_ptr<const pdb::PdbIndex>> ModuleIndex::acquire_pdb() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pdb_) {
            return pdb_;
        }
        if (status_.state == IndexState::Failed) {
            return status_.error ? *status_.error
                                 : Diag::make(pipeline::diag::kIndexUnavailable, "the PDB index of '" + spec_.module_path + "' failed to load",
                                              "See the broker log.");
        }
    }
    // Released (or not yet published): map again. Done outside the lock; the first one in wins.
    Result<std::shared_ptr<const pdb::PdbIndex>> reopened = open_pdb();
    if (!reopened) {
        return reopened.error();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state == IndexState::Failed) {
        // The index failed while we were mapping: do not keep the mapping alive (ADR-0017).
        return status_.error ? *status_.error
                             : Diag::make(pipeline::diag::kIndexUnavailable, "the PDB index of '" + spec_.module_path + "' failed to load", "See the broker log.");
    }
    if (!pdb_) {
        pdb_ = *reopened;
    }
    return pdb_;
}

bool ModuleIndex::is_toolchain_archive(std::string_view library) {
    const std::string l = lower_slashes(library);
    if (l.find("/vc/tools/msvc/") != std::string::npos || l.find("/windows kits/") != std::string::npos || l.find("/lib/clang/") != std::string::npos) {
        return true;  // (the clang runtime libraries live in <llvm>/lib/clang/<version>/lib/windows/)
    }
    const std::size_t slash = l.find_last_of('/');
    std::string stem = slash == std::string::npos ? l : l.substr(slash + 1);
    if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".lib") == 0) {
        stem.resize(stem.size() - 4);
    }
    static const char* const kNames[] = {"libcmt", "libcpmt", "libvcruntime", "libucrt", "msvcrt", "msvcprt", "libconcrt", "oldnames", "vcruntime",
                                         "ucrt", "libclang_rt", "clang_rt", "legacy_stdio_definitions", "msvcmrt"};
    for (const char* name : kNames) {
        const std::string n(name);
        if (stem == n || stem == n + "d") {
            return true;  // libcmt, libcmtd
        }
        if (stem.size() > n.size() && stem.compare(0, n.size(), n) == 0 && (stem[n.size()] == '_' || stem[n.size()] == '.' || stem[n.size()] == '-')) {
            return true;  // clang_rt.builtins-x86_64, legacy_stdio_definitions_x
        }
    }
    return false;
}

bool ModuleIndex::is_object_compiland(const pdb::PdbIndex& index, std::uint32_t compiland) {
    if (compiland >= index.compiland_count()) {
        return false;
    }
    const pdb::Compiland& c = index.compiland(compiland);
    if (c.object_name.empty()) {
        return false;
    }
    return c.object_name.substr(0, 7) != "Import:" && c.object_name.front() != '*';
}

bool ModuleIndex::is_source_compiland(const pdb::PdbIndex& index, std::uint32_t compiland) {
    if (!is_object_compiland(index, compiland)) {
        return false;
    }
    // ADR-0036: the recorded build environment decides, not where the object came from.
    const Result<pdb::BuildEnv> env = index.build_env(compiland);
    if (!env || env->source == pdb::EnvSource::None) {
        return false;
    }
    const auto src = env->source_file();
    return src && !src->empty();
}

std::vector<SkippedCompiland> ModuleIndex::skipped_compilands() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return skipped_;
}

void ModuleIndex::set_status(IndexState state, std::uint32_t percent, std::string phase, std::optional<Diag> error, bool drop_pdb) {
    IndexStatus snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.state = state;
        status_.percent = percent;
        status_.phase = std::move(phase);
        status_.error = std::move(error);
        if (drop_pdb) {
            pdb_.reset();  // in the same critical section as the state change: acquire_pdb() sees Failed or a mapped pdb_, never a gap
        }
        snapshot = status_;
    }
    changed_.notify_all();
    if (spec_.on_change) {
        spec_.on_change(snapshot);
    }
}

void ModuleIndex::run() {
    const Clock::time_point t0 = Clock::now();
    set_status(IndexState::Loading, 2, "opening the PDB");
    Result<std::shared_ptr<const pdb::PdbIndex>> opened = open_pdb();
    if (!opened) {
        CML_LOG(Error, "index: %s", opened.error().format().c_str());
        set_status(IndexState::Failed, 0, "opening the PDB failed", opened.error());
        return;
    }
    const std::shared_ptr<const pdb::PdbIndex> index = *opened;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pdb_ = index;
        status_.pdb_ms = ms_since(t0);
        status_.compilands = index->compiland_count();
    }
    if (cancel_.load(std::memory_order_acquire)) {
        return;
    }
    if (spec_.audit) {
        set_status(IndexState::Loading, 20, "checking the build requirements");
        ModuleAudit audit = audit_opened_module(spec_.module_path, *index);
        if (cancel_.load(std::memory_order_acquire)) {
            return;  // the module was disabled meanwhile: no diagnostics, no Failed state
        }
        for (const AuditFinding& f : audit.findings) {
            if (f.verdict == Verdict::Fail) {
                CML_LOG(Error, "index: %s", f.diag().format().c_str());
            } else if (f.verdict == Verdict::Warn) {
                CML_LOG(Warn, "index: %s", f.diag().format().c_str());
            }
        }
        const AuditFinding* failure = audit.first_failure();
        std::optional<Diag> failure_diag;
        if (failure != nullptr) {
            failure_diag = failure->diag();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            audit_ = audit;
        }
        if (spec_.on_audit) {
            spec_.on_audit(audit);
        }
        if (cancel_.load(std::memory_order_acquire)) {
            return;
        }
        if (failure_diag) {
            // ADR-0017: a module that cannot be enabled must not keep its PDB mapped. State and release are one step.
            set_status(IndexState::Failed, 0, "build requirements not met", failure_diag, /*drop_pdb=*/true);
            return;
        }
    }
    set_status(IndexState::Loading, 30, "reading the file lists");
    std::set<std::string> files;
    std::map<std::string, std::vector<std::uint32_t>> header_deps;
    std::set<std::string> pch_files;
    std::set<std::uint32_t> pch_users;                    // compilands that use a precompiled header
    std::map<std::uint32_t, std::set<std::string>> own_pch;  // what each of them could determine of it
    IncludeScanner scanner;
    const std::uint32_t compiland_total = index->compiland_count();
    std::vector<SkippedCompiland> skipped;
    std::set<std::uint32_t> skipped_set;
    for (std::uint32_t ci = 0; ci < compiland_total; ++ci) {
        if (!is_object_compiland(*index, ci)) {
            continue;  // an import stub or a linker record: nothing to recompile, not counted
        }
        if (cancel_.load(std::memory_order_acquire)) {
            return;
        }
        // WP-26b: a real object is reloadable when its build environment is recorded, whether or not it came from an archive.
        // Otherwise it is skipped: counted, and what it lists is still tracked (include scan and PCH detection run for it as
        // before), so a change to one of its files is reported (CML5921) instead of answering a silent `patched: 0`.
        const pdb::Compiland& comp = index->compiland(ci);
        const std::string library(comp.library_name);
        if (!library.empty() && is_toolchain_archive(library)) {
            skipped_set.insert(ci);  // never counted or tracked: the CRT, the STL, the SDK
            continue;
        }
        Result<pdb::BuildEnv> env = index->build_env(ci);
        std::string reason;
        if (!env || env->source == pdb::EnvSource::None) {
            reason = "no build environment recorded";
        } else if (!env->source_file() || env->source_file()->empty()) {
            reason = "its build environment records no source file";
        } else if (!library.empty() && !file_exists(pdb::normalize_path(*env->source_file(), env->cwd().value_or(std::string_view{})))) {
            reason = "its source file does not exist";
        }
        std::set<std::string> listed;
        for (const std::string_view f : index->compiland_files_normalized(ci)) {
            listed.emplace(f);
        }
        if (reason.empty()) {
            files.insert(listed.begin(), listed.end());
        } else {
            skipped.push_back({ci, base_name_of(comp.object_name), library, reason});
            skipped_set.insert(ci);
            if (!library.empty() && env && env->source_file() && !env->source_file()->empty()) {
                continue;  // an archive member whose source is gone: a library built elsewhere, its files are not on this machine
            }
            for (const std::string& f : listed) {
                if (file_exists(f)) {
                    files.insert(f);
                }
            }
        }
        // The compile command, when it is known, tells whether this compiland builds or uses a precompiled header. That
        // does not depend on scanning includes (WP-15).
        if (!env) {
            CML_LOG(Debug, "index: compiland %u has no build environment, headers come from the PDB list only: %s", ci, env.error().format().c_str());
            continue;
        }
        Result<toolchain::CompileRecord> record = toolchain::record_from_build_env(*env);
        if (!record) {
            continue;
        }
        const ScanRoots roots = scan_roots_from_record(*record);
        std::set<std::string> deps;
        if (spec_.scan_includes) {
            // ADR-0013: the PDB list is not an include graph for clang-cl, so follow the includes ourselves.
            deps = scanner.dependencies(roots);
            for (const std::string& d : deps) {
                files.insert(d);
                header_deps[d].push_back(ci);
            }
        }
        if (roots.creates_pch || roots.uses_pch) {
            // The header named by /Yc or /Yu and the forced includes are the precompiled header, whatever else is known.
            std::set<std::string> mine = scanner.pch_files(roots);
            if (roots.creates_pch) {
                const std::string own = pdb::normalize_path(roots.source, roots.cwd);
                for (const std::string& f : listed) {
                    if (f != own) mine.insert(f);
                }
                mine.insert(deps.begin(), deps.end());
            } else {
                pch_users.insert(ci);
            }
            files.insert(mine.begin(), mine.end());
            pch_files.insert(mine.begin(), mine.end());
            own_pch[ci] = std::move(mine);
        }
        if (ci % 16 == 0) {
            // Stays inside 30..39: the file lists were announced at 30, hashing starts at 40.
            set_status(IndexState::Loading, 30 + 9 * ci / std::max<std::uint32_t>(1, compiland_total),
                       "scanning includes (" + std::to_string(ci) + "/" + std::to_string(compiland_total) + ")");
        }
    }
    // A /Yu compiland whose precompiled header is unknown cannot be recompiled safely: the .pch it would use may be stale.
    std::set<std::uint32_t> pch_unverified;
    for (const std::uint32_t ci : pch_users) {
        if (own_pch[ci].empty() && pch_files.empty()) {
            pch_unverified.insert(ci);
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        skipped_ = skipped;
        skipped_set_ = std::move(skipped_set);
        header_deps_ = std::move(header_deps);
        pch_files_ = std::move(pch_files);
        pch_unverified_ = std::move(pch_unverified);
    }
    CML_LOG(Debug, "index: include scan of %s: %zu files read, %zu headers mapped", spec_.module_path.c_str(), scanner.files_read(), scanned_headers());
    const Clock::time_point t1 = Clock::now();
    std::vector<std::string> list(files.begin(), files.end());
    BaselineOptions options = spec_.baseline;
    options.cancelled = [this] { return cancel_.load(std::memory_order_acquire); };
    options.progress = [this](std::size_t done, std::size_t total) {
        const std::uint32_t percent = total == 0 ? 99u : 40u + static_cast<std::uint32_t>(59u * done / total);
        set_status(IndexState::Loading, percent, "hashing sources (" + std::to_string(done) + "/" + std::to_string(total) + ")");
    };
    set_status(IndexState::Loading, 40, "hashing sources (0/" + std::to_string(list.size()) + ")");
    std::vector<Diag> warnings;
    baseline_.take(list, options, &warnings);
    for (const Diag& w : warnings) {
        CML_LOG(Warn, "index: %s", w.format().c_str());
    }
    if (cancel_.load(std::memory_order_acquire)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.files = baseline_.size();
        status_.baseline_ms = ms_since(t1);
    }
    CML_LOG(Info, "index: %s ready: %zu compilands, %zu tracked files of %zu listed (pdb %.1f ms, baseline %.1f ms)", spec_.module_path.c_str(),
            static_cast<std::size_t>(index->compiland_count()), baseline_.size(), list.size(), status().pdb_ms, status().baseline_ms);
    {
        std::size_t archive_members = 0;
        for (const SkippedCompiland& k : skipped) {
            archive_members += k.library.empty() ? 0 : 1;
        }
        CML_LOG(Info, "index: %s: %zu real objects skipped (%zu from archives)", spec_.module_path.c_str(), skipped.size(), archive_members);
        if (archive_members != 0 && spec_.on_skipped_archive_members && !cancel_.load(std::memory_order_acquire)) {
            std::string names;
            std::size_t listed_names = 0;
            for (const SkippedCompiland& k : skipped) {
                if (k.library.empty() || listed_names >= 5) {
                    continue;
                }
                names += (listed_names == 0 ? "" : ", ") + k.name + " (" + k.reason + ")";
                ++listed_names;
            }
            const std::string module_name = base_name_of(spec_.module_path);
            spec_.on_skipped_archive_members(Diag::make(
                staticlib::diag::kSkippedArchiveCompilands,
                std::to_string(archive_members) + " object(s) linked into " + module_name + " from static libraries cannot be hot-reloaded: " + names +
                    (archive_members > listed_names ? ", and " + std::to_string(archive_members - listed_names) + " more" : std::string()) + ".",
                "Build those libraries with /Z7 (or /Zi) so the PDB records how each object was compiled (clang-cl: -gcodeview-command-line), and keep their sources on disk; "
                "edits to the other members of the same libraries are reloaded normally."));
        }
    }
    set_status(IndexState::Ready, 100, "ready");
}

Result<ModuleIndex::Sweep> ModuleIndex::sweep() {
    if (status().state != IndexState::Ready) {
        return Diag::make(pipeline::diag::kIndexUnavailable, "the PDB index of '" + spec_.module_path + "' is not ready (" + to_string(status().state) + ")",
                          "Wait for the module's index (control command `wait`) and try again.");
    }
    Result<std::shared_ptr<const pdb::PdbIndex>> index = acquire_pdb();
    if (!index) {
        return index.error();
    }
    std::function<Result<Sweep>()> override_sweep;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        override_sweep = sweep_override_;
    }
    if (override_sweep) {
        Result<Sweep> overridden = override_sweep();
        if (overridden) {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const std::uint32_t ci : overridden->compilands) {
                if (pch_unverified_.count(ci) != 0) overridden->pch_unverified.push_back(ci);
            }
        }
        return overridden;
    }
    const SourceBaseline::SweepResult raw = baseline_.sweep();
    Sweep out;
    out.changed = raw.changed;
    out.stamps = raw.stamps;
    out.missing = raw.missing;
    out.hashed = raw.hashed;
    out.checked = raw.checked;
    std::set<std::uint32_t> compilands;
    // One lock for what the per-file loop reads of the skipped compilands.
    std::set<std::uint32_t> skipped_here;
    std::vector<SkippedCompiland> skipped_list;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        skipped_here = skipped_set_;
        skipped_list = skipped_;
    }
    for (const std::string& file : raw.changed) {
        bool mapped = false;
        std::set<std::uint32_t> seen;
        const auto map_to = [&](std::uint32_t ci) {
            if (seen.insert(ci).second) {
                compilands.insert(ci);
                out.file_compilands[file].push_back(ci);
            }
            mapped = true;
        };
        std::set<std::uint32_t> skipped_owners;  // the skipped compilands that list or include the file
        for (const std::uint32_t ci : (*index)->compilands_for_file(file)) {
            if (!is_object_compiland(**index, ci)) {
                continue;
            }
            if (skipped_here.count(ci) != 0) {
                skipped_owners.insert(ci);
            } else {
                map_to(ci);
            }
        }
        // WP-15: the scanned include graph (ADR-0013) adds the compilands whose PDB list lacks the header. Only the
        // lookups happen under the lock; nothing is copied.
        std::vector<std::uint32_t> from_scan;
        bool is_pch = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (const auto it = header_deps_.find(file); it != header_deps_.end()) {
                from_scan = it->second;
            }
            is_pch = pch_files_.count(file) != 0;
        }
        for (const std::uint32_t ci : from_scan) {
            if (skipped_here.count(ci) != 0) {
                skipped_owners.insert(ci);
            } else {
                map_to(ci);
            }
        }
        if (is_pch) {
            out.pch_headers.push_back(file);
        }
        if (!mapped) {
            // WP-26b: a file that only skipped compilands own is not "unmapped": say who owns it and why it is not reloadable.
            std::string detail;
            for (const SkippedCompiland& k : skipped_list) {
                if (skipped_owners.count(k.compiland) != 0) {
                    detail += (detail.empty() ? "" : ", ") + k.name + (k.library.empty() ? "" : " in " + base_name_of(k.library)) + " (" + k.reason + ")";
                }
            }
            if (!detail.empty()) {
                out.not_owned.push_back({file, detail});
            } else {
                out.unmapped.push_back(file);
            }
        }
    }
    out.compilands.assign(compilands.begin(), compilands.end());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const std::uint32_t ci : out.compilands) {
            if (pch_unverified_.count(ci) != 0) out.pch_unverified.push_back(ci);
        }
    }
    return out;
}

std::size_t ModuleIndex::scanned_headers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return header_deps_.size();
}

bool ModuleIndex::is_pch_header(const std::string& normalised_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pch_files_.count(normalised_path) != 0;
}

void ModuleIndex::accept(const std::map<std::string, FileStamp>& stamps) {
    baseline_.accept(stamps);
    std::lock_guard<std::mutex> lock(mutex_);
    if (sweep_override_) {
        accepted_for_testing_.push_back(stamps);
    }
}

void ModuleIndex::set_sweep_override_for_testing(std::function<Result<Sweep>()> sweep) {
    std::lock_guard<std::mutex> lock(mutex_);
    sweep_override_ = std::move(sweep);
}

std::vector<std::map<std::string, FileStamp>> ModuleIndex::accepted_for_testing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return accepted_for_testing_;
}

}  // namespace cml::core
