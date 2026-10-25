#pragma once

// Private representation of cml::pdb::PdbIndex. Not installed; only index*.cpp include it.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cml/pdb/index.h"
#include "detail/counting.h"
#include "detail/mapped_file.h"
#include "detail/msf.h"
#include "detail/name_table.h"

namespace cml::pdb {

namespace detail {

// One recorded module symbol. 28 bytes. Names live in the owning ModuleSymbols' pool; the RVA is
// derived from (section, offset) through the section headers when needed.
struct SymRec {
    std::uint32_t name_off = 0;
    std::uint32_t name_len = 0;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t type_index = 0;
    std::uint32_t parent = kNoSymbol;
    std::uint16_t section = 0;
    std::uint8_t kind = 0;   // SymbolKind
    std::uint8_t flags = 0;  // procs: CV_PROCFLAGS
};
static_assert(sizeof(SymRec) == 28, "SymRec layout");

// A public (S_PUB32) or global data (S_GDATA32 / S_GTHREAD32) record of the symbol record stream.
// 20 bytes. Names point into the retained stream.
struct PubRec {
    std::uint32_t name_off = 0;
    std::uint32_t name_len = 0;
    std::uint32_t offset = 0;
    std::uint32_t type_index = 0;  // global data only
    std::uint16_t section = 0;
    std::uint8_t kind = 0;   // SymbolKind
    std::uint8_t flags = 0;  // publics: CV_PUBSYMFLAGS
};
static_assert(sizeof(PubRec) == 20, "PubRec layout");

struct RvaIndex {
    std::uint32_t rva;
    std::uint32_t index;
};

// Lazily loaded symbols of one compiland. The lookup structures (by name, procs by rva) are built on
// first use, so loading everything does not pay for indexes nobody queries.
struct ModuleSymbols {
    Vec<SymRec> syms;
    Vec<char> pool;
    std::uint32_t buildinfo_id = 0;  // S_BUILDINFO item id (IPI index); 0 = none

    mutable std::mutex lazy_mutex;
    mutable std::atomic<bool> names_ready{false};
    mutable std::atomic<bool> procs_ready{false};
    mutable NameIndex names;
    mutable Vec<RvaIndex> procs_by_rva;  // procs with a known rva, sorted by rva

    void ensure_names() const;
    void ensure_procs(const std::vector<SectionHeader>& sections) const;
};

// Lazily loaded publics and global data from the symbol record stream.
struct GlobalSymbols {
    StreamData stream;  // names point into this
    Vec<PubRec> pubs;
    Vec<PubRec> globs;

    mutable std::mutex lazy_mutex;
    mutable std::atomic<bool> pub_names_ready{false};
    mutable std::atomic<bool> glob_names_ready{false};
    mutable std::atomic<bool> pub_rva_ready{false};
    mutable NameIndex pub_names;
    mutable NameIndex glob_names;
    mutable Vec<RvaIndex> pubs_by_rva;

    void ensure_pub_names() const;
    void ensure_glob_names() const;
    void ensure_pub_rva(const std::vector<SectionHeader>& sections) const;
};

// The IPI stream (stream 4) indexed by record, for LF_BUILDINFO / LF_STRING_ID lookups.
struct IpiTable {
    StreamData data;
    std::uint64_t records_begin = 0;  // offset of the first record in `data`
    std::uint64_t records_end = 0;
    std::uint32_t type_begin = 0;
    Vec<std::uint32_t> offsets;  // per record, relative to records_begin
};

struct FileEntry {
    std::uint32_t raw_off = 0;  // into the DBI source-info names block
    std::uint32_t raw_len = 0;
    std::uint32_t norm_off = 0;  // into file_pool
    std::uint32_t norm_len = 0;
};

// Section contribution, compact (the section offset is rva - section.rva).
struct CompactContribution {
    std::uint32_t rva = 0;
    std::uint32_t size = 0;
    std::uint32_t compiland = 0;
    std::uint16_t section = 0;
    std::uint16_t pad = 0;
};
static_assert(sizeof(CompactContribution) == 16, "CompactContribution layout");

enum class LazyState : std::uint8_t { NotLoaded = 0, Ready = 1, Failed = 2 };

}  // namespace detail

struct PdbIndex::Impl {
    OpenOptions options;
    std::shared_ptr<const detail::MappedFile> file;
    std::unique_ptr<detail::Msf> msf;
    PdbId id;
    std::string path;
    std::uint16_t machine = 0;
    std::uint32_t dbi_age = 0;
    std::uint16_t symrec_stream = 0xFFFF;

    // The DBI stream is only held while open() parses it; the source-info names block is the one
    // part that outlives it (a view into the mapping, or into names_copy when the stream was copied).
    detail::StreamData dbi;
    detail::Vec<char> names_copy;
    const std::uint8_t* source_names = nullptr;
    std::size_t source_names_len = 0;

    // Compilands. Compiland::object_name etc. are views into `text`.
    detail::Vec<char> text;
    std::vector<Compiland> compilands;

    // Files: entries are shared between compilands (deduplicated by raw offset when absolute).
    detail::Vec<detail::FileEntry> file_entries;
    detail::Vec<char> file_pool;
    detail::Vec<std::uint32_t> compiland_file_start;  // per compiland + 1: range in compiland_files
    detail::Vec<std::uint32_t> compiland_files;       // file entry ids
    detail::Vec<std::uint32_t> unique_files;          // file entry id of one representative per distinct file, sorted by normalised path
    detail::Vec<std::uint32_t> unique_file_start;     // CSR into unique_file_compilands, size unique_files+1
    detail::Vec<std::uint32_t> unique_file_compilands;

    std::vector<SectionHeader> sections;
    detail::Vec<detail::CompactContribution> contributions;  // sorted by rva

    // ---- lazy state ----
    mutable std::mutex mutex;
    mutable std::atomic<std::uint8_t> publics_state{0};
    mutable std::optional<Diag> publics_error;
    mutable std::unique_ptr<detail::GlobalSymbols> globals;

    mutable std::atomic<std::uint8_t> ipi_state{0};
    mutable std::optional<Diag> ipi_error;
    mutable std::unique_ptr<detail::IpiTable> ipi;
    mutable std::atomic<std::uint64_t> ipi_ns{0};

    mutable std::unique_ptr<std::atomic<std::uint8_t>[]> module_state;
    mutable std::vector<std::unique_ptr<detail::ModuleSymbols>> module_symbols;
    mutable std::vector<std::optional<Diag>> module_errors;

    // Timings (nanoseconds) for phases that run after open().
    Timings open_timings;
    mutable std::atomic<std::uint64_t> publics_ns{0};
    mutable std::atomic<std::uint64_t> module_ns{0};
    mutable std::atomic<std::uint64_t> load_all_ns{0};

    // ---- helpers shared by index*.cpp ----
    std::optional<std::uint32_t> section_rva(std::uint16_t section, std::uint32_t offset) const noexcept;
    std::uint32_t rva_or_none(std::uint16_t section, std::uint32_t offset) const noexcept {
        const auto r = section_rva(section, offset);
        return r ? *r : kNoRva;
    }
    Result<void> ensure_publics() const;
    Result<void> ensure_compiland(std::uint32_t index) const;
    const detail::ModuleSymbols* module(std::uint32_t index) const noexcept {
        return module_symbols[index].get();
    }
};

// Result of locating a PDB's identity without building the index.
struct PdbProbe {
    std::shared_ptr<const detail::MappedFile> file;
    std::unique_ptr<detail::Msf> msf;
    PdbId id;
    double map_ms = 0;
    double msf_ms = 0;
};

Result<PdbProbe> probe_pdb(const std::string& path);

// Implemented in index_modules.cpp / index_symbols.cpp.
Result<void> build_index(PdbIndex::Impl& impl);

namespace detail {
Result<std::unique_ptr<ModuleSymbols>> parse_module_symbols(const PdbIndex::Impl& impl, std::uint32_t compiland);
Result<std::unique_ptr<GlobalSymbols>> parse_global_symbols(const PdbIndex::Impl& impl);
}  // namespace detail

}  // namespace cml::pdb
