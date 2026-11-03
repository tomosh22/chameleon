// cml_broker: command-line entry point of the Chameleon broker.
//
//   cml_broker --version | --help
//   cml_broker --headless [--pipe NAME] [--control-pipe NAME] [--exit-when-idle MS] [--startup-timeout MS]
//                         [--log FILE] [--verbose]
//
// WP-11 provided the minimal headless broker (agents register, module lists are recorded, EnableModule is
// answered), WP-13 the control pipe, WP-12 the real thing behind it: the PDB index of enabled modules, the
// patch pipeline (`reload`), `wait`, `set-option`, `subscribe` and the diag / patch_applied / log events. The
// control pipe is now served under its default name unless --control-pipe / CML_CONTROL_PIPE name another one.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "cml/core/version.h"
#include "cml/util/log.h"
#include "cml/util/path.h"

#if defined(_WIN32)
#include <windows.h>

#include <shellapi.h>
#include <share.h>

#include <chrono>

#include "cml/core/connection_server.h"
#include "cml/core/log_hub.h"
#include <optional>

#include "cml/ipc/control.h"
#include "cml/ipc/pipe.h"
#include "cml/proc/proc.h"
#endif

namespace {

void print_usage(std::FILE* out) {
    std::fputs(
        "usage: cml_broker --version | --help\n"
        "       cml_broker --headless [--pipe NAME] [--control-pipe NAME] [--exit-when-idle MS]\n"
        "                             [--startup-timeout MS]\n"
        "                             [--log FILE] [--log-format text|json] [--verbose] [--session-dir DIR]\n"
        "\n"
        "  --headless            run without a GUI and serve agents on the broker pipe\n"
        "  --pipe NAME           full pipe name (default: CML_PIPE_NAME, else\n"
        "                        \\\\.\\pipe\\cml_broker_<logon session SID>)\n"
        "  --control-pipe NAME   serve the control protocol (docs/wp/WP-13.md, WP-12.md) on this full pipe\n"
        "                        name; CML_CONTROL_PIPE does the same. Without either the default name\n"
        "                        \\\\.\\pipe\\cml_control_<logon session SID> is served (if another broker\n"
        "                        already serves it, this one runs without a control pipe)\n"
        "  --exit-when-idle MS   exit MS milliseconds after the last agent disconnected\n"
        "  --startup-timeout MS  exit if no agent has connected after MS milliseconds\n"
        "  --log FILE            append the log to FILE instead of writing it to stdout\n"
        "  --log-format F        text (default) or json: one JSON object per log record\n"
        "  --verbose             also log debug messages\n"
        "  --session-dir DIR     keep objects, patch DLLs and PDBs of reloads in DIR (default:\n"
        "                        $CML_SESSION_DIR, else a temporary directory removed at exit)\n",
        out);
}

struct Options {
    bool version = false;
    bool help = false;
    bool headless = false;
    bool verbose = false;
    std::string pipe;
    std::string control_pipe;
    std::string log_file;
    std::string log_format = "text";
    std::string session_dir;
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
        } else if (arg == "--control-pipe") {
            if (!value_of("--control-pipe", &options->control_pipe)) return false;
        } else if (arg == "--log") {
            if (!value_of("--log", &options->log_file)) return false;
        } else if (arg == "--log-format") {
            if (!value_of("--log-format", &options->log_format)) return false;
            if (options->log_format != "text" && options->log_format != "json") {
                std::fprintf(stderr, "cml_broker: --log-format takes text or json\n");
                return false;
            }
        } else if (arg == "--session-dir") {
            if (!value_of("--session-dir", &options->session_dir)) return false;
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
    std::vector<std::string> args;
    int wide_argc = 0;
    LPWSTR* wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (wide_argv != nullptr) {
        for (int i = 0; i < wide_argc; ++i) {
            args.push_back(cml::proc::utf8_from_wide(wide_argv[i], std::wcslen(wide_argv[i])));
        }
        LocalFree(wide_argv);
    }
    return args;
#else
    return std::vector<std::string>(argv, argv + argc);
#endif
}

#if defined(_WIN32)

// Ctrl+C / console close: the handler runs on its own thread, possibly while the server is being
// torn down, so it touches nothing but two events that live as long as the process (never closed).
HANDLE g_stop_event = nullptr;
HANDLE g_done_event = nullptr;  // set when run() returned and the broker has shut down

// CTRL_CLOSE_EVENT, CTRL_LOGOFF_EVENT and CTRL_SHUTDOWN_EVENT end the process as soon as the handler returns (or
// after about 5 s): for these the handler waits until the server has stopped (FU-03), so that patch files, pipes
// and the log are not cut off in the middle of a reload. Ctrl+C and Ctrl+Break only ask for the stop.
BOOL WINAPI on_console_event(DWORD type) {
    if (g_stop_event != nullptr) {
        SetEvent(g_stop_event);
    }
    if ((type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) && g_done_event != nullptr) {
        WaitForSingleObject(g_done_event, 4500);
    }
    return TRUE;
}

int run_headless(const Options& options) {
    std::FILE* out = stdout;
    if (!options.log_file.empty()) {
        const std::wstring wide = cml::proc::wide_from_utf8(options.log_file);
        // _wfsopen, not _wfopen_s: the latter opens the file exclusively, and tests (and users) read
        // the log while the broker is still running.
        out = _wfsopen(wide.c_str(), L"ab", _SH_DENYWR);
        if (out == nullptr) {
            std::fprintf(stderr, "cml_broker: cannot open log file '%s'\n", options.log_file.c_str());
            return 1;
        }
    } else {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
    }
    cml::core::LogHub::Options hub_options;
    hub_options.file = out;
    hub_options.format = options.log_format == "json" ? cml::core::LogFormat::Json : cml::core::LogFormat::Text;
    cml::core::LogHub hub(hub_options);
    cml::set_log_sink(cml::core::LogHub::sink, &hub);
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
    // The control pipe is always served. A name given on the command line or in CML_CONTROL_PIPE is required
    // (the broker does not start without it); the default name is best effort, because several headless brokers
    // can run side by side (tests) and only the first can have the default.
    const std::optional<std::string> control_from_env = cml::proc::get_env("CML_CONTROL_PIPE");
    const bool control_requested = !options.control_pipe.empty() || (control_from_env && !control_from_env->empty());
    cml::Result<std::string> control_name = cml::ipc::resolve_control_pipe_name(options.control_pipe);
    if (!control_name) {
        CML_LOG(Error, "%s", control_name.error().format().c_str());
        return 1;
    }
    server_options.control_pipe_name = *control_name;
    server_options.control_pipe_optional = !control_requested;
    CML_LOG(Info, "control pipe=%s%s", control_name->c_str(), control_requested ? "" : " (default name)");
    server_options.session_dir = options.session_dir;
    server_options.exit_when_idle_ms = options.exit_when_idle_ms;
    server_options.startup_timeout_ms = options.startup_timeout_ms;
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_done_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    server_options.stop_event = g_stop_event;
    cml::core::ConnectionServer server(server_options);
    hub.attach_events(&server.event_bus(), cml::LogLevel::Info);  // `log` events for subscribed control clients
    SetConsoleCtrlHandler(on_console_event, TRUE);

    const cml::Result<cml::core::ServerExit> result = server.run();
    int exit_code = 0;
    if (!result) {
        CML_LOG(Error, "%s", result.error().format().c_str());
        exit_code = 1;
    } else if (*result == cml::core::ServerExit::StartupTimeout) {
        exit_code = 3;
    }
    CML_LOG(Info, "cml_broker exiting with code %d", exit_code);
    hub.attach_events(nullptr);
    cml::set_log_sink(nullptr, nullptr);  // the hub is a local of this function
    SetEvent(g_done_event);  // releases a console-close handler that is waiting for the shutdown
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
