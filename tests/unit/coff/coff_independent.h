#pragma once

// An independent, deliberately naive COFF parse used as an oracle for cml_coff: it shares no code
// with the library (packed structs read with memcpy, its own string-table logic) and records only
// raw facts. compare_with_reader() re-reads every field through the cml_coff API and reports the
// first disagreement.
//
// Also home of the WP-06 hook: once the COFF writer exists it installs itself in
// writer_roundtrip_hook() and test_coff_roundtrip.cpp then demands byte-identity for every object
// the tests know about.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cml/coff/coff.h"

namespace cml_test {

#pragma pack(push, 1)
struct RawFileHeader {
    std::uint16_t machine;
    std::uint16_t sections;
    std::uint32_t timestamp;
    std::uint32_t symtab;
    std::uint32_t nsyms;
    std::uint16_t opt;
    std::uint16_t chars;
};
struct RawBigHeader {
    std::uint16_t sig1;
    std::uint16_t sig2;
    std::uint16_t version;
    std::uint16_t machine;
    std::uint32_t timestamp;
    std::uint8_t classid[16];
    std::uint32_t size_of_data;
    std::uint32_t flags;
    std::uint32_t meta_size;
    std::uint32_t meta_off;
    std::uint32_t sections;
    std::uint32_t symtab;
    std::uint32_t nsyms;
};
struct RawSection {
    char name[8];
    std::uint32_t vsize;
    std::uint32_t vaddr;
    std::uint32_t rawsize;
    std::uint32_t rawptr;
    std::uint32_t relptr;
    std::uint32_t lineptr;
    std::uint16_t nreloc;
    std::uint16_t nline;
    std::uint32_t chars;
};
struct RawReloc {
    std::uint32_t va;
    std::uint32_t sym;
    std::uint16_t type;
};
struct RawSym16 {
    std::uint8_t name[8];
    std::uint32_t value;
    std::int16_t section;
    std::uint16_t type;
    std::uint8_t cls;
    std::uint8_t naux;
};
struct RawSym32 {
    std::uint8_t name[8];
    std::uint32_t value;
    std::int32_t section;
    std::uint16_t type;
    std::uint8_t cls;
    std::uint8_t naux;
};
#pragma pack(pop)

struct IndSection {
    std::string name;
    std::uint32_t vsize = 0, vaddr = 0, rawsize = 0, rawptr = 0, relptr = 0, lineptr = 0;
    std::uint16_t raw_nreloc = 0, nline = 0;
    std::uint32_t chars = 0;
    std::uint32_t nreloc = 0;
    std::vector<std::array<std::uint32_t, 3>> relocs;  // va, symbol, type
};

struct IndSymbol {
    std::uint32_t index = 0;
    std::string name;
    std::uint32_t value = 0;
    std::int32_t section = 0;
    std::uint16_t type = 0;
    std::uint8_t cls = 0;
    std::uint8_t naux = 0;
};

struct IndObject {
    bool ok = false;
    std::string error;
    bool big = false;
    std::uint16_t machine = 0;
    std::uint32_t symtab = 0, nsyms = 0;
    std::vector<IndSection> sections;
    std::vector<IndSymbol> symbols;
};

template <class T>
inline bool take(const std::vector<std::uint8_t>& f, std::uint64_t off, T* out) {
    if (off > f.size() || sizeof(T) > f.size() - off) {
        return false;
    }
    std::memcpy(out, f.data() + off, sizeof(T));
    return true;
}

inline std::string cstr_at(const std::vector<std::uint8_t>& f, std::uint64_t off, std::uint64_t limit) {
    std::string s;
    while (off < limit && off < f.size() && f[static_cast<std::size_t>(off)] != 0) {
        s.push_back(static_cast<char>(f[static_cast<std::size_t>(off)]));
        ++off;
    }
    return s;
}

inline IndObject independent_parse(const std::vector<std::uint8_t>& f) {
    IndObject o;
    std::uint64_t sec_off = 0;
    std::uint32_t nsec = 0;
    RawFileHeader rh{};
    RawBigHeader bh{};
    if (take(f, 0, &rh) && !(rh.machine == 0 && rh.sections == 0xFFFF)) {
        o.machine = rh.machine;
        nsec = rh.sections;
        o.symtab = rh.symtab;
        o.nsyms = rh.nsyms;
        sec_off = sizeof(RawFileHeader) + rh.opt;
    } else if (take(f, 0, &bh) && bh.sig1 == 0 && bh.sig2 == 0xFFFF) {
        o.big = true;
        o.machine = bh.machine;
        nsec = bh.sections;
        o.symtab = bh.symtab;
        o.nsyms = bh.nsyms;
        sec_off = sizeof(RawBigHeader);
    } else {
        o.error = "header";
        return o;
    }
    const std::size_t sym_size = o.big ? sizeof(RawSym32) : sizeof(RawSym16);
    const std::uint64_t strtab = static_cast<std::uint64_t>(o.symtab) + static_cast<std::uint64_t>(o.nsyms) * sym_size;
    std::uint32_t strsize = 0;
    if (o.symtab != 0 && !take(f, strtab, &strsize)) {
        strsize = 0;
    }
    const std::uint64_t strlimit = strtab + strsize;

    for (std::uint32_t i = 0; i < nsec; ++i) {
        RawSection rs{};
        if (!take(f, sec_off + static_cast<std::uint64_t>(i) * sizeof(RawSection), &rs)) {
            o.error = "section table";
            return o;
        }
        IndSection s;
        char name8[9] = {};
        std::memcpy(name8, rs.name, 8);
        s.name = name8;
        if (s.name.size() > 1 && s.name[0] == '/' && s.name[1] != '/') {
            s.name = cstr_at(f, strtab + std::strtoull(s.name.c_str() + 1, nullptr, 10), strlimit);
        }
        s.vsize = rs.vsize;
        s.vaddr = rs.vaddr;
        s.rawsize = rs.rawsize;
        s.rawptr = rs.rawptr;
        s.relptr = rs.relptr;
        s.lineptr = rs.lineptr;
        s.raw_nreloc = rs.nreloc;
        s.nline = rs.nline;
        s.chars = rs.chars;
        std::uint32_t skip = 0;
        s.nreloc = rs.nreloc;
        if ((rs.chars & 0x01000000u) != 0 && rs.nreloc == 0xFFFF) {
            RawReloc ov{};
            if (!take(f, rs.relptr, &ov)) {
                o.error = "ovfl";
                return o;
            }
            s.nreloc = ov.va - 1;
            skip = 1;
        }
        for (std::uint32_t k = 0; k < s.nreloc; ++k) {
            RawReloc rr{};
            if (!take(f, rs.relptr + static_cast<std::uint64_t>(k + skip) * 10, &rr)) {
                o.error = "relocs";
                return o;
            }
            s.relocs.push_back({rr.va, rr.sym, rr.type});
        }
        o.sections.push_back(std::move(s));
    }

    std::uint32_t i = 0;
    while (i < o.nsyms) {
        const std::uint64_t at = static_cast<std::uint64_t>(o.symtab) + static_cast<std::uint64_t>(i) * sym_size;
        IndSymbol s;
        s.index = i;
        std::uint8_t name8[8];
        if (o.big) {
            RawSym32 r{};
            if (!take(f, at, &r)) { o.error = "symbol"; return o; }
            std::memcpy(name8, r.name, 8);
            s.value = r.value; s.section = r.section; s.type = r.type; s.cls = r.cls; s.naux = r.naux;
        } else {
            RawSym16 r{};
            if (!take(f, at, &r)) { o.error = "symbol"; return o; }
            std::memcpy(name8, r.name, 8);
            s.value = r.value; s.section = r.section; s.type = r.type; s.cls = r.cls; s.naux = r.naux;
        }
        std::uint32_t zero = 0, off = 0;
        std::memcpy(&zero, name8, 4);
        std::memcpy(&off, name8 + 4, 4);
        if (zero == 0) {
            s.name = cstr_at(f, strtab + off, strlimit);
        } else {
            char n9[9] = {};
            std::memcpy(n9, name8, 8);
            s.name = n9;
        }
        o.symbols.push_back(std::move(s));
        i += 1u + o.symbols.back().naux;
    }
    o.ok = true;
    return o;
}

// Returns an empty string when cml_coff and the oracle agree on every field.
inline std::string compare_with_reader(const IndObject& ind, const cml::coff::Object& obj) {
    using namespace cml::coff;
    const auto fail = [](const std::string& what) { return what; };
    if (!ind.ok) return fail("oracle failed: " + ind.error);
    if (ind.big != obj.is_bigobj()) return fail("bigobj flag");
    if (ind.machine != obj.header().machine) return fail("machine");
    if (ind.symtab != obj.header().symbol_table_offset) return fail("symtab offset");
    if (ind.nsyms != obj.header().symbol_count) return fail("symbol count");
    if (ind.sections.size() != obj.sections().size()) return fail("section count");
    for (std::size_t i = 0; i < ind.sections.size(); ++i) {
        const IndSection& a = ind.sections[i];
        const Section& b = obj.sections()[i];
        const std::string at = "section " + std::to_string(i + 1) + " ";
        if (b.index != i + 1) return fail(at + "index");
        if (a.name != b.name) return fail(at + "name '" + a.name + "' vs '" + std::string(b.name) + "'");
        if (a.vsize != b.virtual_size) return fail(at + "virtual size");
        if (a.vaddr != b.virtual_address) return fail(at + "virtual address");
        if (a.rawsize != b.size_of_raw_data) return fail(at + "raw size");
        if (a.rawptr != b.pointer_to_raw_data) return fail(at + "raw pointer");
        if (a.relptr != b.pointer_to_relocations) return fail(at + "reloc pointer");
        if (a.lineptr != b.pointer_to_linenumbers) return fail(at + "line pointer");
        if (a.raw_nreloc != b.raw_relocation_count) return fail(at + "raw reloc count");
        if (a.nline != b.linenumber_count) return fail(at + "line count");
        if (a.chars != b.characteristics) return fail(at + "characteristics");
        if (a.nreloc != b.relocation_count) return fail(at + "effective reloc count");
        if (b.relocation_bytes.size() != static_cast<std::size_t>(a.nreloc) * 10u) return fail(at + "reloc bytes");
        const std::vector<Relocation> rel = obj.relocations(b);
        if (rel.size() != a.relocs.size()) return fail(at + "relocation vector size");
        for (std::size_t k = 0; k < rel.size(); ++k) {
            if (rel[k].virtual_address != a.relocs[k][0] || rel[k].symbol_index != a.relocs[k][1] ||
                rel[k].raw_type != a.relocs[k][2]) {
                return fail(at + "relocation " + std::to_string(k));
            }
        }
        const bool has_file_data = a.rawptr != 0 && a.rawsize != 0;
        if (b.data.size() != (has_file_data ? a.rawsize : 0u)) return fail(at + "data size");
    }
    if (ind.symbols.size() != obj.symbols().size()) return fail("primary symbol count");
    for (std::size_t i = 0; i < ind.symbols.size(); ++i) {
        const IndSymbol& a = ind.symbols[i];
        const Symbol& b = obj.symbols()[i];
        const std::string at = "symbol " + std::to_string(a.index) + " ";
        if (a.index != b.index) return fail(at + "index");
        if (a.name != b.name) return fail(at + "name '" + a.name + "' vs '" + std::string(b.name) + "'");
        if (a.value != b.value) return fail(at + "value");
        if (a.section != b.section_number) return fail(at + "section");
        if (a.type != b.type) return fail(at + "type");
        if (a.cls != static_cast<std::uint8_t>(b.storage_class)) return fail(at + "class");
        if (a.naux != b.aux_count) return fail(at + "aux count");
        if (b.aux.size() != static_cast<std::size_t>(a.naux) * obj.header().symbol_record_size) return fail(at + "aux bytes");
        if (obj.symbol_at(a.index) != &b) return fail(at + "symbol_at");
        for (std::uint32_t k = 1; k <= a.naux; ++k) {
            if (obj.symbol_at(a.index + k) != nullptr) return fail(at + "aux slot resolves to a symbol");
        }
    }
    return std::string();
}

// ---- WP-06 hook -------------------------------------------------------------------------------
//
// A rewriter takes a parsed object and must emit the byte-identical file. WP-06 sets this pointer
// from a static initialiser in its test TU; until then it stays null and the round-trip test only
// validates the comparison helper itself.
using ObjectRewriter = std::vector<std::uint8_t> (*)(const cml::coff::Object&);
inline ObjectRewriter& writer_roundtrip_hook() {
    static ObjectRewriter hook = nullptr;
    return hook;
}

// Empty string when identical; otherwise a description of the first difference.
inline std::string first_byte_difference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            return "first difference at offset " + std::to_string(i);
        }
    }
    if (a.size() != b.size()) {
        return "sizes differ: " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
    }
    return std::string();
}

}  // namespace cml_test
