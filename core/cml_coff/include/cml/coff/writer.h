#pragma once

// COFF object writer (regular and /bigobj), WP-06.
//
// ObjectModel is an EDITABLE COPY of a parsed object (the reader's Object is a read-only view).
// write() serialises a model back into a COFF file. The pair from_object() + write() is a
// byte-identical round trip for every object the reader accepts, provided its regions do not
// overlap (CML3304 otherwise): the model keeps
//   * the raw file header and optional header (only the count / pointer fields are patched),
//   * the raw 8-byte name field of every section and symbol,
//   * every symbol record with its aux records as raw bytes, in raw-index order,
//   * the string table as raw bytes; new strings are only ever APPENDED, so every existing
//     name offset (symbols and "/nnn" section names) stays valid,
//   * the ORDER of the file regions (section data, relocation tables, line numbers, symbol table +
//     string table) and any padding bytes between them, and any bytes after the last region.
// New content goes where the editor puts it: appended symbols and strings grow the symbol/string
// table in place; regions the original file does not have (a section added to a parsed object)
// are placed after the last original region.
//
// Rules the model upholds so relocations keep working: symbols are never reordered or removed by
// this library, and a symbol's raw index (aux records counted) is its position in `symbols`
// expanded by the aux records of everything before it.
//
// Errors are cml::Diag values CML3300..CML3309 (see diag_ids.h). No exceptions.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cml/coff/coff.h"
#include "cml/coff/diag_ids.h"
#include "cml/util/result.h"

namespace cml::coff {

using ByteVector = std::vector<std::byte>;

inline constexpr std::uint64_t kNoOffset = ~std::uint64_t{0};

struct ModelRelocation {
    std::uint32_t virtual_address = 0;
    std::uint32_t symbol_index = 0;  // raw symbol-table index (aux records counted)
    std::uint16_t type = 0;          // IMAGE_REL_AMD64_*
};

struct ModelSection {
    std::string name;                // resolved name (informational; the writer emits raw_name)
    std::array<char, 8> raw_name{};  // the header's 8-byte name field verbatim ("/nnn" for long names)

    std::uint32_t virtual_size = 0;
    std::uint32_t virtual_address = 0;
    std::uint32_t characteristics = 0;

    // File data. A section "has file data" when it has a non-empty `data` and has_file_data is set;
    // then SizeOfRawData / PointerToRawData are computed. Otherwise (bss, empty sections) the two
    // stored fields below are written verbatim.
    bool has_file_data = false;
    ByteVector data;
    std::uint32_t size_of_raw_data = 0;
    std::uint32_t pointer_to_raw_data = 0;

    // Relocations: the real records only. With extended_relocations (or 65535 and more records) the
    // writer emits the LNK_NRELOC_OVFL record in front and sets the header count to 0xFFFF.
    std::vector<ModelRelocation> relocations;
    bool extended_relocations = false;
    std::array<std::uint8_t, 6> overflow_record_tail{};  // bytes 4..9 of the overflow record, kept verbatim
    std::uint32_t pointer_to_relocations = 0;            // written verbatim while there is no relocation region

    // Line numbers (obsolete; kept for losslessness). Raw 6-byte records when present.
    std::uint16_t linenumber_count = 0;
    std::uint32_t pointer_to_linenumbers = 0;  // written verbatim while linenumbers is empty
    ByteVector linenumbers;

    // Layout memory of the parsed file (kNoOffset: new). Used only to keep the file order, and the
    // padding bytes that preceded each region.
    std::uint64_t data_offset = kNoOffset;
    std::uint64_t reloc_offset = kNoOffset;
    std::uint64_t linenumber_offset = kNoOffset;
    ByteVector data_pad, reloc_pad, linenumber_pad;

    bool is_comdat() const noexcept { return (characteristics & scn::LnkComdat) != 0; }
};

struct ModelSymbol {
    std::string name;                        // resolved (informational; the writer emits raw_name)
    std::array<std::uint8_t, 8> raw_name{};  // inline name, or 4 zero bytes + string-table offset
    std::uint32_t value = 0;
    std::int32_t section_number = 0;  // 0 undefined, -1 absolute, -2 debug, n > 0 section index
    std::uint16_t type = 0;
    std::uint8_t storage_class = 0;
    ByteVector aux;  // the aux records, raw, aux_count * symbol_record_size bytes
};

class ObjectModel {
public:
    bool is_bigobj = false;
    ByteVector header;           // raw file header: 20 bytes (regular) or 56 (bigobj)
    ByteVector optional_header;  // regular objects only (normally empty)

    std::vector<ModelSection> sections;  // sections[i] is section number i + 1
    std::vector<ModelSymbol> symbols;    // primary records; aux records live in ModelSymbol::aux

    bool has_symbol_table = false;  // the parsed file had a symbol-table pointer
    std::uint64_t symbols_offset = kNoOffset;
    ByteVector symbols_pad;

    bool has_string_table = false;
    ByteVector string_table;  // the bytes AFTER the 4-byte size field

    ByteVector trailing;  // bytes after the last region

    // An empty object of the given machine (no sections, no symbols).
    static ObjectModel make_empty(Machine machine, bool bigobj = false);
    // Copies a parsed object. CML3304 when its regions overlap or a line-number table is out of
    // range (cannot be reproduced exactly).
    static Result<ObjectModel> from_object(const Object& object);

    std::size_t symbol_record_size() const noexcept { return is_bigobj ? 20u : 18u; }
    // Number of raw symbol-table records (primary + aux).
    std::uint64_t raw_symbol_count() const noexcept;

    // Appends `text` + NUL to the string table; returns its offset (counted from the start of the
    // table, i.e. including the 4-byte size field). CML3300 when the table would exceed 4 GiB.
    Result<std::uint32_t> add_string(std::string_view text);
    // The 8-byte symbol name field for `name`: inline when it fits in 8 bytes, otherwise appended to
    // the string table.
    Result<std::array<std::uint8_t, 8>> encode_symbol_name(std::string_view name);
    // Appends a symbol (aux = raw aux bytes) and returns its raw index. O(symbols): bulk code
    // should build ModelSymbol values itself and track the index.
    Result<std::uint32_t> add_symbol(std::string_view name, std::uint32_t value, std::int32_t section_number,
                                     std::uint16_t type, StorageClass storage_class, ByteVector aux = {});
    // Appends a section with file data and returns its 1-based index. Names longer than 8 bytes go
    // to the string table ("/nnn").
    Result<std::uint32_t> add_section(std::string_view name, std::uint32_t characteristics, ByteVector data,
                                      std::vector<ModelRelocation> relocations = {});
};

// Serialises a model. Validates the structure first (CML3300..CML3305).
Result<ByteVector> write(const ObjectModel& model);

// from_object() + write().
Result<ByteVector> rewrite(const Object& object);

// An IMAGE_AUX_SYMBOL section definition record (18 or 20 bytes).
ByteVector make_section_definition_aux(bool bigobj, std::uint32_t length, std::uint32_t relocation_count,
                                       std::uint32_t checksum = 0, std::uint32_t associated_section = 0,
                                       std::uint8_t selection = 0);

}  // namespace cml::coff
