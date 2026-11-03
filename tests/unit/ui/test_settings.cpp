#include "cml/ui/settings.h"

#include <filesystem>
#include <fstream>

#include "doctest.h"

using namespace cml::ui;
namespace json = cml::ipc::json;

namespace {

std::string temp_file(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "cml_ui_test_settings";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path p = dir / name;
    std::filesystem::remove(p, ec);
    return p.string();
}

}  // namespace

TEST_CASE("hotkey parse and format") {
    const auto h = parse_hotkey("Ctrl+Alt+F11");
    REQUIRE(h);
    CHECK(h->ctrl);
    CHECK(h->alt);
    CHECK_FALSE(h->shift);
    CHECK(h->vk == 0x7A);
    CHECK(format_hotkey(*h) == "Ctrl+Alt+F11");
    CHECK(format_hotkey(*parse_hotkey("shift + win + r")) == "Shift+Win+R");
    CHECK(format_hotkey(*parse_hotkey("ctrl+alt+0")) == "Ctrl+Alt+0");
    CHECK(format_hotkey(*parse_hotkey("Alt+Space")) == "Alt+Space");
    CHECK(parse_hotkey("Ctrl+F24")->vk == 0x87);
}

TEST_CASE("bad hotkeys are CML5710") {
    for (const char* bad : {"", "F11", "Ctrl+", "Ctrl+Alt", "Ctrl+Banana", "Ctrl+F25", "F5+Ctrl", "Ctrl+A+B", "+"}) {
        INFO(bad);
        const auto h = parse_hotkey(bad);
        REQUIRE_FALSE(h);
        CHECK(h.error().code == 5710);
    }
}

TEST_CASE("defaults: Ctrl+Alt+F11, compile on save off") {
    const Settings s;
    CHECK(format_hotkey(s.reload_hotkey) == "Ctrl+Alt+F11");
    CHECK(s.hotkey_enabled);
    CHECK_FALSE(s.compile_on_save);
    CHECK(s.editor.kind == EditorKind::VSCode);
    CHECK(s.start_broker);
}

TEST_CASE("settings survive a save and a load") {
    Settings s;
    s.reload_hotkey = *parse_hotkey("Ctrl+Shift+F9");
    s.compile_on_save = true;
    s.jobs = 6;
    s.max_affected_compilands = 40;
    s.editor.kind = EditorKind::Custom;
    s.editor.custom_template = R"("C:\ed\ed.exe" {file}:{line})";
    s.editor.program = R"(C:\ed\ed.exe)";
    s.min_log_level = "warn";
    s.minimize_to_tray = false;
    s.start_broker = false;
    s.control_pipe = R"(\\.\pipe\x)";
    s.broker_path = R"(C:\b\cml_broker.exe)";
    const std::string path = temp_file("roundtrip.json");
    REQUIRE(save_settings(path, s));
    std::vector<std::string> warnings;
    const auto back = load_settings(path, &warnings);
    REQUIRE(back);
    CHECK(warnings.empty());
    CHECK(back->reload_hotkey == s.reload_hotkey);
    CHECK(back->compile_on_save);
    CHECK(back->jobs == 6);
    CHECK(back->max_affected_compilands == 40);
    CHECK(back->editor.kind == EditorKind::Custom);
    CHECK(back->editor.custom_template == s.editor.custom_template);
    CHECK(back->editor.program == s.editor.program);
    CHECK(back->min_log_level == "warn");
    CHECK_FALSE(back->minimize_to_tray);
    CHECK_FALSE(back->start_broker);
    CHECK(back->control_pipe == s.control_pipe);
    CHECK(back->broker_path == s.broker_path);
    // Saving again over the existing file works.
    s.jobs = 2;
    REQUIRE(save_settings(path, s));
    CHECK(load_settings(path)->jobs == 2);
}

TEST_CASE("a missing file gives the defaults, a broken one CML5700") {
    const auto missing = load_settings(temp_file("does_not_exist.json"));
    REQUIRE(missing);
    CHECK(format_hotkey(missing->reload_hotkey) == "Ctrl+Alt+F11");

    const std::string path = temp_file("broken.json");
    {
        std::ofstream out(path);
        out << "{ not json";
    }
    const auto broken = load_settings(path);
    REQUIRE_FALSE(broken);
    CHECK(broken.error().code == 5700);
}

TEST_CASE("bad members keep their defaults and warn; unknown members are ignored") {
    const auto parsed = json::parse(R"({"reload_hotkey":"Banana","compile_on_save":"yes","jobs":-3,"min_log_level":"loud",
        "editor":{"kind":"emacs","custom_template":5},"future_member":[1,2],"start_broker":false})");
    REQUIRE(parsed);
    std::vector<std::string> warnings;
    const Settings s = settings_from_json(*parsed, &warnings);
    CHECK(format_hotkey(s.reload_hotkey) == "Ctrl+Alt+F11");
    CHECK_FALSE(s.compile_on_save);
    CHECK(s.jobs == 0);
    CHECK(s.min_log_level == "info");
    CHECK(s.editor.kind == EditorKind::VSCode);
    CHECK_FALSE(s.start_broker);
    CHECK(warnings.size() == 6);
}

TEST_CASE("an empty hotkey string disables the hotkey") {
    const auto parsed = json::parse(R"({"reload_hotkey":""})");
    REQUIRE(parsed);
    const Settings s = settings_from_json(*parsed);
    CHECK_FALSE(s.hotkey_enabled);
}

TEST_CASE("settings become set-option calls") {
    Settings s;
    auto options = settings_broker_options(s);
    REQUIRE(options.size() == 1);
    CHECK(options[0].name == "compile_on_save");
    CHECK_FALSE(options[0].value.as_bool(true));
    s.compile_on_save = true;
    s.jobs = 4;
    s.max_affected_compilands = 0;
    options = settings_broker_options(s);
    REQUIRE(options.size() == 3);
    CHECK(options[1].name == "max_parallel_compiles");
    CHECK(options[1].value.as_integer() == 4);
    CHECK(options[2].name == "max_affected_compilands");
    CHECK(options[2].value.as_integer() == 0);
}
