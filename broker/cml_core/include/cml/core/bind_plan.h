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
// legal only towards thunks (to old data: CML3101). Each thunk's slot gets a `Slot` fix-up with the old
// function's address.
//
// Everything the planner decides is logged at Trace level.

#include <cstdint>
#include <map>
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
enum class FixupKind : std::uint8_t { Rel32 = 0, Rel32_1, Rel32_2, Rel32_3, Rel32_4, Rel32_5, Addr64, SecRel32, Slot };
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

    PlanSummary summary() const;
    // `BINDPLAN <label> hooks=N bind_old_data=N thunks=N new=N fixups=N` (the CI evidence line).
    std::string summary_line(std::string_view label) const;
    // The input of WP-06's make_binding_object(): thunks first, then placeholders.
    std::vector<coff::BindSymbol> binding_symbols() const;
    const SymbolDecision* find_decision(std::string_view name) const;
};

// ---------------------------------------------------------------------------------------------
// Planning
// ---------------------------------------------------------------------------------------------

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

// One write for the agent: store `value` at patch_base + rva. Rel32* write the low 32 bits (a
// two's-complement displacement), Addr64 and Slot write 64 bits.
struct FixupTriple {
    std::uint32_t rva = 0;
    FixupKind kind = FixupKind::Rel32;
    std::uint64_t value = 0;
};

// Pure. CML3204 (no RVA for a fix-up's section), CML3205 (REL32 displacement out of int32 range),
// CML3206 (slot symbol without RVA). Sorted by rva.
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
