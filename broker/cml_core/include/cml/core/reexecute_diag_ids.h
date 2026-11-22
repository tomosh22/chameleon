#pragma once

#include <cstdint>

// Diagnostic ids of the broker side of "Re-execute function after a crash" (WP-23). ADR-0035 allocates CML5900..CML5919 to it
// (the agent side, CML4600..4649, is in agent/src/crash/diag_ids.h). docs/wp/WP-23.md repeats every id with its message and
// "what to do" text. Ids 5900..5907 appear as the reason in `reasons.reexecute` of the `exception` / `exception_updated` events
// ("CML5901: ..."); 5908 is the refusal of `exception-action` with "reexecute" when it is not offered.
namespace cml::core::reexecute::diag {

inline constexpr std::uint32_t kNoReplacement = 5900;        // no patch applied since the fault replaced a function on the thread's stack
inline constexpr std::uint32_t kOptimised = 5901;            // the function's compiland was built with optimisation
inline constexpr std::uint32_t kOptimisationUnknown = 5902;  // the recorded build command does not say whether optimisation was off
inline constexpr std::uint32_t kNoDebugInfo = 5903;          // the frame's function cannot be identified (patch code, module not enabled, PDB not loaded)
inline constexpr std::uint32_t kSignatureUnsupported = 5904; // the function type cannot be read, or its shape shifts the arguments (variadic, returns a class)
inline constexpr std::uint32_t kParamUnsupported = 5905;     // a register parameter has a type that is not an integer, pointer, float or double
inline constexpr std::uint32_t kLocationUnknown = 5906;      // the PDB does not tell where a register parameter lives (or it lives in a register)
inline constexpr std::uint32_t kNoStack = 5907;              // the agent did not report the stack of the held thread
inline constexpr std::uint32_t kNotOffered = 5908;           // exception-action "reexecute" while re-execute is not offered

}  // namespace cml::core::reexecute::diag
