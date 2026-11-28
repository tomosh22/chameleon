#pragma once

// WP-54 (ADR-0045, R-56): a reload of several modules of one process applies all of them or none.
//
// The reload service hands the enabled modules of ONE process that a reload covers to reload_process(); every process is a call of
// its own, so processes stay independent (a refusal in one changes nothing in another). For a process with several modules (an exe
// and its DLLs, a header both include):
//
//   prepare  every module is swept, compiled, planned, verified and linked (ModuleReload::prepare, the prepare phase of WP-47) before
//            any is applied. The refusal of one leaves every module of the process unchanged: the modules prepared before it give
//            their address space back and re-baseline nothing, the ones after it are not prepared (CML3656, which says whether it was a
//            refusal, a CML3xxx of the planner, a proof, the anchor, the link or the fix-ups, or another failure such as a compile
//            error, the deadline or an injected fault). Its id stays the reply's.
//   apply    then module by module (each one all or nothing over its compilands, WP-47). When applying module k fails, the modules this
//            reload applied before it are rolled back with the WP-22 batch revert of this reload (revert_module, scope last, only this
//            reload's batch), newest first (CML3657; CML3658 when a roll-back fails, CML3659 when it is not confirmed), and the modules
//            after k are not applied. As in WP-47 two cases roll nothing back: module k's patch may still become live (CML5309 / CML4656):
//            the modules before it stay applied, the reload stops there (CML3660); the agent is gone: nothing can be (CML3661).
//   events   `patch_applied` of the process is published once every module of it is applied, never for a module that was rolled back
//            or is not announced. What stays live after a failure is announced: the modules before an unconfirmed module (CML3660) and
//            that module's own compilands applied before its unconfirmed one (WP-47's CML3653).
//   baseline a rolled-back module's source baseline is what it was before the reload (the WP-22 revert puts back the stamps the
//            batch replaced; a sweep only reports files the baseline tracks, so those are all the reload re-baselined).
//
// A process with one module runs PatchPipeline::patch_module() exactly as before.

#include <optional>
#include <vector>

#include "cml/core/agent_link.h"
#include "cml/core/module_state.h"
#include "cml/core/pipeline.h"
#include "cml/util/result.h"

namespace cml::core {

struct ProcessReload {
    std::vector<Result<PatchReport>> modules;  // one per module passed in, in that order
    // The process's error, only set when a module of the process failed: the refusal or failure itself (its own id), its message
    // saying what became of the other modules; except when the roll-back of a module applied before it failed (CML3658) or was not
    // confirmed (CML3659): then that id, its message carrying the failure. The per-module errors of the modules that were only not
    // applied (or rolled back) are in `modules`.
    std::optional<Diag> error;
};

// `modules`: the modules of the process `link` serves, in the order they are prepared and applied (non-empty).
ProcessReload reload_process(const PatchPipeline& pipeline, AgentLink& link, const std::vector<ModuleState*>& modules, const PipelineContext& context);

}  // namespace cml::core
