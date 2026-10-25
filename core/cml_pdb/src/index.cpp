// PdbIndex: opening (path / PE), the (GUID, age) cache and the simple accessors.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "PDB.h"
#include "cml/pdb/diag_ids.h"
#include "cml/pdb/pdb.h"
#include "cml/util/log.h"
#include "detail/codeview.h"
#include "detail/le.h"
#include "detail/parallel.h"
#include "cml/util/fail_fast.h"
#include "index_impl.h"

namespace cml::pdb {

using namespace detail;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

std::string_view base_name(std::string_view path) noexcept {
    const std::size_t pos = path.find_last_of("\\/");
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

// "...\logic.cpp.obj" -> "logic"; "x.obj" -> "x".
std::string object_stem(std::string_view object_name) {
    std::string s(base_name(object_name));
    for (char& c : s) {
        c = lower(c);
    }
    auto strip = [&s](const char* suffix) {
        const std::size_t n = std::strlen(suffix);
        if (s.size() > n && s.compare(s.size() - n, n, suffix) == 0) {
            s.erase(s.size() - n);
            return true;
        }
        return false;
    };
    if (!strip(".obj")) {
        strip(".o");
    }
    for (const char* ext : {".cpp", ".cc", ".cxx", ".c"}) {
        if (strip(ext)) {
            break;
        }
    }
    return s;
}

// Absolute on Windows ("C:\\x", "\\\\server\\x"); on the POSIX hosts used for portable syntax-check builds also "/x".
bool is_native_absolute(const std::string& path) noexcept {
#if defined(_WIN32)
    return is_absolute_path(path);
#else
    return is_absolute_path(path) || (!path.empty() && path[0] == '/');
#endif
}

// ---- cache ---------------------------------------------------------------------------------

struct CacheSlot {
    std::mutex mutex;  // held while the index for this key is being built
    std::weak_ptr<const PdbIndex> index;
};

// Identity alone is not enough: a /PDBSTRIPPED PDB and the full PDB share GUID and age but not
// content, so the canonical file path is part of the key.
struct CacheKey {
    PdbId id;
    std::string path;
    bool operator<(const CacheKey& o) const noexcept { return id != o.id ? id < o.id : path < o.path; }
};

struct Cache {
    std::mutex mutex;
    std::map<CacheKey, std::shared_ptr<CacheSlot>> slots;

    // Drops entries whose index is gone and that no opener holds. Caller holds `mutex`.
    // Openers copy the slot pointer under `mutex` and only then lock the slot, so a slot whose
    // mutex is free may still be about to be built; use_count() == 1 (only the map) is the
    // reliable "nobody else has it" test, because copies are only taken under `mutex`.
    void sweep() {
        for (auto it = slots.begin(); it != slots.end();) {
            bool erase = false;
            if (it->second.use_count() == 1) {
                // Uncontended: nobody else holds the slot. Locking orders this read after the
                // last builder's write of `index` (which happened under the slot mutex).
                std::lock_guard<std::mutex> slot_lock(it->second->mutex);
                erase = it->second->index.expired();
            }
            if (erase) {
                it = slots.erase(it);
            } else {
                ++it;
            }
        }
    }
};

Cache& cache() {
    static Cache* c = new Cache();  // intentionally leaked: usable during static destruction
    return *c;
}

}  // namespace

Result<PdbProbe> probe_pdb(const std::string& path) {
    const auto t_map = Clock::now();
    auto mapped = MappedFile::open(path);
    if (!mapped) {
        return mapped.error();
    }
    PdbProbe probe;
    probe.file = std::move(mapped).value();
    probe.map_ms = ms_since(t_map);
    const auto t_msf = Clock::now();

    // raw_pdb's validation first (super block, free block map). Its parsers are not hardened
    // against arbitrary input, which is why this library only uses it as a gate.
    const PDB::ErrorCode raw = PDB::ValidateFile(probe.file->data(), static_cast<std::size_t>(probe.file->size()));
    auto msf = Msf::open(probe.file);
    if (!msf) {
        return msf.error();  // precise diagnostics (bad magic, truncation, ...) come from our own checks
    }
    if (raw != PDB::ErrorCode::Success) {
        return Diag::make(diag::kUnsupportedPdbFormat,
                          "'" + path + "' failed super block validation (raw_pdb error " + std::to_string(static_cast<unsigned>(raw)) + ").",
                          "The file is damaged; rebuild the module so the linker regenerates the PDB.");
    }
    probe.msf = std::move(msf).value();

    const StreamData info = probe.msf->read_stream(1);
    if (info.size() < 28) {
        return Diag::make(diag::kBadInfoStream, "The PDB info stream of '" + path + "' is missing or too short.",
                          "Rebuild the module so the linker regenerates the PDB.");
    }
    probe.info_age = rd32(info.data() + 8);
    std::memcpy(probe.id.guid.data(), info.data() + 12, 16);
    // Debuggers match the image's RSDS age against the DBI age, so that is the age of the identity.
    // A PDB whose DBI stream cannot be read yet falls back to the info-stream age (open() then
    // fails with a DBI diagnostic anyway).
    probe.id.age = probe.info_age;
    {
        CountedBuffer scratch;
        std::size_t got = 0;
        const std::uint8_t* dbi = probe.msf->read_range(3, 0, 12, scratch, got);
        if (dbi != nullptr && got >= 12 && rd32(dbi) == 0xFFFFFFFFu) {
            probe.id.age = rd32(dbi + 8);
        }
    }
    if (probe.id.age != probe.info_age) {
        CML_LOG(Debug, "cml_pdb: '%s': info-stream age %u differs from DBI age %u; using the DBI age", path.c_str(), probe.info_age,
                probe.id.age);
    }
    probe.msf_ms = ms_since(t_msf);
    return probe;
}

struct IndexBuilder {
    static Result<std::shared_ptr<const PdbIndex>> build(PdbProbe&& probe, const std::string& path, const OpenOptions& options) {
        auto impl = std::make_unique<PdbIndex::Impl>();
        impl->options = options;
        if (impl->options.threads == 0) {
            impl->options.threads = default_thread_count();
        }
        impl->file = std::move(probe.file);
        impl->msf = std::move(probe.msf);
        impl->id = probe.id;
        impl->info_age = probe.info_age;
        impl->path = path;
        impl->open_timings.map_ms = probe.map_ms;
        impl->open_timings.msf_ms = probe.msf_ms;
        const auto t0 = Clock::now();
        if (auto r = build_index(*impl); !r) {
            return r.error();
        }
        impl->module_state = std::make_unique<std::atomic<std::uint8_t>[]>(impl->compilands.size());
        for (std::size_t i = 0; i < impl->compilands.size(); ++i) {
            impl->module_state[i].store(0, std::memory_order_relaxed);
        }
        impl->module_symbols.resize(impl->compilands.size());
        impl->module_errors.resize(impl->compilands.size());
        std::shared_ptr<PdbIndex> index(new PdbIndex(std::move(impl)));
        CML_LOG(Debug, "cml_pdb: indexed '%s' (%u compilands, %zu sections) in %.1f ms", path.c_str(), index->compiland_count(),
                index->sections().size(), ms_since(t0));
        if (options.preload_all_symbols) {
            if (auto r = index->load_publics(); !r) {
                return r.error();
            }
            if (auto r = index->load_all(); !r) {
                return r.error();
            }
        } else if (options.preload_publics) {
            if (auto r = index->load_publics(); !r) {
                return r.error();
            }
        }
        return std::shared_ptr<const PdbIndex>(std::move(index));
    }
};

namespace {

// Preload options are honoured on a shared index too: the tables are built (once) if they are not
// there yet, so a caller asking for preload_all_symbols always gets them.
Result<void> apply_preload(const PdbIndex& index, const OpenOptions& options) {
    if (options.preload_all_symbols) {
        if (auto r = index.load_publics(); !r) {
            return r.error();
        }
        return index.load_all();
    }
    if (options.preload_publics) {
        return index.load_publics();
    }
    return Result<void>();
}

Result<std::shared_ptr<const PdbIndex>> open_probed(PdbProbe&& probe, const std::string& path, const OpenOptions& options) {
    if (!options.symbol_server.empty()) {
        CML_LOG(Debug, "cml_pdb: symbol server '%s' ignored (not implemented)", options.symbol_server.c_str());
    }
    if (!options.use_cache) {
        return IndexBuilder::build(std::move(probe), path, options);
    }
    Cache& c = cache();
    std::shared_ptr<CacheSlot> slot;
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        c.sweep();
        std::shared_ptr<CacheSlot>& entry = c.slots[CacheKey{probe.id, canonical_file_path(path)}];
        if (!entry) {
            entry = std::make_shared<CacheSlot>();
        }
        slot = entry;
    }
    std::lock_guard<std::mutex> build_lock(slot->mutex);
    if (std::shared_ptr<const PdbIndex> existing = slot->index.lock()) {
        CML_LOG(Trace, "cml_pdb: cache hit for %s", probe.id.to_string().c_str());
        if (auto r = apply_preload(*existing, options); !r) {
            return r.error();
        }
        return existing;
    }
    auto built = IndexBuilder::build(std::move(probe), path, options);
    if (built) {
        slot->index = built.value();
    }
    return built;
}

}  // namespace

PdbIndex::PdbIndex(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PdbIndex::~PdbIndex() = default;

Result<std::shared_ptr<const PdbIndex>> PdbIndex::open_pdb(const std::string& pdb_path, const OpenOptions& options) {
    auto probe = probe_pdb(pdb_path);
    if (!probe) {
        return probe.error();
    }
    return open_probed(std::move(probe).value(), pdb_path, options);
}

Result<std::shared_ptr<const PdbIndex>> PdbIndex::open_for_pe(const std::string& pe_path, const OpenOptions& options) {
    auto ref = read_codeview_ref(pe_path);
    if (!ref) {
        return ref.error();
    }
    const std::string pe_dir = dir_name(pe_path);
    const std::string recorded = ref->pdb_path;
    const std::string file = file_name(recorded);

    // Candidate order: recorded path, next to the PE, override directories.
    std::vector<std::string> candidates;
    if (!recorded.empty()) {
        // A relative recorded path is relative to the image's directory.
        candidates.push_back(is_native_absolute(recorded) ? recorded : join_path(pe_dir, recorded));
    }
    if (!file.empty()) {
        candidates.push_back(join_path(pe_dir, file));
        for (const std::string& dir : options.override_dirs) {
            candidates.push_back(join_path(dir, file));
        }
    }
    std::vector<std::string> tried;
    std::string mismatch_path;
    PdbId mismatch_id;
    for (const std::string& cand : candidates) {
        if (std::find(tried.begin(), tried.end(), cand) != tried.end()) {
            continue;
        }
        tried.push_back(cand);
        if (!file_exists(cand)) {
            CML_LOG(Trace, "cml_pdb: no PDB at '%s'", cand.c_str());
            continue;
        }
        auto probe = probe_pdb(cand);
        if (!probe) {
            CML_LOG(Debug, "cml_pdb: candidate '%s' unusable: %s", cand.c_str(), probe.error().format().c_str());
            continue;
        }
        if (probe->id != ref->id) {
            CML_LOG(Debug, "cml_pdb: candidate '%s' is %s, image wants %s", cand.c_str(), probe->id.to_string().c_str(),
                    ref->id.to_string().c_str());
            if (mismatch_path.empty()) {
                mismatch_path = cand;
                mismatch_id = probe->id;
            }
            continue;
        }
        return open_probed(std::move(probe).value(), cand, options);
    }
    if (!mismatch_path.empty()) {
        return Diag::make(diag::kPdbMismatch,
                          "'" + mismatch_path + "' is " + mismatch_id.to_string() + " but '" + pe_path + "' was linked against " +
                              ref->id.to_string() + " (GUID/age mismatch).",
                          "Use the PDB produced by the same link as the image; rebuild if the image and PDB are out of sync.");
    }
    std::string list;
    for (const std::string& t : tried) {
        list += (list.empty() ? "'" : ", '") + t + "'";
    }
    return Diag::make(diag::kPdbNotFound,
                      "No usable PDB for '" + pe_path + "' (recorded path '" + recorded + "'; tried " + list + ").",
                      "Put the PDB next to the image, or add its directory to the PDB search paths.");
}

std::size_t PdbIndex::cache_size() {
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.sweep();
    return c.slots.size();  // alive or being built: the sweep removed the rest
}

std::size_t PdbIndex::cache_slot_count() {
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    return c.slots.size();
}

void PdbIndex::cache_clear() {
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.slots.clear();
}

// ---- accessors ------------------------------------------------------------------------------------

const PdbId& PdbIndex::id() const noexcept {
    return impl_->id;
}
std::uint32_t PdbIndex::info_age() const noexcept {
    return impl_->info_age;
}
const std::string& PdbIndex::path() const noexcept {
    return impl_->path;
}
std::uint64_t PdbIndex::file_size() const noexcept {
    return impl_->file->size();
}
std::uint16_t PdbIndex::machine() const noexcept {
    return impl_->machine;
}
std::uint32_t PdbIndex::dbi_age() const noexcept {
    return impl_->dbi_age;
}
std::uint32_t PdbIndex::compiland_count() const noexcept {
    return static_cast<std::uint32_t>(impl_->compilands.size());
}
const Compiland& PdbIndex::compiland(std::uint32_t index) const {
    if (index >= impl_->compilands.size()) {
        fail_fast("PdbIndex::compiland: index out of range");
    }
    return impl_->compilands[index];
}

std::vector<std::uint32_t> PdbIndex::compilands_by_object_name(std::string_view name) const {
    std::vector<std::uint32_t> out;
    const std::string wanted = normalize_path(name);
    for (const Compiland& c : impl_->compilands) {
        if (iequals(c.object_name, name) || iequals(base_name(c.object_name), name) ||
            (is_absolute_path(name) && normalize_path(c.object_name) == wanted)) {
            out.push_back(c.index);
        }
    }
    return out;
}

std::vector<std::uint32_t> PdbIndex::compilands_by_object_stem(std::string_view stem) const {
    std::vector<std::uint32_t> out;
    std::string wanted(stem);
    for (char& ch : wanted) {
        ch = lower(ch);
    }
    for (const Compiland& c : impl_->compilands) {
        if (object_stem(c.object_name) == wanted) {
            out.push_back(c.index);
        }
    }
    return out;
}

std::vector<std::string_view> PdbIndex::compiland_files(std::uint32_t ci) const {
    std::vector<std::string_view> out;
    if (ci >= impl_->compilands.size()) {
        return out;
    }
    for (std::uint32_t k = impl_->compiland_file_start[ci]; k < impl_->compiland_file_start[ci + 1]; ++k) {
        const FileEntry& e = impl_->file_entries[impl_->compiland_files[k]];
        out.emplace_back(reinterpret_cast<const char*>(impl_->source_names) + e.raw_off, e.raw_len);
    }
    return out;
}

std::vector<std::string_view> PdbIndex::compiland_files_normalized(std::uint32_t ci) const {
    std::vector<std::string_view> out;
    if (ci >= impl_->compilands.size()) {
        return out;
    }
    for (std::uint32_t k = impl_->compiland_file_start[ci]; k < impl_->compiland_file_start[ci + 1]; ++k) {
        const FileEntry& e = impl_->file_entries[impl_->compiland_files[k]];
        out.emplace_back(impl_->file_pool.data() + e.norm_off, e.norm_len);
    }
    return out;
}

std::vector<std::uint32_t> PdbIndex::compilands_for_file(std::string_view file_path) const {
    std::vector<std::uint32_t> out;
    const std::string wanted = normalize_path(file_path);
    const Impl& im = *impl_;
    const char* pool = im.file_pool.data();
    auto norm_of = [&](std::uint32_t entry) {
        const FileEntry& e = im.file_entries[entry];
        return std::string_view(pool + e.norm_off, e.norm_len);
    };
    auto it = std::lower_bound(im.unique_files.begin(), im.unique_files.end(), wanted,
                               [&](std::uint32_t entry, const std::string& w) { return norm_of(entry) < std::string_view(w); });
    if (it == im.unique_files.end() || norm_of(*it) != wanted) {
        return out;
    }
    const std::size_t uid = static_cast<std::size_t>(it - im.unique_files.begin());
    for (std::uint32_t k = im.unique_file_start[uid]; k < im.unique_file_start[uid + 1]; ++k) {
        out.push_back(im.unique_file_compilands[k]);
    }
    return out;
}

std::size_t PdbIndex::distinct_file_count() const noexcept {
    return impl_->unique_files.size();
}

const std::vector<SectionHeader>& PdbIndex::sections() const noexcept {
    return impl_->sections;
}

std::optional<std::uint32_t> PdbIndex::rva_from_section_offset(std::uint16_t section, std::uint32_t offset) const noexcept {
    return impl_->section_rva(section, offset);
}

std::optional<SectionOffset> PdbIndex::section_offset_from_rva(std::uint32_t rva) const noexcept {
    const auto& secs = impl_->sections;
    for (std::size_t i = 0; i < secs.size(); ++i) {
        const std::uint32_t span = std::max(secs[i].virtual_size, secs[i].raw_size);
        if (rva >= secs[i].rva && rva - secs[i].rva < span) {
            SectionOffset so;
            so.section = static_cast<std::uint16_t>(i + 1);
            so.offset = rva - secs[i].rva;
            return so;
        }
    }
    return std::nullopt;
}

std::size_t PdbIndex::contribution_count() const noexcept {
    return impl_->contributions.size();
}

SectionContribution PdbIndex::contribution(std::size_t index) const {
    if (index >= impl_->contributions.size()) {
        fail_fast("PdbIndex::contribution: index out of range");
    }
    const CompactContribution& c = impl_->contributions[index];
    SectionContribution out;
    out.section = c.section;
    out.size = c.size;
    out.compiland = c.compiland;
    out.rva = c.rva;
    out.offset = c.rva - impl_->sections[static_cast<std::size_t>(c.section) - 1].rva;
    return out;
}

std::vector<SectionContribution> PdbIndex::contributions() const {
    std::vector<SectionContribution> out;
    out.reserve(impl_->contributions.size());
    for (std::size_t i = 0; i < impl_->contributions.size(); ++i) {
        out.push_back(contribution(i));
    }
    return out;
}

std::optional<std::uint32_t> PdbIndex::compiland_for_rva(std::uint32_t rva) const noexcept {
    const auto& cs = impl_->contributions;
    auto it = std::upper_bound(cs.begin(), cs.end(), rva, [](std::uint32_t v, const CompactContribution& c) { return v < c.rva; });
    if (it == cs.begin()) {
        return std::nullopt;
    }
    --it;
    if (rva - it->rva < it->size) {
        return it->compiland;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> PdbIndex::compiland_for_section_offset(std::uint16_t section, std::uint32_t offset) const noexcept {
    const auto rva = impl_->section_rva(section, offset);
    if (!rva) {
        return std::nullopt;
    }
    return compiland_for_rva(*rva);
}

Timings PdbIndex::timings() const {
    Timings t = impl_->open_timings;
    t.publics_ms = static_cast<double>(impl_->publics_ns.load(std::memory_order_relaxed)) / 1e6;
    t.module_symbols_ms = static_cast<double>(impl_->module_ns.load(std::memory_order_relaxed)) / 1e6;
    t.load_all_wall_ms = static_cast<double>(impl_->load_all_ns.load(std::memory_order_relaxed)) / 1e6;
    t.buildinfo_ms = static_cast<double>(impl_->ipi_ns.load(std::memory_order_relaxed)) / 1e6;
    return t;
}

}  // namespace cml::pdb
