#pragma once

// Rewriting a recorded compile command into the command that builds a patch object (plan
// section 4.4). The rewrite is driven by DATA: a table of Rule entries per dialect (rules.cpp).
// The table is the documentation of what happens to every option; extending support for an option
// means adding one line to it.
//
// Matching. A command-line token that starts with '-' or '/' is an option; the text after that
// first character is matched against the rules in order, the first match wins, an option that
// matches no rule is kept verbatim (that is how /I, /D, /std, /EHsc, /MD, /Zc, /d1.., /d2.. and
// every other option survive). Tokens that are not options are inputs (source files) and are kept.
//
//   MatchKind::Exact   pattern == option text
//   MatchKind::Prefix  option text starts with pattern
//   MatchKind::Regex   ECMAScript regular expression, matched against the whole option text
//
//   Action::Keep       emit the token (plus `consume_next` following tokens, for options with a
//                      separate value such as cc1's "-o" "file")
//   Action::Drop       emit nothing (also drops `consume_next` following tokens)
//   Action::Replace    emit the token '/' + `replacement` (or '-' + replacement for the cc1
//                      dialect); an empty replacement drops it
//   Action::DropRest   drop this token and everything after it (cl's /link ...)
//   Action::Error      refuse: the rewrite fails with `diag`
//
// A rule with `warn_diag != 0` additionally records a warning Diag when it fires.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cml/toolchain/command_line.h"
#include "cml/toolchain/record.h"
#include "cml/util/result.h"

namespace cml::toolchain {

enum class MatchKind : std::uint8_t { Exact, Prefix, Regex };
enum class Action : std::uint8_t { Keep, Drop, Replace, DropRest, Error };

// What the rewriter needs to remember about an option beyond keep/drop.
enum class Tag : std::uint8_t {
    None,
    DebugInfo,         // /Z7 /Zi /ZI (replaced by /Z7)
    EditAndContinue,   // /ZI: implies /Gy, which is added explicitly when /ZI is replaced
    PchCreate,         // /Yc, cc1 -emit-pch / -building-pch-with-obj
    PchUse,            // /Yu, cc1 -include-pch
    PchFile,           // /Fp
    PdbFile,           // /Fd (value remembered for the private PDB copy)
    ObjectOutput,      // /Fo, cc1 -o
    Compile,           // /c
    Nologo,            // /nologo
    SourceSpec,        // /Tp<file> /Tc<file>
    ShowIncludes,      // /showIncludes
    CodeViewCommandLine,  // -gcodeview-command-line (cc1) or /clang:-gcodeview-command-line
    FunctionLinking,   // /Gy
};

struct Rule {
    MatchKind match;
    const char* pattern;
    Action action;
    const char* replacement;     // Replace only
    std::uint8_t consume_next;   // following tokens that belong to the option
    Tag tag;
    std::uint32_t diag;          // Error: id of the failure
    std::uint32_t warn_diag;     // 0 or the id of a warning recorded when the rule fires
    const char* why;             // documentation; Error/warning rules also use it as the message,
                                 // "{opt}" = the option as written, "{file}" = the source file
};

// The rule tables. Dialect::Cl uses cl_rules(); clang-cl driver form uses clang_cl_extra_rules()
// first and then cl_rules(); cc1 uses cc1_rules().
std::span<const Rule> cl_rules() noexcept;
std::span<const Rule> clang_cl_extra_rules() noexcept;
std::span<const Rule> cc1_rules() noexcept;

// How a TU that uses a precompiled header (/Yu) is recompiled when the original used /Zi. A PCH
// built with /Zi keeps its type information in the compiler PDB, which a /Z7 TU cannot use. The
// strategies are evidence-driven (docs/wp/WP-05.md, "PCH and /Zi"); cl only, clang-cl has no such
// distinction (its /Zi and /Z7 are the same).
enum class PchDebugMode : std::uint8_t {
    Auto,             // the library default (see kDefaultPchDebugMode)
    ForceZ7,          // /Zi -> /Z7 and keep /Yu /Fp
    KeepZiPrivatePdb, // keep /Zi, keep /Yu /Fp, point /Fd at a private copy of the compiler PDB
    DropPch,          // /Zi -> /Z7 and drop /Yu /Fp: the forced-include header is parsed again
};

// Decided from CI evidence on the pch fixture (docs/wp/WP-05.md, "PCH and /Zi").
inline constexpr PchDebugMode kDefaultPchDebugMode = PchDebugMode::KeepZiPrivatePdb;

struct RewriteOptions {
    std::string output_object;  // absolute path of the object to produce (required)
    PchDebugMode pch_debug = PchDebugMode::Auto;
    // KeepZiPrivatePdb: directory that receives the private compiler-PDB copy. Defaults to the
    // output object's directory.
    std::string private_pdb_dir;
    bool capture_includes = false;                 // add /showIncludes (cc1: -show-includes)
    bool ensure_codeview_command_line = true;      // clang: make sure re-reading the new object works (chained patches)
    FileReader file_reader;                        // response files; default: read from disk
};

struct RewriteResult {
    Dialect dialect = Dialect::Unknown;
    std::string compiler;                  // recorded compiler path (may not exist any more)
    std::string cwd;
    std::string source;                    // the translation unit
    std::vector<std::string> expanded_args;   // recorded arguments after @file expansion
    std::vector<std::string> args;         // the new argument list, WITHOUT the program name
    std::string command_line;              // join_command_line(args)
    std::vector<Diag> warnings;
    bool uses_pch = false;                 // /Yu or -include-pch present
    bool debug_info_replaced = false;      // /Zi or /ZI became /Z7
    // KeepZiPrivatePdb: copy `pdb_copy_from` to `pdb_copy_to` before running the compiler.
    std::string pdb_copy_from;
    std::string pdb_copy_to;
    std::vector<std::string> trace;        // one line per decision (also logged at trace level)
};

// Expands response files, applies the rules, sets the output object, adds /c and /nologo (cl
// dialects) or -emit-obj/-o (cc1), appends the source file when the recorded command did not
// contain it. Failures: CML1500/1501 (response files), CML1502 (unsupported compiler), CML1503 (no
// source), CML1504 (unsupported option), CML2102 (PCH-creating TU).
Result<RewriteResult> rewrite_for_patch(const CompileRecord& record, const RewriteOptions& options);

// "<session_tmp>\<module>\<hex16 compiland_hash>\<source stem>.obj" (plan section 4.4).
std::string patch_object_path(std::string_view session_tmp, std::string_view module_name,
                              std::uint64_t compiland_hash, std::string_view source);

}  // namespace cml::toolchain
