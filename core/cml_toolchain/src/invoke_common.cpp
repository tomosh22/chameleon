// Portable parts of cml/toolchain/invoke.h: cancellation, the job pool and compiler-output parsing.
#include <algorithm>
#include <cctype>
#include <thread>

#include "cml/toolchain/invoke.h"

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

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

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
