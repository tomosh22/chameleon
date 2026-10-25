#include "fixture_builder.h"

#if defined(_WIN32)

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

#include "cml/ipc/control_ids.h"
#include "cml/proc/proc.h"

namespace cmltest {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string last_lines(const fs::path& file, std::size_t count) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();
    std::size_t pos = text.size();
    std::size_t seen = 0;
    while (pos > 0 && seen <= count) {
        --pos;
        if (text[pos] == '\n') {
            ++seen;
        }
    }
    return pos == 0 ? text : text.substr(pos + 1);
}

cml::Diag build_failed(const std::string& what, const fs::path& log) {
    return cml::Diag::make(cml::ids::kFixtureBuildFailed, what + "; last lines of " + log.string() + ":\n" + last_lines(log, 25),
                           "Open the log in the artifact directory; the fixture must build with the same compiler as the project.");
}

// Runs one cmake invocation; true if it exited with code 0 in time.
cml::Result<void> run_cmake(Supervisor& supervisor, const BuildSettings& settings, const std::string& label,
                            const std::wstring& arguments, std::uint32_t timeout_ms) {
    ChildSpec spec;
    spec.label = label;
    spec.exe = settings.cmake;
    spec.arguments = arguments;
    spec.working_directory = settings.work_dir;
    spec.output_file = settings.artifacts_dir / (label + ".log");
    cml::Result<std::size_t> started = supervisor.start(spec);
    if (!started) {
        return started.error();
    }
    if (!supervisor.wait_exit(*started, timeout_ms)) {
        supervisor.dump_running(settings.artifacts_dir / "dumps");
        supervisor.kill_all();
        return build_failed(label + " did not finish within " + std::to_string(timeout_ms / 1000) + " s (hung; dumps written)", spec.output_file);
    }
    const std::optional<std::uint32_t> code = supervisor.exit_code(*started);
    if (!code || *code != 0) {
        return build_failed(label + " failed with exit code " + std::to_string(code.value_or(0xFFFFFFFFu)), spec.output_file);
    }
    return cml::Result<void>();
}

bool copy_tree(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) {
        *error = "cannot create '" + to.string() + "': " + ec.message();
        return false;
    }
    // Plain loop instead of fs::copy(recursive): behaves the same on every standard library.
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path relative = it->path().lexically_relative(from);
        const fs::path target = to / relative;
        if (it->is_directory(ec)) {
            fs::create_directories(target, ec);
        } else if (it->is_regular_file(ec)) {
            fs::create_directories(target.parent_path(), ec);
            fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, ec);
            if (!ec) {
                // Checked-out files may be read-only; the edit step needs to write.
                fs::permissions(target, fs::perms::owner_write, fs::perm_options::add, ec);
            }
        }
        if (ec) {
            *error = "cannot copy '" + it->path().string() + "' to '" + target.string() + "': " + ec.message();
            return false;
        }
    }
    if (ec) {
        *error = "cannot read '" + from.string() + "': " + ec.message();
        return false;
    }
    return true;
}

bool has_c_sources(const fs::path& dir) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && lower(it->path().extension().string()) == ".c") {
            return true;
        }
    }
    return false;
}

std::wstring cmake_path(const fs::path& p) {
    // CMake wants forward slashes in -D values.
    std::wstring text = p.wstring();
    std::replace(text.begin(), text.end(), L'\\', L'/');
    return text;
}


// Copies cml_agent.dll next to the fixture exe.
cml::Result<BuiltFixture> finish_fixture(BuiltFixture* built_ptr, const BuildSettings& settings) {
    BuiltFixture& built = *built_ptr;
    std::error_code ec;
    const fs::path agent = settings.agent_dir / "cml_agent.dll";
    if (!fs::exists(agent, ec)) {
        return cml::Diag::make(cml::ids::kHarnessUsage, "cml_agent.dll not found at " + agent.string(),
                               "Build the project first and pass --build-dir (or --agent-dir).");
    }
    fs::copy_file(agent, built.exe.parent_path() / "cml_agent.dll", fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return cml::Diag::make(cml::ids::kFixtureBuildFailed, "cannot copy cml_agent.dll next to the fixture: " + ec.message(),
                               "Check that the work directory is writable.");
    }
    return built;
}

}  // namespace

cml::Result<fs::path> find_executable(const std::string& name) {
    wchar_t buffer[MAX_PATH * 2];
    const DWORD n = SearchPathW(nullptr, cml::proc::wide_from_utf8(name).c_str(), nullptr,
                                static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])), buffer, nullptr);
    if (n == 0 || n >= sizeof(buffer) / sizeof(buffer[0])) {
        return cml::Diag::make(cml::ids::kHarnessUsage, "cannot find " + name + " on PATH",
                               "Install it, run from a developer environment, or pass its full path with the matching option.");
    }
    return fs::path(buffer);
}

cml::Result<CompilerChoice> resolve_compiler(const std::string& cxx) {
    CompilerChoice choice;
    const std::string key = lower(cxx);
    std::string file;
    if (key == "cl" || key == "msvc") {
        file = "cl.exe";
        choice.display = "cl";
    } else if (key == "clang-cl") {
        file = "clang-cl.exe";
        choice.display = "clang-cl";
        choice.is_clang_cl = true;
    }
    if (!file.empty()) {
        wchar_t buffer[MAX_PATH * 2];
        const DWORD n = SearchPathW(nullptr, cml::proc::wide_from_utf8(file).c_str(), nullptr, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])), buffer, nullptr);
        if (n == 0 || n >= sizeof(buffer) / sizeof(buffer[0])) {
            return cml::Diag::make(cml::ids::kHarnessUsage, "cannot find " + file + " on PATH",
                                   "Run cmltest from a developer environment (vcvars) or pass --cxx with the full path of the compiler.");
        }
        choice.path = buffer;
        return choice;
    }
    choice.path = fs::path(cml::proc::wide_from_utf8(cxx));
    const std::string stem = lower(choice.path.stem().string());
    choice.is_clang_cl = stem == "clang-cl";
    choice.display = choice.is_clang_cl ? "clang-cl" : (stem == "cl" ? "cl" : stem);
    std::error_code ec;
    if (!fs::exists(choice.path, ec)) {
        return cml::Diag::make(cml::ids::kHarnessUsage, "compiler '" + cxx + "' does not exist",
                               "Pass --cxx cl, --cxx clang-cl or the full path of the compiler.");
    }
    return choice;
}

cml::Result<BuiltFixture> build_fixture(Supervisor& supervisor, const BuildSettings& settings) {
    BuiltFixture built;
    const fs::path fixture_source = settings.repo_root / "tests" / "fixtures" / settings.fixture;
    const fs::path wrapper = settings.repo_root / "tests" / "harness" / "fixture_wrapper";
    std::error_code ec;
    if (!fs::exists(fixture_source / "CMakeLists.txt", ec) || !fs::exists(wrapper / "CMakeLists.txt", ec)) {
        return cml::Diag::make(cml::ids::kHarnessUsage,
                               "fixture '" + settings.fixture + "' not found: expected " + (fixture_source / "CMakeLists.txt").string() +
                                   " and " + (wrapper / "CMakeLists.txt").string(),
                               "Check the scenario's 'fixture' key and --repo-root.");
    }

    built.source_dir = settings.work_dir / "fixture";
    built.build_dir = settings.work_dir / "build";
    std::string copy_error;
    if (!copy_tree(fixture_source, built.source_dir, &copy_error)) {
        return cml::Diag::make(cml::ids::kFixtureBuildFailed, copy_error, "Check that the work directory is writable.");
    }

    if (!settings.prebuilt_exe.empty()) {
        const fs::path dir = settings.work_dir / "prebuilt";
        fs::create_directories(dir, ec);
        built.exe = dir / settings.prebuilt_exe.filename();
        fs::copy_file(settings.prebuilt_exe, built.exe, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return cml::Diag::make(cml::ids::kHarnessUsage, "cannot copy the prebuilt fixture '" + settings.prebuilt_exe.string() + "': " + ec.message(),
                                   "Pass the path of an existing fixture executable with --prebuilt-fixture.");
        }
        built.build_dir = dir;
        return finish_fixture(&built, settings);
    }

    std::wstring configure = L"-S " + quote_argument(cmake_path(wrapper)) + L" -B " + quote_argument(cmake_path(built.build_dir));
    if (settings.generator == "ninja") {
        configure += L" -G Ninja -DCMAKE_BUILD_TYPE=" + cml::proc::wide_from_utf8(settings.config);
        configure += L" -DCMAKE_CXX_COMPILER=" + quote_argument(cmake_path(settings.compiler.path));
        if (has_c_sources(built.source_dir)) {
            // clang-cl and cl both compile C; use the same compiler family for C as for C++.
            configure += L" -DCMAKE_C_COMPILER=" + quote_argument(cmake_path(settings.compiler.path));
        }
    } else if (settings.generator == "msbuild") {
        std::string vs_generator = settings.vs_generator;
        if (vs_generator.empty()) {
            // Ask CMake which Visual Studio it would pick: the line marked `*` in `cmake --help`
            // ("* Visual Studio 18 2026 = ..."). The images carry different VS versions over time.
            ChildSpec help;
            help.label = "cmake-help";
            help.exe = settings.cmake;
            help.arguments = L"--help";
            help.working_directory = settings.work_dir;
            help.output_file = settings.artifacts_dir / "cmake-help.txt";
            cml::Result<std::size_t> started = supervisor.start(help);
            vs_generator = "Visual Studio 17 2022";
            if (started && supervisor.wait_exit(*started, 60000)) {
                std::istringstream lines(last_lines(help.output_file, 100000));
                std::string line;
                while (std::getline(lines, line)) {
                    if (line.rfind("* Visual Studio", 0) == 0) {
                        const std::size_t eq = line.find('=');
                        std::string name = line.substr(2, eq == std::string::npos ? std::string::npos : eq - 2);
                        while (!name.empty() && (name.back() == ' ' || name.back() == '\r')) {
                            name.pop_back();
                        }
                        if (!name.empty()) {
                            vs_generator = name;
                        }
                        break;
                    }
                }
            }
        }
        configure += L" -G " + quote_argument(cml::proc::wide_from_utf8(vs_generator)) + L" -A x64";
        if (settings.compiler.is_clang_cl) {
            configure += L" -T ClangCL";
        }
    } else {
        return cml::Diag::make(cml::ids::kHarnessUsage, "unknown generator '" + settings.generator + "'", "Use --generator ninja or --generator msbuild.");
    }
    configure += L" -DCML_REPO_ROOT=" + quote_argument(cmake_path(settings.repo_root));
    configure += L" -DCML_FIXTURE_DIR=" + quote_argument(cmake_path(built.source_dir));
    if (has_c_sources(built.source_dir)) {
        configure += L" -DCML_FIXTURE_ENABLE_C=ON";
    }

    cml::Result<void> configured = run_cmake(supervisor, settings, "cmake-configure", configure, settings.configure_timeout_ms);
    if (!configured) {
        return configured.error();
    }
    const std::wstring build = L"--build " + quote_argument(cmake_path(built.build_dir)) + L" --config " +
                               cml::proc::wide_from_utf8(settings.config) + L" --parallel";
    cml::Result<void> compiled = run_cmake(supervisor, settings, "cmake-build", build, settings.build_timeout_ms);
    if (!compiled) {
        return compiled.error();
    }

    // Single-config generators put the exe in bin/, the Visual Studio generator in bin/<config>/.
    const std::string exe_name = "fixture_" + settings.fixture + ".exe";
    for (const fs::path& candidate : {built.build_dir / "bin" / settings.config / exe_name, built.build_dir / "bin" / exe_name}) {
        if (fs::exists(candidate, ec)) {
            built.exe = candidate;
            break;
        }
    }
    if (built.exe.empty()) {
        return cml::Diag::make(cml::ids::kFixtureBuildFailed,
                               "the build succeeded but " + exe_name + " was not found below " + (built.build_dir / "bin").string(),
                               "Check cmake-build.log in the artifact directory.");
    }
    return finish_fixture(&built, settings);
}

}  // namespace cmltest

#endif  // _WIN32
