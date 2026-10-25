#pragma once

// CodeView record kinds and fixed field offsets used by cml_pdb. Offsets are from the start of the
// record body (the bytes after the 2-byte length and 2-byte kind).

#include <cstddef>
#include <cstdint>

#include "detail/le.h"

namespace cml::pdb::detail::cv {

// Symbol kinds.
inline constexpr std::uint16_t S_END = 0x0006;
inline constexpr std::uint16_t S_THUNK32 = 0x1102;
inline constexpr std::uint16_t S_BLOCK32 = 0x1103;
inline constexpr std::uint16_t S_WITH32 = 0x1104;
inline constexpr std::uint16_t S_UDT = 0x1108;
inline constexpr std::uint16_t S_OBJNAME = 0x1101;
inline constexpr std::uint16_t S_LDATA32 = 0x110C;
inline constexpr std::uint16_t S_GDATA32 = 0x110D;
inline constexpr std::uint16_t S_PUB32 = 0x110E;
inline constexpr std::uint16_t S_LPROC32 = 0x110F;
inline constexpr std::uint16_t S_GPROC32 = 0x1110;
inline constexpr std::uint16_t S_LTHREAD32 = 0x1112;
inline constexpr std::uint16_t S_GTHREAD32 = 0x1113;
inline constexpr std::uint16_t S_LMANPROC = 0x1127;
inline constexpr std::uint16_t S_GMANPROC = 0x112A;
inline constexpr std::uint16_t S_SEPCODE = 0x1132;
inline constexpr std::uint16_t S_COMPILE3 = 0x113C;
inline constexpr std::uint16_t S_ENVBLOCK = 0x113D;
inline constexpr std::uint16_t S_LPROC32_ID = 0x1146;
inline constexpr std::uint16_t S_GPROC32_ID = 0x1147;
inline constexpr std::uint16_t S_LPROC32_DPC = 0x1155;
inline constexpr std::uint16_t S_LPROC32_DPC_ID = 0x1156;
inline constexpr std::uint16_t S_BUILDINFO = 0x114C;
inline constexpr std::uint16_t S_INLINESITE = 0x114D;
inline constexpr std::uint16_t S_INLINESITE_END = 0x114E;
inline constexpr std::uint16_t S_PROC_ID_END = 0x114F;
inline constexpr std::uint16_t S_INLINESITE2 = 0x115D;

// Leaf kinds (IPI).
inline constexpr std::uint16_t LF_SUBSTR_LIST = 0x1604;
inline constexpr std::uint16_t LF_BUILDINFO = 0x1603;
inline constexpr std::uint16_t LF_STRING_ID = 0x1605;

inline constexpr bool is_proc(std::uint16_t k) noexcept {
    return k == S_LPROC32 || k == S_GPROC32 || k == S_LPROC32_ID || k == S_GPROC32_ID || k == S_LPROC32_DPC ||
           k == S_LPROC32_DPC_ID;
}
inline constexpr bool is_global_proc(std::uint16_t k) noexcept {
    return k == S_GPROC32 || k == S_GPROC32_ID;
}
inline constexpr bool is_scope_opener(std::uint16_t k) noexcept {
    return is_proc(k) || k == S_BLOCK32 || k == S_THUNK32 || k == S_WITH32 || k == S_INLINESITE ||
           k == S_INLINESITE2 || k == S_SEPCODE || k == S_LMANPROC || k == S_GMANPROC;
}
inline constexpr bool is_scope_closer(std::uint16_t k) noexcept {
    return k == S_END || k == S_INLINESITE_END || k == S_PROC_ID_END;
}

// A record located inside a stream image.
struct Record {
    std::uint16_t kind = 0;
    const std::uint8_t* body = nullptr;
    std::uint32_t body_len = 0;  // bytes after the 4-byte header
    std::uint64_t offset = 0;    // of the record header within the scanned region
};

// Walks records of [data, data+len) (each: u16 length-after-the-length-field, u16 kind, body).
// Calls f(const Record&) for every record. Returns false if a record's length is < 2 or its body
// would leave the region. A trailing fragment too short to hold a header (< 4 bytes) is ignored.
template <class F>
bool for_each_record(const std::uint8_t* data, std::uint64_t begin, std::uint64_t len, F&& f) {
    std::uint64_t off = begin;
    while (off < len) {
        if (len - off < 4) {
            return true;
        }
        const std::uint32_t reclen = rd16(data + off);
        if (reclen < 2 || reclen > len - off - 2) {
            return false;
        }
        Record r;
        r.kind = rd16(data + off + 2);
        r.body = data + off + 4;
        r.body_len = reclen - 2;
        r.offset = off;
        f(r);
        off += 2 + reclen;
    }
    return true;
}

// Length of the NUL-terminated string at [p, p+max) (max if there is no terminator).
inline std::size_t cstr_len(const std::uint8_t* p, std::size_t max) noexcept {
    std::size_t n = 0;
    while (n < max && p[n] != 0) {
        ++n;
    }
    return n;
}

}  // namespace cml::pdb::detail::cv
