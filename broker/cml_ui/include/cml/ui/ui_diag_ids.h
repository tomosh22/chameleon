#pragma once

#include <cstdint>

// Diagnostic ids CML5700..5749, owned by WP-19 (the broker UI, cml_ui). ADR-0032 assigns the range;
// docs/wp/WP-19.md repeats every id with its "what to do" text.
namespace cml::ids {

inline constexpr std::uint32_t kUiSettingsRead = 5700;        // ui.json cannot be read or is not valid JSON
inline constexpr std::uint32_t kUiSettingsWrite = 5701;       // ui.json cannot be written
inline constexpr std::uint32_t kUiFeatureUnavailable = 5702;  // the broker does not provide the command (capability missing / CML5402)
inline constexpr std::uint32_t kUiEditorTemplate = 5703;      // the editor command template is invalid
inline constexpr std::uint32_t kUiEditorLaunch = 5704;        // the editor could not be started
inline constexpr std::uint32_t kUiBrokerStart = 5705;         // cml_broker.exe could not be started
inline constexpr std::uint32_t kUiHotkey = 5706;              // the global hotkey could not be registered
inline constexpr std::uint32_t kUiSelfTest = 5707;            // a --self-test check failed
inline constexpr std::uint32_t kUiBrokerLost = 5708;          // the control connection to the broker was lost
inline constexpr std::uint32_t kUiTray = 5709;                // the tray icon could not be created (warning)
inline constexpr std::uint32_t kUiHotkeyText = 5710;          // a hotkey string cannot be parsed
inline constexpr std::uint32_t kUiWindow = 5711;              // window or Direct3D 11 device could not be created
inline constexpr std::uint32_t kUiEditorBatchFile = 5712;     // the editor resolves to a batch file, which is never started
inline constexpr std::uint32_t kUiUsage = 5713;               // bad command line

}  // namespace cml::ids
