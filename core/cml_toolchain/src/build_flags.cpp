#include "cml/toolchain/build_flags.h"

#include <cstdio>

#include "fs_util.h"

namespace cml::toolchain {

const char* to_string(Tri value) noexcept {
    switch (value) {
        case Tri::Unknown: return "unknown";
        case Tri::No: return "no";
        case Tri::Yes: return "yes";
    }
    return "unknown";
}

const char* to_string(Evidence e) noexcept {
    switch (e) {
        case Evidence::None: return "none";
        case Evidence::ObjectFormat: return "object-format";
        case Evidence::ObjectSections: return "object-sections";
        case Evidence::Compile3: return "S_COMPILE3";
        case Evidence::CommandLine: return "command-line";
        case Evidence::CommandLineImplied: return "command-line-implied";
        case Evidence::CommandLineAbsent: return "command-line-absent";
        case Evidence::PeLoadConfig: return "pe-load-config";
    }
    return "none";
}

const char* to_string(ObjectKind kind) noexcept {
    switch (kind) {
        case ObjectKind::TooSmall: return "too-small";
        case ObjectKind::Coff: return "coff";
        case ObjectKind::CoffBigobj: return "coff-bigobj";
        case ObjectKind::IlLtcg: return "il-ltcg";
        case ObjectKind::LlvmBitcode: return "llvm-bitcode";
        case ObjectKind::Unknown: return "unknown";
    }
    return "unknown";
}

namespace {

std::uint32_t rd16(std::span<const std::byte> b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8);
}
std::uint32_t rd32(std::span<const std::byte> b, std::size_t off) {
    return rd16(b, off) | (rd16(b, off + 2) << 16);
}

// The /bigobj class id (ANON_OBJECT_HEADER_BIGOBJ), the same constant cml_coff checks.
constexpr unsigned char kBigobjClassId[16] = {0xC7, 0xA1, 0xBA, 0xD1, 0xEE, 0xBA, 0xA9, 0x4B,
                                               0xAF, 0x20, 0xFA, 0xF6, 0x6A, 0xA4, 0xDC, 0xB8};

// ---- command-line scanning --------------------------------------------------------------------

struct CmdState {
    Tri value = Tri::Unknown;
    bool implied = false;
    std::string token;
};

void set(CmdState& s, Tri v, bool implied, const std::string& token) {
    s.value = v;
    s.implied = implied;
    s.token = token;
}

struct CmdScan {
    CmdState gl;
    CmdState gy;
    CmdState hotpatch;
    CmdState guard_cf;
};

CmdScan scan_command_line(std::span<const std::string> args, Dialect dialect) {
    CmdScan scan;
    const bool cc1 = dialect == Dialect::ClangCc1;
    const bool cl = dialect == Dialect::Cl || dialect == Dialect::Unknown;
    for (const std::string& tok : args) {
        if (tok.size() < 2 || (tok[0] != '-' && tok[0] != '/')) {
            continue;
        }
        const std::string_view t = std::string_view(tok).substr(1);
        if (cc1) {
            if (t == "ffunction-sections") set(scan.gy, Tri::Yes, false, tok);
            else if (t == "fno-function-sections") set(scan.gy, Tri::No, false, tok);
            else if (t == "flto" || t.substr(0, 5) == "flto=" || t == "flto-unit") set(scan.gl, Tri::Yes, false, tok);  // not -flto-visibility-public-std
            else if (t == "cfguard" || t == "cfguard-no-checks") set(scan.guard_cf, Tri::Yes, false, tok);
            else if (t == "fms-hotpatch") set(scan.hotpatch, Tri::Yes, false, tok);
            continue;
        }
        if (t == "GL") set(scan.gl, Tri::Yes, false, tok);
        else if (t == "GL-") set(scan.gl, Tri::No, false, tok);
        else if (t == "flto" || t.substr(0, 5) == "flto=" || t == "-flto" || t.substr(0, 6) == "-flto=" ||
                 t == "clang:-flto" || t.substr(0, 12) == "clang:-flto=") set(scan.gl, Tri::Yes, false, tok);
        else if (t == "Gy") set(scan.gy, Tri::Yes, false, tok);
        else if (t == "Gy-") set(scan.gy, Tri::No, false, tok);
        else if (cl && t == "ZI") set(scan.gy, Tri::Yes, true, tok);
        else if (cl && (t == "O1" || t == "O2")) set(scan.gy, Tri::Yes, true, tok);
        else if (t == "hotpatch") set(scan.hotpatch, Tri::Yes, false, tok);
        else if (t == "guard:cf" || t.substr(0, 9) == "guard:cf,") set(scan.guard_cf, Tri::Yes, false, tok);
        else if (t == "guard:cf-") set(scan.guard_cf, Tri::No, false, tok);
    }
    return scan;
}

// ---- object analysis -----------------------------------------------------------------------

struct FunctionLayout {
    bool parsed = false;
    unsigned functions = 0;
    unsigned in_comdat = 0;
    unsigned in_plain_section = 0;
    unsigned no_duplicates = 0;  // COMDAT with selection NoDuplicates (cl: non-inline function under /Gy)
};

FunctionLayout analyse_functions(std::span<const std::byte> bytes) {
    FunctionLayout out;
    Result<coff::Object> obj = coff::Object::parse(bytes);
    if (!obj) {
        return out;
    }
    out.parsed = true;
    for (const coff::Symbol& sym : obj->symbols()) {
        if (!sym.is_function() || sym.section_number <= 0) {
            continue;
        }
        const coff::Section* sec = obj->section_of(sym);
        if (sec == nullptr || !sec->is_code()) {
            continue;
        }
        ++out.functions;
        if (sec->is_comdat()) {
            ++out.in_comdat;
            const auto info = obj->comdat(*sec);
            if (info && info->selection == coff::ComdatSelection::NoDuplicates) {
                ++out.no_duplicates;
            }
        } else {
            ++out.in_plain_section;
        }
    }
    return out;
}

std::string with_token(const char* prefix, const CmdState& s) {
    return std::string(prefix) + " '" + s.token + "'";
}

}  // namespace

ObjectKind classify_object(std::span<const std::byte> b) noexcept {
    if (b.size() < 4) {
        return ObjectKind::TooSmall;
    }
    // LLVM bitcode: 'B' 'C' 0xC0 0xDE, or the wrapper magic 0x0B17C0DE.
    if (b[0] == std::byte{'B'} && b[1] == std::byte{'C'} && b[2] == std::byte{0xC0} && b[3] == std::byte{0xDE}) {
        return ObjectKind::LlvmBitcode;
    }
    if (rd32(b, 0) == 0x0B17C0DEu) {
        return ObjectKind::LlvmBitcode;
    }
    // Anonymous object header: Sig1 = 0 (IMAGE_FILE_MACHINE_UNKNOWN), Sig2 = 0xFFFF.
    if (rd16(b, 0) == 0 && rd16(b, 2) == 0xFFFF) {
        if (b.size() >= 28) {
            bool is_bigobj = true;
            for (std::size_t i = 0; i < 16; ++i) {
                if (static_cast<unsigned char>(b[12 + i]) != kBigobjClassId[i]) {
                    is_bigobj = false;
                    break;
                }
            }
            if (is_bigobj) {
                return ObjectKind::CoffBigobj;
            }
        }
        return ObjectKind::IlLtcg;
    }
    if (b.size() >= 20) {
        const std::uint32_t machine = rd16(b, 0);
        if (machine == 0x8664 || machine == 0x014C || machine == 0xAA64 || machine == 0x01C4) {
            return ObjectKind::Coff;
        }
    }
    return ObjectKind::Unknown;
}

CompilandFlagFacts detect_compiland_flags(const CompilandFlagInputs& in) {
    CompilandFlagFacts facts;
    const CmdScan cmd = scan_command_line(in.args, in.dialect);
    const bool clang = is_clang(in.dialect);

    const bool have_object = !in.object_bytes.empty();
    if (have_object) {
        facts.object_kind = classify_object(in.object_bytes);
    }
    const bool object_decides_gl = facts.object_kind == ObjectKind::IlLtcg || facts.object_kind == ObjectKind::LlvmBitcode ||
                                   facts.object_kind == ObjectKind::Coff || facts.object_kind == ObjectKind::CoffBigobj;

    // ---- /GL -----------------------------------------------------------------------------
    {
        FlagFact& f = facts.whole_program_optimization;
        if (object_decides_gl) {
            const bool il = facts.object_kind == ObjectKind::IlLtcg || facts.object_kind == ObjectKind::LlvmBitcode;
            f = {il ? Tri::Yes : Tri::No, Evidence::ObjectFormat,
                 std::string("recorded object is ") + to_string(facts.object_kind)};
            if (in.have_command_line && cmd.gl.value != Tri::Unknown && (cmd.gl.value == Tri::Yes) != il) {
                f.detail += "; but the command line has " + cmd.gl.token + " (stale object?)";
            }
        } else if (in.compile3 != nullptr && in.compile3->ltcg) {
            f = {Tri::Yes, Evidence::Compile3, "S_COMPILE3 LTCG flag is set"};
        } else if (in.have_command_line) {
            if (cmd.gl.value != Tri::Unknown) {
                f = {cmd.gl.value, Evidence::CommandLine, with_token("command line has", cmd.gl)};
            } else {
                f = {Tri::No, Evidence::CommandLineAbsent, "no /GL on the command line"};
            }
        } else if (in.compile3 != nullptr) {
            f = {Tri::No, Evidence::Compile3, "S_COMPILE3 LTCG flag is clear (no command line, no object)"};
        }
    }

    // ---- /Gy -----------------------------------------------------------------------------
    {
        FlagFact& f = facts.function_level_linking;
        FunctionLayout layout;
        if (facts.object_kind == ObjectKind::Coff || facts.object_kind == ObjectKind::CoffBigobj) {
            layout = analyse_functions(in.object_bytes);
        }
        Tri from_object = Tri::Unknown;
        std::string object_detail;
        if (layout.parsed && layout.functions > 0) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%u functions: %u in COMDAT sections (%u NoDuplicates), %u in ordinary sections",
                          layout.functions, layout.in_comdat, layout.no_duplicates, layout.in_plain_section);
            object_detail = buf;
            if (layout.in_plain_section > 0) {
                from_object = Tri::No;
            } else if (layout.no_duplicates > 0 || clang) {
                from_object = Tri::Yes;
            } else {
                object_detail += " (only inline/template COMDATs: not conclusive for cl)";
            }
        }
        if (in.have_command_line && cmd.gy.value != Tri::Unknown && !cmd.gy.implied) {
            f = {cmd.gy.value, Evidence::CommandLine, with_token("command line has", cmd.gy)};
            if (from_object != Tri::Unknown && from_object != cmd.gy.value) {
                f.detail += "; but the object says " + std::string(to_string(from_object)) + " (" + object_detail + ")";
            }
        } else if (from_object != Tri::Unknown) {
            f = {from_object, Evidence::ObjectSections, object_detail};
        } else if (in.have_command_line && cmd.gy.value != Tri::Unknown) {
            f = {cmd.gy.value, Evidence::CommandLineImplied, with_token("implied by", cmd.gy)};
        } else if (in.have_command_line) {
            f = {Tri::No, Evidence::CommandLineAbsent, "no /Gy (and nothing that implies it) on the command line"};
        }
    }

    // ---- /hotpatch -----------------------------------------------------------------------
    {
        FlagFact& f = facts.hotpatch;
        if (in.compile3 != nullptr && in.compile3->hotpatch) {
            f = {Tri::Yes, Evidence::Compile3, "S_COMPILE3 hotpatch flag is set"};
        } else if (in.have_command_line && cmd.hotpatch.value == Tri::Yes) {
            f = {Tri::Yes, Evidence::CommandLine, with_token("command line has", cmd.hotpatch)};
        } else if (in.compile3 != nullptr && !clang) {
            f = {Tri::No, Evidence::Compile3, "S_COMPILE3 hotpatch flag is clear"};
        } else if (in.have_command_line) {
            f = {Tri::No, Evidence::CommandLineAbsent, "no /hotpatch on the command line"};
        }
    }

    // ---- /guard:cf (compile time) ---------------------------------------------------------
    {
        FlagFact& f = facts.guard_cf;
        if (in.have_command_line) {
            if (cmd.guard_cf.value != Tri::Unknown) {
                f = {cmd.guard_cf.value, Evidence::CommandLine, with_token("command line has", cmd.guard_cf)};
            } else {
                f = {Tri::No, Evidence::CommandLineAbsent, "no /guard:cf on the command line"};
            }
        }
    }
    return facts;
}

FlagFact detect_module_guard_cf(const pe::Image& image) {
    const bool bit = (image.optional_header().dll_characteristics & pe::dllchar::GuardCf) != 0;
    Result<pe::LoadConfig> lc = image.load_config();
    char buf[200];
    if (!lc || !lc->present) {
        return {bit ? Tri::Yes : Tri::No, Evidence::PeLoadConfig,
                bit ? "DllCharacteristics GUARD_CF is set (no readable load config)"
                    : "DllCharacteristics GUARD_CF is clear (no readable load config)"};
    }
    const bool table = lc->guard_cf_function_table != 0 && lc->guard_cf_function_count > 0;
    if (bit && table) {
        std::snprintf(buf, sizeof(buf), "DllCharacteristics GUARD_CF set; function table with %llu entries",
                      static_cast<unsigned long long>(lc->guard_cf_function_count));
        return {Tri::Yes, Evidence::PeLoadConfig, buf};
    }
    if (bit) {
        return {Tri::Yes, Evidence::PeLoadConfig, "DllCharacteristics GUARD_CF set; the function table is empty"};
    }
    if (table) {
        return {Tri::Unknown, Evidence::PeLoadConfig, "function table present but DllCharacteristics GUARD_CF is clear"};
    }
    std::snprintf(buf, sizeof(buf), "DllCharacteristics GUARD_CF clear, no function table%s",
                  lc->cf_instrumented() ? " (GuardFlags CF_INSTRUMENTED comes from the CRT libraries only)" : "");
    return {Tri::No, Evidence::PeLoadConfig, buf};
}

}  // namespace cml::toolchain
