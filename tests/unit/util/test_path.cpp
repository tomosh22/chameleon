#include "cml/util/path.h"

#include <string>
#include <string_view>

#include "doctest.h"

using namespace cml::path;

TEST_CASE("base_name and parent_directory handle both separators") {
    CHECK(base_name(std::string_view("C:\\a\\b.dll")) == "b.dll");
    CHECK(base_name(std::string_view("C:/a/b.dll")) == "b.dll");
    CHECK(base_name(std::string_view("b.dll")) == "b.dll");
    CHECK(base_name(std::string_view("C:\\a\\")).empty());
    CHECK(base_name(std::string_view("")).empty());
    CHECK(parent_directory(std::string_view("C:\\a\\b.dll")) == "C:\\a");
    CHECK(parent_directory(std::string_view("C:/a/b.dll")) == "C:/a");
    CHECK(parent_directory(std::string_view("b.dll")).empty());
    CHECK(has_separator(std::string_view("a/b")));
    CHECK_FALSE(has_separator(std::string_view("ab")));
}

TEST_CASE("the helpers work on wide strings too") {
    CHECK(base_name(std::wstring_view(L"C:\\x\\y.exe")) == L"y.exe");
    CHECK(parent_directory(std::wstring_view(L"C:\\x\\y.exe")) == L"C:\\x");
    CHECK(has_separator(std::wstring_view(L"/")));
    CHECK(last_separator(std::wstring_view(L"abc")) == std::wstring_view::npos);
}

TEST_CASE("same_path and matches_module") {
    CHECK(same_path("C:\\a\\b.dll", "c:/A/B.DLL"));
    CHECK(same_path("\\\\?\\C:\\a\\b.dll", "C:\\a\\b.dll"));
    CHECK_FALSE(same_path("C:\\a\\b.dll", "C:\\a\\c.dll"));
    CHECK(matches_module("game.dll", "C:\\Games\\Game.dll"));
    CHECK(matches_module("C:\\Games\\game.dll", "c:/games/Game.dll"));
    CHECK_FALSE(matches_module("C:\\Other\\game.dll", "C:\\Games\\Game.dll"));
    CHECK_FALSE(matches_module("other.dll", "C:\\Games\\Game.dll"));
}
