#include "cml/core/revert.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <utility>

#include "cml/core/pipeline_diag_ids.h"
#include "cml/core/revert_diag_ids.h"
#include "cml/util/log.h"
#include "cml/util/path.h"

namespace cml::core {

namespace rd = revert::diag;
using Clock = std::chrono::steady_clock;

namespace {

// The time left for the next step, capped by the per-request limit; nullopt when the deadline has passed.
std::optional<std::uint32_t> step_budget(const RevertOptions& options) {
    if (options.deadline == Clock::time_point::max()) {
        return options.timeout_ms;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(options.deadline - Clock::now()).count();
    if (left <= 0) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(std::min<long long>(left, options.timeout_ms));
}

Diag out_of_time(const char* step) {
    return Diag::make(rd::kRevertTimeout, std::string("the revert ran out of time before ") + step, "Raise `timeout_ms` of the revert command, then try again.");
}

// One InstallHooks request; true when the agent confirmed it, false when no answer came in time (the agent may still act on
// it: synchronised mode applies it at the next SyncPoint()).
Result<bool> send_hooks(AgentLink& link, std::uint32_t flags, std::vector<ipc::HookSpec> hooks, std::uint32_t timeout_ms, const char* what,
                        std::vector<Diag>* warnings) {
    ipc::InstallHooks request;
    request.request_id = link.next_request_id();
    request.flags = flags;
    request.hooks = std::move(hooks);
    const std::size_t count = request.hooks.size();
    Result<ipc::Message> answer = link.request(ipc::Message(std::move(request)), timeout_ms);
    if (!answer) {
        if (answer.error().code == pipeline::diag::kAgentRequestTimeout) {
            warnings->push_back(Diag::make(rd::kRevertUnconfirmed,
                                           std::string("the agent did not confirm the ") + what + " of " + std::to_string(count) + " function(s) in time: " + answer.error().message,
                                           "In synchronised mode the application must call SyncPoint(); the change may still be applied there."));
            return false;
        }
        return answer.error();
    }
    const auto* done = std::get_if<ipc::HooksInstalled>(&*answer);
    if (done == nullptr) {
        const auto* reply = std::get_if<ipc::Reply>(&*answer);
        return Diag::make(rd::kRevertRefused, std::string("the agent refused the ") + what + ": " + (reply != nullptr ? reply->message : std::string("unexpected answer")),
                          "Nothing was changed. See the agent log, then try again.");
    }
    if (done->status != 0) {
        return Diag::make(rd::kRevertRefused, std::string("the agent refused the ") + what + ": CML" + std::to_string(done->status) + ": " + done->message,
                          "Nothing was changed. If a thread keeps running inside the function, wait until the application leaves it and try again.");
    }
    return true;
}

}  // namespace

Result<RevertReport> revert_module(AgentLink& link, ModuleState& module, RevertScope scope, const RevertOptions& options) {
    RevertReport report;
    const LinkSnapshot snapshot = link.snapshot();
    report.pid = snapshot.pid;
    report.module = module.name;

    // ---- plan: reads the broker's lists in place, copies only what the hook requests need ------------------------------
    std::size_t first = 0;                       // index of the oldest batch to revert
    std::map<std::uint64_t, std::string> affected;  // old entry -> a name for diagnostics
    std::map<std::uint64_t, AppliedHook> older;     // affected old entry -> hook of the newest patch that stays
    std::map<std::uint64_t, std::uint64_t> current; // affected old entry -> where it jumps now (for the undo)
    std::vector<std::uint64_t> patch_bases;         // images of the reverted patches, newest first
    {
        std::lock_guard<std::mutex> lock(module.mutex);
        report.generation_before = module.generation;
        report.generation = module.generation;
        if (module.batches.empty()) {
            return report;
        }
        // The module must still be loaded where the patches were made (the saved bytes and targets are of that image).
        bool loaded = false;
        for (const LoadedModule& m : snapshot.modules) {
            loaded = loaded || (m.base == module.base && path::same_path(m.path, module.path));
        }
        if (!loaded) {
            const std::size_t dropped = module.batches.size();
            module.batches.clear();
            module.hooked.clear();
            return Diag::make(rd::kModuleGone,
                              "the module '" + module.path + "' is no longer loaded in process " + std::to_string(snapshot.pid) + "; its " + std::to_string(dropped) +
                                  " applied reload(s) can no longer be reverted and were forgotten",
                              "Enable the module again, then reload.");
        }
        first = scope == RevertScope::Last ? module.batches.size() - 1 : 0;
        if (scope == RevertScope::Last && options.only_reload != 0 && module.batches.back().reload_id != options.only_reload) {
            return report;  // the newest reload did not touch this module
        }
        for (std::size_t i = first; i < module.batches.size(); ++i) {
            const PatchBatch& b = module.batches[i];
            report.batches += 1;
            report.patches += b.patches.size();
            report.reload_id = b.reload_id;
            for (const AppliedPatch& p : b.patches) {
                for (const AppliedHook& h : p.hooks) {
                    affected.emplace(h.old_entry, h.name);
                }
            }
        }
        for (std::size_t i = 0; i < first; ++i) {
            for (const AppliedPatch& p : module.batches[i].patches) {
                for (const AppliedHook& h : p.hooks) {
                    if (affected.count(h.old_entry) != 0) {
                        older[h.old_entry] = h;  // later patches overwrite: the newest remaining one wins
                    }
                }
            }
        }
        for (const auto& a : affected) {
            if (const auto it = module.hooked.find(a.first); it != module.hooked.end()) {
                current[a.first] = it->second;
            }
        }
        for (std::size_t i = module.batches.size(); i > first; --i) {
            const std::vector<AppliedPatch>& patches = module.batches[i - 1].patches;
            for (std::size_t j = patches.size(); j > 0; --j) {
                patch_bases.push_back(patches[j - 1].base);
            }
        }
    }
    std::vector<ipc::HookSpec> repoint;
    std::vector<ipc::HookSpec> restore;
    for (const auto& a : affected) {
        const auto it = older.find(a.first);
        if (it != older.end()) {
            repoint.push_back(ipc::HookSpec{a.first, it->second.new_entry, a.second});
        } else {
            restore.push_back(ipc::HookSpec{a.first, 0, a.second});
        }
    }
    CML_LOG(Info, "revert[%s]: %s: %zu batch(es), %zu patch(es), %zu function(s) back to the original code, %zu re-pointed at an older patch", module.name.c_str(),
            scope == RevertScope::Last ? "last" : "all", report.batches, report.patches, restore.size(), repoint.size());

    // ---- the hooks (agent side; the stop-the-world installer) -----------------------------------------------------
    bool repointed = false;
    if (!repoint.empty()) {
        const std::optional<std::uint32_t> budget = step_budget(options);
        if (!budget) {
            return out_of_time("the hooks were re-pointed");
        }
        Result<bool> r = send_hooks(link, ipc::kInstallHooksQuiet, repoint, *budget, "re-pointing to the previous patch", &report.warnings);
        if (!r) {
            return r.error();
        }
        repointed = true;
    }
    if (!restore.empty()) {
        const std::optional<std::uint32_t> budget = step_budget(options);
        std::optional<Diag> failure;
        if (!budget) {
            failure = out_of_time("the original code was restored");
        } else {
            Result<bool> r = send_hooks(link, ipc::kInstallHooksRevert, restore, *budget, "restoring of the original code", &report.warnings);
            if (!r) {
                failure = r.error();
            }
        }
        if (failure) {
            if (repointed) {
                // Put the re-pointed functions back where they were, so the refusal leaves the process as it was. This gets its
                // own short budget: the deadline of the revert is spent or about to be.
                std::vector<ipc::HookSpec> undo;
                for (const ipc::HookSpec& h : repoint) {
                    const auto c = current.find(h.old_entry);
                    if (c != current.end()) {
                        undo.push_back(ipc::HookSpec{h.old_entry, c->second, h.name});
                    }
                }
                std::vector<Diag> ignored;
                Result<bool> back = send_hooks(link, ipc::kInstallHooksQuiet, std::move(undo), std::min<std::uint32_t>(options.timeout_ms, 5000), "undo of the re-pointing", &ignored);
                if (!back) {
                    return Diag::make(rd::kRevertMixed,
                                      "the revert failed (" + failure->message + ") and the functions it had already re-pointed could not be put back: " + back.error().message,
                                      "The process runs a mix of patches. Restart the application, or reload again.");
                }
            }
            return *failure;
        }
    }
    report.hooks_restored = restore.size();
    report.hooks_repointed = repoint.size();

    // ---- the application's hooks in the reverted patches stop running ---------------------------------------------------
    {
        std::size_t failed = 0;
        std::string first_failure;
        for (const std::uint64_t base : patch_bases) {
            const std::optional<std::uint32_t> budget = step_budget(options);
            std::string why;
            if (!budget) {
                why = "the revert ran out of time";
            } else {
                ipc::ReleaseRange request;
                request.request_id = link.next_request_id();
                request.base = base;
                request.mode = ipc::ReleaseMode::Uncommit;
                Result<ipc::Message> answer = link.request(ipc::Message(std::move(request)), *budget);
                if (!answer) {
                    why = answer.error().message;
                } else if (const auto* reply = std::get_if<ipc::Reply>(&*answer); reply == nullptr || reply->status != 0) {
                    why = reply != nullptr ? "CML" + std::to_string(reply->status) + ": " + reply->message : std::string("unexpected answer");
                }
            }
            if (!why.empty()) {
                if (failed++ == 0) {
                    first_failure = why;
                }
            }
        }
        if (failed != 0) {
            report.warnings.push_back(Diag::make(rd::kUncommitFailed,
                                                 "the agent did not un-commit " + std::to_string(failed) + " of " + std::to_string(patch_bases.size()) +
                                                     " reverted patch(es): " + first_failure + ". Application hooks (CML_PREPATCH_HOOK, ...) defined in them may keep running",
                                                 "Use an agent from the same build as the broker; reloading again replaces the hooks of the same functions."));
        }
    }

    // ---- the broker's state goes back ---------------------------------------------------------------------------------
    {
        std::lock_guard<std::mutex> lock(module.mutex);
        std::vector<PatchBatch> reverted(std::make_move_iterator(module.batches.begin() + static_cast<std::ptrdiff_t>(first)),
                                         std::make_move_iterator(module.batches.end()));
        module.batches.resize(first);
        for (const ipc::HookSpec& h : restore) {
            module.hooked.erase(h.old_entry);
        }
        for (const ipc::HookSpec& h : repoint) {
            module.hooked[h.old_entry] = h.new_entry;
        }
        for (auto b = reverted.rbegin(); b != reverted.rend(); ++b) {
            for (auto p = b->patches.rbegin(); p != b->patches.rend(); ++p) {
                for (auto u = p->registry.rbegin(); u != p->registry.rend(); ++u) {
                    module.registry.restore(u->name, u->previous);
                }
            }
            if (module.index) {
                module.index->restore_baseline(b->baseline_before);
            }
        }
        module.generation = reverted.front().generation_before;
        report.generation = module.generation;
    }
    if (options.events != nullptr) {
        options.events->publish(make_patch_reverted_event(report.pid, module.name, report.generation));
    }
    CML_LOG(Info, "revert[%s]: done, generation %llu -> %llu", module.name.c_str(), static_cast<unsigned long long>(report.generation_before),
            static_cast<unsigned long long>(report.generation));
    return report;
}

}  // namespace cml::core
