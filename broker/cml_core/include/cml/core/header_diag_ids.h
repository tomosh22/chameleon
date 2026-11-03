#pragma once

#include <cstdint>

// Diagnostic ids of the header dependency handling (WP-15): CML5550..CML5599.
namespace cml::core::header::diag {

inline constexpr std::uint32_t kPchHeaderChanged = 5550;  // a changed file belongs to a precompiled header: cannot be hot-reloaded
inline constexpr std::uint32_t kTooManyCompilands = 5551;  // more compilands affected than `max_affected_compilands`: needs confirmation
inline constexpr std::uint32_t kPchUnknown = 5552;         // a compiland to recompile uses a precompiled header whose files cannot be determined

}  // namespace cml::core::header::diag
