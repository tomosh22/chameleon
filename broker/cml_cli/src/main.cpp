// cml_broker: command-line entry point of the Chameleon broker.
//
//   cml_broker --version | --help
//   cml_broker --headless [--pipe NAME] [--exit-when-idle MS] [--startup-timeout MS]
//                         [--log FILE] [--verbose]
//
// WP-11 provides only the minimal headless broker (agents register, module lists are recorded,
// EnableModule is answered); the real broker, control pipe and GUI arrive with WP-12/WP-13.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "cml/core/version.h"
#include "cml/util/log.h"

#if defined(_WIN32)
#include <windows.h>

#include <chrono>

#include "cml/core/connection_server.h"
#include "cml/ipc/pipe.h"
#include "cml/proc/proc.h"
#endif

namespace {

void print_usage(std::FILE* out) {
    std::fputs(
        "usage: cml_broker --version | --help\n"
        "       cml_broker --headless [--pipe NAME] [--exit-when-idle MS] [--startup-timeout MS]\n"
        "                             [--log FILE] [--verbose]\n"
        "\n"
        "  --headless            run without a GUI and serve agents on the broker pipe\n"
        "  --pipe NAME           full pipe name (default: CML_PIPE_NAME, else\n"
        "                        \\\\.\\pipe\\cml_broker_<logon session SID>)\n"
        "  --exit-when-idle MS   exit MS milliseconds after the last agent disconnected\n"
        "  --startup-timeout MS  exit if no agent has connected after MS milliseconds\n"
        "  --log FILE            append the log to FILE instead of writing it to stdout\n"
        "  --verbose             also log debug messages\n",
        out);
}

struct Options {
    bool version = false;
    bool help = false;
    bool headless = false;
    bool verbose = false;
    std::string pipe;
    std::string log_file;
    std::uint32_t exit_when_idle_ms = 0;
    std::uint32_t startup_timeout_ms = 0;
};

bool parse_milliseconds(const std::string& text, std::uint32_t* out) {
    if (text.empty() || text.size() > 10 || text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    const unsigned long long value = std::strtoull(text.c_str(), nullptr, 10);
    if (value > 0xFFFFFFFEull) {
        return false;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
}

// Returns false (after printing why) for anything it does not understand.
bool parse_args(const std::vector<std::string>& args, Options* options) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto value_of = [&](const char* name, std::string* out) {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "cml_broker: %s needs a value\n", name);
                return false;
            }
            *out = args[++i];
            return true;
        };
        std::string text;
        if (arg == "--version") {
            options->version = true;
        } else if (arg == "--help") {
            options->help = true;
        } else if (arg == "--headless") {
            options->headless = true;
        } else if (arg == "--verbose") {
            options->verbose = true;
        } else if (arg == "--pipe") {
            if (!value_of("--pipe", &options->pipe)) return false;
        } else if (arg == "--log") {
            if (!value_of("--log", &options->log_file)) return false;
        } else if (arg == "--exit-when-idle") {
            if (!value_of("--exit-when-idle", &text) ||
                !parse_milliseconds(text, &options->exit_when_idle_ms)) {
                std::fprintf(stderr, "cml_broker: --exit-when-idle needs a number of milliseconds\n");
                return false;
            }
        } else if (arg == "--startup-timeout") {
            if (!value_of("--startup-timeout", &text) ||
                !parse_milliseconds(text, &options->startup_timeout_ms)) {
                std::fprintf(stderr, "cml_broker: --startup-timeout needs a number of milliseconds\n");
                return false;
            }
        } else {
            std::fprintf(stderr, "cml_broker: unknown option '%s'\n", arg.c_str());
            return false;
        }
    }
    return true;
}

std::vector<std::string> collect_args(int argc, char** argv) {
#if defined(_WIN32)
    // The narrow argv uses the ANSI code page; paths in --log must survive, so use UTF-16.
    (void)argc;
    (void)argv;
    return cml::proc::command_line_arguments();
#else
    return std::vector<std::string>(argv, argv + argc);
#endif
}

#if defined(_WIN32)

// Log sink: one line per message, flushed immediately so that tests and users can tail the file.
struct LogOutput {
    std::FILE* file = nullptr;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};
LogOutput g_log_output;

const char* base_name(const char* path) {
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

void write_log_line(void* user, cml::LogLevel level, const char* file, int line, const char* message) {
    auto* out = static_cast<LogOutput*>(user);
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - out->start)
                             .count();
    std::fprintf(out->file, "[%7lld ms] [%s] %s:%d: %s\n", ms, cml::to_string(level), base_name(file),
                 line, message);
    std::fflush(out->file);
}

cml::core::ConnectionServer* g_server = nullptr;

BOOL WINAPI on_console_event(DWORD) {
    if (g_server != nullptr) {
        g_server->stop();
    }
    return TRUE;
}

int run_headless(const Options& options) {
    g_log_output.file = stdout;
    if (!options.log_file.empty()) {
        const std::wstring wide = cml::proc::wide_from_utf8(options.log_file);
        std::FILE* file = nullptr;
        if (_wfopen_s(&file, wide.c_str(), L"ab") != 0 || file == nullptr) {
            std::fprintf(stderr, "cml_broker: cannot open log file '%s'\n", options.log_file.c_str());
            return 1;
        }
        g_log_output.file = file;
    } else {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
    }
    cml::set_log_sink(write_log_line, &g_log_output);
    cml::set_log_level(options.verbose ? cml::LogLevel::Debug : cml::LogLevel::Info);

    cml::Result<std::string> pipe_name = cml::ipc::resolve_pipe_name(options.pipe);
    if (!pipe_name) {
        CML_LOG(Error, "%s", pipe_name.error().format().c_str());
        return 1;
    }
    CML_LOG(Info, "cml_broker %s pid=%u pipe=%s", cml::core::version_string(),
            cml::proc::current_process_id(), pipe_name->c_str());

    cml::core::ServerOptions server_options;
    server_options.pipe_name = *pipe_name;
    server_options.exit_when_idle_ms = options.exit_when_idle_ms;
    server_options.startup_timeout_ms = options.startup_timeout_ms;
    cml::core::ConnectionServer server(server_options);
    g_server = &server;
    SetConsoleCtrlHandler(on_console_event, TRUE);

    const cml::Result<cml::core::ServerExit> result = server.run();
    SetConsoleCtrlHandler(on_console_event, FALSE);
    g_server = nullptr;
    int exit_code = 0;
    if (!result) {
        CML_LOG(Error, "%s", result.error().format().c_str());
        exit_code = 1;
    } else if (*result == cml::core::ServerExit::StartupTimeout) {
        exit_code = 3;
    }
    CML_LOG(Info, "cml_broker exiting with code %d", exit_code);
    return exit_code;
}

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_args(collect_args(argc, argv), &options)) {
        print_usage(stderr);
        return 2;
    }
    if (options.version) {
        std::printf("cml_broker %s\n", cml::core::version_string());
        return 0;
    }
    if (options.help) {
        print_usage(stdout);
        return 0;
    }
    if (options.headless) {
#if defined(_WIN32)
        return run_headless(options);
#else
        std::fputs("cml_broker: --headless is only available on Windows\n", stderr);
        return 1;
#endif
    }
    print_usage(stderr);
    return 2;
}
