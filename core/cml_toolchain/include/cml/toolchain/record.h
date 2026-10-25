#pragma once

// What is known about how one translation unit was compiled (plan section 4.3, corrected by
// ADR-0012), and the entry points that build it.
//
//   * from the PDB:           record_from_build_env(cml::pdb::PdbIndex::build_env(compiland))
//   * from compile_commands:  record_from_compile_command(entry)   (see compile_commands.h)
//   * by hand:                fill CompileRecord directly (tests, other build systems)

#include <string>
#include <string_view>
#include <vector>

#include "cml/pdb/types.h"
#include "cml/util/result.h"

namespace cml::toolchain {

enum class RecordOrigin {
    Manual,
    PdbEnvBlock,      // S_ENVBLOCK
    PdbBuildInfo,     // S_BUILDINFO -> LF_BUILDINFO (what cl and clang-cl actually emit, ADR-0012)
    CompileCommands,  // compile_commands.json
};

const char* to_string(RecordOrigin origin) noexcept;

// The command-line dialect of the recorded arguments. It decides which rule table applies.
enum class Dialect {
    Unknown,
    Cl,             // cl.exe syntax ("/c /Zi -Fo..."): cl itself, and clang-cl in driver form
    ClangClDriver,  // clang-cl.exe in driver form (cl syntax plus /clang: and -Xclang)
    ClangCc1,       // the cc1 command clang records in CodeView ("-cc1" "-triple" ...), ADR-0012
};

const char* to_string(Dialect dialect) noexcept;
inline bool is_clang(Dialect d) noexcept { return d == Dialect::ClangClDriver || d == Dialect::ClangCc1; }

struct CompileRecord {
    RecordOrigin origin = RecordOrigin::Manual;
    std::string cwd;       // directory the compiler ran in; also the base for relative paths
    std::string compiler;  // path of the compiler binary as recorded (may no longer exist)
    // Recorded arguments WITHOUT the program name, tokenised with Windows rules but with response
    // files not yet expanded (the rewriter expands them against `cwd`).
    std::vector<std::string> args;
    std::string source;        // the translation unit's source file ("" if only in `args`)
    std::string recorded_pdb;  // compiler PDB recorded next to the command ("" for clang)
};

// CML1102 when the build environment has neither a command line nor a compiler. A leading program
// name inside the recorded command (".exe" token that is not an option, or equal to the recorded
// compiler) is stripped.
Result<CompileRecord> record_from_build_env(const pdb::BuildEnv& env);

// Tokenises a recorded command: Windows rules, except that a command starting with -cc1 (clang's
// CodeView record) uses clang's own quoting (split_command_line_posix).
std::vector<std::string> tokenize_recorded_command(std::string_view command);

// Detects the dialect from the compiler name and the first argument. Unknown for anything that is
// neither cl, clang-cl nor a cc1 command.
Dialect detect_dialect(const CompileRecord& record) noexcept;

// File name of `path` without directory, ASCII lower-cased ("C:\x\CL.EXE" -> "cl.exe").
std::string lower_base_name(std::string_view path);

}  // namespace cml::toolchain
