#include "cml/pe/pe.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

#include "cml/util/log.h"

namespace cml::pe {

namespace {

constexpr std::size_t kDosHeaderSize = 64;
constexpr std::size_t kFileHeaderSize = 20;
constexpr std::size_t kOptionalHeaderFixedSize = 112;  // PE32+ up to the data directories
constexpr std::size_t kSectionHeaderSize = 40;
constexpr std::uint16_t kMagicPe32 = 0x10B;
constexpr std::uint16_t kMagicPe32Plus = 0x20B;
constexpr std::size_t kMaxImportModules = 1u << 16;
constexpr std::size_t kMaxImportEntries = 1u << 22;

std::uint32_t rd8(ByteSpan b, std::size_t o) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[o]));
}
std::uint32_t rd16(ByteSpan b, std::size_t o) noexcept {
    return rd8(b, o) | (rd8(b, o + 1) << 8);
}
std::uint32_t rd32(ByteSpan b, std::size_t o) noexcept {
    return rd16(b, o) | (rd16(b, o + 2) << 16);
}
std::uint64_t rd64(ByteSpan b, std::size_t o) noexcept {
    return static_cast<std::uint64_t>(rd32(b, o)) | (static_cast<std::uint64_t>(rd32(b, o + 4)) << 32);
}

bool in_range(std::size_t total, std::uint64_t off, std::uint64_t len) noexcept {
    return off <= total && len <= total - off;
}

std::string format(const char* fmt, ...) CML_PRINTF_LIKE(1, 2);
std::string format(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::string(buf);
}

Diag make_diag(std::uint32_t code, std::string message, std::string what_to_do) {
    return Diag::make(code, std::move(message), std::move(what_to_do));
}

// Re-labels a low-level mapping failure (CML1307/1308/1309) as a directory-specific diagnostic.
Diag rewrap(std::uint32_t code, const char* what, const Diag& inner) {
    return make_diag(code, std::string(what) + ": " + inner.message, "The image is corrupt or truncated; rebuild or re-download it.");
}

char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

std::string_view cstring_in(ByteSpan region, std::size_t offset, bool* ok) noexcept {
    *ok = false;
    if (offset >= region.size()) {
        return {};
    }
    const char* base = reinterpret_cast<const char*>(region.data()) + offset;
    const std::size_t remaining = region.size() - offset;
    const void* nul = std::memchr(base, 0, remaining);
    if (nul == nullptr) {
        return {};
    }
    *ok = true;
    return std::string_view(base, static_cast<std::size_t>(static_cast<const char*>(nul) - base));
}

// Shared by parse() and parse_mapped_module(): validates DOS + NT signatures and the optional
// header magic, returns e_lfanew.
Result<std::uint32_t> check_signatures(ByteSpan b) {
    if (b.size() < kDosHeaderSize || rd16(b, 0) != 0x5A4D) {
        return make_diag(1301, format("not a PE image: missing 'MZ' DOS header in %zu bytes", b.size()),
                         "Pass a .exe or .dll produced by the linker.");
    }
    const std::uint32_t lfanew = rd32(b, 0x3C);
    if (!in_range(b.size(), lfanew, 4 + kFileHeaderSize + 2)) {
        return make_diag(1302, format("e_lfanew 0x%X puts the NT headers outside the %zu-byte image", static_cast<unsigned>(lfanew), b.size()),
                         "The image is truncated or corrupt.");
    }
    if (rd32(b, lfanew) != 0x00004550) {
        return make_diag(1302, format("missing 'PE\\0\\0' signature at offset 0x%X", static_cast<unsigned>(lfanew)),
                         "The image is truncated or corrupt.");
    }
    const std::uint32_t magic = rd16(b, lfanew + 4 + kFileHeaderSize);
    if (magic == kMagicPe32) {
        return make_diag(1303, "image is PE32 (32-bit); only PE32+ (64-bit) images are supported",
                         "Chameleon targets 64-bit processes; build the target for x64.");
    }
    if (magic != kMagicPe32Plus) {
        return make_diag(1304, format("unknown optional header magic 0x%X", static_cast<unsigned>(magic)),
                         "The image is corrupt or is not a PE32+ executable.");
    }
    return lfanew;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Small types
// ---------------------------------------------------------------------------------------------

std::string_view SectionHeader::name() const noexcept {
    std::size_t n = 0;
    while (n < raw_name.size() && raw_name[n] != '\0') {
        ++n;
    }
    return std::string_view(raw_name.data(), n);
}

bool Guid::is_zero() const noexcept {
    if (data1 != 0 || data2 != 0 || data3 != 0) {
        return false;
    }
    for (std::uint8_t v : data4) {
        if (v != 0) {
            return false;
        }
    }
    return true;
}

std::string Guid::to_string() const {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", static_cast<unsigned>(data1),
                  static_cast<unsigned>(data2), static_cast<unsigned>(data3), static_cast<unsigned>(data4[0]),
                  static_cast<unsigned>(data4[1]), static_cast<unsigned>(data4[2]), static_cast<unsigned>(data4[3]),
                  static_cast<unsigned>(data4[4]), static_cast<unsigned>(data4[5]), static_cast<unsigned>(data4[6]),
                  static_cast<unsigned>(data4[7]));
    return std::string(buf);
}

const Export* ExportTable::find_by_name(std::string_view n) const noexcept {
    for (const Export& e : entries) {
        if (e.has_name && e.name == n) {
            return &e;
        }
    }
    return nullptr;
}

const Export* ExportTable::find_by_ordinal(std::uint32_t ordinal) const noexcept {
    if (ordinal < ordinal_base) {
        return nullptr;
    }
    const std::uint64_t i = static_cast<std::uint64_t>(ordinal) - ordinal_base;
    return i < entries.size() ? &entries[static_cast<std::size_t>(i)] : nullptr;
}

const ImportEntry* find_import(const std::vector<ImportModule>& modules, std::string_view dll, std::string_view function) noexcept {
    for (const ImportModule& m : modules) {
        if (!iequals(m.dll_name, dll)) {
            continue;
        }
        for (const ImportEntry& e : m.entries) {
            if (!e.by_ordinal && e.name == function) {
                return &e;
            }
        }
    }
    return nullptr;
}

const RuntimeFunction* ExceptionTable::find(std::uint32_t rva) const noexcept {
    if (sorted) {
        // First entry with begin_rva > rva, then step back one.
        std::size_t lo = 0;
        std::size_t hi = entries.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (entries[mid].begin_rva <= rva) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo == 0) {
            return nullptr;
        }
        const RuntimeFunction& f = entries[lo - 1];
        return rva < f.end_rva ? &f : nullptr;
    }
    for (const RuntimeFunction& f : entries) {
        if (rva >= f.begin_rva && rva < f.end_rva) {
            return &f;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Image::parse
// ---------------------------------------------------------------------------------------------

Result<Image> Image::parse(ByteSpan b, ImageKind image_kind) {
    Result<std::uint32_t> sig = check_signatures(b);
    if (!sig) {
        return sig.error();
    }
    const std::uint32_t nt = sig.value();

    Image img;
    img.bytes_ = b;
    img.kind_ = image_kind;
    img.nt_offset_ = nt;

    const std::size_t fh = static_cast<std::size_t>(nt) + 4;
    FileHeader& f = img.file_header_;
    f.machine = static_cast<std::uint16_t>(rd16(b, fh));
    f.number_of_sections = static_cast<std::uint16_t>(rd16(b, fh + 2));
    f.time_date_stamp = rd32(b, fh + 4);
    f.pointer_to_symbol_table = rd32(b, fh + 8);
    f.number_of_symbols = rd32(b, fh + 12);
    f.size_of_optional_header = static_cast<std::uint16_t>(rd16(b, fh + 16));
    f.characteristics = static_cast<std::uint16_t>(rd16(b, fh + 18));

    const std::size_t oh = fh + kFileHeaderSize;
    if (f.size_of_optional_header < kOptionalHeaderFixedSize || !in_range(b.size(), oh, f.size_of_optional_header)) {
        return make_diag(1305,
                         format("optional header (size %u at offset 0x%zX) is smaller than the 112-byte PE32+ header or extends past the %zu-byte image",
                                static_cast<unsigned>(f.size_of_optional_header), oh, b.size()),
                         "The image is truncated or corrupt.");
    }
    OptionalHeader& o = img.optional_;
    o.magic = static_cast<std::uint16_t>(rd16(b, oh));
    o.major_linker_version = static_cast<std::uint8_t>(rd8(b, oh + 2));
    o.minor_linker_version = static_cast<std::uint8_t>(rd8(b, oh + 3));
    o.size_of_code = rd32(b, oh + 4);
    o.size_of_initialized_data = rd32(b, oh + 8);
    o.size_of_uninitialized_data = rd32(b, oh + 12);
    o.address_of_entry_point = rd32(b, oh + 16);
    o.base_of_code = rd32(b, oh + 20);
    o.image_base = rd64(b, oh + 24);
    o.section_alignment = rd32(b, oh + 32);
    o.file_alignment = rd32(b, oh + 36);
    o.major_os_version = static_cast<std::uint16_t>(rd16(b, oh + 40));
    o.minor_os_version = static_cast<std::uint16_t>(rd16(b, oh + 42));
    o.major_image_version = static_cast<std::uint16_t>(rd16(b, oh + 44));
    o.minor_image_version = static_cast<std::uint16_t>(rd16(b, oh + 46));
    o.major_subsystem_version = static_cast<std::uint16_t>(rd16(b, oh + 48));
    o.minor_subsystem_version = static_cast<std::uint16_t>(rd16(b, oh + 50));
    o.win32_version_value = rd32(b, oh + 52);
    o.size_of_image = rd32(b, oh + 56);
    o.size_of_headers = rd32(b, oh + 60);
    o.checksum = rd32(b, oh + 64);
    o.subsystem = static_cast<std::uint16_t>(rd16(b, oh + 68));
    o.dll_characteristics = static_cast<std::uint16_t>(rd16(b, oh + 70));
    o.size_of_stack_reserve = rd64(b, oh + 72);
    o.size_of_stack_commit = rd64(b, oh + 80);
    o.size_of_heap_reserve = rd64(b, oh + 88);
    o.size_of_heap_commit = rd64(b, oh + 96);
    o.loader_flags = rd32(b, oh + 104);
    o.number_of_rva_and_sizes = rd32(b, oh + 108);

    const std::uint64_t ndirs = std::min<std::uint64_t>(o.number_of_rva_and_sizes, dir::Count);
    if (kOptionalHeaderFixedSize + ndirs * 8 > f.size_of_optional_header) {
        return make_diag(1305,
                         format("optional header declares %u data directories (%llu bytes) but is only %u bytes",
                                static_cast<unsigned>(o.number_of_rva_and_sizes),
                                static_cast<unsigned long long>(kOptionalHeaderFixedSize + ndirs * 8),
                                static_cast<unsigned>(f.size_of_optional_header)),
                         "The image is truncated or corrupt.");
    }
    for (std::size_t i = 0; i < ndirs; ++i) {
        img.dirs_[i].rva = rd32(b, oh + kOptionalHeaderFixedSize + i * 8);
        img.dirs_[i].size = rd32(b, oh + kOptionalHeaderFixedSize + i * 8 + 4);
    }

    const std::size_t st = oh + f.size_of_optional_header;
    if (!in_range(b.size(), st, static_cast<std::uint64_t>(f.number_of_sections) * kSectionHeaderSize)) {
        return make_diag(1306,
                         format("section table (%u sections at offset 0x%zX) extends past the end of the %zu-byte image",
                                static_cast<unsigned>(f.number_of_sections), st, b.size()),
                         "The image is truncated or corrupt.");
    }
    img.sections_.reserve(f.number_of_sections);
    for (std::size_t i = 0; i < f.number_of_sections; ++i) {
        const std::size_t off = st + i * kSectionHeaderSize;
        SectionHeader s;
        for (std::size_t k = 0; k < 8; ++k) {
            s.raw_name[k] = static_cast<char>(rd8(b, off + k));
        }
        s.virtual_size = rd32(b, off + 8);
        s.virtual_address = rd32(b, off + 12);
        s.size_of_raw_data = rd32(b, off + 16);
        s.pointer_to_raw_data = rd32(b, off + 20);
        s.pointer_to_relocations = rd32(b, off + 24);
        s.pointer_to_linenumbers = rd32(b, off + 28);
        s.number_of_relocations = static_cast<std::uint16_t>(rd16(b, off + 32));
        s.number_of_linenumbers = static_cast<std::uint16_t>(rd16(b, off + 34));
        s.characteristics = rd32(b, off + 36);
        img.sections_.push_back(s);
    }

    CML_LOG(Trace, "pe: parsed %s PE32+ image, machine 0x%04x, %u sections",
            image_kind == ImageKind::OnDisk ? "on-disk" : "in-memory", static_cast<unsigned>(f.machine),
            static_cast<unsigned>(f.number_of_sections));
    return img;
}

Result<Image> Image::parse_mapped_module(const void* base) {
    if (base == nullptr) {
        return make_diag(1318, "module base address is null", "Pass the handle of a loaded module.");
    }
    constexpr std::size_t kFirstPage = 0x1000;
    const auto* p = static_cast<const std::byte*>(base);
    const ByteSpan head(p, kFirstPage);  // the first page of any mapped image is readable
    Result<std::uint32_t> sig = check_signatures(head);
    if (!sig) {
        return sig.error();
    }
    const std::size_t size_of_image_off = static_cast<std::size_t>(sig.value()) + 4 + kFileHeaderSize + 56;
    if (!in_range(kFirstPage, size_of_image_off, 4)) {
        return make_diag(1302, "NT headers of the mapped module do not fit in the first page",
                         "The module is not a normally mapped PE image.");
    }
    const std::uint32_t size_of_image = rd32(head, size_of_image_off);
    if (size_of_image < kFirstPage) {
        return make_diag(1305, format("mapped module reports SizeOfImage 0x%X", static_cast<unsigned>(size_of_image)),
                         "The module is not a normally mapped PE image.");
    }
    return parse(ByteSpan(p, size_of_image), ImageKind::InMemory);
}

// ---------------------------------------------------------------------------------------------
// Address mapping
// ---------------------------------------------------------------------------------------------

const DataDirectory& Image::data_directory(std::size_t index) const noexcept {
    static const DataDirectory kEmpty{};
    return index < dirs_.size() ? dirs_[index] : kEmpty;
}

const SectionHeader* Image::find_section(std::string_view name) const noexcept {
    for (const SectionHeader& s : sections_) {
        if (s.name() == name) {
            return &s;
        }
    }
    return nullptr;
}

namespace {
// Address-mapping rule (documented in docs/wp/WP-02.md): the loader maps VirtualSize bytes of a
// section (SizeOfRawData when VirtualSize is 0). Of those, only the first
// min(VirtualSize, SizeOfRawData) bytes come from the file; the rest is zero-fill. Raw bytes past
// VirtualSize are file padding and are never mapped.
std::uint64_t mapped_extent(const SectionHeader& s) noexcept {
    return s.virtual_size != 0 ? s.virtual_size : s.size_of_raw_data;
}
std::uint64_t file_backed_extent(const SectionHeader& s) noexcept {
    return std::min<std::uint64_t>(mapped_extent(s), s.size_of_raw_data);
}
}  // namespace

const SectionHeader* Image::section_for_rva(std::uint32_t rva) const noexcept {
    for (const SectionHeader& s : sections_) {
        const std::uint64_t extent = mapped_extent(s);
        if (rva >= s.virtual_address && static_cast<std::uint64_t>(rva) - s.virtual_address < extent) {
            return &s;
        }
    }
    return nullptr;
}

std::optional<std::uint64_t> Image::rva_to_offset(std::uint32_t rva) const noexcept {
    if (kind_ == ImageKind::InMemory) {
        return rva < bytes_.size() ? std::optional<std::uint64_t>(rva) : std::nullopt;
    }
    if (rva < optional_.size_of_headers) {
        return rva < bytes_.size() ? std::optional<std::uint64_t>(rva) : std::nullopt;
    }
    const SectionHeader* s = section_for_rva(rva);
    if (s == nullptr) {
        return std::nullopt;
    }
    const std::uint64_t in_section = static_cast<std::uint64_t>(rva) - s->virtual_address;
    if (in_section >= file_backed_extent(*s)) {
        return std::nullopt;
    }
    const std::uint64_t off = static_cast<std::uint64_t>(s->pointer_to_raw_data) + in_section;
    return off < bytes_.size() ? std::optional<std::uint64_t>(off) : std::nullopt;
}

std::optional<std::uint32_t> Image::offset_to_rva(std::uint64_t offset) const noexcept {
    if (kind_ == ImageKind::InMemory) {
        return offset < bytes_.size() ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(offset)) : std::nullopt;
    }
    if (offset < optional_.size_of_headers) {
        return static_cast<std::uint32_t>(offset);
    }
    for (const SectionHeader& s : sections_) {
        if (offset >= s.pointer_to_raw_data && offset - s.pointer_to_raw_data < file_backed_extent(s)) {
            return static_cast<std::uint32_t>(s.virtual_address + (offset - s.pointer_to_raw_data));
        }
    }
    return std::nullopt;
}

namespace {

// Bytes from `rva` to the end of the contiguous region that holds it.
Result<ByteSpan> region_at(const Image& img, std::uint32_t rva) {
    const ByteSpan all = img.bytes();
    if (img.kind() == ImageKind::InMemory) {
        if (rva >= all.size()) {
            return make_diag(1307, format("RVA 0x%X is outside the %zu-byte mapped image", static_cast<unsigned>(rva), all.size()),
                             "The image is corrupt.");
        }
        return all.subspan(rva);
    }
    const std::uint32_t headers = img.optional_header().size_of_headers;
    if (rva < headers) {
        const std::size_t end = std::min<std::size_t>(headers, all.size());
        if (rva >= end) {
            return make_diag(1307, format("RVA 0x%X is in the headers but past the end of the file", static_cast<unsigned>(rva)),
                             "The image is truncated.");
        }
        return all.subspan(rva, end - rva);
    }
    const SectionHeader* s = img.section_for_rva(rva);
    if (s == nullptr) {
        return make_diag(1307, format("RVA 0x%X is not inside any section", static_cast<unsigned>(rva)), "The image is corrupt.");
    }
    const std::uint64_t in_section = static_cast<std::uint64_t>(rva) - s->virtual_address;
    if (in_section >= file_backed_extent(*s)) {
        return make_diag(1307, format("RVA 0x%X is in section '%.*s' but beyond its file-backed bytes (zero-fill)", static_cast<unsigned>(rva),
                                      static_cast<int>(s->name().size()), s->name().data()),
                         "The image is corrupt.");
    }
    const std::uint64_t off = static_cast<std::uint64_t>(s->pointer_to_raw_data) + in_section;
    if (off >= all.size()) {
        return make_diag(1307, format("RVA 0x%X maps to file offset 0x%llX past the end of the file", static_cast<unsigned>(rva),
                                      static_cast<unsigned long long>(off)),
                         "The image is truncated.");
    }
    const std::uint64_t avail = std::min<std::uint64_t>(file_backed_extent(*s) - in_section, all.size() - off);
    return all.subspan(static_cast<std::size_t>(off), static_cast<std::size_t>(avail));
}

}  // namespace

Result<ByteSpan> Image::read_rva(std::uint32_t rva, std::uint64_t length) const {
    Result<ByteSpan> region = region_at(*this, rva);
    if (!region) {
        return region.error();
    }
    if (length > region->size()) {
        return make_diag(1308,
                         format("%llu bytes at RVA 0x%X extend past the end of the containing section or image",
                                static_cast<unsigned long long>(length), static_cast<unsigned>(rva)),
                         "The image is corrupt or truncated.");
    }
    return region->subspan(0, static_cast<std::size_t>(length));
}

Result<std::string_view> Image::read_cstring_rva(std::uint32_t rva) const {
    Result<ByteSpan> region = region_at(*this, rva);
    if (!region) {
        return region.error();
    }
    bool ok = false;
    const std::string_view s = cstring_in(*region, 0, &ok);
    if (!ok) {
        return make_diag(1309, format("string at RVA 0x%X is not NUL-terminated within its section", static_cast<unsigned>(rva)),
                         "The image is corrupt or truncated.");
    }
    return s;
}

// ---------------------------------------------------------------------------------------------
// Directories
// ---------------------------------------------------------------------------------------------

Result<ExportTable> Image::exports() const {
    ExportTable t;
    const DataDirectory& dd = dirs_[dir::Export];
    if (dd.rva == 0 || dd.size == 0) {
        return t;
    }
    constexpr const char* kWhat = "export directory";
    Result<ByteSpan> hdr = read_rva(dd.rva, 40);
    if (!hdr) {
        return rewrap(1310, kWhat, hdr.error());
    }
    const ByteSpan h = *hdr;
    t.present = true;
    t.time_date_stamp = rd32(h, 4);
    t.ordinal_base = rd32(h, 16);
    const std::uint32_t name_rva = rd32(h, 12);
    const std::uint32_t nfunc = rd32(h, 20);
    const std::uint32_t nnames = rd32(h, 24);
    const std::uint32_t aof = rd32(h, 28);
    const std::uint32_t aon = rd32(h, 32);
    const std::uint32_t aono = rd32(h, 36);

    if (name_rva != 0) {
        Result<std::string_view> n = read_cstring_rva(name_rva);
        if (!n) {
            return rewrap(1310, kWhat, n.error());
        }
        t.dll_name = *n;
    }

    ByteSpan funcs;
    if (nfunc != 0) {
        Result<ByteSpan> r = read_rva(aof, static_cast<std::uint64_t>(nfunc) * 4);
        if (!r) {
            return rewrap(1310, "export address table", r.error());
        }
        funcs = *r;
    }
    ByteSpan names;
    ByteSpan ords;
    if (nnames != 0) {
        Result<ByteSpan> rn = read_rva(aon, static_cast<std::uint64_t>(nnames) * 4);
        if (!rn) {
            return rewrap(1310, "export name table", rn.error());
        }
        Result<ByteSpan> ro = read_rva(aono, static_cast<std::uint64_t>(nnames) * 2);
        if (!ro) {
            return rewrap(1310, "export ordinal table", ro.error());
        }
        names = *rn;
        ords = *ro;
    }

    t.entries.resize(nfunc);  // bounded: the address table above holds nfunc * 4 real bytes
    for (std::uint32_t i = 0; i < nfunc; ++i) {
        Export& e = t.entries[i];
        e.ordinal = t.ordinal_base + i;
        e.rva = rd32(funcs, static_cast<std::size_t>(i) * 4);
        if (e.rva != 0 && e.rva >= dd.rva && static_cast<std::uint64_t>(e.rva) < static_cast<std::uint64_t>(dd.rva) + dd.size) {
            Result<std::string_view> fwd = read_cstring_rva(e.rva);
            if (!fwd) {
                return rewrap(1310, "export forwarder string", fwd.error());
            }
            e.is_forwarder = true;
            e.forwarder = *fwd;
        }
    }
    for (std::uint32_t j = 0; j < nnames; ++j) {
        const std::uint32_t ord_index = rd16(ords, static_cast<std::size_t>(j) * 2);
        if (ord_index >= nfunc) {
            return make_diag(1310,
                             format("export name #%u uses ordinal index %u but the export address table has only %u entries",
                                    static_cast<unsigned>(j), static_cast<unsigned>(ord_index), static_cast<unsigned>(nfunc)),
                             "The image is corrupt.");
        }
        Result<std::string_view> n = read_cstring_rva(rd32(names, static_cast<std::size_t>(j) * 4));
        if (!n) {
            return rewrap(1310, "export name string", n.error());
        }
        Export& e = t.entries[ord_index];
        if (!e.has_name) {
            e.has_name = true;
            e.name = *n;
        }
    }
    return t;
}

Result<std::vector<ImportModule>> Image::imports() const {
    std::vector<ImportModule> out;
    const DataDirectory& dd = dirs_[dir::Import];
    if (dd.rva == 0 || dd.size == 0) {
        return out;
    }
    constexpr const char* kWhat = "import directory";
    Result<ByteSpan> region = region_at(*this, dd.rva);
    if (!region) {
        return rewrap(1311, kWhat, region.error());
    }
    const ByteSpan desc_region = *region;
    bool terminated = false;
    for (std::size_t d = 0; (d + 1) * 20 <= desc_region.size(); ++d) {
        if (d >= kMaxImportModules) {
            return make_diag(1311, "import directory has more than 65536 descriptors", "The image is corrupt.");
        }
        const ByteSpan desc = desc_region.subspan(d * 20, 20);
        ImportModule m;
        m.lookup_table_rva = rd32(desc, 0);
        m.time_date_stamp = rd32(desc, 4);
        m.forwarder_chain = rd32(desc, 8);
        const std::uint32_t name_rva = rd32(desc, 12);
        m.iat_rva = rd32(desc, 16);
        if (m.lookup_table_rva == 0 && name_rva == 0 && m.iat_rva == 0 && m.time_date_stamp == 0 && m.forwarder_chain == 0) {
            terminated = true;
            break;
        }
        if (name_rva == 0) {
            return make_diag(1311, format("import descriptor %zu has no DLL name", d), "The image is corrupt.");
        }
        Result<std::string_view> dll = read_cstring_rva(name_rva);
        if (!dll) {
            return rewrap(1311, "import DLL name", dll.error());
        }
        m.dll_name = *dll;

        std::uint32_t lookup = m.lookup_table_rva;
        if (lookup == 0) {
            if (m.time_date_stamp != 0) {
                // Bound import without an import name table: the IAT holds bound VAs, not RVAs.
                return make_diag(1311,
                                 format("bound imports without an import name table are not supported (DLL '%.*s' has OriginalFirstThunk 0 and TimeDateStamp 0x%X)",
                                        static_cast<int>(m.dll_name.size()), m.dll_name.data(), static_cast<unsigned>(m.time_date_stamp)),
                                 "Relink without /BIND (or unbind the image) so the import name table is present.");
            }
            if (kind_ == ImageKind::InMemory) {
                m.names_available = false;
                out.push_back(std::move(m));
                continue;
            }
            lookup = m.iat_rva;  // bound/legacy image: the IAT still holds the unbound thunks on disk
        }
        Result<ByteSpan> thunk_region = region_at(*this, lookup);
        if (!thunk_region) {
            return rewrap(1311, "import lookup table", thunk_region.error());
        }
        const ByteSpan thunks = *thunk_region;
        bool thunks_done = false;
        for (std::size_t i = 0; (i + 1) * 8 <= thunks.size(); ++i) {
            if (i >= kMaxImportEntries) {
                return make_diag(1311, "import lookup table has more than 4194304 entries", "The image is corrupt.");
            }
            const std::uint64_t v = rd64(thunks, i * 8);
            if (v == 0) {
                thunks_done = true;
                break;
            }
            ImportEntry e;
            const std::uint64_t slot = static_cast<std::uint64_t>(m.iat_rva) + i * 8;
            if (slot > 0xFFFFFFFFull) {
                return make_diag(1311, "import address table slot RVA overflows 32 bits", "The image is corrupt.");
            }
            e.iat_slot_rva = static_cast<std::uint32_t>(slot);
            if ((v >> 63) != 0) {
                e.by_ordinal = true;
                e.ordinal = static_cast<std::uint16_t>(v & 0xFFFF);
            } else {
                const std::uint32_t hint_name_rva = static_cast<std::uint32_t>(v & 0x7FFFFFFF);
                Result<ByteSpan> hint = read_rva(hint_name_rva, 2);
                if (!hint) {
                    return rewrap(1311, "import hint/name entry", hint.error());
                }
                e.hint = static_cast<std::uint16_t>(rd16(*hint, 0));
                Result<std::string_view> fn = read_cstring_rva(hint_name_rva + 2);
                if (!fn) {
                    return rewrap(1311, "import function name", fn.error());
                }
                e.name = *fn;
            }
            m.entries.push_back(e);
        }
        if (!thunks_done) {
            return make_diag(1311, format("import lookup table of '%.*s' is not terminated within its section",
                                          static_cast<int>(m.dll_name.size()), m.dll_name.data()),
                             "The image is corrupt or truncated.");
        }
        out.push_back(std::move(m));
    }
    if (!terminated) {
        return make_diag(1311, "import directory is not terminated by a null descriptor within its section", "The image is corrupt or truncated.");
    }
    return out;
}

Result<std::vector<DebugEntry>> Image::debug_entries() const {
    std::vector<DebugEntry> out;
    const DataDirectory& dd = dirs_[dir::Debug];
    if (dd.rva == 0 || dd.size == 0) {
        return out;
    }
    const std::uint64_t count = dd.size / 28;
    Result<ByteSpan> r = read_rva(dd.rva, count * 28);
    if (!r) {
        return rewrap(1312, "debug directory", r.error());
    }
    out.reserve(static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t o = i * 28;
        DebugEntry e;
        e.characteristics = rd32(*r, o);
        e.time_date_stamp = rd32(*r, o + 4);
        e.major_version = static_cast<std::uint16_t>(rd16(*r, o + 8));
        e.minor_version = static_cast<std::uint16_t>(rd16(*r, o + 10));
        e.type = rd32(*r, o + 12);
        e.size_of_data = rd32(*r, o + 16);
        e.address_of_raw_data = rd32(*r, o + 20);
        e.pointer_to_raw_data = rd32(*r, o + 24);
        out.push_back(e);
    }
    return out;
}

Result<CodeViewInfo> Image::codeview() const {
    CodeViewInfo info;
    bool skipped_other = false;
    std::uint32_t skipped_sig = 0;
    Result<std::vector<DebugEntry>> entries = debug_entries();
    if (!entries) {
        return entries.error();
    }
    for (const DebugEntry& e : *entries) {
        if (e.type != kDebugTypeCodeView) {
            continue;
        }
        ByteSpan data;
        if (e.address_of_raw_data != 0) {
            Result<ByteSpan> r = read_rva(e.address_of_raw_data, e.size_of_data);
            if (!r) {
                return rewrap(1312, "CodeView debug record", r.error());
            }
            data = *r;
        } else if (kind_ == ImageKind::OnDisk && e.pointer_to_raw_data != 0 &&
                   in_range(bytes_.size(), e.pointer_to_raw_data, e.size_of_data)) {
            data = bytes_.subspan(e.pointer_to_raw_data, e.size_of_data);
        } else {
            return make_diag(1312, "CodeView debug entry has no readable data (no mapped RVA and no valid file pointer)",
                             "The image is corrupt or its debug data was stripped.");
        }
        if (data.size() < 4) {
            return make_diag(1312, format("CodeView record is only %zu bytes", data.size()), "The image is corrupt.");
        }
        const std::uint32_t sig = rd32(data, 0);
        if (sig != 0x53445352) {  // 'RSDS'; skip other formats (e.g. NB10) and keep scanning
            skipped_sig = sig;
            skipped_other = true;
            continue;
        }
        if (data.size() < 25) {
            return make_diag(1312, format("RSDS record is only %zu bytes; 24 bytes plus a path are required", data.size()),
                             "The image is corrupt.");
        }
        info.present = true;
        info.time_date_stamp = e.time_date_stamp;
        info.guid.data1 = rd32(data, 4);
        info.guid.data2 = static_cast<std::uint16_t>(rd16(data, 8));
        info.guid.data3 = static_cast<std::uint16_t>(rd16(data, 10));
        for (std::size_t k = 0; k < 8; ++k) {
            info.guid.data4[k] = static_cast<std::uint8_t>(rd8(data, 12 + k));
        }
        info.age = rd32(data, 20);
        bool ok = false;
        info.pdb_path = cstring_in(data, 24, &ok);
        if (!ok) {
            return make_diag(1312, "RSDS PDB path is not NUL-terminated within the record", "The image is corrupt.");
        }
        return info;
    }
    if (skipped_other) {
        return make_diag(1313,
                         format("no RSDS CodeView record found; the only CodeView record has signature 0x%08X (only RSDS / PDB 7.0 is supported)",
                                static_cast<unsigned>(skipped_sig)),
                         "Link with /DEBUG so the linker emits an RSDS record (MSVC link and lld-link both do).");
    }
    return info;
}

Result<ExceptionTable> Image::exception_table() const {
    ExceptionTable t;
    const DataDirectory& dd = dirs_[dir::Exception];
    if (dd.rva == 0 || dd.size == 0) {
        return t;
    }
    const std::uint64_t count = dd.size / 12;  // a trailing partial entry is ignored, as the OS does
    Result<ByteSpan> r = read_rva(dd.rva, count * 12);
    if (!r) {
        return rewrap(1314, "exception directory", r.error());
    }
    t.entries.reserve(static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        RuntimeFunction f;
        f.begin_rva = rd32(*r, i * 12);
        f.end_rva = rd32(*r, i * 12 + 4);
        f.unwind_rva = rd32(*r, i * 12 + 8);
        if (!t.entries.empty() && f.begin_rva < t.entries.back().begin_rva) {
            t.sorted = false;
        }
        t.entries.push_back(f);
    }
    return t;
}

Result<LoadConfig> Image::load_config() const {
    LoadConfig c;
    const DataDirectory& dd = dirs_[dir::LoadConfig];
    if (dd.rva == 0 || dd.size == 0) {
        return c;
    }
    constexpr const char* kWhat = "load config directory";
    // The loader (and LLVM) go by the structure's own Size field, not the data directory size
    // (linkers often leave the latter smaller); bound it by the bytes actually available.
    Result<ByteSpan> region = region_at(*this, dd.rva);
    if (!region) {
        return rewrap(1315, kWhat, region.error());
    }
    if (region->size() < 4) {
        return make_diag(1315, "load config directory has fewer than 4 readable bytes", "The image is corrupt or truncated.");
    }
    const std::uint64_t declared = rd32(*region, 0);
    if (declared < 4) {
        return make_diag(1315, format("load config Size field is %u (must be at least 4)", static_cast<unsigned>(declared)), "The image is corrupt.");
    }
    const std::uint64_t n = std::min<std::uint64_t>(declared, region->size());
    const ByteSpan body = region->subspan(0, static_cast<std::size_t>(n));
    const ByteSpan b = body;
    c.present = true;
    c.size = rd32(b, 0);
    if (n >= 8) c.time_date_stamp = rd32(b, 4);
    if (n >= 96) c.security_cookie = rd64(b, 88);
    if (n >= 120) c.guard_cf_check_function_pointer = rd64(b, 112);
    if (n >= 128) c.guard_cf_dispatch_function_pointer = rd64(b, 120);
    if (n >= 136) c.guard_cf_function_table = rd64(b, 128);
    if (n >= 144) c.guard_cf_function_count = rd64(b, 136);
    if (n >= 148) {
        c.has_guard_flags = true;
        c.guard_flags = rd32(b, 144);
    }
    return c;
}

bool Image::has_tls() const noexcept {
    return dirs_[dir::Tls].rva != 0 && dirs_[dir::Tls].size != 0;
}

Result<TlsDirectory> Image::tls() const {
    TlsDirectory t;
    if (!has_tls()) {
        return t;
    }
    const DataDirectory& dd = dirs_[dir::Tls];
    if (dd.size < 40) {
        return make_diag(1316, format("TLS directory is %u bytes; IMAGE_TLS_DIRECTORY64 needs 40", static_cast<unsigned>(dd.size)),
                         "The image is corrupt.");
    }
    Result<ByteSpan> r = read_rva(dd.rva, 40);
    if (!r) {
        return rewrap(1316, "TLS directory", r.error());
    }
    t.present = true;
    t.start_address_of_raw_data = rd64(*r, 0);
    t.end_address_of_raw_data = rd64(*r, 8);
    t.address_of_index = rd64(*r, 16);
    t.address_of_callbacks = rd64(*r, 24);
    t.size_of_zero_fill = rd32(*r, 32);
    t.characteristics = rd32(*r, 36);
    return t;
}

// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------

Result<std::vector<std::byte>> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        return make_diag(1300, format("cannot open '%s' for reading", path.c_str()), "Check that the file exists and is readable.");
    }
    const std::streamoff size = f.tellg();
    if (size < 0) {
        return make_diag(1300, format("cannot determine the size of '%s'", path.c_str()), "Check that the path names a regular file.");
    }
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    f.seekg(0, std::ios::beg);
    if (size > 0) {
        f.read(reinterpret_cast<char*>(data.data()), size);
    }
    if (!f) {
        return make_diag(1300, format("failed while reading '%s'", path.c_str()), "Check that the file is not locked or truncated.");
    }
    return data;
}

Result<OwnedImage> load_image_file(const std::string& path) {
    Result<std::vector<std::byte>> bytes = read_file(path);
    if (!bytes) {
        return bytes.error();
    }
    std::vector<std::byte> data = std::move(bytes).value();
    Result<Image> image = Image::parse(ByteSpan(data.data(), data.size()), ImageKind::OnDisk);
    if (!image) {
        Diag d = image.error();
        d.message = path + ": " + d.message;
        return d;
    }
    return OwnedImage{std::move(data), std::move(image).value()};
}

}  // namespace cml::pe
