#include "cml/ui/compile_output.h"

#include <cctype>

namespace cml::ui {

namespace {

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

bool is_alnum(char c) noexcept { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

bool parse_int(std::string_view text, int* out) noexcept {
    if (text.empty() || text.size() > 9) {
        return false;
    }
    int value = 0;
    for (const char c : text) {
        if (!is_digit(c)) {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    *out = value;
    return true;
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

struct Keyword {
    const char* word;
    OutputSeverity severity;
};

// Longer words first ("fatal error" before "error").
constexpr Keyword kKeywords[] = {
    {"fatal error", OutputSeverity::Error}, {"error", OutputSeverity::Error}, {"warning", OutputSeverity::Warning},
    {"note", OutputSeverity::Info},         {"remark", OutputSeverity::Info}, {"info", OutputSeverity::Info},
};

bool starts_with_word(std::string_view s, std::string_view word) noexcept {
    if (s.size() < word.size()) {
        return false;
    }
    for (std::size_t i = 0; i < word.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) != word[i]) {
            return false;
        }
    }
    // The keyword must end there: "errors" or "errorcode" are not diagnostics.
    return s.size() == word.size() || !is_alnum(s[word.size()]);
}

// `rest` starts right after the separator that follows the location ("error C2065: text" or "error: text").
// Fills severity, code and text.
bool parse_severity_and_text(std::string_view rest, CompilerMessage* out) {
    rest = trim(rest);
    for (const Keyword& k : kKeywords) {
        const std::string_view word = k.word;
        if (!starts_with_word(rest, word)) {
            continue;
        }
        out->severity = k.severity;
        rest.remove_prefix(word.size());
        rest = trim(rest);
        // Optional code (a letter, then letters/digits with at least one digit) before ':'.
        std::size_t n = 0;
        while (n < rest.size() && is_alnum(rest[n])) {
            ++n;
        }
        bool has_digit = false;
        for (std::size_t i = 0; i < n; ++i) {
            has_digit = has_digit || is_digit(rest[i]);
        }
        if (n > 0 && has_digit && std::isalpha(static_cast<unsigned char>(rest[0])) != 0) {
            // "C2065:" (cl, clang-cl) or "D8003 :" (cl driver); both end the code at a colon.
            std::size_t after_code = n;
            while (after_code < rest.size() && rest[after_code] == ' ') {
                ++after_code;
            }
            if (after_code < rest.size() && rest[after_code] == ':') {
                out->code.assign(rest.substr(0, n));
                rest.remove_prefix(after_code);
            }
        }
        if (!rest.empty() && rest.front() == ':') {
            rest.remove_prefix(1);
        } else if (out->code.empty()) {
            // "error text" without a colon is plain prose, not a diagnostic line.
            return false;
        }
        out->text.assign(trim(rest));
        return true;
    }
    return false;
}

// cl / clang-cl: path(line[,col]): severity ...
std::optional<CompilerMessage> parse_paren_form(std::string_view line) {
    for (std::size_t close = 0; close < line.size(); ++close) {
        if (line[close] != ')') {
            continue;
        }
        // Walk back over digits and commas to the '('.
        std::size_t open = close;
        while (open > 0 && (is_digit(line[open - 1]) || line[open - 1] == ',')) {
            --open;
        }
        if (open == 0 || line[open - 1] != '(' || open == close) {
            continue;
        }
        const std::string_view numbers = line.substr(open, close - open);
        const std::size_t comma = numbers.find(',');
        int ln = 0;
        int col = 0;
        if (!parse_int(numbers.substr(0, comma), &ln)) {
            continue;
        }
        if (comma != std::string_view::npos) {
            const std::string_view tail = numbers.substr(comma + 1);
            const std::size_t second = tail.find(',');
            if (!parse_int(tail.substr(0, second), &col)) {
                continue;
            }
        }
        std::string_view after = trim(line.substr(close + 1));
        if (after.empty() || after.front() != ':') {
            continue;
        }
        after.remove_prefix(1);
        CompilerMessage message;
        if (!parse_severity_and_text(after, &message)) {
            continue;
        }
        const std::string_view file = trim(line.substr(0, open - 1));
        if (file.empty()) {
            continue;
        }
        message.location.file.assign(file);
        message.location.line = ln;
        message.location.column = col;
        return message;
    }
    return std::nullopt;
}

// clang / gcc: path:line[:col]: severity: ...   (a drive letter's colon is skipped)
std::optional<CompilerMessage> parse_colon_form(std::string_view line) {
    std::size_t start = 0;
    if (line.size() >= 3 && std::isalpha(static_cast<unsigned char>(line[0])) != 0 && line[1] == ':' &&
        (line[2] == '\\' || line[2] == '/')) {
        start = 2;
    }
    for (std::size_t colon = line.find(':', start); colon != std::string_view::npos; colon = line.find(':', colon + 1)) {
        const std::size_t p = colon + 1;
        std::size_t digits = p;
        while (digits < line.size() && is_digit(line[digits])) {
            ++digits;
        }
        if (digits == p) {
            continue;
        }
        int ln = 0;
        int col = 0;
        if (!parse_int(line.substr(p, digits - p), &ln)) {
            continue;
        }
        std::size_t next = digits;
        if (next < line.size() && line[next] == ':') {
            std::size_t cdigits = next + 1;
            while (cdigits < line.size() && is_digit(line[cdigits])) {
                ++cdigits;
            }
            if (cdigits > next + 1 && parse_int(line.substr(next + 1, cdigits - next - 1), &col)) {
                next = cdigits;
            }
        }
        if (next >= line.size() || line[next] != ':') {
            continue;
        }
        CompilerMessage message;
        if (!parse_severity_and_text(line.substr(next + 1), &message)) {
            continue;
        }
        const std::string_view file = trim(line.substr(0, colon));
        if (file.empty()) {
            continue;
        }
        message.location.file.assign(file);
        message.location.line = ln;
        message.location.column = col;
        return message;
    }
    return std::nullopt;
}

// "a.obj : error LNK2019: ..." / "LINK : fatal error LNK1104: ..." / "cl : Command line error D8003 : ..."
std::optional<CompilerMessage> parse_tool_form(std::string_view line) {
    const std::size_t sep = line.find(" : ");
    if (sep == std::string_view::npos) {
        return std::nullopt;
    }
    CompilerMessage message;
    if (!parse_severity_and_text(line.substr(sep + 3), &message) || message.code.empty()) {
        return std::nullopt;
    }
    // No line number: the "location" stays unclickable but names the object or tool.
    message.location.file.assign(trim(line.substr(0, sep)));
    return message;
}

}  // namespace

std::optional<CompilerMessage> parse_compiler_line(std::string_view line) {
    line = trim(line);
    if (line.empty()) {
        return std::nullopt;
    }
    if (auto m = parse_paren_form(line)) {
        return m;
    }
    if (auto m = parse_colon_form(line)) {
        return m;
    }
    return parse_tool_form(line);
}

std::vector<std::string> split_output_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin < text.size()) {
        std::size_t end = text.find('\n', begin);
        const bool last = end == std::string_view::npos;
        if (last) {
            end = text.size();
        }
        std::size_t stop = end;
        if (stop > begin && text[stop - 1] == '\r') {
            --stop;
        }
        lines.emplace_back(text.substr(begin, stop - begin));
        begin = end + 1;
        if (last) {
            break;
        }
    }
    return lines;
}

const char* to_string(OutputSeverity severity) noexcept {
    switch (severity) {
        case OutputSeverity::Info: return "info";
        case OutputSeverity::Warning: return "warning";
        case OutputSeverity::Error: return "error";
    }
    return "info";
}

}  // namespace cml::ui
