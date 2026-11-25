#pragma once

// Test-only builders of CodeView type records (WP-37), for the TPI / IPI streams of a pdb_builder PDB and for the
// `.debug$T` / `.debug$S` sections of a synthetic object. Layouts follow Microsoft's cvinfo.h; every helper returns the
// record BODY (the bytes after the length and the leaf kind).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "pdb_builder.h"

namespace cml::testpdb {

inline constexpr std::uint16_t kLfModifier = 0x1001;
inline constexpr std::uint16_t kLfPointer = 0x1002;
inline constexpr std::uint16_t kLfProcedure = 0x1008;
inline constexpr std::uint16_t kLfMFunction = 0x1009;
inline constexpr std::uint16_t kLfArgList = 0x1201;
inline constexpr std::uint16_t kLfArray = 0x1503;
inline constexpr std::uint16_t kLfClass = 0x1504;
inline constexpr std::uint16_t kLfStructure = 0x1505;
inline constexpr std::uint16_t kLfUnion = 0x1506;
inline constexpr std::uint16_t kLfEnum = 0x1507;
inline constexpr std::uint16_t kLfPrecomp = 0x1509;
inline constexpr std::uint16_t kLfTypeServer2 = 0x1515;
inline constexpr std::uint16_t kLfFuncId = 0x1601;
inline constexpr std::uint16_t kLfMFuncId = 0x1602;

// Simple (built-in) types.
inline constexpr std::uint32_t kTVoid = 0x0003;
inline constexpr std::uint32_t kTInt = 0x0074;
inline constexpr std::uint32_t kTFloat = 0x0040;
inline constexpr std::uint32_t kTDouble = 0x0041;
inline constexpr std::uint32_t kTChar = 0x0070;

// Pointer attributes: a 64-bit pointer (kind 0x0C, size 8) and an lvalue reference to it.
inline constexpr std::uint32_t kPtr64 = 0x1000Cu;
inline constexpr std::uint32_t kRef64 = 0x1002Cu;

inline Bytes lf_modifier(std::uint32_t type, std::uint16_t attr) {  // attr: 1 const, 2 volatile, 4 unaligned
    Bytes b;
    put32(b, type);
    put16(b, attr);
    return b;
}
inline Bytes lf_pointer(std::uint32_t type, std::uint32_t attr = kPtr64) {
    Bytes b;
    put32(b, type);
    put32(b, attr);
    return b;
}
inline Bytes lf_arglist(const std::vector<std::uint32_t>& types) {
    Bytes b;
    put32(b, static_cast<std::uint32_t>(types.size()));
    for (const std::uint32_t t : types) {
        put32(b, t);
    }
    return b;
}
inline Bytes lf_procedure(std::uint32_t ret, std::uint32_t arglist, std::uint16_t count, std::uint8_t call = 0, std::uint8_t attr = 0) {
    Bytes b;
    put32(b, ret);
    b.push_back(call);
    b.push_back(attr);
    put16(b, count);
    put32(b, arglist);
    return b;
}
inline Bytes lf_mfunction(std::uint32_t ret, std::uint32_t cls, std::uint32_t this_type, std::uint32_t arglist, std::uint16_t count, std::int32_t this_adjust = 0) {
    Bytes b;
    put32(b, ret);
    put32(b, cls);
    put32(b, this_type);
    b.push_back(0);  // calling convention
    b.push_back(0);  // function attributes
    put16(b, count);
    put32(b, arglist);
    put32(b, static_cast<std::uint32_t>(this_adjust));
    return b;
}
// LF_CLASS / LF_STRUCTURE. `unique` empty: no unique name (property bit 0x200 clear). `forward`: a forward reference.
inline Bytes lf_udt(const std::string& name, const std::string& unique, bool forward = false, std::uint16_t size = 8) {
    Bytes b;
    put16(b, 0);  // member count
    put16(b, static_cast<std::uint16_t>((unique.empty() ? 0 : 0x200) | (forward ? 0x80 : 0)));
    put32(b, 0);  // field list
    put32(b, 0);  // derived
    put32(b, 0);  // vshape
    put16(b, forward ? std::uint16_t{0} : size);  // size, a numeric leaf below 0x8000
    putstr(b, name);
    if (!unique.empty()) {
        putstr(b, unique);
    }
    return b;
}
inline Bytes lf_enum(const std::string& name, const std::string& unique, std::uint32_t underlying = kTInt) {
    Bytes b;
    put16(b, 0);
    put16(b, static_cast<std::uint16_t>(unique.empty() ? 0 : 0x200));
    put32(b, underlying);
    put32(b, 0);  // field list
    putstr(b, name);
    if (!unique.empty()) {
        putstr(b, unique);
    }
    return b;
}
inline Bytes lf_array(std::uint32_t element, std::uint16_t bytes) {
    Bytes b;
    put32(b, element);
    put32(b, 0x0023);  // index type: unsigned __int64
    put16(b, bytes);
    b.push_back(0);    // name
    return b;
}
inline Bytes lf_func_id(std::uint32_t type, const std::string& name, std::uint32_t scope = 0) {
    Bytes b;
    put32(b, scope);
    put32(b, type);
    putstr(b, name);
    return b;
}
inline Bytes lf_mfunc_id(std::uint32_t parent_class, std::uint32_t type, const std::string& name) {
    Bytes b;
    put32(b, parent_class);
    put32(b, type);
    putstr(b, name);
    return b;
}
inline Bytes lf_typeserver2(const std::array<std::uint8_t, 16>& guid, std::uint32_t age, const std::string& path) {
    Bytes b(guid.begin(), guid.end());
    put32(b, age);
    putstr(b, path);
    return b;
}
inline Bytes lf_precomp(std::uint32_t start, std::uint32_t count, const std::string& object) {
    Bytes b;
    put32(b, start);
    put32(b, count);
    put32(b, 0x12345678);  // signature
    putstr(b, object);
    return b;
}

// The records of one type space, in index order. The same list can become a PDB stream (add_to) or an object's
// `.debug$T` section (debug_t); `first` is the index of the first record (0x1000, or higher after an LF_PRECOMP).
class TypeSet {
public:
    std::uint32_t add(std::uint16_t kind, const Bytes& body) {
        recs_.emplace_back(kind, body);
        return first_ + static_cast<std::uint32_t>(recs_.size()) - 1;
    }
    std::uint32_t next() const { return first_ + static_cast<std::uint32_t>(recs_.size()); }
    void set_first(std::uint32_t first) { first_ = first; }
    std::size_t size() const { return recs_.size(); }

    void add_to(Ipi& stream) const {
        for (const auto& r : recs_) {
            stream.record(r.first, r.second);
        }
    }
    // `.debug$T`: the C13 signature, an optional head record without an index (LF_PRECOMP), then the records.
    Bytes debug_t(const std::pair<std::uint16_t, Bytes>* head = nullptr) const {
        Bytes out;
        put32(out, 4);
        Records r;
        if (head != nullptr) {
            r.add(head->first, head->second);
        }
        for (const auto& rec : recs_) {
            r.add(rec.first, rec.second);
        }
        out.insert(out.end(), r.bytes().begin(), r.bytes().end());
        return out;
    }

private:
    std::vector<std::pair<std::uint16_t, Bytes>> recs_;
    std::uint32_t first_ = 0x1000;
};

// A `.debug$S` section around symbol records: the C13 signature, an unrelated subsection (a string table, 0xF3, so the
// symbols do not start right after the signature) and one symbols subsection (0xF1). `base` receives the offset of the
// first symbol record within the section.
inline Bytes debug_s_section(const Bytes& symbols, std::uint32_t* base = nullptr) {
    Bytes out;
    put32(out, 4);  // C13 signature
    put32(out, 0xF3);
    put32(out, 5);
    out.insert(out.end(), {0, 'a', '.', 'c', 0});
    pad4(out);
    put32(out, 0xF1);
    put32(out, static_cast<std::uint32_t>(symbols.size()));
    if (base != nullptr) {
        *base = static_cast<std::uint32_t>(out.size());
    }
    out.insert(out.end(), symbols.begin(), symbols.end());
    pad4(out);
    return out;
}

// A `.debug$S` section with one symbols subsection holding one procedure record per entry. `offset_fields` receives,
// per entry, the offset within the section of the record's code-offset field (where the object has its SECREL
// relocation).
struct DebugSProc {
    std::uint16_t kind = 0x1146;  // S_LPROC32_ID; 0x1147 S_GPROC32_ID, 0x110F S_LPROC32, 0x1110 S_GPROC32
    std::string name;
    std::uint32_t type = 0;
};
inline Bytes debug_s(const std::vector<DebugSProc>& procs, std::vector<std::uint32_t>* offset_fields = nullptr) {
    Records symbols;
    std::vector<std::uint32_t> starts;
    for (const DebugSProc& p : procs) {
        starts.push_back(static_cast<std::uint32_t>(symbols.bytes().size()));
        symbols.proc(p.kind, p.name, 0, 0, 0x10, 0, p.type);
        symbols.add(p.kind == 0x1146 || p.kind == 0x1147 ? std::uint16_t{0x114F} : std::uint16_t{0x0006}, Bytes());  // S_PROC_ID_END / S_END
    }
    std::uint32_t base = 0;
    Bytes out = debug_s_section(symbols.bytes(), &base);
    if (offset_fields != nullptr) {
        for (const std::uint32_t s : starts) {
            offset_fields->push_back(base + s + 4 + 28);
        }
    }
    return out;
}

}  // namespace cml::testpdb
