#include "cml/core/module_index.h"

#include <algorithm>
#include <chrono>
#include <set>

#include "cml/core/pipeline_diag_ids.h"
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
    if (!pdb_) {
        pdb_ = *reopened;
    }
    return pdb_;
}

bool ModuleIndex::is_source_compiland(const pdb::PdbIndex& index, std::uint32_t compiland) {
    if (compiland >= index.compiland_count()) {
        return false;
    }
    const pdb::Compiland& c = index.compiland(compiland);
    if (!c.library_name.empty() || c.object_name.empty()) {
        return false;
    }
    return c.object_name.substr(0, 7) != "Import:" && c.object_name.front() != '*';
}

void ModuleIndex::set_status(IndexState state, std::uint32_t percent, std::string phase, std::optional<Diag> error) {
    IndexStatus snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.state = state;
        status_.percent = percent;
        status_.phase = std::move(phase);
        status_.error = std::move(error);
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
            if (failure_diag) {
                pdb_.reset();  // ADR-0017: a module that cannot be enabled must not keep its PDB mapped
            }
        }
        if (spec_.on_audit) {
            spec_.on_audit(audit);
        }
        if (failure_diag) {
            set_status(IndexState::Failed, 0, "build requirements not met", failure_diag);
            return;
        }
    }
    set_status(IndexState::Loading, 30, "reading the file lists");
    std::set<std::string> files;
    for (std::uint32_t ci = 0; ci < index->compiland_count(); ++ci) {
        if (!is_source_compiland(*index, ci)) {
            continue;
        }
        for (const std::string_view f : index->compiland_files_normalized(ci)) {
            files.emplace(f);
        }
    }
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
        return override_sweep();
    }
    const SourceBaseline::SweepResult raw = baseline_.sweep();
    Sweep out;
    out.changed = raw.changed;
    out.stamps = raw.stamps;
    out.missing = raw.missing;
    out.hashed = raw.hashed;
    out.checked = raw.checked;
    std::set<std::uint32_t> compilands;
    for (const std::string& file : raw.changed) {
        bool mapped = false;
        for (const std::uint32_t ci : (*index)->compilands_for_file(file)) {
            if (is_source_compiland(**index, ci)) {
                compilands.insert(ci);
                out.file_compilands[file].push_back(ci);
                mapped = true;
            }
        }
        if (!mapped) {
            out.unmapped.push_back(file);
        }
    }
    out.compilands.assign(compilands.begin(), compilands.end());
    return out;
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
