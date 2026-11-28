#pragma once

// WP-54 (ADR-0045, R-56): a reload of several modules of one process applies all of them or none.
//
// The reload service hands the enabled modules of ONE process that a reload covers to reload_process(); every process is a call of
// its own, so processes stay independent (a refusal in one changes nothing in another). For a process with several modules (an exe
// and its DLLs, a header both include):
//
//   prepare  every module is swept, compiled, planned, verified and linked (ModuleReload::prepare, the prepare phase of WP-47) before
//            any is applied. The refusal of one leaves every module of the process unchanged: the modules prepared before it give
//            their address space back and re-baseline nothing, the ones after it are not prepared (CML3656). Its id stays the reply's.
//   apply    then module by module (each one all or nothing over its compilands, WP-47). When applying module k fails, the modules this
//            reload applied before it are rolled back with the WP-22 batch revert of this reload (revert_module, scope last, only this
//            reload's batch), newest first (CML3657; CML3658 when a roll-back fails, CML3659 when it is not confirmed), and the modules
//            after k are not applied. As in WP-47 two cases roll nothing back: module k's patch may still become live (CML5309 / CML4656):
//            the modules before it stay applied, the reload stops there (CML3660); the agent is gone: nothing can be (CML3661).
//   events   `patch_applied` of the process is published once every module of it is applied (or stays applied, CML3660), never for a
//            module that was rolled back or is not announced.
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
    // The process's error: the refusal or failure itself (its own id), its message saying what became of the other modules. Only set
    // when a module of the process failed; the per-module errors of the modules that were only not applied are in `modules`.
    std::optional<Diag> error;
};

// `modules`: the modules of the process `link` serves, in the order they are prepared and applied (non-empty).
ProcessReload reload_process(const PatchPipeline& pipeline, AgentLink& link, const std::vector<ModuleState*>& modules, const PipelineContext& context);

}  // namespace cml::core
