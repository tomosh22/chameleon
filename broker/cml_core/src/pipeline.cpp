#include "cml/core/pipeline.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <unordered_set>

#include "cml/coff/anchors.h"
#include "cml/coff/binding.h"
#include "cml/coff/writer.h"
#include "cml/core/atomic_reload_diag_ids.h"
#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_plan.h"
#include "cml/core/module_audit.h"
#include "cml/core/header_diag_ids.h"
#include "cml/core/patch_image.h"
#include "cml/core/pipeline_diag_ids.h"
#include "cml/core/revert.h"
#include "cml/core/revert_diag_ids.h"
#include "cml/core/static_lib_diag_ids.h"
#include "cml/toolchain/rewrite.h"
#include "cml/util/hash.h"
#include "cml/util/format.h"
#include "cml/util/log.h"
#include "cml/util/path.h"

namespace cml::core {

namespace fs = std::filesystem;
namespace pd = pipeline::diag;
namespace ar = atomic_reload::diag;
using Clock = std::chrono::steady_clock;

void StageTimes::add(const StageTimes& o) {
    sweep += o.sweep;
    compile += o.compile;
    plan += o.plan;
    anchor += o.anchor;
    link += o.link;
    load += o.load;
    fixups += o.fixups;
    init += o.init;
    hooks += o.hooks;
    total += o.total;
}

std::string timing_line(const std::string& fixture, const StageTimes& t) {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "RELOAD-TIMING fixture=%s sweep_ms=%.1f compile_ms=%.1f plan_ms=%.1f anchor_ms=%.1f link_ms=%.1f load_ms=%.1f fixups_ms=%.1f "
                  "init_ms=%.1f hooks_ms=%.1f total_ms=%.1f",
                  fixture.c_str(), t.sweep, t.compile, t.plan, t.anchor, t.link, t.load, t.fixups, t.init, t.hooks, t.total);
    return buf;
}

ipc::json::Value to_json(const StageTimes& t) {
    using ipc::json::Value;
    Value v = Value::object();
    v.set("sweep", Value::number(t.sweep));
    v.set("compile", Value::number(t.compile));
    v.set("plan", Value::number(t.plan));
    v.set("anchor", Value::number(t.anchor));
    v.set("link", Value::number(t.link));
    v.set("load", Value::number(t.load));
    v.set("fixups", Value::number(t.fixups));
    v.set("init", Value::number(t.init));
    v.set("hooks", Value::number(t.hooks));
    v.set("total", Value::number(t.total));
    return v;
}

ipc::json::Value to_json(const PatchReport& r) {
    using ipc::json::Value;
    const auto n = [](std::size_t v) { return Value::integer(static_cast<std::int64_t>(v)); };
    Value v = Value::object();
    v.set("pid", Value::integer(static_cast<std::int64_t>(r.pid)));
    v.set("module", Value::string(r.module));
    v.set("generation", Value::integer(static_cast<std::int64_t>(r.generation)));
    v.set("patched", Value::boolean(r.patched));
    v.set("patches", n(r.patches));
    Value files = Value::array();
    for (const std::string& f : r.changed_files) {
        files.push(Value::string(f));
    }
    v.set("changed_files", std::move(files));
    v.set("compilands", n(r.compilands));
    v.set("compiled", n(r.compiled));
    v.set("compile_reused", n(r.compile_reused));
    v.set("hooks_planned", n(r.hooks_planned));
    v.set("hooks_installed", n(r.hooks_installed));
    v.set("hooks_dropped", n(r.hooks_dropped));
    v.set("no_functional_change", Value::boolean(r.no_functional_change));
    v.set("data_only_change", Value::boolean(r.data_only_change));
    Value kept = Value::array();
    for (const std::string& k : r.hooks_kept) {
        kept.push(Value::string(k));
    }
    if (r.hooks_kept_more != 0) {
        kept.push(Value::string("... and " + std::to_string(r.hooks_kept_more) + " more"));
    }
    v.set("hooks_kept", std::move(kept));
    v.set("new_functions", n(r.new_functions));
    v.set("new_globals", n(r.new_globals));
    v.set("thunks", n(r.thunks));
    v.set("fixups", n(r.fixups));
    v.set("pre_applied", n(r.pre_applied));
    v.set("imgrel_sites", n(r.imgrel_sites));
    v.set("imgrel_verified", n(r.imgrel_verified));
    v.set("initializers", n(r.initializers));
    v.set("initializers_run", n(r.initializers_run));
    v.set("load_attempts", n(r.load_attempts));
    v.set("objects_held_peak", n(r.objects_held_peak));  // WP-47
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(r.base));
    v.set("base", Value::string(buf));
    v.set("image_size", Value::integer(static_cast<std::int64_t>(r.image_size)));
    v.set("timing_ms", to_json(r.times));
    Value warnings = Value::array();
    for (const Diag& w : r.warnings) {
        warnings.push(Value::string(w.format()));
    }
    v.set("warnings", std::move(warnings));
    return v;
}

std::string CompileCache::key_of(const toolchain::CompileRecord& record) {
    // Everything that decides what the compiler is asked to do. Fields are separated by a byte no path or argument holds.
    std::string text;
    text += record.cwd;
    text.push_back('\0');
    text += record.compiler;
    text.push_back('\0');
    for (const std::string& a : record.args) {
        text += a;
        text.push_back('\0');
    }
    text.push_back('\1');
    text += record.source;
    text.push_back('\0');
    text += record.recorded_pdb;
    return text;
}

std::optional<CompileCache::Entry> CompileCache::find(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void CompileCache::put(const std::string& key, Entry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[key] = std::move(entry);
}

namespace {

// WP-35: the first compiler error's position, from the parsed ToolDiagnostic. The file name is the compiler's own spelling, so it
// is made absolute against the directory the compiler ran in (outcome.cwd) and normalised. Nothing is set when the compiler named no
// file or no line (for example "cl : Command line error D8003") or when no absolute path can be formed (the clients then parse the
// message text, as they do for a broker without the capability).
void locate(Diag& d, const toolchain::ToolDiagnostic& td, const std::string& cwd) {
    if (td.file.empty() || td.line <= 0) {
        return;
    }
    std::string file = toolchain::resolve_tool_path(td.file, cwd);
    if (file.empty()) {
        return;
    }
    d.file = std::move(file);
    d.line = static_cast<std::uint32_t>(td.line);
    d.column = td.column > 0 ? static_cast<std::uint32_t>(td.column) : 0;
}

double ms_between(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

class Lap {
public:
    Lap() : t_(Clock::now()) {}
    // Milliseconds since construction or the previous lap().
    double lap() {
        const Clock::time_point now = Clock::now();
        const double ms = ms_between(t_, now);
        t_ = now;
        return ms;
    }

private:
    Clock::time_point t_;
};

// The id of a refusal by the agent: its own diagnostic number when it is a valid one (CML42xx, CML43xx, ...),
// otherwise CML5307/CML5308.
Diag agent_refusal(const char* what, std::uint32_t status, const std::string& message, std::uint32_t fallback_id) {
    Diag probe;
    probe.code = status;
    const std::uint32_t id = probe.valid() ? status : fallback_id;
    return Diag::make(id, std::string("the agent refused ") + what + ": " + (message.empty() ? "status " + std::to_string(status) : message),
                      "See the agent's diagnostic above; the patch was not applied.");
}

struct Unit {
    std::uint32_t compiland = 0;
    std::string object_name;  // as recorded in the PDB
    std::string source;
    toolchain::CompileRecord record;
    toolchain::CompileOutcome outcome;
    std::string key;
    std::optional<Diag> cached_error;    // ... and it was a failure
};

class Run {
public:
    Run(const PipelineEnv& env, AgentLink& link, ModuleState& module, const PipelineContext& ctx)
        : env_(env), link_(link), module_(module), ctx_(ctx), options_(*ctx.options) {}

    ~Run() { release_pdb_now(); }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

    // patch_module(): both phases at once.
    Result<PatchReport> execute() {
        if (Result<void> p = prepare_phase(); !p) {
            return p.error();
        }
        return apply_phase();
    }

    // ---- WP-54 (ADR-0045): the two phases of the run, so that the reload service prepares every module of a process first ----
    // prepare_phase: sweep, compile, and prepare every compiland (WP-47: plan, proofs, link, fix-ups; nothing changes in the process
    // but the module's reservation). An error ends the run: a refused compiland has given the address space back and nothing is
    // re-baselined. After a success apply_phase() or abandon() follows.
    Result<void> prepare_phase() {
        const Clock::time_point t0 = Clock::now();
        Result<void> r = prepare_steps();
        phase_ms_ += ms_between(t0, Clock::now());
        if (!r || state_ == State::Done) {
            release_pdb_now();  // the run of this module is over (WP-54 review: not at the end of the whole process)
        }
        return r;
    }
    Result<PatchReport> apply_phase() {
        const Clock::time_point t0 = Clock::now();
        Result<PatchReport> r = state_ == State::Done ? Result<PatchReport>(report_) : state_ == State::Prepared ? apply_steps() : Result<PatchReport>(not_prepared());
        state_ = State::Finished;
        release_pdb_now();
        phase_ms_ += ms_between(t0, Clock::now());
        if (r) {
            r->times.total = phase_ms_;
        }
        return r;
    }
    // Another module of the process was refused or failed (process_reload.cpp): nothing of this module is applied. Its staged
    // compilands give their address space back and none of its files is re-baselined (as after a refusal of its own).
    Diag abandon(Diag why) {
        if (state_ == State::Prepared) {
            CML_LOG(Trace, "reload: %s is not applied: %s", module_.name.c_str(), why.format().c_str());
            state_ = State::Finished;
            release_ranges(0);
            (void)finish(why);
            release_pdb_now();
        }
        return why;
    }
    bool prepared() const noexcept { return state_ == State::Prepared; }
    bool may_be_live() const noexcept { return maybe_live_; }
    void hold_events() noexcept { hold_events_ = true; }
    std::vector<Event> take_applied_events() { return std::move(held_events_); }
    ModuleState& module() const noexcept { return module_; }
    const std::string& failed_stage() const noexcept { return failed_stage_; }

private:
    // ADR-0017: with the option set the broker's mapping of the PDB ends when the run of this module does, whatever its outcome.
    void release_pdb_now() {
        pdb_ = nullptr;
        pdb_hold_.reset();  // the run's own reference keeps nothing mapped either
        if (release_pdb_ && module_.index) {
            module_.index->release_pdb();
        }
        release_pdb_ = false;
    }
    Diag not_prepared() const {
        return Diag::make(pd::kStageFailed, "the reload of " + module_.name + " was not prepared, or applied already", "This is a bug in the broker; see the broker log.");
    }
    // ---- helpers -----------------------------------------------------------------------------------------
    std::int64_t remaining_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(ctx_.deadline - Clock::now()).count();
    }
    std::uint32_t clamp_timeout(std::uint32_t wanted) const {
        const std::int64_t left = remaining_ms();
        if (left <= 0) {
            return 1;
        }
        return static_cast<std::uint32_t>(std::min<std::int64_t>(left, wanted));
    }
    Result<void> check_time(const char* stage) {
        if (remaining_ms() <= 0) {
            return fail(stage, Diag::make(pd::kReloadTimeout, std::string("the reload ran out of time before the stage '") + stage + "'",
                                          "Raise `timeout_ms` of the reload command, or reduce the number of changed files."));
        }
        return Result<void>();
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
    Diag fail(const char* stage, Diag d) {
        emit(d, "error", stage);
        failed_stage_ = stage;  // WP-54: the stage of the run's last failure
        return d;
    }
    // WP-28: an armed failure-injection stage of the broker (cml/ipc/fault_stages.h) turns into its diagnostic here.
    std::optional<Diag> injected(ipc::fault::Stage stage) {
        return ctx_.faults != nullptr ? ctx_.faults->hit(stage, pid_) : std::nullopt;
    }
    void warn(PatchReport& report, const char* stage, Diag d) {
        emit(d, "warning", stage);
        report.warnings.push_back(std::move(d));
    }

    Result<ipc::Message> ask(ipc::Message request, std::uint32_t timeout_ms, const char* stage) {
        Result<ipc::Message> answer = link_.request(std::move(request), clamp_timeout(timeout_ms));
        if (!answer) {
            return fail(stage, answer.error());
        }
        return answer;
    }

    // WP-17: tells the agent that the compile stage started, succeeded or failed, so that the application's
    // CML_COMPILE_*_HOOK functions run. Best effort: an agent that is gone, or one without hooks, changes nothing.
    void notify_compile(ipc::CompilePhase phase, const Diag* failure = nullptr) {
        ipc::CompileEvent event;
        event.phase = phase;
        event.module = module_.name;
        if (failure != nullptr) {
            event.diag = failure->code;
            std::string text = failure->message;
            std::replace(text.begin(), text.end(), '\n', ' ');
            std::replace(text.begin(), text.end(), '\r', ' ');
            event.text = ipc::sanitize_utf8(text, 400);
        }
        const Result<void> sent = link_.send(ipc::Message(std::move(event)));
        if (!sent) {
            CML_LOG(Debug, "reload: compile notification not delivered: %s", sent.error().format().c_str());
        }
    }

    // ---- the run -----------------------------------------------------------------------------------------
    // Called from the compile workers concurrently: the counter is atomic, CML_LOG is thread-safe and EventBus::publish
    // takes its own lock; everything else it reads (pid_, module_.name) is not written during the compile stage.
    void publish_progress(const char* stage, std::size_t done, std::size_t total) {
        CML_LOG(Info, "reload: %s %zu/%zu (%s)", stage, done, total, module_.name.c_str());
        if (ctx_.events != nullptr) {
            ctx_.events->publish(make_progress_event(stage, pid_, module_.name, done, total));
        }
    }

    std::vector<AppliedPatch> applied_;  // WP-22: the patches this run made live, oldest first
    // WP-47: the compilands of this run after the prepare phase, in unit order (stable addresses: the overlay and the narrowing
    // inputs point into them), and the `patch_applied` events of the applied ones, published when the reload is done.
    struct Staged;
    std::vector<std::unique_ptr<Staged>> staged_;
    std::vector<Event> applied_events_;
    bool hold_back_ = false;  // WP-47: the agent went away while applying: no file of the reload is re-baselined, nothing is announced
    std::optional<std::size_t> reprepare_failed_;  // WP-47: the compiland whose preparation after a relink failed
    const pdb::PdbIndex* pdb_ = nullptr;
    std::string workdir_;
    std::vector<Unit>* units_ = nullptr;
    // WP-54: what the run keeps between its two phases (locals of one function before).
    enum class State { Failed, Done, Prepared, Finished };
    State state_ = State::Failed;     // Done: nothing to compile, report_ is final; Prepared: apply_phase() or abandon() is next
    PatchReport report_;
    ModuleIndex::Sweep swept_;
    std::vector<Unit> units_store_;
    std::shared_ptr<const pdb::PdbIndex> pdb_hold_;
    bool release_pdb_ = false;        // ADR-0017 (~Run)
    std::uint64_t generation_before_ = 0;
    double phase_ms_ = 0;             // times.total: the two phases (not the time a run waits for the other modules of its process)
    bool maybe_live_ = false;         // a failed apply: the failed compiland may still become live (CML5309 / CML4656)
    bool hold_events_ = false;        // `patch_applied` goes to held_events_, not to the bus
    std::vector<Event> held_events_;
    std::string failed_stage_;        // the stage of the last failure ("sweep", "compile", "plan", "link", ...)

    Result<void> prepare_steps() {
        PatchReport& report = report_;
        const LinkSnapshot snap = link_.snapshot();
        pid_ = snap.pid;
        report.pid = snap.pid;
        report.module = module_.name;
        snapshot_ = snap;

        // The module must still be loaded where the state was made.
        bool loaded = false;
        for (const LoadedModule& m : snap.modules) {
            if (m.base == module_.base && path::same_path(m.path, module_.path)) {
                loaded = true;
            }
        }
        if (!loaded) {
            return fail("sweep", Diag::make(pd::kNoTarget, "the module '" + module_.path + "' is no longer loaded in process " + std::to_string(snap.pid),
                                            "Enable it again after it was reloaded."));
        }

        // ---- 1. sweep ----------------------------------------------------------------------------------------
        Lap lap;
        if (!module_.index) {
            return fail("sweep", Diag::make(pd::kIndexUnavailable, "the module '" + module_.name + "' has no PDB index", "Enable the module first."));
        }
        if (!module_.index->wait(static_cast<std::uint32_t>(std::max<std::int64_t>(0, remaining_ms())))) {
            return fail("sweep", Diag::make(pd::kIndexUnavailable, "the PDB index of '" + module_.name + "' is still loading (" + module_.index->status().phase + ")",
                                            "Wait for the index (control command `wait`) or raise `timeout_ms`."));
        }
        {
            const IndexStatus st = module_.index->status();
            if (st.state == IndexState::Failed) {
                const Diag why = st.error ? *st.error : Diag::make(pd::kPdbOpen, "the PDB of '" + module_.name + "' could not be opened", "See the broker log.");
                return fail("sweep", why);
            }
        }
        if (const std::optional<Diag> f = injected(ipc::fault::Stage::PdbLoad)) {
            module_.index->release_pdb();  // the mapping is gone: the next reload maps the PDB again
            return fail("sweep", *f);
        }
        Result<std::shared_ptr<const pdb::PdbIndex>> pdb_result = module_.index->acquire_pdb();
        if (!pdb_result) {
            return fail("sweep", pdb_result.error());
        }
        pdb_hold_ = *pdb_result;
        const std::shared_ptr<const pdb::PdbIndex>& pdb = pdb_hold_;
        // ADR-0017: with the option set the broker's mapping of the PDB ends with the reload, whatever its outcome (~Run).
        release_pdb_ = options_.release_pdb_after_reload;
        if (const std::optional<Diag> f = injected(ipc::fault::Stage::ChangeDetection)) {
            return fail("sweep", *f);
        }
        Result<ModuleIndex::Sweep> sweep_result = module_.index->sweep();
        if (!sweep_result) {
            return fail("sweep", sweep_result.error());
        }
        swept_ = std::move(*sweep_result);
        ModuleIndex::Sweep* const swept = &swept_;
        report.changed_files = swept->changed;
        for (const std::string& f : swept->unmapped) {
            warn(report, "sweep",
                 Diag::make(pd::kChangedFileUnmapped, "the changed file '" + f + "' is not part of any compiland of " + module_.name,
                            "No compiland lists the file and no scanned #include chain of the module reaches it (ADR-0013). Edit a source file that includes the header, or rebuild the project if the include path changed."));
        }
        for (const ModuleIndex::Sweep::NotOwned& n : swept->not_owned) {
            // WP-26b: the file is some compiland's source, but no compiland that can be reloaded owns it: say so instead of a silent `patched: 0`.
            warn(report, "sweep",
                 Diag::make(staticlib::diag::kChangedFileNotOwned,
                            "the changed file '" + n.file + "' is only owned by compilands of " + module_.name + " that cannot be reloaded: " + n.detail,
                            "Rebuild the project (and the library it belongs to) with /Z7 so its PDB records the compile command, then restart; or rebuild and hot-restart now."));
        }
        if (!swept->not_owned.empty()) {
            // Reported once: accepted here, before any path that ends the reload (a refusal, a failure, nothing to compile), so the
            // next reload does not repeat CML5921 for the same edit.
            std::map<std::string, FileStamp> reported;
            for (const ModuleIndex::Sweep::NotOwned& n : swept->not_owned) {
                const auto st = swept->stamps.find(n.file);
                if (st != swept->stamps.end()) {
                    reported[n.file] = st->second;
                }
            }
            module_.index->accept(reported);
        }
        for (const std::string& f : swept->missing) {
            warn(report, "sweep", Diag::make(pd::kBaselineFile, "the tracked file '" + f + "' is gone", "Restore it or ignore this warning."));
        }
        report.times.sweep = lap.lap();
        CML_LOG(Info, "reload: sweep of %s: %zu files checked, %zu hashed, %zu changed, %zu compilands", module_.name.c_str(), swept->checked, swept->hashed,
                swept->changed.size(), swept->compilands.size());
        // ---- WP-15: refuse what cannot be reloaded, and ask before a very large reload -------------------------------
        if (!swept->pch_headers.empty()) {
            std::string names;
            for (const std::string& f : swept->pch_headers) {
                names += (names.empty() ? "'" : ", '") + f + "'";
            }
            return fail("sweep", Diag::make(header::diag::kPchHeaderChanged,
                                            "the changed " + std::string(swept->pch_headers.size() == 1 ? "file " : "files ") + names +
                                                " belong to the precompiled header of " + module_.name + "; a precompiled header cannot be hot-reloaded",
                                            "Revert the change, or rebuild and restart the application. Edit the translation units that use the precompiled header instead."));
        }
        if (!swept->pch_unverified.empty()) {
            std::string names;
            for (const std::uint32_t ci : swept->pch_unverified) {
                names += (names.empty() ? "'" : ", '") + std::string(pdb->compiland(ci).object_name) + "'";
            }
            return fail("sweep", Diag::make(header::diag::kPchUnknown,
                                            "cannot tell which files make up the precompiled header used by " + names +
                                                "; a recompile could use a stale .pch",
                                            "Rebuild the project and hot-restart, or build with /Yu<header> naming the precompiled header."));
        }
        if (options_.max_affected_compilands != 0 && swept->compilands.size() > options_.max_affected_compilands && !ctx_.confirm) {
            return fail("sweep", Diag::make(header::diag::kTooManyCompilands,
                                            "the change affects " + std::to_string(swept->compilands.size()) + " compilands of " + module_.name +
                                                " (" + std::to_string(swept->changed.size()) + " changed files), more than the limit of " +
                                                std::to_string(options_.max_affected_compilands),
                                            "Repeat the reload with \"confirm\": true to recompile them all, or raise the option max_affected_compilands."));
        }
        if (swept->compilands.empty()) {
            // Nothing to compile. Files that map to no compiland are accepted so they are not reported again.
            module_.index->accept(swept->stamps);
            report.patched = false;
            state_ = State::Done;
            return Result<void>();
        }
        report.compilands = swept->compilands.size();

        // ---- 2. compile (parallel) ---------------------------------------------------------------------------
        if (Result<void> t = check_time("compile"); !t) {
            return t.error();
        }
        if (!env_.compile_all) {
            return fail("compile", Diag::make(pd::kStageFailed, "this broker has no compiler driver", "Run the broker on Windows."));
        }
        Result<std::string> workdir = make_work_dir();
        if (!workdir) {
            return workdir.error();
        }
        std::vector<Unit>& units = units_store_;
        std::vector<toolchain::CompileRequest> requests;
        std::vector<std::size_t> fresh;  // index in `units` of requests[k]
        std::optional<Diag> first_error;
        notify_compile(ipc::CompilePhase::Start);
        if (const std::optional<Diag> f = injected(ipc::fault::Stage::Compile)) {
            const Diag d = fail("compile", *f);
            notify_compile(ipc::CompilePhase::Error, &d);
            return d;
        }
        for (const std::uint32_t ci : swept->compilands) {
            Unit u;
            u.compiland = ci;
            u.object_name = std::string(pdb->compiland(ci).object_name);
            Result<pdb::BuildEnv> env = pdb->build_env(ci);
            if (!env) {
                const Diag d = fail("compile", env.error());
                first_error = first_error ? first_error : std::optional<Diag>(d);
                continue;
            }
            Result<toolchain::CompileRecord> record = toolchain::record_from_build_env(*env);
            if (!record) {
                const Diag d = fail("compile", record.error());
                first_error = first_error ? first_error : std::optional<Diag>(d);
                continue;
            }
            u.record = *record;
            u.source = record->source;
            u.key = CompileCache::key_of(*record);
            if (ctx_.compile_cache) {
                if (std::optional<CompileCache::Entry> hit = ctx_.compile_cache->find(u.key)) {
                    // Another module of this reload (typically the same DLL in another process) ran this command line.
                    if (hit->error) {
                        // The shared compile failed: report it for this module too, without compiling anything else.
                        return fail("compile", *hit->error);
                    }
                    u.outcome = hit->outcome;
                    for (const Diag& w : u.outcome.warnings) {
                        warn(report, "compile", w);  // every module and process hears the compiler's warnings
                    }
                    ++report.compile_reused;
                    CML_LOG(Debug, "compile of %s: reused the result of an identical command line (%s)", u.source.c_str(), u.outcome.object_path.c_str());
                    units.push_back(std::move(u));
                    continue;
                }
            }
            toolchain::CompileRequest cr;
            cr.record = *record;
            cr.output_object = toolchain::patch_object_path(*workdir + "/obj", module_.name, hash64(u.object_name + "|" + std::string(pdb->compiland(ci).library_name)), record->source);
            cr.timeout_ms = static_cast<unsigned>(std::max<std::int64_t>(1000, remaining_ms()));
            cr.cancel = ctx_.cancel;
            if (!record->compiler.empty()) {
                cr.fallback_search_dirs.push_back(std::string(path::parent_directory(std::string_view(record->compiler))));
            }
            requests.push_back(std::move(cr));
            fresh.push_back(units.size());
            units.push_back(std::move(u));
        }
        if (first_error) {
            notify_compile(ipc::CompilePhase::Error, &*first_error);
            return *first_error;
        }
        std::vector<toolchain::CompileOutcome> outcomes(requests.size());
        // One call for all cache misses with the full parallelism and one deadline budget (each request carries the time
        // that is left). WP-15 progress counts every unit of the reload: the ones reused from the compile cache are done
        // at the start, every fresh compile is counted when the driver reports it finished (on_done may run concurrently).
        const std::size_t total = units.size();
        std::atomic<std::size_t> finished{total - requests.size()};
        publish_progress("compile", finished.load(), total);
        std::vector<Result<void>> results;
        if (!requests.empty()) {
            results = env_.compile_all(requests, outcomes, options_.max_parallel_compiles, ctx_.cancel,
                                       [&](std::size_t) { publish_progress("compile", finished.fetch_add(1) + 1, total); });
        }
        // A driver that returns fewer results than requests has not compiled the missing ones: that is a failure, never a success.
        while (results.size() < requests.size()) {
            const std::string& missing = units[fresh[results.size()]].source;
            results.emplace_back(Diag::make(pd::kStageFailed, "the compiler driver returned no result for '" + missing + "'",
                                            "This is a bug in the compiler driver; see the broker log."));
        }
        report.compiled += requests.size();
        for (std::size_t k = 0; k < requests.size(); ++k) {
            Unit& u = units[fresh[k]];
            u.outcome = outcomes[k];
            if (!results[k]) {
                Diag d = results[k].error();
                // Name the first compiler error so the diagnostic is useful on its own; the full output is in the log.
                for (const toolchain::ToolDiagnostic& td : outcomes[k].diagnostics) {
                    if (td.severity == toolchain::Severity::Error || td.severity == toolchain::Severity::FatalError) {
                        d.message += " First error: " + td.format();
                        locate(d, td, outcomes[k].cwd);  // WP-35: from the parsed diagnostic, not from the text
                        break;
                    }
                }
                CML_LOG(Error, "compile of %s failed:\n%s", u.source.c_str(), outcomes[k].output.c_str());
                u.cached_error = d;
            } else {
                for (const Diag& w : outcomes[k].warnings) {
                    warn(report, "compile", w);
                }
                CML_LOG(Debug, "compile of %s: %.0f ms, %s", u.source.c_str(), outcomes[k].seconds * 1000.0, outcomes[k].command_line.c_str());
            }
            if (ctx_.compile_cache) {
                ctx_.compile_cache->put(u.key, CompileCache::Entry{u.outcome, u.cached_error});
            }
        }
        for (std::size_t k = 0; k < requests.size(); ++k) {
            if (units[fresh[k]].cached_error) {
                const Diag shown = fail("compile", *units[fresh[k]].cached_error);
                first_error = first_error ? first_error : std::optional<Diag>(shown);
            }
        }
        report.times.compile = lap.lap();
        if (first_error) {
            notify_compile(ipc::CompilePhase::Error, &*first_error);
            return *first_error;
        }
        notify_compile(ipc::CompilePhase::Success);

        // ---- 3. prepare every compiland, 4. apply them (WP-47, ADR-0044) -------------------------------------------
        // Plan, narrowing with its code proofs, anchor, link and fix-ups run for EVERY compiland before any is applied, so a
        // refusal of one leaves the process as it was. A compiland is planned against the compilands staged before it as if
        // they were applied (their new functions and globals, hooks and code records: the overlay below). The apply (load,
        // fix-ups, initialisers, hooks) then runs per compiland; a failure there rolls back what this reload had applied (the
        // WP-22 revert of its batch).
        generation_before_ = [&] {
            std::lock_guard<std::mutex> lock(module_.mutex);
            return module_.generation;
        }();
        pdb_ = pdb.get();
        workdir_ = *workdir;
        units_ = &units;
        std::optional<Diag> failure;
        for (std::size_t i = 0; i < units.size() && !failure; ++i) {
            staged_.push_back(std::make_unique<Staged>());
            staged_.back()->unit = i;
            if (Result<void> prepared = prepare(*staged_.back(), report); !prepared) {
                failure = refused(prepared.error());
            }
        }
        if (failure) {
            release_ranges(0);
            Result<PatchReport> ended = finish(failure);
            return ended.error();
        }
        std::size_t with_effect = 0;
        for (const std::unique_ptr<Staged>& s : staged_) {
            with_effect += s->has_effect ? 1 : 0;
        }
        CML_LOG(Trace, "reload: all %zu compilands of %s are planned, linked and verified (%zu to apply)", staged_.size(), module_.name.c_str(), with_effect);
        state_ = State::Prepared;
        return Result<void>();
    }

    // The apply loop over the staged compilands (WP-47), then finish().
    Result<PatchReport> apply_steps() {
        state_ = State::Finished;
        PatchReport& report = report_;
        std::optional<Diag> failure;
        CML_LOG(Trace, "reload: applying the compilands of %s", module_.name.c_str());
        for (std::size_t i = 0; i < staged_.size() && !failure; ++i) {
            if (!staged_[i]->has_effect) {
                continue;
            }
            if (Result<void> applied = apply(i, report); !applied) {
                release_ranges(i);
                failure = after_apply_failure(applied.error(), i, generation_before_);
            }
        }
        if (!failure) {
            add_plan_counts(report);
        }
        return finish(failure);
    }

    // The end of the run, whatever its outcome: re-baselining, the batch, `patch_applied`, the CML3450 / CML3451 notes.
    Result<PatchReport> finish(const std::optional<Diag>& failure) {
        PatchReport& report = report_;
        const std::vector<Unit>& units = units_store_;
        const ModuleIndex::Sweep* const swept = &swept_;
        const std::uint64_t generation_before = generation_before_;
        // Re-baseline the files whose every compiland is live (applied, or unchanged against the running code); the others are
        // processed again next time. A compiland that loaded nothing counts only when its comparison did not rely on a compiland
        // staged before it that was then not applied.
        std::map<std::uint32_t, bool> unit_ok;
        bool before_applied = true;  // every compiland with effect so far was applied
        for (const std::unique_ptr<Staged>& s : staged_) {
            // A compiland that loaded nothing counts only for a reload that succeeded (WP-47 review): after a failure or a lost agent
            // its comparison may have been against what is not live, so its files are compiled again next time (safe).
            const bool ok = s->prepared && (s->has_effect ? (s->applied && !hold_back_) : (!failure && !hold_back_ && (!s->after_pending || before_applied)));
            unit_ok[units[s->unit].compiland] = ok;
            before_applied = before_applied && (!s->has_effect || (s->applied && !hold_back_));
        }
        std::map<std::string, FileStamp> accepted;
        for (const auto& entry : swept->file_compilands) {
            bool all = true;
            for (const std::uint32_t ci : entry.second) {
                const auto it = unit_ok.find(ci);
                all = all && it != unit_ok.end() && it->second;
            }
            if (all) {
                const auto st = swept->stamps.find(entry.first);
                if (st != swept->stamps.end()) {
                    accepted[entry.first] = st->second;
                }
            }
        }
        for (const std::string& f : swept->unmapped) {
            const auto st = swept->stamps.find(f);
            if (st != swept->stamps.end()) {
                accepted[f] = st->second;
            }
        }
        // WP-22: the patches of this reload are one batch a `revert` can undo, together with the baseline they replaced.
        PatchBatch batch;
        if (!applied_.empty()) {
            std::vector<std::string> accepted_files;
            for (const auto& entry : accepted) {
                accepted_files.push_back(entry.first);
            }
            batch.baseline_before = module_.index->baseline_stamps(accepted_files);
            batch.patches = std::move(applied_);
            batch.generation_before = generation_before;
            batch.reload_id = ctx_.reload_id;
        }
        module_.index->accept(accepted);
        if (!batch.patches.empty()) {
            std::lock_guard<std::mutex> lock(module_.mutex);
            module_.batches.push_back(std::move(batch));
        }
        // WP-47: `patch_applied` is published once the reload's compilands are live, never for one that a later failure rolled back.
        // WP-54: for a module of a process with several, once every module of the process is (the caller takes them).
        for (Event& e : applied_events_) {
            if (hold_events_) {
                held_events_.push_back(std::move(e));
            } else if (ctx_.events != nullptr) {
                ctx_.events->publish(std::move(e));
            }
        }
        applied_events_.clear();
        if (failure) {
            return *failure;
        }
        report.patched = report.generation != 0;
        // WP-25: every compiled unit turned out identical to the running code and nothing else came with it: say so.
        if (!units.empty() && report.units_unchanged == units.size()) {
            if (report.data_changes.empty()) {
                report.no_functional_change = true;
                warn(report, "plan",
                     Diag::make(pd::kNoFunctionalChange,
                                "the edit compiled to identical code (" + std::to_string(report.hooks_dropped) + " of " + std::to_string(report.hooks_planned) +
                                    " planned hooks are identical to the running code, nothing new): no patch was loaded for " + module_.name,
                                "Nothing to do. Change code or data that reaches the generated code, or set-option narrow_hooks false to hook everything."));
            } else {
                // The code is identical, but the initial value of data the running image owns changed: nothing can apply that.
                report.data_only_change = true;
                std::string names;
                for (const std::string& n : report.data_changes) {
                    names += (names.empty() ? "" : ", ") + n;
                }
                warn(report, "plan",
                     Diag::make(pd::kBoundDataChangeOnly,
                                "the edit only changes the initial value of data that lives in the running image (" + names + "): the code is identical, so no patch was loaded for " +
                                    module_.name + " and the running program keeps its current values (initialisers of existing globals do not run again)",
                                "Restart the application to pick the new initial value up, or assign the value from code that runs again (a function the patch can hook)."));
            }
        }
        return report;
    }

    Result<std::string> make_work_dir() {
        std::error_code ec;
        // WP-16: per process. WP-45 (F-42.2): per instance of the module in that process, too: the patch DLLs of an instance that
        // was freed (or disabled) stay loaded, so a later instance must never write its g<n> to their paths. The first instance
        // keeps the plain `p<pid>`.
        const std::string dir = ctx_.session_dir + "/" + module_.name + "/p" + std::to_string(pid_) +
                                (module_.instance > 1 ? "-i" + std::to_string(module_.instance) : std::string());
        fs::create_directories(fs::path(dir) / "obj", ec);
        if (ec) {
            return fail("compile", Diag::make(pd::kWorkDir, "cannot create the session directory '" + dir + "': " + ec.message(),
                                              "Check the TEMP directory (free space, permissions)."));
        }
        return dir;
    }

    // ---- WP-47: one compiland, staged ------------------------------------------------------------------------
    // What the prepare phase made of one compiland: everything its apply needs, and what it adds to the module's state once it is
    // applied (`hook_table`, `code`, `symbols`). Until then those are only seen through the overlay, by the compilands planned
    // after it.
    struct PlanCounts {
        std::size_t hooks_planned = 0, hooks_dropped = 0, new_functions = 0, new_globals = 0, thunks = 0, imgrel_sites = 0, imgrel_verified = 0;
        std::vector<std::string> kept;           // hooks_kept notes
        std::vector<std::string> data_changes;   // WP-25 (a compiland that loads nothing)
    };
    struct Staged {  // declared with the members above
        std::size_t unit = 0;            // index in units_
        std::uint64_t patch_no = 0;      // names its files; kept when the compiland is prepared again after a relink
        unsigned link_attempt = 0;       // names its DLLs (patch_<no>_<attempt>); never goes back
        bool has_effect = false;         // false: identical to the running code, nothing to load (WP-25)
        bool after_pending = false;      // planned while an earlier compiland with effect was staged and not yet applied
        bool prepared = false;           // prepare() got through: planned, verified (and linked when it has effect)
        std::unique_ptr<coff::OwnedObject> object;
        bind::BindPlan plan;
        std::vector<bind::HookEntry> hooks;  // narrowed: one per old entry
        std::size_t planned = 0;         // narrowing: distinct old entries planned
        std::size_t dropped = 0;
        PlanCounts counts;
        // link inputs (a relink at another base reuses them)
        std::string patch_dir;
        std::string anchored_path;
        std::string binding_path;
        std::vector<coff::SectionLocator> locators;
        std::vector<coff::BindSymbol> binds;
        // link outputs
        bool linked_ok = false;          // linked, fix-ups made, its contributions computed
        bool range_held = false;         // its slice of the reservation is used while nothing is mapped there (given back on release)
        toolchain::LinkedPatch linked;
        PatchFixups fixups;
        std::vector<std::uint32_t> init_rvas;
        std::vector<AppliedHook> hook_table;  // old entry -> new entry, index-aligned with `hooks`
        std::vector<std::pair<std::uint64_t, std::shared_ptr<const CodeImage>>> code;  // WP-25 records, by patch address
        std::vector<std::pair<std::string, SymbolAddress>> symbols;  // new functions and globals: the registry entries
        std::vector<Diag> layout_warnings;    // new functions / globals without an address (published when applied)
        // apply
        bool mapped = false;             // LoadPatch succeeded (the slice stays used, also after an unload)
        bool applied = false;            // committed: live in the process and in the module's state
        bool live_unconfirmed = false;   // CML5309 / CML4656: may be live, recorded as installed, cannot be rolled back

        // Back to "not prepared"; the identity (unit, patch number, DLL counter) stays.
        void reset() {
            Staged fresh;
            fresh.unit = unit;
            fresh.patch_no = patch_no;
            fresh.link_attempt = link_attempt;
            *this = std::move(fresh);
        }
        // What the overlay shows: staged with effect, linked, not yet in the module's state.
        bool pending() const noexcept { return has_effect && linked_ok && !applied && !live_unconfirmed; }
    };

    // The registry as the compiland being planned sees it: the staged compilands' new symbols (newest first), then the module's.
    class OverlayRegistry final : public SymbolRegistry {
    public:
        explicit OverlayRegistry(const Run& run) : run_(run) {}
        std::optional<SymbolAddress> find(std::string_view name) const override {
            for (auto s = run_.staged_.rbegin(); s != run_.staged_.rend(); ++s) {
                if (!(*s)->pending()) {
                    continue;
                }
                for (auto e = (*s)->symbols.rbegin(); e != (*s)->symbols.rend(); ++e) {
                    if (e->first == name) {
                        return e->second;
                    }
                }
            }
            return run_.module_.registry.find(name);
        }

    private:
        const Run& run_;
    };

    bool staged_is_hooked(std::uint64_t va) const {
        for (const std::unique_ptr<Staged>& s : staged_) {
            if (s->pending()) {
                for (const AppliedHook& h : s->hook_table) {
                    if (h.old_entry == va) {
                        return true;
                    }
                }
            }
        }
        return module_.is_hooked(va);
    }
    // ModuleState::code_running_at over the module's hook table and code records with the staged compilands' on top.
    std::shared_ptr<const CodeImage> staged_code_running_at(std::uint64_t entry) const {
        std::optional<std::uint64_t> target;
        for (auto s = staged_.rbegin(); s != staged_.rend() && !target; ++s) {
            if ((*s)->pending()) {
                for (const AppliedHook& h : (*s)->hook_table) {
                    if (h.old_entry == entry) {
                        target = h.new_entry;  // the last staged hook of an entry wins, as it would once applied
                    }
                }
            }
        }
        std::lock_guard<std::mutex> lock(module_.mutex);
        if (!target) {
            const auto hooked = module_.hooked.find(entry);
            target = hooked != module_.hooked.end() ? hooked->second : entry;
        }
        for (auto s = staged_.rbegin(); s != staged_.rend(); ++s) {
            if ((*s)->pending()) {
                for (const auto& c : (*s)->code) {
                    if (c.first == *target) {
                        return c.second;
                    }
                }
            }
        }
        const auto found = module_.code.find(*target);
        return found != module_.code.end() ? found->second : nullptr;
    }
    bool any_pending() const {
        return std::any_of(staged_.begin(), staged_.end(), [](const std::unique_ptr<Staged>& s) { return s->pending(); });
    }
    NarrowInput narrow_input(const Staged& s, const pe::OwnedImage* image) const {
        NarrowInput ni;
        ni.object = &s.object->object;
        ni.plan = &s.plan;
        ni.old_image = image != nullptr ? &image->image : nullptr;
        ni.load_base = module_.base;
        ni.is_hooked = [this](std::uint64_t va) { return staged_is_hooked(va); };
        ni.prior_code = [this](std::uint64_t va) { return staged_code_running_at(va); };  // WP-25
        return ni;
    }
    std::string unit_name(const Staged& s) const {
        const Unit& u = (*units_)[s.unit];
        return std::string(path::base_name(std::string_view(u.source.empty() ? u.object_name : u.source)));
    }
    // " (compiland 3 of 3, tu_c.cpp)" for a reload of several compilands, else nothing.
    std::string unit_suffix(const Staged& s) const {
        if (units_->size() < 2) {
            return std::string();
        }
        return " (compiland " + std::to_string(s.unit + 1) + " of " + std::to_string(units_->size()) + ", " + unit_name(s) + ")";
    }

    // ---- prepare: plan, narrowing and its proofs, anchor, link, fix-ups -----------------------------------------
    // Nothing here changes the process (the agent is asked for the reservation, once per module, and nothing else).
    Result<void> prepare(Staged& s, PatchReport& report) {
        s.reset();
        if (Result<void> t = check_time("plan"); !t) {
            return t;
        }
        Unit& u = (*units_)[s.unit];
        const pdb::PdbIndex& pdb = *pdb_;
        Lap lap;
        if (s.patch_no == 0) {
            std::lock_guard<std::mutex> lock(module_.mutex);
            s.patch_no = ++module_.patch_counter;
        }
        const std::string tag = "g" + std::to_string(s.patch_no);
        s.after_pending = any_pending();

        // ---- plan --------------------------------------------------------------------------------------------
        // WP-50 (R-47): where the plan stage goes, one `WP50-PLAN` line per unit (the first reload of a session spends more here).
        const Clock::time_point plan_started = Clock::now();
        Result<coff::OwnedObject> object = coff::load_object_file(u.outcome.object_path);
        if (!object) {
            return fail("plan", object.error());
        }
        const Clock::time_point plan_object = Clock::now();
        s.object = std::make_unique<coff::OwnedObject>(std::move(*object));
        report.objects_held_peak = std::max<std::size_t>(report.objects_held_peak, static_cast<std::size_t>(std::count_if(
            staged_.begin(), staged_.end(), [](const std::unique_ptr<Staged>& t) { return t->object != nullptr; })));
        std::shared_ptr<const pe::OwnedImage> image = module_image(report);
        const Clock::time_point plan_image = Clock::now();
        const std::size_t export_parses_before = ctx_.exports ? ctx_.exports->parses() : 0;
        const pdb::Timings pdb_before = pdb.timings();  // WP-50: the PDB's lazily loaded tables, before and after the bind plan
        FileProcessModules process_modules(snapshot_.modules, LoadedModule{module_.path, module_.base, module_.size}, ctx_.exports, image);
        const OverlayRegistry registry(*this);
        bind::PlanInput in;
        in.object = &s.object->object;
        in.object_name = std::string(path::base_name(std::string_view(u.outcome.object_path)));
        in.module_pdb = &pdb;
        in.module_name = module_.name;
        in.module_base = module_.base;
        in.compiland_index = u.compiland;
        in.registry = &registry;
        in.modules = &process_modules;
        in.inline_verdicts = &module_.inline_verdicts;  // WP-55 (R-62)
        in.prove_inlined_owners = true;  // WP-57 (R-59): proven below (prove_inlined_owner_statics)
        const std::uint64_t verdict_kept_before = module_.inline_verdicts.kept();
        const std::uint64_t verdict_reads_before = module_.inline_verdicts.reads();
        if (const std::optional<Diag> f = injected(ipc::fault::Stage::BindPlan)) {
            return fail("plan", *f);
        }
        std::vector<Diag> errors;
        Result<bind::BindPlan> planned = bind::plan_bind(in, &errors);
        for (const Diag& skipped : process_modules.skipped()) {
            warn(report, "plan", skipped);
        }
        if (!planned) {
            return first_failure("plan", errors, planned.error());
        }
        s.plan = std::move(*planned);
        bind::BindPlan& plan = s.plan;
        const Clock::time_point plan_bound = Clock::now();
        // WP-40 (R-48 item 5, ADR-0043 rule (d)): lambdas the type records cannot tell apart were paired by their numbers;
        // their call operators' code decides. Where it shows that lambdas changed places, the unit is planned again with
        // that pairing (once: the second plan is not compared again).
        if (!plan.lambda_sets.empty()) {
            const NarrowInput li = narrow_input(s, image.get());
            LambdaPairing lambdas = pair_lambdas_by_code(li);
            for (const Diag& w : lambdas.warnings) {
                warn(report, "plan", w);
            }
            if (!lambdas.errors.empty()) {
                return first_failure("plan", lambdas.errors, lambdas.errors.front());
            }
            if (!lambdas.closure_pairing.empty()) {
                in.closure_pairing = lambdas.closure_pairing;
                errors.clear();
                Result<bind::BindPlan> again = bind::plan_bind(in, &errors);
                if (!again) {
                    return first_failure("plan", errors, again.error());
                }
                plan = std::move(*again);
            }
        }
        // WP-46 (F-41.2, ADR-0043 ruling D1): lambdas cl names by a hash (its legacy lambda processor) were paired by that name,
        // whose derivation is not documented; their code proves the pairing (one changed lambda allowed), otherwise CML3630.
        if (!plan.hashed_lambdas.empty()) {
            const std::vector<Diag> unproven = prove_hashed_lambdas(narrow_input(s, image.get()));
            if (!unproven.empty()) {
                return first_failure("plan", unproven, unproven.front());
            }
        }
        // WP-40 (second high review, ADR-0043 ruling): a `$S<n>` bitmask bound to the old one only while its function's
        // code is unchanged (the bits follow the order of its dynamically initialised statics); otherwise CML3600.
        if (!plan.bitmask_guards.empty()) {
            const std::vector<Diag> unproven = prove_bitmask_guards(narrow_input(s, image.get()));
            if (!unproven.empty()) {
                return first_failure("plan", unproven, unproven.front());
            }
        }
        // WP-53 (R-61, ADR-0045): a lambda's static bound to a record directly in the procedure that inlined it stands only
        // while that procedure's code is the old code (the record may be the procedure's own static); otherwise CML3207.
        // WP-57 (R-59, ADR-0046): an own static WP-51 found at risk (its procedure inlined closure code) stands only while that
        // procedure's code is the old code as well (the record may be a deleted lambda's static); otherwise CML3207. Both
        // proofs share one verdict per procedure and follow calls to new static functions (the callee step).
        if (!plan.inlined_lambda_statics.empty() || !plan.inlined_owner_statics.empty()) {
            InlinedProcVerdicts verdicts;
            NarrowInput proof = narrow_input(s, image.get());
            proof.old_proc_size = [&pdb](std::uint32_t rva) -> std::optional<std::uint32_t> {
                const Result<std::optional<pdb::SymbolView>> proc_at = pdb.find_proc_by_rva(rva);
                if (!proc_at || !proc_at->has_value() || (*proc_at)->rva != rva || (*proc_at)->size == 0) {
                    return std::nullopt;
                }
                return (*proc_at)->size;
            };
            std::vector<Diag> unproven = prove_inlined_lambda_statics(proof, &verdicts);
            const std::vector<Diag> owners = prove_inlined_owner_statics(proof, &verdicts);
            unproven.insert(unproven.end(), owners.begin(), owners.end());
            if (!unproven.empty()) {
                return first_failure("plan", unproven, unproven.front());
            }
        }
        for (const Diag& w : plan.warnings) {
            warn(report, "plan", w);
        }
        const Clock::time_point plan_proved = Clock::now();
        // WP-36 evidence: image-relative references to old data in this object, and how many were proved displacements.
        s.counts.imgrel_sites = plan.imgrel_sites;
        s.counts.imgrel_verified = plan.imgrel_verified;
        // (WP-40, R-50: at Info only for an object that has such references; the counters of the reload answer, which cmltest
        // and the corpus read, do not depend on this line. The planner itself logs it at Debug for every object.)
        if (plan.imgrel_sites != 0) {
            CML_LOG(Info, "%s", plan.imgrel_line(in.object_name).c_str());
        }
        NarrowInput ni = narrow_input(s, image.get());
        ni.enabled = options_.narrow_hooks;
        const NarrowResult narrowed = narrow_hooks(ni);
        const Clock::time_point plan_narrowed = Clock::now();
        // After merging plan hooks that share an old entry: planned - dropped == hooks.size() == hooks installed by this patch.
        s.planned = narrowed.planned;
        s.dropped = narrowed.dropped;
        s.counts.hooks_planned = narrowed.planned;
        s.counts.hooks_dropped = narrowed.dropped;
        s.counts.new_functions = plan.new_functions.size();
        s.counts.new_globals = plan.new_globals.size();
        s.counts.thunks = plan.thunks.size();
        for (const NarrowDecision& d : narrowed.decisions) {
            if (!is_dropped(d.reason) && d.reason != NarrowReason::Disabled) {
                s.counts.kept.push_back(d.display + ": " + to_string(d.reason) + (d.detail.empty() ? "" : " - " + d.detail));
            }
            CML_LOG(Debug, "reload: hook %s: %s%s%s", d.display.c_str(), to_string(d.reason), d.detail.empty() ? "" : " - ", d.detail.c_str());
        }
        s.hooks = narrowed.hooks;  // one per old entry
        // WP-18, CML3104: a changed small function of an optimised compiland may be inlined into its callers (one warning per compiland).
        {
            std::vector<std::string> small_changed;  // one warning per compiland, naming the small functions
            for (const bind::HookEntry& h : s.hooks) {
                if (h.old_size != 0 && h.old_size <= kSmallFunctionBytes) {
                    small_changed.push_back(h.display);
                }
            }
            if (std::optional<Diag> stale = stale_inline_warning(pdb, u.compiland, small_changed, module_.name)) {
                warn(report, "plan", std::move(*stale));
            }
        }
        s.has_effect = !s.hooks.empty() || !plan.new_functions.empty() || !plan.new_globals.empty() || !plan.initialisers.empty();
        report.times.plan += lap.lap();
        {
            const Clock::time_point plan_done = Clock::now();
            const std::size_t export_parses = (ctx_.exports ? ctx_.exports->parses() : 0) - export_parses_before;
            const pdb::Timings pdb_after = pdb.timings();
            CML_LOG(Info, "WP50-PLAN module=%s object=%s total_ms=%.1f object_ms=%.1f image_ms=%.1f bind_ms=%.1f export_parses=%zu prove_ms=%.1f "
                          "narrow_ms=%.1f rest_ms=%.1f hooks_planned=%zu pdb_module_symbols_ms=%.1f pdb_publics_ms=%.1f pdb_buildinfo_ms=%.1f",
                    module_.name.c_str(), std::string(path::base_name(std::string_view(u.outcome.object_path))).c_str(), ms_between(plan_started, plan_done),
                    ms_between(plan_started, plan_object), ms_between(plan_object, plan_image), ms_between(plan_image, plan_bound), export_parses,
                    ms_between(plan_bound, plan_proved), ms_between(plan_proved, plan_narrowed), ms_between(plan_narrowed, plan_done), narrowed.planned,
                    pdb_after.module_symbols_ms - pdb_before.module_symbols_ms, pdb_after.publics_ms - pdb_before.publics_ms,
                    pdb_after.buildinfo_ms - pdb_before.buildinfo_ms);
            // WP-55 (R-62): the verdicts on inline sites this unit's plan took from earlier plans of the image, and those it read.
            const std::uint64_t verdict_kept = module_.inline_verdicts.kept() - verdict_kept_before;
            const std::uint64_t verdict_reads = module_.inline_verdicts.reads() - verdict_reads_before;
            if (verdict_kept != 0 || verdict_reads != 0) {
                CML_LOG(Info, "WP55-INLINE-CACHE module=%s object=%s kept=%llu read=%llu entries=%zu", module_.name.c_str(),
                        std::string(path::base_name(std::string_view(u.outcome.object_path))).c_str(), static_cast<unsigned long long>(verdict_kept),
                        static_cast<unsigned long long>(verdict_reads), module_.inline_verdicts.size());
            }
        }
        if (!s.has_effect) {
            CML_LOG(Info, "reload: %s changes no code (all %zu planned hooks are identical to the running code); nothing to load", u.source.c_str(),
                    narrowed.planned);
            for (std::string& name : bound_data_changes(ni)) {
                s.counts.data_changes.push_back(std::move(name));
            }
            CML_LOG(Trace, "reload: compiland %s%s prepared: nothing to load%s", unit_name(s).c_str(), unit_suffix(s).c_str(),
                    s.after_pending ? " (compared with a compiland staged before it)" : "");
            s.prepared = true;
            release_prepared(s);
            return Result<void>();
        }

        // ---- anchor + binding object -----------------------------------------------------------------------
        s.patch_dir = workdir_ + "/" + tag;
        std::error_code ec;
        fs::create_directories(s.patch_dir, ec);
        if (ec) {
            return fail("anchor", Diag::make(pd::kWorkDir, "cannot create '" + s.patch_dir + "': " + ec.message(), "Check the TEMP directory."));
        }
        Result<coff::ObjectModel> model = coff::ObjectModel::from_object(s.object->object);
        if (!model) {
            return fail("anchor", model.error());
        }
        coff::AnchorOptions ao;
        ao.tag = tag + "u" + std::to_string(u.compiland);
        {
            std::set<std::uint32_t> assoc(plan.associative_locator_sections.begin(), plan.associative_locator_sections.end());
            for (const bind::InitialiserEntry& e : plan.initialisers) {
                assoc.insert(e.table_section);  // a cl `.CRT$XCU` entry can be an associative COMDAT: it needs a locator
            }
            ao.anchor_associative.assign(assoc.begin(), assoc.end());
        }
        Result<std::vector<coff::SectionLocator>> locators = coff::add_anchors(*model, ao);
        if (!locators) {
            return fail("anchor", locators.error());
        }
        s.locators = std::move(*locators);
        Result<coff::ByteVector> anchored = coff::write(*model);
        if (!anchored) {
            return fail("anchor", anchored.error());
        }
        s.anchored_path = s.patch_dir + "/" + std::string(path::base_name(std::string_view(u.source))) + ".anchored.obj";
        if (!write_file(s.anchored_path, *anchored)) {
            return fail("anchor", Diag::make(pd::kWorkDir, "cannot write '" + s.anchored_path + "'", "Check free disk space and the TEMP directory."));
        }
        s.binds = plan.binding_symbols();
        std::vector<coff::BindSymbol>& binds = s.binds;
        // `/INCLUDE:sym` linker directives (cl writes `/INCLUDE:_fltused` into every object that uses floating
        // point) are no relocations, so the planner never sees them; with /NODEFAULTLIB nothing defines the symbol.
        // The patch gets an unused placeholder so the link resolves it.
        // Undefined externals nothing relocates against (the same `_fltused` as an undefined symbol the /INCLUDE
        // directive forces) are invisible to the planner too.
        for (const coff::Symbol& sym : s.object->object.symbols()) {
            if (!sym.is_external() || !sym.is_undefined() || sym.name.empty()) {
                continue;
            }
            const std::string name(sym.name);
            const bind::SymbolDecision* d = plan.find_decision(name);
            if (d != nullptr && d->decision != bind::DecisionKind::PatchFresh && d->decision != bind::DecisionKind::Unresolved) {
                continue;  // bound, linker-provided, weak default ...: the planner decided
            }
            const bool bound = std::any_of(binds.begin(), binds.end(), [&](const coff::BindSymbol& b) { return b.name == name; });
            if (!bound && name != "__ImageBase") {
                CML_LOG(Debug, "reload: undefined symbol %s is referenced by no relocation: placeholder", name.c_str());
                binds.push_back(coff::BindSymbol{name, coff::BindKind::Data});
            }
        }
        for (const coff::Directive& dir : s.object->object.directives()) {
            if (dir.kind != coff::DirectiveKind::Include || dir.value.empty()) {
                continue;
            }
            const coff::Symbol* defined = s.object->object.find_symbol(dir.value);
            const bool in_object = defined != nullptr && !defined->is_undefined();
            const bool bound = std::any_of(binds.begin(), binds.end(), [&](const coff::BindSymbol& b) { return b.name == dir.value; });
            if (!in_object && !bound) {
                CML_LOG(Debug, "reload: /INCLUDE:%s has no definition in the patch: placeholder", dir.value.c_str());
                binds.push_back(coff::BindSymbol{dir.value, coff::BindKind::Data});
            }
        }
        if (!binds.empty()) {
            Result<coff::ByteVector> binding = coff::make_binding_object(binds);
            if (!binding) {
                return fail("anchor", binding.error());
            }
            s.binding_path = s.patch_dir + "/binding.obj";
            if (!write_file(s.binding_path, *binding)) {
                return fail("anchor", Diag::make(pd::kWorkDir, "cannot write '" + s.binding_path + "'", "Check free disk space and the TEMP directory."));
            }
        }
        report.times.anchor += lap.lap();

        // ---- reserve address space (once per module), link -----------------------------------------------------
        if (Result<void> r = ensure_reserved(); !r) {
            return r;
        }
        if (Result<void> r = ensure_linker(u, pdb, report); !r) {
            return r;
        }
        if (Result<void> r = link(s, report); !r) {
            return r;
        }
        CML_LOG(Trace, "reload: compiland %s%s prepared: %zu hooks, %zu new functions, %zu new globals, linked at %s%s", unit_name(s).c_str(), unit_suffix(s).c_str(),
                s.hooks.size(), plan.new_functions.size(), plan.new_globals.size(), hex(s.linked.base).c_str(),
                s.after_pending ? " (planned against the compilands staged before it)" : "");
        s.prepared = true;
        release_prepared(s);
        return Result<void>();
    }

    // Drops what only prepare() and link() need: the COFF object (with its /Z7 debug sections), the plan, the narrowed hooks and the
    // link inputs. A header fan-out over hundreds of compilands must not hold every object until the reload ends (the sequential
    // pipeline freed each one in turn). What the apply and the overlay need stays: the fix-ups, the hook table, the code records and
    // the registry entries. A relink prepares the compiland again from its object file.
    void release_prepared(Staged& s) {
        s.object.reset();
        s.plan = bind::BindPlan{};
        std::vector<bind::HookEntry>().swap(s.hooks);
        std::vector<coff::SectionLocator>().swap(s.locators);
        std::vector<coff::BindSymbol>().swap(s.binds);
    }

    // The first of `errors` (each one published as a diag event), else `fallback`.
    Diag first_failure(const char* stage, const std::vector<Diag>& errors, const Diag& fallback) {
        std::optional<Diag> first;
        for (const Diag& e : errors) {
            const Diag shown = fail(stage, e);
            first = first ? first : std::optional<Diag>(shown);
        }
        return first ? *first : fail(stage, fallback);
    }

    // Links the staged compiland at the next free base, makes its fix-ups and computes what applying it adds: the hook table, the
    // WP-25 code records and the registry entries. Nothing is loaded. On failure its slice stays held (release_ranges gives it back).
    Result<void> link(Staged& s, PatchReport& report) {
        Lap lap;
        s.linked_ok = false;
        if (Result<void> t = check_time("link"); !t) {
            return t;
        }
        std::uint64_t base = 0;
        {
            std::lock_guard<std::mutex> lock(module_.mutex);
            base = module_.reservation.next_base(kMinPatchRoom);
        }
        if (base == 0) {
            return fail("link", Diag::make(pd::kReserveFailed, "the reserved address space near " + module_.name + " has no room left for a patch",
                                           "Restart the application, or free address space near the module."));
        }
        toolchain::LinkPatchRequest lr;
        {
            std::lock_guard<std::mutex> lock(module_.mutex);
            lr.linker = *module_.linker;
        }
        lr.objects.push_back(toolchain::PatchObjectInput{s.anchored_path, s.locators});
        lr.binding_object = s.binding_path;
        lr.bindings = s.binds;
        lr.out_dir = s.patch_dir;
        lr.name = "patch_" + std::to_string(s.patch_no) + "_" + std::to_string(++s.link_attempt);
        lr.base = base;
        lr.timeout_ms = static_cast<unsigned>(std::max<std::int64_t>(1000, remaining_ms()));
        lr.cancel = ctx_.cancel;
        if (!env_.link_patch) {
            return fail("link", Diag::make(pd::kStageFailed, "this broker has no linker driver", "Run the broker on Windows."));
        }
        if (const std::optional<Diag> f = injected(ipc::fault::Stage::Link)) {
            return fail("link", *f);
        }
        toolchain::LinkTrace trace;
        Result<toolchain::LinkedPatch> lp = env_.link_patch(lr, &trace);
        if (!lp) {
            CML_LOG(Error, "link failed:\n%s", trace.output.c_str());
            return fail("link", lp.error());
        }
        s.linked = std::move(*lp);
        for (const Diag& w : s.linked.warnings) {
            warn(report, "link", w);
        }
        report.times.link += lap.lap();
        {
            std::lock_guard<std::mutex> lock(module_.mutex);
            module_.reservation.consume(base, s.linked.image_size);
        }
        s.range_held = true;

        // ---- fix-ups (broker side) -----------------------------------------------------------------------------
        Result<void> prepared = prepare_fixups(s.plan, s.linked, report, &s.fixups, &s.init_rvas);
        report.times.fixups += lap.lap();
        if (!prepared) {
            return prepared;
        }

        // ---- what applying it adds --------------------------------------------------------------------------------
        s.hook_table.clear();
        for (const bind::HookEntry& h : s.hooks) {
            const std::optional<std::uint64_t> target = function_va(s.plan, s.linked, h.new_symbol);
            if (!target) {
                return fail("link", Diag::make(bind::diag::kLayoutIncomplete, "the patch layout has no address for the hooked function '" + h.display + "'",
                                               "Report this with the bind plan."));
            }
            s.hook_table.push_back(AppliedHook{h.old_va, *target, h.display});
        }
        // WP-25: what the functions of this patch look like, so the next compiland (and the next reload) can compare with it
        // (hooked functions: keyed by the patch entry the hook jumps to; new functions: by their own address).
        s.code.clear();
        {
            const auto address_of = [&](const std::string& name) -> std::optional<std::uint64_t> {
                const bind::SymbolDecision* d = s.plan.find_decision(name);
                if (d == nullptr) {
                    return std::nullopt;
                }
                return symbol_va(s.linked, d->section, d->value);
            };
            const NarrowInput ni = narrow_input(s, module_image(report).get());
            const CodeRecorder recorder(ni);  // the maps over the plan and the resolved relocations are built once
            const auto record_one = [&](const std::string& symbol, const std::string& display, std::uint64_t entry) {
                std::string why;
                std::optional<CodeImage> recorded = recorder.capture(symbol, &why);
                if (!recorded || !finalize_code(*recorded, address_of)) {
                    CML_LOG(Trace, "reload: the code of %s is not recorded%s%s", display.c_str(), why.empty() ? "" : ": ", why.c_str());
                    return;
                }
                recorded->display = display;
                s.code.emplace_back(entry, std::make_shared<const CodeImage>(std::move(*recorded)));
            };
            for (std::size_t i = 0; i < s.hook_table.size(); ++i) {
                record_one(s.hooks[i].new_symbol, s.hooks[i].display, s.hook_table[i].new_entry);
            }
            for (const bind::NewFunction& f : s.plan.new_functions) {
                if (const std::optional<std::uint64_t> va = symbol_va(s.linked, f.section, f.value)) {
                    record_one(f.name, f.display, *va);
                }
            }
        }
        s.symbols.clear();
        s.layout_warnings.clear();
        const std::string origin = "patch " + std::to_string(s.patch_no);
        for (const bind::NewFunction& f : s.plan.new_functions) {
            if (const std::optional<std::uint64_t> va = symbol_va(s.linked, f.section, f.value)) {
                s.symbols.emplace_back(f.name, SymbolAddress{*va, true, origin});
            } else {
                s.layout_warnings.push_back(Diag::make(bind::diag::kLayoutIncomplete,
                                                       "the new function '" + f.display + "' has no address in the patch layout; later patches cannot bind to it",
                                                       "Report this with the bind plan."));
            }
        }
        for (const bind::NewGlobal& g : s.plan.new_globals) {
            if (const std::optional<std::uint64_t> va = symbol_va(s.linked, g.section, g.value)) {
                s.symbols.emplace_back(g.name, SymbolAddress{*va, false, origin});
            } else {
                s.layout_warnings.push_back(Diag::make(bind::diag::kLayoutIncomplete,
                                                       "the new global '" + g.display + "' has no address in the patch layout; later patches cannot bind to it",
                                                       "Report this with the bind plan."));
            }
        }
        s.linked_ok = true;
        return Result<void>();
    }

    // Gives back the slices of the reservation that the staged compilands from `first` on hold with nothing mapped, newest first
    // (only the newest slice of a block can be given back, so the order matters).
    void release_ranges(std::size_t first) {
        for (std::size_t i = staged_.size(); i > first; --i) {
            Staged& s = *staged_[i - 1];
            if (s.range_held) {
                give_back_range(s.linked.base, s.linked.image_size);
                s.range_held = false;
            }
        }
    }

    // A compiland was refused while the reload was being prepared: nothing was applied. The refusal keeps its id; for a reload of
    // several compilands its message names the compiland and CML3650 says that nothing of the reload was applied.
    Diag refused(Diag d) {
        const Staged& s = *staged_.back();
        CML_LOG(Trace, "reload: compiland %s%s refused (%s): nothing of the reload is applied", unit_name(s).c_str(), unit_suffix(s).c_str(), d.id().c_str());
        if (units_->size() < 2) {
            return d;
        }
        std::size_t ready = 0;
        for (std::size_t i = 0; i + 1 < staged_.size(); ++i) {
            ready += staged_[i]->has_effect ? 1 : 0;
        }
        emit(Diag::make(ar::kRefusedNothingApplied,
                        "the reload of " + module_.name + " was refused at compiland " + std::to_string(s.unit + 1) + " of " + std::to_string(units_->size()) + " (" +
                            unit_name(s) + ", " + d.id() + "): none of its compilands was applied (" + std::to_string(ready) +
                            " compiland(s) before it were planned and linked, none was loaded); the process runs what it ran before",
                        "Fix what the refusal names, then reload: the whole edit is applied then. Files the reload did not apply are compiled again."),
             "warning", "plan");
        d.message += unit_suffix(s) + "; nothing of this reload was applied";
        return d;
    }

    // Applying compiland `k` failed (its own patch is already unloaded, or kept when it may still become live). Rolls back what the
    // reload applied before it, unless that patch may still become live (it cannot be rolled back) or the agent is gone.
    Diag after_apply_failure(Diag d, std::size_t k, std::uint64_t generation_before) {
        const Staged& s = *staged_[reprepare_failed_.value_or(k)];  // the compiland that failed: k, or one prepared again after its relink
        std::size_t with_effect = 0;
        for (const std::unique_ptr<Staged>& t : staged_) {
            with_effect += t->has_effect ? 1 : 0;
        }
        std::size_t applied = 0;
        for (const std::unique_ptr<Staged>& t : staged_) {
            applied += t->applied ? 1 : 0;
        }
        if (link_.closed() && !applied_.empty()) {
            // Nothing can be rolled back or confirmed any more: the applied compilands stay recorded (the batch), but none of the
            // reload's files is re-baselined and no `patch_applied` is published (hold_back_). A compiland whose hooks reached the
            // agent before it went away (live_unconfirmed) may be live: it is neither "applied" nor "not applied".
            const std::size_t maybe = s.live_unconfirmed ? 1 : 0;
            const std::size_t left = with_effect - applied - maybe;
            CML_LOG(Trace, "reload: applying compiland %s%s failed (%s) and the agent is gone: %zu applied compiland(s) stay recorded, %zu possibly live, %zu not applied",
                    unit_name(s).c_str(), unit_suffix(s).c_str(), d.id().c_str(), applied, maybe, left);
            hold_back_ = true;
            applied_events_.clear();
            const std::string state = std::to_string(applied) + " compiland(s) of this reload were applied before it, " +
                                      (maybe != 0 ? std::string("its hooks reached the agent, so it may be live, ") : std::string()) + std::to_string(left) +
                                      (maybe != 0 ? " other(s) were not" : " were not");
            emit(Diag::make(ar::kAgentLostPartly,
                            "the agent of process " + std::to_string(pid_) + " disconnected while compiland " + std::to_string(s.unit + 1) + " of " +
                                std::to_string(units_->size()) + " of " + module_.name + " (" + unit_name(s) + ", " + d.id() + ") was applied: " + state +
                                ", and nothing could be rolled back; if the process still runs, it runs a part of the edit",
                            "Restart the application (or hot-restart it); if it still runs, enable the module again and reload: the reload's files are compiled again."),
                 "warning", "load");
            d.message += unit_suffix(s) + "; the agent is gone: " + state;
            return d;
        }
        if (s.live_unconfirmed) {
            maybe_live_ = true;  // WP-54: the other modules of the process are not rolled back either
            const std::size_t left = with_effect - applied - 1;
            CML_LOG(Trace, "reload: compiland %s%s may still become live (%s): no roll-back; %zu applied before it, %zu not applied", unit_name(s).c_str(),
                    unit_suffix(s).c_str(), d.id().c_str(), applied, left);
            if (with_effect > 1) {
                emit(Diag::make(ar::kStoppedPartly,
                                "compiland " + std::to_string(s.unit + 1) + " of " + std::to_string(units_->size()) + " of " + module_.name + " (" + unit_name(s) + ", " + d.id() +
                                    ") may still become live and cannot be rolled back, so the reload stopped there: " + std::to_string(applied) +
                                    " compiland(s) before it are applied, " + std::to_string(left) + " after it are not; the process may run a part of the edit",
                                "In synchronised mode let the application reach SyncPoint(), then reload (the files of the compilands that were not applied are compiled again), or `revert` this reload."),
                     "warning", "hooks");
                d.message += unit_suffix(s) + "; " + std::to_string(left) + " compiland(s) after it were not applied";
            }
            return d;
        }
        if (applied_.empty()) {
            CML_LOG(Trace, "reload: applying compiland %s%s failed (%s) before anything of the reload was live: nothing to roll back", unit_name(s).c_str(),
                    unit_suffix(s).c_str(), d.id().c_str());
            if (with_effect > 1) {
                d.message += unit_suffix(s) + "; nothing of this reload was applied";
            }
            return d;
        }
        // The WP-22 revert of this reload's batch: re-points what an older patch also hooked, restores the original bytes of the rest,
        // un-commits the images, and puts the registry, the hook table, the code records and the generation back.
        const std::size_t count = applied_.size();
        CML_LOG(Trace, "reload: applying compiland %s%s failed (%s): rolling back the %zu compiland(s) this reload applied", unit_name(s).c_str(), unit_suffix(s).c_str(),
                d.id().c_str(), count);
        {
            PatchBatch b;
            b.patches = std::move(applied_);
            b.generation_before = generation_before;
            b.reload_id = ctx_.reload_id;
            std::lock_guard<std::mutex> lock(module_.mutex);
            module_.batches.push_back(std::move(b));
        }
        applied_.clear();
        RevertOptions ro;
        ro.timeout_ms = options_.hooks_timeout_ms;
        // Its own budget: the failure may be the reload's deadline.
        ro.deadline = Clock::now() + std::chrono::milliseconds(std::max<std::int64_t>(remaining_ms(), kRollbackBudgetMs));
        ro.only_reload = ctx_.reload_id;
        ro.events = nullptr;  // nothing of it was announced: `patch_applied` waits for the end of the reload
        Result<RevertReport> back = revert_module(link_, module_, RevertScope::Last, ro);
        // Whatever the outcome, nothing of this reload is re-baselined (the batch replaced no baseline, so the next reload compiles its
        // files again) and nothing of it is announced.
        for (const std::unique_ptr<Staged>& t : staged_) {
            t->applied = false;
        }
        applied_events_.clear();
        const std::string what = "applying compiland " + std::to_string(s.unit + 1) + " of " + std::to_string(units_->size()) + " of " + module_.name + " (" +
                                 unit_name(s) + ") failed (" + d.format() + ") and ";
        if (!back) {
            // revert_module changed nothing (or reports the mixed state itself, CML5857). The batch stays recorded unless the revert
            // dropped the module's patch list (CML5856: the module is no longer loaded where the patches were made).
            bool batch_kept = false;
            {
                std::lock_guard<std::mutex> lock(module_.mutex);
                batch_kept = !module_.batches.empty() && module_.batches.back().reload_id == ctx_.reload_id;
            }
            return fail("rollback", Diag::make(ar::kRollbackFailed,
                                               what + "rolling back the " + std::to_string(count) + " compiland(s) this reload had applied failed too (" +
                                                   back.error().format() + "): the process runs a part of the edit" +
                                                   (batch_kept ? std::string("; they stay recorded as this reload's batch")
                                                               : std::string("; the module's patch list was dropped")),
                                               batch_kept ? "Send `revert` (scope last) to try the roll-back again, or restart the application."
                                                          : "Restart the application, or enable the module again and reload."));
        }
        for (const Diag& w : back->warnings) {
            emit(w, "warning", "rollback");
        }
        // revert_module succeeds also when the agent did not confirm the hook changes in time (CML5852) or did not un-commit the
        // images (CML5855): its state then follows the roll-back as if the agent applies it later (WP-22). That is not "the process
        // runs what it ran before": say so, as an error.
        std::string unconfirmed;
        for (const Diag& w : back->warnings) {
            if (w.code == revert::diag::kRevertUnconfirmed || w.code == revert::diag::kUncommitFailed) {
                unconfirmed += (unconfirmed.empty() ? "" : "; ") + w.format();
            }
        }
        if (!unconfirmed.empty()) {
            return fail("rollback", Diag::make(ar::kRollbackUnconfirmed,
                                               what + "the roll-back of the " + std::to_string(count) + " compiland(s) this reload had applied was sent but not confirmed (" +
                                                   unconfirmed + "): the broker's state follows the roll-back, and the process may run a part of the edit until the agent applies it",
                                               "In synchronised mode let the application reach SyncPoint(); if the application's hooks of the rolled-back patches keep running, restart the application."));
        }
        emit(Diag::make(ar::kRolledBack,
                        "applying compiland " + std::to_string(s.unit + 1) + " of " + std::to_string(units_->size()) + " of " + module_.name + " (" + unit_name(s) + ", " +
                            d.id() + ") failed, so the " + std::to_string(count) + " compiland(s) this reload had applied were rolled back (" +
                            std::to_string(back->hooks_restored) + " function(s) back to the original code, " + std::to_string(back->hooks_repointed) +
                            " re-pointed at an older patch): the process runs what it ran before the reload",
                        "Fix what the failure names, then reload: the whole edit is applied then."),
             "warning", "rollback");
        d.message += unit_suffix(s) + "; the " + std::to_string(count) + " compiland(s) this reload had applied were rolled back";
        return d;
    }

    // The plan counters of the compilands, added to the report once the reload is applied (a compiland prepared twice counts once).
    void add_plan_counts(PatchReport& report) {
        for (const std::unique_ptr<Staged>& sp : staged_) {
            const PlanCounts& c = sp->counts;
            report.hooks_planned += c.hooks_planned;
            report.hooks_dropped += c.hooks_dropped;
            report.new_functions += c.new_functions;
            report.new_globals += c.new_globals;
            report.thunks += c.thunks;
            report.imgrel_sites += c.imgrel_sites;
            report.imgrel_verified += c.imgrel_verified;
            for (const std::string& k : c.kept) {
                if (report.hooks_kept.size() < kMaxKeptNotes) {
                    report.hooks_kept.push_back(k);
                } else {
                    ++report.hooks_kept_more;
                }
            }
            if (!sp->has_effect) {
                ++report.units_unchanged;  // WP-25: finish() turns "every unit unchanged" into the CML3450 / CML3451 warning
                for (const std::string& n : c.data_changes) {
                    report.data_changes.push_back(n);
                }
            }
        }
    }

    // ---- apply: load, fix-ups, initialisers, hooks, commit --------------------------------------------------------
    Result<void> apply(std::size_t k, PatchReport& report) {
        Staged& s = *staged_[k];
        Lap lap;
        const unsigned attempts_allowed = std::max(1u, options_.max_load_attempts);
        for (unsigned attempt = 1;; ++attempt) {
            if (Result<void> t = check_time("load"); !t) {
                return t;
            }
            ++report.load_attempts;
            ipc::LoadPatch load;
            load.request_id = link_.next_request_id();
            load.patch_id = s.patch_no;
            ipc::PatchCandidate candidate;
            candidate.path = s.linked.dll_path;
            candidate.image_base = s.linked.base;
            candidate.image_size = s.linked.image_size;
            load.candidates.push_back(std::move(candidate));
            if (const std::optional<Diag> f = injected(ipc::fault::Stage::SendPatch)) {
                return fail("load", *f);  // LoadPatch is never sent: nothing is loaded in the application
            }
            Result<ipc::Message> answer = ask(std::move(load), options_.agent_timeout_ms, "load");
            if (!answer) {
                if (answer.error().code == pd::kAgentRequestTimeout) {
                    // The agent works through requests in order, so an unload sent now runs after a LoadPatch that is
                    // still queued: the late load does not leave an unused patch behind.
                    unload(s.linked.base);
                    s.range_held = false;  // a late load may have mapped it: never handed out again
                }
                return answer.error();
            }
            report.times.load += lap.lap();
            std::uint32_t status = 0;
            std::string message;
            bool loaded = false;
            if (const auto* pl = std::get_if<ipc::PatchLoaded>(&*answer)) {
                status = pl->status;
                message = pl->message;
                if (status == 0) {
                    loaded = true;
                    report.base = pl->image_base;
                    report.image_size = pl->image_size;
                }
            } else if (const auto* rep = std::get_if<ipc::Reply>(&*answer)) {
                status = rep->status != 0 ? rep->status : 5306;
                message = rep->message;
            } else {
                status = 5306;
                message = "unexpected answer type";
            }
            if (loaded) {
                s.mapped = true;
                s.range_held = false;  // an image that was loaded keeps its slice
                break;
            }
            if ((status == 4203 || status == 4205) && attempt < attempts_allowed) {
                CML_LOG(Warn, "reload: the address range at %s was taken (CML%u); relinking at another base", hex(s.linked.base).c_str(), status);
                s.range_held = false;  // taken by something else: not ours to give back
                if (Result<void> r = relink(k, report); !r) {
                    return r;
                }
                continue;
            }
            return fail("load", agent_refusal("LoadPatch", status, message, pd::kLoadFailed));  // not mapped: release_ranges gives the slice back
        }

        // From here on a failure unloads the patch again.
        const std::uint64_t base = s.linked.base;
        const auto abandon = [&] { unload(base); };

        // ---- apply the fix-ups ------------------------------------------------------------------------------
        {
            std::size_t sent = 0;
            while (sent < s.fixups.wire.size()) {
                const std::size_t n = std::min<std::size_t>(ipc::kMaxFixupsPerMessage, s.fixups.wire.size() - sent);
                ipc::ApplyFixups req;
                req.request_id = link_.next_request_id();
                req.image_base = base;
                req.fixups.assign(s.fixups.wire.begin() + static_cast<std::ptrdiff_t>(sent), s.fixups.wire.begin() + static_cast<std::ptrdiff_t>(sent + n));
                Result<ipc::Message> answer = ask(std::move(req), options_.agent_timeout_ms, "fixups");
                if (!answer) {
                    abandon();
                    return answer.error();
                }
                if (const auto* rep = std::get_if<ipc::Reply>(&*answer); rep == nullptr || rep->status != 0) {
                    abandon();
                    return fail("fixups", agent_refusal("ApplyFixups", rep != nullptr ? rep->status : 5306, rep != nullptr ? rep->message : "unexpected answer",
                                                        pd::kLoadFailed));
                }
                sent += n;
            }
            report.fixups += s.fixups.wire.size();
            report.pre_applied += s.fixups.pre_applied.size();
            report.times.fixups += lap.lap();
        }

        // ---- initialisers of the new globals ------------------------------------------------------------------
        report.initializers += s.init_rvas.size();
        if (!s.init_rvas.empty()) {
            ipc::RunInitializers req;
            req.request_id = link_.next_request_id();
            req.image_base = base;
            req.rvas = s.init_rvas;
            Result<ipc::Message> answer = ask(std::move(req), options_.agent_timeout_ms, "init");
            if (!answer) {
                abandon();
                return answer.error();
            }
            if (const auto* rep = std::get_if<ipc::Reply>(&*answer); rep == nullptr || rep->status != 0) {
                abandon();
                return fail("init", agent_refusal("RunInitializers", rep != nullptr ? rep->status : 5306, rep != nullptr ? rep->message : "unexpected answer",
                                                  pd::kLoadFailed));
            }
            report.initializers_run += s.init_rvas.size();
        }
        report.times.init += lap.lap();

        // ---- hooks -------------------------------------------------------------------------------------------------
        if (!s.hook_table.empty()) {
            const Clock::time_point hooks_started = Clock::now();  // WP-50: request built / round trip
            ipc::InstallHooks req;
            req.request_id = link_.next_request_id();
            const std::uint64_t hooks_request_id = req.request_id;
            for (const AppliedHook& h : s.hook_table) {
                req.hooks.push_back(ipc::HookSpec{h.old_entry, h.new_entry, h.name});
            }
            const Clock::time_point hooks_sent = Clock::now();
            Result<ipc::Message> answer = link_.request(ipc::Message(std::move(req)), clamp_timeout(options_.hooks_timeout_ms));
            // WP-50 (R-47): the broker's side of the hooks stage. The agent's breakdown of the same request (the line
            // `WP50-HOOKS side=agent request=<same id>`) is forwarded by the agent and logged by the session before this one.
            CML_LOG(Info, "WP50-HOOKS side=broker request=%llu module=%s hooks=%zu prepare_ms=%.1f roundtrip_ms=%.1f answered=%d",
                    static_cast<unsigned long long>(hooks_request_id), module_.name.c_str(), s.hook_table.size(), ms_between(hooks_started, hooks_sent),
                    ms_between(hooks_sent, Clock::now()), answer ? 1 : 0);
            if (!answer) {
                // No answer: the agent may still apply the hooks (synchronised mode waits for SyncPoint). The
                // patch must then stay loaded; remember the hooks as installed so the next reload re-points them.
                const bool timed_out = answer.error().code == pd::kAgentRequestTimeout;  // a send failure or a lost agent is a plain failure
                if (timed_out) {
                    keep_unconfirmed(s);
                    return fail("hooks", Diag::make(pd::kHooksTimeout, "the agent did not confirm the hooks in time: " + answer.error().message,
                                                    "In synchronised mode the application must call SyncPoint(); the patch stays loaded and the hooks may still apply."));
                }
                if (link_.closed()) {
                    // WP-47 review: the agent went away with the request in flight; it may have installed the hooks before. Treated
                    // like a timeout: the patch is never unloaded and counts as possibly live (after_apply_failure, CML3655).
                    keep_unconfirmed(s);
                    return fail("hooks", answer.error());
                }
                abandon();
                return fail("hooks", answer.error());
            }
            const auto* done = std::get_if<ipc::HooksInstalled>(&*answer);
            if (done == nullptr) {
                abandon();
                const auto* rep = std::get_if<ipc::Reply>(&*answer);
                return fail("hooks", agent_refusal("InstallHooks", rep != nullptr ? rep->status : 5306, rep != nullptr ? rep->message : "unexpected answer",
                                                   pd::kHooksFailed));
            }
            if (done->status == ipc::fault::ids::kAgentRollbackIncomplete) {
                // WP-28: the agent could not take every hook of this request out again; a live hook may still jump into the patch.
                // The image must stay mapped (never free an image a live hook can target): leak it, remember the hooks as installed
                // like after a timeout, and report the failure.
                keep_unconfirmed(s);
                return fail("hooks", agent_refusal("InstallHooks", done->status, done->message, pd::kHooksFailed));
            }
            if (done->status != 0) {
                abandon();
                return fail("hooks", agent_refusal("InstallHooks", done->status, done->message, pd::kHooksFailed));
            }
            CML_LOG(Info, "reload: %zu hooks installed (threads stopped: %u, window %u ms, attempts %u, retries %u)", s.hook_table.size(), done->threads, done->window_ms,
                    done->attempts, done->retries);
            report.hooks_installed += s.hook_table.size();
        }
        report.times.hooks += lap.lap();
        commit(s, report);
        return Result<void>();
    }

    // The range of compiland `k` was taken: prepare it again (its object and plan were dropped after its link, release_prepared), which
    // links it at the next free base, and prepare the staged compilands after it again, since they were planned against its addresses
    // (its new functions and globals, its hook targets). A failure names the compiland that failed (reprepare_failed_).
    Result<void> relink(std::size_t k, PatchReport& report) {
        release_ranges(k + 1);
        for (std::size_t j = k + 1; j < staged_.size(); ++j) {
            staged_[j]->reset();  // out of the overlay until prepared again
        }
        for (std::size_t j = k; j < staged_.size(); ++j) {
            if (j > k) {
                CML_LOG(Trace, "reload: compiland %s is prepared again: compiland %zu was relinked at %s", unit_name(*staged_[j]).c_str(), staged_[k]->unit + 1,
                        hex(staged_[k]->linked.base).c_str());
            }
            Result<void> r = prepare(*staged_[j], report);
            if (r && j == k && !staged_[k]->has_effect) {
                staged_[k]->reset();  // not prepared: its files must not be re-baselined (WP-47 review)
                r = fail("link", Diag::make(pd::kStageFailed, "compiland " + unit_name(*staged_[k]) + " planned to nothing when it was prepared again for a relink",
                                            "This is a bug in the broker; see the broker log."));
            }
            if (!r) {
                if (j != k) {
                    reprepare_failed_ = j;
                    Diag d = r.error();
                    d.message += " (when compiland " + std::to_string(staged_[j]->unit + 1) + " was prepared again after compiland " + std::to_string(staged_[k]->unit + 1) +
                                 " was relinked at another base)";
                    return d;
                }
                return r;
            }
        }
        return Result<void>();
    }

    // The patch may be live (no confirmation in time, or the agent could not take its hooks out again): it stays mapped, its hooks are
    // remembered as installed (so the next reload re-points them) and it is part of the reload's batch (so a revert knows it).
    void keep_unconfirmed(Staged& s) {
        {
            std::lock_guard<std::mutex> lock(module_.mutex);
            for (const AppliedHook& h : s.hook_table) {
                module_.hooked[h.old_entry] = h.new_entry;
            }
        }
        for (const auto& c : s.code) {
            module_.record_code(c.first, c.second);  // WP-25; takes the module lock itself
        }
        AppliedPatch applied;
        applied.number = s.patch_no;
        applied.base = s.linked.base;
        applied.dll_path = s.linked.dll_path;
        applied.hooks = s.hook_table;
        applied_.push_back(std::move(applied));
        s.live_unconfirmed = true;
    }

    // ---- commit: the process symbol registry and the hook table (plan step 9) -----------------------------------
    void commit(Staged& s, PatchReport& report) {
        AppliedPatch applied;  // WP-22: what a revert needs
        applied.number = s.patch_no;
        applied.base = s.linked.base;
        applied.dll_path = s.linked.dll_path;
        applied.hooks = s.hook_table;
        {
            std::lock_guard<std::mutex> lock(module_.mutex);
            for (const AppliedHook& h : s.hook_table) {
                module_.hooked[h.old_entry] = h.new_entry;
            }
            ++module_.generation;
            report.generation = module_.generation;
            ++report.patches;
        }
        for (const auto& c : s.code) {
            module_.record_code(c.first, c.second);  // WP-25; takes the module lock itself
        }
        for (const auto& [name, address] : s.symbols) {
            applied.registry.push_back(RegistryUndo{name, module_.registry.replace(name, address)});
        }
        for (const Diag& w : s.layout_warnings) {
            warn(report, "hooks", w);
        }
        module_.prune_code();  // records of patches nothing reaches any more (superseded hooks)
        applied_.push_back(std::move(applied));
        s.applied = true;
        report.dll_path = s.linked.dll_path;
        report.patched = true;
        applied_events_.push_back(make_patch_applied_event(pid_, module_.name, report.generation, s.hook_table.size(), s.counts.new_functions,
                                                           s.counts.new_globals, s.planned, s.dropped));
        CML_LOG(Info, "reload: patch %llu of %s applied at %s (%zu hooks, %zu dropped, %zu new functions, %zu new globals, %zu fix-ups, %zu initialisers)",
                static_cast<unsigned long long>(report.generation), module_.name.c_str(), hex(s.linked.base).c_str(), s.hook_table.size(), s.dropped,
                s.counts.new_functions, s.counts.new_globals, s.fixups.wire.size(), s.init_rvas.size());
    }
    // ---- pieces -----------------------------------------------------------------------------------------------
    static constexpr std::size_t kMaxKeptNotes = 50;  // entries of PatchReport::hooks_kept
    static constexpr std::uint64_t kMinPatchRoom = 4ull << 20;  // a patch needs room for at least this much image
    static constexpr std::int64_t kRollbackBudgetMs = 10000;      // WP-47: the least time a roll-back gets, whatever the reload has left

    std::shared_ptr<const pe::OwnedImage> module_image(PatchReport& report) {
        // WP-50 (R-47): normally read in the background once the index was ready (ReloadService); this waits for a read in flight
        // instead of reading the file a second time, and reads it here only when no background read did. The run keeps what it got
        // for its later units: a state released meanwhile (disabled while this reload runs) stores nothing, and each unit would
        // otherwise read the whole file again and hold a copy of its own.
        if (image_) {
            return image_;
        }
        ModuleState::ImageRead read = module_.ensure_image(false);
        if (read.read_now) {
            CML_LOG(Info, "WP50-ONEOFF what=module-image image=reload module=%s ms=%.1f mib=%.1f ok=%d", module_.name.c_str(), read.ms, read.mib,
                    read.image ? 1 : 0);
        }
        if (!read.image) {
            // Not fatal: without the old bytes every planned hook is kept.
            const Diag d = Diag::make(pd::kModuleImage,
                                      "cannot read the module file '" + module_.path + "' to compare function bodies: " +
                                          (read.error ? read.error->message : std::string("it was not read")),
                                      "All functions of the patched object are hooked this time.");
            emit(d, "warning", "plan");
            report.warnings.push_back(d);
            return nullptr;
        }
        image_ = read.image;
        return image_;
    }
    std::shared_ptr<const pe::OwnedImage> image_;  // the module file as this run compares with it (module_image)

    Result<void> ensure_reserved() {
        {
            std::lock_guard<std::mutex> lock(module_.mutex);
            if (module_.reservation.reserved()) {
                return Result<void>();
            }
        }
        const Clock::time_point reserve_started = Clock::now();
        ipc::ReserveRange req;
        req.request_id = link_.next_request_id();
        req.module_base = module_.base;
        req.module_size = module_.size;
        req.block_size = static_cast<std::uint64_t>(options_.reserve_block_mb) << 20;
        req.block_count = options_.reserve_blocks;
        Result<ipc::Message> answer = ask(std::move(req), options_.agent_timeout_ms, "reserve");
        if (!answer) {
            return answer.error();
        }
        const auto* rr = std::get_if<ipc::ReserveRangeResult>(&*answer);
        if (rr == nullptr) {
            const auto* rep = std::get_if<ipc::Reply>(&*answer);
            return fail("reserve", agent_refusal("ReserveRange", rep != nullptr ? rep->status : 5306, rep != nullptr ? rep->message : "unexpected answer",
                                                 pd::kReserveFailed));
        }
        if (rr->status != 0 || rr->blocks.empty()) {
            return fail("reserve", agent_refusal("ReserveRange", rr->status != 0 ? rr->status : 4201, rr->message.empty() ? "no block could be reserved" : rr->message,
                                                 pd::kReserveFailed));
        }
        for (const ipc::AddressBlock& b : rr->blocks) {
            CML_LOG(Info, "reload: reserved %llu MiB at %s near %s", static_cast<unsigned long long>(b.size >> 20), hex(b.base).c_str(), module_.name.c_str());
        }
        CML_LOG(Info, "WP50-ONEOFF what=reserve-range module=%s ms=%.1f blocks=%zu", module_.name.c_str(), ms_between(reserve_started, Clock::now()), rr->blocks.size());
        std::lock_guard<std::mutex> lock(module_.mutex);
        module_.reservation.set_blocks(rr->blocks);
        return Result<void>();
    }

    Result<void> ensure_linker(const Unit& u, const pdb::PdbIndex& pdb, PatchReport& report) {
        // The cache is keyed by the options that decide the linker: set-option linker / linker_path takes effect.
        const std::string key = options_.linker_override + "|" + options_.linker_path;
        {
            std::lock_guard<std::mutex> lock(module_.mutex);
            if (module_.linker && module_.linker_key == key) {
                return Result<void>();
            }
        }
        if (!env_.resolve_linker) {
            return fail("link", Diag::make(pd::kStageFailed, "this broker has no linker driver", "Run the broker on Windows."));
        }
        const Clock::time_point detect_started = Clock::now();
        std::vector<std::string> dirs;
        if (!u.outcome.compiler_used.empty()) {
            dirs.push_back(std::string(path::parent_directory(std::string_view(u.outcome.compiler_used))));
        }
        std::vector<Diag> warnings;
        Result<toolchain::ToolInfo> tool = env_.resolve_linker(module_.path, &pdb, dirs, options_, &warnings);
        for (const Diag& w : warnings) {
            warn(report, "link", w);
        }
        if (!tool) {
            return fail("link", tool.error());
        }
        CML_LOG(Info, "reload: linker for %s: %s %s", module_.name.c_str(), tool->path.c_str(), tool->version_text.c_str());
        CML_LOG(Info, "WP50-ONEOFF what=linker-detection module=%s ms=%.1f", module_.name.c_str(), ms_between(detect_started, Clock::now()));
        std::lock_guard<std::mutex> lock(module_.mutex);
        module_.linker = *tool;
        module_.linker_key = key;
        return Result<void>();
    }

    std::optional<std::uint64_t> section_va(const toolchain::LinkedPatch& linked, std::uint32_t section) const {
        if (const toolchain::SectionPlacement* p = linked.find_section(0, section)) {
            return linked.base + p->section_rva;
        }
        return std::nullopt;
    }
    std::optional<std::uint64_t> symbol_va(const toolchain::LinkedPatch& linked, std::uint32_t section, std::uint32_t value) const {
        if (const std::optional<std::uint64_t> s = section_va(linked, section)) {
            return *s + value;
        }
        return std::nullopt;
    }
    std::optional<std::uint64_t> function_va(const bind::BindPlan& plan, const toolchain::LinkedPatch& linked, const std::string& name) const {
        const bind::SymbolDecision* d = plan.find_decision(name);
        if (d == nullptr) {
            return std::nullopt;
        }
        return symbol_va(linked, d->section, d->value);
    }

    Result<void> prepare_fixups(const bind::BindPlan& plan, const toolchain::LinkedPatch& linked, PatchReport& report, PatchFixups* fixups,
                                std::vector<std::uint32_t>* init_rvas) {
        bind::PatchLayout layout;
        layout.patch_base = linked.base;
        for (const toolchain::SectionPlacement& sp : linked.sections) {
            if (sp.object == 0) {
                layout.section_rva[sp.section_index] = sp.section_rva;
            }
        }
        for (const bind::ThunkEntry& t : plan.thunks) {
            const auto it = linked.slot_rvas.find(t.name);
            if (it != linked.slot_rvas.end()) {
                layout.symbol_rva[t.slot] = it->second;
            }
        }
        for (const bind::PlaceholderEntry& p : plan.placeholders) {
            const auto it = linked.data_rvas.find(p.name);
            if (it != linked.data_rvas.end()) {
                layout.symbol_rva[p.name] = it->second;
            }
        }
        Result<std::vector<bind::FixupTriple>> made = bind::make_fixup_triples(plan, layout);
        if (!made) {
            return fail("fixups", made.error());
        }
        Result<PatchFixups> converted = to_patch_fixups(*made, linked.base);
        if (!converted) {
            return fail("fixups", converted.error());
        }
        *fixups = std::move(*converted);
        if (const std::optional<Diag> f = injected(ipc::fault::Stage::PreApply)) {
            return fail("fixups", *f);
        }
        if (Result<void> pre = preapply_write32(linked.dll_path, fixups->pre_applied); !pre) {
            return fail("fixups", pre.error());
        }
        init_rvas->clear();
        for (const bind::InitialiserEntry& e : plan.initialisers) {
            const toolchain::SectionPlacement* p = linked.find_section(0, e.table_section);
            if (p == nullptr) {
                return fail("fixups", Diag::make(bind::diag::kLayoutIncomplete,
                                                 "the initialiser table '" + e.table_section_name + "' holding '" + e.display + "' has no locator in the patch",
                                                 "Report this with the bind plan; the new global's constructor cannot run."));
            }
            init_rvas->push_back(p->section_rva + e.table_offset);
        }
        (void)report;
        return Result<void>();
    }

    // The patch at [base, base + size) was linked but never loaded: its slice of the reserved address space is free again.
    void give_back_range(std::uint64_t base, std::uint32_t size) {
        std::lock_guard<std::mutex> lock(module_.mutex);
        module_.reservation.release_tail(base, size);
    }

    void unload(std::uint64_t base) {
        ipc::ReleaseRange req;
        req.request_id = link_.next_request_id();
        req.base = base;
        req.mode = ipc::ReleaseMode::UnloadPatch;
        Result<ipc::Message> answer = link_.request(std::move(req), clamp_timeout(options_.agent_timeout_ms));
        if (!answer) {
            CML_LOG(Warn, "reload: could not unload the failed patch at %s: %s", hex(base).c_str(), answer.error().format().c_str());
        } else if (const auto* rep = std::get_if<ipc::Reply>(&*answer); rep != nullptr && rep->status != 0) {
            CML_LOG(Warn, "reload: the agent could not unload the failed patch at %s: %s", hex(base).c_str(), rep->message.c_str());
        }
    }

    static bool write_file(const std::string& path, const coff::ByteVector& bytes) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(out);
    }

    const PipelineEnv& env_;
    AgentLink& link_;
    ModuleState& module_;
    const PipelineContext& ctx_;
    const ReloadOptions& options_;
    LinkSnapshot snapshot_;
    std::uint32_t pid_ = 0;
};

}  // namespace

Result<PatchReport> PatchPipeline::patch_module(AgentLink& link, ModuleState& module, const PipelineContext& context) const {
    Run run(env_, link, module, context);
    return run.execute();
}

// ---- WP-54: ModuleReload ----------------------------------------------------------------------------------------------------

struct ModuleReload::Impl {
    Impl(const PipelineEnv& env, AgentLink& link, ModuleState& module, const PipelineContext& context) : run(env, link, module, context) {}
    Run run;
};

ModuleReload::ModuleReload(const PatchPipeline& pipeline, AgentLink& link, ModuleState& module, const PipelineContext& context, bool events_held)
    : impl_(std::make_unique<Impl>(pipeline.env(), link, module, context)) {
    if (events_held) {
        impl_->run.hold_events();
    }
}
ModuleReload::~ModuleReload() = default;
Result<void> ModuleReload::prepare() { return impl_->run.prepare_phase(); }
bool ModuleReload::prepared() const { return impl_->run.prepared(); }
Result<PatchReport> ModuleReload::apply() { return impl_->run.apply_phase(); }
Diag ModuleReload::abandon(Diag why) { return impl_->run.abandon(std::move(why)); }
bool ModuleReload::may_be_live() const { return impl_->run.may_be_live(); }
const std::string& ModuleReload::failed_stage() const { return impl_->run.failed_stage(); }
std::vector<Event> ModuleReload::take_applied_events() { return impl_->run.take_applied_events(); }
ModuleState& ModuleReload::module() const { return impl_->run.module(); }

}  // namespace cml::core
