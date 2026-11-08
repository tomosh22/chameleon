#pragma once

#include <cstdint>

// Diagnostic ids of the bind planner (WP-07). ADR-0008 allocates CML3200..CML3299; CML3101 and CML3102
// are the by-design limitations that docs/KNOWN_LIMITATIONS.md pre-allocated to this WP (ADR-0003).
// Message and "what to do" text live next to the code that raises each id; docs/wp/WP-07.md lists them.
namespace cml::core::bind::diag {

// By-design limitations (KNOWN_LIMITATIONS.md).
inline constexpr std::uint32_t kAddr32NbToOldData = 3101;  // image-relative reference to old-module data
inline constexpr std::uint32_t kThreadLocal = 3102;        // thread-local reference shape that cannot be retargeted (WP-24 narrowed it)

// Errors.
inline constexpr std::uint32_t kUnresolved = 3200;         // undefined symbol found nowhere
inline constexpr std::uint32_t kCompilandNotFound = 3201;  // no compiland of the module matches the TU
inline constexpr std::uint32_t kUnsupportedReloc = 3202;   // relocation kind that cannot be retargeted
inline constexpr std::uint32_t kUnsupportedObject = 3203;  // not an x64 object / malformed for planning
inline constexpr std::uint32_t kNoLocator = 3204;          // fix-up needed in a section without locator
inline constexpr std::uint32_t kRel32OutOfRange = 3205;    // REL32 displacement does not fit 32 bits
inline constexpr std::uint32_t kLayoutIncomplete = 3206;   // patch layout lacks a section or symbol
inline constexpr std::uint32_t kAmbiguousStatic = 3207;    // static matches several old symbols
inline constexpr std::uint32_t kBadRelocationSite = 3208;  // relocation site outside its section
inline constexpr std::uint32_t kCompilandAmbiguous = 3209; // several compilands match the TU name
inline constexpr std::uint32_t kAmbiguousOverloads = 3214; // same-scope static overloads cannot be told apart
inline constexpr std::uint32_t kUnmatchedInitialiser = 3215; // dynamic initialiser without a global of the TU

// WP-24 (ADR-0032: CML3400..CML3449).
inline constexpr std::uint32_t kNewThreadLocal = 3400;     // thread_local the old image does not have (error)
inline constexpr std::uint32_t kAmbiguousThreadLocal = 3402;  // several S_GTHREAD32 records for one name (error)
inline constexpr std::uint32_t kThreadLocalInit = 3401;   // dynamic initialiser/destructor of a thread_local stays the old one (warning)

// Warnings (the plan is still produced).
inline constexpr std::uint32_t kSharedInitialiser = 3210;  // TU-wide initialiser may re-run initialisers of bound globals
inline constexpr std::uint32_t kGuardUnmatched = 3211;     // function-local static guard not found in the old image
inline constexpr std::uint32_t kSectionRelative = 3212;    // reference through a section symbol into bound data
// 3213 is retired (ambiguous static functions are the error 3214).

}  // namespace cml::core::bind::diag
