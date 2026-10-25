#pragma once

// The linker driver (plan section 4.6, corrected by ADR-0020 and ADR-0024), WP-09.
//
//   1. Which linker?   detect_module_linker() reads the MODULE: the linker version and the Rich header in the
//                      PE, the `* Linker *` compiland (S_COMPILE3 version string, S_ENVBLOCK, "exe") in its PDB.
//                      Both are cross-checked; the PDB wins a disagreement (CML3366 warning). The user can
//                      override the family (DetectLinkerOptions::override_family) and the binary
//                      (ResolveLinkerOptions::explicit_path).
//   2. The command.    build_link_command() produces the plan 4.6 command line. The driver ALWAYS passes
//                      /OPT:NOREF /OPT:NOICF (the precondition of every section locator, ADR-0024) and refuses
//                      (CML3360) any caller-supplied flag that re-enables REF or ICF, /FORCE, /INCREMENTAL, ...
//   3. Link.           link_patch() runs the linker (Windows), then read_linked_patch() opens the patch PDB with
//                      cml_pdb, resolves every section locator and every thunk slot to an RVA and releases the
//                      index before returning (ADR-0017: an open index maps the PDB read-only and a linker cannot
//                      overwrite a mapped PDB).
//
// Everything except link_patch(), resolve_linker() and the process parts is portable and unit-tested on every
// host.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cml/coff/anchors.h"
#include "cml/coff/binding.h"
#include "cml/pdb/index.h"
#include "cml/toolchain/invoke.h"
#include "cml/toolchain/link_diag_ids.h"
#include "cml/toolchain/tool_info.h"
#include "cml/util/result.h"

namespace cml::toolchain {

// ---- 1. which linker built the module ---------------------------------------------------------------------

// What the PE says. Parsed from the raw bytes (DOS header, e_lfanew, optional header) so synthetic images work.
struct PeLinkerFacts {
    bool valid = false;         // a PE32+ header was found
    unsigned major = 0;         // OptionalHeader.MajorLinkerVersion (link.exe: 14, MinorLinkerVersion = toolset)
    unsigned minor = 0;
    bool rich_header = false;   // "Rich" marker between the DOS header and the PE signature: written by link.exe,
                                // never by lld-link
};
PeLinkerFacts read_pe_linker_facts(std::span<const std::byte> pe_bytes) noexcept;

// What the module's PDB says about the `* Linker *` compiland.
struct PdbLinkerFacts {
    bool linker_module_found = false;
    bool has_compile3 = false;
    std::string compile_version;                 // S_COMPILE3 version string
    std::array<unsigned, 4> backend_version{};   // S_COMPILE3 backend version
    bool has_env_block = false;                  // S_ENVBLOCK in the linker module (lld-link writes one, link.exe does not)
    std::string exe_path;                        // the "exe" key of that block, if any
    std::string command;                         // the "cmd" key, if any
};
// A PDB without a linker module yields linker_module_found == false (not an error).
PdbLinkerFacts read_pdb_linker_facts(const pdb::PdbIndex& index);

struct DetectLinkerOptions {
    // The user's choice. Link or LldLink replaces the detection (the evidence is still collected and reported);
    // Unknown means "detect".
    ToolFamily override_family = ToolFamily::Unknown;
};

struct LinkerDetection {
    ToolFamily family = ToolFamily::Unknown;     // Link or LldLink
    ToolFamily from_pdb = ToolFamily::Unknown;
    ToolFamily from_pe = ToolFamily::Unknown;
    std::array<unsigned, 4> version{};           // linker version as recorded in the module (PE major.minor, else PDB)
    bool overridden = false;
    std::vector<std::string> evidence;           // one line per clue, for the log and the CI evidence
    std::vector<Diag> warnings;                  // CML3366 when PE and PDB disagree
};

// The pure decision. `pdb` may be null (no PDB available). CML3365 when nothing identifies the linker and there is no
// override.
Result<LinkerDetection> decide_linker_family(const PeLinkerFacts& pe, const PdbLinkerFacts* pdb,
                                             const DetectLinkerOptions& options = {});

// Reads the PE at `pe_path` and its PDB (located like pdb::PdbIndex::open_for_pe), decides, and releases the PDB index
// again before returning. A PDB that cannot be opened is a warning, not an error, as long as the PE alone decides.
// CML3367 when the PE cannot be read.
Result<LinkerDetection> detect_module_linker(const std::string& pe_path, const pdb::OpenOptions& pdb_options = {},
                                             const DetectLinkerOptions& options = {});

// ---- 2. the command line ----------------------------------------------------------------------------------

struct LinkCommandSpec {
    ToolFamily family = ToolFamily::Link;        // Link or LldLink
    std::vector<std::string> objects;            // anchored objects and binding.obj, in link order
    std::string out_dll;
    std::string out_pdb;
    std::uint64_t base = 0;                      // /BASE, a multiple of 64 KiB, non-zero
    // Extra linker arguments from the caller, appended after the driver's own and before the objects. Checked:
    // CML3360 for options that would break the locators or the patch, CML3361 for options the driver owns.
    std::vector<std::string> extra_args;
    // Warning numbers for /IGNORE:n,m. Empty (the default) passes no /IGNORE: the CI evidence is that the
    // plan 4.6 command prints no warning with either linker (docs/wp/WP-09.md).
    std::vector<unsigned> ignore_warnings;
};

// Formats a /BASE value: "0x180000000" (lower-case hex, no padding).
std::string format_base_argument(std::uint64_t base);

// The plan 4.6 command (without the program name), in this order:
//   /nologo /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 /DEBUG:FULL /INCREMENTAL:NO /OPT:NOREF /OPT:NOICF
//   /FUNCTIONPADMIN /FIXED /DYNAMICBASE:NO /BASE:0x... /PDB:<pdb> /OUT:<dll> [/IGNORE:...] [extra...] <objects...>
// Arguments are returned unquoted (quote_argument()/join_command_line() quote them when run).
// CML3362 for a request without objects, without output names, or with a base that is zero or not a multiple of 64 KiB.
Result<std::vector<std::string>> build_link_command(const LinkCommandSpec& spec);

// Checks the caller-supplied arguments alone (also used by build_link_command). `@file` arguments are expanded
// (response files are read from disk) before they are checked.
Result<void> check_extra_link_args(const std::vector<std::string>& extra_args);

// Whether the arguments need a response file: the command line (program + quoted arguments) is longer than `limit`
// characters (CreateProcess allows 32767).
bool needs_response_file(std::string_view program, std::span<const std::string> args, std::size_t limit = 30000);
// The bytes of the response file for `args`: UTF-16LE with a byte-order mark (the encoding both link.exe and lld-link
// read for non-ASCII paths), one quoted argument per line.
std::vector<std::byte> render_response_file(std::span<const std::string> args);

// A patch that binds nothing still needs a binding object: lld-link 20.1.8 crashes (access violation, WP-09 CI evidence)
// when it links one anchored clang-cl object with /DEBUG:FULL /NODEFAULTLIB and no second object, and does not when a
// binding object with a data placeholder is added. This is that object: a binding object with the single data placeholder
// `__cml_binding_pad`. link_patch() adds it by itself when LinkPatchRequest::binding_object is empty.
inline constexpr std::string_view kBindingPadSymbol = "__cml_binding_pad";
Result<std::vector<std::byte>> make_padding_binding_object();

// What the linker printed.
struct LinkOutputSummary {
    std::vector<std::string> errors;             // one per error line (LNKnnnn, lld-link: error:)
    std::vector<std::string> warnings;           // one per warning line
    std::vector<std::string> unresolved;         // symbol names of LNK2001/LNK2019 and "undefined symbol:"
};
LinkOutputSummary summarize_link_output(std::string_view output);

// ---- 3. the linked patch ----------------------------------------------------------------------------------

// One anchored object of the link and its locators (coff::add_anchors() result; element i = section i + 1).
struct PatchObjectInput {
    std::string path;
    std::vector<coff::SectionLocator> locators;
};

// Where one section of one object landed.
struct SectionPlacement {
    std::uint32_t object = 0;                    // index into LinkPatchRequest::objects
    std::uint32_t section_index = 0;             // 1-based, as in SectionLocator
    coff::LocatorKind kind = coff::LocatorKind::None;
    std::string symbol;                          // the anchor or the COMDAT leader
    std::uint32_t symbol_rva = 0;                // RVA of that public in the patch PDB
    std::uint32_t section_rva = 0;               // symbol_rva - SectionLocator::offset
};

struct LinkedPatch {
    std::string dll_path;
    std::string pdb_path;
    std::uint64_t base = 0;                      // the ImageBase of the DLL (== the requested /BASE)
    std::uint32_t image_size = 0;                // SizeOfImage
    // Section locators in (object, section) order. Only sections that have a locator are listed.
    std::vector<SectionPlacement> sections;
    // __cml_slot_<name> RVA by function symbol <name> (binding.obj thunk slots) and the RVA of data placeholders by
    // data symbol name. Thunks: thunk_rvas[name] is the RVA of the thunk itself (the public <name>).
    std::unordered_map<std::string, std::uint32_t> slot_rvas;
    std::unordered_map<std::string, std::uint32_t> thunk_rvas;
    std::unordered_map<std::string, std::uint32_t> data_rvas;
    // RVA of the functions requested through LinkPatchRequest::new_functions.
    std::unordered_map<std::string, std::uint32_t> function_rvas;

    double link_ms = 0.0;                        // wall time of the linker process
    double pdb_read_ms = 0.0;                    // opening the patch PDB and resolving everything
    ToolFamily linker_family = ToolFamily::Unknown;
    std::string linker_path;
    std::string linker_version;
    std::vector<Diag> warnings;                  // CML3357 per linker warning

    // RVA of the start of section `section_index` of object `object`; nullptr for a section without locator.
    const SectionPlacement* find_section(std::uint32_t object, std::uint32_t section_index) const noexcept;
};

// What a successful or failed link left behind (for logs and the CI evidence).
struct LinkTrace {
    std::vector<std::string> args;               // the arguments (without the program), as built
    std::string command_line;                    // what was passed to CreateProcess
    std::string response_file;                   // kept next to the output when one was used
    std::string padding_binding_object;          // set when link_patch() added the padding binding object itself
    std::string output;                          // linker stdout + stderr
    int exit_code = -1;
    double link_ms = 0.0;
};

struct ReadPatchRequest {
    std::string dll_path;
    std::string pdb_path;
    std::uint64_t base = 0;                      // expected ImageBase; 0 = do not check
    std::vector<PatchObjectInput> objects;
    std::vector<coff::BindSymbol> bindings;
    std::vector<std::string> new_functions;      // decorated names whose RVA is wanted
};

// Opens the patch PDB (no process-wide cache, released before returning) and the DLL header, resolves everything.
// CML3370/3371 (unreadable), CML3372 (locator not a public), CML3373 (slot or placeholder missing), CML3374 (new
// function missing), CML3375 (ImageBase differs from the expected one: /FIXED was not honoured). Fills `dll_path`,
// `pdb_path`, `base`, `image_size`, `sections`, `slot_rvas`, `thunk_rvas`, `data_rvas`, `function_rvas`, `pdb_read_ms`.
Result<LinkedPatch> read_linked_patch(const ReadPatchRequest& request);

struct LinkPatchRequest {
    ToolInfo linker;                             // from resolve_linker(), or filled by hand (family + path + version_text)
    std::vector<PatchObjectInput> objects;       // anchored objects; link order
    std::string binding_object;                  // path of binding.obj; "" = nothing to bind, a padding object is linked
    std::vector<coff::BindSymbol> bindings;      // the symbols binding.obj was made from
    std::vector<std::string> new_functions;
    std::string out_dir;                         // created when missing
    std::string name;                            // output stem: <out_dir>\<name>.dll / .pdb
    std::uint64_t base = 0;
    std::vector<std::string> extra_args;
    std::vector<unsigned> ignore_warnings;
    unsigned timeout_ms = 60000;
    bool force_response_file = false;
    std::size_t max_command_line = 30000;
    CancelToken* cancel = nullptr;
};

#if defined(_WIN32)
struct ResolveLinkerOptions {
    // A path the user chose. Must exist and must be the requested family (CML3351 otherwise).
    std::string explicit_path;
    // Searched first, e.g. the directory of the compiler recorded for the module (cl.exe sits next to link.exe,
    // clang-cl.exe next to lld-link.exe).
    std::vector<std::string> search_dirs;
};

// Finds and identifies the linker of `family` (Link or LldLink): explicit path, search_dirs, then for Link the
// VCToolsInstallDir of a developer environment, for LldLink C:\Program Files\LLVM\bin, then PATH. Every candidate is
// run (detect_tool) and must report the requested family: this is what keeps Git for Windows' coreutils `link.exe`
// from being used. CML3351 when none qualifies.
Result<ToolInfo> resolve_linker(ToolFamily family, const ResolveLinkerOptions& options = {});

// build_link_command + run + read_linked_patch. `trace` (optional) receives the command and the output also when the
// link fails. CML3350 (errors; the message holds the first error line), CML3356 (unresolved symbol, names it), CML3352,
// CML3353, CML3354, CML3355, CML3358 and everything read_linked_patch() reports. A failed link removes nothing: the
// response file and whatever the linker wrote stay for diagnosis.
Result<LinkedPatch> link_patch(const LinkPatchRequest& request, LinkTrace* trace = nullptr);
#endif

}  // namespace cml::toolchain
