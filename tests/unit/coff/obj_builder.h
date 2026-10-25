#pragma once

// A tiny COFF object builder for tests. It is deliberately independent of cml_coff (and is not the
// WP-06 writer): it only lays bytes out so the reader can be exercised on hand-made input,
// including structurally odd or malformed objects.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace cml_test {

using Bytes = std::vector<std::uint8_t>;

inline void put16(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
inline void put32(Bytes& b, std::uint32_t v) {
    put16(b, v & 0xFFFFu);
    put16(b, v >> 16);
}
inline void set32(Bytes& b, std::size_t off, std::uint32_t v) {
    b[off] = static_cast<std::uint8_t>(v);
    b[off + 1] = static_cast<std::uint8_t>(v >> 8);
    b[off + 2] = static_cast<std::uint8_t>(v >> 16);
    b[off + 3] = static_cast<std::uint8_t>(v >> 24);
}
inline void set16(Bytes& b, std::size_t off, std::uint32_t v) {
    b[off] = static_cast<std::uint8_t>(v);
    b[off + 1] = static_cast<std::uint8_t>(v >> 8);
}

inline std::span<const std::byte> as_span(const Bytes& b) {
    return std::as_bytes(std::span<const std::uint8_t>(b.data(), b.size()));
}

struct RelocSpec {
    std::uint32_t va = 0;
    std::uint32_t symbol = 0;
    std::uint16_t type = 0;
};

struct SectionSpec {
    std::string name;
    std::uint32_t characteristics = 0;
    Bytes data;                   // file data
    std::uint32_t bss_size = 0;   // when data is empty: SizeOfRawData with no file pointer
    std::vector<RelocSpec> relocs;
    bool extended_relocs = false;  // force the LNK_NRELOC_OVFL encoding
};

struct SymbolSpec {
    std::string name;
    std::uint32_t value = 0;
    std::int32_t section = 0;
    std::uint16_t type = 0;
    std::uint8_t storage_class = 2;  // External
    std::vector<Bytes> aux;          // each padded / truncated to the record size
};

struct ObjSpec {
    bool bigobj = false;
    std::uint16_t machine = 0x8664;
    std::uint16_t bigobj_version = 2;
    std::vector<SectionSpec> sections;
    std::vector<SymbolSpec> symbols;
};

struct Built {
    Bytes bytes;
    std::size_t header_size = 0;
    std::size_t section_table_off = 0;
    std::size_t symtab_off = 0;
    std::size_t strtab_off = 0;
    std::size_t strtab_size = 0;
    std::size_t record_size = 0;
    std::vector<std::size_t> data_off;
    std::vector<std::size_t> reloc_off;
    std::vector<std::size_t> symbol_off;  // byte offset of each primary symbol (spec order)
};

constexpr std::array<std::uint8_t, 16> kBigobjClassId = {0xC7, 0xA1, 0xBA, 0xD1, 0xEE, 0xBA, 0xA9, 0x4B,
                                                         0xAF, 0x20, 0xFA, 0xF6, 0x6A, 0xA4, 0xDC, 0xB8};

inline Bytes aux_section_def(bool big, std::uint32_t length, std::uint32_t nreloc, std::uint32_t nline,
                             std::uint32_t checksum, std::uint32_t number, std::uint8_t selection) {
    Bytes a;
    put32(a, length);
    put16(a, nreloc);
    put16(a, nline);
    put32(a, checksum);
    put16(a, number & 0xFFFFu);
    a.push_back(selection);
    a.push_back(0);
    put16(a, big ? (number >> 16) : 0u);
    put16(a, 0);
    return a;
}

inline Bytes aux_weak(std::uint32_t tag, std::uint32_t characteristics) {
    Bytes a;
    put32(a, tag);
    put32(a, characteristics);
    return a;
}

inline Bytes aux_function(std::uint32_t tag, std::uint32_t total_size, std::uint32_t ptr_line, std::uint32_t ptr_next) {
    Bytes a;
    put32(a, tag);
    put32(a, total_size);
    put32(a, ptr_line);
    put32(a, ptr_next);
    return a;
}

inline Bytes aux_file(const std::string& name) {
    return Bytes(name.begin(), name.end());
}

inline Built build_object(const ObjSpec& spec) {
    Built out;
    Bytes& b = out.bytes;
    const std::size_t nsec = spec.sections.size();
    out.header_size = spec.bigobj ? 56 : 20;
    out.record_size = spec.bigobj ? 20 : 18;
    out.section_table_off = out.header_size;

    Bytes strtab(4, 0);  // size field patched at the end
    const auto add_string = [&](const std::string& s) {
        const std::size_t off = strtab.size();
        strtab.insert(strtab.end(), s.begin(), s.end());
        strtab.push_back(0);
        return static_cast<std::uint32_t>(off);
    };

    // Header placeholder + section table placeholder.
    b.assign(out.header_size + nsec * 40, 0);

    // Section data and relocations.
    std::vector<std::uint32_t> data_ptr(nsec, 0);
    std::vector<std::uint32_t> reloc_ptr(nsec, 0);
    std::vector<std::uint32_t> reloc_count_field(nsec, 0);
    std::vector<std::uint32_t> chars(nsec, 0);
    for (std::size_t i = 0; i < nsec; ++i) {
        const SectionSpec& s = spec.sections[i];
        chars[i] = s.characteristics;
        out.data_off.push_back(0);
        out.reloc_off.push_back(0);
        if (!s.data.empty()) {
            data_ptr[i] = static_cast<std::uint32_t>(b.size());
            out.data_off[i] = b.size();
            b.insert(b.end(), s.data.begin(), s.data.end());
        }
        if (!s.relocs.empty() || s.extended_relocs) {
            const bool ext = s.extended_relocs || s.relocs.size() > 0xFFFF;
            reloc_ptr[i] = static_cast<std::uint32_t>(b.size());
            out.reloc_off[i] = b.size();
            if (ext) {
                chars[i] |= 0x01000000u;
                reloc_count_field[i] = 0xFFFFu;
                put32(b, static_cast<std::uint32_t>(s.relocs.size() + 1));
                put32(b, 0);
                put16(b, 0);
            } else {
                reloc_count_field[i] = static_cast<std::uint32_t>(s.relocs.size());
            }
            for (const RelocSpec& r : s.relocs) {
                put32(b, r.va);
                put32(b, r.symbol);
                put16(b, r.type);
            }
        }
    }

    // Section names that need the string table must be added before we know the final layout, but
    // the string table is last, so offsets are known now.
    std::vector<std::array<char, 8>> raw_names(nsec);
    for (std::size_t i = 0; i < nsec; ++i) {
        raw_names[i].fill('\0');
        const std::string& n = spec.sections[i].name;
        if (n.size() <= 8) {
            std::memcpy(raw_names[i].data(), n.data(), n.size());
        } else {
            const std::string ref = "/" + std::to_string(add_string(n));
            std::memcpy(raw_names[i].data(), ref.data(), ref.size());
        }
    }

    // Symbol table.
    out.symtab_off = b.size();
    std::uint32_t symbol_records = 0;
    for (const SymbolSpec& s : spec.symbols) {
        out.symbol_off.push_back(b.size());
        std::array<std::uint8_t, 8> raw{};
        if (s.name.size() <= 8) {
            std::memcpy(raw.data(), s.name.data(), s.name.size());
        } else {
            const std::uint32_t off = add_string(s.name);
            raw[4] = static_cast<std::uint8_t>(off);
            raw[5] = static_cast<std::uint8_t>(off >> 8);
            raw[6] = static_cast<std::uint8_t>(off >> 16);
            raw[7] = static_cast<std::uint8_t>(off >> 24);
        }
        b.insert(b.end(), raw.begin(), raw.end());
        put32(b, s.value);
        if (spec.bigobj) {
            put32(b, static_cast<std::uint32_t>(s.section));
        } else {
            put16(b, static_cast<std::uint32_t>(s.section) & 0xFFFFu);
        }
        put16(b, s.type);
        b.push_back(s.storage_class);
        b.push_back(static_cast<std::uint8_t>(s.aux.size()));
        ++symbol_records;
        for (const Bytes& a : s.aux) {
            Bytes rec = a;
            rec.resize(out.record_size, 0);
            b.insert(b.end(), rec.begin(), rec.end());
            ++symbol_records;
        }
    }
    out.strtab_off = b.size();
    set32(strtab, 0, static_cast<std::uint32_t>(strtab.size()));
    out.strtab_size = strtab.size();
    b.insert(b.end(), strtab.begin(), strtab.end());

    // Header.
    if (spec.bigobj) {
        set16(b, 0, 0);
        set16(b, 2, 0xFFFF);
        set16(b, 4, spec.bigobj_version);
        set16(b, 6, spec.machine);
        set32(b, 8, 0x12345678);
        std::memcpy(b.data() + 12, kBigobjClassId.data(), 16);
        set32(b, 28, 0);
        set32(b, 32, 0);
        set32(b, 36, 0);
        set32(b, 40, 0);
        set32(b, 44, static_cast<std::uint32_t>(nsec));
        set32(b, 48, static_cast<std::uint32_t>(out.symtab_off));
        set32(b, 52, symbol_records);
    } else {
        set16(b, 0, spec.machine);
        set16(b, 2, static_cast<std::uint32_t>(nsec));
        set32(b, 4, 0x12345678);
        set32(b, 8, static_cast<std::uint32_t>(out.symtab_off));
        set32(b, 12, symbol_records);
        set16(b, 16, 0);
        set16(b, 18, 0);
    }

    // Section table.
    for (std::size_t i = 0; i < nsec; ++i) {
        const SectionSpec& s = spec.sections[i];
        const std::size_t o = out.section_table_off + i * 40;
        std::memcpy(b.data() + o, raw_names[i].data(), 8);
        set32(b, o + 8, 0);
        set32(b, o + 12, 0);
        set32(b, o + 16, s.data.empty() ? s.bss_size : static_cast<std::uint32_t>(s.data.size()));
        set32(b, o + 20, data_ptr[i]);
        set32(b, o + 24, reloc_ptr[i]);
        set32(b, o + 28, 0);
        set16(b, o + 32, reloc_count_field[i]);
        set16(b, o + 34, 0);
        set32(b, o + 36, chars[i]);
    }
    return out;
}

}  // namespace cml_test
