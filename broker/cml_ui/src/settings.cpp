#include "cml/ui/settings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cml/ui/ui_diag_ids.h"

namespace cml::ui {

namespace json = ipc::json;

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

struct NamedKey {
    const char* name;
    std::uint32_t vk;
};

constexpr NamedKey kNamedKeys[] = {
    {"space", 0x20}, {"enter", 0x0D}, {"return", 0x0D}, {"tab", 0x09},    {"esc", 0x1B},   {"escape", 0x1B}, {"backspace", 0x08},
    {"insert", 0x2D}, {"delete", 0x2E}, {"home", 0x24},  {"end", 0x23},    {"pageup", 0x21}, {"pagedown", 0x22}, {"left", 0x25},
    {"up", 0x26},    {"right", 0x27},  {"down", 0x28},   {"pause", 0x13},  {"scrolllock", 0x91},
};

// std::filesystem::u8path is deprecated in C++20 (and /WX turns that into an error): build the path from UTF-8 by hand.
std::filesystem::path path_from_utf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

Diag hotkey_error(std::string_view text, const std::string& why) {
    return Diag::make(ids::kUiHotkeyText, "hotkey '" + std::string(text) + "' is not valid: " + why,
                      "Write modifiers and one key joined by '+', for example Ctrl+Alt+F11.");
}

bool valid_level(const std::string& level) {
    return level == "trace" || level == "debug" || level == "info" || level == "warn" || level == "error";
}

}  // namespace

Result<Hotkey> parse_hotkey(std::string_view text) {
    Hotkey hotkey;
    std::size_t begin = 0;
    bool have_key = false;
    while (begin <= text.size()) {
        std::size_t end = text.find('+', begin);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string part = lower(text.substr(begin, end - begin));
        part.erase(std::remove(part.begin(), part.end(), ' '), part.end());
        if (part.empty()) {
            return hotkey_error(text, "empty part");
        }
        if (have_key) {
            return hotkey_error(text, "the key must come last");
        }
        if (part == "ctrl" || part == "control") {
            hotkey.ctrl = true;
        } else if (part == "alt") {
            hotkey.alt = true;
        } else if (part == "shift") {
            hotkey.shift = true;
        } else if (part == "win" || part == "super" || part == "meta") {
            hotkey.win = true;
        } else {
            have_key = true;
            if (part.size() == 1 && part[0] >= 'a' && part[0] <= 'z') {
                hotkey.vk = static_cast<std::uint32_t>(part[0] - 'a' + 'A');
            } else if (part.size() == 1 && part[0] >= '0' && part[0] <= '9') {
                hotkey.vk = static_cast<std::uint32_t>(part[0]);
            } else if (part[0] == 'f' && part.size() >= 2 && part.size() <= 3 &&
                       std::all_of(part.begin() + 1, part.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                const int n = std::atoi(part.c_str() + 1);
                if (n < 1 || n > 24) {
                    return hotkey_error(text, "function keys are F1 to F24");
                }
                hotkey.vk = 0x70u + static_cast<std::uint32_t>(n - 1);
            } else {
                bool found = false;
                for (const NamedKey& k : kNamedKeys) {
                    if (part == k.name) {
                        hotkey.vk = k.vk;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    return hotkey_error(text, "unknown key '" + part + "'");
                }
            }
        }
        if (end == text.size()) {
            break;
        }
        begin = end + 1;
    }
    if (!have_key) {
        return hotkey_error(text, "no key");
    }
    if (!(hotkey.ctrl || hotkey.alt || hotkey.shift || hotkey.win)) {
        return hotkey_error(text, "a global hotkey needs at least one modifier");
    }
    return hotkey;
}

std::string format_hotkey(const Hotkey& hotkey) {
    if (hotkey.vk == 0) {
        return "";
    }
    std::string out;
    if (hotkey.ctrl) out += "Ctrl+";
    if (hotkey.alt) out += "Alt+";
    if (hotkey.shift) out += "Shift+";
    if (hotkey.win) out += "Win+";
    if (hotkey.vk >= 0x70 && hotkey.vk <= 0x87) {
        out += "F" + std::to_string(hotkey.vk - 0x70 + 1);
    } else if ((hotkey.vk >= 'A' && hotkey.vk <= 'Z') || (hotkey.vk >= '0' && hotkey.vk <= '9')) {
        out.push_back(static_cast<char>(hotkey.vk));
    } else {
        bool found = false;
        for (const NamedKey& k : kNamedKeys) {
            if (k.vk == hotkey.vk) {
                std::string name = k.name;
                name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
                out += name;
                found = true;
                break;
            }
        }
        if (!found) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "0x%02X", static_cast<unsigned>(hotkey.vk));
            out += buf;
        }
    }
    return out;
}

json::Value settings_to_json(const Settings& s) {
    json::Value v = json::Value::object();
    v.set("version", json::Value::integer(1));
    v.set("reload_hotkey", json::Value::string(format_hotkey(s.reload_hotkey)));
    v.set("hotkey_enabled", json::Value::boolean(s.hotkey_enabled));
    v.set("compile_on_save", json::Value::boolean(s.compile_on_save));
    v.set("jobs", json::Value::integer(s.jobs));
    v.set("max_affected_compilands", json::Value::integer(s.max_affected_compilands));
    json::Value editor = json::Value::object();
    editor.set("kind", json::Value::string(to_string(s.editor.kind)));
    editor.set("custom_template", json::Value::string(s.editor.custom_template));
    editor.set("program", json::Value::string(s.editor.program));
    v.set("editor", std::move(editor));
    v.set("min_log_level", json::Value::string(s.min_log_level));
    v.set("minimize_to_tray", json::Value::boolean(s.minimize_to_tray));
    v.set("start_broker", json::Value::boolean(s.start_broker));
    v.set("control_pipe", json::Value::string(s.control_pipe));
    v.set("broker_path", json::Value::string(s.broker_path));
    return v;
}

Settings settings_from_json(const json::Value& v, std::vector<std::string>* warnings) {
    Settings s;
    const auto warn = [&](const std::string& text) {
        if (warnings != nullptr) {
            warnings->push_back(text);
        }
    };
    if (!v.is_object()) {
        warn("ui.json: the top level is not an object; defaults are used");
        return s;
    }
    const auto boolean = [&](const char* key, bool* out) {
        if (const json::Value* m = v.find(key)) {
            if (m->is_bool()) {
                *out = m->as_bool();
            } else {
                warn(std::string("ui.json: '") + key + "' must be true or false; kept the default");
            }
        }
    };
    const auto text = [&](const json::Value& object, const char* key, std::string* out) {
        if (const json::Value* m = object.find(key)) {
            if (m->is_string()) {
                *out = m->as_string();
            } else {
                warn(std::string("ui.json: '") + key + "' must be a string; kept the default");
            }
        }
    };
    const auto number = [&](const char* key, int lo, int hi, int* out) {
        if (const json::Value* m = v.find(key)) {
            if (m->is_integer() && m->as_integer() >= lo && m->as_integer() <= hi) {
                *out = static_cast<int>(m->as_integer());
            } else {
                warn(std::string("ui.json: '") + key + "' must be an integer from " + std::to_string(lo) + " to " + std::to_string(hi) +
                     "; kept the default");
            }
        }
    };
    if (const json::Value* m = v.find("reload_hotkey")) {
        if (m->is_string()) {
            if (m->as_string().empty()) {
                s.reload_hotkey = Hotkey{};
                s.hotkey_enabled = false;
            } else if (Result<Hotkey> h = parse_hotkey(m->as_string())) {
                s.reload_hotkey = *h;
            } else {
                warn("ui.json: " + h.error().message + "; kept the default");
            }
        } else {
            warn("ui.json: 'reload_hotkey' must be a string; kept the default");
        }
    }
    boolean("hotkey_enabled", &s.hotkey_enabled);
    boolean("compile_on_save", &s.compile_on_save);
    number("jobs", 0, 256, &s.jobs);
    number("max_affected_compilands", -1, 1000000, &s.max_affected_compilands);
    if (const json::Value* e = v.find("editor")) {
        if (e->is_object()) {
            std::string kind;
            text(*e, "kind", &kind);
            if (!kind.empty() && !editor_kind_from_string(kind, &s.editor.kind)) {
                warn("ui.json: unknown editor kind '" + kind + "'; kept the default");
            }
            text(*e, "custom_template", &s.editor.custom_template);
            text(*e, "program", &s.editor.program);
        } else {
            warn("ui.json: 'editor' must be an object; kept the default");
        }
    }
    std::string level = s.min_log_level;
    text(v, "min_log_level", &level);
    if (valid_level(level)) {
        s.min_log_level = level;
    } else {
        warn("ui.json: unknown min_log_level '" + level + "'; kept the default");
    }
    boolean("minimize_to_tray", &s.minimize_to_tray);
    boolean("start_broker", &s.start_broker);
    text(v, "control_pipe", &s.control_pipe);
    text(v, "broker_path", &s.broker_path);
    return s;
}

Result<Settings> load_settings(const std::string& path, std::vector<std::string>* warnings) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path file = path_from_utf8(path);
    if (!fs::exists(file, ec)) {
        return Settings{};
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return Diag::make(ids::kUiSettingsRead, "cannot read the settings file '" + path + "'",
                          "Check the file permissions; delete the file to go back to the defaults.");
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        // A UTF-8 byte order mark written by a text editor.
        Result<json::Value> parsed = json::parse(std::string_view(text).substr(3));
        if (!parsed) {
            return Diag::make(ids::kUiSettingsRead, "the settings file '" + path + "' is not valid JSON: " + parsed.error().message,
                              "Fix the file by hand or delete it to go back to the defaults.");
        }
        return settings_from_json(*parsed, warnings);
    }
    Result<json::Value> parsed = json::parse(text);
    if (!parsed) {
        return Diag::make(ids::kUiSettingsRead, "the settings file '" + path + "' is not valid JSON: " + parsed.error().message,
                          "Fix the file by hand or delete it to go back to the defaults.");
    }
    return settings_from_json(*parsed, warnings);
}

Result<void> save_settings(const std::string& path, const Settings& settings) {
    namespace fs = std::filesystem;
    const auto fail = [&](const std::string& why) {
        return Diag::make(ids::kUiSettingsWrite, "cannot write the settings file '" + path + "': " + why,
                          "Check that the folder exists and is writable; the settings stay in effect until the UI closes.");
    };
    std::error_code ec;
    const fs::path file = path_from_utf8(path);
    if (file.has_parent_path()) {
        fs::create_directories(file.parent_path(), ec);
        if (ec) {
            return fail(ec.message());
        }
    }
    const fs::path temporary = path_from_utf8(path + ".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return fail("cannot create the temporary file");
        }
        out << json::serialize(settings_to_json(settings)) << '\n';
        out.flush();
        if (!out) {
            return fail("write error");
        }
    }
    fs::rename(temporary, file, ec);
    if (ec) {
        // Windows refuses to rename over an existing file on some file systems: replace it explicitly.
        std::error_code ignore;
        fs::remove(file, ignore);
        fs::rename(temporary, file, ec);
        if (ec) {
            fs::remove(temporary, ignore);
            return fail(ec.message());
        }
    }
    return {};
}

std::vector<OptionSetting> settings_broker_options(const Settings& s) {
    std::vector<OptionSetting> out;
    out.push_back({"compile_on_save", json::Value::boolean(s.compile_on_save)});
    if (s.jobs > 0) {
        out.push_back({"max_parallel_compiles", json::Value::integer(s.jobs)});
    }
    if (s.max_affected_compilands >= 0) {
        out.push_back({"max_affected_compilands", json::Value::integer(s.max_affected_compilands)});
    }
    return out;
}

}  // namespace cml::ui
