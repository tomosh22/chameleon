#include "cml/coff/writer.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

#include "cml/util/log.h"

namespace cml::coff {

namespace {

constexpr std::size_t kRegularHeaderSize = 20;
constexpr std::size_t kBigobjHeaderSize = 56;
constexpr std::size_t kSectionHeaderSize = 40;
constexpr std::size_t kRelocationSize = 10;
constexpr std::size_t kLineNumberSize = 6;
constexpr std::uint32_t kMaxRegularSections = 65279;
constexpr std::uint64_t kMax32 = 0xFFFFFFFFull;

// ANON_OBJECT_HEADER_BIGOBJ class id as stored in the file (same bytes the reader checks).
constexpr std::array<std::uint8_t, 16> kBigobjClassId = {0xC7, 0xA1, 0xBA, 0xD1, 0xEE, 0xBA, 0xA9, 0x4B,
                                                         0xAF, 0x20, 0xFA, 0xF6, 0x6A, 0xA4, 0xDC, 0xB8};

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

std::uint32_t get8(ByteSpan b, std::size_t o) noexcept { return std::to_integer<std::uint32_t>(b[o]); }
std::uint32_t get16(ByteSpan b, std::size_t o) noexcept { return get8(b, o) | (get8(b, o + 1) << 8); }
std::uint32_t get32(ByteSpan b, std::size_t o) noexcept { return get16(b, o) | (get16(b, o + 2) << 16); }

void set8(std::byte* p, std::uint32_t v) noexcept { *p = static_cast<std::byte>(v & 0xFFu); }
void set16(std::byte* p, std::uint32_t v) noexcept {
    set8(p, v);
    set8(p + 1, v >> 8);
}
void set32(std::byte* p, std::uint32_t v) noexcept {
    set16(p, v & 0xFFFFu);
    set16(p + 2, v >> 16);
}


ByteVector copy_bytes(ByteSpan s) { return ByteVector(s.begin(), s.end()); }

}  // namespace

// ---------------------------------------------------------------------------------------------
// ObjectModel: construction
// ---------------------------------------------------------------------------------------------

ObjectModel ObjectModel::make_empty(Machine machine, bool bigobj) {
    ObjectModel m;
    m.is_bigobj = bigobj;
    m.has_string_table = true;  // a COFF file always carries at least the 4-byte string-table size field
    const std::uint32_t mach = static_cast<std::uint32_t>(machine);
    if (bigobj) {
        m.header.assign(kBigobjHeaderSize, std::byte{0});
        std::byte* h = m.header.data();
        set16(h + 0, 0);       // Sig1
        set16(h + 2, 0xFFFF);  // Sig2
        set16(h + 4, 2);       // Version
        set16(h + 6, mach);
        for (std::size_t i = 0; i < 16; ++i) {
            h[12 + i] = static_cast<std::byte>(kBigobjClassId[i]);
        }
    } else {
        m.header.assign(kRegularHeaderSize, std::byte{0});
        set16(m.header.data(), mach);
    }
    return m;
}

Result<ObjectModel> ObjectModel::from_object(const Object& o) {
    const ObjectHeader& h = o.header();
    ObjectModel m;
    m.is_bigobj = h.is_bigobj;
    m.header = copy_bytes(o.header_bytes());
    if (h.section_table_offset > h.header_size) {
        m.optional_header = copy_bytes(o.bytes().subspan(h.header_size, h.section_table_offset - h.header_size));
    }

    // A region of the file we have to reproduce, to learn the padding between regions.
    enum class Kind { Data, Reloc, Lines, Symbols };
    struct Region {
        std::uint64_t offset;
        std::uint64_t size;
        Kind kind;
        std::size_t section;  // index into m.sections
    };
    std::vector<Region> regions;
    const std::size_t file_size = o.bytes().size();

    m.sections.reserve(o.sections().size());
    for (const Section& s : o.sections()) {
        ModelSection ms;
        ms.name = std::string(s.name);
        ms.raw_name = s.raw_name;
        ms.virtual_size = s.virtual_size;
        ms.virtual_address = s.virtual_address;
        ms.characteristics = s.characteristics;
        ms.size_of_raw_data = s.size_of_raw_data;
        ms.pointer_to_raw_data = s.pointer_to_raw_data;
        ms.pointer_to_relocations = s.pointer_to_relocations;
        ms.pointer_to_linenumbers = s.pointer_to_linenumbers;
        ms.linenumber_count = s.linenumber_count;

        const std::size_t si = m.sections.size();
        if (!s.data.empty()) {
            ms.has_file_data = true;
            ms.data = copy_bytes(s.data);
            ms.data_offset = s.pointer_to_raw_data;
            regions.push_back({s.pointer_to_raw_data, s.data.size(), Kind::Data, si});
        }
        if (!s.relocation_table.empty()) {
            ms.extended_relocations = s.has_extended_relocations;
            if (s.has_extended_relocations) {
                for (std::size_t k = 0; k < 6; ++k) {
                    ms.overflow_record_tail[k] = static_cast<std::uint8_t>(get8(s.relocation_table, 4 + k));
                }
            }
            ms.relocations.reserve(s.relocation_count);
            for (std::uint32_t r = 0; r < s.relocation_count; ++r) {
                const std::size_t off = static_cast<std::size_t>(r) * kRelocationSize;
                ModelRelocation mr;
                mr.virtual_address = get32(s.relocation_bytes, off);
                mr.symbol_index = get32(s.relocation_bytes, off + 4);
                mr.type = static_cast<std::uint16_t>(get16(s.relocation_bytes, off + 8));
                ms.relocations.push_back(mr);
            }
            ms.reloc_offset = s.pointer_to_relocations;
            regions.push_back({s.pointer_to_relocations, s.relocation_table.size(), Kind::Reloc, si});
        }
        if (s.linenumber_count != 0 && s.pointer_to_linenumbers != 0) {
            const std::uint64_t size = static_cast<std::uint64_t>(s.linenumber_count) * kLineNumberSize;
            if (static_cast<std::uint64_t>(s.pointer_to_linenumbers) + size > file_size) {
                return make_diag(diag::kUnrepresentable,
                                 format("section %u '%.*s' line-number table (offset %u, %u records) extends past the end of the object",
                                        static_cast<unsigned>(s.index), static_cast<int>(s.name.size()), s.name.data(),
                                        static_cast<unsigned>(s.pointer_to_linenumbers),
                                        static_cast<unsigned>(s.linenumber_count)),
                                 "The object is corrupt; rebuild it.");
            }
            ms.linenumbers = copy_bytes(o.bytes().subspan(s.pointer_to_linenumbers, static_cast<std::size_t>(size)));
            ms.linenumber_offset = s.pointer_to_linenumbers;
            regions.push_back({s.pointer_to_linenumbers, size, Kind::Lines, si});
        }
        m.sections.push_back(std::move(ms));
    }

    m.symbols.reserve(o.symbols().size());
    for (const Symbol& s : o.symbols()) {
        ModelSymbol ms;
        ms.name = std::string(s.name);
        ms.raw_name = s.raw_name;
        ms.value = s.value;
        ms.section_number = s.section_number;
        ms.type = s.type;
        ms.storage_class = static_cast<std::uint8_t>(s.storage_class);
        ms.aux = copy_bytes(s.aux);
        m.symbols.push_back(std::move(ms));
    }
    m.has_symbol_table = h.symbol_table_offset != 0;
    if (!o.string_table_bytes().empty()) {
        m.has_string_table = true;
        m.string_table = copy_bytes(o.string_table_bytes().subspan(4));
    }
    if (m.has_symbol_table) {
        m.symbols_offset = h.symbol_table_offset;
        const std::uint64_t size =
            static_cast<std::uint64_t>(h.symbol_count) * h.symbol_record_size + o.string_table_bytes().size();
        regions.push_back({h.symbol_table_offset, size, Kind::Symbols, 0});
    }

    // Padding between regions and trailing bytes.
    std::stable_sort(regions.begin(), regions.end(),
                     [](const Region& a, const Region& b) { return a.offset < b.offset; });
    std::uint64_t cursor = h.section_table_offset + static_cast<std::uint64_t>(h.section_count) * kSectionHeaderSize;
    for (const Region& r : regions) {
        if (r.offset < cursor) {
            return make_diag(diag::kUnrepresentable,
                             format("object regions overlap or lie inside the headers (region at offset %llu, previous region ends at %llu)",
                                    static_cast<unsigned long long>(r.offset), static_cast<unsigned long long>(cursor)),
                             "The object cannot be rewritten losslessly; rebuild it with the compiler.");
        }
        ByteVector pad = copy_bytes(o.bytes().subspan(static_cast<std::size_t>(cursor),
                                                      static_cast<std::size_t>(r.offset - cursor)));
        switch (r.kind) {
            case Kind::Data: m.sections[r.section].data_pad = std::move(pad); break;
            case Kind::Reloc: m.sections[r.section].reloc_pad = std::move(pad); break;
            case Kind::Lines: m.sections[r.section].linenumber_pad = std::move(pad); break;
            case Kind::Symbols: m.symbols_pad = std::move(pad); break;
        }
        cursor = r.offset + r.size;
    }
    if (cursor < file_size) {
        m.trailing = copy_bytes(o.bytes().subspan(static_cast<std::size_t>(cursor)));
    }
    return m;
}

std::uint64_t ObjectModel::raw_symbol_count() const noexcept {
    const std::size_t rec = symbol_record_size();
    std::uint64_t n = 0;
    for (const ModelSymbol& s : symbols) {
        n += 1 + s.aux.size() / rec;
    }
    return n;
}

Result<std::uint32_t> ObjectModel::add_string(std::string_view text) {
    const std::uint64_t offset = 4ull + string_table.size();
    const std::uint64_t end = offset + text.size() + 1;
    if (end > kMax32) {
        return make_diag(diag::kSizeOverflow,
                         format("string table would reach %llu bytes adding '%.*s'; the COFF limit is 4 GiB",
                                static_cast<unsigned long long>(end), static_cast<int>(std::min<std::size_t>(text.size(), 64)),
                                text.data()),
                         "Split the module into smaller objects.");
    }
    for (char c : text) {
        string_table.push_back(static_cast<std::byte>(c));
    }
    string_table.push_back(std::byte{0});
    has_string_table = true;
    return static_cast<std::uint32_t>(offset);
}

Result<std::array<std::uint8_t, 8>> ObjectModel::encode_symbol_name(std::string_view name) {
    std::array<std::uint8_t, 8> raw{};
    if (name.empty()) {
        return make_diag(diag::kBadSymbol, "symbol name is empty",
                         "Give the symbol a non-empty name (an all-zero name field means string-table offset 0, which is invalid).");
    }
    if (name.size() <= 8 && name.find('\0') == std::string_view::npos) {
        for (std::size_t i = 0; i < name.size(); ++i) {
            raw[i] = static_cast<std::uint8_t>(name[i]);
        }
        return raw;
    }
    auto off = add_string(name);
    if (!off) {
        return off.error();
    }
    // First four bytes zero, then the little-endian string-table offset.
    raw[4] = static_cast<std::uint8_t>(*off);
    raw[5] = static_cast<std::uint8_t>(*off >> 8);
    raw[6] = static_cast<std::uint8_t>(*off >> 16);
    raw[7] = static_cast<std::uint8_t>(*off >> 24);
    return raw;
}

Result<std::uint32_t> ObjectModel::add_symbol(std::string_view name, std::uint32_t value, std::int32_t section_number,
                                              std::uint16_t type, StorageClass storage_class, ByteVector aux) {
    const std::uint64_t index = raw_symbol_count();
    if (index + 1 + aux.size() / symbol_record_size() > kMax32) {
        return make_diag(diag::kSizeOverflow, "symbol table would exceed 2^32 records",
                         "Split the module into smaller objects.");
    }
    auto raw = encode_symbol_name(name);
    if (!raw) {
        return raw.error();
    }
    ModelSymbol s;
    s.name = std::string(name);
    s.raw_name = *raw;
    s.value = value;
    s.section_number = section_number;
    s.type = type;
    s.storage_class = static_cast<std::uint8_t>(storage_class);
    s.aux = std::move(aux);
    symbols.push_back(std::move(s));
    has_symbol_table = true;
    return static_cast<std::uint32_t>(index);
}

Result<std::uint32_t> ObjectModel::add_section(std::string_view name, std::uint32_t characteristics, ByteVector data,
                                               std::vector<ModelRelocation> relocations) {
    if (name.empty() || name.find('\0') != std::string_view::npos) {
        return make_diag(diag::kBadSectionName, "section name is empty or contains a NUL byte",
                         "Give the section a non-empty name.");
    }
    if (sections.size() >= (is_bigobj ? kMax32 : kMaxRegularSections)) {
        return make_diag(diag::kTooManySections,
                         format("object already has %zu sections; a %s object cannot hold more", sections.size(),
                                is_bigobj ? "bigobj" : "regular"),
                         "Use the /bigobj format or split the module.");
    }
    ModelSection s;
    s.name = std::string(name);
    if (name.size() <= 8 && name[0] != '/') {  // a leading '/' would read as a string-table reference
        std::memcpy(s.raw_name.data(), name.data(), name.size());
    } else {
        auto off = add_string(name);
        if (!off) {
            return off.error();
        }
        if (*off > 9999999u) {
            return make_diag(diag::kBadSectionName,
                             format("section name '%.*s' lies at string-table offset %u, beyond the 7 decimal digits of the /nnn form",
                                    static_cast<int>(std::min<std::size_t>(name.size(), 64)), name.data(),
                                    static_cast<unsigned>(*off)),
                             "Add fewer long-named sections or shorten symbol names.");
        }
        char buf[16];
        std::snprintf(buf, sizeof(buf), "/%u", static_cast<unsigned>(*off));
        std::memcpy(s.raw_name.data(), buf, std::strlen(buf));
    }
    s.characteristics = characteristics;
    s.has_file_data = !data.empty();
    s.size_of_raw_data = static_cast<std::uint32_t>(data.size());
    s.data = std::move(data);
    s.relocations = std::move(relocations);
    sections.push_back(std::move(s));
    return static_cast<std::uint32_t>(sections.size());
}

ByteVector make_section_definition_aux(bool bigobj, std::uint32_t length, std::uint32_t relocation_count,
                                       std::uint32_t checksum, std::uint32_t associated_section,
                                       std::uint8_t selection) {
    ByteVector aux(bigobj ? 20u : 18u, std::byte{0});
    std::byte* p = aux.data();
    set32(p + 0, length);
    set16(p + 4, relocation_count > 0xFFFFu ? 0xFFFFu : relocation_count);  // 0xFFFF with NRELOC_OVFL sections
    set16(p + 6, 0);
    set32(p + 8, checksum);
    set16(p + 12, associated_section & 0xFFFFu);
    set8(p + 14, selection);
    if (bigobj) {
        set16(p + 16, associated_section >> 16);
    }
    return aux;
}

// ---------------------------------------------------------------------------------------------
// write
// ---------------------------------------------------------------------------------------------

namespace {

enum class RegionKind { Data = 0, Reloc = 1, Lines = 2, Symbols = 3 };

struct PlannedRegion {
    std::uint64_t key;       // original offset, kNoOffset for new regions
    std::uint32_t rank;      // canonical order among equal keys
    RegionKind kind;
    std::size_t section;     // section index for Data/Reloc/Lines
    const ByteVector* pad;
    std::uint64_t size;      // bytes of the region itself
    std::uint64_t offset = 0;
};

bool section_needs_extended(const ModelSection& s) noexcept {
    return s.extended_relocations || s.relocations.size() >= 0xFFFFu;
}

}  // namespace

Result<ByteVector> write(const ObjectModel& m) {
    const std::size_t rec = m.symbol_record_size();
    const std::size_t header_size = m.is_bigobj ? kBigobjHeaderSize : kRegularHeaderSize;
    if (m.header.size() != header_size) {
        return make_diag(diag::kUnrepresentable,
                         format("model header is %zu bytes; a %s object needs %zu", m.header.size(),
                                m.is_bigobj ? "bigobj" : "regular", header_size),
                         "Create the model with ObjectModel::make_empty() or from_object().");
    }
    if (m.is_bigobj ? m.sections.size() > kMax32 : m.sections.size() > kMaxRegularSections) {
        return make_diag(diag::kTooManySections,
                         format("regular COFF object would have %zu sections; the limit is 65279 (larger objects need /bigobj)",
                                m.sections.size()),
                         "Write the object in the /bigobj format.");
    }

    // ---- symbols: structure ----
    const std::uint64_t sym_records = m.raw_symbol_count();
    if (sym_records > kMax32) {
        return make_diag(diag::kSizeOverflow, format("symbol table would hold %llu records", static_cast<unsigned long long>(sym_records)),
                         "Split the module into smaller objects.");
    }
    for (std::size_t i = 0; i < m.symbols.size(); ++i) {
        const ModelSymbol& s = m.symbols[i];
        if (std::all_of(s.raw_name.begin(), s.raw_name.end(), [](std::uint8_t b) { return b == 0; })) {
            return make_diag(diag::kBadSymbol, format("symbol %zu has an empty name (all-zero name field)", i),
                             "Give the symbol a non-empty name.");
        }
        if (s.aux.size() % rec != 0 || s.aux.size() / rec > 255) {
            return make_diag(diag::kBadSymbol,
                             format("symbol '%.*s' has %zu aux bytes (not a whole number of %zu-byte records, or more than 255)",
                                    static_cast<int>(std::min<std::size_t>(s.name.size(), 64)), s.name.data(), s.aux.size(), rec),
                             "Fix the symbol's aux records.");
        }
        if (s.section_number > 0 && static_cast<std::uint64_t>(s.section_number) > m.sections.size()) {
            return make_diag(diag::kBadSymbol,
                             format("symbol '%.*s' refers to section %d but the object has %zu sections",
                                    static_cast<int>(std::min<std::size_t>(s.name.size(), 64)), s.name.data(),
                                    static_cast<int>(s.section_number), m.sections.size()),
                             "Add the section before the symbol, or fix the section number.");
        }
        if (s.section_number < -2) {
            return make_diag(diag::kBadSymbol,
                             format("symbol '%.*s' has invalid section number %d", static_cast<int>(std::min<std::size_t>(s.name.size(), 64)),
                                    s.name.data(), static_cast<int>(s.section_number)),
                             "Use 0 (undefined), -1 (absolute), -2 (debug) or a section index.");
        }
    }
    for (std::size_t si = 0; si < m.sections.size(); ++si) {
        const ModelSection& s = m.sections[si];
        if (!s.linenumbers.empty() && s.linenumbers.size() != static_cast<std::size_t>(s.linenumber_count) * kLineNumberSize) {
            return make_diag(diag::kUnrepresentable,
                             format("section %zu '%s' declares %u line numbers but holds %zu bytes of them (%zu expected)", si + 1,
                                    s.name.c_str(), static_cast<unsigned>(s.linenumber_count), s.linenumbers.size(),
                                    static_cast<std::size_t>(s.linenumber_count) * kLineNumberSize),
                             "Keep linenumber_count equal to linenumbers.size() / 6.");
        }
    }
    // Relocation targets must exist.
    for (std::size_t si = 0; si < m.sections.size(); ++si) {
        for (const ModelRelocation& r : m.sections[si].relocations) {
            if (r.symbol_index >= sym_records) {
                return make_diag(diag::kBadRelocation,
                                 format("section %zu '%s' has a relocation at offset %u naming symbol index %u but the table has %llu records",
                                        si + 1, m.sections[si].name.c_str(), static_cast<unsigned>(r.virtual_address),
                                        static_cast<unsigned>(r.symbol_index), static_cast<unsigned long long>(sym_records)),
                                 "Relocations must name existing symbol indices; never remove or reorder symbols.");
            }
        }
    }

    // ---- layout ----
    std::vector<PlannedRegion> regions;
    regions.reserve(m.sections.size() * 2 + 1);
    for (std::size_t i = 0; i < m.sections.size(); ++i) {
        const ModelSection& s = m.sections[i];
        const std::uint32_t rank_base = static_cast<std::uint32_t>(i) * 3u;
        if (s.has_file_data && !s.data.empty()) {
            regions.push_back({s.data_offset, rank_base, RegionKind::Data, i, &s.data_pad, s.data.size()});
        }
        if (!s.relocations.empty() || s.extended_relocations) {
            const std::uint64_t n = s.relocations.size() + (section_needs_extended(s) ? 1u : 0u);
            regions.push_back({s.reloc_offset, rank_base + 1, RegionKind::Reloc, i, &s.reloc_pad, n * kRelocationSize});
        }
        if (!s.linenumbers.empty()) {
            regions.push_back({s.linenumber_offset, rank_base + 2, RegionKind::Lines, i, &s.linenumber_pad, s.linenumbers.size()});
        }
    }
    const bool strtab = m.has_string_table || !m.string_table.empty();
    const std::uint64_t strtab_total = strtab ? 4ull + m.string_table.size() : 0ull;
    if (strtab && strtab_total > kMax32) {
        return make_diag(diag::kSizeOverflow, "string table exceeds 4 GiB", "Split the module into smaller objects.");
    }
    const bool symbol_region = m.has_symbol_table || !m.symbols.empty() || strtab;
    if (symbol_region) {
        regions.push_back({m.symbols_offset, static_cast<std::uint32_t>(m.sections.size()) * 3u + 3u, RegionKind::Symbols, 0,
                           &m.symbols_pad, sym_records * rec + strtab_total});
    }
    std::stable_sort(regions.begin(), regions.end(), [](const PlannedRegion& a, const PlannedRegion& b) {
        return a.key != b.key ? a.key < b.key : a.rank < b.rank;
    });

    std::uint64_t cursor = header_size + m.optional_header.size() + m.sections.size() * kSectionHeaderSize;
    for (PlannedRegion& r : regions) {
        cursor += r.pad->size();
        r.offset = cursor;
        cursor += r.size;
        if (r.offset > kMax32 || cursor > kMax32) {
            return make_diag(diag::kSizeOverflow,
                             format("object would be larger than 4 GiB (a region would end at byte %llu)", static_cast<unsigned long long>(cursor)),
                             "Split the module into smaller objects.");
        }
    }
    cursor += m.trailing.size();
    if (cursor > kMax32) {
        return make_diag(diag::kSizeOverflow, "object would be larger than 4 GiB", "Split the module into smaller objects.");
    }

    // Offsets chosen by the plan.
    std::vector<std::uint64_t> data_at(m.sections.size(), kNoOffset), reloc_at(m.sections.size(), kNoOffset),
        lines_at(m.sections.size(), kNoOffset);
    std::uint64_t symbols_at = kNoOffset;
    for (const PlannedRegion& r : regions) {
        switch (r.kind) {
            case RegionKind::Data: data_at[r.section] = r.offset; break;
            case RegionKind::Reloc: reloc_at[r.section] = r.offset; break;
            case RegionKind::Lines: lines_at[r.section] = r.offset; break;
            case RegionKind::Symbols: symbols_at = r.offset; break;
        }
    }

    // ---- emit ----
    ByteVector out(static_cast<std::size_t>(cursor), std::byte{0});
    std::byte* base = out.data();

    std::memcpy(base, m.header.data(), header_size);
    if (m.is_bigobj) {
        set32(base + 44, static_cast<std::uint32_t>(m.sections.size()));
        set32(base + 48, symbol_region ? static_cast<std::uint32_t>(symbols_at) : 0u);
        set32(base + 52, static_cast<std::uint32_t>(sym_records));
    } else {
        set16(base + 2, static_cast<std::uint32_t>(m.sections.size()));
        set32(base + 8, symbol_region ? static_cast<std::uint32_t>(symbols_at) : 0u);
        set32(base + 12, static_cast<std::uint32_t>(sym_records));
    }
    if (!m.optional_header.empty()) {
        std::memcpy(base + header_size, m.optional_header.data(), m.optional_header.size());
    }

    std::byte* sec = base + header_size + m.optional_header.size();
    for (std::size_t i = 0; i < m.sections.size(); ++i, sec += kSectionHeaderSize) {
        const ModelSection& s = m.sections[i];
        std::memcpy(sec, s.raw_name.data(), 8);
        set32(sec + 8, s.virtual_size);
        set32(sec + 12, s.virtual_address);
        const bool has_data = data_at[i] != kNoOffset;
        set32(sec + 16, has_data ? static_cast<std::uint32_t>(s.data.size()) : s.size_of_raw_data);
        set32(sec + 20, has_data ? static_cast<std::uint32_t>(data_at[i]) : s.pointer_to_raw_data);
        const bool has_reloc = reloc_at[i] != kNoOffset;
        const bool ext = section_needs_extended(s);
        set32(sec + 24, has_reloc ? static_cast<std::uint32_t>(reloc_at[i]) : s.pointer_to_relocations);
        // Without a line-number table both fields are written as 0 (the format is obsolete).
        set32(sec + 28, lines_at[i] != kNoOffset ? static_cast<std::uint32_t>(lines_at[i]) : 0u);
        set16(sec + 32, has_reloc ? (ext ? 0xFFFFu : static_cast<std::uint32_t>(s.relocations.size())) : 0u);
        set16(sec + 34, lines_at[i] != kNoOffset ? s.linenumber_count : 0u);
        set32(sec + 36, ext ? (s.characteristics | scn::LnkNrelocOvfl) : s.characteristics);
    }

    for (const PlannedRegion& r : regions) {
        // The pad bytes are the `r.pad->size()` bytes in front of the region; the output is zero
        // initialised, so copy them only when they carry something.
        if (!r.pad->empty()) {
            std::memcpy(base + r.offset - r.pad->size(), r.pad->data(), r.pad->size());
        }
        std::byte* p = base + r.offset;
        switch (r.kind) {
            case RegionKind::Data: {
                const ModelSection& s = m.sections[r.section];
                std::memcpy(p, s.data.data(), s.data.size());
                break;
            }
            case RegionKind::Reloc: {
                const ModelSection& s = m.sections[r.section];
                if (section_needs_extended(s)) {
                    set32(p, static_cast<std::uint32_t>(s.relocations.size() + 1));
                    for (std::size_t k = 0; k < 6; ++k) {
                        p[4 + k] = static_cast<std::byte>(s.overflow_record_tail[k]);
                    }
                    p += kRelocationSize;
                }
                for (const ModelRelocation& rel : s.relocations) {
                    set32(p, rel.virtual_address);
                    set32(p + 4, rel.symbol_index);
                    set16(p + 8, rel.type);
                    p += kRelocationSize;
                }
                break;
            }
            case RegionKind::Lines: {
                const ModelSection& s = m.sections[r.section];
                std::memcpy(p, s.linenumbers.data(), s.linenumbers.size());
                break;
            }
            case RegionKind::Symbols: {
                for (const ModelSymbol& s : m.symbols) {
                    std::memcpy(p, s.raw_name.data(), 8);
                    set32(p + 8, s.value);
                    const std::uint32_t aux_count = static_cast<std::uint32_t>(s.aux.size() / rec);
                    if (m.is_bigobj) {
                        set32(p + 12, static_cast<std::uint32_t>(s.section_number));
                        set16(p + 16, s.type);
                        set8(p + 18, s.storage_class);
                        set8(p + 19, aux_count);
                    } else {
                        // -1 / -2 are the 16-bit absolute / debug numbers; real sections are unsigned.
                        set16(p + 12, static_cast<std::uint32_t>(s.section_number) & 0xFFFFu);
                        set16(p + 14, s.type);
                        set8(p + 16, s.storage_class);
                        set8(p + 17, aux_count);
                    }
                    p += rec;
                    if (!s.aux.empty()) {
                        std::memcpy(p, s.aux.data(), s.aux.size());
                        p += s.aux.size();
                    }
                }
                if (strtab) {
                    set32(p, static_cast<std::uint32_t>(strtab_total));
                    if (!m.string_table.empty()) {
                        std::memcpy(p + 4, m.string_table.data(), m.string_table.size());
                    }
                }
                break;
            }
        }
    }
    if (!m.trailing.empty()) {
        std::memcpy(base + cursor - m.trailing.size(), m.trailing.data(), m.trailing.size());
    }
    CML_LOG(Trace, "coff: wrote %s object, %zu sections, %llu symbol records, %zu bytes", m.is_bigobj ? "bigobj" : "regular",
            m.sections.size(), static_cast<unsigned long long>(sym_records), out.size());
    return out;
}

Result<ByteVector> rewrite(const Object& object) {
    auto model = ObjectModel::from_object(object);
    if (!model) {
        return model.error();
    }
    return write(*model);
}

}  // namespace cml::coff
