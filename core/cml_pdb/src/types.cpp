#include <cstdio>
#include <string>

#include "cml/pdb/types.h"

namespace cml::pdb {

bool PdbId::is_zero() const noexcept {
    for (std::uint8_t b : guid) {
        if (b != 0) {
            return false;
        }
    }
    return age == 0;
}

std::string PdbId::to_string() const {
    // GUID layout: Data1 (u32 LE), Data2 (u16 LE), Data3 (u16 LE), Data4 (8 bytes).
    const std::uint32_t d1 = static_cast<std::uint32_t>(guid[0]) | (static_cast<std::uint32_t>(guid[1]) << 8) |
                             (static_cast<std::uint32_t>(guid[2]) << 16) | (static_cast<std::uint32_t>(guid[3]) << 24);
    const unsigned d2 = static_cast<unsigned>(guid[4]) | (static_cast<unsigned>(guid[5]) << 8);
    const unsigned d3 = static_cast<unsigned>(guid[6]) | (static_cast<unsigned>(guid[7]) << 8);
    char buf[80];
    std::snprintf(buf, sizeof buf, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X/%u", static_cast<unsigned>(d1), d2, d3,
                  static_cast<unsigned>(guid[8]), static_cast<unsigned>(guid[9]), static_cast<unsigned>(guid[10]),
                  static_cast<unsigned>(guid[11]), static_cast<unsigned>(guid[12]), static_cast<unsigned>(guid[13]),
                  static_cast<unsigned>(guid[14]), static_cast<unsigned>(guid[15]), static_cast<unsigned>(age));
    return std::string(buf);
}

const char* to_string(SymbolKind kind) noexcept {
    switch (kind) {
        case SymbolKind::GlobalProc: return "GPROC32";
        case SymbolKind::LocalProc: return "LPROC32";
        case SymbolKind::GlobalData: return "GDATA32";
        case SymbolKind::LocalData: return "LDATA32";
        case SymbolKind::GlobalThread: return "GTHREAD32";
        case SymbolKind::LocalThread: return "LTHREAD32";
        case SymbolKind::Udt: return "UDT";
        case SymbolKind::Public: return "PUB32";
    }
    return "?";
}

const char* CompileInfo::language_name() const noexcept {
    switch (language) {
        case 0: return "C";
        case 1: return "C++";
        case 2: return "Fortran";
        case 3: return "MASM";
        case 4: return "Pascal";
        case 5: return "Basic";
        case 6: return "COBOL";
        case 7: return "LINK";
        case 8: return "CVTRES";
        case 9: return "CVTPGD";
        case 0x10: return "C#";
        case 0x11: return "Visual Basic";
        case 0x12: return "ILASM";
        case 0x13: return "Java";
        case 0x14: return "JScript";
        case 0x15: return "MSIL";
        case 0x16: return "HLSL";
        default: return "unknown";
    }
}

const char* to_string(OptimizationHint hint) noexcept {
    switch (hint) {
        case OptimizationHint::Unknown: return "unknown";
        case OptimizationHint::Disabled: return "disabled (/Od)";
        case OptimizationHint::Size: return "size (/O1)";
        case OptimizationHint::Speed: return "speed (/O2)";
        case OptimizationHint::Full: return "full (/Ox)";
    }
    return "?";
}

const char* to_string(EnvSource source) noexcept {
    switch (source) {
        case EnvSource::None: return "none";
        case EnvSource::EnvBlock: return "S_ENVBLOCK";
        case EnvSource::BuildInfo: return "S_BUILDINFO";
    }
    return "?";
}

OptimizationHint optimization_hint_from_command(std::string_view command) noexcept {
    OptimizationHint hint = OptimizationHint::Unknown;
    std::size_t i = 0;
    while (i < command.size()) {
        while (i < command.size() && (command[i] == ' ' || command[i] == '\t' || command[i] == '"')) {
            ++i;
        }
        const std::size_t start = i;
        while (i < command.size() && command[i] != ' ' && command[i] != '\t' && command[i] != '"') {
            ++i;
        }
        const std::string_view tok = command.substr(start, i - start);
        if (tok.size() < 3 || (tok[0] != '/' && tok[0] != '-') || tok[1] != 'O') {
            continue;
        }
        const std::string_view opt = tok.substr(2);
        if (opt == "d") {
            hint = OptimizationHint::Disabled;
        } else if (opt == "1") {
            hint = OptimizationHint::Size;
        } else if (opt == "2") {
            hint = OptimizationHint::Speed;
        } else if (opt == "x") {
            hint = OptimizationHint::Full;
        } else if (opt == "s") {
            hint = OptimizationHint::Size;
        } else if (opt == "3") {
            hint = OptimizationHint::Full;
        } else if (opt == "0") {
            hint = OptimizationHint::Disabled;
        }
    }
    return hint;
}

std::optional<std::string_view> Compiland::env_value(std::string_view key) const noexcept {
    for (const EnvPair& p : env) {
        if (p.first == key) {
            return p.second;
        }
    }
    return std::nullopt;
}

std::optional<std::string_view> BuildEnv::get(std::string_view key) const noexcept {
    for (const auto& p : pairs) {
        if (p.first == key) {
            return std::string_view(p.second);
        }
    }
    return std::nullopt;
}

OptimizationHint BuildEnv::optimization() const noexcept {
    const auto cmd = command();
    return cmd ? optimization_hint_from_command(*cmd) : OptimizationHint::Unknown;
}

OptimizationHint Compiland::optimization() const noexcept {
    const auto cmd = command();
    return cmd ? optimization_hint_from_command(*cmd) : OptimizationHint::Unknown;
}

}  // namespace cml::pdb
