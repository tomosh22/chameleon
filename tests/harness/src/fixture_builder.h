#pragma once

// Builds a fixture from a scratch COPY of tests/fixtures/<name> with CMake (Ninja or the Visual
// Studio generator), standalone: the fixture project is wrapped by
// tests/harness/fixture_wrapper/CMakeLists.txt, which includes CmlFixture.cmake and points it at the
// repository's agent header. The repository tree is never modified.

#if defined(_WIN32)

#include <filesystem>
#include <string>

#include "cml/util/result.h"
#include "supervisor.h"

namespace cmltest {

struct CompilerChoice {
    std::filesystem::path path;  // full path of cl.exe / clang-cl.exe
    bool is_clang_cl = false;
    std::string display;         // "cl" or "clang-cl"
};

// Full path of `name` (e.g. "cmake.exe") found on PATH; CML5421 when it is not there.
cml::Result<std::filesystem::path> find_executable(const std::string& name);

// "cl" / "msvc" / "clang-cl" are looked up on PATH; anything else is taken as a path to the compiler.
cml::Result<CompilerChoice> resolve_compiler(const std::string& cxx);

struct BuildSettings {
    std::filesystem::path repo_root;
    std::string fixture;                  // directory name below tests/fixtures
    std::filesystem::path work_dir;       // scratch root of this run (created, must not exist or be empty)
    std::filesystem::path artifacts_dir;  // cmake logs go here
    std::filesystem::path agent_dir;      // directory containing cml_agent.dll (copied next to the fixture exe)
    std::string generator = "ninja";      // "ninja" or "msbuild"
    std::string vs_generator;             // empty = ask CMake which Visual Studio it would pick
    std::string config = "Debug";
    CompilerChoice compiler;
    std::filesystem::path cmake = "cmake";
    // Debugging aid (--prebuilt-fixture): use this fixture exe instead of configuring and building.
    // The source copy is still made (edit steps patch it); the exe is copied with the agent DLL.
    std::filesystem::path prebuilt_exe;
    std::uint32_t configure_timeout_ms = 300000;
    std::uint32_t build_timeout_ms = 900000;
};

struct BuiltFixture {
    std::filesystem::path source_dir;  // the scratch copy of the fixture sources (what `edit` steps patch)
    std::filesystem::path build_dir;
    std::filesystem::path exe;         // fixture_<name>.exe, with cml_agent.dll beside it
};

// Copy, configure, build, locate the exe, copy the agent DLL beside it. CML5414 when configure or
// build fails (the message carries the end of the log) or times out (children are dumped then killed).
cml::Result<BuiltFixture> build_fixture(Supervisor& supervisor, const BuildSettings& settings);

}  // namespace cmltest

#endif  // _WIN32
