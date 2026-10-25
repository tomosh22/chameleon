#include "cml/toolchain/command_line.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "cml/pdb/index.h"
#include "cml/toolchain/diag_ids.h"
#include "cml/util/log.h"
#include "cml/util/path.h"
#include "fs_util.h"

namespace cml::toolchain {

namespace {

bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

std::vector<std::string> split_command_line(std::string_view s, QuoteMode mode) {
    std::vector<std::string> out;
    std::string cur;
    bool in_token = false;
    bool in_quotes = false;
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const char c = s[i];
        if (c == '\\') {
            std::size_t j = i;
            while (j < n && s[j] == '\\') {
                ++j;
            }
            const std::size_t slashes = j - i;
            in_token = true;
            if (j < n && s[j] == '"') {
                cur.append(slashes / 2, '\\');
                if (slashes % 2 == 1) {
                    cur.push_back('"');  // escaped quote
                    i = j + 1;
                } else {
                    i = j;  // the quote itself is handled by the next iteration (toggle)
                }
            } else {
                cur.append(slashes, '\\');
                i = j;
            }
            continue;
        }
        if (c == '"') {
            in_token = true;
            if (in_quotes && i + 1 < n && s[i + 1] == '"') {
                cur.push_back('"');  // "" inside quotes: one literal quote ...
                i += 2;
                in_quotes = mode == QuoteMode::Crt;  // ... and the CRT stays quoted, CommandLineToArgvW does not
                continue;
            }
            in_quotes = !in_quotes;
            ++i;
            continue;
        }
        if (!in_quotes && is_space(c)) {
            if (in_token) {
                out.push_back(std::move(cur));
                cur.clear();
                in_token = false;
            }
            ++i;
            continue;
        }
        cur.push_back(c);
        in_token = true;
        ++i;
    }
    if (in_token) {
        out.push_back(std::move(cur));
    }
    return out;
}

std::vector<std::string> split_command_line_posix(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    bool in_token = false;
    bool in_quotes = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            const char n = s[i + 1];
            if (n == '"' || n == '\\' || n == '$' || n == '`') {
                cur.push_back(n);
                in_token = true;
                ++i;
                continue;
            }
        }
        if (c == '"') {
            in_quotes = !in_quotes;
            in_token = true;
            continue;
        }
        if (!in_quotes && is_space(c)) {
            if (in_token) {
                out.push_back(std::move(cur));
                cur.clear();
                in_token = false;
            }
            continue;
        }
        cur.push_back(c);
        in_token = true;
    }
    if (in_token) {
        out.push_back(std::move(cur));
    }
    return out;
}

std::string quote_argument(std::string_view arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos) {
        return std::string(arg);
    }
    std::string out;
    out.push_back('"');
    std::size_t i = 0;
    while (i < arg.size()) {
        std::size_t slashes = 0;
        while (i < arg.size() && arg[i] == '\\') {
            ++slashes;
            ++i;
        }
        if (i == arg.size()) {
            out.append(slashes * 2, '\\');  // backslashes before the closing quote
        } else if (arg[i] == '"') {
            out.append(slashes * 2 + 1, '\\');
            out.push_back('"');
            ++i;
        } else {
            out.append(slashes, '\\');
            out.push_back(arg[i]);
            ++i;
        }
    }
    out.push_back('"');
    return out;
}

std::string join_command_line(std::span<const std::string> arguments) {
    std::string out;
    for (const std::string& a : arguments) {
        if (!out.empty()) {
            out.push_back(' ');
        }
        out += quote_argument(a);
    }
    return out;
}

std::string decode_response_file_bytes(std::string_view b) {
    const auto u8 = [&](std::size_t i) { return static_cast<unsigned char>(b[i]); };
    if (b.size() >= 2 && u8(0) == 0xFF && u8(1) == 0xFE) {
        std::string out;
        std::size_t i = 2;
        while (i + 1 < b.size()) {
            std::uint32_t unit = u8(i) | (static_cast<std::uint32_t>(u8(i + 1)) << 8);
            i += 2;
            if (unit >= 0xD800 && unit < 0xDC00 && i + 1 < b.size()) {
                const std::uint32_t low = u8(i) | (static_cast<std::uint32_t>(u8(i + 1)) << 8);
                if (low >= 0xDC00 && low < 0xE000) {
                    unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                }
            }
            append_utf8(out, unit);
        }
        return out;
    }
    if (b.size() >= 3 && u8(0) == 0xEF && u8(1) == 0xBB && u8(2) == 0xBF) {
        return std::string(b.substr(3));
    }
    return std::string(b);
}

FileReader default_file_reader() {
    return [](const std::string& path) -> Result<std::string> {
        std::ifstream in(detail::to_path(path), std::ios::binary);
        if (!in) {
            return Diag::make(diag::kResponseFileUnreadable, "cannot read response file '" + path + "'",
                              "Check that the response file recorded in the build still exists.");
        }
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return decode_response_file_bytes(bytes);
    };
}

namespace {

struct ExpandState {
    int files = 0;
    std::size_t tokens = 0;
    std::vector<std::string> stack;  // normalised paths of the response files being expanded, outermost first
};

Result<void> expand_into(const std::vector<std::string>& in, std::string_view cwd, const FileReader& reader, ExpandState& st,
                         std::vector<std::string>& out) {
    for (const std::string& arg : in) {
        if (arg.size() < 2 || arg[0] != '@') {
            out.push_back(arg);
            if (++st.tokens > kMaxResponseTokens) {
                return Diag::make(diag::kResponseFileTooDeep,
                                  "the command line expands to more than " + std::to_string(kMaxResponseTokens) + " tokens",
                                  "Remove the runaway @file reference from the recorded command line.");
            }
            continue;
        }
        if (static_cast<int>(st.stack.size()) >= kMaxResponseFileDepth) {
            return Diag::make(diag::kResponseFileTooDeep,
                              "response files nest deeper than " + std::to_string(kMaxResponseFileDepth) + " levels at '" + arg + "'",
                              "Remove the deeply nested @file reference from the recorded command line.");
        }
        if (st.files >= kMaxResponseFiles) {
            return Diag::make(diag::kResponseFileTooDeep,
                              "more than " + std::to_string(kMaxResponseFiles) + " response files are read at '" + arg + "'",
                              "Remove the runaway @file references from the recorded command line.");
        }
        std::string path = arg.substr(1);
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
            path = path.substr(1, path.size() - 2);
        }
        if (!detail::is_absolute(path) && !cwd.empty()) {
            std::string joined(cwd);
            if (!joined.empty() && !path::is_separator(joined.back())) {
                joined.push_back('\\');
            }
            path = joined + path;
        }
        const std::string canonical = pdb::normalize_path(path);
        for (const std::string& open : st.stack) {
            if (open == canonical) {
                return Diag::make(diag::kResponseFileTooDeep, "response file '" + path + "' includes itself (cycle)",
                                  "Remove the recursive @file reference from the recorded command line.");
            }
        }
        ++st.files;
        Result<std::string> text = reader(path);
        if (!text) {
            return text.error();
        }
        CML_LOG(Trace, "toolchain: expanding response file %s", path.c_str());
        st.stack.push_back(canonical);
        Result<void> nested = expand_into(split_command_line(*text), cwd, reader, st, out);
        st.stack.pop_back();
        if (!nested) {
            return nested;
        }
    }
    return {};
}

}  // namespace

Result<std::vector<std::string>> expand_response_files(const std::vector<std::string>& arguments,
                                                       std::string_view cwd, const FileReader& reader) {
    std::vector<std::string> out;
    out.reserve(arguments.size());
    ExpandState st;
    Result<void> r = expand_into(arguments, cwd, reader ? reader : default_file_reader(), st, out);
    if (!r) {
        return r.error();
    }
    return out;
}

}  // namespace cml::toolchain
