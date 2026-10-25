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
