#pragma once

// Private helpers: UTF-8 <-> std::filesystem::path and a few non-throwing file operations (core/
// code is built without exceptions, so only the std::error_code overloads of <filesystem> are used).

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace cml::toolchain::detail {

inline std::filesystem::path to_path(std::string_view utf8) {
    const char8_t* p = reinterpret_cast<const char8_t*>(utf8.data());
    return std::filesystem::path(std::u8string(p, p + utf8.size()));
}

inline std::string to_utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

// "X:\..." , "\\server\..." , "/..." (a leading separator counts as absolute for portable tests).
inline bool is_absolute(std::string_view p) noexcept {
    if (p.size() >= 3 && p[1] == ':' && (p[2] == '\\' || p[2] == '/') &&
        ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'))) {
        return true;
    }
    return p.size() >= 2 && (p[0] == '\\' || p[0] == '/');
}

inline bool file_exists(std::string_view utf8) {
    std::error_code ec;
    return std::filesystem::is_regular_file(to_path(utf8), ec) && !ec;
}

inline char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = ascii_lower(c);
    }
    return out;
}

inline bool starts_with(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

inline bool ends_with(std::string_view s, std::string_view suffix) noexcept {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

}  // namespace cml::toolchain::detail
