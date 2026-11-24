#pragma once

#include <cstdint>

// Diagnostic ids of the agent's fix-up writer that WP-36 added (ADR-0042 reserves CML4700..CML4719). The older loader
// ids (CML4200..CML4214) are in core/cml_proc/include/cml/proc/loader_diag_ids.h. Like those, the id travels on the wire
// as the `status` of the Reply to ApplyFixups; the broker passes it on unchanged. docs/TROUBLESHOOTING.md has the entry.
namespace cml::ids {

// Addr32NbDisp: `target - patch image base` does not fit a signed 32-bit displacement. Nothing is written.
inline constexpr std::uint32_t kLoaderAddr32NbOutOfRange = 4700;

}  // namespace cml::ids
