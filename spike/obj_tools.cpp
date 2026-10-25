#include "obj_tools.h"

#include <cstring>
#include <string_view>

#include "common.h"

namespace spike {

namespace {

constexpr std::size_t kSymbolSize = 18;
constexpr std::size_t kSectionHeaderSize = 40;
constexpr std::size_t kRelocSize = 10;

void put8(std::vector<std::uint8_t>& b, std::uint8_t v) { b.push_back(v); }
void put16(std::vector<std::uint8_t>& b, std::uint16_t v)
{
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(std::vector<std::uint8_t>& b, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}
void patch32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    }
}

// Appends one 18-byte symbol record. Names longer than 8 bytes go to the string table `strtab`
// (which includes its 4-byte size field at the front; fixed up by the caller at the end).
void put_symbol(std::vector<std::uint8_t>& out, std::vector<std::uint8_t>& strtab, const std::string& name,
                std::uint32_t value, std::uint16_t section, std::uint16_t type, std::uint8_t storage_class)
{
    if (name.size() <= 8) {
        std::uint8_t field[8] = {};
        std::memcpy(field, name.data(), name.size());
        out.insert(out.end(), field, field + 8);
    } else {
        const std::uint32_t off = static_cast<std::uint32_t>(strtab.size());
        strtab.insert(strtab.end(), name.begin(), name.end());
        strtab.push_back(0);
        put32(out, 0);
        put32(out, off);
    }
    put32(out, value);
    put16(out, section);
    put16(out, type);
    put8(out, storage_class);
    put8(out, 0);  // no aux records
}

bool starts_with(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

bool is_anchorable(const cml::coff::Section& s)
{
    if ((s.characteristics & (cml::coff::scn::LnkInfo | cml::coff::scn::LnkRemove)) != 0) {
        return false;
    }
    static constexpr std::string_view kSkipped[] = {".debug", ".drectve", ".sxdata", ".gfids", ".giats",
                                                    ".gljmp", ".gehcont", ".voltbl"};
    for (std::string_view prefix : kSkipped) {
        if (starts_with(s.name, prefix)) {
            return false;
        }
    }
    return true;
}

std::string anchor_name(std::uint32_t n) { return "__cml_anchor_" + std::to_string(n); }
std::string static_anchor_name(std::uint32_t n) { return "__cml_sanchor_" + std::to_string(n); }
std::string slot_name(const std::string& function_name) { return "__cml_slot_" + function_name; }

AnchorOutput add_anchors(const cml::coff::Object& obj)
{
    using namespace cml::coff;
    if (obj.is_bigobj()) {
        die("the anchor rewriter handles regular COFF only; this object is /bigobj (do not pass /bigobj to the patch compile)");
    }
    const ObjectHeader& h = obj.header();
    const ByteSpan all = obj.bytes();
    const std::size_t symoff = h.symbol_table_offset;
    const std::size_t symend = symoff + static_cast<std::size_t>(h.symbol_count) * kSymbolSize;
    const ByteSpan st = obj.string_table_bytes();

    // Layout we rely on: [header][section table][section data + relocations ...][symbols][strings] EOF.
    std::vector<std::uint8_t> strtab;
    if (st.empty()) {
        if (symend != all.size()) {
            die("unexpected object layout: %zu bytes after the symbol table but no string table", all.size() - symend);
        }
        strtab.assign(4, 0);
    } else {
        if (symend + st.size() != all.size()) {
            die("unexpected object layout: string table is not at the end of the file (symend=%zu strtab=%zu file=%zu)",
                symend, st.size(), all.size());
        }
        strtab.resize(st.size());
        std::memcpy(strtab.data(), st.data(), st.size());
    }
    for (const Section& s : obj.sections()) {
        const std::uint64_t data_end = s.pointer_to_raw_data == 0 ? 0 : std::uint64_t{s.pointer_to_raw_data} + s.size_of_raw_data;
        const std::uint64_t rel_end = s.pointer_to_relocations == 0
                                          ? 0
                                          : std::uint64_t{s.pointer_to_relocations} + s.relocation_table.size();
        if (data_end > symoff || rel_end > symoff) {
            die("unexpected object layout: section %u (%.*s) data/relocations lie after the symbol table", s.index,
                static_cast<int>(s.name.size()), s.name.data());
        }
    }

    AnchorOutput result;
    result.symbols_before = h.symbol_count;
    result.bytes.assign(reinterpret_cast<const std::uint8_t*>(all.data()),
                        reinterpret_cast<const std::uint8_t*>(all.data()) + symend);

    std::uint32_t added = 0;
    for (const Section& s : obj.sections()) {
        if (!is_anchorable(s)) {
            log("anchor: section %u '%.*s' skipped (debug/metadata/removed)", s.index,
                static_cast<int>(s.name.size()), s.name.data());
            continue;
        }
        if (s.index > 0xFEFFu) {
            die("section index %u does not fit a regular COFF symbol record", s.index);
        }
        put_symbol(result.bytes, strtab, anchor_name(s.index), 0, static_cast<std::uint16_t>(s.index), 0,
                   static_cast<std::uint8_t>(StorageClass::External));
        put_symbol(result.bytes, strtab, static_anchor_name(s.index), 0, static_cast<std::uint16_t>(s.index), 0,
                   static_cast<std::uint8_t>(StorageClass::Static));
        added += 2;
        AnchorInfo info;
        info.section = s.index;
        info.section_name = std::string(s.name);
        info.comdat = s.is_comdat();
        info.size = s.size_of_raw_data;
        result.anchors.push_back(info);
        log("anchor: section %u '%.*s'%s size=%u -> %s (external) + %s (static)", s.index,
            static_cast<int>(s.name.size()), s.name.data(), s.is_comdat() ? " [COMDAT]" : "", s.size_of_raw_data,
            anchor_name(s.index).c_str(), static_anchor_name(s.index).c_str());
    }

    patch32(strtab, 0, static_cast<std::uint32_t>(strtab.size()));
    result.bytes.insert(result.bytes.end(), strtab.begin(), strtab.end());
    patch32(result.bytes, 12, h.symbol_count + added);  // IMAGE_FILE_HEADER.NumberOfSymbols
    result.symbols_after = h.symbol_count + added;
    log("anchor: symbol count %u -> %u, string table %zu -> %zu bytes, file %zu -> %zu bytes",
        result.symbols_before, result.symbols_after, st.size(), strtab.size(), all.size(), result.bytes.size());
    return result;
}

std::vector<std::uint8_t> emit_binding_obj(const std::vector<BindEntry>& entries)
{
    std::vector<const BindEntry*> funcs;
    std::vector<const BindEntry*> datas;
    for (const BindEntry& e : entries) {
        (e.is_function ? funcs : datas).push_back(&e);
    }
    const std::size_t nf = funcs.size();
    const std::size_t nd = datas.size();
    const bool has_text = nf != 0;
    const bool has_data = (nf + nd) != 0;
    const std::uint16_t text_sec = has_text ? 1 : 0;
    const std::uint16_t data_sec = has_data ? static_cast<std::uint16_t>(has_text ? 2 : 1) : 0;
    const std::size_t nsec = (has_text ? 1u : 0u) + (has_data ? 1u : 0u);

    constexpr std::size_t kThunk = 16;
    const std::size_t text_size = nf * kThunk;
    const std::size_t text_relocs = nf;
    const std::size_t data_size = (nf + nd) * 8;

    std::size_t pos = 20 + nsec * kSectionHeaderSize;
    const std::size_t text_ptr = has_text ? pos : 0;
    pos += text_size;
    const std::size_t text_rel_ptr = has_text ? pos : 0;
    pos += text_relocs * kRelocSize;
    const std::size_t data_ptr = has_data ? pos : 0;
    pos += data_size;
    const std::size_t sym_ptr = pos;
    const std::size_t nsyms = nf * 2 + nd;

    std::vector<std::uint8_t> out;
    // IMAGE_FILE_HEADER
    put16(out, 0x8664);
    put16(out, static_cast<std::uint16_t>(nsec));
    put32(out, 0);
    put32(out, static_cast<std::uint32_t>(sym_ptr));
    put32(out, static_cast<std::uint32_t>(nsyms));
    put16(out, 0);
    put16(out, 0);

    auto section_header = [&](const char* name, std::size_t raw_size, std::size_t raw_ptr, std::size_t rel_ptr,
                              std::size_t nreloc, std::uint32_t chars) {
        std::uint8_t field[8] = {};
        std::memcpy(field, name, std::strlen(name));
        out.insert(out.end(), field, field + 8);
        put32(out, 0);  // virtual size
        put32(out, 0);  // virtual address
        put32(out, static_cast<std::uint32_t>(raw_size));
        put32(out, static_cast<std::uint32_t>(raw_ptr));
        put32(out, static_cast<std::uint32_t>(rel_ptr));
        put32(out, 0);  // line numbers
        put16(out, static_cast<std::uint16_t>(nreloc));
        put16(out, 0);
        put32(out, chars);
    };
    // CNT_CODE | ALIGN_16BYTES | MEM_EXECUTE | MEM_READ
    if (has_text) {
        section_header(".text", text_size, text_ptr, text_rel_ptr, text_relocs, 0x60500020u);
    }
    // CNT_INITIALIZED_DATA | ALIGN_8BYTES | MEM_READ | MEM_WRITE
    if (has_data) {
        section_header(".data", data_size, data_ptr, 0, 0, 0xC0400040u);
    }

    // .text raw data: thunk i = FF 25 <rel32 -> slot i> CC...
    for (std::size_t i = 0; i < nf; ++i) {
        put8(out, 0xFF);
        put8(out, 0x25);
        put32(out, 0);
        for (std::size_t k = 6; k < kThunk; ++k) {
            put8(out, 0xCC);
        }
    }
    // .text relocations: REL32 at thunk+2 against the slot symbol (symbol index 2*i+1).
    for (std::size_t i = 0; i < nf; ++i) {
        put32(out, static_cast<std::uint32_t>(i * kThunk + 2));
        put32(out, static_cast<std::uint32_t>(2 * i + 1));
        put16(out, 0x0004);  // IMAGE_REL_AMD64_REL32
    }
    // .data raw data: zeros.
    out.insert(out.end(), data_size, 0);

    // Symbol table + string table.
    std::vector<std::uint8_t> strtab(4, 0);
    for (std::size_t i = 0; i < nf; ++i) {
        put_symbol(out, strtab, funcs[i]->name, static_cast<std::uint32_t>(i * kThunk), text_sec, 0x20,
                   static_cast<std::uint8_t>(cml::coff::StorageClass::External));
        put_symbol(out, strtab, slot_name(funcs[i]->name), static_cast<std::uint32_t>(i * 8), data_sec, 0,
                   static_cast<std::uint8_t>(cml::coff::StorageClass::External));
    }
    for (std::size_t i = 0; i < nd; ++i) {
        put_symbol(out, strtab, datas[i]->name, static_cast<std::uint32_t>((nf + i) * 8), data_sec, 0,
                   static_cast<std::uint8_t>(cml::coff::StorageClass::External));
    }
    patch32(strtab, 0, static_cast<std::uint32_t>(strtab.size()));
    out.insert(out.end(), strtab.begin(), strtab.end());
    return out;
}

}  // namespace spike
