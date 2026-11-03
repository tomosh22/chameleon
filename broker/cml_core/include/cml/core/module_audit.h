#pragma once

// Audit of one module against the build requirements of plan section 4.2 and the unsupported cases of section 1
// (WP-18). One function does the work for both callers, so they can never disagree:
//
//   * `cml_broker check <module>` (broker/cml_cli) runs audit_module_file() offline and prints every check as PASS /
//     WARN / FAIL with its diagnostic id; the exit code is non-zero on any FAIL.
//   * ModuleIndex runs audit_opened_module() on the PDB it just opened when a module is enabled (spec.audit); a FAIL
//     fails the index with that finding's diagnostic ("enabling a module without them fails with a precise
//     message"), a WARN is published as a warning diagnostic.
//
// Checks (id = the diagnostic the check raises, also shown for a PASS):
//
//   CML1106 x64 image                          FAIL   CML1400/1410/1411.. PDB found and matches   FAIL
//   CML1101 full PDB (/DEBUG:FULL)             FAIL   CML1102 compile command recorded            FAIL (all) / WARN (some)
//   CML1103 no /GL                             FAIL   CML1104 no /guard:cf                        FAIL
//   CML1105 /Gy (function-level linking)       FAIL when absent, WARN when it cannot be told (ADR-0026)
//   CML1504 no /clr, /ZW                       FAIL
//   CML1601 /FUNCTIONPADMIN + /hotpatch        WARN   CML1602 /OPT:NOREF /OPT:NOICF              WARN
//   CML1603 /Ob0 or /Od on iterated modules    WARN
//
// CML3104 (a changed small function of an optimised compiland may have inlined copies elsewhere) is the same
// condition as CML1603 seen per function at reload time: stale_inline_warning() raises it, the pipeline calls it.
//
// Portable; no threads.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cml/ipc/json.h"
#include "cml/pdb/index.h"
#include "cml/util/result.h"

namespace cml::core {

namespace audit_diag {
// CML1600..CML1699 (ADR-0031). CML1101..1106 and CML1504 keep the ids docs/KNOWN_LIMITATIONS.md gave them.
inline constexpr std::uint32_t kFastlinkPdb = 1101;        // /DEBUG:FASTLINK (or a PDB without function symbols)
inline constexpr std::uint32_t kNoCompileCommand = 1102;   // compilands without a recorded command line
inline constexpr std::uint32_t kWholeProgram = 1103;       // /GL
inline constexpr std::uint32_t kGuardCf = 1104;            // /guard:cf
inline constexpr std::uint32_t kNoFunctionLevelLinking = 1105;  // /Gy absent or unknown
inline constexpr std::uint32_t kNotX64 = 1106;             // x86, ARM64, ... (and the PE reader's CML1303)
inline constexpr std::uint32_t kManaged = 1504;            // /clr, /ZW compilands
inline constexpr std::uint32_t kNoHotpatchPadding = 1601;  // functions without 6 bytes of padding before them
inline constexpr std::uint32_t kOptFoldsOrStrips = 1602;   // /OPT:REF or /OPT:ICF in effect
inline constexpr std::uint32_t kInliningEnabled = 1603;    // optimised compilands with inlining on
inline constexpr std::uint32_t kStaleInlinedCopies = 3104; // per function, raised at reload time
}  // namespace audit_diag

enum class Verdict : std::uint8_t { Pass, Warn, Fail };
const char* to_string(Verdict verdict) noexcept;  // "PASS" "WARN" "FAIL"

struct AuditFinding {
    std::string check;       // short name: "image", "pdb", "debug-info", "compile-command", "gl", "guard-cf", "gy", ...
    Verdict verdict = Verdict::Pass;
    std::uint32_t id = 0;    // the diagnostic this check raises
    std::string message;     // one line; names the module / compiland
    std::string what_to_do;  // for WARN and FAIL
    std::string evidence;    // how the verdict was reached (always filled, shown with --verbose)

    Diag diag() const { return Diag::make(id, message, what_to_do); }
};

struct ModuleAudit {
    std::string module_path;
    std::string pdb_path;
    std::size_t compilands_total = 0;     // DBI modules
    std::size_t compilands_examined = 0;  // source compilands that were looked at
    std::vector<AuditFinding> findings;   // in the order of the table above

    std::size_t count(Verdict verdict) const noexcept;
    bool has_failures() const noexcept { return count(Verdict::Fail) != 0; }
    const AuditFinding* first_failure() const noexcept;
    const AuditFinding* find(const std::string& check) const noexcept;
};

struct AuditOptions {
    std::size_t max_compilands = 4000;       // source compilands examined (the rest are counted, not read)
    std::size_t max_functions_sampled = 64;  // functions whose preceding bytes are checked for padding
    std::size_t max_code_samples = 16;       // code contributions looked up for function symbols (FASTLINK heuristic)
    std::uint64_t max_object_bytes = 8u << 20;    // one recorded .obj read for /GL and /Gy evidence
    std::uint64_t object_budget_bytes = 128u << 20;  // all of them
};

// Offline entry point: loads the PE at `module_path`, opens its PDB (recorded path, next to the PE, override_dirs) and
// audits. A module or PDB that cannot be read is a FAIL finding (CML1300.., CML1106, CML14xx), never an error.
ModuleAudit audit_module_file(const std::string& module_path, const pdb::OpenOptions& pdb_options = {},
                              const AuditOptions& options = {});

// The same checks on an index the caller already opened (ModuleIndex). The PE is read again from `module_path`.
ModuleAudit audit_opened_module(const std::string& module_path, const pdb::PdbIndex& index, const AuditOptions& options = {});

// ---- reporting ------------------------------------------------------------------------------------------------
// Text for the console: one line per finding, "  FAIL CML1103 message" plus "       What to do: ..." for WARN and FAIL
// and the evidence lines with `verbose`; ends with a summary line.
std::string format_audit_text(const ModuleAudit& audit, bool verbose);
ipc::json::Value audit_to_json(const ModuleAudit& audit);

// ---- pieces the pipeline and the tests use ---------------------------------------------------------------------
// What the effective link options of a module are, from the command line of its `* Linker *` compiland.
struct LinkOptionsFacts {
    bool known = false;                  // a command line was recorded
    std::optional<bool> debug_fastlink;  // /DEBUG:FASTLINK given (nullopt: no /DEBUG option seen)
    bool opt_ref = false;                // /OPT:REF in effect
    bool opt_icf = false;                // /OPT:ICF in effect
    bool function_pad_min = false;       // /FUNCTIONPADMIN given
    bool guard_cf = false;               // /GUARD:CF given
    std::string detail;
};
LinkOptionsFacts parse_link_options(const std::string& linker_command);

// True when the recorded compile command allows inlining: optimised (/O1 /O2 /Ox /Os, -O1.. -O3) and no /Ob0 or
// -fno-inline. A command with no /O option is /Od.
bool command_allows_inlining(const std::string& compile_command);

// Size in bytes up to which a changed function counts as "small" for CML3104.
inline constexpr std::uint32_t kSmallFunctionBytes = 128;

// CML3104 (warning) when `compiland` was compiled with inlining and the changed function `display` (`old_size` bytes in the
// running module, 0 = unknown) is small: copies inlined into other translation units keep the old code.
std::optional<Diag> stale_inline_warning(const pdb::PdbIndex& index, std::uint32_t compiland, const std::string& display,
                                         std::uint32_t old_size, const std::string& module_name);

}  // namespace cml::core
