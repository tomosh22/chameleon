#include "cml/core/pipeline.h"

#include <algorithm>
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
#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_plan.h"
#include "cml/core/patch_image.h"
#include "cml/core/pipeline_diag_ids.h"
#include "cml/toolchain/rewrite.h"
#include "cml/util/hash.h"
#include "cml/util/log.h"
#include "cml/util/path.h"

namespace cml::core {

namespace fs = std::filesystem;
namespace pd = pipeline::diag;
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
    v.set("new_functions", n(r.new_functions));
    v.set("new_globals", n(r.new_globals));
    v.set("thunks", n(r.thunks));
    v.set("fixups", n(r.fixups));
    v.set("pre_applied", n(r.pre_applied));
    v.set("initializers", n(r.initializers));
    v.set("initializers_run", n(r.initializers_run));
    v.set("load_attempts", n(r.load_attempts));
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

std::uint64_t CompileCache::key_of(const toolchain::CompileRecord& record) {
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
    return hash64(text);
}

std::optional<CompileCache::Entry> CompileCache::find(std::uint64_t key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void CompileCache::put(std::uint64_t key, Entry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[key] = std::move(entry);
}

std::size_t CompileCache::compiled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

std::size_t CompileCache::reused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reused_;
}

void CompileCache::note_reuse() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++reused_;
}

namespace {

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

std::string hex(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

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
    std::uint64_t key = 0;
    bool reused = false;                 // the outcome came from the reload's compile cache
    std::optional<Diag> cached_error;    // ... and it was a failure
};

class Run {
public:
    Run(const PipelineEnv& env, AgentLink& link, ModuleState& module, const PipelineContext& ctx)
        : env_(env), link_(link), module_(module), ctx_(ctx), options_(*ctx.options) {}

    Result<PatchReport> execute() {
        const Clock::time_point t_start = Clock::now();
        Result<PatchReport> r = run();
        if (r) {
            r->times.total = ms_between(t_start, Clock::now());
        }
        return r;
    }

private:
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
        return d;
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

    // ---- the run -----------------------------------------------------------------------------------------
    Result<PatchReport> run() {
        PatchReport report;
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
        Result<std::shared_ptr<const pdb::PdbIndex>> pdb_result = module_.index->acquire_pdb();
        if (!pdb_result) {
            return fail("sweep", pdb_result.error());
        }
        const std::shared_ptr<const pdb::PdbIndex> pdb = *pdb_result;
        // ADR-0017: with the option set the broker's mapping of the PDB ends with the reload, whatever its outcome.
        struct ReleaseGuard {
            ModuleIndex& index;
            bool release;
            ~ReleaseGuard() {
                if (release) {
                    index.release_pdb();
                }
            }
        } release_guard{*module_.index, options_.release_pdb_after_reload};
        Result<ModuleIndex::Sweep> swept = module_.index->sweep();
        if (!swept) {
            return fail("sweep", swept.error());
        }
        report.changed_files = swept->changed;
        for (const std::string& f : swept->unmapped) {
            warn(report, "sweep",
                 Diag::make(pd::kChangedFileUnmapped, "the changed file '" + f + "' is not part of any compiland of " + module_.name,
                            "clang-cl PDBs do not list headers that hold no code (ADR-0013). Edit a .cpp file that includes the header; touching a file does not count, only a change of its content does."));
        }
        for (const std::string& f : swept->missing) {
            warn(report, "sweep", Diag::make(pd::kBaselineFile, "the tracked file '" + f + "' is gone", "Restore it or ignore this warning."));
        }
        report.times.sweep = lap.lap();
        CML_LOG(Info, "reload: sweep of %s: %zu files checked, %zu hashed, %zu changed, %zu compilands", module_.name.c_str(), swept->checked, swept->hashed,
                swept->changed.size(), swept->compilands.size());
        if (swept->compilands.empty()) {
            // Nothing to compile. Files that map to no compiland are accepted so they are not reported again.
            module_.index->accept(swept->stamps);
            report.patched = false;
            return report;
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
        std::vector<Unit> units;
        std::vector<toolchain::CompileRequest> requests;
        std::vector<std::size_t> fresh;  // index in `units` of requests[k]
        std::optional<Diag> first_error;
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
                    u.outcome = hit->outcome;
                    u.cached_error = hit->error;
                    u.reused = true;
                    ctx_.compile_cache->note_reuse();
                    ++report.compile_reused;
                    CML_LOG(Debug, "compile of %s: reused the result of an identical command line (%s)", u.source.c_str(), u.outcome.object_path.c_str());
                    units.push_back(std::move(u));
                    continue;
                }
            }
            toolchain::CompileRequest cr;
            cr.record = *record;
            cr.output_object = toolchain::patch_object_path(*workdir + "/obj", module_.name, hash64(u.object_name), record->source);
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
            return *first_error;
        }
        std::vector<toolchain::CompileOutcome> outcomes(requests.size());
        std::vector<Result<void>> results;
        if (!requests.empty()) {
            results = env_.compile_all(requests, outcomes, options_.max_parallel_compiles, ctx_.cancel);
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
        for (Unit& u : units) {
            if (u.cached_error) {
                // Published per module (diag events carry the pid), also when the failure came from the cache.
                const Diag shown = fail("compile", *u.cached_error);
                first_error = first_error ? first_error : std::optional<Diag>(shown);
            }
        }
        report.times.compile = lap.lap();
        if (first_error) {
            return *first_error;
        }

        // ---- 3.. per compiland: one patch each ---------------------------------------------------------------
        std::map<std::uint32_t, bool> unit_ok;
        std::optional<Diag> failure;
        for (Unit& u : units) {
            if (Result<void> t = check_time("plan"); !t) {
                failure = t.error();
                break;
            }
            Result<void> applied = apply_unit(u, *pdb, *workdir, report);
            unit_ok[u.compiland] = applied.has_value();
            if (!applied) {
                failure = applied.error();
                break;
            }
        }

        // Re-baseline the files whose every compiland was patched; the others are processed again next time.
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
        module_.index->accept(accepted);
        if (failure) {
            return *failure;
        }
        report.patched = report.generation != 0;
        return report;
    }

    Result<std::string> make_work_dir() {
        std::error_code ec;
        const std::string dir = ctx_.session_dir + "/" + module_.name;
        fs::create_directories(fs::path(dir) / "obj", ec);
        if (ec) {
            return fail("compile", Diag::make(pd::kWorkDir, "cannot create the session directory '" + dir + "': " + ec.message(),
                                              "Check the TEMP directory (free space, permissions)."));
        }
        return dir;
    }

    // ---- one compiland ------------------------------------------------------------------------------------
    Result<void> apply_unit(Unit& u, const pdb::PdbIndex& pdb, const std::string& workdir, PatchReport& report) {
        Lap lap;
        const std::uint64_t patch_no = [&] {
            std::lock_guard<std::mutex> lock(module_.mutex);
            return ++module_.patch_counter;
        }();
        const std::string tag = "g" + std::to_string(patch_no);

        // ---- plan --------------------------------------------------------------------------------------------
        Result<coff::OwnedObject> object = coff::load_object_file(u.outcome.object_path);
        if (!object) {
            return fail("plan", object.error());
        }
        std::shared_ptr<const pe::OwnedImage> image = module_image(report);
        FileProcessModules process_modules(snapshot_.modules, LoadedModule{module_.path, module_.base, module_.size}, ctx_.exports, image);
        bind::PlanInput in;
        in.object = &object->object;
        in.object_name = std::string(path::base_name(std::string_view(u.outcome.object_path)));
        in.module_pdb = &pdb;
        in.module_name = module_.name;
        in.module_base = module_.base;
        in.compiland_index = u.compiland;
        in.registry = &module_.registry;
        in.modules = &process_modules;
        std::vector<Diag> errors;
        Result<bind::BindPlan> planned = bind::plan_bind(in, &errors);
        for (const Diag& skipped : process_modules.skipped()) {
            warn(report, "plan", skipped);
        }
        if (!planned) {
            std::optional<Diag> first;
            for (const Diag& e : errors) {
                const Diag shown = fail("plan", e);
                first = first ? first : std::optional<Diag>(shown);
            }
            if (!first) {
                first = fail("plan", planned.error());
            }
            return *first;
        }
        bind::BindPlan plan = std::move(*planned);
        for (const Diag& w : plan.warnings) {
            warn(report, "plan", w);
        }
        NarrowInput ni;
        ni.object = &object->object;
        ni.plan = &plan;
        ni.old_image = image ? &image->image : nullptr;
        ni.load_base = module_.base;
        ni.is_hooked = [this](std::uint64_t va) { return module_.is_hooked(va); };
        ni.enabled = options_.narrow_hooks;
        const NarrowResult narrowed = narrow_hooks(ni);
        report.hooks_planned += plan.hooks.size();
        report.hooks_dropped += narrowed.dropped;
        report.new_functions += plan.new_functions.size();
        report.new_globals += plan.new_globals.size();
        report.thunks += plan.thunks.size();
        for (const NarrowDecision& d : narrowed.decisions) {
            CML_LOG(Debug, "reload: hook %s: %s%s%s", d.display.c_str(), to_string(d.reason), d.detail.empty() ? "" : " - ", d.detail.c_str());
        }
        std::vector<bind::HookEntry> hooks;
        {
            std::set<std::uint64_t> seen;
            for (const bind::HookEntry& h : narrowed.hooks) {
                if (seen.insert(h.old_va).second) {
                    hooks.push_back(h);
                }
            }
        }
        const bool has_effect = !hooks.empty() || !plan.new_functions.empty() || !plan.new_globals.empty() || !plan.initialisers.empty();
        report.times.plan += lap.lap();
        if (!has_effect) {
            CML_LOG(Info, "reload: %s changes no code (all %zu planned hooks are identical to the running code); nothing to load", u.source.c_str(),
                    plan.hooks.size());
            return Result<void>();
        }

        // ---- anchor + binding object -----------------------------------------------------------------------
        const std::string patch_dir = workdir + "/p" + std::to_string(pid_) + "_" + tag;
        std::error_code ec;
        fs::create_directories(patch_dir, ec);
        if (ec) {
            return fail("anchor", Diag::make(pd::kWorkDir, "cannot create '" + patch_dir + "': " + ec.message(), "Check the TEMP directory."));
        }
        Result<coff::ObjectModel> model = coff::ObjectModel::from_object(object->object);
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
        Result<coff::ByteVector> anchored = coff::write(*model);
        if (!anchored) {
            return fail("anchor", anchored.error());
        }
        const std::string anchored_path = patch_dir + "/" + std::string(path::base_name(std::string_view(u.source))) + ".anchored.obj";
        if (!write_file(anchored_path, *anchored)) {
            return fail("anchor", Diag::make(pd::kWorkDir, "cannot write '" + anchored_path + "'", "Check free disk space and the TEMP directory."));
        }
        std::vector<coff::BindSymbol> binds = plan.binding_symbols();
        // `/INCLUDE:sym` linker directives (cl writes `/INCLUDE:_fltused` into every object that uses floating
        // point) are no relocations, so the planner never sees them; with /NODEFAULTLIB nothing defines the symbol.
        // The patch gets an unused placeholder so the link resolves it.
        // Undefined externals nothing relocates against (the same `_fltused` as an undefined symbol the /INCLUDE
        // directive forces) are invisible to the planner too.
        for (const coff::Symbol& sym : object->object.symbols()) {
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
        for (const coff::Directive& dir : object->object.directives()) {
            if (dir.kind != coff::DirectiveKind::Include || dir.value.empty()) {
                continue;
            }
            const coff::Symbol* defined = object->object.find_symbol(dir.value);
            const bool in_object = defined != nullptr && !defined->is_undefined();
            const bool bound = std::any_of(binds.begin(), binds.end(), [&](const coff::BindSymbol& b) { return b.name == dir.value; });
            if (!in_object && !bound) {
                CML_LOG(Debug, "reload: /INCLUDE:%s has no definition in the patch: placeholder", dir.value.c_str());
                binds.push_back(coff::BindSymbol{dir.value, coff::BindKind::Data});
            }
        }
        std::string binding_path;
        if (!binds.empty()) {
            Result<coff::ByteVector> binding = coff::make_binding_object(binds);
            if (!binding) {
                return fail("anchor", binding.error());
            }
            binding_path = patch_dir + "/binding.obj";
            if (!write_file(binding_path, *binding)) {
                return fail("anchor", Diag::make(pd::kWorkDir, "cannot write '" + binding_path + "'", "Check free disk space and the TEMP directory."));
            }
        }
        report.times.anchor += lap.lap();

        // ---- reserve address space (once per module) -----------------------------------------------------
        if (Result<void> r = ensure_reserved(); !r) {
            return r;
        }

        // ---- link, load (relink at another base when the range was taken) ------------------------------------
        if (Result<void> t = check_time("link"); !t) {
            return t;
        }
        if (Result<void> r = ensure_linker(u, report); !r) {
            return r;
        }
        toolchain::LinkedPatch linked;
        PatchFixups fixups;
        std::vector<std::uint32_t> init_rvas;
        std::vector<bind::FixupTriple> triples;
        bool loaded = false;
        const unsigned attempts_allowed = std::max(1u, options_.max_load_attempts);
        for (unsigned attempt = 1; attempt <= attempts_allowed && !loaded; ++attempt) {
            ++report.load_attempts;
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
            lr.objects.push_back(toolchain::PatchObjectInput{anchored_path, *locators});
            lr.binding_object = binding_path;
            lr.bindings = binds;
            lr.out_dir = patch_dir;
            lr.name = "patch_" + std::to_string(patch_no) + "_" + std::to_string(attempt);
            lr.base = base;
            lr.timeout_ms = static_cast<unsigned>(std::max<std::int64_t>(1000, remaining_ms()));
            lr.cancel = ctx_.cancel;
            if (!env_.link_patch) {
                return fail("link", Diag::make(pd::kStageFailed, "this broker has no linker driver", "Run the broker on Windows."));
            }
            toolchain::LinkTrace trace;
            Result<toolchain::LinkedPatch> lp = env_.link_patch(lr, &trace);
            if (!lp) {
                CML_LOG(Error, "link failed:\n%s", trace.output.c_str());
                return fail("link", lp.error());
            }
            linked = std::move(*lp);
            for (const Diag& w : linked.warnings) {
                warn(report, "link", w);
            }
            report.times.link += lap.lap();
            {
                std::lock_guard<std::mutex> lock(module_.mutex);
                module_.reservation.consume(base, linked.image_size);
            }

            // ---- fix-ups (broker side) ---------------------------------------------------------------------
            Result<void> prepared = prepare_fixups(plan, linked, report, &fixups, &init_rvas, &triples);
            report.times.fixups += lap.lap();
            if (!prepared) {
                return prepared;
            }

            // ---- load ------------------------------------------------------------------------------------------
            if (Result<void> t = check_time("load"); !t) {
                return t;
            }
            ipc::LoadPatch load;
            load.request_id = link_.next_request_id();
            load.patch_id = patch_no;
            ipc::PatchCandidate candidate;
            candidate.path = linked.dll_path;
            candidate.image_base = linked.base;
            candidate.image_size = linked.image_size;
            load.candidates.push_back(std::move(candidate));
            Result<ipc::Message> answer = ask(std::move(load), options_.agent_timeout_ms, "load");
            if (!answer) {
                if (answer.error().code == pd::kAgentRequestTimeout) {
                    // The agent works through requests in order, so an unload sent now runs after a LoadPatch that is
                    // still queued: the late load does not leave an unused patch behind.
                    unload(linked.base);
                }
                return answer.error();
            }
            report.times.load += lap.lap();
            std::uint32_t status = 0;
            std::string message;
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
            if (!loaded) {
                if ((status == 4203 || status == 4205) && attempt < attempts_allowed) {
                    CML_LOG(Warn, "reload: the address range at %s was taken (CML%u); relinking at another base", hex(linked.base).c_str(), status);
                    continue;
                }
                return fail("load", agent_refusal("LoadPatch", status, message, pd::kLoadFailed));
            }
        }

        // From here on a failure unloads the patch again.
        const std::uint64_t base = linked.base;
        const auto abandon = [&] { unload(base); };

        // ---- apply the fix-ups ------------------------------------------------------------------------------
        {
            std::size_t sent = 0;
            while (sent < fixups.wire.size()) {
                const std::size_t n = std::min<std::size_t>(ipc::kMaxFixupsPerMessage, fixups.wire.size() - sent);
                ipc::ApplyFixups req;
                req.request_id = link_.next_request_id();
                req.image_base = base;
                req.fixups.assign(fixups.wire.begin() + static_cast<std::ptrdiff_t>(sent), fixups.wire.begin() + static_cast<std::ptrdiff_t>(sent + n));
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
            report.fixups += fixups.wire.size();
            report.pre_applied += fixups.pre_applied.size();
            report.times.fixups += lap.lap();
        }

        // ---- initialisers of the new globals ------------------------------------------------------------------
        report.initializers += init_rvas.size();
        if (!init_rvas.empty()) {
            ipc::RunInitializers req;
            req.request_id = link_.next_request_id();
            req.image_base = base;
            req.rvas = init_rvas;
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
            report.initializers_run += init_rvas.size();
        }
        report.times.init += lap.lap();

        // ---- hooks -------------------------------------------------------------------------------------------------
        std::vector<std::pair<std::uint64_t, std::uint64_t>> installed;
        if (!hooks.empty()) {
            ipc::InstallHooks req;
            req.request_id = link_.next_request_id();
            for (const bind::HookEntry& h : hooks) {
                const std::optional<std::uint64_t> target = function_va(plan, linked, h.new_symbol);
                if (!target) {
                    abandon();
                    return fail("hooks", Diag::make(bind::diag::kLayoutIncomplete, "the patch layout has no address for the hooked function '" + h.display + "'",
                                                    "Report this with the bind plan."));
                }
                req.hooks.push_back(ipc::HookSpec{h.old_va, *target, h.display});
                installed.emplace_back(h.old_va, *target);
            }
            Result<ipc::Message> answer = link_.request(ipc::Message(std::move(req)), clamp_timeout(options_.hooks_timeout_ms));
            if (!answer) {
                // No answer: the agent may still apply the hooks (synchronised mode waits for SyncPoint). The
                // patch must then stay loaded; remember the hooks as installed so the next reload re-points them.
                const bool timed_out = answer.error().code == pd::kAgentRequestTimeout;  // a send failure or a lost agent is a plain failure
                if (timed_out) {
                    std::lock_guard<std::mutex> lock(module_.mutex);
                    for (const auto& h : installed) {
                        module_.hooked[h.first] = h.second;
                    }
                    return fail("hooks", Diag::make(pd::kHooksTimeout, "the agent did not confirm the hooks in time: " + answer.error().message,
                                                    "In synchronised mode the application must call SyncPoint(); the patch stays loaded and the hooks may still apply."));
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
            if (done->status != 0) {
                abandon();
                return fail("hooks", agent_refusal("InstallHooks", done->status, done->message, pd::kHooksFailed));
            }
            CML_LOG(Info, "reload: %zu hooks installed (threads stopped: %u, window %u ms, attempts %u, retries %u)", hooks.size(), done->threads, done->window_ms,
                    done->attempts, done->retries);
            report.hooks_installed += hooks.size();
        }
        report.times.hooks += lap.lap();

        // ---- commit: the process symbol registry and the hook table (plan step 9) -----------------------------------
        {
            std::lock_guard<std::mutex> lock(module_.mutex);
            for (const auto& h : installed) {
                module_.hooked[h.first] = h.second;
            }
            ++module_.generation;
            report.generation = module_.generation;
            ++report.patches;
        }
        for (const bind::NewFunction& f : plan.new_functions) {
            if (const std::optional<std::uint64_t> va = symbol_va(plan, linked, f.name, f.section, f.value)) {
                SymbolAddress a;
                a.va = *va;
                a.is_function = true;
                a.origin = "patch " + std::to_string(patch_no);
                module_.registry.add(f.name, std::move(a));
            } else {
                warn(report, "hooks", Diag::make(bind::diag::kLayoutIncomplete, "the new function '" + f.display + "' has no address in the patch layout; later patches cannot bind to it",
                                                 "Report this with the bind plan."));
            }
        }
        for (const bind::NewGlobal& g : plan.new_globals) {
            if (const std::optional<std::uint64_t> va = symbol_va(plan, linked, g.name, g.section, g.value)) {
                SymbolAddress a;
                a.va = *va;
                a.is_function = false;
                a.origin = "patch " + std::to_string(patch_no);
                module_.registry.add(g.name, std::move(a));
            } else {
                warn(report, "hooks", Diag::make(bind::diag::kLayoutIncomplete, "the new global '" + g.display + "' has no address in the patch layout; later patches cannot bind to it",
                                                 "Report this with the bind plan."));
            }
        }
        report.dll_path = linked.dll_path;
        report.patched = true;
        if (ctx_.events != nullptr) {
            ctx_.events->publish(make_patch_applied_event(pid_, module_.name, report.generation, hooks.size(), plan.new_functions.size(), plan.new_globals.size()));
        }
        CML_LOG(Info, "reload: patch %llu of %s applied at %s (%zu hooks, %zu dropped, %zu new functions, %zu new globals, %zu fix-ups, %zu initialisers)",
                static_cast<unsigned long long>(report.generation), module_.name.c_str(), hex(base).c_str(), hooks.size(), narrowed.dropped, plan.new_functions.size(),
                plan.new_globals.size(), fixups.wire.size(), init_rvas.size());
        return Result<void>();
    }

    // ---- pieces -----------------------------------------------------------------------------------------------
    static constexpr std::uint64_t kMinPatchRoom = 4ull << 20;  // a patch needs room for at least this much image

    std::shared_ptr<const pe::OwnedImage> module_image(PatchReport& report) {
        std::lock_guard<std::mutex> lock(module_.mutex);
        if (!module_.image) {
            Result<pe::OwnedImage> loaded = pe::load_image_file(module_.path);
            if (!loaded) {
                // Not fatal: without the old bytes every planned hook is kept.
                const Diag d = Diag::make(pd::kModuleImage, "cannot read the module file '" + module_.path + "' to compare function bodies: " + loaded.error().message,
                                          "All functions of the patched object are hooked this time.");
                emit(d, "warning", "plan");
                report.warnings.push_back(d);
                return nullptr;
            }
            module_.image = std::make_shared<const pe::OwnedImage>(std::move(*loaded));
        }
        return module_.image;
    }

    Result<void> ensure_reserved() {
        {
            std::lock_guard<std::mutex> lock(module_.mutex);
            if (module_.reservation.reserved()) {
                return Result<void>();
            }
        }
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
        std::lock_guard<std::mutex> lock(module_.mutex);
        module_.reservation.set_blocks(rr->blocks);
        return Result<void>();
    }

    Result<void> ensure_linker(const Unit& u, PatchReport& report) {
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
        std::vector<std::string> dirs;
        if (!u.outcome.compiler_used.empty()) {
            dirs.push_back(std::string(path::parent_directory(std::string_view(u.outcome.compiler_used))));
        }
        std::vector<Diag> warnings;
        Result<toolchain::ToolInfo> tool = env_.resolve_linker(module_.path, dirs, options_, &warnings);
        for (const Diag& w : warnings) {
            warn(report, "link", w);
        }
        if (!tool) {
            return fail("link", tool.error());
        }
        CML_LOG(Info, "reload: linker for %s: %s %s", module_.name.c_str(), tool->path.c_str(), tool->version_text.c_str());
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
    std::optional<std::uint64_t> symbol_va(const bind::BindPlan&, const toolchain::LinkedPatch& linked, const std::string&, std::uint32_t section,
                                           std::uint32_t value) const {
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
        return symbol_va(plan, linked, name, d->section, d->value);
    }

    Result<void> prepare_fixups(const bind::BindPlan& plan, const toolchain::LinkedPatch& linked, PatchReport& report, PatchFixups* fixups,
                                std::vector<std::uint32_t>* init_rvas, std::vector<bind::FixupTriple>* triples) {
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
        *triples = *made;
        Result<PatchFixups> converted = to_patch_fixups(*triples, linked.base);
        if (!converted) {
            return fail("fixups", converted.error());
        }
        *fixups = std::move(*converted);
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

Result<PatchReport> PatchPipeline::patch_module(AgentLink& link, ModuleState& module, const PipelineContext& context) {
    Run run(env_, link, module, context);
    return run.execute();
}

}  // namespace cml::core
