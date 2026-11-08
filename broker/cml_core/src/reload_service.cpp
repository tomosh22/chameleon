#include "cml/core/reload_service.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <thread>

#include "cml/core/control_args.h"
#include "cml/core/header_diag_ids.h"
#include "cml/core/pipeline_diag_ids.h"
#include "cml/core/process_registry.h"
#include "cml/core/restart.h"
#include "cml/core/restart_diag_ids.h"
#include "cml/core/revert.h"
#include "cml/core/revert_diag_ids.h"
#include "cml/util/log.h"
#include "cml/util/path.h"
#include "cml/watch/watch_diag_ids.h"

namespace cml::core {

namespace fs = std::filesystem;
namespace pd = pipeline::diag;
using Clock = std::chrono::steady_clock;
using ipc::json::Value;

namespace {

Diag bad_request(const std::string& message) {
    return Diag::make(pd::kBadRequest, message, "Check the command's arguments (docs/wp/WP-12.md, \"Control commands\").");
}

// Reads an optional unsigned integer argument; false (and *error set) when present with a wrong type or range.
bool read_uint(const Value& args, const char* key, std::uint32_t max, std::uint32_t* out, std::optional<Diag>* error) {
    return read_uint_arg(args, key, max, out, error, bad_request);
}

std::string normalized_key(const std::string& p) { return normalized_path_key(p); }

// Value of an environment variable ("" when unset); _dupenv_s on Windows (getenv is deprecated there, /WX).
std::string env_value(const char* name) {
#if defined(_WIN32)
    char* buffer = nullptr;
    std::size_t size = 0;
    std::string out;
    if (_dupenv_s(&buffer, &size, name) == 0 && buffer != nullptr) {
        out = buffer;
        std::free(buffer);
    }
    return out;
#else
    const char* v = std::getenv(name);
    return v != nullptr ? v : "";
#endif
}

}  // namespace

ReloadService::ReloadService(PipelineEnv env, EventBus* events, std::string session_dir)
    : pipeline_(std::move(env)), events_(events), session_dir_(std::move(session_dir)) {
    // A directory the user chose (--session-dir, $CML_SESSION_DIR) is theirs: it is never removed. Only the
    // temporary default is.
    keep_session_dir_ = !session_dir_.empty();
    set_process_host(native_process_host());  // WP-21: null on hosts that cannot restart processes
}

ReloadService::~ReloadService() {
    cancel_.cancel();
    {
        std::shared_ptr<SaveWatcher> watcher;
        {
            std::lock_guard<std::mutex> lock(watch_mutex_);
            watcher.swap(save_watcher_);
        }
        if (watcher) {
            watcher->stop();
        }
        watcher.reset();  // joins its thread (a running reload was cancelled above)
        std::vector<std::future<void>> retiring;
        {
            std::lock_guard<std::mutex> lock(watch_mutex_);
            retiring.swap(retiring_);
        }
        for (std::future<void>& f : retiring) {
            f.wait();
        }
    }
    std::map<std::uint64_t, AgentEntry> agents;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        agents.swap(agents_);
    }
    agents.clear();  // joins the index threads outside the lock
    if (session_dir_made_ && !keep_session_dir_ && env_value("CML_KEEP_SESSION").empty()) {
        std::error_code ec;
        fs::remove_all(fs::path(session_dir_), ec);  // patch DLLs still loaded in a process cannot be removed: ignored
    }
}

Result<std::string> ReloadService::ensure_session_dir() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_dir_made_) {
        return session_dir_;
    }
    std::error_code ec;
    if (session_dir_.empty()) {
        if (const std::string base = env_value("CML_SESSION_DIR"); !base.empty()) {
            session_dir_ = base;
            keep_session_dir_ = true;  // the user asked for a place to look at: leave it
        } else {
            const auto stamp = static_cast<unsigned long long>(Clock::now().time_since_epoch().count());
            session_dir_ = (fs::temp_directory_path(ec) / ("chameleon-" + std::to_string(stamp % 1000000007ull))).string();
        }
    }
    fs::create_directories(fs::path(session_dir_), ec);
    if (ec) {
        return Diag::make(pd::kWorkDir, "cannot create the session directory '" + session_dir_ + "': " + ec.message(), "Check the TEMP directory (free space, permissions).");
    }
    session_dir_made_ = true;
    CML_LOG(Info, "session directory: %s", session_dir_.c_str());
    return session_dir_;
}

// ---- connections ---------------------------------------------------------------------------------------

void ReloadService::attach(std::shared_ptr<AgentLink> link) {
    const std::uint64_t id = link->agent_id();
    std::lock_guard<std::mutex> lock(mutex_);
    agents_[id].link = std::move(link);
}

void ReloadService::detach(std::uint64_t agent_id) {
    AgentEntry entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = agents_.find(agent_id);
        if (it == agents_.end()) {
            return;
        }
        entry = std::move(it->second);
        agents_.erase(it);
    }
    entry.modules.clear();  // index threads are joined here, outside the lock
    rearm_save_watcher();
}

void ReloadService::on_session_changed(std::uint64_t agent_id) {
    std::shared_ptr<AgentLink> link;
    ReloadOptions options;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = agents_.find(agent_id);
        if (it == agents_.end()) {
            return;
        }
        link = it->second.link;
        options = options_;
    }
    const LinkSnapshot snap = link->snapshot();
    // The modules that should have state: enabled and loaded, with the address the agent reports.
    std::map<std::string, LoadedModule> desired;
    for (const std::string& enabled : snap.enabled) {
        for (const LoadedModule& m : snap.modules) {
            if (path::same_path(m.path, enabled)) {
                desired[normalized_key(m.path)] = m;
                break;
            }
        }
    }
    std::vector<std::shared_ptr<ModuleState>> dropped;
    std::vector<std::pair<std::string, LoadedModule>> created;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = agents_.find(agent_id);
        if (it == agents_.end()) {
            return;
        }
        AgentEntry& entry = it->second;
        for (auto m = entry.modules.begin(); m != entry.modules.end();) {
            const auto want = desired.find(m->first);
            if (want == desired.end() || want->second.base != m->second->base) {
                dropped.push_back(std::move(m->second));  // disabled, unloaded, or reloaded at another address
                m = entry.modules.erase(m);
            } else {
                ++m;
            }
        }
        for (const auto& want : desired) {
            if (entry.modules.count(want.first) == 0) {
                auto state = std::make_shared<ModuleState>();
                state->path = want.second.path;
                state->name = std::string(path::base_name(std::string_view(want.second.path)));
                state->base = want.second.base;
                state->size = want.second.size;
                entry.modules[want.first] = state;
                created.emplace_back(want.first, want.second);
            }
        }
    }
    const bool any_dropped = !dropped.empty();
    dropped.clear();  // joins their threads
    if (any_dropped) {
        rearm_save_watcher();
    }
    for (const auto& c : created) {
        std::shared_ptr<ModuleState> state;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = agents_.find(agent_id);
            if (it == agents_.end()) {
                return;
            }
            const auto m = it->second.modules.find(c.first);
            if (m == it->second.modules.end()) {
                continue;
            }
            state = m->second;
        }
        ModuleIndexSpec spec;
        spec.module_path = state->path;
        spec.baseline.track_system = options.track_system_headers;
        const std::string name = state->name;
        spec.on_change = [this, name](const IndexStatus& s) {
            if (s.state == IndexState::Ready) {
                rearm_save_watcher();  // its tracked files are known now (compile-on-save)
            }
            if (s.state == IndexState::Failed) {
                CML_LOG(Warn, "index of %s failed: %s", name.c_str(), s.error ? s.error->format().c_str() : "unknown error");
            } else {
                CML_LOG(Debug, "index of %s: %s %u%% (%s)", name.c_str(), to_string(s.state), s.percent, s.phase.c_str());
            }
        };
        // WP-18: the build requirements (the same audit as `cml_broker check`). Warnings and failures are published as
        // diag events (stage "enable"); a failure also fails the index, so a reload names it.
        spec.audit = true;
        if (events_ != nullptr) {
            EventBus* bus = events_;
            const std::uint32_t pid = snap.pid;
            spec.on_audit = [bus, pid](const ModuleAudit& audit) {
                for (const AuditFinding& f : audit.findings) {
                    if (f.verdict != Verdict::Pass) {
                        bus->publish(make_diag_event(f.diag(), f.verdict == Verdict::Fail ? "error" : "warning", "enable", pid));
                    }
                }
            };
        }
        CML_LOG(Info, "module %s enabled in process %u: loading its PDB index", state->name.c_str(), snap.pid);
        {
            std::shared_ptr<ModuleIndex> index = ModuleIndex::start(std::move(spec));
            std::lock_guard<std::mutex> state_lock(state->mutex);  // tracked_files() reads it from the watcher's threads
            state->index = std::move(index);
        }
        rearm_save_watcher();  // the index may have become ready before it was stored
    }
}

std::optional<IndexStatus> ReloadService::index_status(std::uint64_t agent_id, const std::string& module_path) const {
    std::shared_ptr<ModuleState> state = module_state(agent_id, module_path);
    if (!state || !state->index) {
        return std::nullopt;
    }
    return state->index->status();
}

std::shared_ptr<ModuleState> ReloadService::module_state(std::uint64_t agent_id, const std::string& module_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = agents_.find(agent_id);
    if (it == agents_.end()) {
        return nullptr;
    }
    const auto m = it->second.modules.find(normalized_key(module_path));
    return m == it->second.modules.end() ? nullptr : m->second;
}

// ---- hot-restart (WP-21) --------------------------------------------------------------------------------------

void ReloadService::set_process_host(std::shared_ptr<ProcessHost> host) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!host) {
        restart_.reset();
        return;
    }
    restart_ = std::make_shared<RestartCoordinator>(*this, events_, std::move(host));
}

void ReloadService::set_restart_trace_for_testing(std::function<void(const std::string&)> trace) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (restart_) {
        restart_->set_trace_for_testing(std::move(trace));
    }
}

bool ReloadService::restart_available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return restart_ != nullptr;
}

Result<Value> ReloadService::restart(const Value& args) {
    std::shared_ptr<RestartCoordinator> coordinator;  // held for the whole call: set_process_host may replace it meanwhile
    {
        std::lock_guard<std::mutex> lock(mutex_);
        coordinator = restart_;
    }
    if (coordinator == nullptr) {
        return Diag::make(restart::diag::kUnavailable, "this broker cannot restart processes on this system",
                          "Restart the application yourself; hot-restart needs the Windows broker.");
    }
    return coordinator->restart(args);
}

std::vector<std::shared_ptr<AgentLink>> ReloadService::links() const {
    std::vector<std::shared_ptr<AgentLink>> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& agent : agents_) {
        if (agent.second.link) {
            out.push_back(agent.second.link);
        }
    }
    return out;
}

std::shared_ptr<AgentLink> ReloadService::link_for_pid(std::uint32_t pid) const {
    for (const std::shared_ptr<AgentLink>& link : links()) {
        const LinkSnapshot snap = link->snapshot();
        if (snap.handshaken && snap.pid == pid && !link->closed()) {
            return link;
        }
    }
    return nullptr;
}

std::shared_ptr<AgentLink> ReloadService::link_for_restart_token(const std::string& token, std::uint32_t pid) const {
    if (token.empty()) {
        return nullptr;
    }
    for (const std::shared_ptr<AgentLink>& link : links()) {
        const LinkSnapshot snap = link->snapshot();
        if (snap.handshaken && snap.restart_token == token && snap.pid == pid && !link->closed()) {
            return link;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<ModuleState>> ReloadService::module_states(std::uint64_t agent_id) const {
    std::vector<std::shared_ptr<ModuleState>> out;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = agents_.find(agent_id);
    if (it != agents_.end()) {
        for (const auto& m : it->second.modules) {
            out.push_back(m.second);
        }
    }
    return out;
}

ReloadOptions ReloadService::options() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return options_;
}

std::vector<ReloadService::Target> ReloadService::targets(std::uint32_t pid, const std::string& module) const {
    std::vector<Target> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& agent : agents_) {
        if (!agent.second.link) {
            continue;
        }
        if (pid != 0 && agent.second.link->snapshot().pid != pid) {
            continue;
        }
        for (const auto& m : agent.second.modules) {
            if (module.empty() || path::matches_module(module, m.second->path)) {
                out.push_back(Target{agent.second.link, m.second});
            }
        }
    }
    return out;
}

// ---- reload -------------------------------------------------------------------------------------------------

Result<Value> ReloadService::reload(const Value& args) {
    return run_reload(args, false);
}

std::unique_ptr<ExclusiveRun> ReloadService::lock_reloads() {
    auto run = std::make_unique<ExclusiveRun>(reload_mutex_, running_);
    if (!run->acquired()) {
        return nullptr;
    }
    return run;
}

Result<Value> ReloadService::reload_locked(const Value& args) {
    return run_reload(args, false, true);
}

Result<Value> ReloadService::run_reload(const Value& args, bool automatic, bool lock_held) {
    std::uint32_t pid = 0;
    std::uint32_t timeout_ms = 60000;
    std::optional<Diag> error;
    if (!read_uint(args, "pid", 0xFFFFFFFFu, &pid, &error) || !read_uint(args, "timeout_ms", 3600u * 1000u, &timeout_ms, &error)) {
        return *error;
    }
    const std::string module = args.get_string("module");
    if (const Value* m = args.find("module"); m != nullptr && !m->is_null() && !m->is_string()) {
        return bad_request("argument 'module' must be a string");
    }
    if (timeout_ms == 0) {
        timeout_ms = 60000;
    }
    bool confirm = false;
    if (const Value* c = args.find("confirm"); c != nullptr && !c->is_null()) {
        if (!c->is_bool()) {
            return bad_request("argument 'confirm' must be a boolean");
        }
        confirm = c->as_bool();
    }

    std::optional<ExclusiveRun> exclusive;  // WP-21: a restart holds the run lock itself (lock_reloads) and calls reload_locked()
    if (!lock_held) {
        exclusive.emplace(reload_mutex_, running_);
    }
    if (!lock_held && !exclusive->acquired()) {
        const Diag d = Diag::make(pd::kBusy, "another reload is still running", "Wait for it to finish (control command `wait`) and try again.");
        if (automatic) {
            return d;  // the caller retries quietly
        }
        if (events_ != nullptr) {
            events_->publish(make_diag_event(d, "error", "reload"));
        }
        return d;
    }

    const Clock::time_point start = Clock::now();
    std::vector<Target> list = targets(pid, module);
    if (automatic) {
        // A save while a module is still indexing (or its index failed) is not a failed reload: that module is
        // skipped here, and picked up when its index becomes ready (the watcher is re-armed then).
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [](const Target& t) {
                                      std::shared_ptr<ModuleIndex> index;
                                      {
                                          std::lock_guard<std::mutex> lock(t.module->mutex);
                                          index = t.module->index;
                                      }
                                      return !index || index->status().state != IndexState::Ready;
                                  }),
                   list.end());
        if (list.empty()) {
            CML_LOG(Debug, "compile-on-save: no module with a ready index; nothing to reload");
            Value none = Value::object();
            none.set("patched", Value::integer(0));
            return none;
        }
    }
    if (list.empty()) {
        const Diag d = Diag::make(pd::kNoTarget,
                                  std::string("no enabled module to reload") + (pid != 0 ? " in process " + std::to_string(pid) : "") +
                                      (module.empty() ? "" : " matching '" + module + "'"),
                                  "Enable a module with cml::Agent::EnableModule (see `status`), then reload.");
        CML_LOG(Warn, "reload: %s", d.format().c_str());
        if (events_ != nullptr) {
            events_->publish(make_diag_event(d, "error", "reload"));
        }
        return d;
    }
    Result<std::string> dir = ensure_session_dir();
    if (!dir) {
        if (events_ != nullptr) {
            events_->publish(make_diag_event(dir.error(), "error", "reload"));
        }
        return dir.error();
    }
    const ReloadOptions opts = options();
    PipelineContext ctx;
    ctx.session_dir = *dir;
    ctx.options = &opts;
    ctx.events = events_;
    ctx.exports = exports_;
    ctx.deadline = start + std::chrono::milliseconds(timeout_ms);
    ctx.cancel = &cancel_;
    ctx.confirm = confirm;
    ctx.reload_id = ++reload_counter_;  // WP-22: `revert last` undoes the batches of one reload id
    // WP-16: one compile per distinct command line for the whole reload; every process still links and loads its own patch.
    ctx.compile_cache = std::make_shared<CompileCache>();

    std::vector<PatchReport> reports;
    std::optional<Diag> first_error;
    std::size_t failed = 0;
    StageTimes sum;
    for (const Target& t : list) {
        const auto module_generation = [&] {
            std::lock_guard<std::mutex> lock(t.module->mutex);
            return t.module->generation;
        };
        const std::uint64_t generation_before = module_generation();
        Result<PatchReport> r = pipeline_.patch_module(*t.link, *t.module, ctx);
        // Patches that were applied count (and have published their patch_applied events) even when a later
        // compiland of the same reload failed: the broker-wide generation follows the module's own.
        generation_.fetch_add(module_generation() - generation_before);
        if (!r) {
            ++failed;
            keep_session_dir_ = true;  // objects, DLLs and PDBs of a failed reload are evidence
            if (!first_error) {
                first_error = r.error();
            }
            continue;
        }
        sum.add(r->times);
        if (r->patched) {
            const std::string stem(path::base_name(std::string_view(t.module->name)));
            const std::size_t dot = stem.find_last_of('.');
            CML_LOG(Info, "%s", timing_line(dot == std::string::npos ? stem : stem.substr(0, dot), r->times).c_str());
        }
        reports.push_back(std::move(*r));
    }
    const double wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    if (first_error) {
        Diag d = *first_error;
        if (failed > 1 || reports.size() > 0) {
            d.message += " (" + std::to_string(failed) + " of " + std::to_string(list.size()) + " modules failed)";
        }
        return d;
    }

    Value result = Value::object();
    result.set("generation", Value::integer(static_cast<std::int64_t>(generation_.load())));
    std::size_t hooks = 0, run = 0, patched = 0, compiled = 0, reused = 0, attempts = 0, modules_patched = 0;
    std::size_t hooks_planned = 0, hooks_dropped = 0, modules_unchanged = 0;  // WP-25
    std::set<std::uint32_t> patched_pids;
    Value patches = Value::array();
    Value changed = Value::array();
    for (const PatchReport& p : reports) {
        hooks += p.hooks_installed;
        hooks_planned += p.hooks_planned;
        hooks_dropped += p.hooks_dropped;
        modules_unchanged += p.no_functional_change ? 1 : 0;
        run += p.initializers_run;
        patched += p.patches;
        compiled += p.compiled;
        reused += p.compile_reused;
        attempts += p.load_attempts;
        if (p.patches != 0) {
            ++modules_patched;
            patched_pids.insert(p.pid);
        }
        for (const std::string& f : p.changed_files) {
            changed.push(Value::string(f));
        }
        patches.push(to_json(p));
    }
    result.set("patched", Value::integer(static_cast<std::int64_t>(patched)));
    result.set("hooks_installed", Value::integer(static_cast<std::int64_t>(hooks)));
    // WP-25: "N functions changed" = hooks_planned - hooks_dropped (hooks_installed is what the agent installed).
    // `no_functional_change`: the edit compiled to identical code in every module that was recompiled and nothing was
    // loaded (CML3450 is in `warnings` and was published as a diag event).
    result.set("hooks_planned", Value::integer(static_cast<std::int64_t>(hooks_planned)));
    result.set("hooks_dropped", Value::integer(static_cast<std::int64_t>(hooks_dropped)));
    result.set("modules_unchanged", Value::integer(static_cast<std::int64_t>(modules_unchanged)));
    result.set("no_functional_change", Value::boolean(modules_unchanged != 0 && patched == 0));
    // WP-16: compiler runs of this reload, results taken from another module's identical command line, and the
    // modules / processes that received at least one patch.
    result.set("compiled", Value::integer(static_cast<std::int64_t>(compiled)));
    result.set("compile_reused", Value::integer(static_cast<std::int64_t>(reused)));
    result.set("load_attempts", Value::integer(static_cast<std::int64_t>(attempts)));  // LoadPatch requests; more than `patched` = relinks at another base
    result.set("modules_patched", Value::integer(static_cast<std::int64_t>(modules_patched)));
    result.set("processes_patched", Value::integer(static_cast<std::int64_t>(patched_pids.size())));
    result.set("initializers_run", Value::integer(static_cast<std::int64_t>(run)));
    result.set("changed_files", std::move(changed));
    result.set("patches", std::move(patches));
    {
        Value warnings = Value::array();  // WP-25: the warnings of every module report, one line each
        for (const PatchReport& p : reports) {
            for (const Diag& w : p.warnings) {
                warnings.push(Value::string(w.format()));
            }
        }
        result.set("warnings", std::move(warnings));
    }
    sum.total = wall_ms;
    result.set("timing_ms", to_json(sum));
    std::string fixture = reports.empty() ? std::string() : reports.front().module;
    if (const std::size_t dot = fixture.find_last_of('.'); dot != std::string::npos) {
        fixture.resize(dot);
    }
    result.set("timing", Value::string(timing_line(fixture, sum)));
    return result;
}

// ---- revert (WP-22) -------------------------------------------------------------------------------------------

Result<Value> ReloadService::revert(const Value& args) {
    std::uint32_t pid = 0;
    std::uint32_t timeout_ms = 60000;
    std::optional<Diag> error;
    if (!read_uint(args, "pid", 0xFFFFFFFFu, &pid, &error) || !read_uint(args, "timeout_ms", 3600u * 1000u, &timeout_ms, &error)) {
        return *error;
    }
    if (timeout_ms == 0) {
        timeout_ms = 60000;
    }
    const std::string module = args.get_string("module");
    if (const Value* m = args.find("module"); m != nullptr && !m->is_null() && !m->is_string()) {
        return bad_request("argument 'module' must be a string");
    }
    RevertScope scope = RevertScope::Last;
    if (const Value* sc = args.find("scope"); sc != nullptr && !sc->is_null()) {
        if (!sc->is_string() || (sc->as_string() != "last" && sc->as_string() != "all")) {
            return Diag::make(revert::diag::kBadScope, "argument 'scope' must be \"last\" or \"all\"",
                              "Send {\"scope\":\"last\"} to undo the newest reload, {\"scope\":\"all\"} to undo every reload.");
        }
        scope = sc->as_string() == "all" ? RevertScope::All : RevertScope::Last;
    }
    // A revert and a reload never overlap: both change the hooks and the broker's per-module patch list.
    ExclusiveRun exclusive(reload_mutex_, running_);
    if (!exclusive.acquired()) {
        const Diag d = Diag::make(pd::kBusy, "a reload is still running", "Wait for it to finish (control command `wait`) and try again.");
        if (events_ != nullptr) {
            events_->publish(make_diag_event(d, "error", "revert"));
        }
        return d;
    }

    const std::vector<Target> list = targets(pid, module);
    RevertOptions revert_options;
    revert_options.timeout_ms = options().hooks_timeout_ms;
    revert_options.deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);  // one deadline for every module and step
    revert_options.events = events_;
    if (scope == RevertScope::Last) {
        // "last" is the newest reload: the modules that reload did not touch keep their older ones.
        for (const Target& t : list) {
            std::lock_guard<std::mutex> lock(t.module->mutex);
            if (!t.module->batches.empty()) {
                revert_options.only_reload = std::max(revert_options.only_reload, t.module->batches.back().reload_id);
            }
        }
    }
    Value reverted = Value::array();
    Value failed = Value::array();
    std::size_t reverted_count = 0;
    std::size_t failed_count = 0;
    std::optional<Diag> first_error;
    for (const Target& t : list) {
        const std::uint32_t target_pid = t.link->snapshot().pid;
        Result<RevertReport> r = revert_module(*t.link, *t.module, scope, revert_options);
        if (!r) {
            CML_LOG(Warn, "revert: %s", r.error().format().c_str());
            if (events_ != nullptr) {
                events_->publish(make_diag_event(r.error(), "error", "revert", target_pid));
            }
            Value entry = Value::object();
            entry.set("pid", Value::integer(static_cast<std::int64_t>(target_pid)));
            entry.set("module", Value::string(t.module->name));
            entry.set("id", Value::string(r.error().id()));
            entry.set("message", Value::string(r.error().message));
            failed.push(std::move(entry));
            ++failed_count;
            if (!first_error) {
                first_error = r.error();
            }
            continue;
        }
        for (const Diag& w : r->warnings) {
            CML_LOG(Warn, "revert: %s", w.format().c_str());
            if (events_ != nullptr) {
                events_->publish(make_diag_event(w, "warning", "revert", r->pid));
            }
        }
        if (r->batches == 0) {
            continue;
        }
        // The broker-wide generation counts the live patches: it goes back with the module's.
        generation_.fetch_sub(std::min<std::uint64_t>(generation_.load(), r->generation_before - r->generation));
        Value entry = Value::object();
        entry.set("pid", Value::integer(static_cast<std::int64_t>(r->pid)));
        entry.set("module", Value::string(r->module));
        entry.set("generation", Value::integer(static_cast<std::int64_t>(r->generation)));
        entry.set("batches", Value::integer(static_cast<std::int64_t>(r->batches)));
        entry.set("patches", Value::integer(static_cast<std::int64_t>(r->patches)));
        entry.set("hooks_restored", Value::integer(static_cast<std::int64_t>(r->hooks_restored)));
        entry.set("hooks_repointed", Value::integer(static_cast<std::int64_t>(r->hooks_repointed)));
        reverted.push(std::move(entry));
        ++reverted_count;
    }
    if (reverted_count == 0 && first_error) {
        Diag d = *first_error;
        if (failed_count > 1) {
            d.message += " (" + std::to_string(failed_count) + " of " + std::to_string(list.size()) + " modules failed)";
        }
        return d;
    }
    if (reverted_count == 0) {
        const Diag d = Diag::make(revert::diag::kNothingToRevert,
                                  std::string("no patch to revert") + (list.empty() ? ": no enabled module" : "") + (pid != 0 ? " in process " + std::to_string(pid) : "") +
                                      (module.empty() ? "" : " matching '" + module + "'"),
                                  "A revert undoes patches that a `reload` applied in this session; reload first.");
        if (events_ != nullptr) {
            events_->publish(make_diag_event(d, "error", "revert"));
        }
        return d;
    }
    Value result = Value::object();
    result.set("reverted", std::move(reverted));
    result.set("generation", Value::integer(static_cast<std::int64_t>(generation_.load())));
    if (failed_count != 0) {
        // Some modules were reverted and others were not: the answer says which, so a retry does not undo the first ones again.
        const Diag d = Diag::make(revert::diag::kRevertPartial,
                                  std::to_string(failed_count) + " of " + std::to_string(reverted_count + failed_count) + " modules could not be reverted: " + first_error->message,
                                  "The modules in `reverted` are done; fix the cause for those in `failed` and send `revert` again (it only touches what is still applied).");
        CML_LOG(Warn, "revert: %s", d.format().c_str());
        if (events_ != nullptr) {
            events_->publish(make_diag_event(d, "error", "revert"));
        }
        result.set("partial", Value::boolean(true));
        result.set("failed", std::move(failed));
        result.set("diag", Value::string(d.id()));
    }
    return result;
}

// ---- compile on save (WP-14) -----------------------------------------------------------------------------------

std::vector<std::string> ReloadService::tracked_files() const {
    std::vector<std::shared_ptr<ModuleState>> modules;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& a : agents_) {
            for (const auto& m : a.second.modules) {
                modules.push_back(m.second);
            }
        }
    }
    std::set<std::string> files;
    for (const std::shared_ptr<ModuleState>& m : modules) {
        std::shared_ptr<ModuleIndex> index;
        {
            std::lock_guard<std::mutex> lock(m->mutex);
            index = m->index;
        }
        if (index && index->status().state == IndexState::Ready) {
            for (std::string& f : index->tracked_files()) {
                files.insert(std::move(f));
            }
        }
    }
    return std::vector<std::string>(files.begin(), files.end());
}

SaveWatcherHooks::ReloadResult ReloadService::reload_for_save() {
    Result<Value> r = run_reload(Value::object(), true);
    if (!r && r.error().code == header::diag::kTooManyCompilands) {
        // A save that touches more compilands than max_affected_compilands: the pipeline has published CML5551 (with
        // the way to confirm). Nobody is there to be asked, so nothing is compiled; the files stay "changed" and the
        // next explicit `reload` (with "confirm": true) or a smaller save picks them up.
        CML_LOG(Info, "compile-on-save: the change affects too many compilands; run `reload` with \"confirm\": true to recompile them");
        return SaveWatcherHooks::ReloadResult::Done;
    }
    if (r) {
        return SaveWatcherHooks::ReloadResult::Done;
    }
    return r.error().code == pd::kBusy ? SaveWatcherHooks::ReloadResult::Busy : SaveWatcherHooks::ReloadResult::Failed;
}

std::optional<Diag> ReloadService::apply_compile_on_save(bool on) {
    std::shared_ptr<SaveWatcher> old;
    std::shared_ptr<SaveWatcher> created;
    std::shared_ptr<SaveWatcher> existing;
    {
        std::lock_guard<std::mutex> lock(watch_mutex_);
        if (!on) {
            old.swap(save_watcher_);
        } else if (save_watcher_) {
            existing = save_watcher_;
        } else {
            SaveWatcherHooks hooks;
            hooks.tracked_files = [this] { return tracked_files(); };
            hooks.reload = [this] { return reload_for_save(); };
            hooks.report = [this](const Diag& d, const char* severity) {
                if (events_ != nullptr) {
                    events_->publish(make_diag_event(d, severity, "watch"));
                }
            };
            SaveWatcherOptions options;
            options.factory = watcher_factory_;
            save_watcher_ = std::make_shared<SaveWatcher>(std::move(hooks), std::move(options));
            created = save_watcher_;
        }
    }
    if (old) {
        // Switching off returns at once: no new reload starts, and a reload a save already started finishes on its
        // own (it is joined when the service is destroyed).
        old->stop();
        std::lock_guard<std::mutex> lock(watch_mutex_);
        retiring_.erase(std::remove_if(retiring_.begin(), retiring_.end(),
                                       [](std::future<void>& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }),
                        retiring_.end());
        retiring_.push_back(std::async(std::launch::async, [w = std::move(old)]() mutable { w.reset(); }));
    }
    if (!on) {
        CML_LOG(Info, "compile on save: off");
        return std::nullopt;
    }
    if (created) {
        CML_LOG(Info, "compile on save: on");
        return created->rearm();
    }
    return existing->rearm();
}

void ReloadService::rearm_save_watcher() {
    std::shared_ptr<SaveWatcher> watcher;
    {
        std::lock_guard<std::mutex> lock(watch_mutex_);
        watcher = save_watcher_;
    }
    if (watcher) {
        (void)watcher->rearm();
    }
}

void ReloadService::set_watcher_factory_for_testing(WatcherFactory factory) {
    std::lock_guard<std::mutex> lock(watch_mutex_);
    watcher_factory_ = std::move(factory);
}

// ---- wait ---------------------------------------------------------------------------------------------------

Result<Value> ReloadService::wait(const Value& args) {
    std::uint32_t timeout_ms = 60000;
    std::optional<Diag> error;
    if (!read_uint(args, "timeout_ms", 3600u * 1000u, &timeout_ms, &error)) {
        return *error;
    }
    const std::string what = args.find("for") != nullptr ? args.get_string("for") : "idle";
    if (what != "idle" && what != "index") {
        return bad_request("argument 'for' must be \"idle\" or \"index\"");
    }
    const Clock::time_point start = Clock::now();
    const Clock::time_point deadline = start + std::chrono::milliseconds(timeout_ms);
    const auto remaining = [&] {
        return static_cast<std::uint32_t>(std::max<long long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count()));
    };
    std::vector<std::shared_ptr<ModuleState>> modules;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& a : agents_) {
            for (const auto& m : a.second.modules) {
                modules.push_back(m.second);
            }
        }
    }
    Value failed = Value::array();
    for (const std::shared_ptr<ModuleState>& m : modules) {
        if (m->index && !m->index->wait(remaining())) {
            return Diag::make(pd::kReloadTimeout, "the PDB index of '" + m->name + "' is still loading (" + m->index->status().phase + ")",
                              "Raise `timeout_ms`; a large PDB can take a few seconds.");
        }
        if (m->index && m->index->status().state == IndexState::Failed) {
            failed.push(Value::string(m->name));
        }
    }
    if (what == "idle") {
        while (running_.load()) {
            if (remaining() == 0) {
                return Diag::make(pd::kReloadTimeout, "a reload is still running", "Raise `timeout_ms`.");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    Value result = Value::object();
    result.set("for", Value::string(what));
    result.set("modules", Value::integer(static_cast<std::int64_t>(modules.size())));
    result.set("failed", std::move(failed));
    result.set("waited_ms", Value::integer(static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count())));
    return result;
}

// ---- set-option ---------------------------------------------------------------------------------------------

Result<Value> ReloadService::set_option(const Value& args) {
    const Value* name_v = args.find("name");
    const Value* value = args.find("value");
    if (name_v == nullptr || !name_v->is_string() || value == nullptr) {
        return bad_request("set-option needs a string \"name\" and a \"value\"");
    }
    const std::string name = name_v->as_string();
    std::unique_lock<std::mutex> lock(mutex_);
    ReloadOptions& o = options_;
    const auto want_bool = [&](bool* field) -> std::optional<Diag> {
        if (!value->is_bool()) {
            return bad_request("option '" + name + "' takes a boolean");
        }
        *field = value->as_bool();
        return std::nullopt;
    };
    const auto want_uint = [&](std::uint32_t* field, std::uint32_t lo, std::uint32_t hi) -> std::optional<Diag> {
        if (!value->is_integer() || value->as_integer() < static_cast<std::int64_t>(lo) || value->as_integer() > static_cast<std::int64_t>(hi)) {
            return bad_request("option '" + name + "' takes an integer from " + std::to_string(lo) + " to " + std::to_string(hi));
        }
        *field = static_cast<std::uint32_t>(value->as_integer());
        return std::nullopt;
    };
    const auto want_string = [&](std::string* field) -> std::optional<Diag> {
        if (!value->is_string()) {
            return bad_request("option '" + name + "' takes a string");
        }
        *field = value->as_string();
        return std::nullopt;
    };
    Value previous;
    std::optional<Diag> error;
    bool toggle = false;
    if (name == "narrow_hooks") {
        previous = Value::boolean(o.narrow_hooks);
        error = want_bool(&o.narrow_hooks);
    } else if (name == "release_pdb_after_reload") {
        previous = Value::boolean(o.release_pdb_after_reload);
        error = want_bool(&o.release_pdb_after_reload);
    } else if (name == "track_system_headers") {
        previous = Value::boolean(o.track_system_headers);
        error = want_bool(&o.track_system_headers);
    } else if (name == "max_parallel_compiles") {
        previous = Value::integer(o.max_parallel_compiles);
        std::uint32_t v = o.max_parallel_compiles;
        error = want_uint(&v, 0, 256);
        o.max_parallel_compiles = v;
    } else if (name == "max_affected_compilands") {
        previous = Value::integer(o.max_affected_compilands);
        std::uint32_t v = o.max_affected_compilands;
        error = want_uint(&v, 0, 1000000);
        o.max_affected_compilands = v;
    } else if (name == "agent_timeout_ms") {
        previous = Value::integer(o.agent_timeout_ms);
        error = want_uint(&o.agent_timeout_ms, 100, 3600u * 1000u);
    } else if (name == "hooks_timeout_ms") {
        previous = Value::integer(o.hooks_timeout_ms);
        error = want_uint(&o.hooks_timeout_ms, 100, 3600u * 1000u);
    } else if (name == "reserve_block_mb") {
        previous = Value::integer(o.reserve_block_mb);
        // A patch needs room for at least 4 MiB in a block (PatchPipeline's kMinPatchRoom): smaller blocks could
        // never be used.
        if (value->is_integer() && value->as_integer() >= 1 && value->as_integer() < 4) {
            return bad_request("option 'reserve_block_mb' must be at least 4 (MiB): a smaller block cannot hold a patch");
        }
        error = want_uint(&o.reserve_block_mb, 4, 1024);
    } else if (name == "reserve_blocks") {
        previous = Value::integer(o.reserve_blocks);
        error = want_uint(&o.reserve_blocks, 1, 64);
    } else if (name == "max_load_attempts") {
        previous = Value::integer(o.max_load_attempts);
        std::uint32_t v = o.max_load_attempts;
        error = want_uint(&v, 1, 5);
        o.max_load_attempts = v;
    } else if (name == "linker") {
        previous = Value::string(o.linker_override);
        std::string v = o.linker_override;
        error = want_string(&v);
        if (!error && v != "" && v != "link" && v != "lld-link") {
            error = bad_request("option 'linker' takes \"\", \"link\" or \"lld-link\"");
        }
        if (!error) {
            o.linker_override = v;
        }
    } else if (name == "linker_path") {
        previous = Value::string(o.linker_path);
        error = want_string(&o.linker_path);
    } else if (name == "compile_on_save") {
        previous = Value::boolean(o.compile_on_save);
        error = want_bool(&o.compile_on_save);
        toggle = true;
    } else if (name == "crash_handler") {
        previous = Value::boolean(o.crash_handler);
        error = want_bool(&o.crash_handler);
    } else if (name == "break_with_debugger") {
        previous = Value::boolean(o.break_with_debugger);
        error = want_bool(&o.break_with_debugger);
    } else if (name == "log_level") {
        static const char* const kNames[] = {"trace", "debug", "info", "warn", "error", "off"};
        previous = Value::string(kNames[static_cast<int>(log_level())]);
        if (!value->is_string()) {
            error = bad_request("option 'log_level' takes a string");
        } else {
            error = bad_request("option 'log_level' takes trace, debug, info, warn, error or off");
            for (int i = 0; i < 6; ++i) {
                if (value->as_string() == kNames[i]) {
                    set_log_level(static_cast<LogLevel>(i));
                    error.reset();
                }
            }
        }
    } else {
        return bad_request("unknown option '" + name +
                           "'; options: narrow_hooks, release_pdb_after_reload, track_system_headers, max_parallel_compiles, max_affected_compilands, agent_timeout_ms, "
                           "hooks_timeout_ms, reserve_block_mb, reserve_blocks, max_load_attempts, linker, linker_path, compile_on_save, crash_handler, break_with_debugger, log_level");
    }
    if (error) {
        return *error;
    }
    CML_LOG(Info, "option %s changed", name.c_str());
    const bool compile_on_save = o.compile_on_save;
    lock.unlock();  // the watcher's thread takes mutex_ (options()) while it reloads
    if (toggle) {
        const std::optional<Diag> unavailable = apply_compile_on_save(compile_on_save);
        if (unavailable && unavailable->code == watch::diag::kUnsupported) {
            // No directory watcher on this host: the option cannot be active, so it must not look as if it were.
            {
                std::lock_guard<std::mutex> relock(mutex_);
                options_.compile_on_save = false;
            }
            (void)apply_compile_on_save(false);
            return *unavailable;
        }
    }
    OptionObserver observer;
    {
        std::lock_guard<std::mutex> observer_lock(observer_mutex_);
        observer = option_observer_;
    }
    if (observer) {
        observer(name);
    }
    Value result = Value::object();
    result.set("name", Value::string(name));
    result.set("previous", std::move(previous));
    return result;
}

void ReloadService::set_option_observer(OptionObserver observer) {
    std::lock_guard<std::mutex> lock(observer_mutex_);
    option_observer_ = std::move(observer);
}

}  // namespace cml::core
