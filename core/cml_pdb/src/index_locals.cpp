// WP-23: the parameters of one function and the locations the compiler recorded for them (PdbIndex::proc_info).
//
// Walks the module symbol stream of the function's compiland up to the S_END of the function and keeps its direct children:
// S_FRAMEPROC (frame description), S_LOCAL + S_DEFRANGE_* (clang-cl, newer MSVC) and S_REGREL32 (MSVC at /Od). The parameter
// TYPES come from the function's type (LF_PROCEDURE / LF_MFUNCTION through LF_FUNC_ID / LF_MFUNC_ID for the *_ID procs) and
// are classified for the x64 calling convention: integer-class, floating-point class or "not understood" (which makes the
// caller refuse rather than guess).

#include <cstdio>
#include <cstring>
#include <string>

#include "cml/pdb/diag_ids.h"
#include "detail/codeview.h"
#include "detail/le.h"
#include "index_impl.h"

namespace cml::pdb {

using namespace detail;

namespace {

constexpr std::uint16_t S_FRAMEPROC = 0x1012;
constexpr std::uint16_t S_REGREL32 = 0x1111;
constexpr std::uint16_t S_LOCAL = 0x113E;
constexpr std::uint16_t S_DEFRANGE_REGISTER = 0x1141;
constexpr std::uint16_t S_DEFRANGE_FRAMEPOINTER_REL = 0x1142;
constexpr std::uint16_t S_DEFRANGE_SUBFIELD_REGISTER = 0x1143;
constexpr std::uint16_t S_DEFRANGE_FRAMEPOINTER_REL_FULL_SCOPE = 0x1144;
constexpr std::uint16_t S_DEFRANGE_REGISTER_REL = 0x1145;

constexpr std::uint16_t LF_MODIFIER = 0x1001;
constexpr std::uint16_t LF_POINTER = 0x1002;
constexpr std::uint16_t LF_PROCEDURE = 0x1008;
constexpr std::uint16_t LF_MFUNCTION = 0x1009;
constexpr std::uint16_t LF_ARGLIST = 0x1201;
constexpr std::uint16_t LF_CLASS = 0x1504;
constexpr std::uint16_t LF_STRUCTURE = 0x1505;
constexpr std::uint16_t LF_UNION = 0x1506;
constexpr std::uint16_t LF_ENUM = 0x1507;
constexpr std::uint16_t LF_INTERFACE = 0x1519;
constexpr std::uint16_t LF_FUNC_ID = 0x1601;
constexpr std::uint16_t LF_MFUNC_ID = 0x1602;

constexpr std::uint32_t kMaxParams = 256;
constexpr int kMaxTypeDepth = 8;

bool is_id_proc(std::uint16_t kind) noexcept {
    return kind == cv::S_LPROC32_ID || kind == cv::S_GPROC32_ID || kind == cv::S_LPROC32_DPC_ID;
}

std::string hex32(std::uint32_t v) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%X", v);
    return text;
}

// ---- type streams ----------------------------------------------------------------------------------------------------

const TypeTable* type_table(const PdbIndex::Impl& impl, int which, std::string* why) {
    std::lock_guard<std::mutex> lock(impl.types_mutex);
    if (!impl.types_tried[which]) {
        impl.types_tried[which] = true;
        const std::uint32_t stream = which == 0 ? 2u : 4u;
        const char* name = which == 0 ? "TPI" : "IPI";
        auto fail = [&](const char* what) {
            impl.types_error[which] = Diag::make(diag::kBadBuildInfo, std::string("The ") + name + " stream of '" + impl.path + "' is invalid (" + what + ").",
                                                 "Rebuild the module so the linker regenerates the PDB.");
        };
        auto table = std::make_unique<TypeTable>();
        table->data = impl.msf->read_stream(stream);
        const std::uint8_t* d = table->data.data();
        const std::size_t size = table->data.size();
        if (size < 56) {
            fail("stream missing or shorter than its header");
        } else {
            const std::uint32_t header_size = rd32(d + 4);
            table->type_begin = rd32(d + 8);
            const std::uint32_t record_bytes = rd32(d + 16);
            if (header_size < 56 || header_size > size || record_bytes > size - header_size) {
                fail("header sizes exceed the stream");
            } else {
                table->records_begin = header_size;
                table->records_end = static_cast<std::uint64_t>(header_size) + record_bytes;
                table->offsets.reserve(record_bytes / 24 + 8);
                const bool ok = cv::for_each_record(d + header_size, 0, record_bytes, [&](const cv::Record& r) {
                    table->offsets.push_back(static_cast<std::uint32_t>(r.offset));
                });
                if (!ok) {
                    fail("a record leaves the stream");
                } else {
                    impl.type_tables[which] = std::move(table);
                }
            }
        }
    }
    if (impl.type_tables[which] == nullptr) {
        if (why != nullptr && impl.types_error[which]) {
            *why = impl.types_error[which]->message;
        }
        return nullptr;
    }
    return impl.type_tables[which].get();
}

struct TypeRec {
    std::uint16_t kind = 0;
    const std::uint8_t* body = nullptr;
    std::uint32_t body_len = 0;
};

bool find_type(const TypeTable& t, std::uint32_t id, TypeRec& out) {
    if (id < t.type_begin || id - t.type_begin >= t.offsets.size()) {
        return false;
    }
    const std::uint8_t* base = t.data.data() + t.records_begin;
    const std::uint64_t region = t.records_end - t.records_begin;
    const std::uint64_t off = t.offsets[id - t.type_begin];
    if (off + 4 > region) {
        return false;
    }
    const std::uint32_t reclen = rd16(base + off);
    if (reclen < 2 || off + 2 + reclen > region) {
        return false;
    }
    out.kind = rd16(base + off + 2);
    out.body = base + off + 4;
    out.body_len = reclen - 2;
    return true;
}

ParamType make_int(std::uint32_t type_index, std::uint8_t size) {
    ParamType p;
    p.cls = ParamClass::Int;
    p.size = size;
    p.type_index = type_index;
    return p;
}

ParamType make_float(std::uint32_t type_index, std::uint8_t size) {
    ParamType p;
    p.cls = ParamClass::Float;
    p.size = size;
    p.type_index = type_index;
    return p;
}

ParamType unsupported(std::uint32_t type_index, std::string why) {
    ParamType p;
    p.type_index = type_index;
    p.why = std::move(why);
    return p;
}

// Simple (built-in) types: index < 0x1000. Bits 0-7 the kind, bits 8-10 the pointer mode.
ParamType classify_simple(std::uint32_t idx) {
    const std::uint32_t mode = (idx >> 8) & 7u;
    const std::uint32_t kind = idx & 0xFFu;
    if (mode != 0) {
        if (mode == 6) {  // 64-bit near pointer
            return make_int(idx, 8);
        }
        if (mode == 4) {  // 32-bit near pointer (a __ptr32)
            return make_int(idx, 4);
        }
        return unsupported(idx, "pointer of an unsupported kind (simple type " + hex32(idx) + ")");
    }
    switch (kind) {
        case 0x08: return make_int(idx, 4);   // HRESULT
        case 0x10: case 0x20: case 0x30: case 0x68: case 0x69: case 0x70: case 0x7c: return make_int(idx, 1);
        case 0x11: case 0x21: case 0x31: case 0x71: case 0x72: case 0x73: case 0x7a: return make_int(idx, 2);
        case 0x12: case 0x22: case 0x32: case 0x74: case 0x75: case 0x7b: return make_int(idx, 4);
        case 0x13: case 0x23: case 0x33: case 0x76: case 0x77: return make_int(idx, 8);
        case 0x40: return make_float(idx, 4);
        case 0x41: return make_float(idx, 8);
        default: break;
    }
    return unsupported(idx, "built-in type " + hex32(idx) + " is not an integer, pointer, float or double");
}

ParamType classify_type(const TypeTable* tpi, std::uint32_t idx, int depth) {
    if (idx < 0x1000) {
        return classify_simple(idx);
    }
    if (tpi == nullptr || depth > kMaxTypeDepth) {
        return unsupported(idx, "type " + hex32(idx) + " cannot be resolved");
    }
    TypeRec rec;
    if (!find_type(*tpi, idx, rec)) {
        return unsupported(idx, "type " + hex32(idx) + " is not in the type stream");
    }
    switch (rec.kind) {
        case LF_MODIFIER:
            if (rec.body_len >= 4) {
                return classify_type(tpi, rd32(rec.body), depth + 1);
            }
            break;
        case LF_POINTER:
            if (rec.body_len >= 8) {
                const std::uint32_t attr = rd32(rec.body + 4);
                const std::uint32_t mode = (attr >> 5) & 7u;
                if (mode == 2 || mode == 3) {
                    return unsupported(idx, "pointer to member (type " + hex32(idx) + ")");
                }
                const std::uint32_t size = (attr >> 13) & 0x3Fu;
                return make_int(idx, size == 4 ? 4 : 8);
            }
            break;
        case LF_ENUM:
            if (rec.body_len >= 12) {
                ParamType underlying = classify_type(tpi, rd32(rec.body + 4), depth + 1);
                underlying.type_index = idx;
                return underlying;
            }
            break;
        case LF_CLASS:
        case LF_STRUCTURE:
        case LF_UNION:
        case LF_INTERFACE:
            return unsupported(idx, "passed by value as a class, struct or union (type " + hex32(idx) + ")");
        default: break;
    }
    return unsupported(idx, "type " + hex32(idx) + " has an unsupported record kind 0x" + hex32(rec.kind).substr(2));
}

bool is_aggregate_type(const TypeTable* tpi, std::uint32_t idx, int depth) {
    if (idx < 0x1000 || tpi == nullptr || depth > kMaxTypeDepth) {
        return false;
    }
    TypeRec rec;
    if (!find_type(*tpi, idx, rec)) {
        return false;
    }
    if (rec.kind == LF_MODIFIER && rec.body_len >= 4) {
        return is_aggregate_type(tpi, rd32(rec.body), depth + 1);
    }
    return rec.kind == LF_CLASS || rec.kind == LF_STRUCTURE || rec.kind == LF_UNION || rec.kind == LF_INTERFACE;
}

// Fills info.params / params_known / params_error from the function's type.
void resolve_params(const PdbIndex::Impl& impl, std::uint32_t type_index, bool id_proc, ProcInfo& info) {
    std::string why;
    const TypeTable* tpi = type_table(impl, 0, &why);
    if (tpi == nullptr) {
        info.params_error = "the TPI stream is unreadable: " + why;
        return;
    }
    std::uint32_t function_type = type_index;
    TypeRec rec;
    if (id_proc) {
        const TypeTable* ipi = type_table(impl, 1, &why);
        if (ipi == nullptr) {
            info.params_error = "the IPI stream is unreadable: " + why;
            return;
        }
        if (!find_type(*ipi, type_index, rec) || (rec.kind != LF_FUNC_ID && rec.kind != LF_MFUNC_ID) || rec.body_len < 8) {
            info.params_error = "the function id " + hex32(type_index) + " is not an LF_FUNC_ID / LF_MFUNC_ID record";
            return;
        }
        function_type = rd32(rec.body + 4);
    }
    if (!find_type(*tpi, function_type, rec)) {
        info.params_error = "the function type " + hex32(function_type) + " is not in the TPI stream";
        return;
    }
    std::uint32_t return_type = 0;
    std::uint32_t this_type = 0;
    std::uint32_t arglist = 0;
    std::uint32_t declared = 0;
    if (rec.kind == LF_PROCEDURE && rec.body_len >= 12) {
        return_type = rd32(rec.body);
        declared = rd16(rec.body + 6);
        arglist = rd32(rec.body + 8);
    } else if (rec.kind == LF_MFUNCTION && rec.body_len >= 24) {
        return_type = rd32(rec.body);
        this_type = rd32(rec.body + 8);
        declared = rd16(rec.body + 14);
        arglist = rd32(rec.body + 16);
    } else {
        info.params_error = "the function type " + hex32(function_type) + " is neither LF_PROCEDURE nor LF_MFUNCTION";
        return;
    }
    if (is_aggregate_type(tpi, return_type, 0)) {
        info.params_error = "the function returns a class, struct or union by value (a hidden return-buffer parameter shifts the arguments)";
        return;
    }
    if (!find_type(*tpi, arglist, rec) || rec.kind != LF_ARGLIST || rec.body_len < 4) {
        info.params_error = "the argument list " + hex32(arglist) + " is not an LF_ARGLIST record";
        return;
    }
    const std::uint32_t count = rd32(rec.body);
    if (count > kMaxParams || count != declared || static_cast<std::uint64_t>(count) * 4 + 4 > rec.body_len) {
        info.params_error = "the argument list of the function type is inconsistent (" + std::to_string(count) + " entries, " + std::to_string(declared) + " declared)";
        return;
    }
    info.params.clear();
    if (this_type != 0) {
        info.params.push_back(make_int(this_type, 8));
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t arg = rd32(rec.body + 4 + 4 * i);
        if (arg == 0) {
            info.params.clear();
            info.params_error = "the function takes a variable argument list";
            return;
        }
        info.params.push_back(classify_type(tpi, arg, 0));
    }
    info.params_known = true;
}

std::uint16_t register_for_encoded(std::uint32_t encoded) noexcept {
    switch (encoded) {
        case 1: return kCvRegRsp;
        case 2: return kCvRegRbp;
        case 3: return kCvRegR13;
        default: return 0;
    }
}

}  // namespace

Result<ProcInfo> PdbIndex::proc_info(const SymbolView& proc) const {
    const Impl& impl = *impl_;
    if (!is_proc_kind(proc.kind) || proc.compiland >= impl.compilands.size()) {
        return Diag::make(diag::kCompilandOutOfRange, "proc_info needs a proc symbol of a compiland of this index.", "Pass a symbol returned by find_proc_by_rva.");
    }
    const Compiland& c = impl.compilands[proc.compiland];
    ProcInfo info;
    info.name = std::string(proc.name);
    info.rva = proc.rva;
    info.size = proc.size;
    info.compiland = proc.compiland;
    if (c.symbol_stream == 0xFFFF || c.symbol_bytes <= 4 || c.symbol_bytes > impl.msf->stream_size(c.symbol_stream)) {
        return Diag::make(diag::kBadModuleSymbols, "The symbols of module '" + std::string(c.object_name) + "' in '" + impl.path + "' are unreadable.",
                          "Rebuild the module so the linker regenerates the PDB.");
    }
    CountedBuffer scratch;
    std::size_t got = 0;
    const std::uint8_t* data = impl.msf->read_range(c.symbol_stream, 0, c.symbol_bytes, scratch, got);
    if (data == nullptr || got < c.symbol_bytes) {
        return Diag::make(diag::kBadModuleSymbols, "The symbols of module '" + std::string(c.object_name) + "' in '" + impl.path + "' cannot be read.",
                          "Rebuild the module so the linker regenerates the PDB.");
    }

    enum class State { Before, Inside, Done } state = State::Before;
    int depth = 0;
    bool id_proc = false;
    std::uint32_t type_index = 0;
    std::int64_t current_local = -1;  // index in info.variables of the S_LOCAL the next S_DEFRANGE_* belongs to

    auto add_range = [&](std::size_t body_offset, const std::uint8_t* b, std::uint32_t n, VarLocation location) {
        // LocalVariableAddrRange { u32 offStart; u16 isectStart; u16 cbRange }
        if (current_local < 0 || n < body_offset + 8) {
            return;
        }
        const std::uint32_t rva = impl.rva_or_none(rd16(b + body_offset + 4), rd32(b + body_offset));
        if (rva == kNoRva) {
            return;
        }
        location.start_rva = rva;
        location.length = rd16(b + body_offset + 6);
        info.variables[static_cast<std::size_t>(current_local)].locations.push_back(location);
    };

    const bool walked = cv::for_each_record(data, 4, c.symbol_bytes, [&](const cv::Record& r) {
        if (state == State::Done) {
            return;
        }
        const std::uint16_t k = r.kind;
        const std::uint8_t* b = r.body;
        const std::uint32_t n = r.body_len;
        if (state == State::Before) {
            if (cv::is_proc(k) && n >= 36 && rd16(b + 32) == proc.section && rd32(b + 28) == proc.offset) {
                state = State::Inside;
                depth = 1;
                id_proc = is_id_proc(k);
                type_index = rd32(b + 24);
            }
            return;
        }
        if (cv::is_scope_opener(k)) {
            ++depth;
            current_local = -1;
            return;
        }
        if (cv::is_scope_closer(k)) {
            if (--depth <= 0) {
                state = State::Done;
            }
            current_local = -1;
            return;
        }
        if (depth != 1) {
            return;
        }
        switch (k) {
            case S_FRAMEPROC:
                if (n >= 26) {
                    info.has_frameproc = true;
                    info.frame_bytes = rd32(b);
                    info.frame_flags = rd32(b + 22);
                    info.local_base_register = register_for_encoded((info.frame_flags >> 14) & 3u);
                    info.param_base_register = register_for_encoded((info.frame_flags >> 16) & 3u);
                    info.opt_for_speed = ((info.frame_flags >> 20) & 1u) != 0;
                }
                break;
            case S_REGREL32:
                if (n >= 11) {
                    ProcVariable v;
                    v.name.assign(reinterpret_cast<const char*>(b + 10), cv::cstr_len(b + 10, n - 10));
                    v.type_index = rd32(b + 4);
                    v.from_regrel = true;
                    VarLocation loc;
                    loc.kind = VarLocationKind::RegisterRelative;
                    loc.reg = rd16(b + 8);
                    loc.offset = static_cast<std::int32_t>(rd32(b));
                    v.locations.push_back(loc);
                    info.variables.push_back(std::move(v));
                    current_local = -1;
                }
                break;
            case S_LOCAL:
                if (n >= 7) {
                    ProcVariable v;
                    v.type_index = rd32(b);
                    const std::uint16_t flags = rd16(b + 4);
                    v.is_param = (flags & 1u) != 0;
                    v.optimized_out = (flags & 0x100u) != 0;
                    v.name.assign(reinterpret_cast<const char*>(b + 6), cv::cstr_len(b + 6, n - 6));
                    info.variables.push_back(std::move(v));
                    current_local = static_cast<std::int64_t>(info.variables.size()) - 1;
                }
                break;
            case S_DEFRANGE_FRAMEPOINTER_REL_FULL_SCOPE:
                if (current_local >= 0 && n >= 4) {
                    VarLocation loc;
                    loc.kind = VarLocationKind::FramePointerRelative;
                    loc.offset = static_cast<std::int32_t>(rd32(b));
                    loc.length = VarLocation::kWholeFunction;
                    info.variables[static_cast<std::size_t>(current_local)].locations.push_back(loc);
                }
                break;
            case S_DEFRANGE_FRAMEPOINTER_REL:
                if (n >= 4) {
                    VarLocation loc;
                    loc.kind = VarLocationKind::FramePointerRelative;
                    loc.offset = static_cast<std::int32_t>(rd32(b));
                    add_range(4, b, n, loc);
                }
                break;
            case S_DEFRANGE_REGISTER_REL:
                if (n >= 8) {
                    VarLocation loc;
                    loc.kind = VarLocationKind::RegisterRelative;
                    loc.reg = rd16(b);
                    loc.offset = static_cast<std::int32_t>(rd32(b + 4));
                    add_range(8, b, n, loc);
                }
                break;
            case S_DEFRANGE_REGISTER:
            case S_DEFRANGE_SUBFIELD_REGISTER:
                if (n >= 4) {
                    VarLocation loc;
                    loc.kind = VarLocationKind::Register;
                    loc.reg = rd16(b);
                    add_range(4, b, n, loc);
                }
                break;
            default: break;
        }
    });
    if (!walked) {
        return Diag::make(diag::kBadModuleSymbols, "The symbols of module '" + std::string(c.object_name) + "' in '" + impl.path + "' are corrupt (a record leaves the symbol area).",
                          "Rebuild the module so the linker regenerates the PDB.");
    }
    if (state == State::Before) {
        return Diag::make(diag::kBadModuleSymbols, "The symbol record of '" + std::string(proc.name) + "' was not found in module '" + std::string(c.object_name) + "'.",
                          "Rebuild the module so the linker regenerates the PDB.");
    }
    resolve_params(impl, type_index, id_proc, info);
    return info;
}

}  // namespace cml::pdb
