#pragma once

// The patch pipeline of one module (plan section 3, steps 1 to 9; WP-12).
//
//   sweep      the PDB index maps the changed source files to compilands (ModuleIndex)
//   compile    each compiland's recorded command line is replayed with /Z7 into the session directory (parallel)
//   plan       plan_bind() per object, then the hook list is narrowed to the functions that really changed
//   anchor     section locators (add_anchors, incl. the plan's associative sections) and binding.obj
//   link       one patch DLL at a fixed base inside the address space the agent reserved near the module
//   load       LoadPatch: the agent loads the DLL
//   fixups     SECREL sites are pre-applied to the DLL file; the rest goes out as ApplyFixups
//   init       RunInitializers: .CRT$XCU entries of NEW globals only
//   hooks      InstallHooks (immediate or at the next SyncPoint), then the registry/hook state is updated
//
// Every stage is timed; every failure becomes a diag event (stable id) and the returned Diag. After a failure
// that follows a successful load the patch is unloaded (ReleaseRange UnloadPatch), so a refused reload leaves
// the process as it was (initialisers that already ran are not undone: docs/wp/WP-12.md).
//
// The external effects (compiler, linker) are in PipelineEnv so the orchestration is unit-tested on every host
// with a scripted agent; native_pipeline_env() has the Windows implementation.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cml/core/agent_link.h"
#include "cml/core/event_bus.h"
#include "cml/core/hook_narrowing.h"
#include "cml/core/module_state.h"
#include "cml/core/module_symbols.h"
#include "cml/ipc/json.h"
#include "cml/toolchain/invoke.h"
#include "cml/toolchain/link_patch.h"
#include "cml/util/result.h"

namespace cml::core {

struct ReloadOptions {
    bool narrow_hooks = true;                  // plan step 8 narrowing (docs/wp/WP-12.md)
    unsigned max_parallel_compiles = 0;        // 0 = hardware concurrency
    std::uint32_t agent_timeout_ms = 30000;    // per request to the agent (reserve, load, fix-ups, initialisers)
    std::uint32_t hooks_timeout_ms = 30000;    // InstallHooks: in synchronised mode this waits for SyncPoint()
    bool release_pdb_after_reload = false;     // ADR-0017: drop the PDB mapping when a reload is done
    bool track_system_headers = false;         // baseline also hashes SDK/toolchain headers
    // WP-15: a reload that would recompile more compilands than this does not start unless the request is confirmed
    // (control command `reload` with "confirm": true). 0 = no limit.
    std::uint32_t max_affected_compilands = 64;
    std::uint32_t reserve_block_mb = 64;
    std::uint32_t reserve_blocks = 4;
    unsigned max_load_attempts = 3;            // relink at another base when the range was taken (CML4203)
    std::string linker_override;               // "link" | "lld-link" | "" (detect)
    std::string linker_path;                   // explicit linker binary
    bool compile_on_save = false;              // WP-14: watch the tracked sources and reload when one is saved
};

// Replaceable effects.
struct PipelineEnv {
    std::function<std::vector<Result<void>>(const std::vector<toolchain::CompileRequest>&, std::vector<toolchain::CompileOutcome>&, unsigned,
                                            toolchain::CancelToken*, const std::function<void(std::size_t index)>& on_done)>
        compile_all;  // on_done (may be empty) is called from a worker thread when unit `index` has finished
    // Finds the linker that mirrors the module's (ADR-0027).
    std::function<Result<toolchain::ToolInfo>(const std::string& module_path, const std::vector<std::string>& search_dirs, const ReloadOptions&,
                                              std::vector<Diag>* warnings)>
        resolve_linker;
    std::function<Result<toolchain::LinkedPatch>(const toolchain::LinkPatchRequest&, toolchain::LinkTrace*)> link_patch;
};

// The real compiler/linker (Windows). On other hosts every function fails with a diagnostic.
PipelineEnv native_pipeline_env();

struct StageTimes {
    double sweep = 0, compile = 0, plan = 0, anchor = 0, link = 0, load = 0, fixups = 0, init = 0, hooks = 0, total = 0;
    void add(const StageTimes& o);
};
// `RELOAD-TIMING fixture=<name> sweep_ms=.. compile_ms=.. plan_ms=.. anchor_ms=.. link_ms=.. load_ms=.. fixups_ms=..
//  init_ms=.. hooks_ms=.. total_ms=..` (one line, milliseconds with one decimal).
std::string timing_line(const std::string& fixture, const StageTimes& times);

struct PatchReport {
    std::uint32_t pid = 0;
    std::string module;
    std::uint64_t generation = 0;
    bool patched = false;           // false: the sweep found nothing to do
    std::size_t patches = 0;        // patches loaded by this run (one per recompiled compiland that changed code)
    std::vector<std::string> changed_files;
    std::size_t compilands = 0;
    std::size_t compiled = 0;       // compiler runs of this module (WP-16)
    std::size_t compile_reused = 0; // compile results taken from the reload's cache (another process or module ran the same command line)
    std::size_t hooks_planned = 0;  // before narrowing
    std::size_t hooks_installed = 0;
    std::size_t hooks_dropped = 0;
    std::size_t new_functions = 0;
    std::size_t new_globals = 0;
    std::size_t thunks = 0;
    std::size_t fixups = 0;         // sent to the agent (wire)
    std::size_t pre_applied = 0;    // SECREL sites written into the DLL
    std::size_t initializers = 0;   // requested
    std::size_t initializers_run = 0;
    std::size_t load_attempts = 0;
    std::uint64_t base = 0;
    std::uint32_t image_size = 0;
    std::string dll_path;
    StageTimes times;
    std::vector<Diag> warnings;
};

// Compile results shared by the modules of ONE reload (WP-16): the compile depends only on the translation unit's command
// line (compiler, working directory, arguments, source), not on the process or the module, so a DLL shared by two
// processes (or two modules built from one recorded command line) is compiled once; linking and loading stay per process.
// A cache lives for one reload command: the next reload compiles again, because the sources may have changed.
class CompileCache {
public:
    struct Entry {
        toolchain::CompileOutcome outcome;
        std::optional<Diag> error;  // set when the compile failed (a failure is shared as well)
    };
    // The key of a recorded command line.
    static std::string key_of(const toolchain::CompileRecord& record);

    std::optional<Entry> find(const std::string& key) const;
    void put(const std::string& key, Entry entry);

private:
    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;
};

// Where the pipeline works and who hears about it.
struct PipelineContext {
    std::string session_dir;                      // per-session temporary directory (created on demand)
    const ReloadOptions* options = nullptr;
    EventBus* events = nullptr;                   // may be null
    std::shared_ptr<ExportCache> exports;         // shared between reloads
    std::chrono::steady_clock::time_point deadline;
    toolchain::CancelToken* cancel = nullptr;
    std::uint64_t reload_id = 0;                  // WP-22: the reload command this run belongs to (a revert of `last` undoes one id)
    bool confirm = false;                         // WP-15: the request accepted a reload above max_affected_compilands
    std::shared_ptr<CompileCache> compile_cache;  // may be null: every compile runs
};

class PatchPipeline {
public:
    explicit PatchPipeline(PipelineEnv env) : env_(std::move(env)) {}

    // Runs steps 1 to 9 for one module. Returns the report (patched == false when nothing changed) or the
    // first error; every error and warning has been published as a diag event already.
    Result<PatchReport> patch_module(AgentLink& link, ModuleState& module, const PipelineContext& context);

private:
    PipelineEnv env_;
};

ipc::json::Value to_json(const PatchReport& report);
ipc::json::Value to_json(const StageTimes& times);

}  // namespace cml::core
