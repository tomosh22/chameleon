#pragma once

// Scenario files (scenario.toml) and their validation. See tests/harness/README.md for the format.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "cml/util/result.h"

namespace cmltest {

enum class StepKind {
    Launch,               // start the headless broker (+ control pipe) and the fixture with the agent
    ExpectStdout,         // regex must match fixture output produced since the last match/reload
    ExpectModuleEnabled,  // `status` must list the fixture process with a matching module enabled
    Edit,                 // apply a unified diff to the scratch copy of the fixture sources
    Reload,               // control command `reload` (needs capability "reload")
    ExpectDiag,           // a `diag` event with this id must arrive (needs capability "diag-events")
    ExpectMonotonic,      // a `key=<int>` of the fixture's TICK lines never decreases (state kept in the old storage)
    SetOption,            // control command `set-option` (WP-14: e.g. compile_on_save = true)
    Wait,                 // control command `wait` for "index" or "idle"
    ExpectEvent,          // an event of this name (patch_applied, ...) must arrive (needs capability "subscribe")
    SignalFixture,        // create the file named by CML_FIXTURE_SIGNAL_FILE, which the fixture waits for (WP-17)
    ExpectReloadResult,   // an integer member of the most recent reload's result has this value (WP-16)
    Restart,              // control command `restart` (WP-21, restart_step.h): the process is replaced by a relaunched one
    Revert,               // control command `revert` with scope "last" or "all" (WP-22; needs capability "revert")
    // WP-20, crash -> patch -> continue (bodies in exception_steps.cpp, needs the capability "exceptions").
    WatchExceptions,          // subscribe to `exception` events: the agents start holding faulting threads
    ExpectException,          // an `exception` event arrives (optional code / function must match)
    ExceptionAction,          // control command `exception-action` for the current exception
    ExpectPendingExceptions,  // control command `exceptions` lists exactly `count` held threads
    // WP-27 (perf_steps.h; run by tests/bench/scenarios/perf.toml in the pdb-benchmark job).
    PerfIdle,    // observe the broker's CPU time while it idles with a connected fixture
    PerfReport,  // statistics of the recorded reloads, the broker's peak memory, perf-reload.json
};

const char* to_string(StepKind kind);

struct Step {
    StepKind kind = StepKind::Launch;
    int line = 0;          // line of the [[step]] header in the scenario file
    std::string regex;     // ExpectStdout
    std::string module;    // ExpectModuleEnabled: file name or path suffix, compared case-insensitively
    std::string patch;     // Edit: path relative to the fixture directory
    std::string id;        // ExpectDiag: "CML3101"
    std::string event;     // ExpectEvent: "patch_applied"
    std::string option;    // SetOption: option name
    enum class OptionKind { Bool, Integer, String } option_kind = OptionKind::Bool;
    bool option_bool = false;
    long long option_integer = 0;
    std::string option_string;
    std::string wait_for;  // Wait: "index" | "idle"
    int within_s = 10;     // ExpectStdout / ExpectModuleEnabled / Reload / ExpectDiag / ExpectEvent / Wait
    std::string key;       // ExpectMonotonic: the TICK key ("calls")
    int min_samples = 10;  // ExpectMonotonic: at least this many samples of the key in the whole output
    // Reload: the result object of the reload command must hold this integer member (e.g. "initializers_run" = 1).
    std::string result_key;
    long long result_value = 0;
    bool has_result_value = false;
    // Reload (WP-15): the reload must be refused with this diagnostic id (e.g. "CML5550"); empty = it must succeed.
    std::string expect_error;
    // WP-16, several processes. Launch: `exe` is a fixture executable of the same build (file name, ".exe" optional; empty =
    // the fixture's primary exe) and `label` names the process (default "fixture" for the primary exe, else the exe's name
    // without extension). `process` selects the process of ExpectStdout / ExpectModuleEnabled / ExpectMonotonic (empty =
    // the first launched). ExpectReloadResult uses `key` and `result_value`.
    std::string exe;
    std::string label;
    std::string args;   // Launch: extra command-line arguments of the fixture process (after --agent)
    std::string process;
    // Revert (WP-22): "last" (the newest reload) or "all"; `expect_error` (a CML id) makes a refusal the expected outcome.
    std::string revert_scope = "last";
    // WP-20. ExpectException: `exception_code` ("0xC0000005") and `exception_function` (substring) must match when set.
    // ExceptionAction: `exception_action` is continue | leave | ignore; `expect_error` (CMLnnnn) = the action must be refused.
    // ExpectPendingExceptions: `result_value` is the expected count.
    std::string exception_code;
    std::string exception_function;
    std::string exception_action;

    // "expect_stdout /value=2/ within 10 s"
    std::string describe() const;
    // The broker capability the step needs, or nullptr when it needs none beyond `status`.
    const char* required_capability() const;
};

struct Scenario {
    std::string name;
    std::string description;
    std::string fixture;  // tests/fixtures/<fixture>
    std::string fixture_args;  // extra command-line arguments for the fixture after `--agent` (WP-17); may be empty
    int timeout_s = 120;
    std::vector<Step> steps;
    std::filesystem::path path;  // where it was loaded from (empty for parse_scenario)
};

// CML5410 for TOML syntax errors, CML5411 for unknown keys, missing fields and bad values, CML5420
// for a regex that does not compile.
cml::Result<Scenario> parse_scenario(std::string_view text);
cml::Result<Scenario> load_scenario(const std::filesystem::path& path);

}  // namespace cmltest
