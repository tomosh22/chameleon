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
    ExpectReloadResult,   // an integer member of the most recent reload's result has this value (WP-16)
};

const char* to_string(StepKind kind);

struct Step {
    StepKind kind = StepKind::Launch;
    int line = 0;          // line of the [[step]] header in the scenario file
    std::string regex;     // ExpectStdout
    std::string module;    // ExpectModuleEnabled: file name or path suffix, compared case-insensitively
    std::string patch;     // Edit: path relative to the fixture directory
    std::string id;        // ExpectDiag: "CML3101"
    int within_s = 10;     // ExpectStdout / ExpectModuleEnabled / Reload / ExpectDiag
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

    // "expect_stdout /value=2/ within 10 s"
    std::string describe() const;
    // The broker capability the step needs, or nullptr when it needs none beyond `status`.
    const char* required_capability() const;
};

struct Scenario {
    std::string name;
    std::string description;
    std::string fixture;  // tests/fixtures/<fixture>
    int timeout_s = 120;
    std::vector<Step> steps;
    std::filesystem::path path;  // where it was loaded from (empty for parse_scenario)
};

// CML5410 for TOML syntax errors, CML5411 for unknown keys, missing fields and bad values, CML5420
// for a regex that does not compile.
cml::Result<Scenario> parse_scenario(std::string_view text);
cml::Result<Scenario> load_scenario(const std::filesystem::path& path);

}  // namespace cmltest
