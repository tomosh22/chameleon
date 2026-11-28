#include "cml/core/process_reload.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "cml/core/atomic_reload_diag_ids.h"
#include "cml/core/event_bus.h"
#include "cml/core/pipeline_diag_ids.h"
#include "cml/core/revert.h"
#include "cml/core/revert_diag_ids.h"
#include "cml/util/log.h"

namespace cml::core {

namespace ar = atomic_reload::diag;
using Clock = std::chrono::steady_clock;

namespace {

// The least time a roll-back of a module gets, whatever the reload has left (the failure may be the reload's deadline; WP-47's rule).
constexpr std::int64_t kRollbackBudgetMs = 10000;

std::string names_of(const std::vector<ModuleState*>& modules, const std::vector<std::size_t>& which) {
    std::string out;
    for (const std::size_t i : which) {
        out += (out.empty() ? "" : ", ") + modules[i]->name;
    }
    return out;
}

class ProcessRun {
public:
    ProcessRun(const PatchPipeline& pipeline, AgentLink& link, const std::vector<ModuleState*>& modules, const PipelineContext& ctx)
        : pipeline_(pipeline), link_(link), modules_(modules), ctx_(ctx), pid_(link.snapshot().pid), results_(modules.size()) {}

    ProcessReload run() {
        const std::size_t n = modules_.size();
        // ---- 1. prepare every module ----------------------------------------------------------------------------------
        for (std::size_t i = 0; i < n; ++i) {
            runs_.push_back(std::make_unique<ModuleReload>(pipeline_, link_, *modules_[i], ctx_, true));
            Result<void> prepared = runs_[i]->prepare();
            if (!prepared) {
                results_[i] = prepared.error();
                return refused(i, prepared.error());
            }
            CML_LOG(Trace, "reload: module %s%s prepared%s", modules_[i]->name.c_str(), where(i).c_str(), runs_[i]->prepared() ? "" : ": nothing to compile");
        }
        CML_LOG(Trace, "reload: all %zu modules of process %u are prepared; applying", n, pid_);

        // ---- 2. apply them in order -------------------------------------------------------------------------------------
        std::vector<std::size_t> applied;  // modules that hold a batch of this reload, oldest first
        for (std::size_t i = 0; i < n; ++i) {
            Result<PatchReport> r = runs_[i]->apply();
            if (!r) {
                results_[i] = r.error();
                return apply_failed(i, r.error(), applied);
            }
            if (r->patches != 0) {
                applied.push_back(i);
            }
            results_[i] = std::move(r);
        }
        publish_events(n);
        return finish(std::nullopt);
    }

private:
    // " (module 2 of 2 of process 4711)"
    std::string where(std::size_t i) const {
        return " (module " + std::to_string(i + 1) + " of " + std::to_string(modules_.size()) + " of process " + std::to_string(pid_) + ")";
    }
    void emit(const Diag& d, const char* severity, const char* stage) {
        if (std::string(severity) == "error") {
            CML_LOG(Error, "reload[%s]: %s", stage, d.format().c_str());
        } else {
            CML_LOG(Warn, "reload[%s]: %s", stage, d.format().c_str());
        }
        if (ctx_.events != nullptr) {
            ctx_.events->publish(make_diag_event(d, severity, stage, pid_));
        }
    }
    // The `patch_applied` events of the modules before `end`, in module order.
    void publish_events(std::size_t end) {
        for (std::size_t i = 0; i < end && i < runs_.size(); ++i) {
            for (Event& e : runs_[i]->take_applied_events()) {
                if (ctx_.events != nullptr) {
                    ctx_.events->publish(std::move(e));
                }
            }
        }
    }
    // The `patch_applied` events module `i` holds: its compilands that are live although its reload failed (WP-47's CML3653: hooks
    // that may still become live after earlier compilands were applied; those are recorded and were announced before WP-54).
    void publish_events_of(std::size_t i) {
        for (Event& e : runs_[i]->take_applied_events()) {
            if (ctx_.events != nullptr) {
                ctx_.events->publish(std::move(e));
            }
        }
    }
    // The baseline this reload's batch of module `i` replaced (empty when the module holds no batch of this reload).
    std::map<std::string, FileStamp> baseline_before(std::size_t i) {
        ModuleState& m = *modules_[i];
        std::lock_guard<std::mutex> lock(m.mutex);
        if (!m.batches.empty() && m.batches.back().reload_id == ctx_.reload_id) {
            return m.batches.back().baseline_before;
        }
        return {};
    }
    // A failure of a module's prepare phase is a refusal when its diagnostic is one of the bind / link range CML3xxx: the bind
    // planner, a code proof, the anchor, the link or the fix-ups refused the edit. Anything else is a failure: the PDB (CML1xxx), the
    // compiler (CML2xxx), the agent (CML4xxx, e.g. its reservation) and the broker's own CML5xxx (the deadline, no room in the
    // reservation, no linker driver, an injected fault, a cancellation, the agent lost).
    static bool is_refusal(const Diag& d) { return d.code >= 3000 && d.code <= 3999; }

    // The modules after `k`: a prepared one is abandoned (`why`), one with nothing to compile keeps its report, one that was never
    // prepared gets `why` as well.
    void not_applied_after(std::size_t k, const Diag& why) {
        for (std::size_t j = k + 1; j < modules_.size(); ++j) {
            if (j < runs_.size() && !runs_[j]->prepared()) {
                results_[j] = runs_[j]->apply();  // nothing to compile: its report
            } else if (j < runs_.size()) {
                results_[j] = runs_[j]->abandon(why);
            } else {
                results_[j] = why;
            }
        }
    }
    ProcessReload finish(std::optional<Diag> error) {
        ProcessReload out;
        for (std::optional<Result<PatchReport>>& r : results_) {
            out.modules.push_back(r ? std::move(*r)
                                    : Result<PatchReport>(Diag::make(pipeline::diag::kStageFailed, "a module of the process has no result", "This is a bug in the broker.")));
        }
        out.error = std::move(error);
        return out;
    }

    // ---- module k was refused while it was prepared -------------------------------------------------------------------------
    ProcessReload refused(std::size_t k, Diag d) {
        const std::size_t n = modules_.size();
        std::vector<std::size_t> waiting;  // prepared before it, never applied
        const std::string stage = runs_[k]->failed_stage().empty() ? std::string("sweep") : runs_[k]->failed_stage();
        const bool refusal = is_refusal(d);
        // " was refused (CML3400)" / " failed while it was prepared (stage compile, CML2200)"
        const std::string what = refusal ? " was refused (" + d.id() + ")" : " failed while it was prepared (stage " + stage + ", " + d.id() + ")";
        const Diag why = Diag::make(ar::kModuleRefusedNothingApplied, "not applied: module " + modules_[k]->name + where(k) + what,
                                    refusal ? "Fix what the refusal names, then reload." : "Fix what the failure names, then reload.");
        for (std::size_t j = 0; j < k; ++j) {
            if (runs_[j]->prepared()) {
                waiting.push_back(j);
                results_[j] = runs_[j]->abandon(why);
            } else {
                results_[j] = runs_[j]->apply();  // nothing to compile: its report (patched == false)
            }
        }
        not_applied_after(k, why);
        CML_LOG(Trace, "reload: module %s%s%s: no module of the process is applied (%zu prepared before it, %zu not prepared after it)",
                modules_[k]->name.c_str(), where(k).c_str(), what.c_str(), waiting.size(), n - k - 1);
        if (waiting.empty() && k + 1 == n) {
            return finish(std::move(d));  // no other module of the process had anything to apply: as for a reload of one module
        }
        const std::string at = refusal ? "was refused at module " + std::to_string(k + 1) + " of " + std::to_string(n) + " (" + modules_[k]->name + ", " + d.id() + ")"
                                       : "stopped at module " + std::to_string(k + 1) + " of " + std::to_string(n) + " (" + modules_[k]->name +
                                             "), which failed while it was prepared (stage " + stage + ", " + d.id() + ")";
        emit(Diag::make(ar::kModuleRefusedNothingApplied,
                        "the reload of process " + std::to_string(pid_) + " " + at + ": none of its modules was applied (" + std::to_string(waiting.size()) +
                            " module(s) before it" + (waiting.empty() ? std::string() : " (" + names_of(modules_, waiting) + ")") +
                            " were planned and linked, none was loaded; " + std::to_string(n - k - 1) + " after it were not prepared); the process runs what it ran before",
                        std::string(refusal ? "Fix what the refusal names" : "Fix what the failure names") +
                            ", then reload: the edit is applied to every module then. Files the reload did not apply are compiled again."),
             "warning", refusal && (stage == "plan" || stage == "link") ? "plan" : stage.c_str());
        d.message += " (module " + std::to_string(k + 1) + " of " + std::to_string(n) + ", " + modules_[k]->name + "); no module of process " + std::to_string(pid_) +
                     " was applied";
        return finish(std::move(d));
    }

    // ---- applying module k failed (its own compilands are dealt with: WP-47) ---------------------------------------------------
    ProcessReload apply_failed(std::size_t k, Diag d, const std::vector<std::size_t>& applied) {
        const std::size_t n = modules_.size();
        const std::string head = "applying module " + std::to_string(k + 1) + " of " + std::to_string(n) + " of process " + std::to_string(pid_) + " (" +
                                 modules_[k]->name + ", " + d.id() + ") failed";
        std::size_t after = 0;  // prepared modules after k: not applied
        for (std::size_t j = k + 1; j < runs_.size(); ++j) {
            after += runs_[j]->prepared() ? 1 : 0;
        }
        if (applied.empty() && after == 0) {
            // No other module of the process had anything to apply: as for a reload of one module, including the `patch_applied` of
            // its compilands that stay live (WP-47's CML3653).
            not_applied_after(k, d);
            publish_events_of(k);
            return finish(std::move(d));
        }

        if (link_.closed()) {
            // Nothing can be rolled back. The modules applied before stay recorded (their batches), but none of their files is
            // re-baselined (the baseline they replaced comes back) and nothing of them is announced (WP-47's CML3655, per process).
            for (const std::size_t i : applied) {
                const std::map<std::string, FileStamp> before = baseline_before(i);
                if (modules_[i]->index) {
                    modules_[i]->index->restore_baseline(before);
                }
                (void)runs_[i]->take_applied_events();
            }
            const Diag why = Diag::make(ar::kModulesAgentLost, "not applied: the agent of process " + std::to_string(pid_) + " disconnected", "Restart the application.");
            for (const std::size_t i : applied) {
                results_[i] = Diag::make(ar::kModulesAgentLost, "applied, then the agent of process " + std::to_string(pid_) + " disconnected while " + modules_[k]->name +
                                                                    " was applied: kept recorded, not announced, not re-baselined",
                                         "Restart the application.");
            }
            not_applied_after(k, why);
            const std::string state = std::to_string(applied.size()) + " module(s) of this reload were applied before it" +
                                      (applied.empty() ? std::string() : " (" + names_of(modules_, applied) + ")") + ", " + std::to_string(after) + " after it were not";
            CML_LOG(Trace, "reload: %s and the agent is gone: %s", head.c_str(), state.c_str());
            emit(Diag::make(ar::kModulesAgentLost,
                            "the agent of process " + std::to_string(pid_) + " disconnected while module " + std::to_string(k + 1) + " of " + std::to_string(n) + " (" +
                                modules_[k]->name + ", " + d.id() + ") was applied: " + state + ", and nothing could be rolled back; if the process still runs, it runs a part of the edit",
                            "Restart the application (or hot-restart it); if it still runs, enable the modules again and reload: the reload's files are compiled again."),
                 "warning", "load");
            d.message += " (module " + std::to_string(k + 1) + " of " + std::to_string(n) + ", " + modules_[k]->name + "); the agent is gone: " + state;
            return finish(std::move(d));
        }

        if (runs_[k]->may_be_live()) {
            // Module k's patch may still become live (CML5309 / CML4656): rolling the others back would queue reverts behind it. The
            // modules before it stay applied (and are announced), the ones after it are not applied.
            const Diag why = Diag::make(ar::kModulesStoppedPartly, "not applied: module " + modules_[k]->name + where(k) + " may still become live (" + d.id() + ")",
                                        "Let the application reach SyncPoint(), then reload.");
            not_applied_after(k, why);
            publish_events(k + 1);  // the modules before it, and module k's compilands that stay live (WP-47's CML3653)
            CML_LOG(Trace, "reload: module %s%s may still become live (%s): no roll-back; %zu module(s) applied before it, %zu not applied after it",
                    modules_[k]->name.c_str(), where(k).c_str(), d.id().c_str(), applied.size(), after);
            emit(Diag::make(ar::kModulesStoppedPartly,
                            "module " + std::to_string(k + 1) + " of " + std::to_string(n) + " of process " + std::to_string(pid_) + " (" + modules_[k]->name + ", " + d.id() +
                                ") may still become live and cannot be rolled back, so the reload of the process stopped there: " + std::to_string(applied.size()) +
                                " module(s) before it are applied" + (applied.empty() ? std::string() : " (" + names_of(modules_, applied) + ")") + ", " +
                                std::to_string(after) + " after it are not; the process may run a part of the edit",
                            "In synchronised mode let the application reach SyncPoint(), then reload (the files of the modules that were not applied are compiled again), or `revert` this reload."),
                 "warning", "hooks");
            d.message += " (module " + std::to_string(k + 1) + " of " + std::to_string(n) + ", " + modules_[k]->name + "); " + std::to_string(after) +
                         " module(s) after it were not applied";
            return finish(std::move(d));
        }

        if (applied.empty()) {
            const Diag why = Diag::make(ar::kModulesRolledBack, "not applied: module " + modules_[k]->name + where(k) + " failed (" + d.id() + ")",
                                        "Fix what the failure names, then reload.");
            not_applied_after(k, why);
            CML_LOG(Trace, "reload: %s before any module of the process was applied: nothing to roll back", head.c_str());
            d.message += " (module " + std::to_string(k + 1) + " of " + std::to_string(n) + ", " + modules_[k]->name + "); no module of process " + std::to_string(pid_) +
                         " was applied";
            return finish(std::move(d));
        }

        // The WP-22 revert of this reload's batch in every module applied before k, newest first.
        CML_LOG(Trace, "reload: %s: rolling back the %zu module(s) this reload applied before it", head.c_str(), applied.size());
        std::vector<std::string> failed;       // "name: why"
        std::vector<std::string> unconfirmed;  // "name: warnings"
        std::vector<std::size_t> kept;         // modules whose batch stays recorded after a failed roll-back
        std::size_t restored = 0, repointed = 0;
        for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
            const std::size_t i = *it;
            ModuleState& m = *modules_[i];
            (void)runs_[i]->take_applied_events();  // never announced
            RevertOptions ro;
            ro.timeout_ms = ctx_.options != nullptr ? ctx_.options->hooks_timeout_ms : 30000;
            const std::int64_t left = std::chrono::duration_cast<std::chrono::milliseconds>(ctx_.deadline - Clock::now()).count();
            ro.deadline = Clock::now() + std::chrono::milliseconds(std::max<std::int64_t>(left, kRollbackBudgetMs));
            ro.only_reload = ctx_.reload_id;
            ro.events = nullptr;  // nothing of it was announced
            Result<RevertReport> back = revert_module(link_, m, RevertScope::Last, ro);
            if (!back) {
                bool batch_kept = false;
                {
                    std::lock_guard<std::mutex> lock(m.mutex);
                    batch_kept = !m.batches.empty() && m.batches.back().reload_id == ctx_.reload_id;
                }
                if (batch_kept) {
                    kept.push_back(i);
                }
                failed.push_back(m.name + ": " + back.error().format());
                results_[i] = Diag::make(ar::kModulesRollbackFailed, "applied, and its roll-back failed: " + back.error().format(), "Send `revert` (scope last), or restart.");
                CML_LOG(Trace, "reload: the roll-back of %s%s failed: %s", m.name.c_str(), where(i).c_str(), back.error().format().c_str());
                continue;
            }
            for (const Diag& w : back->warnings) {
                emit(w, "warning", "rollback");
            }
            std::string not_confirmed;
            for (const Diag& w : back->warnings) {
                if (w.code == revert::diag::kRevertUnconfirmed || w.code == revert::diag::kUncommitFailed) {
                    not_confirmed += (not_confirmed.empty() ? "" : "; ") + w.format();
                }
            }
            if (!not_confirmed.empty()) {
                unconfirmed.push_back(m.name + ": " + not_confirmed);
            }
            restored += back->hooks_restored;
            repointed += back->hooks_repointed;
            results_[i] = Diag::make(not_confirmed.empty() ? ar::kModulesRolledBack : ar::kModulesRollbackUnconfirmed,
                                     "applied, then rolled back because " + modules_[k]->name + " failed (" + d.id() + ")", "Fix what the failure names, then reload.");
            CML_LOG(Trace, "reload: %s%s rolled back (%zu function(s) restored, %zu re-pointed)%s", m.name.c_str(), where(i).c_str(), back->hooks_restored,
                    back->hooks_repointed, not_confirmed.empty() ? "" : " but not confirmed");
        }
        const Diag why = Diag::make(ar::kModulesRolledBack, "not applied: module " + modules_[k]->name + where(k) + " failed (" + d.id() + ")",
                                    "Fix what the failure names, then reload.");
        not_applied_after(k, why);
        const std::string count = std::to_string(applied.size()) + " module(s) of this reload applied before it (" + names_of(modules_, applied) + ")";
        if (!failed.empty()) {
            std::string list;
            for (const std::string& f : failed) {
                list += (list.empty() ? "" : "; ") + f;
            }
            const Diag e = Diag::make(ar::kModulesRollbackFailed,
                                      head + " (" + d.format() + ") and rolling back the " + count + " failed for " + std::to_string(failed.size()) + " of them (" + list +
                                          "): the process runs a part of the edit" +
                                          (kept.empty() ? std::string("; the module's patch list was dropped")
                                                        : "; " + names_of(modules_, kept) + " stay(s) recorded as this reload's batch"),
                                      kept.empty() ? "Restart the application, or enable the module again and reload."
                                                   : "Send `revert` (scope last) to try the roll-back again, or restart the application.");
            emit(e, "error", "rollback");
            return finish(e);
        }
        if (!unconfirmed.empty()) {
            std::string list;
            for (const std::string& u : unconfirmed) {
                list += (list.empty() ? "" : "; ") + u;
            }
            const Diag e = Diag::make(ar::kModulesRollbackUnconfirmed,
                                      head + " (" + d.format() + ") and the roll-back of the " + count + " was sent but not confirmed (" + list +
                                          "): the broker's state follows the roll-back, and the process may run a part of the edit until the agent applies it",
                                      "In synchronised mode let the application reach SyncPoint(); if the application's hooks of the rolled-back patches keep running, restart the application.");
            emit(e, "error", "rollback");
            return finish(e);
        }
        emit(Diag::make(ar::kModulesRolledBack,
                        head + ", so the " + count + " were rolled back (" + std::to_string(restored) + " function(s) back to the original code, " +
                            std::to_string(repointed) + " re-pointed at an older patch)" + (after != 0 ? "; " + std::to_string(after) + " after it were not applied" : std::string()) +
                            ": the process runs what it ran before the reload",
                        "Fix what the failure names, then reload: the edit is applied to every module then."),
             "warning", "rollback");
        d.message += " (module " + std::to_string(k + 1) + " of " + std::to_string(n) + ", " + modules_[k]->name + "); the " + count + " were rolled back";
        return finish(std::move(d));
    }

    const PatchPipeline& pipeline_;
    AgentLink& link_;
    const std::vector<ModuleState*>& modules_;
    const PipelineContext& ctx_;
    const std::uint32_t pid_;
    std::vector<std::unique_ptr<ModuleReload>> runs_;
    std::vector<std::optional<Result<PatchReport>>> results_;
};

}  // namespace

ProcessReload reload_process(const PatchPipeline& pipeline, AgentLink& link, const std::vector<ModuleState*>& modules, const PipelineContext& context) {
    if (modules.size() == 1) {
        // One module: patch_module() as before (WP-47 within the module).
        ProcessReload out;
        Result<PatchReport> r = pipeline.patch_module(link, *modules.front(), context);
        if (!r) {
            out.error = r.error();
        }
        out.modules.push_back(std::move(r));
        return out;
    }
    ProcessRun run(pipeline, link, modules, context);
    return run.run();
}

}  // namespace cml::core
