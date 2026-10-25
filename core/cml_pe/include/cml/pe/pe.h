#pragma once

// PE32+ (64-bit) image reader, WP-02.
//
//   * Non-owning view over caller-owned bytes, in one of two layouts:
//       ImageKind::OnDisk    the file as stored; RVAs are mapped through the section table
//       ImageKind::InMemory  a mapped image (SizeOfImage bytes starting at the image base);
//                            RVA == offset
//   * PE32 (32-bit) images are rejected with CML1303.
//   * Every failure is a cml::Diag (CML1300..CML1399). All offset + size sums are 64-bit and
//     checked against the available bytes before any read, so truncated or hostile input is safe.
//   * string_views in the result types point into the image bytes and are valid only as long as
//     those bytes are (for an in-memory image: as long as the module stays mapped).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cml/util/result.h"

namespace cml::pe {

using ByteSpan = std::span<const std::byte>;

enum class ImageKind { OnDisk, InMemory };

// Data directory indices (IMAGE_DIRECTORY_ENTRY_*).
namespace dir {
inline constexpr std::size_t Export = 0;
inline constexpr std::size_t Import = 1;
inline constexpr std::size_t Resource = 2;
inline constexpr std::size_t Exception = 3;
inline constexpr std::size_t Security = 4;
inline constexpr std::size_t BaseReloc = 5;
inline constexpr std::size_t Debug = 6;
inline constexpr std::size_t Architecture = 7;
inline constexpr std::size_t GlobalPtr = 8;
inline constexpr std::size_t Tls = 9;
inline constexpr std::size_t LoadConfig = 10;
inline constexpr std::size_t BoundImport = 11;
inline constexpr std::size_t Iat = 12;
inline constexpr std::size_t DelayImport = 13;
inline constexpr std::size_t ComDescriptor = 14;
inline constexpr std::size_t Count = 16;
}  // namespace dir

// IMAGE_DLLCHARACTERISTICS_*.
namespace dllchar {
inline constexpr std::uint16_t HighEntropyVa = 0x0020;
inline constexpr std::uint16_t DynamicBase = 0x0040;
inline constexpr std::uint16_t ForceIntegrity = 0x0080;
inline constexpr std::uint16_t NxCompat = 0x0100;
inline constexpr std::uint16_t NoSeh = 0x0400;
inline constexpr std::uint16_t GuardCf = 0x4000;
}  // namespace dllchar

// IMAGE_GUARD_* flags in the load config directory.
namespace guard {
inline constexpr std::uint32_t CfInstrumented = 0x00000100;
inline constexpr std::uint32_t CfwInstrumented = 0x00000200;
inline constexpr std::uint32_t CfFunctionTablePresent = 0x00000400;
inline constexpr std::uint32_t SecurityCookieUnused = 0x00000800;
inline constexpr std::uint32_t ProtectDelayloadIat = 0x00001000;
inline constexpr std::uint32_t CfExportSuppressionInfoPresent = 0x00004000;
inline constexpr std::uint32_t CfEnableExportSuppression = 0x00008000;
inline constexpr std::uint32_t CfLongjumpTablePresent = 0x00010000;
}  // namespace guard

inline constexpr std::uint32_t kDebugTypeCodeView = 2;

struct DataDirectory {
    std::uint32_t rva = 0;
    std::uint32_t size = 0;
};

struct FileHeader {
    std::uint16_t machine = 0;
    std::uint16_t number_of_sections = 0;
    std::uint32_t time_date_stamp = 0;
    std::uint32_t pointer_to_symbol_table = 0;
    std::uint32_t number_of_symbols = 0;
    std::uint16_t size_of_optional_header = 0;
    std::uint16_t characteristics = 0;
};

struct OptionalHeader {
    std::uint16_t magic = 0;  // 0x20B
    std::uint8_t major_linker_version = 0;
    std::uint8_t minor_linker_version = 0;
    std::uint32_t size_of_code = 0;
    std::uint32_t size_of_initialized_data = 0;
    std::uint32_t size_of_uninitialized_data = 0;
    std::uint32_t address_of_entry_point = 0;
    std::uint32_t base_of_code = 0;
    std::uint64_t image_base = 0;
    std::uint32_t section_alignment = 0;
    std::uint32_t file_alignment = 0;
    std::uint16_t major_os_version = 0;
    std::uint16_t minor_os_version = 0;
    std::uint16_t major_image_version = 0;
    std::uint16_t minor_image_version = 0;
    std::uint16_t major_subsystem_version = 0;
    std::uint16_t minor_subsystem_version = 0;
    std::uint32_t win32_version_value = 0;
    std::uint32_t size_of_image = 0;
    std::uint32_t size_of_headers = 0;
    std::uint32_t checksum = 0;
    std::uint16_t subsystem = 0;
    std::uint16_t dll_characteristics = 0;
    std::uint64_t size_of_stack_reserve = 0;
    std::uint64_t size_of_stack_commit = 0;
    std::uint64_t size_of_heap_reserve = 0;
    std::uint64_t size_of_heap_commit = 0;
    std::uint32_t loader_flags = 0;
    std::uint32_t number_of_rva_and_sizes = 0;  // as stored; at most 16 are read
};

struct SectionHeader {
    std::array<char, 8> raw_name{};
    std::uint32_t virtual_size = 0;
    std::uint32_t virtual_address = 0;
    std::uint32_t size_of_raw_data = 0;
    std::uint32_t pointer_to_raw_data = 0;
    std::uint32_t pointer_to_relocations = 0;
    std::uint32_t pointer_to_linenumbers = 0;
    std::uint16_t number_of_relocations = 0;
    std::uint16_t number_of_linenumbers = 0;
    std::uint32_t characteristics = 0;

    // Name up to the first NUL (at most 8 characters). The view points into this object.
    std::string_view name() const noexcept;
};

struct Guid {
    std::uint32_t data1 = 0;
    std::uint16_t data2 = 0;
    std::uint16_t data3 = 0;
    std::array<std::uint8_t, 8> data4{};
    bool is_zero() const noexcept;
    // "AABBCCDD-EEFF-0011-2233-445566778899" (upper-case hex).
    std::string to_string() const;
};

// ---- exports ----
struct Export {
    std::uint32_t ordinal = 0;  // already biased by the ordinal base
    std::uint32_t rva = 0;      // 0 for an unused slot in the export address table
    bool has_name = false;
    std::string_view name;
    bool is_forwarder = false;       // rva lies inside the export directory
    std::string_view forwarder;      // "DLL.Function" or "DLL.#ordinal"
};

struct ExportTable {
    bool present = false;
    std::string_view dll_name;
    std::uint32_t time_date_stamp = 0;
    std::uint32_t ordinal_base = 0;
    std::vector<Export> entries;  // indexed by (ordinal - ordinal_base)

    const Export* find_by_name(std::string_view name) const noexcept;
    const Export* find_by_ordinal(std::uint32_t ordinal) const noexcept;
};

// ---- imports ----
struct ImportEntry {
    bool by_ordinal = false;
    std::uint16_t ordinal = 0;
    std::uint16_t hint = 0;
    std::string_view name;           // empty when by_ordinal
    std::uint32_t iat_slot_rva = 0;  // FirstThunk + 8 * index
};

struct ImportModule {
    std::string_view dll_name;
    std::uint32_t lookup_table_rva = 0;  // OriginalFirstThunk (0 on bound-only images)
    std::uint32_t iat_rva = 0;           // FirstThunk
    std::uint32_t time_date_stamp = 0;
    std::uint32_t forwarder_chain = 0;
    // False for an in-memory image without an import lookup table: the IAT already holds
    // resolved addresses so entries cannot be named, and `entries` is empty.
    bool names_available = true;
    std::vector<ImportEntry> entries;
};

// Case-insensitive DLL match, exact function name match. Null if not imported by name.
const ImportEntry* find_import(const std::vector<ImportModule>& modules, std::string_view dll, std::string_view function) noexcept;

// ---- debug ----
struct DebugEntry {
    std::uint32_t characteristics = 0;
    std::uint32_t time_date_stamp = 0;
    std::uint16_t major_version = 0;
    std::uint16_t minor_version = 0;
    std::uint32_t type = 0;
    std::uint32_t size_of_data = 0;
    std::uint32_t address_of_raw_data = 0;
    std::uint32_t pointer_to_raw_data = 0;
};

struct CodeViewInfo {
    bool present = false;
    std::uint32_t time_date_stamp = 0;  // of the debug directory entry
    Guid guid;
    std::uint32_t age = 0;
    std::string_view pdb_path;
};

// ---- exceptions (.pdata) ----
struct RuntimeFunction {
    std::uint32_t begin_rva = 0;
    std::uint32_t end_rva = 0;
    std::uint32_t unwind_rva = 0;  // low bit set: chained entry (UNW_FLAG_CHAININFO form)
};

struct ExceptionTable {
    std::vector<RuntimeFunction> entries;
    bool sorted = true;  // begin_rva non-decreasing (required by the OS for lookup)

    // Function covering `rva` (begin <= rva < end); binary search when sorted, linear otherwise.
    const RuntimeFunction* find(std::uint32_t rva) const noexcept;
};

// ---- load config ----
struct LoadConfig {
    bool present = false;
    std::uint32_t size = 0;  // the structure's own Size field
    std::uint32_t time_date_stamp = 0;
    std::uint64_t security_cookie = 0;
    std::uint64_t guard_cf_check_function_pointer = 0;
    std::uint64_t guard_cf_dispatch_function_pointer = 0;
    std::uint64_t guard_cf_function_table = 0;
    std::uint64_t guard_cf_function_count = 0;
    bool has_guard_flags = false;  // false when `size` is too small to contain GuardFlags
    std::uint32_t guard_flags = 0;

    bool cf_instrumented() const noexcept { return (guard_flags & guard::CfInstrumented) != 0; }
};

// ---- TLS ----
struct TlsDirectory {
    bool present = false;
    std::uint64_t start_address_of_raw_data = 0;
    std::uint64_t end_address_of_raw_data = 0;
    std::uint64_t address_of_index = 0;
    std::uint64_t address_of_callbacks = 0;
    std::uint32_t size_of_zero_fill = 0;
    std::uint32_t characteristics = 0;
};

// ---------------------------------------------------------------------------------------------
// Image
// ---------------------------------------------------------------------------------------------

class Image {
public:
    static Result<Image> parse(ByteSpan image_bytes, ImageKind image_kind);
    // Parses the module mapped at `base` (e.g. GetModuleHandleW(nullptr), or the handle from
    // LoadLibraryExW with the low flag bits cleared). Reads the headers at `base` to find
    // SizeOfImage, then views that many bytes; the caller guarantees `base` is a fully mapped PE
    // image. This is the only function that dereferences a raw pointer.
    static Result<Image> parse_mapped_module(const void* base);

    ImageKind kind() const noexcept { return kind_; }
    ByteSpan bytes() const noexcept { return bytes_; }
    const FileHeader& file_header() const noexcept { return file_header_; }
    const OptionalHeader& optional_header() const noexcept { return optional_; }
    std::uint32_t nt_headers_offset() const noexcept { return nt_offset_; }
    std::span<const SectionHeader> sections() const noexcept { return sections_; }
    const DataDirectory& data_directory(std::size_t index) const noexcept;  // zero entry when absent
    const SectionHeader* find_section(std::string_view name) const noexcept;
    const SectionHeader* section_for_rva(std::uint32_t rva) const noexcept;

    // ---- address mapping ----
    // OnDisk: through the section table (or the header region). InMemory: rva itself.
    // nullopt when the RVA is not backed by bytes.
    std::optional<std::uint64_t> rva_to_offset(std::uint32_t rva) const noexcept;
    // Inverse for OnDisk images; identity for InMemory. nullopt if no section holds the offset.
    std::optional<std::uint32_t> offset_to_rva(std::uint64_t offset) const noexcept;
    // `length` bytes at `rva`, entirely within one section's file data (OnDisk) or within the
    // mapped image (InMemory). CML1307 / CML1308 otherwise.
    Result<ByteSpan> read_rva(std::uint32_t rva, std::uint64_t length) const;
    // NUL-terminated string at `rva` (terminator not included). CML1309 if it leaves the section.
    Result<std::string_view> read_cstring_rva(std::uint32_t rva) const;

    // ---- directories (an absent directory is not an error) ----
    Result<ExportTable> exports() const;
    Result<std::vector<ImportModule>> imports() const;
    Result<std::vector<DebugEntry>> debug_entries() const;
    Result<CodeViewInfo> codeview() const;
    Result<ExceptionTable> exception_table() const;
    Result<LoadConfig> load_config() const;
    bool has_tls() const noexcept;
    Result<TlsDirectory> tls() const;

private:
    Image() = default;

    ByteSpan bytes_;
    ImageKind kind_ = ImageKind::OnDisk;
    std::uint32_t nt_offset_ = 0;
    FileHeader file_header_;
    OptionalHeader optional_;
    std::array<DataDirectory, dir::Count> dirs_{};
    std::vector<SectionHeader> sections_;
};

// ---- files ----
Result<std::vector<std::byte>> read_file(const std::string& path);  // CML1300

// An on-disk Image together with the bytes it views.
struct OwnedImage {
    std::vector<std::byte> bytes;
    Image image;
};
Result<OwnedImage> load_image_file(const std::string& path);

}  // namespace cml::pe
