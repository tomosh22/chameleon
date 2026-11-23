#pragma once

#include <cstdint>

// Diagnostic ids of WP-26b (static-library members are reloadable, ADR-0036 decision 1). ADR-0036 allocates CML5920..CML5939 to
// the broker side. Message and "what to do" text live next to the code that raises each id; docs/wp/WP-26b.md lists them.
namespace cml::core::staticlib::diag {

// (warning, once per module at enable time) archive members were skipped: no build environment, a missing source file.
inline constexpr std::uint32_t kSkippedArchiveCompilands = 5920;
// (warning, per reload) a changed source file belongs only to compilands that cannot be reloaded.
inline constexpr std::uint32_t kChangedFileNotOwned = 5921;

}  // namespace cml::core::staticlib::diag
