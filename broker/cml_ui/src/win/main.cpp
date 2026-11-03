// cml_ui.exe: the Chameleon broker UI (ADR-0033). A client of the control pipe only.
//
//   cml_ui.exe                      the window (single instance; a second start brings the first forward)
//   cml_ui.exe --minimized          start in the tray
//   cml_ui.exe --settings FILE      use FILE instead of %APPDATA%\Chameleon\ui.json
//   cml_ui.exe --self-test --broker cml_broker.exe [--work-dir DIR]
//                                   no window: connect to a broker, run status, exercise the view model

#include <windows.h>

#include <shellapi.h>

#include <string>
#include <vector>

#include "app.h"
#include "cml/proc/proc.h"
#include "cml/ui/ui_diag_ids.h"
#include "cml/util/diag.h"
#include "self_test.h"

namespace {

constexpr const char* kUsage =
    "cml_ui [--minimized] [--settings FILE]\n"
    "cml_ui --self-test --broker PATH [--work-dir DIR]\n"
    "cml_ui --help | --version\n";

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; argv != nullptr && i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    if (argv != nullptr) {
        LocalFree(argv);
    }

    bool self_test = false;
    cml::ui::win::SelfTestOptions test_options;
    cml::ui::win::AppOptions app_options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::wstring& arg = args[i];
        const auto value = [&](std::wstring* out) {
            if (i + 1 >= args.size()) {
                cml::ui::win::write_stdout("cml_ui: " + cml::proc::utf8_from_wide(arg) + " needs a value\n");
                return false;
            }
            *out = args[++i];
            return true;
        };
        std::wstring text;
        if (arg == L"--self-test") {
            self_test = true;
        } else if (arg == L"--broker") {
            if (!value(&test_options.broker_path)) return 2;
        } else if (arg == L"--work-dir") {
            if (!value(&test_options.work_dir)) return 2;
        } else if (arg == L"--settings") {
            if (!value(&text)) return 2;
            app_options.settings_path = cml::proc::utf8_from_wide(text);
        } else if (arg == L"--minimized") {
            app_options.start_hidden = true;
        } else if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            cml::ui::win::write_stdout(kUsage);
            return 0;
        } else if (arg == L"--version") {
            cml::ui::win::write_stdout("cml_ui (Chameleon broker UI)\n");
            return 0;
        } else {
            cml::ui::win::write_stdout("cml_ui: unknown argument '" + cml::proc::utf8_from_wide(arg) + "'\n" + kUsage);
            return 2;
        }
    }
    if (self_test) {
        if (test_options.broker_path.empty()) {
            cml::ui::win::write_stdout(std::string("cml_ui: --self-test needs --broker PATH\n") + kUsage);
            return 2;
        }
        return cml::ui::win::run_self_test(test_options);
    }
    return cml::ui::win::run_app(instance, app_options);
}
