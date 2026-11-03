#pragma once

#include <string>

namespace cml::ui::win {

struct SelfTestOptions {
    std::wstring broker_path;  // cml_broker.exe to start
    std::wstring work_dir;     // scratch directory (settings round trip, broker log)
};

// `cml_ui --self-test`: starts the given broker on private pipes, connects, runs `status`, drives the controller and
// the view model through the same code paths as the window (events included), and checks the command table against
// the capabilities the broker advertises. Creates no window. Returns 0 when every check passed.
int run_self_test(const SelfTestOptions& options);

// Writes UTF-8 text to the process's standard output (a GUI-subsystem process has no console; this uses the handle
// the parent gave it, which is what ctest captures).
void write_stdout(const std::string& text);

}  // namespace cml::ui::win
