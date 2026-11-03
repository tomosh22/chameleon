#pragma once

#include <windows.h>

#include <string>

namespace cml::ui::win {

struct AppOptions {
    std::string settings_path;  // empty = %APPDATA%\Chameleon\ui.json
    bool start_hidden = false;  // start in the tray without showing the window
};

// The window, tray icon, global hotkey and the ImGui front-end. Returns the process exit code.
int run_app(HINSTANCE instance, const AppOptions& options);

}  // namespace cml::ui::win
