// WP-37: the canonical signature of a function from CodeView type records, and the readers of an object's
// `.debug$T` / `.debug$S` sections. Record layouts follow Microsoft's published cvinfo.h (microsoft-pdb, MIT) and
// LLVM's CodeView documentation; nothing here is copied from either.

#include "cml/pdb/type_records.h"

#include <cstdio>
#include <cstring>

#include "cml/pdb/diag_ids.h"
#include "detail/codeview.h"
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
                    out += names.unique;
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
            const bool ok = cv::for_each_record(d, begin, begin + len, [&](const cv::Record& r) {
                if (!cv::is_proc(r.kind) || r.body_len < 36) {
                    return;
                }
                ObjectProc p;
                p.offset_field = static_cast<std::uint32_t>(r.offset + 4 + 28);
                p.type_index = rd32(r.body + 24);
                p.type_is_id = r.kind == cv::S_LPROC32_ID || r.kind == cv::S_GPROC32_ID || r.kind == cv::S_LPROC32_DPC_ID;
                p.global = cv::is_global_proc(r.kind);
                p.name = std::string_view(reinterpret_cast<const char*>(r.body + 35), cv::cstr_len(r.body + 35, r.body_len - 35));
                out.push_back(p);
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

}  // namespace cml::pdb
