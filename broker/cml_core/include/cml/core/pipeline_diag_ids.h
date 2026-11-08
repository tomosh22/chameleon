#pragma once

#include <cstdint>

// Diagnostic ids of the broker pipeline (WP-12). ADR-0008 allocates CML5300..CML5399 to it. The pipeline
// forwards the ids of the stages it drives unchanged (CML15xx/CML2xxx compile, CML3200.. plan, CML33xx
// link, CML42xx load, CML43xx hooks); these are the ones the pipeline itself raises. Message and "what to
// do" text live next to the code that raises each id; docs/wp/WP-12.md lists them.
namespace cml::core::pipeline::diag {

inline constexpr std::uint32_t kNoTarget = 5300;           // reload: no process / enabled module to patch
inline constexpr std::uint32_t kIndexUnavailable = 5301;   // the module's PDB index is not ready / failed to load
inline constexpr std::uint32_t kReloadTimeout = 5302;      // the reload did not finish within its time limit
inline constexpr std::uint32_t kChangedFileUnmapped = 5303;  // (warning) a changed file belongs to no compiland
inline constexpr std::uint32_t kBusy = 5304;               // another reload is running
inline constexpr std::uint32_t kReserveFailed = 5305;      // the agent could not reserve address space near the module
inline constexpr std::uint32_t kAgentRequestFailed = 5306;  // transport: request not sent / no answer / connection lost
inline constexpr std::uint32_t kLoadFailed = 5307;         // LoadPatch / ApplyFixups / RunInitializers refused (agent id inside)
inline constexpr std::uint32_t kHooksFailed = 5308;        // InstallHooks refused (agent id inside)
inline constexpr std::uint32_t kHooksTimeout = 5309;       // no HooksInstalled in time (synchronised mode: SyncPoint not reached)
inline constexpr std::uint32_t kPatchImage = 5310;         // the patch DLL could not be prepared (pre-applied fix-ups)
inline constexpr std::uint32_t kBadRequest = 5311;         // control command arguments invalid / unknown option
inline constexpr std::uint32_t kBaselineFile = 5312;       // (warning) a source file could not be hashed
inline constexpr std::uint32_t kModuleImage = 5313;        // the module's file cannot be read (exports, imports, old code bytes)
inline constexpr std::uint32_t kNarrowing = 5314;          // (warning) old code bytes unavailable: the function is hooked
inline constexpr std::uint32_t kWorkDir = 5315;            // the session's temporary directory cannot be created
inline constexpr std::uint32_t kPdbOpen = 5316;            // the module's PDB cannot be opened (cml_pdb's own id inside)
inline constexpr std::uint32_t kAgentLost = 5317;          // the agent disconnected while the reload ran
inline constexpr std::uint32_t kStageFailed = 5318;        // generic wrapper: a stage failed (message names stage and cause)
inline constexpr std::uint32_t kNoChanges = 5319;          // (info) the sweep found no changed source
inline constexpr std::uint32_t kAgentRequestTimeout = 5320;  // no answer within the time limit (the agent may still act on the request)
// WP-25 owns CML3450-3499 (ADR-0032).
inline constexpr std::uint32_t kNoFunctionalChange = 3450;   // (warning) the edit compiled to identical code: nothing was loaded
inline constexpr std::uint32_t kBoundDataChangeOnly = 3451;  // (warning) identical code, but the initial value of data in the running image changed: nothing was loaded

}  // namespace cml::core::pipeline::diag
