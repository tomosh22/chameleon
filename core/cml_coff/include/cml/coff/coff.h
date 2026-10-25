#pragma once

// COFF object-file reader (regular and /bigobj), WP-02.
//
// Design rules (the WP-06 writer relies on them):
//   * Every type here is a non-owning VIEW over caller-owned bytes. Names, section data, relocation
//     tables and aux records are spans / string_views into the input; the input must outlive them.
//   * Symbol "index" is always the raw symbol-table index, aux records included. A primary symbol
//     with N aux records occupies indices [index, index + N]; symbol_at() returns nullptr for an
//     index that falls on an aux slot.
//   * Nothing is normalised away: raw header fields, the 8-byte name fields, raw relocation tables
//     (including the IMAGE_SCN_LNK_NRELOC_OVFL record) and the string table are all reachable.
//   * Malformed input produces a cml::Diag (CML1200..CML1299), never UB. Every offset + size sum is
//     computed in 64 bits and checked against the input size before any byte is read.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cml/util/result.h"

namespace cml::coff {

using ByteSpan = std::span<const std::byte>;

// ---------------------------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------------------------

enum class Machine : std::uint16_t {
    Unknown = 0x0000,
    I386 = 0x014C,
    Arm = 0x01C0,
    ArmNt = 0x01C4,
    Amd64 = 0x8664,
    Arm64 = 0xAA64,
    Arm64Ec = 0xA641,
    Arm64X = 0xA64E,
};

// IMAGE_SCN_* section characteristics.
namespace scn {
inline constexpr std::uint32_t CntCode = 0x00000020;
inline constexpr std::uint32_t CntInitializedData = 0x00000040;
inline constexpr std::uint32_t CntUninitializedData = 0x00000080;
inline constexpr std::uint32_t LnkInfo = 0x00000200;
inline constexpr std::uint32_t LnkRemove = 0x00000800;
inline constexpr std::uint32_t LnkComdat = 0x00001000;
inline constexpr std::uint32_t Gprel = 0x00008000;
inline constexpr std::uint32_t AlignMask = 0x00F00000;
inline constexpr std::uint32_t AlignShift = 20;
inline constexpr std::uint32_t LnkNrelocOvfl = 0x01000000;
inline constexpr std::uint32_t MemDiscardable = 0x02000000;
inline constexpr std::uint32_t MemNotCached = 0x04000000;
inline constexpr std::uint32_t MemNotPaged = 0x08000000;
inline constexpr std::uint32_t MemShared = 0x10000000;
inline constexpr std::uint32_t MemExecute = 0x20000000;
inline constexpr std::uint32_t MemRead = 0x40000000;
inline constexpr std::uint32_t MemWrite = 0x80000000;
}  // namespace scn

// Symbol section numbers (shared by the 16-bit and 32-bit forms once sign-extended).
inline constexpr std::int32_t kSectionUndefined = 0;
inline constexpr std::int32_t kSectionAbsolute = -1;
inline constexpr std::int32_t kSectionDebug = -2;

enum class StorageClass : std::uint8_t {
    EndOfFunction = 0xFF,
    Null = 0,
    Automatic = 1,
    External = 2,
    Static = 3,
    Register = 4,
    ExternalDef = 5,
    Label = 6,
    UndefinedLabel = 7,
    MemberOfStruct = 8,
    Argument = 9,
    StructTag = 10,
    MemberOfUnion = 11,
    UnionTag = 12,
    TypeDefinition = 13,
    UndefinedStatic = 14,
    EnumTag = 15,
    MemberOfEnum = 16,
    RegisterParam = 17,
    BitField = 18,
    Block = 100,
    Function = 101,
    EndOfStruct = 102,
    File = 103,
    Section = 104,
    WeakExternal = 105,
    ClrToken = 107,
};

// IMAGE_COMDAT_SELECT_*. 0 means "not a COMDAT" (section-definition aux of an ordinary section).
enum class ComdatSelection : std::uint8_t {
    None = 0,
    NoDuplicates = 1,
    Any = 2,
    SameSize = 3,
    ExactMatch = 4,
    Associative = 5,
    Largest = 6,
};

// IMAGE_WEAK_EXTERN_*.
enum class WeakCharacteristics : std::uint32_t {
    SearchNoLibrary = 1,
    SearchLibrary = 2,
    SearchAlias = 3,
    AntiDependency = 4,
};

// IMAGE_REL_AMD64_*. Only meaningful when the object's machine is Amd64; the raw 16-bit value is
// always preserved in Relocation::raw_type.
enum class RelocType : std::uint16_t {
    Absolute = 0x0000,
    Addr64 = 0x0001,
    Addr32 = 0x0002,
    Addr32Nb = 0x0003,
    Rel32 = 0x0004,
    Rel32_1 = 0x0005,
    Rel32_2 = 0x0006,
    Rel32_3 = 0x0007,
    Rel32_4 = 0x0008,
    Rel32_5 = 0x0009,
    Section = 0x000A,
    SecRel = 0x000B,
    SecRel7 = 0x000C,
    Token = 0x000D,
    SRel32 = 0x000E,
    Pair = 0x000F,
    SSpan32 = 0x0010,
};

// "IMAGE_REL_AMD64_REL32"; "IMAGE_REL_AMD64_UNKNOWN" for values outside the enumeration.
std::string_view reloc_type_name(RelocType type) noexcept;
// True for REL32, REL32_1..REL32_5 (the RIP-relative family).
bool is_rel32_family(RelocType type) noexcept;

// ---------------------------------------------------------------------------------------------
// Parsed structures
// ---------------------------------------------------------------------------------------------

struct ObjectHeader {
    bool is_bigobj = false;
    std::uint16_t machine = 0;
    std::uint32_t time_date_stamp = 0;
    std::uint32_t section_count = 0;
    std::uint32_t symbol_table_offset = 0;
    std::uint32_t symbol_count = 0;  // raw count, aux records included

    // Regular header only.
    std::uint16_t optional_header_size = 0;
    std::uint16_t characteristics = 0;

    // Bigobj (ANON_OBJECT_HEADER_BIGOBJ) only.
    std::uint16_t bigobj_version = 0;
    std::uint32_t bigobj_flags = 0;
    std::uint32_t bigobj_meta_data_size = 0;
    std::uint32_t bigobj_meta_data_offset = 0;
    std::array<std::uint8_t, 16> bigobj_class_id{};

    // Derived layout facts.
    std::size_t header_size = 0;           // 20 or 56
    std::size_t section_table_offset = 0;  // header_size + optional_header_size
    std::size_t symbol_record_size = 0;    // 18 or 20 (primary and aux records alike)
};

struct Section {
    std::uint32_t index = 0;  // 1-based, as used by symbol section numbers
    std::string_view name;    // resolved ("/nnn" and "//base64" long names go through the string table)
    std::array<char, 8> raw_name{};

    std::uint32_t virtual_size = 0;
    std::uint32_t virtual_address = 0;
    std::uint32_t size_of_raw_data = 0;
    std::uint32_t pointer_to_raw_data = 0;
    std::uint32_t pointer_to_relocations = 0;
    std::uint32_t pointer_to_linenumbers = 0;
    std::uint16_t raw_relocation_count = 0;  // as stored in the header (0xFFFF when extended)
    std::uint16_t linenumber_count = 0;
    std::uint32_t characteristics = 0;

    bool has_extended_relocations = false;  // LNK_NRELOC_OVFL set and raw count == 0xFFFF
    std::uint32_t relocation_count = 0;     // effective count of real relocation records

    ByteSpan header_bytes;      // the 40-byte header
    ByteSpan data;              // empty for sections without file data (e.g. .bss)
    ByteSpan relocation_table;  // everything on disk, including the overflow record if present
    ByteSpan relocation_bytes;  // relocation_count * 10 bytes of real records

    bool is_comdat() const noexcept { return (characteristics & scn::LnkComdat) != 0; }
    bool is_code() const noexcept { return (characteristics & scn::CntCode) != 0; }
    // Alignment in bytes from the IMAGE_SCN_ALIGN_* bits; 0 when the object does not specify one.
    std::uint32_t alignment() const noexcept {
        const std::uint32_t n = (characteristics & scn::AlignMask) >> scn::AlignShift;
        return n == 0 ? 0u : (1u << (n - 1u));
    }
};

struct Symbol {
    std::uint32_t index = 0;  // raw symbol-table index
    std::string_view name;
    std::array<std::uint8_t, 8> raw_name{};  // the 8-byte name field verbatim
    bool name_in_string_table = false;
    std::uint32_t name_offset = 0;  // string-table offset when name_in_string_table

    std::uint32_t value = 0;
    // Regular objects: 16-bit field, 0xFFFF = -1 (absolute), 0xFFFE = -2 (debug), everything else
    // unsigned (toolchains use 1..65279). Bigobj: a plain signed 32-bit value.
    std::int32_t section_number = 0;
    std::uint16_t type = 0;
    StorageClass storage_class = StorageClass::Null;
    std::uint8_t aux_count = 0;

    ByteSpan record;  // this primary record
    ByteSpan aux;     // aux_count records, contiguous

    bool is_undefined() const noexcept { return section_number == kSectionUndefined && value == 0; }
    bool is_common() const noexcept { return section_number == kSectionUndefined && value != 0 &&
                                              storage_class == StorageClass::External; }
    bool is_absolute() const noexcept { return section_number == kSectionAbsolute; }
    // DTYPE_FUNCTION (0x20) in the derived-type nibble; this is how cl and clang-cl mark functions.
    bool is_function() const noexcept { return ((type >> 4) & 0xFu) == 2u; }
    bool is_external() const noexcept { return storage_class == StorageClass::External; }
    bool is_weak_external() const noexcept { return storage_class == StorageClass::WeakExternal; }
    bool is_static() const noexcept { return storage_class == StorageClass::Static; }
};

struct SectionDefinitionAux {
    std::uint32_t length = 0;
    std::uint32_t relocation_count = 0;  // 16-bit field in the aux record
    std::uint32_t linenumber_count = 0;
    std::uint32_t checksum = 0;
    std::uint32_t associated_section = 0;  // "Number": 1-based section index for Associative (32-bit in bigobj)
    std::uint8_t selection_raw = 0;
    ComdatSelection selection = ComdatSelection::None;
};

struct FunctionDefinitionAux {
    std::uint32_t tag_index = 0;
    std::uint32_t total_size = 0;
    std::uint32_t pointer_to_linenumber = 0;
    std::uint32_t pointer_to_next_function = 0;
};

struct WeakExternalAux {
    std::uint32_t tag_index = 0;  // raw symbol index of the default (fallback) symbol
    std::uint32_t characteristics = 0;
};

struct Relocation {
    std::uint32_t virtual_address = 0;  // offset of the fixup within the section
    std::uint32_t symbol_index = 0;     // raw symbol-table index
    std::uint16_t raw_type = 0;
    RelocType type = RelocType::Absolute;
};

struct ResolvedRelocation {
    Relocation reloc;
    const Symbol* target = nullptr;  // never null in a successful resolve_relocations()
};

struct ComdatInfo {
    ComdatSelection selection = ComdatSelection::None;
    const Symbol* section_symbol = nullptr;  // the static symbol carrying the section-definition aux
    const Symbol* leader = nullptr;          // COMDAT symbol; null for Associative sections
    std::uint32_t associated_section = 0;    // parent section index (Associative only)
};

// ---------------------------------------------------------------------------------------------
// Linker directives (.drectve)
// ---------------------------------------------------------------------------------------------

enum class DirectiveKind {
    Other,
    DefaultLib,
    NoDefaultLib,
    Export,
    Include,
    FailIfMismatch,
    AlternateName,
    Merge,
    Section,
    Entry,
    Subsystem,
};

struct Directive {
    DirectiveKind kind = DirectiveKind::Other;
    std::string name;   // option name without the leading '/' or '-', lower-cased ("alternatename")
    std::string value;  // text after the first ':' with one pair of enclosing quotes removed
    std::string text;   // the token exactly as it appears in .drectve
    // Filled when `value` contains '=' (AlternateName, FailIfMismatch, Merge): the two halves.
    bool has_pair = false;
    std::string lhs;
    std::string rhs;
};

// Tokenises the contents of a .drectve section (quotes, optional UTF-8 BOM, any whitespace).
std::vector<Directive> parse_directives(std::string_view text);

// ---------------------------------------------------------------------------------------------
// Object
// ---------------------------------------------------------------------------------------------

class Object {
public:
    // Validates the whole container structure (headers, section table, symbol table, string table,
    // relocation table ranges, symbol name offsets, symbol section numbers). On success all
    // accessors below are bounds-safe.
    static Result<Object> parse(ByteSpan bytes);

    ByteSpan bytes() const noexcept { return bytes_; }
    const ObjectHeader& header() const noexcept { return header_; }
    bool is_bigobj() const noexcept { return header_.is_bigobj; }
    Machine machine() const noexcept { return static_cast<Machine>(header_.machine); }

    // Raw regions, for byte-identity round trips (WP-06).
    ByteSpan header_bytes() const noexcept { return header_bytes_; }
    ByteSpan section_table_bytes() const noexcept { return section_table_bytes_; }
    ByteSpan symbol_table_bytes() const noexcept { return symbol_table_bytes_; }
    ByteSpan string_table_bytes() const noexcept { return string_table_bytes_; }  // empty if absent

    // ---- sections ----
    std::span<const Section> sections() const noexcept { return sections_; }
    const Section* section(std::uint32_t index) const noexcept;  // 1-based; null if out of range
    const Section* find_section(std::string_view name) const noexcept;  // first match
    // Section a symbol is defined in; null for undefined/absolute/debug symbols.
    const Section* section_of(const Symbol& symbol) const noexcept;

    // ---- symbols ----
    std::span<const Symbol> symbols() const noexcept { return symbols_; }  // primary records only
    const Symbol* symbol_at(std::uint32_t raw_index) const noexcept;      // null for aux slots / out of range
    // First symbol with this name. External and weak-external symbols win over static ones with
    // the same name (e.g. a section symbol ".text").
    const Symbol* find_symbol(std::string_view name) const noexcept;

    // Typed aux decoding. Each returns nullopt when the symbol is not of that kind.
    std::optional<SectionDefinitionAux> section_definition(const Symbol& symbol) const noexcept;
    std::optional<FunctionDefinitionAux> function_definition(const Symbol& symbol) const noexcept;
    std::optional<WeakExternalAux> weak_external(const Symbol& symbol) const noexcept;
    std::optional<std::string_view> file_name(const Symbol& symbol) const noexcept;  // IMAGE_SYM_CLASS_FILE aux
    // The fallback ("default") symbol of a weak external; null when not weak or the tag is invalid.
    const Symbol* weak_external_default(const Symbol& symbol) const noexcept;

    // ---- relocations ----
    std::vector<Relocation> relocations(const Section& sec) const;
    // Like relocations() but resolves every target. CML1214/CML1215 when a symbol index is out of
    // range or lands on an aux record.
    Result<std::vector<ResolvedRelocation>> resolve_relocations(const Section& sec) const;

    // ---- COMDAT ----
    // nullopt for sections that are not COMDAT or lack a section-definition aux.
    std::optional<ComdatInfo> comdat(const Section& sec) const noexcept;
    // Sections that are Associative children of `parent`, in section order.
    std::vector<const Section*> associative_children(const Section& parent) const;

    // ---- directives ----
    // All .drectve sections concatenated in section order.
    std::vector<Directive> directives() const;

private:
    Object() = default;

    ByteSpan bytes_;
    ObjectHeader header_;
    ByteSpan header_bytes_;
    ByteSpan section_table_bytes_;
    ByteSpan symbol_table_bytes_;
    ByteSpan string_table_bytes_;
    std::vector<Section> sections_;
    std::vector<Symbol> symbols_;
    std::vector<std::uint32_t> slot_;  // raw index -> position in symbols_, or kNoSlot
    std::vector<std::uint32_t> section_def_pos_;  // per section (1-based), position of its definition symbol
    std::vector<std::uint32_t> leader_pos_;       // per section (1-based), position of first non-definition symbol
    std::unordered_map<std::string_view, std::uint32_t> by_name_;

    static constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;
};

// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------

// Reads a whole file. CML1200 on failure.
Result<std::vector<std::byte>> read_file(const std::string& path);

// An Object together with the bytes it views (the bytes live on the heap, so moving is safe).
struct OwnedObject {
    std::vector<std::byte> bytes;
    Object object;
};
Result<OwnedObject> load_object_file(const std::string& path);

}  // namespace cml::coff
