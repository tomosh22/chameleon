#pragma once

// Failure injection (WP-28, ADR-0035): the pipeline stages that can be made to fail, shared by the broker and the agent.
//
// Compiled in under CMake option CML_FAULT_INJECTION (default ON; the define CML_FAULT_INJECTION=1 is set for every target);
// reachable only through the `fault-injection` control capability (commands `inject-failure` and `fault-stages`) and, for the
// agent's stages, the ArmFault message (0x00C0) the broker sends on behalf of that command. docs/wp/WP-28.md has the table.
//
// An armed stage fails its next `count` passes (default 1) with the stage's own diagnostic, then disarms itself. The failure
// takes the stage's normal error path, so the reload is refused with a diagnostic, the patch (if one was loaded) is unloaded,
// and the application keeps running its previous code; the next reload without injection succeeds.
//
// Diagnostic ids: CML4650..4699 (agent stages), CML5960..5989 (broker stages and the command), ADR-0035.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace cml::ipc::fault {

enum class Stage : std::uint8_t {
    PdbLoad = 0,
    ChangeDetection,
    Compile,
    BindPlan,
    Reservation,    // agent
    Link,
    PreApply,
    SendPatch,
    PatchLoad,      // agent
    HookInstall,    // agent, inside the window in which the application's threads are suspended
    PostPatchHooks, // agent
    Count
};
inline constexpr std::size_t kStageCount = static_cast<std::size_t>(Stage::Count);

// Ids of the injected failures. The agent's are the status of its Reply / ReserveRangeResult / PatchLoaded / HooksInstalled
// (the broker reports them as the reload's diagnostic), the broker's are raised by the pipeline itself.
namespace ids {
inline constexpr std::uint32_t kAgentReservation = 4650;
inline constexpr std::uint32_t kAgentPatchLoad = 4651;
inline constexpr std::uint32_t kAgentHookInstall = 4652;
inline constexpr std::uint32_t kAgentPostPatchHooks = 4653;
inline constexpr std::uint32_t kAgentBadArm = 4654;          // ArmFault names an unknown stage / a stage of the broker
inline constexpr std::uint32_t kAgentNotCompiledIn = 4655;   // the agent was built without CML_FAULT_INJECTION
// HooksInstalled status: the post-patch-hooks stage failed AND putting the hooks back failed (partly): the patch image is still
// the target of live hooks. The broker must not unload it; it stays mapped (leaked) until the process ends.
inline constexpr std::uint32_t kAgentRollbackIncomplete = 4656;

inline constexpr std::uint32_t kBadRequest = 5960;           // inject-failure: missing/unknown stage, bad count or pid
inline constexpr std::uint32_t kNoSuchProcess = 5961;        // inject-failure: no agent with that pid
inline constexpr std::uint32_t kArmRefused = 5962;           // the agent did not arm the stage
inline constexpr std::uint32_t kNotApplicable = 5970;       // inject-failure: the stage cannot fire for that process (reservation already made)
inline constexpr std::uint32_t kPartialDisarm = 5971;        // inject-failure "none": some agents could not be disarmed
inline constexpr std::uint32_t kPdbLoad = 5963;
inline constexpr std::uint32_t kChangeDetection = 5964;
inline constexpr std::uint32_t kCompile = 5965;
inline constexpr std::uint32_t kBindPlan = 5966;
inline constexpr std::uint32_t kLink = 5967;
inline constexpr std::uint32_t kPreApply = 5968;
inline constexpr std::uint32_t kSendPatch = 5969;
}  // namespace ids

struct StageInfo {
    Stage stage;
    const char* name;         // the name used by `inject-failure` and listed by `fault-stages`
    bool agent_side;          // armed in the agent through ArmFault
    std::uint32_t diag_id;    // the diagnostic the failure carries
    const char* where;        // one line: what is made to fail
};

// In pipeline order (the order `fault-stages` lists them).
inline constexpr StageInfo kStages[] = {
    {Stage::PdbLoad, "pdb-load", false, ids::kPdbLoad, "the broker's mapping of the module's PDB (dropped, then fails to open)"},
    {Stage::ChangeDetection, "change-detection", false, ids::kChangeDetection, "the sweep that hashes the tracked sources"},
    {Stage::Compile, "compile", false, ids::kCompile, "the compile stage (the application's compile-error hook runs)"},
    {Stage::BindPlan, "bind-plan", false, ids::kBindPlan, "the bind planner of the first recompiled compiland"},
    {Stage::Reservation, "reservation", true, ids::kAgentReservation, "the agent's reservation of address space near the module"},
    {Stage::Link, "link", false, ids::kLink, "the link of the patch DLL"},
    {Stage::PreApply, "pre-apply", false, ids::kPreApply, "the broker's pre-applied relocations in the patch DLL"},
    {Stage::SendPatch, "send-patch", false, ids::kSendPatch, "sending LoadPatch to the agent"},
    {Stage::PatchLoad, "patch-load", true, ids::kAgentPatchLoad, "the agent's load of the patch (after the DLL is mapped: it is unloaded again)"},
    {Stage::HookInstall, "hook-install", true, ids::kAgentHookInstall, "hook installation, inside the window with the application's threads suspended"},
    {Stage::PostPatchHooks, "post-patch-hooks", true, ids::kAgentPostPatchHooks, "after the post-patch hooks ran: the installed hooks are rolled back"},
};
static_assert(sizeof(kStages) / sizeof(kStages[0]) == kStageCount, "every stage has a table row");

inline constexpr const StageInfo& info(Stage stage) noexcept {
    return kStages[static_cast<std::size_t>(stage)];
}

// nullptr for an unknown name.
inline const StageInfo* find_stage(std::string_view name) noexcept {
    for (const StageInfo& s : kStages) {
        if (name == s.name) {
            return &s;
        }
    }
    return nullptr;
}

// The name that disarms every stage.
inline constexpr const char* kDisarm = "none";
inline constexpr std::uint32_t kMaxCount = 1000;

}  // namespace cml::ipc::fault
