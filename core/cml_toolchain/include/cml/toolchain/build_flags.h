#pragma once

// Detection of /GL, /Gy, /hotpatch and /guard:cf for an existing module (plan section 13, Q1;
// decision recorded in docs/wp/WP-05.md). The API returns FACTS with the evidence they come from;
// turning facts into CML1103/CML1104/CML1105 diagnostics is WP-18's job.
//
//   /GL        object format of the recorded .obj if it is present (an IL object is not COFF), else
//              the S_COMPILE3 LTCG flag, else the command line. (clang: bitcode objects, -flto.)
//   /Gy        explicit /Gy or /Gy- on the command line; else the recorded object (every defined
//              function in a COMDAT section = on, a function in an ordinary section = off); else
//              what other options imply (cl: /ZI, /O1, /O2); else off.
//   /hotpatch  the S_COMPILE3 hotpatch bit (authoritative for cl), else the command line.
//   /guard:cf  per compiland: the command line. Per module: DllCharacteristics GUARD_CF plus a
//              non-empty load-config function table; GuardFlags alone are not enough because the
//              CRT libraries set CF_INSTRUMENTED in every image (ADR-0015).

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "cml/coff/coff.h"
#include "cml/pdb/types.h"
#include "cml/pe/pe.h"
#include "cml/toolchain/record.h"

namespace cml::toolchain {

enum class Tri : std::uint8_t { Unknown, No, Yes };
const char* to_string(Tri value) noexcept;

enum class Evidence : std::uint8_t {
    None,              // nothing available; value is Unknown
    ObjectFormat,      // header of the recorded object (IL / bitcode / COFF)
    ObjectSections,    // COMDAT-ness of the functions in the recorded object
    Compile3,          // S_COMPILE3 flags in the PDB
    CommandLine,       // an explicit option on the recorded command line
    CommandLineImplied,// an option that implies the flag (cl: /ZI, /O1, /O2 imply /Gy)
    CommandLineAbsent, // command line known, option not on it: default (off)
    PeLoadConfig,      // PE DllCharacteristics + load config directory
};
const char* to_string(Evidence evidence) noexcept;

struct FlagFact {
    Tri value = Tri::Unknown;
    Evidence source = Evidence::None;
    std::string detail;  // human-readable: what exactly was seen, and conflicts with other sources
};

enum class ObjectKind : std::uint8_t {
    TooSmall,
    Coff,         // regular COFF
    CoffBigobj,   // /bigobj
    IlLtcg,       // cl /GL: anonymous object header that is not /bigobj
    LlvmBitcode,  // clang -flto: 'BC' 0xC0DE, or the 0x0B17C0DE wrapper
    Unknown,
};
const char* to_string(ObjectKind kind) noexcept;

// Looks at the first bytes only. Never fails.
ObjectKind classify_object(std::span<const std::byte> bytes) noexcept;

struct CompilandFlagInputs {
    // Recorded arguments with response files expanded (RewriteResult::expanded_args, or
    // expand_response_files()). Empty when unknown.
    std::span<const std::string> args;
    Dialect dialect = Dialect::Unknown;
    bool have_command_line = false;
    const pdb::CompileInfo* compile3 = nullptr;     // S_COMPILE3 of the compiland, if any
    std::span<const std::byte> object_bytes;        // the recorded .obj, if it could be read
};

struct CompilandFlagFacts {
    FlagFact whole_program_optimization;   // /GL, clang -flto
    FlagFact function_level_linking;       // /Gy, clang -ffunction-sections
    FlagFact hotpatch;                     // /hotpatch
    FlagFact guard_cf;                     // /guard:cf compiled in (command line only)
    ObjectKind object_kind = ObjectKind::TooSmall;
};

CompilandFlagFacts detect_compiland_flags(const CompilandFlagInputs& inputs);

// Module-level /guard:cf (see the file comment). Tri::No is returned with detail "CF_INSTRUMENTED
// from the CRT only" for the common case of an image whose objects were not built with /guard:cf.
FlagFact detect_module_guard_cf(const pe::Image& image);

}  // namespace cml::toolchain
