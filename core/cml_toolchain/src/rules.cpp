// The rewrite rules as data (plan section 4.4). See rewrite.h for the matching semantics. The
// tables are ordered: the first matching rule wins and options matching no rule are kept verbatim.
//
// Columns: match, pattern, action, replacement, consume_next, tag, diag, warn_diag, why.
#include <array>

#include "cml/toolchain/diag_ids.h"
#include "cml/toolchain/rewrite.h"

namespace cml::toolchain {

namespace {

constexpr std::uint32_t kNone = 0;

// clang-format off
constexpr std::array kClRules = {
    // ---- debug information -------------------------------------------------------------------
    // /Z7 puts CodeView into the object itself: nothing is shared with a compiler PDB, so
    // parallel patch compiles do not contend and the object is self-contained. (With a PCH see
    // PchDebugMode.)
    Rule{MatchKind::Exact, "Zi", Action::Replace, "Z7", 0, Tag::DebugInfo, kNone, kNone, "/Zi -> /Z7: debug info inside the object"},
    Rule{MatchKind::Exact, "ZI", Action::Replace, "Z7", 0, Tag::EditAndContinue, kNone, kNone, "/ZI -> /Z7 (and /Gy is added: /ZI implied it)"},
    Rule{MatchKind::Exact, "Z7", Action::Keep, "", 0, Tag::DebugInfo, kNone, kNone, "already /Z7"},
    // "/Fd:" <file> (colon form with the value in the next token) and "/Fd:<file>" / "/Fd<file>" (attached).
    Rule{MatchKind::Exact, "Fd:", Action::Drop, "", 1, Tag::PdbFile, kNone, kNone, "/Fd: <file>: no compiler PDB with /Z7 (value remembered for PchDebugMode::KeepZiPrivatePdb)"},
    Rule{MatchKind::Prefix, "Fd", Action::Drop, "", 0, Tag::PdbFile, kNone, kNone, "/Fd: no compiler PDB with /Z7 (value remembered for PchDebugMode::KeepZiPrivatePdb)"},
    Rule{MatchKind::Exact, "FS", Action::Drop, "", 0, Tag::None, kNone, kNone, "/FS only matters with a shared compiler PDB"},
    Rule{MatchKind::Exact, "Gm", Action::Drop, "", 0, Tag::None, kNone, kNone, "/Gm (minimal rebuild) is deprecated and conflicts with /Z7"},
    Rule{MatchKind::Exact, "Gm-", Action::Drop, "", 0, Tag::None, kNone, kNone, "/Gm- is the default"},
    // ---- build-system plumbing that must not reach the patch compile ---------------------------
    Rule{MatchKind::Regex, "MP[0-9]*", Action::Drop, "", 0, Tag::None, kNone, kNone, "/MP[n]: one process per patch compile; Chameleon parallelises itself"},
    Rule{MatchKind::Prefix, "showIncludes", Action::Drop, "", 0, Tag::ShowIncludes, kNone, kNone, "/showIncludes: re-added only when RewriteOptions::capture_includes is set"},
    // UnrealBuildTool writes "/sourceDependencies <file>" and "/experimental:log <file>" with the value in the next token.
    Rule{MatchKind::Exact, "sourceDependencies", Action::Drop, "", 1, Tag::None, kNone, kNone, "/sourceDependencies <file> writes a JSON dependency file into the build tree"},
    Rule{MatchKind::Prefix, "sourceDependencies:", Action::Drop, "", 0, Tag::None, kNone, kNone, "/sourceDependencies:<form> writes into the build tree"},
    Rule{MatchKind::Exact, "experimental:log", Action::Drop, "", 1, Tag::None, kNone, kNone, "/experimental:log <file> writes a SARIF file into the build tree"},
    Rule{MatchKind::Prefix, "analyze", Action::Drop, "", 0, Tag::None, kNone, kNone, "/analyze is far too slow for a hot-reload compile"},
    Rule{MatchKind::Exact, "Fo:", Action::Drop, "", 1, Tag::ObjectOutput, kNone, kNone, "/Fo: <file>: replaced by the patch object path"},
    Rule{MatchKind::Prefix, "Fo", Action::Drop, "", 0, Tag::ObjectOutput, kNone, kNone, "/Fo: replaced by the patch object path"},
    Rule{MatchKind::Exact, "Fe:", Action::Drop, "", 1, Tag::None, kNone, kNone, "/Fe: <file>: no link step"},
    Rule{MatchKind::Prefix, "Fe", Action::Drop, "", 0, Tag::None, kNone, kNone, "/Fe: no link step"},
    Rule{MatchKind::Prefix, "Fa", Action::Drop, "", 0, Tag::None, kNone, kNone, "/Fa: no assembly listing into the build tree"},
    Rule{MatchKind::Prefix, "FA", Action::Drop, "", 0, Tag::None, kNone, kNone, "/FA: no assembly listing into the build tree"},
    Rule{MatchKind::Prefix, "Fm", Action::Drop, "", 0, Tag::None, kNone, kNone, "/Fm: no map file"},
    Rule{MatchKind::Prefix, "FR", Action::Drop, "", 0, Tag::None, kNone, kNone, "/FR: no browse information"},
    Rule{MatchKind::Prefix, "Fr", Action::Drop, "", 0, Tag::None, kNone, kNone, "/Fr: no browse information"},
    Rule{MatchKind::Exact, "c", Action::Drop, "", 0, Tag::Compile, kNone, kNone, "/c is added exactly once"},
    Rule{MatchKind::Exact, "nologo", Action::Drop, "", 0, Tag::Nologo, kNone, kNone, "/nologo is added exactly once"},
    Rule{MatchKind::Exact, "link", Action::DropRest, "", 0, Tag::None, kNone, kNone, "/link and everything after it are linker options"},
    // ---- precompiled headers -----------------------------------------------------------------
    Rule{MatchKind::Prefix, "Yc", Action::Error, "", 0, Tag::PchCreate, diag::kPchCreatingTu, kNone, "{file} creates the precompiled header (/Yc); changes to it cannot be hot-reloaded."},
    Rule{MatchKind::Prefix, "Yu", Action::Keep, "", 0, Tag::PchUse, kNone, kNone, "/Yu: the existing PCH is reused"},
    Rule{MatchKind::Prefix, "Fp", Action::Keep, "", 0, Tag::PchFile, kNone, kNone, "/Fp: the existing PCH is reused"},
    // ---- whole-program optimisation: the object would be IL, not COFF ------------------------
    Rule{MatchKind::Exact, "GL", Action::Replace, "GL-", 0, Tag::None, kNone, diag::kWholeProgramDowngraded, "/GL objects are IL, not COFF; the patch object is compiled without whole-program optimisation (/GL-)"},
    // ---- cannot be recompiled into a native COFF object --------------------------------------
    Rule{MatchKind::Prefix, "clr", Action::Error, "", 0, Tag::None, diag::kUnsupportedOption, kNone, "{opt}: managed code (/clr) cannot be hot-reloaded"},
    Rule{MatchKind::Exact, "ZW", Action::Error, "", 0, Tag::None, diag::kUnsupportedOption, kNone, "{opt}: C++/CX (/ZW) cannot be hot-reloaded"},
    // ---- options the rewriter needs to see, kept verbatim ------------------------------------
    Rule{MatchKind::Exact, "Gy", Action::Keep, "", 0, Tag::FunctionLinking, kNone, kNone, "/Gy"},
    Rule{MatchKind::Exact, "Gy-", Action::Keep, "", 0, Tag::FunctionLinking, kNone, kNone, "/Gy-"},
    Rule{MatchKind::Prefix, "Tp", Action::Keep, "", 0, Tag::SourceSpec, kNone, kNone, "/Tp<file>: names the source"},
    Rule{MatchKind::Prefix, "Tc", Action::Keep, "", 0, Tag::SourceSpec, kNone, kNone, "/Tc<file>: names the source"},
    // Everything else is preserved verbatim, notably: /I /D /U /std /EH* /MD /MT /MDd /MTd /Zc:*
    // /O* /W* /w* /FI /external:* /permissive- /utf-8 /source-charset /guard:* /hotpatch /Gh /GH
    // /RTC* /GS /sdl /JMC /d1* /d2* /Zp /arch /fp /await /experimental:* /openmp /diagnostics:*.
};

constexpr std::array kClangClExtraRules = {
    Rule{MatchKind::Exact, "Xclang", Action::Keep, "", 1, Tag::None, kNone, kNone, "-Xclang <arg>: the argument belongs to the option"},
    // CMake passes the depfile flags as separate /clang: tokens: /clang:-MD /clang:-MT /clang:<obj> /clang:-MF /clang:<file>
    Rule{MatchKind::Regex, "clang:-M[FTQ]", Action::Drop, "", 1, Tag::None, kNone, kNone, "/clang:-MF|-MT|-MQ <value>: dependency-file output (value is the next /clang: token)"},
    Rule{MatchKind::Regex, "clang:-(MD|MMD|MP|MV|M[FTQ].+)", Action::Drop, "", 0, Tag::None, kNone, kNone, "/clang:-MD -MMD -MP and joined -MF<file>: dependency-file outputs"},
    Rule{MatchKind::Regex, "-?flto(=.*)?", Action::Drop, "", 0, Tag::None, kNone, diag::kWholeProgramDowngraded, "-flto objects are LLVM bitcode, not COFF; the patch is compiled without LTO"},
    Rule{MatchKind::Regex, "clang:-flto(=.*)?", Action::Drop, "", 0, Tag::None, kNone, diag::kWholeProgramDowngraded, "/clang:-flto objects are LLVM bitcode, not COFF; the patch is compiled without LTO"},
    Rule{MatchKind::Exact, "clang:-gcodeview-command-line", Action::Keep, "", 0, Tag::CodeViewCommandLine, kNone, kNone, "keeps the command line in the CodeView of the new object"},
    Rule{MatchKind::Exact, "gcodeview-command-line", Action::Keep, "", 0, Tag::CodeViewCommandLine, kNone, kNone, "keeps the command line in the CodeView of the new object"},
    Rule{MatchKind::Exact, "clang:-gno-codeview-command-line", Action::Drop, "", 0, Tag::None, kNone, kNone, "chained patches need the command line"},
    Rule{MatchKind::Exact, "gno-codeview-command-line", Action::Drop, "", 0, Tag::None, kNone, kNone, "chained patches need the command line"},
};

// cc1: tokens always start with '-', the text after it is matched. This is the command clang
// records (ADR-0012) and Chameleon replays as `<clang> -cc1 <args>`.
constexpr std::array kCc1Rules = {
    Rule{MatchKind::Exact, "o", Action::Drop, "", 1, Tag::ObjectOutput, kNone, kNone, "-o <file>: replaced by the patch object path"},
    // dependency-file outputs
    Rule{MatchKind::Exact, "dependency-file", Action::Drop, "", 1, Tag::None, kNone, kNone, "depfile output"},
    Rule{MatchKind::Exact, "MT", Action::Drop, "", 1, Tag::None, kNone, kNone, "depfile target"},
    Rule{MatchKind::Exact, "MQ", Action::Drop, "", 1, Tag::None, kNone, kNone, "depfile target"},
    Rule{MatchKind::Exact, "sys-header-deps", Action::Drop, "", 0, Tag::None, kNone, kNone, "depfile content"},
    Rule{MatchKind::Exact, "dependency-filter", Action::Drop, "", 1, Tag::None, kNone, kNone, "depfile content"},
    Rule{MatchKind::Exact, "module-dependency-dir", Action::Drop, "", 1, Tag::None, kNone, kNone, "depfile content"},
    Rule{MatchKind::Exact, "show-includes", Action::Drop, "", 0, Tag::ShowIncludes, kNone, kNone, "re-added only when RewriteOptions::capture_includes is set"},
    // clang-cl 20 records the option as --show-includes (CI evidence).
    Rule{MatchKind::Exact, "-show-includes", Action::Drop, "", 0, Tag::ShowIncludes, kNone, kNone, "re-added only when RewriteOptions::capture_includes is set"},
    Rule{MatchKind::Exact, "header-include-file", Action::Drop, "", 1, Tag::None, kNone, kNone, "include-tree output"},
    Rule{MatchKind::Exact, "header-include-format", Action::Drop, "", 1, Tag::None, kNone, kNone, "include-tree output"},
    Rule{MatchKind::Exact, "header-include-filtering", Action::Drop, "", 1, Tag::None, kNone, kNone, "include-tree output"},
    Rule{MatchKind::Exact, "coverage-notes-file", Action::Drop, "", 1, Tag::None, kNone, kNone, "no .gcno into the build tree"},
    // the output of Chameleon's own diagnostics parser must not contain escape codes
    Rule{MatchKind::Exact, "fcolor-diagnostics", Action::Drop, "", 0, Tag::None, kNone, kNone, "plain text diagnostics"},
    Rule{MatchKind::Exact, "fansi-escape-codes", Action::Drop, "", 0, Tag::None, kNone, kNone, "plain text diagnostics"},
    // LTO objects are bitcode, not COFF
    // Careful: -flto-visibility-public-std is an unrelated cc1 option that every clang-cl compile records.
    Rule{MatchKind::Regex, "flto(=.*)?|flto-unit|flto-jobs=.*|fwhole-program-vtables", Action::Drop, "", 0, Tag::None, kNone, diag::kWholeProgramDowngraded, "bitcode objects cannot be patched; compiled without LTO"},
    Rule{MatchKind::Exact, "emit-llvm-bc", Action::Replace, "emit-obj", 0, Tag::None, kNone, diag::kWholeProgramDowngraded, "bitcode objects cannot be patched; -emit-obj instead"},
    // PCH
    Rule{MatchKind::Exact, "emit-pch", Action::Error, "", 0, Tag::PchCreate, diag::kPchCreatingTu, kNone, "{file} creates the precompiled header (/Yc); changes to it cannot be hot-reloaded."},
    Rule{MatchKind::Exact, "building-pch-with-obj", Action::Error, "", 0, Tag::PchCreate, diag::kPchCreatingTu, kNone, "{file} creates the precompiled header (/Yc); changes to it cannot be hot-reloaded."},
    Rule{MatchKind::Exact, "include-pch", Action::Keep, "", 1, Tag::PchUse, kNone, kNone, "-include-pch <file>: the existing PCH is reused"},
    // kept verbatim, but the rewriter needs to know
    Rule{MatchKind::Exact, "gcodeview-command-line", Action::Keep, "", 0, Tag::CodeViewCommandLine, kNone, kNone, "keeps the command line in the CodeView of the new object"},
    Rule{MatchKind::Exact, "gno-codeview-command-line", Action::Drop, "", 0, Tag::None, kNone, kNone, "chained patches need the command line"},
    Rule{MatchKind::Exact, "ffunction-sections", Action::Keep, "", 0, Tag::FunctionLinking, kNone, kNone, "-ffunction-sections is clang's /Gy"},
    Rule{MatchKind::Exact, "fno-function-sections", Action::Keep, "", 0, Tag::FunctionLinking, kNone, kNone, "-fno-function-sections"},
    // Everything else is preserved verbatim: -triple, -D, -internal-isystem, -I, -std=, -fexceptions,
    // -fms-*, -debug-info-kind=, -gcodeview, -resource-dir, -main-file-name, -fdebug-compilation-dir=...
};
// clang-format on

}  // namespace

std::span<const Rule> cl_rules() noexcept {
    return kClRules;
}
std::span<const Rule> clang_cl_extra_rules() noexcept {
    return kClangClExtraRules;
}
std::span<const Rule> cc1_rules() noexcept {
    return kCc1Rules;
}

}  // namespace cml::toolchain
