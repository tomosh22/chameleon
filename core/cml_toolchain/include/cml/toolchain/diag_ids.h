#pragma once

#include <cstdint>

// Diagnostic ids owned by cml_toolchain (WP-05): CML1500..CML1599 (parsing / detection) and
// CML2200..CML2299 (compile invocation). CML1102 and CML2102 are not in this range: they are the
// ids pre-allocated in docs/KNOWN_LIMITATIONS.md for "no recorded command" and "PCH-creating TU
// changed", and are raised here because this library is where those conditions are found.
namespace cml::toolchain::diag {

// ---- shared with docs/KNOWN_LIMITATIONS.md ------------------------------------------------
inline constexpr std::uint32_t kNoCompileCommand = 1102;  // neither S_ENVBLOCK/S_BUILDINFO nor compile_commands.json
inline constexpr std::uint32_t kPchCreatingTu = 2102;     // /Yc TU (clang: -emit-pch / -building-pch-with-obj) changed

// ---- CML15xx: parsing and detection ---------------------------------------------------------
inline constexpr std::uint32_t kResponseFileUnreadable = 1500;
inline constexpr std::uint32_t kResponseFileTooDeep = 1501;
inline constexpr std::uint32_t kUnsupportedCompiler = 1502;   // not cl, clang-cl or a cc1 invocation
inline constexpr std::uint32_t kNoSourceFile = 1503;          // the command names no source file
inline constexpr std::uint32_t kUnsupportedOption = 1504;     // /clr, /ZW, ... : cannot be recompiled
inline constexpr std::uint32_t kBadJson = 1505;               // compile_commands.json malformed
inline constexpr std::uint32_t kCompileCommandsUnreadable = 1506;
inline constexpr std::uint32_t kNoCompileCommandsEntry = 1507;
inline constexpr std::uint32_t kToolNotFound = 1510;
inline constexpr std::uint32_t kToolDetectFailed = 1511;
inline constexpr std::uint32_t kObjectUnreadable = 1520;      // build-flag detection: object file not usable

// ---- CML22xx: compile invocation ------------------------------------------------------------
inline constexpr std::uint32_t kCompileFailed = 2200;         // the compiler ran and reported errors
inline constexpr std::uint32_t kCompilerFallback = 2201;      // warning: recorded compiler missing, using another
inline constexpr std::uint32_t kCannotStartCompiler = 2202;   // CreateProcess failed / no compiler anywhere
inline constexpr std::uint32_t kCompileCancelled = 2203;
inline constexpr std::uint32_t kCompileTimedOut = 2204;
inline constexpr std::uint32_t kNoObjectProduced = 2205;      // exit code 0 but the object is missing
inline constexpr std::uint32_t kCannotPrepareOutput = 2206;   // output directory / response file failed
inline constexpr std::uint32_t kWholeProgramDowngraded = 2210;  // warning: /GL (or -flto) removed for the patch compile
inline constexpr std::uint32_t kPdbCopyFailed = 2211;         // warning: private compiler PDB copy could not be made

}  // namespace cml::toolchain::diag
