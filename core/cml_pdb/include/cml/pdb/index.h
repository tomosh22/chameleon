#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cml/pdb/type_records.h"
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
    // GUID from the info stream; age from the DBI stream (what debuggers match against the image's
    // RSDS age). An info-stream age that differs is logged at debug level and exposed as info_age().
    const PdbId& id() const noexcept;
    std::uint32_t info_age() const noexcept;
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
    // The proc (S_GPROC32 / S_LPROC32) whose [rva, rva+size) contains `rva`. The compiland that
    // contributed `rva` (section contributions) is the only one loaded; a miss there is a miss. Only
    // when the PDB has no section contributions at all are the other compilands searched, and
    // compilands whose symbols are corrupt are then skipped (see compiland_error()). Corrupt owning
    // compiland: nullopt, not an error.
    Result<std::optional<SymbolView>> find_proc_by_rva(std::uint32_t rva) const;

    // WP-23: the parameters of `proc` (a proc symbol of this index, as returned by find_proc_by_rva / compiland_symbols) with the
    // locations the compiler recorded for them (S_FRAMEPROC, S_LOCAL + S_DEFRANGE_*, S_REGREL32) and their types resolved
    // through the TPI/IPI streams. Loads the compiland's symbols and the type streams. A function whose type cannot be read
    // still returns a ProcInfo (params_known == false); an error only when the symbol records are unreadable.
    Result<ProcInfo> proc_info(const SymbolView& proc) const;
    // WP-46 (F-41.2 review): for every outermost procedure of a compiland, the types of its own local variables (S_LOCAL, parameters left out;
    // S_REGREL32 and S_BPREL32 as cl writes them at /Od), at any block depth but not inside an inline site (an inlined
    // call's locals are the inlinee's). One walk of the module stream; nothing is cached. CML1413-style errors when the
    // stream cannot be read.
    struct LocalTypeUse {
        std::uint32_t proc_rva = kNoRva;  // the procedure's start
        std::uint32_t type_index = 0;     // TPI index of the local's type
    };
    Result<std::vector<LocalTypeUse>> compiland_local_types(std::uint32_t ci) const;

    // WP-51 (R-54): the functions `proc` (a proc symbol of this index) inlined: every S_INLINESITE / S_INLINESITE2 between
    // its record and its S_END, at any depth, with the inlinee each names resolved through the IPI stream (LF_FUNC_ID /
    // LF_MFUNC_ID) and, for a member function, its class through the TPI. clang records the function-local statics of
    // inlined code as plain records of the outermost procedure, so these sites are the only trace of whose code a record
    // there may be. Walks the compiland's module stream up to the end of the procedure; nothing is cached. An inline site
    // whose inlinee cannot be read is listed with `readable == false` (never left out). CML1413 when the stream is
    // unreadable or the procedure's record is not found.
    struct Inlinee {
        std::uint32_t id = 0;           // the IPI index the inline site names
        bool readable = false;          // false: `why`
        bool member = false;            // LF_MFUNC_ID (else LF_FUNC_ID)
        std::string name;               // as recorded: "operator()", "__invoke", "RTTI", "b_helper"
        std::string class_name;         // member: the class's name ("o2_step::<lambda_0>")
        std::string class_unique;       // member: its unique name without the unit's discriminator ("" when it has none)
        bool signature_known = false;   // the canonical signature of the inlinee's type (type_records.h)
        std::string signature;
        std::uint32_t sites = 0;        // inline sites of the procedure that name this inlinee
        std::uint32_t depth = 0;        // the smallest nesting depth of those sites (1: directly in the procedure)
        std::string why;
    };
    struct ProcInlinees {
        std::uint32_t sites = 0;           // inline-site records of the procedure
        std::vector<Inlinee> inlinees;     // one per distinct id, in the order of first appearance
        bool complete = true;              // false: more than kMaxProcInlinees distinct inlinees (the rest are not listed)
    };
    static constexpr std::size_t kMaxProcInlinees = 4096;
    Result<ProcInlinees> proc_inlinees(const SymbolView& proc) const;
    // WP-51 (review): the same for several procedures of ONE compiland in one walk of its module stream (each inlinee id is
    // resolved once for all of them); element i belongs to procs[i]. A procedure nested in another (an S_LPROC32 inside a
    // procedure's scope) is a scope of its own: its inline sites are its own, not the enclosing procedure's. CML1417 when
    // the procedures are not of one compiland of this index, CML1413 when one is not found or has no end record.
    Result<std::vector<ProcInlinees>> procs_inlinees(const std::vector<SymbolView>& procs) const;

    // ---- Types (WP-37, lazy) -------------------------------------------------------------
    // The type records of this PDB: find_type reads the TPI stream, find_id the IPI stream. Opening an index reads no
    // type record; the first lookup reads the stream header and the type-index offset table of the hash stream, and
    // every lookup only the chunk of records around the index it asks for (cml/pdb/type_records.h).
    const TypeSource& types() const;
    // WP-37 (ADR-0042, amended): the data records declared inside `proc` (a proc symbol of this index), in the order of
    // the module stream, with the place of each in the procedure (ProcStatic::block_path). They are the symbols of
    // compiland_symbols() whose parent_proc is `proc.index`, in the same order. Walks the compiland's module stream up
    // to the end of the procedure; nothing is cached. `complete` turns false when the procedure has more than
    // kMaxProcStatics of them. CML1413 when the stream is unreadable or the procedure's record is not found.
    Result<std::vector<ProcStatic>> proc_statics(const SymbolView& proc, bool* complete = nullptr) const;

    // The canonical signature of a proc symbol of this index (compiland_symbols / find_proc_by_rva). `known` is false
    // with a CML14xx id and a reason when the type streams or the records it needs cannot be read.
    FunctionSignature proc_signature(const SymbolView& proc) const;
    // What has been read of the TPI (`tpi` true) or IPI stream so far. `opened` is false before the first lookup.
    TypeStreamStats type_stream_stats(bool tpi) const;
    // WP-46 (R-54): every closure class (a class or struct whose unique name starts with ".?AV<lambda_" / ".?AU<lambda_")
    // of the TPI stream, once each (forward references and definitions are one class), in index order. A lambda that the
    // compiler inlined into every caller has no procedure, but its class stays in the TPI. The first call walks the whole
    // TPI stream (nothing else does); the result is kept for the life of the index. CML1430 when the stream cannot be read.
    Result<const std::vector<ClosureClass>*> closure_classes() const;
    // WP-46 (review): the size in bytes of a type of the TPI: simple types, pointers, modifiers, arrays, enums, and classes,
    // structs and unions; a forward reference is resolved through its exact unique name (definitions of one name that disagree
    // on the size: unknown; the first call that needs one walks the
    // TPI once and keeps a map of the definitions' sizes). nullopt when the size cannot be known.
    std::optional<std::uint64_t> type_size(std::uint32_t index) const;

    // ---- Loading control -----------------------------------------------------------------
    Result<void> load_publics() const;
    Result<void> load_compiland(std::uint32_t ci) const;
    // True when the compiland's symbols are loaded (does not load them).
    bool compiland_loaded(std::uint32_t ci) const noexcept;
    // The error remembered for a compiland whose symbols failed to load, if any.
    std::optional<Diag> compiland_error(std::uint32_t ci) const;
    // Loads every compiland's symbols using `threads` workers (0 = options.threads default). Every
    // compiland is attempted even after a failure; the first error is returned and each failing
    // compiland keeps its own error (compiland_error()).
    Result<void> load_all(unsigned threads = 0) const;

    Timings timings() const;

    // ---- Cache ---------------------------------------------------------------------------
    // Number of live entries in the process-wide cache (entries whose index is still alive).
    static std::size_t cache_size();
    // All entries, including expired ones not yet swept (they are swept on the next open or cache_size).
    static std::size_t cache_slot_count();
    // Forgets all cache entries (indexes that callers still hold stay valid).
    static void cache_clear();

    struct Impl;

private:
    explicit PdbIndex(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend struct IndexBuilder;
};

// ---- PE support: the CodeView (RSDS) record of an image, read with cml_pe (FU-05, WP-18) -------
Result<CodeViewRef> read_codeview_ref(const std::string& pe_path);
Result<CodeViewRef> read_codeview_ref_from_image(const void* data, std::size_t size);

}  // namespace cml::pdb
