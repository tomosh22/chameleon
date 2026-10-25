// cmltest: builds a fixture, launches it with the agent against a headless broker, drives the
// broker through the control pipe, applies scripted edits and asserts on the fixture's stdout.
//
//   cmltest --scenario FILE [options]
//
// See tests/harness/README.md and docs/wp/WP-13.md.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "cml/ipc/control_ids.h"
#include "cml/proc/proc.h"
#include "fixture_builder.h"
#include "report.h"
#include "run_dirs.h"
#include "runner.h"
#include "scenario.h"

namespace {

namespace fs = std::filesystem;

void print_usage(std::FILE* out) {
    std::fputs(
        "usage: cmltest --scenario FILE [options]\n"
        "\n"
        "  --scenario FILE           scenario.toml to run (required)\n"
        "  --repo-root DIR           repository root (default: found above the scenario file)\n"
        "  --build-dir DIR           build tree of the project, holding bin/cml_broker.exe and\n"
        "                            bin/cml_agent.dll (default: <repo-root>/build)\n"
        "  --broker EXE              cml_broker.exe (default: <build-dir>/bin[/<config>]/cml_broker.exe)\n"
        "  --agent-dir DIR           directory with cml_agent.dll (default: the broker's directory)\n"
        "  --generator ninja|msbuild how the fixture is built (default: ninja)\n"
        "  --vs-generator NAME       CMake generator name for msbuild (default: the one `cmake --help` marks with *)\n"
        "  --config Debug|RelWithDebInfo   fixture build configuration (default: Debug)\n"
        "  --cxx cl|clang-cl|PATH    compiler for the fixture (default: cl)\n"
        "  --cmake EXE               cmake.exe (default: found on PATH)\n"
        "  --work-dir DIR            scratch base, same rule as --artifacts (default: <build-dir>/cmltest_scratch)\n"
        "  --artifacts DIR           base directory; cmltest creates cmltest-<scenario>-<timestamp> inside it for logs,\n"
        "                            fixture stdout and dumps (default base: <build-dir>/cmltest-artifacts)\n"
        "  --junit FILE              write a JUnit XML report\n"
        "  --prebuilt-fixture EXE    debugging aid: skip configuring/building and run this fixture exe\n"
        "  --build-timeout-s N       limit for configuring + building the fixture (default: 900)\n"
        "  --require-all-capabilities  steps skipped for missing broker capabilities fail the run\n"
        "  --clean-work              delete the scratch directory (the one cmltest created) after the run\n"
        "  --help\n"
        "\n"
        "exit code: 0 passed or partial, 1 failed, 2 usage error\n",
        out);
}

struct Args {
    std::string scenario;
    std::string repo_root;
    std::string build_dir;
    std::string broker;
    std::string agent_dir;
    std::string generator = "ninja";
    std::string vs_generator;  // empty = the generator CMake marks as default (`cmake --help`)
    std::string config = "Debug";
    std::string cxx = "cl";
    std::string cmake;
    std::string work_dir;
    std::string artifacts;
    std::string junit;
    std::string prebuilt_fixture;
    unsigned long build_timeout_s = 900;
    bool require_all = false;
    bool clean_work = false;
    bool help = false;
};

bool parse_args(const std::vector<std::string>& argv, Args* args) {
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string& a = argv[i];
        auto value = [&](std::string* out) {
            if (i + 1 >= argv.size()) {
                std::fprintf(stderr, "cmltest: %s needs a value\n", a.c_str());
                return false;
            }
            *out = argv[++i];
            return true;
        };
        std::string text;
        if (a == "--help" || a == "-h") args->help = true;
        else if (a == "--require-all-capabilities") args->require_all = true;
        else if (a == "--clean-work") args->clean_work = true;
        else if (a == "--scenario") { if (!value(&args->scenario)) return false; }
        else if (a == "--repo-root") { if (!value(&args->repo_root)) return false; }
        else if (a == "--build-dir") { if (!value(&args->build_dir)) return false; }
        else if (a == "--broker") { if (!value(&args->broker)) return false; }
        else if (a == "--agent-dir") { if (!value(&args->agent_dir)) return false; }
        else if (a == "--generator") { if (!value(&args->generator)) return false; }
        else if (a == "--vs-generator") { if (!value(&args->vs_generator)) return false; }
        else if (a == "--config") { if (!value(&args->config)) return false; }
        else if (a == "--cxx") { if (!value(&args->cxx)) return false; }
        else if (a == "--cmake") { if (!value(&args->cmake)) return false; }
        else if (a == "--work-dir") { if (!value(&args->work_dir)) return false; }
        else if (a == "--artifacts") { if (!value(&args->artifacts)) return false; }
        else if (a == "--junit") { if (!value(&args->junit)) return false; }
        else if (a == "--prebuilt-fixture") { if (!value(&args->prebuilt_fixture)) return false; }
        else if (a == "--build-timeout-s") {
            if (!value(&text)) return false;
            if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos || text.size() > 6) {
                std::fprintf(stderr, "cmltest: --build-timeout-s needs a number of seconds\n");
                return false;
            }
            args->build_timeout_s = std::strtoul(text.c_str(), nullptr, 10);
        } else {
            std::fprintf(stderr, "cmltest: unknown option '%s'\n", a.c_str());
            return false;
        }
    }
    if (args->help) {
        return true;
    }
    if (args->scenario.empty()) {
        std::fputs("cmltest: --scenario is required\n", stderr);
        return false;
    }
    if (args->generator != "ninja" && args->generator != "msbuild") {
        std::fprintf(stderr, "cmltest: --generator must be ninja or msbuild (got '%s')\n", args->generator.c_str());
        return false;
    }
    if (args->config != "Debug" && args->config != "RelWithDebInfo") {
        std::fprintf(stderr, "cmltest: --config must be Debug or RelWithDebInfo (got '%s')\n", args->config.c_str());
        return false;
    }
    return true;
}

fs::path to_path(const std::string& utf8) {
    return fs::path(cml::proc::wide_from_utf8(utf8));
}

fs::path make_absolute(const fs::path& p) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    return (ec ? p : a).lexically_normal();
}

// The directory above `start` that contains tests/fixtures.
fs::path find_repo_root(const fs::path& start) {
    std::error_code ec;
    for (fs::path dir = start; !dir.empty(); dir = dir.parent_path()) {
        if (fs::exists(dir / "tests" / "fixtures", ec) && fs::exists(dir / "CMakeLists.txt", ec)) {
            return dir;
        }
        if (dir == dir.parent_path()) {
            break;
        }
    }
    return fs::current_path(ec);
}

void print(const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

int finish(const cmltest::ScenarioReport& report, const Args& args) {
    print(cmltest::console_report(report, args.require_all));
    if (!args.junit.empty()) {
        const cml::Result<void> written = cmltest::write_text_file(args.junit, cmltest::junit_xml(report, args.require_all));
        if (!written) {
            std::fprintf(stderr, "%s\n", written.error().format().c_str());
            return 2;
        }
    }
    return report.outcome(args.require_all) == cmltest::Outcome::Failed ? 1 : 0;
}

}  // namespace

int wmain(int argc, wchar_t** wargv) {
    std::vector<std::string> argv;
    for (int i = 0; i < argc; ++i) {
        argv.push_back(cml::proc::utf8_from_wide(wargv[i], std::wcslen(wargv[i])));
    }
    Args args;
    if (!parse_args(argv, &args)) {
        print_usage(stderr);
        return 2;
    }
    if (args.help) {
        print_usage(stdout);
        return 0;
    }

    const fs::path scenario_path = make_absolute(to_path(args.scenario));
    cmltest::ScenarioReport failed_early;
    failed_early.scenario = scenario_path.stem().string();
    failed_early.generator = args.generator;
    failed_early.config = args.config;
    failed_early.compiler = args.cxx;

    cml::Result<cmltest::Scenario> scenario = cmltest::load_scenario(scenario_path);
    if (!scenario) {
        cmltest::StepResult step;
        step.name = "load scenario";
        step.status = cmltest::StepStatus::Failed;
        step.detail = scenario.error().format();
        step.diag_id = scenario.error().id();
        failed_early.steps.push_back(step);
        return finish(failed_early, args);
    }

    cmltest::RunnerOptions options;
    options.repo_root = args.repo_root.empty() ? find_repo_root(scenario_path.parent_path()) : make_absolute(to_path(args.repo_root));
    const fs::path build_dir = make_absolute(args.build_dir.empty() ? options.repo_root / "build" : to_path(args.build_dir));
    options.generator = args.generator;
    options.vs_generator = args.vs_generator;
    options.config = args.config;
    options.cxx = args.cxx;
    options.require_all_capabilities = args.require_all;
    if (!args.prebuilt_fixture.empty()) {
        options.prebuilt_fixture = make_absolute(to_path(args.prebuilt_fixture));
    }
    options.build_timeout_ms = static_cast<std::uint32_t>(std::min<unsigned long>(args.build_timeout_s, 100000ul) * 1000ul);

    std::error_code ec;
    if (!args.broker.empty()) {
        options.broker_exe = make_absolute(to_path(args.broker));
    } else {
        const fs::path with_config = build_dir / "bin" / args.config / "cml_broker.exe";
        options.broker_exe = fs::exists(with_config, ec) ? with_config : build_dir / "bin" / "cml_broker.exe";
    }
    options.agent_dir = args.agent_dir.empty() ? options.broker_exe.parent_path() : make_absolute(to_path(args.agent_dir));
    if (!fs::exists(options.broker_exe, ec)) {
        std::fprintf(stderr, "%s\n",
                     cml::Diag::make(cml::ids::kHarnessUsage, "broker executable not found: " + options.broker_exe.string(),
                                     "Build the project and pass --build-dir (or --broker).")
                         .format()
                         .c_str());
        return 2;
    }
    if (args.cmake.empty() && !args.prebuilt_fixture.empty()) {
        options.cmake = "cmake.exe";  // never started: nothing is configured or built
    } else if (args.cmake.empty()) {
        cml::Result<fs::path> cmake = cmltest::find_executable("cmake.exe");
        if (!cmake) {
            std::fprintf(stderr, "%s\n", cmake.error().format().c_str());
            return 2;
        }
        options.cmake = *cmake;
    } else {
        options.cmake = make_absolute(to_path(args.cmake));
    }

    // The user-supplied directories are only bases: cmltest creates its own subdirectory below them
    // (marked with .cmltest-artifacts) and never deletes anything it did not create.
    const fs::path artifacts_base = args.artifacts.empty() ? build_dir / "cmltest-artifacts" : make_absolute(to_path(args.artifacts));
    const fs::path work_base = args.work_dir.empty() ? build_dir / "cmltest_scratch" : make_absolute(to_path(args.work_dir));
    cml::Result<fs::path> artifacts_dir = cmltest::create_run_directory(artifacts_base, scenario->name);
    cml::Result<fs::path> work_dir = artifacts_dir ? cmltest::create_run_directory(work_base, scenario->name) : artifacts_dir;
    if (!artifacts_dir || !work_dir) {
        std::fprintf(stderr, "%s\n", (artifacts_dir ? work_dir : artifacts_dir).error().format().c_str());
        return 2;
    }
    options.artifacts_dir = *artifacts_dir;
    options.work_dir = *work_dir;

    std::printf("cmltest: scenario '%s' (%s), fixture '%s', generator %s, config %s\n", scenario->name.c_str(),
                scenario_path.string().c_str(), scenario->fixture.c_str(), args.generator.c_str(), args.config.c_str());
    std::printf("cmltest: artifacts in %s\n", options.artifacts_dir.string().c_str());
    std::fflush(stdout);

    const cmltest::ScenarioReport report = cmltest::run_scenario(*scenario, options);
    const int exit_code = finish(report, args);
    if (args.clean_work) {
        const cml::Result<void> removed = cmltest::remove_run_directory(options.work_dir);  // refuses anything cmltest did not create
        if (!removed) {
            std::fprintf(stderr, "%s\n", removed.error().format().c_str());
        }
    }
    return exit_code;
}
