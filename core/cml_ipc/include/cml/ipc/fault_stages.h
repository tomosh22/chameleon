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
    // WP-43 (ADR-0043): the hot-restart (restart.cpp) and the patch revert (revert.cpp). Append only: WP-44 adds its stages after these.
    PreRestartHooks,      // agent (the old process): after the application's pre-restart hooks ran, the preparation is refused
    RestartTerminate,     // broker: before the old process is terminated
    RestartRelaunch,      // broker: the first attempt to start the new process (the broker tries once more)
    RestartHold,          // broker: a held restart (hold_new) cannot hold the new process: it is resumed at once
    RestartRegistration,  // broker: the wait for the new process's agent to register
    RestartReenable,      // broker: the request that enables the old process's modules in the new one
    RestartReapply,       // broker: the reload that re-applies the session's edits to the new process
    RevertRepoint,        // agent: a revert's re-pointing of hooks to an older patch, inside the window with the threads suspended
    RevertRestore,        // agent: a revert's restoring of original bytes, inside the window with the threads suspended
    RevertUncommit,       // agent: the un-commit of a reverted patch image (ReleaseRange, mode Uncommit)
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
// WP-43 (ADR-0043): CML5972-5978 (broker) and CML4657-4679 (agent).
// A stage of the groups "restart" and "revert" is armed for the NEXT restart (revert) of its process only: at the end of that operation
// it is disarmed whether it fired or not (it never carries over to a later one), and its count must be 1.
inline constexpr std::uint32_t kAgentPreRestartHooks = 4657;
inline constexpr std::uint32_t kAgentRevertRepoint = 4658;
inline constexpr std::uint32_t kAgentRevertRestore = 4659;
inline constexpr std::uint32_t kAgentRevertUncommit = 4660;
inline constexpr std::uint32_t kRestartTerminate = 5972;
inline constexpr std::uint32_t kRestartRelaunch = 5973;
inline constexpr std::uint32_t kRestartHold = 5974;
inline constexpr std::uint32_t kRestartRegistration = 5975;
inline constexpr std::uint32_t kRestartReenable = 5976;
inline constexpr std::uint32_t kRestartReapply = 5977;
inline constexpr std::uint32_t kInjectBusy = 5978;           // inject-failure: a restart / revert stage while a restart / revert runs
}  // namespace ids

struct StageInfo {
    Stage stage;
    const char* name;         // the name used by `inject-failure` and listed by `fault-stages`
    bool agent_side;          // armed in the agent through ArmFault
    std::uint32_t diag_id;    // the diagnostic the failure carries
    const char* where;        // one line: what is made to fail
    // WP-43: which path the stage belongs to: "reload" (WP-28's pipeline stages), "restart", "revert" (WP-44 adds its own). A
    // default, so the rows of the earlier stages stay as they were (the table is append-only).
    const char* group = "reload";
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
    // WP-43: the hot-restart, in the order of its sequence, then the revert.
    {Stage::PreRestartHooks, "pre-restart-hooks", true, ids::kAgentPreRestartHooks,
     "the old process's preparation, after its pre-restart hooks ran: it refuses, so the process is not touched", "restart"},
    {Stage::RestartTerminate, "restart-terminate", false, ids::kRestartTerminate, "the termination of the old process (it is not terminated)", "restart"},
    {Stage::RestartRelaunch, "restart-relaunch", false, ids::kRestartRelaunch,
     "the first attempt to start the new process, after the old one is gone (the broker starts it on its second attempt)", "restart"},
    {Stage::RestartHold, "restart-hold", false, ids::kRestartHold,
     "holding the new process of a restart with hold_new for the debugger (it is resumed at once, the restart goes on)", "restart"},
    {Stage::RestartRegistration, "restart-registration", false, ids::kRestartRegistration,
     "the wait for the new process to register (it is left running; the next restart of it enables its modules and re-applies the edits)", "restart"},
    {Stage::RestartReenable, "restart-reenable", false, ids::kRestartReenable,
     "the request to enable the old process's modules in the new one (not sent; the next restart enables them)", "restart"},
    {Stage::RestartReapply, "restart-reapply", false, ids::kRestartReapply,
     "the reload that re-applies the session's edits to the new process (it runs the original code until the next reload)", "restart"},
    {Stage::RevertRepoint, "revert-repoint", true, ids::kAgentRevertRepoint,
     "a revert's re-pointing of hooks to an older patch, inside the window with the application's threads suspended; nothing is written", "revert"},
    {Stage::RevertRestore, "revert-restore", true, ids::kAgentRevertRestore,
     "a revert's restoring of original bytes, inside the window with the application's threads suspended; nothing is written", "revert"},
    {Stage::RevertUncommit, "revert-uncommit", true, ids::kAgentRevertUncommit,
     "the un-commit of a reverted patch image (its application hooks may keep running; the revert itself succeeds)", "revert"},
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
