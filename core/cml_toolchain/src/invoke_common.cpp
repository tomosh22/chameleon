// Portable parts of cml/toolchain/invoke.h: cancellation, the job pool and compiler-output parsing.
#include <algorithm>
#include <cctype>
#include <thread>

#include "cml/toolchain/invoke.h"
#include "fs_util.h"

namespace cml::toolchain {

// ---- CancelToken ----------------------------------------------------------------------------

void CancelToken::cancel() {
    std::vector<Killer> to_run;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled_.exchange(true, std::memory_order_acq_rel)) {
            return;  // already cancelled: the killers have run
        }
        for (auto& kv : killers_) {
            to_run.push_back(kv.second);
        }
    }
    for (Killer& k : to_run) {
        k();
    }
}

CancelToken::Handle CancelToken::add_killer(Killer killer) {
    Handle h = 0;
    bool run_now = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        h = next_++;
        if (cancelled_.load(std::memory_order_acquire)) {
            run_now = true;
        } else {
            killers_.emplace(h, killer);
        }
    }
    if (run_now) {
        killer();
    }
    return h;
}

void CancelToken::remove_killer(Handle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    killers_.erase(handle);
}

// ---- job pool -------------------------------------------------------------------------------

unsigned pool_size(std::size_t jobs, unsigned max_workers) noexcept {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) {
        hw = 1;
    }
    if (max_workers != 0 && max_workers < hw) {
        hw = max_workers;
    }
    if (jobs == 0) {
        return 0;
    }
    return static_cast<unsigned>(std::min<std::size_t>(jobs, hw));
}

void run_parallel(std::size_t count, unsigned max_workers, const std::function<void(std::size_t)>& work,
                  const CancelToken* cancel) {
    const unsigned workers = pool_size(count, max_workers);
    if (workers == 0) {
        return;
    }
    std::atomic<std::size_t> next{0};
    const auto loop = [&]() {
        while (true) {
            if (cancel != nullptr && cancel->cancelled()) {
                return;
            }
            const std::size_t i = next.fetch_add(1);
            if (i >= count) {
                return;
            }
            work(i);
        }
    };
    std::vector<std::thread> threads;
    threads.reserve(workers - 1);
    for (unsigned t = 1; t < workers; ++t) {
        threads.emplace_back(loop);
    }
    loop();
    for (std::thread& t : threads) {
        t.join();
    }
}

// ---- output parsing -------------------------------------------------------------------------

const char* to_string(Severity severity) noexcept {
    switch (severity) {
        case Severity::Note: return "note";
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
        case Severity::FatalError: return "fatal error";
    }
    return "error";
}

std::string ToolDiagnostic::format() const {
    std::string out;
    if (!file.empty()) {
        out = file;
        if (line > 0) {
            out += "(" + std::to_string(line);
            if (column > 0) {
                out += "," + std::to_string(column);
            }
            out += ")";
        }
    } else {
        out = tool.empty() ? "cl" : tool;
    }
    out += ": ";
    out += to_string(severity);
    if (!code.empty()) {
        out += " " + code;
    }
    out += ": ";
    out += message;
    return out;
}

namespace {

using detail::trim;

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

// "fatal error" | "error" | "warning" | "note" at the start of `s`, followed by a space, ':' or end.
bool severity_at(std::string_view s, Severity& sev, std::size_t& len) {
    struct Entry {
        const char* word;
        Severity sev;
    };
    static constexpr Entry kWords[] = {{"fatal error", Severity::FatalError},
                                       {"error", Severity::Error},
                                       {"warning", Severity::Warning},
                                       {"note", Severity::Note}};
    for (const Entry& e : kWords) {
        const std::string_view w(e.word);
        if (s.substr(0, w.size()) == w && (s.size() == w.size() || s[w.size()] == ' ' || s[w.size()] == ':')) {
            sev = e.sev;
            len = w.size();
            return true;
        }
    }
    return false;
}

// "C2065", "LNK1104", "D8016", "MSB3073": 1-4 capital letters followed by 3-5 digits.
bool looks_like_code(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && s[i] >= 'A' && s[i] <= 'Z') {
        ++i;
    }
    if (i == 0 || i > 4) {
        return false;
    }
    const std::size_t digits = s.size() - i;
    if (digits < 3 || digits > 5) {
        return false;
    }
    for (std::size_t k = i; k < s.size(); ++k) {
        if (!is_digit(s[k])) {
            return false;
        }
    }
    return true;
}

bool parse_int(std::string_view s, int& out) {
    if (s.empty() || s.size() > 9) {
        return false;
    }
    int v = 0;
    for (const char c : s) {
        if (!is_digit(c)) {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

// Splits "(12,5)" / "(12)" / "(12-14)" off the end of `prefix`.
bool split_paren_location(std::string_view prefix, std::string_view& file, int& line, int& col) {
    if (prefix.empty() || prefix.back() != ')') {
        return false;
    }
    const std::size_t open = prefix.rfind('(');
    if (open == std::string_view::npos) {
        return false;
    }
    std::string_view inner = prefix.substr(open + 1, prefix.size() - open - 2);
    const std::size_t comma = inner.find(',');
    std::string_view line_part = comma == std::string_view::npos ? inner : inner.substr(0, comma);
    std::string_view col_part = comma == std::string_view::npos ? std::string_view() : inner.substr(comma + 1);
    const std::size_t dash = line_part.find('-');
    if (dash != std::string_view::npos) {
        line_part = line_part.substr(0, dash);
    }
    const std::size_t cdash = col_part.find('-');
    if (cdash != std::string_view::npos) {
        col_part = col_part.substr(0, cdash);
    }
    int l = 0;
    int c = 0;
    if (!parse_int(line_part, l)) {
        return false;
    }
    if (!col_part.empty() && !parse_int(col_part, c)) {
        return false;
    }
    file = trim(prefix.substr(0, open));
    line = l;
    col = c;
    return !file.empty();
}

// Splits ":12:5" / ":12" off the end of `prefix` (clang's GCC-style location).
bool split_colon_location(std::string_view prefix, std::string_view& file, int& line, int& col) {
    std::string_view rest = prefix;
    int nums[2] = {0, 0};
    int found = 0;
    while (found < 2) {
        const std::size_t colon = rest.rfind(':');
        if (colon == std::string_view::npos || colon + 1 >= rest.size()) {
            break;
        }
        int v = 0;
        if (!parse_int(rest.substr(colon + 1), v)) {
            break;
        }
        nums[found++] = v;
        rest = rest.substr(0, colon);
    }
    if (found == 0 || rest.empty()) {
        return false;
    }
    file = rest;
    if (found == 2) {
        line = nums[1];
        col = nums[0];
    } else {
        line = nums[0];
        col = 0;
    }
    return true;
}

bool parse_line(std::string_view raw, ToolDiagnostic& out) {
    const std::string_view s = trim(raw);
    if (s.size() < 8) {
        return false;
    }
    for (std::size_t i = 0; i + 1 < s.size(); ++i) {
        if (s[i] != ':') {
            continue;
        }
        std::size_t j = i + 1;
        while (j < s.size() && s[j] == ' ') {
            ++j;
        }
        if (s.substr(j, 13) == "Command line ") {
            j += 13;
        }
        Severity sev = Severity::Error;
        std::size_t len = 0;
        if (!severity_at(s.substr(j), sev, len)) {
            continue;
        }
        const std::string_view prefix = trim(s.substr(0, i));
        if (prefix.empty()) {
            continue;
        }
        std::string_view file;
        int line = 0;
        int col = 0;
        ToolDiagnostic d;
        d.severity = sev;
        if (split_paren_location(prefix, file, line, col) || split_colon_location(prefix, file, line, col)) {
            d.file = std::string(file);
            d.line = line;
            d.column = col;
        } else if (prefix.find_first_of("\\/.") == std::string_view::npos) {
            d.tool = std::string(prefix);  // "cl", "LINK", "clang-cl"
        } else {
            d.file = std::string(prefix);
        }
        std::string_view rest = s.substr(j + len);
        rest = trim(rest);
        // optional code
        const std::size_t end_code = rest.find_first_of(" :");
        if (end_code != std::string_view::npos && looks_like_code(rest.substr(0, end_code))) {
            d.code = std::string(rest.substr(0, end_code));
            rest = rest.substr(end_code);
        }
        rest = trim(rest);
        if (!rest.empty() && rest.front() == ':') {
            rest.remove_prefix(1);
        }
        d.message = std::string(trim(rest));
        out = std::move(d);
        return true;
    }
    return false;
}

template <class Fn>
void for_each_line(std::string_view text, Fn&& fn) {
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) {
            nl = text.size();
        }
        std::string_view line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!(nl == text.size() && line.empty())) {
            fn(line, nl < text.size());
        }
        pos = nl + 1;
    }
}

}  // namespace

namespace {

bool is_sep(char c) noexcept {
    return c == '\\' || c == '/';
}

bool has_drive(std::string_view s) noexcept {
    return s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) != 0 && s[1] == ':';
}

void split_components(std::string_view s, std::vector<std::string>& out) {
    std::size_t i = 0;
    while (i <= s.size()) {
        std::size_t j = i;
        while (j < s.size() && !is_sep(s[j])) {
            ++j;
        }
        if (j > i) {
            out.emplace_back(s.substr(i, j - i));
        }
        i = j + 1;
    }
}

struct AbsPath {
    std::string root;                  // "C:", "\\\\server\\share" or "/" (portable tests)
    std::vector<std::string> parts;
};

// An absolute path in one of the three roots; false for anything else.
bool parse_absolute(std::string_view s, AbsPath& out) {
    out = AbsPath();
    if (s.size() >= 2 && is_sep(s[0]) && is_sep(s[1])) {
        std::vector<std::string> comps;
        split_components(s, comps);
        if (comps.empty()) {
            return false;
        }
        out.root = "\\\\" + comps[0] + (comps.size() > 1 ? "\\" + comps[1] : std::string());
        out.parts.assign(comps.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(2, comps.size())), comps.end());
        return true;
    }
    if (s.size() >= 3 && has_drive(s) && is_sep(s[2])) {
        out.root = std::string(1, s[0]) + ":";
        split_components(s.substr(2), out.parts);
        return true;
    }
    if (!s.empty() && is_sep(s[0])) {
        out.root = "/";
        split_components(s, out.parts);
        return true;
    }
    return false;
}

}  // namespace

std::string resolve_tool_path(std::string_view file, std::string_view cwd) {
    if (file.empty()) {
        return {};
    }
    AbsPath base;
    std::vector<std::string> add;
    const bool unc = file.size() >= 2 && is_sep(file[0]) && is_sep(file[1]);
    const bool drive_abs = file.size() >= 3 && has_drive(file) && is_sep(file[2]);
    AbsPath cwd_abs;
    const bool have_cwd = parse_absolute(cwd, cwd_abs);
    if (unc || drive_abs) {
        parse_absolute(file, base);
    } else if (has_drive(file)) {  // "C:logic.cpp": relative to the current directory of drive C
        if (!have_cwd || cwd_abs.root.size() != 2 || cwd_abs.root[1] != ':' ||
            std::tolower(static_cast<unsigned char>(cwd_abs.root[0])) != std::tolower(static_cast<unsigned char>(file[0]))) {
            return {};
        }
        base = cwd_abs;
        split_components(file.substr(2), add);
    } else if (is_sep(file[0])) {  // "\src\a.cpp": the root of cwd's drive or share
        if (have_cwd && cwd_abs.root != "/") {
            base.root = cwd_abs.root;
            split_components(file, add);
        } else {
            parse_absolute(file, base);  // no drive to take: a portable "/p" path
        }
    } else {
        if (!have_cwd) {
            return {};
        }
        base = cwd_abs;
        split_components(file, add);
    }
    std::vector<std::string> parts;
    for (const std::vector<std::string>* list : {&base.parts, &add}) {
        for (const std::string& c : *list) {
            if (c == ".") {
                continue;
            }
            if (c == "..") {
                if (!parts.empty()) {
                    parts.pop_back();
                }
                continue;
            }
            parts.push_back(c);
        }
    }
    const char separator = base.root == "/" ? '/' : '\\';
    std::string out = base.root == "/" ? std::string() : base.root;
    for (const std::string& c : parts) {
        out.push_back(separator);
        out += c;
    }
    if (parts.empty()) {
        out.push_back(separator);
    }
    return out;
}

std::vector<ToolDiagnostic> parse_tool_output(std::string_view output) {
    std::vector<ToolDiagnostic> out;
    for_each_line(output, [&](std::string_view line, bool) {
        ToolDiagnostic d;
        if (parse_line(line, d)) {
            out.push_back(std::move(d));
        }
    });
    return out;
}

std::vector<std::string> parse_show_includes(std::string_view output) {
    constexpr std::string_view kPrefix = "Note: including file:";
    std::vector<std::string> out;
    for_each_line(output, [&](std::string_view line, bool) {
        if (line.substr(0, kPrefix.size()) == kPrefix) {
            out.emplace_back(trim(line.substr(kPrefix.size())));
        }
    });
    return out;
}

std::string reformat_tool_output(std::string_view output) {
    std::string out;
    for_each_line(output, [&](std::string_view line, bool had_newline) {
        ToolDiagnostic d;
        if (parse_line(line, d)) {
            out += d.format();
        } else {
            out += line;
        }
        if (had_newline) {
            out.push_back('\n');
        }
    });
    return out;
}

}  // namespace cml::toolchain
