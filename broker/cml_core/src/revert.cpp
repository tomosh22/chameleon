#include "cml/core/revert.h"

#include <map>
#include <set>
#include <utility>

#include "cml/core/pipeline_diag_ids.h"
#include "cml/core/revert_diag_ids.h"
#include "cml/util/log.h"

namespace cml::core {

namespace rd = revert::diag;

namespace {

// One InstallHooks request; true when the agent confirmed it, false when no answer came in time (the agent may still
// act on it: synchronised mode applies it at the next SyncPoint()).
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
    report.pid = link.snapshot().pid;
    report.module = module.name;

    // ---- plan (broker state only) -------------------------------------------------------------------------------------
    std::size_t first = 0;                       // index of the oldest batch to revert
    std::vector<PatchBatch> reverted;            // copies, oldest first
    std::map<std::uint64_t, AppliedHook> older;  // old entry -> hook of the newest patch that stays
    std::map<std::uint64_t, std::uint64_t> current;
    {
        std::lock_guard<std::mutex> lock(module.mutex);
        report.generation_before = module.generation;
        report.generation = module.generation;
        if (module.batches.empty()) {
            return report;
        }
        first = scope == RevertScope::Last ? module.batches.size() - 1 : 0;
        reverted.assign(module.batches.begin() + static_cast<std::ptrdiff_t>(first), module.batches.end());
        for (std::size_t i = 0; i < first; ++i) {
            for (const AppliedPatch& p : module.batches[i].patches) {
                for (const AppliedHook& h : p.hooks) {
                    older[h.old_entry] = h;  // later patches overwrite: the newest remaining one wins
                }
            }
        }
        current = module.hooked;
    }
    std::map<std::uint64_t, std::string> affected;  // old entry -> a name for diagnostics
    for (const PatchBatch& b : reverted) {
        report.batches += 1;
        report.patches += b.patches.size();
        for (const AppliedPatch& p : b.patches) {
            for (const AppliedHook& h : p.hooks) {
                affected.emplace(h.old_entry, h.name);
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
        Result<bool> r = send_hooks(link, ipc::kInstallHooksQuiet, repoint, options.timeout_ms, "re-pointing to the previous patch", &report.warnings);
        if (!r) {
            return r.error();
        }
        repointed = true;
    }
    if (!restore.empty()) {
        Result<bool> r = send_hooks(link, ipc::kInstallHooksRevert, restore, options.timeout_ms, "restoring of the original code", &report.warnings);
        if (!r) {
            if (repointed) {
                // Put the re-pointed functions back where they were, so the refusal leaves the process as it was.
                std::vector<ipc::HookSpec> undo;
                for (const ipc::HookSpec& h : repoint) {
                    const auto c = current.find(h.old_entry);
                    if (c != current.end()) {
                        undo.push_back(ipc::HookSpec{h.old_entry, c->second, h.name});
                    }
                }
                std::vector<Diag> ignored;
                Result<bool> back = send_hooks(link, ipc::kInstallHooksQuiet, std::move(undo), options.timeout_ms, "undo of the re-pointing", &ignored);
                if (!back) {
                    return Diag::make(rd::kRevertPartial,
                                      "the revert failed (" + r.error().message + ") and the functions it had already re-pointed could not be put back: " + back.error().message,
                                      "The process runs a mix of patches. Restart the application, or reload again.");
                }
            }
            return r.error();
        }
    }
    report.hooks_restored = restore.size();
    report.hooks_repointed = repoint.size();

    // ---- the broker's state goes back ---------------------------------------------------------------------------------
    {
        std::lock_guard<std::mutex> lock(module.mutex);
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
        module.batches.resize(first);
    }
    if (options.events != nullptr) {
        Event e;
        e.name = "patch_reverted";
        e.body = ipc::json::Value::object();
        e.body.set("event", ipc::json::Value::string("patch_reverted"));
        e.body.set("pid", ipc::json::Value::integer(static_cast<std::int64_t>(report.pid)));
        e.body.set("module", ipc::json::Value::string(module.name));
        e.body.set("generation", ipc::json::Value::integer(static_cast<std::int64_t>(report.generation)));
        options.events->publish(e);
    }
    CML_LOG(Info, "revert[%s]: done, generation %llu -> %llu", module.name.c_str(), static_cast<unsigned long long>(report.generation_before),
            static_cast<unsigned long long>(report.generation));
    return report;
}

}  // namespace cml::core
