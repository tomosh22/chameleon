#pragma once

// Small string-level path helpers shared by the agent, the broker and the libraries. They treat
// '\' and '/' alike and never touch the file system. Templates over the character type so that
// both UTF-8 (std::string) and UTF-16 (std::wstring) paths use the same code.

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace cml::path {

template <class Char>
constexpr bool is_separator(Char c) noexcept {
    return c == Char('\\') || c == Char('/');
}

// Index of the last separator, or npos.
template <class Char>
constexpr std::size_t last_separator(std::basic_string_view<Char> path) noexcept {
    for (std::size_t i = path.size(); i > 0; --i) {
        if (is_separator(path[i - 1])) {
            return i - 1;
        }
    }
    return std::basic_string_view<Char>::npos;
}

template <class Char>
constexpr bool has_separator(std::basic_string_view<Char> path) noexcept {
    return last_separator(path) != std::basic_string_view<Char>::npos;
}

// "C:\a\b.dll" -> "b.dll"; a path without separator is returned unchanged.
template <class Char>
constexpr std::basic_string_view<Char> base_name(std::basic_string_view<Char> path) noexcept {
    const std::size_t pos = last_separator(path);
    return pos == std::basic_string_view<Char>::npos ? path : path.substr(pos + 1);
}

// "C:\a\b.dll" -> "C:\a"; empty when there is no separator.
template <class Char>
constexpr std::basic_string_view<Char> parent_directory(std::basic_string_view<Char> path) noexcept {
    const std::size_t pos = last_separator(path);
    return pos == std::basic_string_view<Char>::npos ? std::basic_string_view<Char>() : path.substr(0, pos);
}

// Compares two paths the way Windows would for display purposes: '/' equals '\', a leading
// "\\?\" is ignored and ASCII letters compare case-insensitively.
inline bool same_path(std::string_view a, std::string_view b) noexcept {
    constexpr std::string_view kExtended = "\\\\?\\";
    if (a.size() > kExtended.size() && a.substr(0, kExtended.size()) == kExtended) {
        a.remove_prefix(kExtended.size());
    }
    if (b.size() > kExtended.size() && b.substr(0, kExtended.size()) == kExtended) {
        b.remove_prefix(kExtended.size());
    }
    const auto fold = [](char c) {
        if (c == '/') return '\\';
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [&fold](char x, char y) { return fold(x) == fold(y); });
}

// Does `query` (a full path, or a bare file name such as "Game.dll") name the module at
// `full_path`? A bare name matches by file name; anything with a directory must be the same path.
inline bool matches_module(std::string_view query, std::string_view full_path) noexcept {
    return has_separator(query) ? same_path(full_path, query) : same_path(base_name(full_path), query);
}

}  // namespace cml::path
