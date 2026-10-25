#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cml::pdb {

inline constexpr std::uint32_t kNoRva = 0xFFFFFFFFu;
inline constexpr std::uint32_t kNoCompiland = 0xFFFFFFFFu;
inline constexpr std::uint32_t kNoSymbol = 0xFFFFFFFFu;

// Identity of a PDB / of the image built against it: the 16-byte GUID and the age.
struct PdbId {
    std::array<std::uint8_t, 16> guid{};
    std::uint32_t age = 0;

    bool operator==(const PdbId& o) const noexcept { return guid == o.guid && age == o.age; }
    bool operator!=(const PdbId& o) const noexcept { return !(*this == o); }
    bool operator<(const PdbId& o) const noexcept {
        return guid != o.guid ? guid < o.guid : age < o.age;
    }
    bool is_zero() const noexcept;
    // "8CC3833C-8700-EB0D-4C4C-44205044422E/1" (Microsoft GUID text layout: first three fields
    // little-endian, then bytes) followed by "/<age>".
    std::string to_string() const;
};

// What the PE debug directory says about its PDB (CodeView RSDS record).
struct CodeViewRef {
    PdbId id;
    std::string pdb_path;  // exactly as recorded by the linker (usually absolute)
};

enum class SymbolKind : std::uint8_t {
    GlobalProc,    // S_GPROC32 / S_GPROC32_ID
    LocalProc,     // S_LPROC32 / S_LPROC32_ID (static functions, ??__E initialisers, ...)
    GlobalData,    // S_GDATA32
    LocalData,     // S_LDATA32 (static data, including function-local statics)
    GlobalThread,  // S_GTHREAD32
    LocalThread,   // S_LTHREAD32
    Udt,           // S_UDT (no address)
    Public,        // S_PUB32 (decorated, linker-visible)
};

const char* to_string(SymbolKind kind) noexcept;

inline constexpr bool is_proc_kind(SymbolKind k) noexcept {
    return k == SymbolKind::GlobalProc || k == SymbolKind::LocalProc;
}
inline constexpr bool is_data_kind(SymbolKind k) noexcept {
    return k == SymbolKind::GlobalData || k == SymbolKind::LocalData || k == SymbolKind::GlobalThread ||
           k == SymbolKind::LocalThread;
}

// S_PUB32 flag bits (CV_PUBSYMFLAGS).
inline constexpr std::uint32_t kPublicCode = 0x1;
inline constexpr std::uint32_t kPublicFunction = 0x2;

// A symbol as returned by lookups. `name` points into storage owned by the PdbIndex that returned
// it: valid for as long as that index (shared_ptr) is alive. Names are exactly as recorded:
// publics are decorated linker names; module symbols are *display* names (see docs/wp/WP-04.md).
struct SymbolView {
    std::string_view name;
    SymbolKind kind = SymbolKind::Public;
    std::uint32_t compiland = kNoCompiland;  // owning compiland; kNoCompiland for publics / globals
    std::uint32_t index = kNoSymbol;         // position in that compiland's symbol list (compiland_symbols)
    std::uint16_t section = 0;               // 1-based section number; 0 = none
    std::uint32_t offset = 0;                // offset inside the section
    std::uint32_t rva = kNoRva;              // kNoRva when the section is unknown
    std::uint32_t size = 0;                  // procs: code length in bytes; others 0
    std::uint32_t type_index = 0;            // CodeView TPI (or IPI for *_ID procs) index
    std::uint32_t flags = 0;                 // procs: CV_PROCFLAGS; publics: kPublic* bits
    std::uint32_t parent_proc = kNoSymbol;   // function-local symbols: index of the enclosing proc
    std::string_view parent_name;            // name of that enclosing proc ("" when top level)

    bool is_function() const noexcept {
        return is_proc_kind(kind) || (kind == SymbolKind::Public && (flags & kPublicFunction) != 0);
    }
};

struct SectionHeader {
    std::array<char, 9> name{};  // NUL-terminated copy of the 8-byte name
    std::uint32_t virtual_size = 0;
    std::uint32_t rva = 0;
    std::uint32_t raw_size = 0;
    std::uint32_t characteristics = 0;
};

struct SectionOffset {
    std::uint16_t section = 0;  // 1-based
    std::uint32_t offset = 0;
};

// One DBI section contribution: bytes [offset, offset+size) of `section` came from `compiland`.
struct SectionContribution {
    std::uint16_t section = 0;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t compiland = kNoCompiland;
    std::uint32_t rva = kNoRva;
};

// S_COMPILE3.
struct CompileInfo {
    std::uint8_t language = 0;  // CV_CFL_LANG: 0 C, 1 C++, 0x10 .. see language_name()
    std::uint32_t raw_flags = 0;
    std::uint16_t machine = 0;  // CV_CPU_TYPE: 0xD0 = x64
    std::array<std::uint16_t, 4> frontend{};
    std::array<std::uint16_t, 4> backend{};
    std::string_view version;  // compiler version string

    // Decoded flag bits.
    bool edit_and_continue = false;  // /ZI
    bool no_debug_info = false;
    bool ltcg = false;  // /GL (whole-program optimisation; the object holds IL, not COFF)
    bool no_data_align = false;
    bool managed = false;
    bool security_checks = false;  // /GS
    bool hotpatch = false;         // /hotpatch
    bool cvtcil = false;
    bool msil_module = false;
    bool sdl = false;  // /sdl
    bool pgo = false;
    bool exp_module = false;

    const char* language_name() const noexcept;
};

// Optimisation level as far as it can be read from the recorded command line. S_COMPILE3 itself
// records no /O level (only /GL), so this is a heuristic over the `cmd` string (last /O option wins).
enum class OptimizationHint : std::uint8_t { Unknown, Disabled, Size, Speed, Full };

OptimizationHint optimization_hint_from_command(std::string_view command) noexcept;
const char* to_string(OptimizationHint hint) noexcept;

// Where a compiland's build environment (cwd / compiler / command ...) came from.
enum class EnvSource : std::uint8_t {
    None,       // neither S_ENVBLOCK nor a usable S_BUILDINFO
    EnvBlock,   // S_ENVBLOCK (MSVC cl, lld-link's "* Linker *")
    BuildInfo,  // S_BUILDINFO -> IPI LF_BUILDINFO (how clang-cl records it; see PdbIndex::build_env)
};

const char* to_string(EnvSource source) noexcept;

using EnvPair = std::pair<std::string_view, std::string_view>;

// One DBI module. Strings are views into storage owned by the PdbIndex.
struct Compiland {
    std::uint32_t index = 0;
    std::string_view object_name;   // module name: the .obj path, "Import:kernel32.dll", "* Linker *"
    std::string_view library_name;  // the archive it came from; empty when it is a plain object
    std::string_view objname_record;  // S_OBJNAME name (usually == object_name)
    std::uint16_t symbol_stream = 0xFFFF;  // MSF stream holding the module's symbols, 0xFFFF = none
    std::uint32_t symbol_bytes = 0;
    std::uint32_t file_count = 0;  // entries in the DBI source-info list

    bool has_compile3 = false;
    CompileInfo compile;

    // Filled at open() from the head of the module stream: EnvBlock when an S_ENVBLOCK record was
    // found, None otherwise. A compiland without S_ENVBLOCK may still describe its build through
    // S_BUILDINFO (clang-cl does this); that needs PdbIndex::build_env(), which reads the module
    // symbols and the IPI stream lazily.
    EnvSource env_source = EnvSource::None;
    // Ordered S_ENVBLOCK key/value pairs: cwd / cl / cmd / src / pdb (+ vendor extras, e.g.
    // lld-link adds "exe").
    std::vector<EnvPair> env;

    bool has_env_block() const noexcept { return env_source == EnvSource::EnvBlock; }

    std::optional<std::string_view> env_value(std::string_view key) const noexcept;
    std::optional<std::string_view> cwd() const noexcept { return env_value("cwd"); }
    std::optional<std::string_view> compiler() const noexcept { return env_value("cl"); }
    std::optional<std::string_view> command() const noexcept { return env_value("cmd"); }
    std::optional<std::string_view> source() const noexcept { return env_value("src"); }
    std::optional<std::string_view> pdb() const noexcept { return env_value("pdb"); }

    OptimizationHint optimization() const noexcept;
};

// A compiland's build environment from either source, with owned strings. Keys are the S_ENVBLOCK
// names (cwd, cl, cmd, src, pdb); a BuildInfo-sourced environment is mapped onto the same keys
// (LF_BUILDINFO args: CurrentDirectory -> cwd, BuildTool -> cl, SourceFile -> src,
// TypeServerPDB -> pdb, CommandLine -> cmd). Empty values are kept (clang records an empty pdb).
struct BuildEnv {
    EnvSource source = EnvSource::None;
    std::vector<std::pair<std::string, std::string>> pairs;

    std::optional<std::string_view> get(std::string_view key) const noexcept;
    std::optional<std::string_view> cwd() const noexcept { return get("cwd"); }
    std::optional<std::string_view> compiler() const noexcept { return get("cl"); }
    std::optional<std::string_view> command() const noexcept { return get("cmd"); }
    std::optional<std::string_view> source_file() const noexcept { return get("src"); }
    std::optional<std::string_view> pdb() const noexcept { return get("pdb"); }
    OptimizationHint optimization() const noexcept;
};

// Memory accounting for the index (see docs/wp/WP-04.md). `tracked_*` counts every allocation made
// by cml_pdb's own containers and stream copies; `mapped_*` counts bytes of PDB files currently
// mapped (resident pages are only those actually touched, but the budget counts the whole file).
struct MemoryStats {
    std::size_t tracked_bytes = 0;
    std::size_t tracked_peak_bytes = 0;
    std::size_t mapped_bytes = 0;
    std::size_t mapped_peak_bytes = 0;
    std::size_t peak_total() const noexcept { return tracked_peak_bytes + mapped_peak_bytes; }
};

MemoryStats memory_stats() noexcept;
// Sets the peaks to the current values (benchmarks call this before opening a PDB).
void reset_memory_peaks() noexcept;

// Wall-clock milliseconds spent per phase of open() and of the lazy loads.
struct Timings {
    double map_ms = 0;            // CreateFileMapping / MapViewOfFile
    double msf_ms = 0;            // super block + stream directory
    double dbi_ms = 0;            // DBI header and substream slicing
    double modules_ms = 0;        // module table + S_COMPILE3 / S_ENVBLOCK / S_OBJNAME heads
    double buildinfo_ms = 0;      // lazy: IPI scan for the S_BUILDINFO fallback
    double files_ms = 0;          // source-info lists + normalised file map
    double sections_ms = 0;       // section headers + contributions
    double open_total_ms = 0;
    double publics_ms = 0;        // lazy: symbol record stream (publics + global data)
    double module_symbols_ms = 0; // lazy: sum over compilands (CPU time across threads)
    double load_all_wall_ms = 0;  // wall time of the last load_all()
};

}  // namespace cml::pdb
