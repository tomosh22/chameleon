#pragma once

// Runs one scenario: build the fixture from a scratch copy, start a headless broker with a control
// pipe and the fixture with the agent, drive the steps, collect artifacts. Windows only.

#if defined(_WIN32)

#include <cstdint>
#include <filesystem>
#include <string>

#include "report.h"
#include "scenario.h"

namespace cmltest {

struct RunnerOptions {
    std::filesystem::path repo_root;
    std::filesystem::path broker_exe;      // cml_broker.exe
    std::filesystem::path agent_dir;       // directory containing cml_agent.dll
    std::filesystem::path work_dir;        // scratch root for this run (fixture copy + build tree)
    std::filesystem::path artifacts_dir;   // logs, stdout, dumps
    std::filesystem::path cmake;           // full path of cmake.exe
    std::string generator = "ninja";       // "ninja" | "msbuild"
    std::string vs_generator = "Visual Studio 17 2022";
    std::string config = "Debug";
    std::string cxx = "cl";                // "cl" | "clang-cl" | path
    std::filesystem::path prebuilt_fixture;  // debugging aid: skip configure/build and use this fixture exe
    bool require_all_capabilities = false;
    std::uint32_t build_timeout_ms = 900000;  // configure + build of the fixture (not part of timeout_s)
};

// Never throws; every failure becomes a failed step in the report. The scenario's timeout_s bounds
// everything after the build; on a failure or timeout minidumps of all live children are written to
// <artifacts_dir>/dumps before they are killed.
ScenarioReport run_scenario(const Scenario& scenario, const RunnerOptions& options);

}  // namespace cmltest

#endif  // _WIN32
