#pragma once

// "Re-execute function after a crash" (plan section 4.11, ADR-0035, WP-23): the broker's half of the decision.
//
// The agent reports the stack of a held thread with the `Exception` message (frames, innermost first, with the registers each
// frame has). From the frames, the PDB of the enabled modules and the hook table this file decides
//   * which frame would be re-executed: the innermost frame whose function a patch applied AFTER the fault has replaced
//     (`hooked[old entry]` differs from what it was when the fault was reported);
//   * whether that is safe to offer: the function's compiland was built without optimisation, every register parameter has a
//     type the x64 convention places in RCX/RDX/R8/R9 or XMM0..XMM3, and the PDB records where each of them lives;
//   * the recipe the agent applies: for each register parameter its size, class and a register-relative location.
//
// Never offered when unsure: every "no" carries a reason with a CML59xx id.
//
// Portable: no Win32; the unit tests run it on synthetic ProcInfo records and on the PDBs of the fixtures.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cml/core/module_state.h"
#include "cml/ipc/messages.h"
#include "cml/pdb/index.h"

namespace cml::core::reexecute {

// What the recipe builder decided for one frame.
struct RecipeResult {
    bool ok = false;
    std::uint32_t diag = 0;  // diag::k* when !ok
    std::string reason;      // when !ok: one sentence naming the function and the cause
    std::vector<ipc::ReexecuteParam> params;  // when ok: one per register parameter (position < 4)
};

// Decides whether the function described by `proc` can be re-executed from `frame`, and how to reload its register arguments.
// `optimisation`: what the compiland's recorded build command says (pdb::OptimizationHint); only Disabled is accepted.
// `frame_rva`: the RVA of the frame's rip (selects the S_DEFRANGE range). Pure function.
RecipeResult build_recipe(const pdb::ProcInfo& proc, pdb::OptimizationHint optimisation, std::uint32_t frame_rva, const ipc::ExceptionFrame& frame);

// Entry VA of a hooked function -> VA of the code it jumps to, merged over all enabled modules of the process.
using HookMap = std::map<std::uint64_t, std::uint64_t>;
HookMap snapshot_hooks(const std::vector<std::shared_ptr<ModuleState>>& modules);

// The verdict for one held thread.
struct Offer {
    bool offered = false;
    std::uint32_t diag = 0;      // !offered: diag::k*
    std::string reason;          // !offered: "CML5901: ..."
    std::uint32_t frame_index = 0;  // offered: the frame to re-execute
    std::uint64_t entry_rsp = 0;    // offered: the RSP the agent reported for that frame
    std::uint64_t target = 0;       // offered: entry of the replacement function
    std::string function;           // offered: its display name
    std::vector<ipc::ReexecuteParam> params;

    bool same_as(const Offer& other) const { return offered == other.offered && reason == other.reason && frame_index == other.frame_index && target == other.target; }
};

// Evaluates the thread `exception` describes. `at_fault` is snapshot_hooks() taken when the fault was reported; `modules` are the
// process's enabled modules now.
Offer evaluate(const std::vector<std::shared_ptr<ModuleState>>& modules, const ipc::Exception& exception, const HookMap& at_fault);

}  // namespace cml::core::reexecute
