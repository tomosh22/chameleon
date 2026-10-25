#pragma once

#include <cstdint>

// Diagnostic ids owned by the linker driver (WP-09): CML3350..CML3399 (ADR-0008; WP-06 owns
// CML3300..3349 and uses 3300..3312).
namespace cml::toolchain::diag {

// ---- running the linker ---------------------------------------------------------------------
inline constexpr std::uint32_t kLinkFailed = 3350;           // the linker ran and reported errors
inline constexpr std::uint32_t kLinkerNotFound = 3351;       // no usable link.exe / lld-link
inline constexpr std::uint32_t kCannotStartLinker = 3352;    // CreateProcess failed
inline constexpr std::uint32_t kLinkCancelled = 3353;
inline constexpr std::uint32_t kLinkTimedOut = 3354;
inline constexpr std::uint32_t kNoLinkOutput = 3355;         // exit code 0 but the DLL or PDB is missing
inline constexpr std::uint32_t kUnresolvedSymbol = 3356;     // LNK2001 / LNK2019 / lld "undefined symbol"
inline constexpr std::uint32_t kLinkerWarning = 3357;        // warning: the linker printed a warning
inline constexpr std::uint32_t kCannotPrepareLink = 3358;    // output directory / response file failed

// ---- the command line -----------------------------------------------------------------------
inline constexpr std::uint32_t kForbiddenLinkerOption = 3360;  // /OPT:REF, /OPT:ICF, /FORCE, /INCREMENTAL, ...
inline constexpr std::uint32_t kReservedLinkerOption = 3361;   // an option the driver owns (/OUT, /BASE, /PDB, /DLL, ...)
inline constexpr std::uint32_t kBadLinkRequest = 3362;         // no objects, empty output name, base not aligned, ...

// ---- family detection -----------------------------------------------------------------------
inline constexpr std::uint32_t kLinkerFamilyUnknown = 3365;    // neither PE nor PDB tells which linker built the module
inline constexpr std::uint32_t kLinkerEvidenceConflict = 3366; // warning: the PE and the PDB disagree
inline constexpr std::uint32_t kCannotReadModule = 3367;       // PE or PDB of the module unreadable

// ---- reading the linked patch ---------------------------------------------------------------
inline constexpr std::uint32_t kPatchPdbUnreadable = 3370;
inline constexpr std::uint32_t kPatchImageUnreadable = 3371;
inline constexpr std::uint32_t kLocatorMissing = 3372;         // a section locator is not a public of the patch PDB
inline constexpr std::uint32_t kSlotMissing = 3373;            // a __cml_slot_<name> / data placeholder is not a public
inline constexpr std::uint32_t kFunctionMissing = 3374;        // a new function is not a public of the patch PDB
inline constexpr std::uint32_t kBaseMismatch = 3375;           // the DLL's ImageBase is not the requested /BASE

}  // namespace cml::toolchain::diag
