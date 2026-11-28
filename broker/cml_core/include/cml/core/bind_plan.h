#pragma once

// Bind planner (WP-07, plan sections 4.5 and 4.6, corrected by ADR-0010, 0015, 0020, 0024, 0025).
//
// Input: the new object of a patched TU (cml_coff reader), the old module's PDB index (cml_pdb), the
// TU's compiland in that PDB, a SymbolRegistry (symbols of earlier patches) and ProcessModules (IAT
// slots, exports). Output: a BindPlan, a plain-data description of what the patch must do. The planner
// links, loads and hooks nothing; WP-09 (linker), WP-08 (loader) and WP-10 (hooks) consume the plan
// through WP-12.
//
// What the plan says (plan 4.5, one row per kind of symbol):
//   defined function, in the old image         PatchHooked      patch definition; old entry is hooked
//   defined function, not in the old image     PatchNewFunction canonical symbol, no hook
//   defined data, in the old image             BindOldData      every reference is retargeted
//   defined data, not in the old image         PatchNewData     patch-local; its initialiser runs once
//   vtables, RTTI, constants, unwind data,
//     initialiser stubs, funclets              PatchFresh       fresh patch-local copies
//   undefined function                         BindOldFunction  thunk + slot in binding.obj
//   undefined data                             BindOldData      8-byte placeholder in binding.obj
// "In the old image" means: the process symbol registry, else the module PDB (publics for externals;
// for statics the compiland's S_LPROC32/S_LDATA32 records, matched by name and compiland, ADR-0010).
// Undefined symbols are resolved in this order: (1) registry, (2) the compiland's own statics (only
// for names that can denote a TU-private entity), (3) PDB publics, then PDB global data, (4) the
// module's IAT slot for __imp_X (PDB public __imp_X is step 3; else the import directory in memory),
// (5) exports of other loaded modules. Nothing found: CML3200.
//
// Fix-ups (4.6 + ADR-0025): for every relocation whose target is bind-to-old DATA the plan holds
// (section, offset, kind, old address S, in-place addend A); once the patch is linked,
// make_fixup_triples() turns them into (rva, kind, value) with value = S + A - (site + 4 + N) for the
// REL32 family and S + A for ADDR64. Calls to bind-to-old functions go through the binding object's
// thunks and need no fix-up; ADDR64 function pointers are retargeted to the real address; ADDR32NB is
// legal towards thunks, and towards old data only where the site is proved to be the sign-extended 32-bit
// displacement of a memory operand (WP-36, ADR-0042, imgrel_site.h): such a site gets an `Addr32NbDisp`
// fix-up, value = S + A - patch_base. Any other ADDR32NB to old data is CML3101. Each thunk's slot gets a
// `Slot` fix-up with the old function's address.
//
// Everything the planner decides is logged at Trace level.

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cml/coff/anchors.h"
#include "cml/coff/binding.h"
#include "cml/coff/coff.h"
#include "cml/core/symbol_registry.h"
#include "cml/pdb/index.h"
#include "cml/util/result.h"

namespace cml::core::bind {

inline constexpr std::uint32_t kNoRva = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------------------------
// Decisions
// ---------------------------------------------------------------------------------------------

enum class SymbolType : std::uint8_t { Function, Data };

enum class DecisionKind : std::uint8_t {
    PatchHooked,
    PatchNewFunction,
    BindOldData,
    PatchNewData,
    PatchFresh,
    BindOldFunction,
    LinkerProvided,  // __ImageBase: the linker defines it, nothing to bind
    WeakDefault,     // weak external that nothing resolves: its default symbol in the object is used
    Unresolved,
};

// Where the old address of a bind/hook decision came from.
enum class BindSource : std::uint8_t {
    None,
    Registry,            // (1) process symbol registry
    CompilandStatic,     // (2) compiland S_L* record matched by name (ADR-0010)
    CompilandOrdinal,    //     ... by position among same-named records
    CompilandRva,        //     ... by RVA within the compiland's contribution
    CompilandLayout,     //     ... compiler-generated guard inferred from the unchanged section layout
    CompilandSignature,  //     ... a static function told apart from its overloads by its signature (WP-37)
    CompilandClosure,    //     ... a lambda's function-local static, by the identity of the closure class (WP-37)
    CompilandProcedure,  //     ... a function-local static, by its (matched) function and its place among the
                         //         function's statics of that name (WP-37, ADR-0042 amended; CML3570 when there are several)
    PdbPublic,           // (3) S_PUB32
    PdbGlobal,           // (3) S_GDATA32
    ImportSlotPdb,       // (4) public __imp_X of the module's PDB
    ImportSlotModule,    // (4) import directory of the module in memory
    ExportOtherModule,   // (5) export of another loaded module
    SynthesizedImportSlot,  // (5) __imp_X not imported by the module: placeholder filled with the export's address
};

struct TargetAddress {
    bool valid = false;
    std::uint64_t va = 0;
    std::uint32_t rva = kNoRva;  // module-relative when the address lies in the patched module
    std::string module;          // module name (module hits) or origin (registry, exports)
};

struct SymbolDecision {
    std::uint32_t symbol_index = 0;  // raw symbol-table index in the new object
    std::string name;                // decorated
    std::string display;             // display_name(name)
    SymbolType type = SymbolType::Data;
    bool defined = false;
    bool is_static = false;
    std::uint32_t section = 0;       // defining section (defined symbols), 1-based
    std::string section_name;        // its name (".text$mn", ".bss", ...)
    std::uint32_t value = 0;         // offset in that section
    DecisionKind decision = DecisionKind::PatchFresh;
    BindSource source = BindSource::None;
    TargetAddress old_address;       // hook target / bound address
    std::string reason;
    // First relocation that references the symbol (undefined symbols); 0 when none.
    std::uint32_t first_ref_section = 0;
    std::uint32_t first_ref_offset = 0;
};

const char* to_string(DecisionKind k) noexcept;
const char* to_string(BindSource s) noexcept;
const char* to_string(SymbolType t) noexcept;

// ---------------------------------------------------------------------------------------------
// Outputs
// ---------------------------------------------------------------------------------------------

struct HookEntry {
    std::string new_symbol;    // patch definition (decorated)
    std::string display;
    std::uint64_t old_va = 0;  // entry to hook
    std::uint32_t old_rva = kNoRva;
    std::uint32_t old_size = 0;  // code length when the PDB knows it (WP-10 skips too-short functions), else 0
    std::string module;
    BindSource source = BindSource::None;
};

struct ThunkEntry {
    std::string name;  // decorated; the binding object defines it as a thunk
    std::string display;
    std::string slot;  // __cml_slot_<name>
    TargetAddress target;
    BindSource source = BindSource::None;
};

struct PlaceholderEntry {
    std::string name;  // decorated; the binding object defines an 8-byte placeholder
    std::string display;
    TargetAddress target;
    BindSource source = BindSource::None;
    // true: the placeholder IS the import slot (references stay as linked) and a Slot fix-up writes
    // `target` into it; false: references to it are retargeted to `target`.
    bool is_synthesized_slot = false;
};

struct NewFunction {
    std::string name;
    std::string display;
    bool is_static = false;
    std::uint32_t section = 0;
    std::uint32_t value = 0;
};

// An entry of a .CRT$XC* table that must be run once after load: the pointer lives at `table_offset`
// of object section `table_section`; read it from the loaded patch (the initialisers are static
// functions, hence not in the PDB's publics).
struct InitialiserEntry {
    std::string function;
    std::string display;
    std::string table_section_name;
    std::uint32_t table_section = 0;
    std::uint32_t table_offset = 0;
    bool tu_wide = false;  // clang's _GLOBAL__sub_I_<file>: initialises every global of the TU
    std::vector<std::string> globals;
};

struct NewGlobal {
    std::string name;
    std::string display;
    bool is_static = false;
    std::uint32_t section = 0;
    std::uint32_t value = 0;
    std::vector<std::string> initialisers;  // InitialiserEntry::function values that run for it
};

// SecRel32: IMAGE_REL_AMD64_SECREL against a symbol of the old module (the CRT's thread-safe-static
// epoch `_Init_thread_epoch` is read through it): the 32-bit offset of the target inside its image
// section, plus the in-place addend. Valid for module targets only (the PDB knows the section).
// Addr32NbDisp (WP-36, ADR-0042): IMAGE_REL_AMD64_ADDR32NB against data of the old image, at a site that is proved
// to be the 32-bit displacement of a ModRM/SIB memory operand (`[reg + index*scale + sym@IMGREL]`, the register
// holding the PATCH's __ImageBase): the value is int32(S + A - patch_base), which the processor sign-extends.
enum class FixupKind : std::uint8_t { Rel32 = 0, Rel32_1, Rel32_2, Rel32_3, Rel32_4, Rel32_5, Addr64, SecRel32, Slot, Addr32NbDisp };
const char* to_string(FixupKind k) noexcept;
inline constexpr bool is_rel32(FixupKind k) noexcept { return k <= FixupKind::Rel32_5; }
// N of REL32_N (bytes that follow the 4-byte field inside the instruction).
inline constexpr std::uint32_t rel32_extra(FixupKind k) noexcept { return static_cast<std::uint32_t>(k); }

struct Fixup {
    std::uint32_t section_index = 0;  // 1-based object section holding the site; 0 for Slot
    std::uint32_t offset = 0;         // offset of the site in that section
    FixupKind kind = FixupKind::Rel32;
    std::int64_t addend = 0;          // in-place addend of the OBJECT (ADR-0025)
    std::uint64_t target_va = 0;      // S: old address of the symbol
    std::uint32_t target_rva = kNoRva;
    std::uint32_t target_section_offset = 0;  // SecRel32 only: offset of the target in its image section
    // REL32 to a bound FUNCTION: the reference was linked to the binding object's thunk, so if the real
    // address turns out to be out of rel32 reach of the patch, make_fixup_triples() leaves the thunk in
    // place instead of failing.
    bool thunk_fallback = false;
    std::string symbol;               // the referenced object symbol; for Slot the patch symbol that receives the address
    std::string section_name;         // informational
};

struct PlanSummary {
    std::size_t hooks = 0;
    std::size_t bind_old_data = 0;
    std::size_t thunks = 0;
    std::size_t new_symbols = 0;  // new functions + new globals
    std::size_t fixups = 0;
};

// WP-40 (R-48 item 5, ADR-0043 rule (d)): lambdas of one enclosing function whose call operators the type records cannot
// tell apart (the same signature but for the closure's number). Their statics were paired by that number; the pipeline
// compares the code of the call operators with the old ones (hook_narrowing.h: pair_lambdas_by_code) and, where the code
// says that lambdas changed places, plans again with PlanInput::closure_pairing.
struct LambdaEntry {
    std::string closure;        // the patch's closure, decorated, without the unit's hash: "<lambda_1>@?1??f@@YAXXZ@"
    std::string old_closure;    // the old closure its statics were paired with
    std::string call_operator;  // the patch's operator() ("??R<lambda_1>@?1??f@@YAXXZ@QEBA...")
    std::vector<std::string> statics;  // the patch's statics of the lambda paired through it (guards included)
};
struct LambdaSet {
    std::string function;  // display name of the enclosing function
    std::vector<LambdaEntry> lambdas;
};

// WP-40 (second high review, ADR-0043 ruling): a bitmask guard `$S<n>` (no thread-safe statics) the plan binds to the
// old bitmask. Its bits go to the function's dynamically initialised statics in the order they are reached, so the old
// bitmask is right only when those statics are the same set in the same order as in the old image. The records cannot
// show that (a static made constant-initialised keeps its record); the code of the function can: the pipeline proves
// that `function` is unchanged (hook_narrowing.h: prove_bitmask_guards), or refuses the reload (CML3600).
struct BitmaskGuard {
    std::string guard;     // decorated name of the bitmask
    std::string function;  // decorated name of the function whose statics it guards ("" when it cannot be read)
};

// WP-46 (F-41.2, ADR-0043 ruling D1): a lambda that cl's legacy lambda processor (C++17 without /permissive-, /Zc:lambda-)
// names by a hash, `<lambda_<32 hex digits>>`, in no scope. The hash depends on the lambda's own code (CI, fixture
// legacy_lambdas: editing a lambda renames it; moving it does not). Its functions (call operator, invoker) and statics were
// paired by that name, or, for the one lambda left unmatched on each side (an edited lambda), by elimination; the pipeline
// proves the pairing by code (hook_narrowing.h: prove_hashed_lambdas): every name-matched function's new code must be the old
// code it is hooked to (under the plan, so its statics are the old lambda's); one changed lambda is allowed (ADR-0043 rule
// (d)), and that is the eliminated one when there is one. Otherwise CML3630. Functions with external linkage (paired by their
// public decorated name, like any external function) are not listed.
struct HashedLambda {
    std::string closure;                  // "<lambda_9c7f...>"
    std::string old_closure;              // WP-46 round 2: the old closure it was paired with by elimination ("" when by its name)
    std::vector<std::string> functions;   // the patch's static functions of the closure that are hooked to old ones
    std::vector<std::string> statics;     // the patch's statics of the closure bound to old ones (guards included)
};

struct BindPlan {
    std::string object_name;
    std::string module_name;
    std::string compiland;  // object name of the matched compiland
    std::uint32_t compiland_index = 0;
    std::uint64_t module_base = 0;

    std::vector<SymbolDecision> decisions;  // in symbol-table order
    std::vector<HookEntry> hooks;
    std::vector<ThunkEntry> thunks;
    std::vector<PlaceholderEntry> placeholders;
    std::vector<NewFunction> new_functions;
    std::vector<NewGlobal> new_globals;
    std::vector<InitialiserEntry> initialisers;  // in run order
    std::vector<Fixup> fixups;                   // by section, offset
    // Sections of the object that hold a (non-slot) fix-up and therefore need a locator (ADR-0024).
    // The associative ones among them (cl's `.text$x` EH funclets that touch a bound static, e.g. the
    // guard `$TSS0`) must be passed to coff::AnchorOptions::anchor_associative, otherwise they have
    // none and make_fixup_triples() fails with CML3204.
    std::vector<std::uint32_t> locator_sections;
    std::vector<std::uint32_t> associative_locator_sections;
    std::vector<Diag> warnings;
    // WP-36: ADDR32NB relocations of the object against data bound to the old image, and how many of them were proved
    // to be address displacements (each of those has an Addr32NbDisp fix-up). A plan that is returned has
    // verified == sites; a failed planning reports the counts through `partial`.
    std::size_t imgrel_sites = 0;
    std::size_t imgrel_verified = 0;
    std::vector<LambdaSet> lambda_sets;  // WP-40, see LambdaSet
    std::vector<BitmaskGuard> bitmask_guards;  // WP-40, see BitmaskGuard
    std::vector<HashedLambda> hashed_lambdas;  // WP-46, see HashedLambda

    PlanSummary summary() const;
    // `BINDPLAN <label> hooks=N bind_old_data=N thunks=N new=N fixups=N` (the CI evidence line).
    std::string summary_line(std::string_view label) const;
    // `WP36-IMGREL sites=N verified=N object=<label>` (the WP-36 evidence line).
    std::string imgrel_line(std::string_view label) const;
    // The input of WP-06's make_binding_object(): thunks first, then placeholders.
    std::vector<coff::BindSymbol> binding_symbols() const;
    const SymbolDecision* find_decision(std::string_view name) const;
};

// ---------------------------------------------------------------------------------------------
// Planning
// ---------------------------------------------------------------------------------------------

// WP-55 (R-62): WP-51's verdict on an old procedure's inline sites (what it inlined, read from the module stream through
// PdbIndex::procs_inlinees), kept across the reloads of one module image. The image does not change while it is loaded, so
// neither do its procedures' inline sites, and a verdict taken from here is the one the planner would read again. Owned by the
// module's state (ModuleState) and dropped with it (disable, unload; a module loaded again is a new state). Keyed by the PDB's
// identity (GUID and age), the compiland and the procedure's index in its symbols; each entry also holds the procedure's RVA and
// name, and a lookup that finds another procedure there finds nothing. A store under another PDB identity drops every entry of
// the old one. A verdict whose inline sites could not be read is not kept. Thread-safe.
struct CachedInlineVerdict {
    std::uint32_t proc_rva = 0;
    std::string proc_name;
    std::string risk;
    std::string wider;
    const char* wider_kind = "";  // a string literal of the planner
    std::vector<std::string> closure_tails;
    std::uint32_t sites = 0;
    std::size_t inlinees = 0;
    std::size_t closures = 0;
    std::size_t unreadable = 0;
};
class InlineVerdictCache {
public:
    // The verdict on procedure `proc` (its index in compiland_symbols(compiland)) when one is kept for this image and it still
    // names a procedure at `rva` called `name`.
    std::optional<CachedInlineVerdict> find(const pdb::PdbId& image, std::uint32_t compiland, std::uint32_t proc, std::uint32_t rva,
                                            std::string_view name) const;
    // A zero identity (an image of unknown identity) is never kept.
    void store(const pdb::PdbId& image, std::uint32_t compiland, std::uint32_t proc, CachedInlineVerdict verdict);
    std::size_t size() const;
    std::uint64_t hits() const;    // find() calls that found a verdict (the planner's batch probes included)
    std::uint64_t stores() const;  // verdicts stored
    // What the planner reports (WP55-INLINE-CACHE): verdicts of procedures it asked about that came from here, and verdicts it
    // judged from a read of the module stream (whether or not the sites could be read, and whether or not they were kept).
    void count_kept();
    void count_read();
    std::uint64_t kept() const;
    std::uint64_t reads() const;

private:
    mutable std::mutex mutex_;
    pdb::PdbId image_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, CachedInlineVerdict> entries_;  // (compiland, proc)
    mutable std::uint64_t hits_ = 0;
    std::uint64_t stores_ = 0;
    std::uint64_t kept_ = 0;
    std::uint64_t reads_ = 0;
};

struct PlanInput {
    const coff::Object* object = nullptr;        // the new object, as the compiler wrote it
    std::string object_name;                     // for messages ("logic.cpp.obj")
    const pdb::PdbIndex* module_pdb = nullptr;   // the old module
    std::string module_name;                     // "fixture_basic.exe" (messages, ProcessModules::iat_slot)
    std::uint64_t module_base = 0;               // load address of the old module: VA = base + RVA
    // The TU's compiland: its object name as recorded in the PDB (full path or its last component),
    // or its file stem. `compiland_index` overrides the lookup.
    std::string compiland;
    std::int64_t compiland_index = -1;
    const SymbolRegistry* registry = nullptr;    // optional
    const ProcessModules* modules = nullptr;     // optional
    // WP-40 (rule (d)): closure of the patch -> old closure, where the code of the call operators showed that lambdas
    // changed places (pair_lambdas_by_code). Their statics, guards and call operators are paired across the numbers.
    std::map<std::string, std::string> closure_pairing;
    // WP-55 (R-62): the verdicts on inline sites of earlier plans against the same module image (optional). Read before the
    // module stream is walked, filled with what was walked.
    InlineVerdictCache* inline_verdicts = nullptr;
};

// Builds the plan. On failure returns the first error; with `all_errors` every error is collected
// (the usual case: several unresolved symbols). With `partial`, a failed planning (diagnostics, not an
// unreadable PDB/object) still fills it with every decision made, for tools and evidence. Warnings are
// in BindPlan::warnings.
Result<BindPlan> plan_bind(const PlanInput& input, std::vector<Diag>* all_errors = nullptr, BindPlan* partial = nullptr);

// ---------------------------------------------------------------------------------------------
// From plan to (rva, kind, value)
// ---------------------------------------------------------------------------------------------

// Where the linked patch put things.
struct PatchLayout {
    std::uint64_t patch_base = 0;
    std::map<std::uint32_t, std::uint32_t> section_rva;           // object section index -> RVA of its start
    std::map<std::string, std::uint32_t, std::less<>> symbol_rva;  // patch publics: __cml_slot_*, placeholders
};

// One write for the agent: store `value` at patch_base + rva. Rel32* and Addr32NbDisp write the low 32 bits (a
// two's-complement displacement, from the end of the field for Rel32* and from the patch base for Addr32NbDisp),
// Addr64 and Slot write 64 bits.
struct FixupTriple {
    std::uint32_t rva = 0;
    FixupKind kind = FixupKind::Rel32;
    std::uint64_t value = 0;
};

// Pure. CML3204 (no RVA for a fix-up's section), CML3205 (REL32 displacement out of int32 range),
// CML3206 (slot symbol without RVA), CML3550 (image-relative displacement out of int32 range). Sorted by rva.
Result<std::vector<FixupTriple>> make_fixup_triples(const BindPlan& plan, const PatchLayout& layout);

// Fills a PatchLayout from the patch's PDB: a section starts at (RVA of its locator symbol - the
// locator's offset), slots and placeholders are looked up as publics. `locators` is what
// coff::add_anchors() returned for this object. Only the sections the plan needs are resolved.
Result<PatchLayout> layout_from_pdb(const BindPlan& plan, std::span<const coff::SectionLocator> locators,
                                    const pdb::PdbIndex& patch_pdb, std::uint64_t patch_base);

// ---------------------------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------------------------

struct JsonOptions {
    bool pretty = true;
    bool addresses = true;  // old VAs / RVAs (off: only names and decisions, comparable across builds)
    bool reasons = true;
};
std::string to_json(const BindPlan& plan, const JsonOptions& options = {});
std::string to_json(std::span<const FixupTriple> triples);

}  // namespace cml::core::bind
