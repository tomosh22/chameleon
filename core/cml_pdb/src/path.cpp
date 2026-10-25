#include <string>
#include <string_view>
#include <vector>

#include "cml/pdb/index.h"

namespace cml::pdb {

namespace {

char lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool has_drive(std::string_view s) noexcept {
    return s.size() >= 2 && s[1] == ':' &&
           ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'));
}

// Separator-normalised copy (forward slashes become backslashes).
std::string with_backslashes(std::string_view path) {
    std::string s(path);
    for (char& c : s) {
        if (c == '/') {
            c = '\\';
        }
    }
    return s;
}

void strip_extended_prefix(std::string& s) {
    if (s.rfind("\\\\?\\UNC\\", 0) == 0) {
        s.erase(0, 7);                // "\\?\UNC" removed, "\server\..." remains
        s.insert(s.begin(), '\\');    // -> "\\server\..."
    } else if (s.rfind("\\\\?\\", 0) == 0 || s.rfind("\\\\.\\", 0) == 0) {
        s.erase(0, 4);
    }
}

}  // namespace

bool is_absolute_path(std::string_view path) noexcept {
    if (path.size() >= 3 && has_drive(path) && (path[2] == '\\' || path[2] == '/')) {
        return true;
    }
    return path.size() >= 2 && (path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/');
}

std::string normalize_path(std::string_view path, std::string_view cwd) {
    std::string s = with_backslashes(path);
    strip_extended_prefix(s);

    if (!is_absolute_path(s) && !cwd.empty()) {
        std::string base = with_backslashes(cwd);
        strip_extended_prefix(base);
        if (is_absolute_path(base)) {
            // A rooted path ("\foo") keeps the cwd's drive; a drive-relative one ("c:foo") is left alone.
            if (!s.empty() && s[0] == '\\' && has_drive(base)) {
                s = base.substr(0, 2) + s;
            } else if (!has_drive(s)) {
                s = base + "\\" + s;
            }
        }
    }

    std::string prefix;
    std::size_t pos = 0;
    bool anchored = false;  // absolute or rooted: ".." cannot climb above it
    if (s.size() >= 2 && s[0] == '\\' && s[1] == '\\') {
        prefix = "\\\\";
        pos = 2;
        anchored = true;
    } else if (has_drive(s)) {
        prefix = std::string(1, lower_ascii(s[0])) + ":";
        pos = 2;
        if (pos < s.size() && s[pos] == '\\') {
            prefix += "\\";
            anchored = true;
        }
    } else if (!s.empty() && s[0] == '\\') {
        prefix = "\\";
        anchored = true;
    }

    std::vector<std::string> segments;
    while (pos <= s.size()) {
        const std::size_t end = s.find('\\', pos);
        const std::size_t stop = end == std::string::npos ? s.size() : end;
        std::string seg = s.substr(pos, stop - pos);
        pos = stop + 1;
        if (seg.empty() || seg == ".") {
            continue;
        }
        if (seg == "..") {
            if (!segments.empty() && segments.back() != "..") {
                segments.pop_back();
            } else if (!anchored) {
                segments.push_back(seg);
            }
            continue;
        }
        for (char& c : seg) {
            c = lower_ascii(c);
        }
        segments.push_back(std::move(seg));
    }

    std::string out = prefix;
    for (const std::string& seg : segments) {
        if (!out.empty() && out.back() != '\\' && out.back() != ':') {
            out += '\\';
        }
        out += seg;
    }
    return out;
}

}  // namespace cml::pdb
