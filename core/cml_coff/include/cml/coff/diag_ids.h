#pragma once

#include <cstdint>

// Diagnostic ids owned by the cml_coff WRITER (WP-06): CML3300..CML3349 (link / bind range, ADR-0008).
// The reader's ids (CML1200..CML1299) live next to the reader code. Message and "what to do" text of
// each id live next to the code that raises it; docs/wp/WP-06.md lists them.
namespace cml::coff::diag {

inline constexpr std::uint32_t kSizeOverflow = 3300;       // a count, offset or size does not fit its COFF field
inline constexpr std::uint32_t kTooManySections = 3301;    // regular object with more than 65279 sections
inline constexpr std::uint32_t kBadSymbol = 3302;          // symbol refers to a missing section / bad aux data
inline constexpr std::uint32_t kBadRelocation = 3303;      // relocation names a symbol index that does not exist
inline constexpr std::uint32_t kUnrepresentable = 3304;    // object cannot be reproduced losslessly (overlapping regions ...)
inline constexpr std::uint32_t kBadSectionName = 3305;     // section name cannot be encoded
inline constexpr std::uint32_t kAnchorNameClash = 3306;    // object already has a __cml_anchor_<n> symbol
inline constexpr std::uint32_t kBadAnchorOptions = 3307;   // external anchors without a tag, or a tag with other characters than [A-Za-z0-9_]
inline constexpr std::uint32_t kBindNameInvalid = 3310;    // empty / NUL-containing / linker-provided bind symbol name
inline constexpr std::uint32_t kBindNameDuplicate = 3311;  // the same name (or a derived slot name) twice
inline constexpr std::uint32_t kBindTooMany = 3312;        // binding object would exceed a COFF limit

}  // namespace cml::coff::diag
