// WP-37: the canonical signature of a function from CodeView type records, and the readers of an object's
// `.debug$T` / `.debug$S` sections. Record layouts follow Microsoft's published cvinfo.h (microsoft-pdb, MIT) and
// LLVM's CodeView documentation; nothing here is copied from either.

#include "cml/pdb/type_records.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "cml/pdb/diag_ids.h"
#include "detail/codeview.h"
#include "detail/static_scopes.h"
#include "detail/le.h"

namespace cml::pdb {

using namespace detail;

namespace {

constexpr std::uint16_t LF_MODIFIER = 0x1001;
constexpr std::uint16_t LF_POINTER = 0x1002;
constexpr std::uint16_t LF_PROCEDURE = 0x1008;
constexpr std::uint16_t LF_MFUNCTION = 0x1009;
constexpr std::uint16_t LF_ARGLIST = 0x1201;
constexpr std::uint16_t LF_BITFIELD = 0x1205;
constexpr std::uint16_t LF_ARRAY = 0x1503;
constexpr std::uint16_t LF_CLASS = 0x1504;
constexpr std::uint16_t LF_STRUCTURE = 0x1505;
constexpr std::uint16_t LF_UNION = 0x1506;
constexpr std::uint16_t LF_ENUM = 0x1507;
constexpr std::uint16_t LF_PRECOMP = 0x1509;
constexpr std::uint16_t LF_TYPESERVER2 = 0x1515;
constexpr std::uint16_t LF_INTERFACE = 0x1519;
constexpr std::uint16_t LF_FUNC_ID = 0x1601;
constexpr std::uint16_t LF_MFUNC_ID = 0x1602;

constexpr std::uint16_t kPropHasUniqueName = 0x0200;
constexpr std::uint16_t kPropForwardRef = 0x0080;

// WP-46: local variable records, and the field-list leaves a closure class holds.
constexpr std::uint16_t kSymLocal = 0x113E;     // S_LOCAL: type u32, flags u16, name
constexpr std::uint16_t kSymRegRel32 = 0x1111;  // S_REGREL32: offset u32, type u32, register u16, name
constexpr std::uint16_t kSymBpRel32 = 0x110B;   // S_BPREL32: offset i32, type u32, name
constexpr std::uint16_t LF_FIELDLIST = 0x1203;
constexpr std::uint16_t LF_MEMBER = 0x150D;
constexpr std::uint16_t LF_METHOD = 0x150F;
constexpr std::uint16_t LF_NESTTYPE = 0x1510;
constexpr std::uint16_t LF_ONEMETHOD = 0x1511;

// Limits of one signature. Real signatures use a few dozen records; a crafted record graph (a pointer to itself, an
// argument list that fans out) runs into these instead of the stack or the heap.
constexpr int kMaxDepth = 40;
constexpr std::uint32_t kMaxRecords = 4096;
constexpr std::uint32_t kMaxArguments = 1024;
constexpr std::size_t kMaxText = 64 * 1024;

constexpr std::uint32_t kC13Signature = 4;
constexpr std::uint32_t kSubsectionSymbols = 0xF1;
constexpr std::uint32_t kSubsectionIgnore = 0x80000000u;

std::string hex(std::uint32_t v) {
    char text[16];
    std::snprintf(text, sizeof text, "%X", v);
    return text;
}

void append_hex(std::string& out, std::uint64_t v) {
    char text[24];
    std::snprintf(text, sizeof text, "%llX", static_cast<unsigned long long>(v));
    out += text;
}

// A CodeView numeric leaf: a u16 below 0x8000 is the value, otherwise it names the width of what follows.
bool read_numeric(const std::uint8_t* p, std::uint32_t n, std::uint32_t& used, std::uint64_t& value) {
    if (n < 2) {
        return false;
    }
    const std::uint16_t lead = rd16(p);
    if (lead < 0x8000) {
        used = 2;
        value = lead;
        return true;
    }
    std::uint32_t width = 0;
    switch (lead) {
        case 0x8000: width = 1; break;  // LF_CHAR
        case 0x8001:                    // LF_SHORT
        case 0x8002: width = 2; break;  // LF_USHORT
        case 0x8003:                    // LF_LONG
        case 0x8004: width = 4; break;  // LF_ULONG
        case 0x8009:                    // LF_QUADWORD
        case 0x800A: width = 8; break;  // LF_UQUADWORD
        default: return false;
    }
    if (n < 2 + width) {
        return false;
    }
    std::uint64_t v = 0;
    for (std::uint32_t i = 0; i < width; ++i) {
        v |= static_cast<std::uint64_t>(p[2 + i]) << (8 * i);
    }
    used = 2 + width;
    value = v;
    return true;
}

// The NUL-terminated string at [p, p+n). False when there is no terminator.
bool read_cstr(const std::uint8_t* p, std::uint32_t n, std::string_view& out, std::uint32_t& used) {
    const std::size_t len = cv::cstr_len(p, n);
    if (len >= n) {
        return false;
    }
    out = std::string_view(reinterpret_cast<const char*>(p), len);
    used = static_cast<std::uint32_t>(len) + 1;
    return true;
}

struct UdtNames {
    std::string_view name;
    std::string_view unique;  // empty when the record has none
};

// The names of a class / struct / interface / union / enum record.
bool udt_names(const TypeRecord& r, UdtNames& out) {
    std::uint32_t at = 0;
    if (r.kind == LF_CLASS || r.kind == LF_STRUCTURE || r.kind == LF_INTERFACE || r.kind == LF_UNION) {
        const std::uint32_t fixed = r.kind == LF_UNION ? 8u : 16u;
        if (r.body_len < fixed + 2) {
            return false;
        }
        std::uint32_t used = 0;
        std::uint64_t size = 0;
        if (!read_numeric(r.body + fixed, r.body_len - fixed, used, size)) {
            return false;
        }
        at = fixed + used;
    } else if (r.kind == LF_ENUM) {
        at = 12;
    } else {
        return false;
    }
    if (r.body_len <= at) {
        return false;
    }
    const std::uint16_t property = rd16(r.body + 2);
    std::uint32_t used = 0;
    if (!read_cstr(r.body + at, r.body_len - at, out.name, used)) {
        return false;
    }
    at += used;
    out.unique = {};
    if ((property & kPropHasUniqueName) != 0) {
        if (r.body_len <= at || !read_cstr(r.body + at, r.body_len - at, out.unique, used)) {
            return false;
        }
    }
    return true;
}

class Renderer {
public:
    explicit Renderer(const TypeSource& src) : src_(src) {}

    const std::string& why() const noexcept { return why_; }

    bool type(std::uint32_t index, int depth, std::string& out) {
        if (index < 0x1000) {
            out += 't';
            append_hex(out, index);
            return true;
        }
        if (depth > kMaxDepth) {
            return fail("the type nests deeper than " + std::to_string(kMaxDepth) + " records (type 0x" + hex(index) + ")");
        }
        if (++records_ > kMaxRecords || out.size() > kMaxText) {
            return fail("the type needs more than " + std::to_string(kMaxRecords) + " records");
        }
        TypeRecord r;
        if (!src_.find_type(index, r)) {
            return fail("type 0x" + hex(index) + " is not in the type records");
        }
        switch (r.kind) {
            case LF_MODIFIER: {
                if (r.body_len < 6) {
                    return short_record(index, r);
                }
                out += 'M';
                append_hex(out, rd16(r.body + 4) & 7u);
                out += '(';
                if (!type(rd32(r.body), depth + 1, out)) {
                    return false;
                }
                out += ')';
                return true;
            }
            case LF_POINTER: {
                if (r.body_len < 8) {
                    return short_record(index, r);
                }
                const std::uint32_t attr = rd32(r.body + 4);
                const std::uint32_t mode = (attr >> 5) & 7u;
                out += 'P';
                append_hex(out, attr);
                out += '(';
                if (!type(rd32(r.body), depth + 1, out)) {
                    return false;
                }
                if (mode == 2 || mode == 3) {  // pointer to data member / to member function
                    if (r.body_len < 14) {
                        return short_record(index, r);
                    }
                    out += ";of=";
                    if (!type(rd32(r.body + 8), depth + 1, out)) {
                        return false;
                    }
                    out += ";rep=";
                    append_hex(out, rd16(r.body + 12));
                }
                out += ')';
                return true;
            }
            case LF_ARRAY: {
                if (r.body_len < 10) {
                    return short_record(index, r);
                }
                std::uint32_t used = 0;
                std::uint64_t size = 0;
                if (!read_numeric(r.body + 8, r.body_len - 8, used, size)) {
                    return fail("the array type 0x" + hex(index) + " has an unreadable size");
                }
                out += 'A';
                append_hex(out, size);
                out += '(';
                if (!type(rd32(r.body), depth + 1, out)) {
                    return false;
                }
                out += ')';
                return true;
            }
            case LF_BITFIELD: {
                if (r.body_len < 6) {
                    return short_record(index, r);
                }
                out += 'B';
                append_hex(out, r.body[4]);
                out += ':';
                append_hex(out, r.body[5]);
                out += '(';
                if (!type(rd32(r.body), depth + 1, out)) {
                    return false;
                }
                out += ')';
                return true;
            }
            case LF_PROCEDURE:
            case LF_MFUNCTION: return function(index, r, depth, out, nullptr);
            case LF_CLASS:
            case LF_STRUCTURE:
            case LF_INTERFACE:
            case LF_UNION:
            case LF_ENUM: {
                UdtNames names;
                if (!udt_names(r, names)) {
                    return fail("the class, union or enum record 0x" + hex(index) + " is malformed");
                }
                if (!names.unique.empty()) {
                    out += "U{";
                    out += without_tu_discriminator(names.unique);
                } else {
                    // No unique name (C, or a compiler that writes none): the leaf kind and the name are all there is.
                    out += 'N';
                    append_hex(out, r.kind);
                    out += '{';
                    out += names.name;
                }
                out += '}';
                return true;
            }
            default: break;
        }
        return fail("type 0x" + hex(index) + " is a record of kind 0x" + hex(r.kind) + ", which the signature reader does not know");
    }

    // LF_PROCEDURE / LF_MFUNCTION. `class_type` receives the class type index of a member function.
    bool function(std::uint32_t index, const TypeRecord& r, int depth, std::string& out, std::uint32_t* class_type) {
        const bool member = r.kind == LF_MFUNCTION;
        if (r.body_len < (member ? 24u : 12u)) {
            return short_record(index, r);
        }
        const std::uint32_t base = member ? 12u : 4u;
        out += member ? "MF(cc=" : "F(cc=";
        append_hex(out, r.body[base]);
        out += ",fa=";
        append_hex(out, r.body[base + 1]);
        out += ",r=";
        if (!type(rd32(r.body), depth + 1, out)) {
            return false;
        }
        if (member) {
            if (class_type != nullptr) {
                *class_type = rd32(r.body + 4);
            }
            out += ",c=";
            if (!type(rd32(r.body + 4), depth + 1, out)) {
                return false;
            }
            out += ",this=";
            if (!type(rd32(r.body + 8), depth + 1, out)) {
                return false;
            }
            out += ",adj=";
            append_hex(out, rd32(r.body + 20));
        }
        out += ",a=[";
        const std::uint32_t arglist = rd32(r.body + base + 4);
        TypeRecord args;
        if (!src_.find_type(arglist, args)) {
            return fail("the argument list 0x" + hex(arglist) + " is not in the type records");
        }
        if (args.kind != LF_ARGLIST || args.body_len < 4) {
            return fail("the argument list 0x" + hex(arglist) + " is not an LF_ARGLIST record");
        }
        const std::uint32_t count = rd32(args.body);
        if (count > kMaxArguments || static_cast<std::uint64_t>(count) * 4 + 4 > args.body_len) {
            return fail("the argument list 0x" + hex(arglist) + " is inconsistent");
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            if (i != 0) {
                out += ',';
            }
            if (!type(rd32(args.body + 4 + static_cast<std::size_t>(i) * 4), depth + 1, out)) {
                return false;
            }
        }
        out += "])";
        return true;
    }

    bool fail(std::string why) {
        if (why_.empty()) {
            why_ = std::move(why);
        }
        return false;
    }

private:
    bool short_record(std::uint32_t index, const TypeRecord& r) {
        return fail("the record 0x" + hex(index) + " (kind 0x" + hex(r.kind) + ") is shorter than its fixed part");
    }

    const TypeSource& src_;
    std::uint32_t records_ = 0;
    std::string why_;
};

FunctionSignature unknown(std::string why) {
    FunctionSignature s;
    s.diag_id = diag::kBadTypeRecord;
    s.why = std::move(why);
    return s;
}

}  // namespace

std::string without_tu_discriminator(std::string_view name) {
    const auto is_hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
    std::string out;
    out.reserve(name.size());
    for (std::size_t i = 0; i < name.size();) {
        if (name[i] == '`') {
            // "`" + exactly eight hex digits. (A backtick is not a character of the name mangling.)
            std::size_t k = i + 1;
            while (k < name.size() && k - i <= 8 && is_hex(name[k])) {
                ++k;
            }
            if (k - i - 1 == 8 && (k == name.size() || !is_hex(name[k]))) {
                i = k;
                continue;
            }
        } else if (name.compare(i, 4, "?A0x") == 0) {
            std::size_t k = i + 4;
            while (k < name.size() && is_hex(name[k])) {
                ++k;
            }
            if (k > i + 4 && k < name.size() && name[k] == '@') {
                out += "?A0x";
                i = k;  // the '@' is copied by the next round
                continue;
            }
        }
        out += name[i];
        ++i;
    }
    return out;
}

FunctionSignature function_signature(const TypeSource& types, std::uint32_t index, bool index_is_id) {
    std::uint32_t function_type = index;
    if (index_is_id) {
        TypeRecord id;
        if (!types.find_id(index, id)) {
            return unknown("the function id 0x" + hex(index) + " is not in the id records");
        }
        if ((id.kind != LF_FUNC_ID && id.kind != LF_MFUNC_ID) || id.body_len < 8) {
            return unknown("the function id 0x" + hex(index) + " is not an LF_FUNC_ID / LF_MFUNC_ID record (kind 0x" + hex(id.kind) + ")");
        }
        function_type = rd32(id.body + 4);
    }
    if (function_type < 0x1000) {
        return unknown("the function has no type record (type 0x" + hex(function_type) + ")");
    }
    TypeRecord r;
    if (!types.find_type(function_type, r)) {
        return unknown("the function type 0x" + hex(function_type) + " is not in the type records");
    }
    if (r.kind != LF_PROCEDURE && r.kind != LF_MFUNCTION) {
        return unknown("the function type 0x" + hex(function_type) + " is neither LF_PROCEDURE nor LF_MFUNCTION (kind 0x" + hex(r.kind) + ")");
    }
    Renderer renderer(types);
    FunctionSignature s;
    std::uint32_t class_type = 0;
    if (!renderer.function(function_type, r, 0, s.text, &class_type) || s.text.size() > kMaxText) {
        return unknown(renderer.why().empty() ? std::string("the signature is too long") : renderer.why());
    }
    if (r.kind == LF_MFUNCTION) {
        s.member = true;
        TypeRecord cls;
        UdtNames names;
        if (class_type >= 0x1000 && types.find_type(class_type, cls) && udt_names(cls, names)) {
            s.class_name = std::string(names.name);
            s.class_unique_name = std::string(names.unique);
        }
    }
    s.known = true;
    return s;
}

std::optional<std::string> canonical_type(const TypeSource& types, std::uint32_t type_index, std::string* why) {
    Renderer renderer(types);
    std::string out;
    if (!renderer.type(type_index, 0, out) || out.size() > kMaxText) {
        if (why != nullptr) {
            *why = renderer.why().empty() ? std::string("the type is too long") : renderer.why();
        }
        return std::nullopt;
    }
    return out;
}

bool udt_record_names(const TypeRecord& record, std::string_view& name, std::string_view& unique) {
    UdtNames names;
    if (record.body == nullptr || !udt_names(record, names)) {
        return false;
    }
    name = names.name;
    unique = names.unique;
    return true;
}

std::optional<std::uint64_t> udt_definition_size(const TypeRecord& r) {
    if ((r.kind != LF_CLASS && r.kind != LF_STRUCTURE && r.kind != LF_UNION) || r.body == nullptr) {
        return std::nullopt;
    }
    const std::uint32_t fixed = r.kind == LF_UNION ? 8u : 16u;
    if (r.body_len < fixed + 2 || (rd16(r.body + 2) & kPropForwardRef) != 0) {
        return std::nullopt;
    }
    std::uint32_t used = 0;
    std::uint64_t size = 0;
    if (!read_numeric(r.body + fixed, r.body_len - fixed, used, size)) {
        return std::nullopt;
    }
    return size;
}

std::optional<std::uint64_t> type_size_of(const TypeSource& types, std::uint32_t index,
                                          const std::function<std::optional<std::uint64_t>(const std::string& unique)>& forward) {
    for (int depth = 0; depth < 8; ++depth) {
        if (index < 0x1000) {
            if (((index >> 8) & 0xFu) != 0) {
                return 8;  // a pointer mode of a simple type (64-bit)
            }
            switch (index & 0xFFu) {
                case 0x10: case 0x20: case 0x30: case 0x68: case 0x69: case 0x70: case 0x7C: return 1;
                case 0x11: case 0x21: case 0x31: case 0x71: case 0x72: case 0x73: case 0x7A: return 2;
                case 0x12: case 0x22: case 0x32: case 0x40: case 0x74: case 0x75: case 0x7B: return 4;
                case 0x13: case 0x23: case 0x33: case 0x41: case 0x76: case 0x77: return 8;
                default: return std::nullopt;
            }
        }
        TypeRecord r;
        if (!types.find_type(index, r)) {
            return std::nullopt;
        }
        switch (r.kind) {
            case LF_MODIFIER:
                if (r.body_len < 6) {
                    return std::nullopt;
                }
                index = rd32(r.body);
                continue;
            case LF_POINTER: {
                if (r.body_len < 8) {
                    return std::nullopt;
                }
                const std::uint32_t size = (rd32(r.body + 4) >> 13) & 0x3Fu;
                return size == 0 ? 8u : size;
            }
            case LF_ENUM:
                if (r.body_len < 8) {
                    return std::nullopt;
                }
                index = rd32(r.body + 4);
                continue;
            case LF_ARRAY: {
                std::uint32_t used = 0;
                std::uint64_t size = 0;
                if (r.body_len < 10 || !read_numeric(r.body + 8, r.body_len - 8, used, size)) {
                    return std::nullopt;
                }
                return size;
            }
            case LF_CLASS:
            case LF_STRUCTURE:
            case LF_UNION: {
                if (const std::optional<std::uint64_t> size = udt_definition_size(r)) {
                    return size;
                }
                UdtNames names;
                if (!udt_names(r, names) || names.unique.empty() || !forward) {
                    return std::nullopt;
                }
                return forward(std::string(names.unique));  // (second review: exact, the unit's discriminator included)
            }
            default: return std::nullopt;
        }
    }
    return std::nullopt;
}

std::string class_unique_name_of(const TypeSource& types, std::uint32_t index) {
    for (int depth = 0; depth < 4 && index >= 0x1000; ++depth) {
        TypeRecord r;
        if (!types.find_type(index, r)) {
            return {};
        }
        if (r.kind == LF_MODIFIER && r.body_len >= 6) {
            index = rd32(r.body);
            continue;
        }
        if (r.kind != LF_CLASS && r.kind != LF_STRUCTURE) {
            return {};
        }
        UdtNames names;
        if (!udt_names(r, names) || names.unique.empty()) {
            return {};
        }
        return without_tu_discriminator(names.unique);
    }
    return {};
}

std::optional<std::string> udt_layout(const TypeSource& types, std::uint32_t index, std::string* why) {
    const auto fail = [&](const std::string& text) -> std::optional<std::string> {
        if (why != nullptr) {
            *why = text;
        }
        return std::nullopt;
    };
    TypeRecord r;
    if (!types.find_type(index, r)) {
        return fail("type record 0x" + hex(index) + " cannot be read");
    }
    if ((r.kind != LF_CLASS && r.kind != LF_STRUCTURE) || r.body_len < 18) {
        return fail("type record 0x" + hex(index) + " is not a class");
    }
    if ((rd16(r.body + 2) & kPropForwardRef) != 0) {
        return fail("type record 0x" + hex(index) + " is a forward reference");
    }
    const std::uint32_t field_index = rd32(r.body + 4);
    std::uint32_t used = 0;
    std::uint64_t size = 0;
    if (!read_numeric(r.body + 16, r.body_len - 16, used, size)) {
        return fail("the size of type record 0x" + hex(index) + " cannot be read");
    }
    std::string out = "size=" + std::to_string(size);
    if (field_index == 0) {
        return out;
    }
    TypeRecord fl;
    if (!types.find_type(field_index, fl) || fl.kind != LF_FIELDLIST) {
        return fail("the field list 0x" + hex(field_index) + " cannot be read");
    }
    const std::uint8_t* p = fl.body;
    const std::uint32_t n = fl.body_len;
    std::uint32_t off = 0;
    std::uint32_t members = 0;
    while (off < n) {
        if (p[off] >= 0xF0) {  // LF_PAD<k>: k bytes of padding, this one included
            const std::uint32_t skip = p[off] & 0x0Fu;
            if (skip == 0) {
                return fail("the field list 0x" + hex(field_index) + " is malformed");
            }
            off += skip;
            continue;
        }
        if (n - off < 2) {
            return fail("the field list 0x" + hex(field_index) + " is malformed");
        }
        const std::uint16_t leaf = rd16(p + off);
        std::uint32_t name_at = 0;
        if (leaf == LF_MEMBER) {
            if (n - off < 10) {
                return fail("a member of the field list 0x" + hex(field_index) + " is malformed");
            }
            const std::uint32_t type = rd32(p + off + 4);
            std::uint64_t at = 0;
            std::uint32_t width = 0;
            if (!read_numeric(p + off + 8, n - off - 8, width, at)) {
                return fail("a member offset of the field list 0x" + hex(field_index) + " cannot be read");
            }
            std::string unreadable;
            const std::optional<std::string> text = canonical_type(types, type, &unreadable);
            if (!text) {
                return fail("the type of a member cannot be read: " + unreadable);
            }
            out += ";" + std::to_string(at) + ":" + *text;
            if (++members > kMaxArguments) {
                return fail("the class has too many members");
            }
            name_at = off + 8 + width;
        } else if (leaf == LF_ONEMETHOD) {
            if (n - off < 8) {
                return fail("a method of the field list 0x" + hex(field_index) + " is malformed");
            }
            const std::uint32_t kind = (rd16(p + off + 2) >> 2) & 7u;  // CV_MTintro / CV_MTpureintro carry a vbase offset
            name_at = off + 8 + ((kind == 4 || kind == 6) ? 4u : 0u);
        } else if (leaf == LF_METHOD || leaf == LF_NESTTYPE) {
            name_at = off + 8;
        } else {
            return fail("the field list 0x" + hex(field_index) + " has a leaf 0x" + hex(leaf) + " this reader does not know");
        }
        if (name_at >= n) {
            return fail("the field list 0x" + hex(field_index) + " is malformed");
        }
        std::string_view name;
        std::uint32_t name_len = 0;
        if (!read_cstr(p + name_at, n - name_at, name, name_len)) {
            return fail("the field list 0x" + hex(field_index) + " is malformed");
        }
        off = name_at + name_len;
    }
    return out;
}

// ---- objects ---------------------------------------------------------------------------------------------------------

Result<DebugTInfo> inspect_debug_t(std::span<const std::uint8_t> section) {
    if (section.size() < 4 || rd32(section.data()) != kC13Signature) {
        return Diag::make(diag::kNoObjectTypes, "The object's .debug$T section is not a CodeView C13 type section.",
                          "Compile the translation unit with /Z7 (or /Zi) so the object carries type information.");
    }
    DebugTInfo info;
    if (section.size() < 8) {
        return info;
    }
    const std::uint8_t* d = section.data();
    const std::uint32_t reclen = rd16(d + 4);
    if (reclen < 2 || reclen > section.size() - 6) {
        return Diag::make(diag::kBadTypeRecord, "The first type record of the object's .debug$T section leaves the section.",
                          "Recompile the translation unit.");
    }
    const std::uint16_t kind = rd16(d + 6);
    const std::uint8_t* body = d + 8;
    const std::uint32_t body_len = reclen - 2;
    info.kind = DebugTInfo::Kind::Records;
    if (kind == LF_TYPESERVER2 && body_len >= 21) {
        info.kind = DebugTInfo::Kind::TypeServer;
        std::memcpy(info.server.id.guid.data(), body, 16);
        info.server.id.age = rd32(body + 16);
        info.server.pdb_path.assign(reinterpret_cast<const char*>(body + 20), cv::cstr_len(body + 20, body_len - 20));
    } else if (kind == LF_PRECOMP && body_len >= 13) {
        info.kind = DebugTInfo::Kind::Precompiled;
        const std::uint64_t first = static_cast<std::uint64_t>(rd32(body)) + rd32(body + 4);
        info.first_own_index = first > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(first);
        info.precompiled_object.assign(reinterpret_cast<const char*>(body + 12), cv::cstr_len(body + 12, body_len - 12));
    }
    return info;
}

Result<ObjectTypes> ObjectTypes::parse(std::span<const std::uint8_t> section) {
    Result<DebugTInfo> info = inspect_debug_t(section);
    if (!info) {
        return info.error();
    }
    if (info->kind == DebugTInfo::Kind::TypeServer) {
        return Diag::make(diag::kNoObjectTypes,
                          "The object's .debug$T section only refers to the compiler PDB '" + info->server.pdb_path + "' (the object was compiled with /Zi).",
                          "Read the types from that PDB, or compile the translation unit with /Z7.");
    }
    ObjectTypes out;
    out.bytes_ = section;
    out.info_ = *info;
    out.first_index_ = info->kind == DebugTInfo::Kind::Precompiled ? info->first_own_index : 0x1000u;
    if (out.first_index_ < 0x1000u) {
        return Diag::make(diag::kBadTypeRecord, "The LF_PRECOMP record of the object's .debug$T section names an invalid type index range.", "Recompile the translation unit.");
    }
    const std::uint8_t* d = section.data();
    const std::uint64_t size = section.size();
    std::size_t count = 0;
    for (int pass = 0; pass < 2; ++pass) {
        bool first = true;
        const bool ok = cv::for_each_record(d, 4, size, [&](const cv::Record& r) {
            if (first && out.info_.kind == DebugTInfo::Kind::Precompiled) {
                first = false;  // the LF_PRECOMP record itself has no index
                return;
            }
            first = false;
            if (pass == 0) {
                ++count;
            } else {
                out.offsets_.push_back(static_cast<std::uint32_t>(r.offset));
            }
        });
        if (!ok) {
            return Diag::make(diag::kBadTypeRecord, "A type record of the object's .debug$T section leaves the section.", "Recompile the translation unit.");
        }
        if (pass == 0) {
            if (size > 0xFFFFFFF0ull || count > 0xFFFFFFFFull - out.first_index_) {
                return Diag::make(diag::kBadTypeRecord, "The object's .debug$T section is too large.", "Recompile the translation unit.");
            }
            out.offsets_.reserve(count);
        }
    }
    return out;
}

bool ObjectTypes::find_type(std::uint32_t index, TypeRecord& out) const {
    if (index < first_index_ || index - first_index_ >= offsets_.size()) {
        return false;
    }
    // parse() checked that every record lies inside the section.
    const std::uint8_t* p = bytes_.data() + offsets_[index - first_index_];
    out.kind = rd16(p + 2);
    out.body = p + 4;
    out.body_len = static_cast<std::uint32_t>(rd16(p)) - 2u;
    return true;
}

Result<std::vector<ObjectProc>> read_object_procs(std::span<const std::uint8_t> section) {
    std::vector<ObjectProc> out;
    if (section.empty()) {
        return out;
    }
    const auto bad = [](const char* why) {
        return Diag::make(diag::kBadObjectSymbols, std::string("An object's .debug$S section is malformed (") + why + ").", "Recompile the translation unit.");
    };
    if (section.size() < 4 || rd32(section.data()) != kC13Signature) {
        return bad("it does not start with the C13 signature");
    }
    if (section.size() > 0xFFFFFFF0ull) {
        return bad("it is larger than 4 GiB");
    }
    const std::uint8_t* d = section.data();
    const std::uint64_t size = section.size();
    std::uint64_t off = 4;
    while (size - off >= 8) {
        const std::uint32_t kind = rd32(d + off);
        const std::uint32_t len = rd32(d + off + 4);
        const std::uint64_t begin = off + 8;
        if (len > size - begin) {
            return bad("a subsection leaves the section");
        }
        if ((kind & ~kSubsectionIgnore) == kSubsectionSymbols && (kind & kSubsectionIgnore) == 0) {
            // Scopes do not cross a subsection. A procedure record too short to read still opens its scope, so that its
            // statics are not taken for those of an enclosing one.
            constexpr std::uint32_t kNoProc = 0xFFFFFFFFu;
            ScopeStack scopes;
            std::vector<std::uint32_t> path;
            const bool ok = cv::for_each_record(d, begin, begin + len, [&](const cv::Record& r) {
                if (cv::is_proc(r.kind)) {
                    if (r.body_len < 36) {
                        scopes.open(ScopeStack::Kind::Proc, kNoProc);
                        return;
                    }
                    ObjectProc p;
                    p.offset_field = static_cast<std::uint32_t>(r.offset + 4 + 28);
                    p.type_index = rd32(r.body + 24);
                    p.type_is_id = r.kind == cv::S_LPROC32_ID || r.kind == cv::S_GPROC32_ID || r.kind == cv::S_LPROC32_DPC_ID;
                    p.global = cv::is_global_proc(r.kind);
                    p.name = std::string_view(reinterpret_cast<const char*>(r.body + 35), cv::cstr_len(r.body + 35, r.body_len - 35));
                    scopes.open(ScopeStack::Kind::Proc, static_cast<std::uint32_t>(out.size()));
                    out.push_back(std::move(p));
                    return;
                }
                if (r.kind == cv::S_INLINESITE || r.kind == cv::S_INLINESITE2) {
                    // WP-60 (R-57): what the procedure inlined (parent u32, end u32, inlinee u32, ...), at any depth.
                    std::uint32_t proc = kNoProc;
                    bool plain = false;
                    if (scopes.locate(proc, path, plain) && proc != kNoProc) {
                        ObjectProc& owner = out[proc];
                        if (r.body_len < 12 || owner.inlinees.size() >= kMaxProcStatics) {
                            owner.inlinees_complete = false;
                        } else if (const std::uint32_t id = rd32(r.body + 8); std::find(owner.inlinees.begin(), owner.inlinees.end(), id) == owner.inlinees.end()) {
                            owner.inlinees.push_back(id);
                        }
                    }
                    scopes.open(ScopeStack::Kind::Other);
                    return;
                }
                if (cv::is_scope_opener(r.kind)) {
                    scopes.open(r.kind == cv::S_BLOCK32 ? ScopeStack::Kind::Block : ScopeStack::Kind::Other);
                    return;
                }
                if (cv::is_scope_closer(r.kind)) {
                    scopes.close();
                    return;
                }
                // WP-46: the types of the procedure's own local variables (S_LOCAL; S_REGREL32 / S_BPREL32 of cl at /Od), not
                // those of an inlined call (an inline site scope).
                if ((r.kind == kSymLocal && r.body_len >= 6) || ((r.kind == kSymRegRel32 || r.kind == kSymBpRel32) && r.body_len >= 8)) {
                    std::uint32_t proc = kNoProc;
                    bool plain = false;
                    // (Second review: a parameter, S_LOCAL with fIsParam, is not counted: a template that takes the closure is not
                    // the function that creates it.)
                    const bool parameter = r.kind == kSymLocal && (rd16(r.body + 4) & 1u) != 0;
                    if (!parameter && scopes.locate(proc, path, plain) && proc != kNoProc && plain && out[proc].local_types.size() < kMaxProcStatics) {
                        out[proc].local_types.push_back(rd32(r.body + (r.kind == kSymLocal ? 0 : 4)));
                    }
                    return;
                }
                const bool thread = r.kind == cv::S_LTHREAD32 || r.kind == cv::S_GTHREAD32;
                if ((r.kind != cv::S_LDATA32 && r.kind != cv::S_GDATA32 && !thread) || r.body_len < 11) {
                    return;
                }
                std::uint32_t proc = kNoProc;
                bool plain = false;
                if (!scopes.locate(proc, path, plain) || proc == kNoProc) {
                    return;  // file scope, or inside a procedure whose record could not be read
                }
                ObjectProc& owner = out[proc];
                if (owner.statics.size() >= kMaxProcStatics) {
                    owner.statics_complete = false;
                    return;
                }
                ProcStatic s;
                s.name.assign(reinterpret_cast<const char*>(r.body + 10), cv::cstr_len(r.body + 10, r.body_len - 10));
                s.type_index = rd32(r.body);
                s.thread_storage = thread;
                s.offset_field = static_cast<std::uint32_t>(r.offset + 4 + 4);
                s.block_path = path;
                s.plain_scopes = plain;
                owner.statics.push_back(std::move(s));
            });
            if (!ok) {
                return bad("a symbol record leaves its subsection");
            }
        }
        off = begin + len;
        off = (off + 3) & ~static_cast<std::uint64_t>(3);
        if (off > size) {
            break;
        }
    }
    return out;
}

Result<void> read_c13_inlinees(std::span<const std::uint8_t> subsections, C13Inlinees& out) {
    constexpr std::uint32_t kInlineeLines = 0xF6;
    constexpr std::uint32_t kFileChecksums = 0xF4;
    const std::uint8_t* d = subsections.data();
    const std::uint64_t size = subsections.size();
    std::uint64_t off = 0;
    while (size - off >= 8) {
        const std::uint32_t kind = rd32(d + off);
        const std::uint32_t len = rd32(d + off + 4);
        const std::uint64_t begin = off + 8;
        if (len > size - begin) {
            return Diag::make(diag::kBadObjectSymbols, "A C13 line subsection leaves its section.", "Recompile the translation unit.");
        }
        if (kind == kFileChecksums) {
            if (out.checksum_tables++ == 0) {
                out.checksums.assign(d + begin, d + begin + len);
            }
        } else if (kind == kInlineeLines && len >= 4) {
            // Signature 0: (inlinee, file id, line); 1 (extra files): then a count and that many more file ids.
            const std::uint32_t signature = rd32(d + begin);
            const std::uint64_t entry = signature == 1 ? 16 : 12;
            const std::uint64_t end = begin + len;
            std::uint64_t at = begin + 4;
            while (signature <= 1 && at <= end && end - at >= entry) {
                out.lines.emplace_back(rd32(d + at), rd32(d + at + 4));
                at += signature == 1 ? 16 + static_cast<std::uint64_t>(rd32(d + at + 12)) * 4 : 12;
            }
        }
        off = (begin + len + 3) & ~static_cast<std::uint64_t>(3);
    }
    return Result<void>();
}

std::optional<FileChecksum> inlinee_file(const C13Inlinees& c13, std::uint32_t inlinee) {
    if (c13.checksum_tables != 1) {
        return std::nullopt;
    }
    std::optional<std::uint32_t> file;
    for (const auto& [id, f] : c13.lines) {
        if (id != inlinee) {
            continue;
        }
        if (file && *file != f) {
            return std::nullopt;
        }
        file = f;
    }
    // An entry: name offset u32, checksum size u8, kind u8, the checksum.
    if (!file || static_cast<std::uint64_t>(*file) + 6 > c13.checksums.size()) {
        return std::nullopt;
    }
    const std::uint8_t* e = c13.checksums.data() + *file;
    const std::size_t n = e[4];
    if (e[5] == 0 || n == 0 || static_cast<std::uint64_t>(*file) + 6 + n > c13.checksums.size()) {
        return std::nullopt;
    }
    FileChecksum out;
    out.kind = e[5];
    out.bytes.assign(e + 6, e + 6 + n);
    return out;
}

}  // namespace cml::pdb
