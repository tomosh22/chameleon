#include "cml/coff/coff.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

#include "cml/util/log.h"

namespace cml::coff {

namespace {

constexpr std::size_t kRegularHeaderSize = 20;
constexpr std::size_t kBigobjHeaderSize = 56;
constexpr std::size_t kSectionHeaderSize = 40;
constexpr std::size_t kRelocationSize = 10;
constexpr std::size_t kRegularSymbolSize = 18;
constexpr std::size_t kBigobjSymbolSize = 20;
constexpr std::uint32_t kNoPos = 0xFFFFFFFFu;
constexpr std::uint32_t kMaxRegularSections = 65279;

// {D1BA02C1... } ANON_OBJECT_HEADER_BIGOBJ class id, as stored in the file.
constexpr std::array<std::uint8_t, 16> kBigobjClassId = {0xC7, 0xA1, 0xBA, 0xD1, 0xEE, 0xBA, 0xA9, 0x4B,
                                                         0xAF, 0x20, 0xFA, 0xF6, 0x6A, 0xA4, 0xDC, 0xB8};

std::uint32_t rd8(ByteSpan b, std::size_t o) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[o]));
}
std::uint32_t rd16(ByteSpan b, std::size_t o) noexcept {
    return rd8(b, o) | (rd8(b, o + 1) << 8);
}
std::uint32_t rd32(ByteSpan b, std::size_t o) noexcept {
    return rd16(b, o) | (rd16(b, o + 2) << 16);
}

// off + len <= total, computed without overflow.
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

// A NUL-terminated string inside the string table. Offsets 0..3 address the size field itself and
// are invalid.
bool string_table_at(ByteSpan table, std::uint64_t off, std::string_view* out) noexcept {
    if (off < 4 || off >= table.size()) {
        return false;
    }
    const char* base = reinterpret_cast<const char*>(table.data());
    const std::size_t remaining = table.size() - static_cast<std::size_t>(off);
    const void* nul = std::memchr(base + off, 0, remaining);
    if (nul == nullptr) {
        return false;
    }
    const std::size_t len = static_cast<std::size_t>(static_cast<const char*>(nul) - (base + off));
    *out = std::string_view(base + off, len);
    return true;
}

int base64_digit(char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::string_view trim_at_nul(const char* p, std::size_t max) noexcept {
    std::size_t n = 0;
    while (n < max && p[n] != '\0') {
        ++n;
    }
    return std::string_view(p, n);
}

char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Relocation names
// ---------------------------------------------------------------------------------------------

std::string_view reloc_type_name(RelocType type) noexcept {
    switch (type) {
        case RelocType::Absolute: return "IMAGE_REL_AMD64_ABSOLUTE";
        case RelocType::Addr64: return "IMAGE_REL_AMD64_ADDR64";
        case RelocType::Addr32: return "IMAGE_REL_AMD64_ADDR32";
        case RelocType::Addr32Nb: return "IMAGE_REL_AMD64_ADDR32NB";
        case RelocType::Rel32: return "IMAGE_REL_AMD64_REL32";
        case RelocType::Rel32_1: return "IMAGE_REL_AMD64_REL32_1";
        case RelocType::Rel32_2: return "IMAGE_REL_AMD64_REL32_2";
        case RelocType::Rel32_3: return "IMAGE_REL_AMD64_REL32_3";
        case RelocType::Rel32_4: return "IMAGE_REL_AMD64_REL32_4";
        case RelocType::Rel32_5: return "IMAGE_REL_AMD64_REL32_5";
        case RelocType::Section: return "IMAGE_REL_AMD64_SECTION";
        case RelocType::SecRel: return "IMAGE_REL_AMD64_SECREL";
        case RelocType::SecRel7: return "IMAGE_REL_AMD64_SECREL7";
        case RelocType::Token: return "IMAGE_REL_AMD64_TOKEN";
        case RelocType::SRel32: return "IMAGE_REL_AMD64_SREL32";
        case RelocType::Pair: return "IMAGE_REL_AMD64_PAIR";
        case RelocType::SSpan32: return "IMAGE_REL_AMD64_SSPAN32";
    }
    return "IMAGE_REL_AMD64_UNKNOWN";
}

bool is_rel32_family(RelocType type) noexcept {
    switch (type) {
        case RelocType::Rel32:
        case RelocType::Rel32_1:
        case RelocType::Rel32_2:
        case RelocType::Rel32_3:
        case RelocType::Rel32_4:
        case RelocType::Rel32_5:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------------------------
// Directives
// ---------------------------------------------------------------------------------------------

std::vector<Directive> parse_directives(std::string_view text) {
    std::vector<Directive> out;
    std::size_t i = 0;
    const std::size_t n = text.size();
    if (n >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        i = 3;
    }
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0'; };
    while (i < n) {
        while (i < n && is_space(text[i])) {
            ++i;
        }
        if (i >= n) {
            break;
        }
        const std::size_t start = i;
        bool quoted = false;
        while (i < n && (quoted || !is_space(text[i]))) {
            if (text[i] == '"') {
                quoted = !quoted;
            }
            ++i;
        }
        const std::string_view token = text.substr(start, i - start);

        Directive d;
        d.text = std::string(token);
        std::string_view body = token;
        if (!body.empty() && (body.front() == '/' || body.front() == '-')) {
            body.remove_prefix(1);
        }
        const std::size_t colon = body.find(':');
        std::string_view name = colon == std::string_view::npos ? body : body.substr(0, colon);
        std::string_view value = colon == std::string_view::npos ? std::string_view() : body.substr(colon + 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        d.name.reserve(name.size());
        for (char c : name) {
            d.name.push_back(ascii_lower(c));
        }
        d.value = std::string(value);

        if (d.name == "defaultlib") d.kind = DirectiveKind::DefaultLib;
        else if (d.name == "nodefaultlib") d.kind = DirectiveKind::NoDefaultLib;
        else if (d.name == "export") d.kind = DirectiveKind::Export;
        else if (d.name == "include") d.kind = DirectiveKind::Include;
        else if (d.name == "failifmismatch") d.kind = DirectiveKind::FailIfMismatch;
        else if (d.name == "alternatename") d.kind = DirectiveKind::AlternateName;
        else if (d.name == "merge") d.kind = DirectiveKind::Merge;
        else if (d.name == "section") d.kind = DirectiveKind::Section;
        else if (d.name == "entry") d.kind = DirectiveKind::Entry;
        else if (d.name == "subsystem") d.kind = DirectiveKind::Subsystem;

        if (d.kind == DirectiveKind::AlternateName || d.kind == DirectiveKind::FailIfMismatch ||
            d.kind == DirectiveKind::Merge) {
            const std::size_t eq = d.value.find('=');
            if (eq != std::string::npos) {
                d.has_pair = true;
                d.lhs = d.value.substr(0, eq);
                d.rhs = d.value.substr(eq + 1);
            }
        }
        out.push_back(std::move(d));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Object::parse
// ---------------------------------------------------------------------------------------------

Result<Object> Object::parse(ByteSpan b) {
    Object o;
    o.bytes_ = b;
    ObjectHeader& h = o.header_;
    const std::size_t n = b.size();

    if (n < kRegularHeaderSize) {
        return make_diag(1201, format("COFF object is %zu bytes, too small for the 20-byte COFF header", n),
                         "Check that the file is a complete .obj produced by the compiler.");
    }

    const std::uint32_t sig1 = rd16(b, 0);
    const std::uint32_t sig2 = rd16(b, 2);
    if (sig1 == 0 && sig2 == 0xFFFF) {
        if (n < kBigobjHeaderSize) {
            return make_diag(1201,
                             format("COFF /bigobj header is truncated: %zu bytes, 56 required", n),
                             "Check that the file is a complete .obj produced by the compiler.");
        }
        const std::uint32_t version = rd16(b, 4);
        if (version != 2) {
            return make_diag(1203,
                             format("anonymous COFF header has version %u; only /bigobj version 2 is supported",
                                    static_cast<unsigned>(version)),
                             "The file is not a /bigobj object (version 0 is a short import object); pass the "
                             "compiler's .obj, not an import library member.");
        }
        for (std::size_t i = 0; i < 16; ++i) {
            h.bigobj_class_id[i] = static_cast<std::uint8_t>(rd8(b, 12 + i));
        }
        if (h.bigobj_class_id != kBigobjClassId) {
            return make_diag(1204, "anonymous COFF header has an unrecognised class id (not ANON_OBJECT_HEADER_BIGOBJ)",
                             "Only regular COFF and /bigobj objects are supported; LTCG/bitcode objects are not.");
        }
        h.is_bigobj = true;
        h.bigobj_version = static_cast<std::uint16_t>(version);
        h.machine = static_cast<std::uint16_t>(rd16(b, 6));
        h.time_date_stamp = rd32(b, 8);
        h.bigobj_flags = rd32(b, 32);
        h.bigobj_meta_data_size = rd32(b, 36);
        h.bigobj_meta_data_offset = rd32(b, 40);
        h.section_count = rd32(b, 44);
        h.symbol_table_offset = rd32(b, 48);
        h.symbol_count = rd32(b, 52);
        h.header_size = kBigobjHeaderSize;
        h.symbol_record_size = kBigobjSymbolSize;
    } else {
        h.machine = static_cast<std::uint16_t>(rd16(b, 0));
        h.section_count = rd16(b, 2);
        h.time_date_stamp = rd32(b, 4);
        h.symbol_table_offset = rd32(b, 8);
        h.symbol_count = rd32(b, 12);
        h.optional_header_size = static_cast<std::uint16_t>(rd16(b, 16));
        h.characteristics = static_cast<std::uint16_t>(rd16(b, 18));
        h.header_size = kRegularHeaderSize;
        h.symbol_record_size = kRegularSymbolSize;
        if (h.section_count > kMaxRegularSections) {
            // As LLVM does: 0xFFFE / 0xFFFF are the debug / absolute section numbers, so a regular
            // object cannot have more than 65279 sections.
            return make_diag(1216,
                             format("regular COFF object declares %u sections; the limit is 65279 (larger objects need the /bigobj format)",
                                    static_cast<unsigned>(h.section_count)),
                             "The object is corrupt; rebuild it (compile with /bigobj if it genuinely needs this many sections).");
        }
    }
    h.section_table_offset = h.header_size + h.optional_header_size;
    o.header_bytes_ = b.subspan(0, h.header_size);

    // ---- section table range ----
    const std::uint64_t section_table_size = static_cast<std::uint64_t>(h.section_count) * kSectionHeaderSize;
    if (!in_range(n, h.section_table_offset, section_table_size)) {
        return make_diag(1202,
                         format("section table (%u sections at offset %zu) extends past the end of the %zu-byte object",
                                static_cast<unsigned>(h.section_count), h.section_table_offset, n),
                         "The object is truncated or corrupt; rebuild it.");
    }
    o.section_table_bytes_ = b.subspan(h.section_table_offset, static_cast<std::size_t>(section_table_size));

    // ---- symbol table + string table ranges ----
    const std::uint64_t symtab_size = static_cast<std::uint64_t>(h.symbol_count) * h.symbol_record_size;
    if (h.symbol_table_offset == 0) {
        if (h.symbol_count != 0) {
            return make_diag(1207,
                             format("object declares %u symbols but its symbol table offset is 0",
                                    static_cast<unsigned>(h.symbol_count)),
                             "The object is corrupt; rebuild it.");
        }
    } else {
        if (!in_range(n, h.symbol_table_offset, symtab_size)) {
            return make_diag(1207,
                             format("symbol table (%u records at offset %u) extends past the end of the %zu-byte object",
                                    static_cast<unsigned>(h.symbol_count), static_cast<unsigned>(h.symbol_table_offset), n),
                             "The object is truncated or corrupt; rebuild it.");
        }
        o.symbol_table_bytes_ = b.subspan(h.symbol_table_offset, static_cast<std::size_t>(symtab_size));
        const std::uint64_t strtab_off = static_cast<std::uint64_t>(h.symbol_table_offset) + symtab_size;
        const std::uint64_t left = strtab_off <= n ? n - strtab_off : 0;
        if (left != 0) {
            if (left < 4) {
                return make_diag(1208, format("string table at offset %llu is only %llu bytes; its size field needs 4",
                                              static_cast<unsigned long long>(strtab_off),
                                              static_cast<unsigned long long>(left)),
                                 "The object is truncated or corrupt; rebuild it.");
            }
            const std::uint32_t strtab_size = rd32(b, static_cast<std::size_t>(strtab_off));
            if (strtab_size < 4 || strtab_size > left) {
                return make_diag(1208,
                                 format("string table declares size %u but %llu bytes remain after offset %llu",
                                        static_cast<unsigned>(strtab_size), static_cast<unsigned long long>(left),
                                        static_cast<unsigned long long>(strtab_off)),
                                 "The object is truncated or corrupt; rebuild it.");
            }
            o.string_table_bytes_ = b.subspan(static_cast<std::size_t>(strtab_off), strtab_size);
        }
    }

    // ---- sections ----
    o.sections_.reserve(h.section_count);
    for (std::uint32_t i = 0; i < h.section_count; ++i) {
        const std::size_t off = h.section_table_offset + static_cast<std::size_t>(i) * kSectionHeaderSize;
        Section s;
        s.index = i + 1;
        s.header_bytes = b.subspan(off, kSectionHeaderSize);
        for (std::size_t k = 0; k < 8; ++k) {
            s.raw_name[k] = static_cast<char>(rd8(b, off + k));
        }
        s.virtual_size = rd32(b, off + 8);
        s.virtual_address = rd32(b, off + 12);
        s.size_of_raw_data = rd32(b, off + 16);
        s.pointer_to_raw_data = rd32(b, off + 20);
        s.pointer_to_relocations = rd32(b, off + 24);
        s.pointer_to_linenumbers = rd32(b, off + 28);
        s.raw_relocation_count = static_cast<std::uint16_t>(rd16(b, off + 32));
        s.linenumber_count = static_cast<std::uint16_t>(rd16(b, off + 34));
        s.characteristics = rd32(b, off + 36);

        // Name.
        if (s.raw_name[0] == '/') {
            std::uint64_t str_off = 0;
            bool ok = true;
            if (s.raw_name[1] == '/') {
                for (std::size_t k = 2; k < 8 && ok; ++k) {
                    const int d = base64_digit(s.raw_name[k]);
                    if (d < 0) {
                        ok = false;
                    } else {
                        str_off = (str_off << 6) | static_cast<std::uint64_t>(d);
                    }
                }
            } else {
                std::size_t digits = 0;
                for (std::size_t k = 1; k < 8 && s.raw_name[k] != '\0'; ++k) {
                    if (s.raw_name[k] < '0' || s.raw_name[k] > '9') {
                        ok = false;
                        break;
                    }
                    str_off = str_off * 10 + static_cast<std::uint64_t>(s.raw_name[k] - '0');
                    ++digits;
                }
                ok = ok && digits > 0;
            }
            std::string_view resolved;
            if (!ok || !string_table_at(o.string_table_bytes_, str_off, &resolved)) {
                return make_diag(1211,
                                 format("section %u has a long-name reference '%.8s' that does not resolve to a string-table entry",
                                        static_cast<unsigned>(s.index), s.raw_name.data()),
                                 "The object is corrupt; rebuild it.");
            }
            s.name = resolved;
        } else {
            // View the bytes in the input, not the copy in s.raw_name (s is copied into the vector).
            s.name = trim_at_nul(reinterpret_cast<const char*>(s.header_bytes.data()), 8);
        }

        // Raw data.
        // IMAGE_SCN_CNT_UNINITIALIZED_DATA sections have no file contents whatever PointerToRawData says.
        if ((s.characteristics & scn::CntUninitializedData) == 0 && s.pointer_to_raw_data != 0 && s.size_of_raw_data != 0) {
            if (!in_range(n, s.pointer_to_raw_data, s.size_of_raw_data)) {
                return make_diag(1205,
                                 format("section %u '%.*s' raw data (offset %u, size %u) extends past the end of the %zu-byte object",
                                        static_cast<unsigned>(s.index), static_cast<int>(s.name.size()), s.name.data(),
                                        static_cast<unsigned>(s.pointer_to_raw_data),
                                        static_cast<unsigned>(s.size_of_raw_data), n),
                                 "The object is truncated or corrupt; rebuild it.");
            }
            s.data = b.subspan(s.pointer_to_raw_data, s.size_of_raw_data);
        }

        // Relocations.
        s.has_extended_relocations = (s.characteristics & scn::LnkNrelocOvfl) != 0 && s.raw_relocation_count == 0xFFFF;
        std::uint64_t total_records = s.raw_relocation_count;
        if (s.has_extended_relocations) {
            if (!in_range(n, s.pointer_to_relocations, kRelocationSize) || s.pointer_to_relocations == 0) {
                return make_diag(1212,
                                 format("section %u '%.*s' is flagged LNK_NRELOC_OVFL but its overflow relocation record is outside the object",
                                        static_cast<unsigned>(s.index), static_cast<int>(s.name.size()), s.name.data()),
                                 "The object is truncated or corrupt; rebuild it.");
            }
            total_records = rd32(b, s.pointer_to_relocations);  // VirtualAddress of the first record
            if (total_records == 0) {
                return make_diag(1212,
                                 format("section %u '%.*s' has an extended relocation count of 0 (must include the count record itself)",
                                        static_cast<unsigned>(s.index), static_cast<int>(s.name.size()), s.name.data()),
                                 "The object is corrupt; rebuild it.");
            }
        }
        if (total_records != 0) {
            if (s.pointer_to_relocations == 0 || !in_range(n, s.pointer_to_relocations, total_records * kRelocationSize)) {
                return make_diag(1206,
                                 format("section %u '%.*s' declares %llu relocation records at offset %u which extend past the end of the %zu-byte object",
                                        static_cast<unsigned>(s.index), static_cast<int>(s.name.size()), s.name.data(),
                                        static_cast<unsigned long long>(total_records),
                                        static_cast<unsigned>(s.pointer_to_relocations), n),
                                 "The object is truncated or corrupt; rebuild it.");
            }
            s.relocation_table = b.subspan(s.pointer_to_relocations, static_cast<std::size_t>(total_records * kRelocationSize));
            if (s.has_extended_relocations) {
                s.relocation_count = static_cast<std::uint32_t>(total_records - 1);
                s.relocation_bytes = s.relocation_table.subspan(kRelocationSize);
            } else {
                s.relocation_count = static_cast<std::uint32_t>(total_records);
                s.relocation_bytes = s.relocation_table;
            }
        }
        o.sections_.push_back(s);
    }

    // ---- symbols ----
    o.slot_.assign(h.symbol_count, kNoSlot);
    o.symbols_.reserve(h.symbol_count);
    const std::size_t rec = h.symbol_record_size;
    std::uint32_t idx = 0;
    while (idx < h.symbol_count) {
        const std::size_t off = static_cast<std::size_t>(h.symbol_table_offset) + static_cast<std::size_t>(idx) * rec;
        Symbol s;
        s.index = idx;
        s.record = b.subspan(off, rec);
        for (std::size_t k = 0; k < 8; ++k) {
            s.raw_name[k] = static_cast<std::uint8_t>(rd8(b, off + k));
        }
        s.value = rd32(b, off + 8);
        if (h.is_bigobj) {
            s.section_number = static_cast<std::int32_t>(rd32(b, off + 12));
            s.type = static_cast<std::uint16_t>(rd16(b, off + 16));
            s.storage_class = static_cast<StorageClass>(rd8(b, off + 18));
            s.aux_count = static_cast<std::uint8_t>(rd8(b, off + 19));
        } else {
            // The spec calls this field signed, but cl, clang-cl and lld all use the whole unsigned range
            // for real sections (up to 65279; clang-cl emits regular objects with 50000+ sections);
            // only 0xFFFF (absolute) and 0xFFFE (debug) are special.
            const std::uint32_t raw_section = rd16(b, off + 12);
            s.section_number = raw_section == 0xFFFFu ? kSectionAbsolute
                               : raw_section == 0xFFFEu ? kSectionDebug
                                                        : static_cast<std::int32_t>(raw_section);
            s.type = static_cast<std::uint16_t>(rd16(b, off + 14));
            s.storage_class = static_cast<StorageClass>(rd8(b, off + 16));
            s.aux_count = static_cast<std::uint8_t>(rd8(b, off + 17));
        }

        if (static_cast<std::uint64_t>(idx) + 1 + s.aux_count > h.symbol_count) {
            return make_diag(1210,
                             format("symbol %u declares %u aux records but only %u records remain in the symbol table",
                                    static_cast<unsigned>(idx), static_cast<unsigned>(s.aux_count),
                                    static_cast<unsigned>(h.symbol_count - idx - 1)),
                             "The object is truncated or corrupt; rebuild it.");
        }
        s.aux = b.subspan(off + rec, static_cast<std::size_t>(s.aux_count) * rec);

        if (s.section_number > 0 && static_cast<std::uint32_t>(s.section_number) > h.section_count) {
            return make_diag(1213,
                             format("symbol %u refers to section %d but the object has only %u sections",
                                    static_cast<unsigned>(idx), static_cast<int>(s.section_number),
                                    static_cast<unsigned>(h.section_count)),
                             "The object is corrupt; rebuild it.");
        }
        if (s.section_number < kSectionDebug) {
            return make_diag(1213,
                             format("symbol %u has invalid section number %d", static_cast<unsigned>(idx),
                                    static_cast<int>(s.section_number)),
                             "The object is corrupt; rebuild it.");
        }

        // Name.
        if (rd32(s.record, 0) == 0) {
            const std::uint32_t str_off = rd32(s.record, 4);
            std::string_view resolved;
            if (!string_table_at(o.string_table_bytes_, str_off, &resolved)) {
                return make_diag(1209,
                                 format("symbol %u has string-table offset %u which is outside the %zu-byte string table or unterminated",
                                        static_cast<unsigned>(idx), static_cast<unsigned>(str_off),
                                        o.string_table_bytes_.size()),
                                 "The object is corrupt; rebuild it.");
            }
            s.name = resolved;
            s.name_in_string_table = true;
            s.name_offset = str_off;
        } else {
            s.name = trim_at_nul(reinterpret_cast<const char*>(s.record.data()), 8);
        }

        const std::uint32_t pos = static_cast<std::uint32_t>(o.symbols_.size());
        o.slot_[idx] = pos;
        const std::uint32_t next = idx + 1 + s.aux_count;
        o.symbols_.push_back(s);
        idx = next;
    }

    // ---- derived indices ----
    o.section_def_pos_.assign(static_cast<std::size_t>(h.section_count) + 1, kNoPos);
    o.leader_pos_.assign(static_cast<std::size_t>(h.section_count) + 1, kNoPos);
    for (std::uint32_t pos = 0; pos < o.symbols_.size(); ++pos) {
        const Symbol& s = o.symbols_[pos];
        if (s.section_number > 0) {
            const std::size_t sec = static_cast<std::size_t>(s.section_number);
            const bool is_def = o.section_definition(s).has_value();
            if (o.section_def_pos_[sec] == kNoPos) {
                if (is_def) {
                    o.section_def_pos_[sec] = pos;
                }
                // A symbol of this section that precedes its section-definition symbol is not the leader.
            } else if (o.leader_pos_[sec] == kNoPos && !is_def) {
                o.leader_pos_[sec] = pos;
            }
        }
        auto it = o.by_name_.find(s.name);
        if (it == o.by_name_.end()) {
            o.by_name_.emplace(s.name, pos);
        } else if (o.symbols_[it->second].storage_class == StorageClass::Static &&
                   (s.storage_class == StorageClass::External || s.storage_class == StorageClass::WeakExternal)) {
            it->second = pos;
        }
    }

    CML_LOG(Trace, "coff: parsed %s object, machine 0x%04x, %u sections, %u symbol records",
            h.is_bigobj ? "bigobj" : "regular", static_cast<unsigned>(h.machine), static_cast<unsigned>(h.section_count),
            static_cast<unsigned>(h.symbol_count));
    return o;
}

// ---------------------------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------------------------

const Section* Object::section(std::uint32_t index) const noexcept {
    if (index == 0 || index > sections_.size()) {
        return nullptr;
    }
    return &sections_[index - 1];
}

const Section* Object::find_section(std::string_view name) const noexcept {
    for (const Section& s : sections_) {
        if (s.name == name) {
            return &s;
        }
    }
    return nullptr;
}

const Section* Object::section_of(const Symbol& symbol) const noexcept {
    if (symbol.section_number <= 0) {
        return nullptr;
    }
    return section(static_cast<std::uint32_t>(symbol.section_number));
}

const Symbol* Object::symbol_at(std::uint32_t raw_index) const noexcept {
    if (raw_index >= slot_.size() || slot_[raw_index] == kNoSlot) {
        return nullptr;
    }
    return &symbols_[slot_[raw_index]];
}

const Symbol* Object::find_symbol(std::string_view name) const noexcept {
    const auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : &symbols_[it->second];
}

std::optional<SectionDefinitionAux> Object::section_definition(const Symbol& s) const noexcept {
    if (s.storage_class != StorageClass::Static || s.section_number <= 0 || s.aux_count < 1 || s.value != 0 ||
        s.is_function()) {
        return std::nullopt;
    }
    const ByteSpan a = s.aux.subspan(0, header_.symbol_record_size);
    SectionDefinitionAux d;
    d.length = rd32(a, 0);
    d.relocation_count = rd16(a, 4);
    d.linenumber_count = rd16(a, 6);
    d.checksum = rd32(a, 8);
    d.associated_section = rd16(a, 12);
    d.selection_raw = static_cast<std::uint8_t>(rd8(a, 14));
    d.selection = d.selection_raw <= 6 ? static_cast<ComdatSelection>(d.selection_raw) : ComdatSelection::None;
    if (header_.is_bigobj) {
        d.associated_section |= rd16(a, 16) << 16;
    }
    return d;
}

std::optional<FunctionDefinitionAux> Object::function_definition(const Symbol& s) const noexcept {
    if ((s.storage_class != StorageClass::External && s.storage_class != StorageClass::Static) || s.section_number <= 0 ||
        s.aux_count < 1 || !s.is_function()) {
        return std::nullopt;
    }
    const ByteSpan a = s.aux.subspan(0, header_.symbol_record_size);
    FunctionDefinitionAux d;
    d.tag_index = rd32(a, 0);
    d.total_size = rd32(a, 4);
    d.pointer_to_linenumber = rd32(a, 8);
    d.pointer_to_next_function = rd32(a, 12);
    return d;
}

std::optional<WeakExternalAux> Object::weak_external(const Symbol& s) const noexcept {
    if (s.storage_class != StorageClass::WeakExternal || s.aux_count < 1) {
        return std::nullopt;
    }
    const ByteSpan a = s.aux.subspan(0, header_.symbol_record_size);
    WeakExternalAux w;
    w.tag_index = rd32(a, 0);
    w.characteristics = rd32(a, 4);
    return w;
}

std::optional<std::string_view> Object::file_name(const Symbol& s) const noexcept {
    if (s.storage_class != StorageClass::File || s.aux_count < 1) {
        return std::nullopt;
    }
    return trim_at_nul(reinterpret_cast<const char*>(s.aux.data()), s.aux.size());
}

const Symbol* Object::weak_external_default(const Symbol& s) const noexcept {
    const auto w = weak_external(s);
    if (!w) {
        return nullptr;
    }
    return symbol_at(w->tag_index);
}

std::vector<Relocation> Object::relocations(const Section& sec) const {
    std::vector<Relocation> out;
    out.reserve(sec.relocation_count);
    const ByteSpan r = sec.relocation_bytes;
    for (std::size_t i = 0; i + kRelocationSize <= r.size(); i += kRelocationSize) {
        Relocation x;
        x.virtual_address = rd32(r, i);
        x.symbol_index = rd32(r, i + 4);
        x.raw_type = static_cast<std::uint16_t>(rd16(r, i + 8));
        x.type = static_cast<RelocType>(x.raw_type);
        out.push_back(x);
    }
    return out;
}

Result<std::vector<ResolvedRelocation>> Object::resolve_relocations(const Section& sec) const {
    std::vector<ResolvedRelocation> out;
    const std::vector<Relocation> raw = relocations(sec);
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const Relocation& r = raw[i];
        if (r.symbol_index >= header_.symbol_count) {
            return make_diag(1214,
                             format("relocation %zu of section %u '%.*s' targets symbol index %u but the object has %u symbol records",
                                    i, static_cast<unsigned>(sec.index), static_cast<int>(sec.name.size()),
                                    sec.name.data(), static_cast<unsigned>(r.symbol_index),
                                    static_cast<unsigned>(header_.symbol_count)),
                             "The object is corrupt; rebuild it.");
        }
        const Symbol* target = symbol_at(r.symbol_index);
        if (target == nullptr) {
            return make_diag(1215,
                             format("relocation %zu of section %u '%.*s' targets symbol index %u which is an auxiliary record, not a symbol",
                                    i, static_cast<unsigned>(sec.index), static_cast<int>(sec.name.size()),
                                    sec.name.data(), static_cast<unsigned>(r.symbol_index)),
                             "The object is corrupt; rebuild it.");
        }
        out.push_back(ResolvedRelocation{r, target});
    }
    return out;
}

std::optional<ComdatInfo> Object::comdat(const Section& sec) const noexcept {
    if (!sec.is_comdat() || sec.index >= section_def_pos_.size() || section_def_pos_[sec.index] == kNoPos) {
        return std::nullopt;
    }
    const Symbol& def_sym = symbols_[section_def_pos_[sec.index]];
    const auto def = section_definition(def_sym);
    if (!def) {
        return std::nullopt;
    }
    ComdatInfo info;
    info.selection = def->selection;
    info.section_symbol = &def_sym;
    if (def->selection == ComdatSelection::Associative) {
        info.associated_section = def->associated_section;
    } else if (leader_pos_[sec.index] != kNoPos) {
        info.leader = &symbols_[leader_pos_[sec.index]];
    }
    return info;
}

std::vector<const Section*> Object::associative_children(const Section& parent) const {
    std::vector<const Section*> out;
    for (const Section& s : sections_) {
        const auto c = comdat(s);
        if (c && c->selection == ComdatSelection::Associative && c->associated_section == parent.index) {
            out.push_back(&s);
        }
    }
    return out;
}

std::vector<Directive> Object::directives() const {
    std::vector<Directive> out;
    for (const Section& s : sections_) {
        if (s.name != ".drectve" || s.data.empty()) {
            continue;
        }
        const std::string_view text(reinterpret_cast<const char*>(s.data.data()), s.data.size());
        std::vector<Directive> part = parse_directives(text);
        for (Directive& d : part) {
            out.push_back(std::move(d));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------

Result<std::vector<std::byte>> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        return make_diag(1200, format("cannot open '%s' for reading", path.c_str()),
                         "Check that the file exists and is readable.");
    }
    const std::streamoff size = f.tellg();
    if (size < 0) {
        return make_diag(1200, format("cannot determine the size of '%s'", path.c_str()),
                         "Check that the path names a regular file.");
    }
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    f.seekg(0, std::ios::beg);
    if (size > 0) {
        f.read(reinterpret_cast<char*>(data.data()), size);
    }
    if (!f) {
        return make_diag(1200, format("failed while reading '%s'", path.c_str()),
                         "Check that the file is not locked or truncated.");
    }
    return data;
}

Result<OwnedObject> load_object_file(const std::string& path) {
    Result<std::vector<std::byte>> bytes = read_file(path);
    if (!bytes) {
        return bytes.error();
    }
    std::vector<std::byte> data = std::move(bytes).value();
    // The vector's heap block does not move when the vector is moved, so views stay valid.
    Result<Object> object = Object::parse(ByteSpan(data.data(), data.size()));
    if (!object) {
        Diag d = object.error();
        d.message = path + ": " + d.message;
        return d;
    }
    return OwnedObject{std::move(data), std::move(object).value()};
}

}  // namespace cml::coff
