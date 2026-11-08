#include "cml/core/reexecute.h"

#include <algorithm>
#include <cstdio>
#include <optional>

#include "cml/core/bind_names.h"
#include "cml/core/module_index.h"
#include "cml/core/reexecute_diag_ids.h"

namespace cml::core::reexecute {

namespace {

std::string hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof text, "0x%llX", static_cast<unsigned long long>(value));
    return text;
}

RecipeResult refuse(std::uint32_t id, const std::string& what) {
    RecipeResult r;
    r.diag = id;
    r.reason = "CML" + std::to_string(id) + ": " + what;
    return r;
}

bool supported_base(std::uint16_t reg) noexcept {
    return reg == pdb::kCvRegRsp || reg == pdb::kCvRegRbp || reg == pdb::kCvRegR13;
}

std::optional<std::uint64_t> register_value(const ipc::ExceptionFrame& frame, std::uint16_t reg) noexcept {
    switch (reg) {
        case pdb::kCvRegRsp: return frame.rsp;
        case pdb::kCvRegRbp: return frame.rbp;
        case pdb::kCvRegR13: return frame.r13;
        default: return std::nullopt;
    }
}

const char* optimisation_name(pdb::OptimizationHint hint) {
    return pdb::to_string(hint);
}

}  // namespace

RecipeResult build_recipe(const pdb::ProcInfo& proc, pdb::OptimizationHint optimisation, std::uint32_t frame_rva, const ipc::ExceptionFrame& frame) {
    const std::string name = bind::display_name(proc.name);
    if (optimisation == pdb::OptimizationHint::Unknown) {
        return refuse(diag::kOptimisationUnknown, name + " is in a compiland whose recorded build command does not show that optimisation was off (no /Od); re-execute is only offered for unoptimised code");
    }
    if (optimisation != pdb::OptimizationHint::Disabled || proc.opt_for_speed) {
        return refuse(diag::kOptimised, name + " is in a compiland built with optimisation (" + std::string(optimisation_name(optimisation)) +
                                            "); its parameters are not kept in fixed places, so re-execute is only offered for code built with /Od");
    }
    if (!proc.params_known) {
        return refuse(diag::kSignatureUnsupported, name + ": " + (proc.params_error.empty() ? std::string("the parameters cannot be read from the PDB") : proc.params_error));
    }
    const std::size_t in_registers = std::min<std::size_t>(proc.params.size(), 4);
    for (std::size_t i = 0; i < in_registers; ++i) {
        if (proc.params[i].cls == pdb::ParamClass::Unsupported) {
            return refuse(diag::kParamUnsupported, name + ": parameter " + std::to_string(i + 1) + " is " + proc.params[i].why);
        }
    }
    RecipeResult result;
    if (in_registers == 0) {
        result.ok = true;
        return result;
    }

    // Parameters the compiler marks (S_LOCAL, clang-cl and newer MSVC), in declaration order.
    std::vector<const pdb::ProcVariable*> marked;
    for (const pdb::ProcVariable& v : proc.variables) {
        if (v.is_param) {
            marked.push_back(&v);
        }
    }
    if (!marked.empty()) {
        if (marked.size() != proc.params.size()) {
            return refuse(diag::kLocationUnknown, name + ": the PDB has records for " + std::to_string(marked.size()) + " parameters but the function type declares " +
                                                      std::to_string(proc.params.size()) + " (unnamed parameters have no record)");
        }
        for (std::size_t i = 0; i < in_registers; ++i) {
            const pdb::ProcVariable& v = *marked[i];
            const pdb::VarLocation* location = nullptr;
            for (const pdb::VarLocation& l : v.locations) {
                if (l.covers(frame_rva)) {
                    location = &l;
                    break;
                }
            }
            if (location == nullptr) {
                return refuse(diag::kLocationUnknown, name + ": the PDB records no location of parameter '" + v.name + "' at the faulting instruction" + (v.optimized_out ? " (optimised out)" : ""));
            }
            if (location->kind == pdb::VarLocationKind::Register) {
                return refuse(diag::kLocationUnknown, name + ": parameter '" + v.name + "' lives in a register at the faulting instruction, not in memory");
            }
            const std::uint16_t base = location->kind == pdb::VarLocationKind::FramePointerRelative ? proc.param_base_register : location->reg;
            if (!supported_base(base)) {
                return refuse(diag::kLocationUnknown, name + ": parameter '" + v.name + "' is addressed from a register re-execute cannot read (CodeView register " +
                                                          std::to_string(base) + ")");
            }
            ipc::ReexecuteParam p;
            p.position = static_cast<std::uint32_t>(i);
            p.is_float = proc.params[i].cls == pdb::ParamClass::Float ? 1u : 0u;
            p.size = proc.params[i].size;
            p.base_register = base;
            p.offset = location->offset;
            result.params.push_back(p);
        }
        result.ok = true;
        return result;
    }

    // MSVC at /Od spills each register parameter into its home space and records it as [reg + offset] (S_REGREL32) with no
    // parameter flag: parameter i is the record whose address is the home slot of position i, entry RSP + 8 + 8 * i.
    for (std::size_t i = 0; i < in_registers; ++i) {
        const std::uint64_t home = frame.entry_rsp + 8 + 8 * static_cast<std::uint64_t>(i);
        const pdb::ProcVariable* best = nullptr;
        const pdb::VarLocation* best_location = nullptr;
        for (const pdb::ProcVariable& v : proc.variables) {
            if (!v.from_regrel || v.locations.empty()) {
                continue;
            }
            const pdb::VarLocation& l = v.locations.front();
            const std::optional<std::uint64_t> base = register_value(frame, l.reg);
            if (!base || *base + static_cast<std::int64_t>(l.offset) != home) {
                continue;
            }
            if (best == nullptr || (v.type_index == proc.params[i].type_index && best->type_index != proc.params[i].type_index)) {
                best = &v;
                best_location = &l;
            }
        }
        if (best == nullptr) {
            return refuse(diag::kLocationUnknown, name + ": no record in the PDB places parameter " + std::to_string(i + 1) + " at its home slot " + hex(home) +
                                                      " (the compiler did not spill it there)");
        }
        ipc::ReexecuteParam p;
        p.position = static_cast<std::uint32_t>(i);
        p.is_float = proc.params[i].cls == pdb::ParamClass::Float ? 1u : 0u;
        p.size = proc.params[i].size;
        p.base_register = best_location->reg;
        p.offset = best_location->offset;
        result.params.push_back(p);
    }
    result.ok = true;
    return result;
}

HookMap snapshot_hooks(const std::vector<std::shared_ptr<ModuleState>>& modules) {
    HookMap all;
    for (const std::shared_ptr<ModuleState>& module : modules) {
        if (module == nullptr) {
            continue;
        }
        std::lock_guard<std::mutex> lock(module->mutex);
        for (const auto& [old_entry, target] : module->hooked) {
            all[old_entry] = target;
        }
    }
    return all;
}

namespace {

Offer not_offered(std::uint32_t id, std::string reason) {
    Offer o;
    o.diag = id;
    o.reason = std::move(reason);
    return o;
}

std::string with_id(std::uint32_t id, const std::string& what) {
    return "CML" + std::to_string(id) + ": " + what;
}

// The enabled module whose image contains `address`.
std::shared_ptr<ModuleState> module_of(const std::vector<std::shared_ptr<ModuleState>>& modules, std::uint64_t address) {
    for (const std::shared_ptr<ModuleState>& m : modules) {
        if (m != nullptr && address >= m->base && address - m->base < m->size) {
            return m;
        }
    }
    return nullptr;
}

}  // namespace

Offer evaluate(const std::vector<std::shared_ptr<ModuleState>>& modules, const ipc::Exception& exception, const HookMap& at_fault) {
    if (exception.frames.empty()) {
        return not_offered(diag::kNoStack, with_id(diag::kNoStack, "the agent did not report the stack of the held thread"));
    }
    const HookMap now = snapshot_hooks(modules);
    std::string static_reason;      // why the innermost examinable frame could not be re-executed, whichever patch comes
    std::uint32_t static_diag = 0;
    std::string frame0_note;        // the faulting frame itself could not be identified (patch code, PDB missing)
    for (std::size_t i = 0; i < exception.frames.size(); ++i) {
        const ipc::ExceptionFrame& frame = exception.frames[i];
        const std::uint64_t pc = i == 0 ? frame.rip : frame.rip - 1;
        const std::shared_ptr<ModuleState> module = module_of(modules, pc);
        if (module == nullptr) {
            if (i == 0) {
                frame0_note = "the faulting instruction (" + hex(frame.rip) + ") is not in an enabled module's image: it is patch code or another module, whose parameters the PDB does not describe";
            }
            continue;
        }
        const std::shared_ptr<const pdb::PdbIndex> index = module->index != nullptr ? module->index->pdb() : nullptr;
        if (index == nullptr) {
            if (i == 0) {
                frame0_note = "the PDB of " + module->name + " is not loaded";
            }
            continue;
        }
        const std::uint64_t rva = pc - module->base;
        Result<std::optional<pdb::SymbolView>> proc = rva <= 0xFFFFFFFFull ? index->find_proc_by_rva(static_cast<std::uint32_t>(rva))
                                                                          : Result<std::optional<pdb::SymbolView>>(std::optional<pdb::SymbolView>());
        if (!proc || !*proc) {
            if (i == 0) {
                frame0_note = "the faulting instruction (" + hex(frame.rip) + ") is in no function of " + module->name + "'s PDB";
            }
            continue;
        }
        const std::uint64_t entry_va = module->base + (*proc)->rva;
        const auto current = now.find(entry_va);
        const auto before = at_fault.find(entry_va);
        const bool replaced = current != now.end() && (before == at_fault.end() || before->second != current->second);

        RecipeResult recipe;
        {
            Result<pdb::ProcInfo> info = index->proc_info(**proc);
            if (!info) {
                recipe = refuse(diag::kSignatureUnsupported, bind::display_name((*proc)->name) + ": " + info.error().message);
            } else {
                pdb::OptimizationHint hint = pdb::OptimizationHint::Unknown;
                Result<pdb::BuildEnv> env = index->build_env((*proc)->compiland);
                if (env) {
                    hint = env->optimization();
                }
                recipe = build_recipe(*info, hint, static_cast<std::uint32_t>(rva), frame);
            }
        }
        if (!replaced) {
            if (!recipe.ok && static_reason.empty()) {
                static_reason = recipe.reason;
                static_diag = recipe.diag;
            }
            continue;
        }
        if (!recipe.ok) {
            // The patch arrived (that is news to the client, whose event said "no patch yet"), but the verdict is the same as before.
            return not_offered(recipe.diag, recipe.reason + "; a patch has replaced " + bind::display_name((*proc)->name) + ", but it cannot be re-executed");
        }
        Offer o;
        o.offered = true;
        o.frame_index = static_cast<std::uint32_t>(i);
        o.entry_rsp = frame.entry_rsp;
        o.target = current->second;
        o.function = bind::display_name((*proc)->name);
        o.params = std::move(recipe.params);
        if (frame.entry_rsp == 0) {
            return not_offered(diag::kNoStack, with_id(diag::kNoStack, "the agent could not unwind frame " + std::to_string(i) + " of the held thread"));
        }
        return o;
    }
    if (!frame0_note.empty()) {
        return not_offered(diag::kNoDebugInfo, with_id(diag::kNoDebugInfo, frame0_note + "; re-execute needs the PDB record of the function"));
    }
    if (!static_reason.empty()) {
        return not_offered(static_diag, static_reason);
    }
    return not_offered(diag::kNoReplacement, with_id(diag::kNoReplacement, "no patch applied since the fault has replaced a function on this thread's stack; apply a patch that fixes the function first"));
}

}  // namespace cml::core::reexecute
