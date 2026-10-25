#pragma once

// Section locators (plan section 4.6, revised by ADR-0020 and the WP-06 evidence), WP-06.
//
// After linking, the broker needs to know where each section of a patch object landed. The PDB
// publishes only EXTERNAL symbols (S_PUB32): STATIC symbols never reach it (0 found under link.exe
// and lld-link, in publics and in module symbols). add_anchors() therefore gives every section a
// "locator", a symbol whose RVA in the patch PDB is known:
//
//   Anchor        a new EXTERNAL symbol `__cml_anchor_<tag>_<n>` (n = 1-based section index) at
//                 offset 0, appended to the symbol table. Used for every linked section that is not
//                 located by a COMDAT leader: non-COMDAT sections, and COMDAT sections whose leader
//                 is static (`reason` StaticLeader) or missing (`reason` NoLeader). Such COMDATs are
//                 never folded against another object's copy (the linkers fold by the EXTERNAL leader
//                 name; the only other discard path is /OPT:REF, and patches link with /OPT:NOREF),
//                 so the anchor cannot be left undefined by a discarded section.
//   ComdatLeader  COMDAT section with an external leader (the first symbol after the section
//                 definition; if that one is not external, any other external symbol defined in the
//                 section serves): NO new symbol. The leader is already a public; the section starts at
//                 `leader RVA - SectionLocator::offset` (offset = the leader's value, usually 0). When
//                 the linker discards the COMDAT as a duplicate, every copy resolves to the kept
//                 section, which is what retargeting that section's relocations needs. (An external
//                 anchor inside such a COMDAT would make lld-link fail with "undefined symbol".)
//   None          no locator, with a reason:
//                   NotLinked    sections that never reach the image (see is_anchorable)
//                   Associative  .pdata/.xdata/.debug$S children of a COMDAT: they never need to be
//                                retargeted to old data; their relocations target the patch's own
//                                functions or binding thunks
//                 A caller that needs a locator for such a section gets a diagnostic from
//                 require_locator() (CML3309), never a silent gap.
//
// PRECONDITION of every locator: the patch is linked with /OPT:NOREF /OPT:NOICF. With /OPT:REF the
// linker drops unreferenced COMDATs, and with it their leader publics (the unit.coff_link test
// shows an unreferenced leader vanishing under /OPT:REF); /OPT:ICF can fold sections so that two
// sections share one address. WP-09 enforces the flags.
//
// What add_anchors() guarantees:
//   * Symbols are only APPENDED, after the last existing record (aux records included). No existing
//     symbol moves, so every relocation's symbol index, every weak-external tag and every
//     section-definition association stays valid; the COMDAT leader rule holds by construction.
//   * Sections, relocations and data are not touched. No section is added, resized or reordered.
//   * The leader / section-definition rule is the reader's (section_symbols.h), not a copy of it.
//   * Verified in CI: anchored and unanchored links give identical images in every section except
//     the timestamp/checksum and the debug directory, under link.exe and lld-link.
//
// Tags: anchor names must not collide between the objects of one link. The default tag is 16 hex
// digits of a 64-bit hash of the complete object file (content_tag); WP-09 can pass its own tag (for
// example the compiland index). Two objects with equal tags in one link are refused through
// AnchorOptions::used_tags (CML3308) or check_unique_tags().
//
// Not anchored (is_anchorable): sections with IMAGE_SCN_LNK_INFO, LNK_REMOVE or MEM_DISCARDABLE
// (.drectve, .chks64, .debug$S/T/F/P, clang .llvm_addrsig) and names starting ".debug" or ".llvm_".

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "cml/coff/writer.h"

namespace cml::coff {

inline constexpr std::string_view kAnchorPrefix = "__cml_anchor_";

enum class LocatorKind : std::uint8_t { Anchor, ComdatLeader, None };
// For kind == Anchor the reason is HasLocator, StaticLeader or NoLeader (why a COMDAT is anchored).
enum class NoLocatorReason : std::uint8_t { HasLocator, NotLinked, Associative, NoLeader, StaticLeader };

struct SectionLocator {
    std::uint32_t section_index = 0;  // 1-based
    LocatorKind kind = LocatorKind::None;
    NoLocatorReason reason = NoLocatorReason::NotLinked;  // see above
    std::string symbol_name;          // the anchor or the COMDAT leader; empty for None
    std::uint32_t symbol_index = 0;   // raw symbol-table index of that symbol in the written object
    std::uint32_t offset = 0;         // the locator symbol's offset in its section: section start = RVA - offset
};

struct AnchorOptions {
    // Appears in the anchor names: "__cml_anchor_<tag>_<n>". Letters, digits and '_' only. Empty:
    // content_tag() of the serialised object (identical to the compiler's file for a parsed object).
    std::string tag;
    // When set, the tag is inserted here and the call fails with CML3308 if it was already present:
    // share one set between all objects of a link.
    std::unordered_set<std::string>* used_tags = nullptr;
    // The compiler's original file bytes, for the default tag (avoids serialising the model just to
    // hash it). Ignored when `tag` is set.
    ByteSpan object_bytes;
};

// True when the section is linked into the image (see the rules above).
bool is_anchorable(const ModelSection& section) noexcept;

// 16 hex digits of a 64-bit hash over all bytes of an object file.
std::string content_tag(ByteSpan object_bytes);

// CML3308 when two entries are equal.
Result<void> check_unique_tags(std::span<const std::string> tags);

// "__cml_anchor_<tag>_<section_index>".
std::string anchor_name(std::uint32_t section_index, std::string_view tag);

// Adds the anchors and returns one locator per section (element i describes section i + 1).
// CML3306 when the object already holds a symbol named __cml_anchor_*; CML3307 for invalid tag
// characters; CML3308 for a tag already in options.used_tags; CML3300 when the symbol or string
// table would overflow. Transactional: on any failure the model and used_tags are unchanged. When no
// section needs an anchor nothing is touched and no tag is computed or claimed.
Result<std::vector<SectionLocator>> add_anchors(ObjectModel& model, const AnchorOptions& options = {});

// CML3309 when the section has no locator (names the section and the reason).
Result<void> require_locator(const SectionLocator& locator);

}  // namespace cml::coff
