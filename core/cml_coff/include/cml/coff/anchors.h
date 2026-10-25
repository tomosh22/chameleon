#pragma once

// Anchor insertion (plan section 4.6), WP-06.
//
// Before a module object is linked into a patch DLL, the broker adds one static symbol
// `__cml_anchor_<n>` (n = the 1-based section index) at offset 0 of every section that ends up in the
// image. After linking, the position of each anchor tells where that object section landed.
//
// What add_anchors() guarantees:
//   * Symbols are only APPENDED, after the last existing record (aux records included). No existing
//     symbol moves, so every relocation's symbol index, every weak-external tag and every
//     section-definition association stays valid.
//   * Because the anchors come last, the COMDAT leader rule holds by construction: the leader of a
//     COMDAT section is still the first symbol that follows the section-definition symbol.
//   * Anchors are `IMAGE_SYM_CLASS_STATIC`, value 0, type 0, no aux records. COMDAT sections keep
//     their selection; Associative sections are anchored too.
//   * Sections and relocations are not touched. No section is added, resized or reordered.
//
// Which sections are NOT anchored (is_anchorable()): sections that never reach the image, because
// they carry linker or debugger metadata. They are recognised by flags first, names second:
//   * IMAGE_SCN_LNK_INFO or IMAGE_SCN_LNK_REMOVE        (.drectve, .chks64, .sxdata, ...)
//   * IMAGE_SCN_MEM_DISCARDABLE                          (.debug$S, .debug$T, .debug$F, .debug$P)
//   * names beginning ".debug"                           (cl and clang-cl use .debug$S/.debug$T/.debug$F)
//   * names beginning ".llvm_"                           (.llvm_addrsig)
// Everything else is anchored, including empty sections, .bss, .CRT$XCU, .pdata, .xdata, .rdata$r,
// COMDAT and Associative sections.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cml/coff/writer.h"

namespace cml::coff {

inline constexpr std::string_view kAnchorPrefix = "__cml_anchor_";

struct AnchorOptions {
    // IMAGE_SYM_CLASS_STATIC (default, the plan's form) or IMAGE_SYM_CLASS_EXTERNAL. PDB publics
    // (S_PUB32) are the linker's external symbols: an external anchor is a public with its RVA, a
    // static one is not published as a public (docs/wp/WP-06.md has the per-linker evidence).
    bool external = false;
    // Inserted into the name: "__cml_anchor_<tag>_<n>". Mandatory for external anchors (two objects
    // of one link must not define the same external name); letters, digits and '_' only.
    std::string tag;
};

struct Anchor {
    std::uint32_t section_index = 0;  // 1-based
    std::uint32_t symbol_index = 0;   // raw symbol-table index of the anchor in the written object
    std::string symbol_name;          // "__cml_anchor_<section_index>" or "__cml_anchor_<tag>_<section_index>"
};

// True when the section is linked into the image (see the rules above).
bool is_anchorable(const ModelSection& section) noexcept;

// The anchor symbol name of a section: "__cml_anchor_<section_index>", or with a tag
// "__cml_anchor_<tag>_<section_index>".
std::string anchor_name(std::uint32_t section_index, std::string_view tag = {});

// Appends the anchors; returns them in section order. CML3306 when the object already holds a
// symbol named __cml_anchor_*; CML3307 for invalid options (external without a tag, bad tag
// characters); CML3300 when the symbol or string table would overflow.
Result<std::vector<Anchor>> add_anchors(ObjectModel& model, const AnchorOptions& options = {});

}  // namespace cml::coff
