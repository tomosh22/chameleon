#pragma once

// COFF helpers for the WP-03 spike: the ad-hoc anchor rewriter and the hand-written binding.obj
// emitter. Throwaway code that WP-06 (the real COFF writer) replaces; docs/wp/WP-03.md records
// what was tricky.

#include <cstdint>
#include <string>
#include <vector>

#include "cml/coff/coff.h"

namespace spike {

// ---- anchors ----------------------------------------------------------------------------------

// Sections that never get an anchor (and whose relocations are never retargeted): debug info,
// linker directives, CFG/EH metadata tables, discardable/removed sections.
bool is_anchorable(const cml::coff::Section& section);

std::string anchor_name(std::uint32_t section_index);         // external: "__cml_anchor_<n>"
std::string static_anchor_name(std::uint32_t section_index);  // static probe: "__cml_sanchor_<n>"

struct AnchorInfo {
    std::uint32_t section = 0;
    std::string section_name;
    bool comdat = false;
    std::uint32_t size = 0;
};

struct AnchorOutput {
    std::vector<std::uint8_t> bytes;  // the rewritten object
    std::vector<AnchorInfo> anchors;
    std::uint32_t symbols_before = 0;
    std::uint32_t symbols_after = 0;
};

// Appends, for every anchorable section n, an EXTERNAL symbol __cml_anchor_<n> and a STATIC symbol
// __cml_sanchor_<n>, both at offset 0 of the section. Existing symbols keep their indices (the new
// ones go at the end), so relocations of the original object stay valid for the rewritten one.
// Regular COFF only; dies on bigobj or on a layout it does not understand.
AnchorOutput add_anchors(const cml::coff::Object& object);

// ---- binding object ---------------------------------------------------------------------------

struct BindEntry {
    std::string name;  // decorated symbol name as the patch object references it
    bool is_function = false;
};

std::string slot_name(const std::string& function_name);  // "__cml_slot_<name>"

// Writes a regular COFF AMD64 object:
//   .text  one 16-byte thunk per function: FF 25 <rel32 to slot> + int3 padding, symbol = <name>
//   .data  one 8-byte slot per function (symbol __cml_slot_<name>) and one 8-byte placeholder per
//          data symbol (symbol = <name>); 8-byte aligned, zero initialised.
std::vector<std::uint8_t> emit_binding_obj(const std::vector<BindEntry>& entries);

}  // namespace spike
