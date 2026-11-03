#pragma once

// UI settings, stored as JSON in %APPDATA%\Chameleon\ui.json (the caller supplies the path; this file does no
// path discovery). Loading is forgiving: a missing file gives the defaults, unknown members are ignored and a
// member with a bad value keeps its default and adds a warning. Saving writes a temporary file and renames it.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cml/ipc/json.h"
#include "cml/ui/editor.h"
#include "cml/util/result.h"

namespace cml::ui {

// Windows virtual-key codes are used as plain numbers so that this header stays portable.
struct Hotkey {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool win = false;
    std::uint32_t vk = 0;  // 0 = no hotkey

    bool valid() const noexcept { return vk != 0 && (ctrl || alt || shift || win); }
    friend bool operator==(const Hotkey& a, const Hotkey& b) noexcept {
        return a.ctrl == b.ctrl && a.alt == b.alt && a.shift == b.shift && a.win == b.win && a.vk == b.vk;
    }
};

// "Ctrl+Alt+F11", "Shift+Win+R", "Ctrl+Alt+0". Letters, digits, F1..F24 and a few named keys. CML5710 on bad text.
Result<Hotkey> parse_hotkey(std::string_view text);
std::string format_hotkey(const Hotkey& hotkey);

inline constexpr std::uint32_t kVkF11 = 0x7A;

struct Settings {
    Hotkey reload_hotkey{true, true, false, false, kVkF11};  // Ctrl+Alt+F11 sends `reload`
    bool hotkey_enabled = true;
    bool compile_on_save = false;        // set-option compile_on_save
    int jobs = 0;                        // set-option max_parallel_compiles; 0 = leave the broker's value
    int max_affected_compilands = -1;    // set-option max_affected_compilands; -1 = leave, 0 = no limit
    EditorConfig editor;
    std::string min_log_level = "info";  // log view filter: trace|debug|info|warn|error
    bool minimize_to_tray = true;        // the close button hides the window to the tray
    bool start_broker = true;            // start cml_broker.exe next to cml_ui.exe when none answers
    std::string control_pipe;            // override of the control pipe name (empty = default)
    std::string broker_path;             // override of the cml_broker.exe path (empty = next to cml_ui.exe)
};

ipc::json::Value settings_to_json(const Settings& settings);
// Never fails; `warnings` (optional) receives one line per ignored member.
Settings settings_from_json(const ipc::json::Value& value, std::vector<std::string>* warnings = nullptr);

// Missing file: defaults. Unreadable or not JSON: CML5700.
Result<Settings> load_settings(const std::string& path, std::vector<std::string>* warnings = nullptr);
// CML5701.
Result<void> save_settings(const std::string& path, const Settings& settings);

// The `set-option` calls that make the broker match the settings: {name, value}. Options at their "leave it"
// value (jobs 0, max_affected_compilands -1) are not listed.
struct OptionSetting {
    std::string name;
    ipc::json::Value value;
};
std::vector<OptionSetting> settings_broker_options(const Settings& settings);

}  // namespace cml::ui
