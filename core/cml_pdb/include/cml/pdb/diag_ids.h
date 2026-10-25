#pragma once

#include <cstdint>

// Diagnostic ids owned by cml_pdb: CML1400..CML1499 (parse / PDB range). The message and
// "what to do" text of each lives next to the code that raises it; docs/wp/WP-04.md lists them.
namespace cml::pdb::diag {

inline constexpr std::uint32_t kCannotOpenFile = 1400;        // file missing, unreadable or cannot be mapped
inline constexpr std::uint32_t kNotAPdb = 1401;               // too small / wrong magic
inline constexpr std::uint32_t kUnsupportedPdbFormat = 1402;  // old MSF 2.00 or invalid super block
inline constexpr std::uint32_t kTruncatedPdb = 1403;          // file shorter than the super block claims
inline constexpr std::uint32_t kCorruptStreamDirectory = 1404;
inline constexpr std::uint32_t kBadDbiStream = 1405;
inline constexpr std::uint32_t kBadModuleInfo = 1406;
inline constexpr std::uint32_t kBadInfoStream = 1407;
inline constexpr std::uint32_t kNoCodeViewRecord = 1408;      // PE has no RSDS debug record
inline constexpr std::uint32_t kBadPeImage = 1409;            // PE headers malformed / out of bounds
inline constexpr std::uint32_t kPdbNotFound = 1410;
inline constexpr std::uint32_t kPdbMismatch = 1411;           // GUID / age differ from the PE
inline constexpr std::uint32_t kBadModuleSymbols = 1413;
inline constexpr std::uint32_t kBadSectionContributions = 1414;
inline constexpr std::uint32_t kBadSymbolRecords = 1415;      // publics / globals record stream
inline constexpr std::uint32_t kBadSourceInfo = 1416;
inline constexpr std::uint32_t kCompilandOutOfRange = 1417;
inline constexpr std::uint32_t kCapacityExceeded = 1418;      // a 32-bit offset / count would overflow
inline constexpr std::uint32_t kBadBuildInfo = 1419;          // IPI stream (S_BUILDINFO fallback) malformed

}  // namespace cml::pdb::diag
