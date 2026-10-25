#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cml/pdb/types.h"
#include "cml/util/result.h"

namespace cml::pdb {

// Normalises a path for comparison: backslashes, no duplicate separators, "." and ".." resolved,
// ASCII-lower-cased, "\\?\" prefix removed. If `path` is relative and `cwd` is a non-empty
// absolute path, the path is resolved against `cwd` first. A relative path with no usable cwd stays
// relative (leading ".." kept). Non-ASCII bytes are not case-folded (documented limitation).
std::string normalize_path(std::string_view path, std::string_view cwd = {});
// "X:\..." or "\\server\..." (after separator normalisation).
bool is_absolute_path(std::string_view path) noexcept;

struct OpenOptions {
    // Extra directories searched (after the recorded path and the PE's own directory) for the PDB
    // when opening from a PE. Matched by file name.
    std::vector<std::string> override_dirs;
    // Seam for symbol-server support: not implemented. When set it is only logged.
    std::string symbol_server;
    // Share the index through the process-wide (GUID, age) cache.
    bool use_cache = true;
    // Load the publics / all compiland symbol tables as part of open() instead of lazily.
    bool preload_publics = false;
    bool preload_all_symbols = false;
    // Threads for load_all(); 0 = hardware concurrency (capped at 16).
    unsigned threads = 0;
};

// An opened PDB. Immutable after open() except for lazily built symbol tables, which are built at
// most once under internal synchronisation: every const member is thread-safe. The index memory
// maps the PDB read-only; see docs/wp/WP-04.md about the PDB being locked while mapped.
class PdbIndex {
public:
    ~PdbIndex();
    PdbIndex(const PdbIndex&) = delete;
    PdbIndex& operator=(const PdbIndex&) = delete;

    // ---- Opening -------------------------------------------------------------------------
    static Result<std::shared_ptr<const PdbIndex>> open_pdb(const std::string& pdb_path,
                                                            const OpenOptions& options = {});
    // Reads the RSDS record of the PE at `pe_path`, locates the PDB (recorded path, next to the
    // PE, then override_dirs), checks GUID and age, and opens it.
    static Result<std::shared_ptr<const PdbIndex>> open_for_pe(const std::string& pe_path,
                                                               const OpenOptions& options = {});

    // ---- Identity ------------------------------------------------------------------------
    const PdbId& id() const noexcept;
    const std::string& path() const noexcept;
    std::uint64_t file_size() const noexcept;
    std::uint16_t machine() const noexcept;  // DBI machine type (0x8664 = AMD64)
    std::uint32_t dbi_age() const noexcept;

    // ---- Compilands ----------------------------------------------------------------------
    std::uint32_t compiland_count() const noexcept;
    const Compiland& compiland(std::uint32_t index) const;  // index < compiland_count()
    // Compilands whose object name, or the last path component of it, equals `name`
    // (case-insensitive, either separator).
    std::vector<std::uint32_t> compilands_by_object_name(std::string_view name) const;
    // Compilands whose object file stem (file name without directory and without the ".obj" or
    // ".cpp.obj" style suffix) equals `stem`, e.g. "logic" matches "...\logic.cpp.obj".
    std::vector<std::uint32_t> compilands_by_object_stem(std::string_view stem) const;

    // Build environment of a compiland (cwd, compiler, command, source, pdb): S_ENVBLOCK when
    // present, otherwise S_BUILDINFO resolved through the IPI stream (loads that compiland's
    // symbols). source == None when the compiland records neither.
    Result<BuildEnv> build_env(std::uint32_t ci) const;

    // ---- Files ---------------------------------------------------------------------------
    // Source files of a compiland, as recorded (before normalisation), in recorded order.
    std::vector<std::string_view> compiland_files(std::uint32_t ci) const;
    // The same files normalised (see normalize_path; relative names resolved against the
    // compiland's recorded cwd when known).
    std::vector<std::string_view> compiland_files_normalized(std::uint32_t ci) const;
    // Compilands that include `path` (any spelling; normalised internally), ascending.
    std::vector<std::uint32_t> compilands_for_file(std::string_view file_path) const;
    std::size_t distinct_file_count() const noexcept;

    // ---- Sections ------------------------------------------------------------------------
    const std::vector<SectionHeader>& sections() const noexcept;
    bool has_sections() const noexcept { return !sections().empty(); }
    std::optional<std::uint32_t> rva_from_section_offset(std::uint16_t section, std::uint32_t offset) const noexcept;
    std::optional<SectionOffset> section_offset_from_rva(std::uint32_t rva) const noexcept;
    // Section contributions sorted by rva (stored compactly; entries are materialised on demand).
    // Entries the linker marks as internal or that map to no section are dropped.
    std::size_t contribution_count() const noexcept;
    SectionContribution contribution(std::size_t index) const;  // index < contribution_count()
    std::vector<SectionContribution> contributions() const;     // copies all of them
    std::optional<std::uint32_t> compiland_for_rva(std::uint32_t rva) const noexcept;
    std::optional<std::uint32_t> compiland_for_section_offset(std::uint16_t section, std::uint32_t offset) const noexcept;

    // ---- Symbols (lazy) ------------------------------------------------------------------
    // Publics: every linker-visible symbol, decorated names. `find_public` is an exact match.
    Result<std::vector<SymbolView>> find_public(std::string_view decorated_name) const;
    // Global data (S_GDATA32 / S_GTHREAD32) by recorded display name.
    Result<std::vector<SymbolView>> find_global_data(std::string_view name) const;
    // Publics first, then global data.
    Result<std::vector<SymbolView>> find_global(std::string_view name) const;
    // Publics located at exactly `rva` (aliases possible).
    Result<std::vector<SymbolView>> publics_at_rva(std::uint32_t rva) const;
    // The public with the greatest rva <= `rva`, within the same section.
    Result<std::optional<SymbolView>> public_containing_rva(std::uint32_t rva) const;
    std::size_t public_count() const;  // loads publics; 0 on failure
    // Every public, in stream order. Copies one SymbolView per public: for tools and tests, not for
    // hot paths.
    Result<std::vector<SymbolView>> all_publics() const;

    // Per-compiland: all recorded procs / data / UDTs with that display name.
    Result<std::vector<SymbolView>> find_in_compiland(std::uint32_t ci, std::string_view name) const;
    // Every recorded symbol of a compiland in stream order (index == SymbolView::index).
    Result<std::vector<SymbolView>> compiland_symbols(std::uint32_t ci) const;
    // The proc (S_GPROC32 / S_LPROC32) whose [rva, rva+size) contains `rva`.
    Result<std::optional<SymbolView>> find_proc_by_rva(std::uint32_t rva) const;

    // ---- Loading control -----------------------------------------------------------------
    Result<void> load_publics() const;
    Result<void> load_compiland(std::uint32_t ci) const;
    // Loads every compiland's symbols using `threads` workers (0 = options.threads default).
    Result<void> load_all(unsigned threads = 0) const;

    Timings timings() const;

    // ---- Cache ---------------------------------------------------------------------------
    // Number of live entries in the process-wide cache (entries whose index is still alive).
    static std::size_t cache_size();
    // Forgets all cache entries (indexes that callers still hold stay valid).
    static void cache_clear();

    struct Impl;

private:
    explicit PdbIndex(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend struct IndexBuilder;
};

// ---- PE support (private to cml_pdb until cml_pe from WP-02 is available) --------------------
Result<CodeViewRef> read_codeview_ref(const std::string& pe_path);
Result<CodeViewRef> read_codeview_ref_from_image(const void* data, std::size_t size);

}  // namespace cml::pdb
