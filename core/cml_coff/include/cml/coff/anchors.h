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
    // EXTERNAL anchors (default). Neither link.exe nor lld-link writes STATIC symbols to the PDB
    // (publics or module symbols: 0 found, docs/wp/WP-06.md and ADR-0020), while an external anchor
    // is an S_PUB32 public with its RVA. false gives the plan's original static form (never visible).
    bool external = true;
    // With `external`: keep the anchors of COMDAT sections STATIC. Needed when several objects are
    // linked together: an external symbol inside a COMDAT that the linker drops as a duplicate of
    // another object's makes lld-link report "undefined symbol" (WP-06 CI evidence; link.exe
    // tolerates it). Single-object links need not set it. A COMDAT function's leader is a public anyway.
    bool comdat_static = false;
    // Inserted into the name: "__cml_anchor_<tag>_<n>" so that two objects of one link never define
    // the same external name. Letters, digits and '_' only. Empty with `external`: derived from a
    // 64-bit hash of the object's content (see content_tag()), 16 hex digits.
    std::string tag;
};

struct Anchor {
    std::uint32_t section_index = 0;  // 1-based
    std::uint32_t symbol_index = 0;   // raw symbol-table index of the anchor in the written object
    std::string symbol_name;          // "__cml_anchor_<section_index>" or "__cml_anchor_<tag>_<section_index>"
};

// True when the section is linked into the image (see the rules above).
bool is_anchorable(const ModelSection& section) noexcept;

// 16 hex digits of a 64-bit hash over the model's section names, headers, data and symbol names.
std::string content_tag(const ObjectModel& model);

// The anchor symbol name of a section: "__cml_anchor_<section_index>", or with a tag
// "__cml_anchor_<tag>_<section_index>".
std::string anchor_name(std::uint32_t section_index, std::string_view tag = {});

// Appends the anchors; returns them in section order. CML3306 when the object already holds a
// symbol named __cml_anchor_*; CML3307 for invalid tag characters; CML3300 when the symbol or string table would overflow.
Result<std::vector<Anchor>> add_anchors(ObjectModel& model, const AnchorOptions& options = {});

}  // namespace cml::coff
