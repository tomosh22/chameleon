#include "cml/core/reload_service.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <thread>

#include "cml/core/pipeline_diag_ids.h"
#include "cml/core/process_registry.h"
#include "cml/util/log.h"
#include "cml/util/path.h"

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
    const Value* v = args.find(key);
    if (v == nullptr || v->is_null()) {
        return true;
    }
    if (!v->is_integer() || v->as_integer() < 0 || v->as_integer() > static_cast<std::int64_t>(max)) {
        *error = bad_request(std::string("argument '") + key + "' must be an integer from 0 to " + std::to_string(max));
        return false;
    }
    *out = static_cast<std::uint32_t>(v->as_integer());
    return true;
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
}

ReloadService::~ReloadService() {
    cancel_.cancel();
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
    dropped.clear();  // joins their threads
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
        spec.on_change = [name](const IndexStatus& s) {
            if (s.state == IndexState::Failed) {
                CML_LOG(Warn, "index of %s failed: %s", name.c_str(), s.error ? s.error->format().c_str() : "unknown error");
            } else {
                CML_LOG(Debug, "index of %s: %s %u%% (%s)", name.c_str(), to_string(s.state), s.percent, s.phase.c_str());
            }
        };
        CML_LOG(Info, "module %s enabled in process %u: loading its PDB index", state->name.c_str(), snap.pid);
        state->index = ModuleIndex::start(std::move(spec));
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

    std::unique_lock<std::mutex> only_one(reload_mutex_, std::try_to_lock);
    if (!only_one.owns_lock()) {
        const Diag d = Diag::make(pd::kBusy, "another reload is still running", "Wait for it to finish (control command `wait`) and try again.");
        if (events_ != nullptr) {
            events_->publish(make_diag_event(d, "error", "reload"));
        }
        return d;
    }
    struct Running {
        std::atomic<bool>& flag;
        explicit Running(std::atomic<bool>& f) : flag(f) { flag.store(true); }
        ~Running() { flag.store(false); }
    } running(running_);

    const Clock::time_point start = Clock::now();
    const std::vector<Target> list = targets(pid, module);
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
    std::set<std::uint32_t> patched_pids;
    Value patches = Value::array();
    Value changed = Value::array();
    for (const PatchReport& p : reports) {
        hooks += p.hooks_installed;
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
    sum.total = wall_ms;
    result.set("timing_ms", to_json(sum));
    std::string fixture = reports.empty() ? std::string() : reports.front().module;
    if (const std::size_t dot = fixture.find_last_of('.'); dot != std::string::npos) {
        fixture.resize(dot);
    }
    result.set("timing", Value::string(timing_line(fixture, sum)));
    return result;
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
    std::lock_guard<std::mutex> lock(mutex_);
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
                           "hooks_timeout_ms, reserve_block_mb, reserve_blocks, max_load_attempts, linker, linker_path, log_level");
    }
    if (error) {
        return *error;
    }
    CML_LOG(Info, "option %s changed", name.c_str());
    Value result = Value::object();
    result.set("name", Value::string(name));
    result.set("previous", std::move(previous));
    return result;
}

}  // namespace cml::core
