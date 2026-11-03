#include "cml/core/include_scan.h"

#include <cctype>
#include <fstream>

#include "cml/core/source_baseline.h"
#include "cml/pdb/index.h"
#include "cml/toolchain/command_line.h"
#include "cml/util/path.h"

namespace cml::core {

namespace {

constexpr std::uintmax_t kMaxFileBytes = 8u * 1024u * 1024u;

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

bool is_ident(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// The identifier-like token that ends at out.back() (empty when out ends in something else).
std::string_view trailing_token(const std::string& out) {
    std::size_t start = out.size();
    while (start > 0 && is_ident(out[start - 1])) {
        --start;
    }
    return std::string_view(out).substr(start);
}

// Removes comments and the contents of raw string literals, keeping every newline (so line structure survives).
// String and character literals are copied verbatim and never start a comment: `"assets/*"` hides nothing.
std::string strip_comments(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n) {
        const char c = text[i];
        if (c == '/' && i + 1 < n && text[i + 1] == '/') {
            // Line comment; a backslash before the newline continues it.
            while (i < n && text[i] != '\n') {
                if (text[i] == '\\' && i + 1 < n && (text[i + 1] == '\n' || (text[i + 1] == '\r' && i + 2 < n && text[i + 2] == '\n'))) {
                    i += text[i + 1] == '\r' ? 3 : 2;
                    out.push_back('\n');
                    continue;
                }
                ++i;
            }
            continue;
        }
        if (c == '/' && i + 1 < n && text[i + 1] == '*') {
            i += 2;
            while (i < n && !(text[i] == '*' && i + 1 < n && text[i + 1] == '/')) {
                if (text[i] == '\n') out.push_back('\n');
                ++i;
            }
            i = i + 2 <= n ? i + 2 : n;
            out.push_back(' ');
            continue;
        }
        if (c == '"') {
            const std::string_view tok = trailing_token(out);
            const bool raw_prefix = tok == "R" || tok == "LR" || tok == "uR" || tok == "UR" || tok == "u8R";
            if (raw_prefix) {
                // R"delim( ... )delim"
                std::size_t d = i + 1;
                while (d < n && d - (i + 1) <= 16 && text[d] != '(' && text[d] != '"' && text[d] != '\n' && text[d] != ' ' && text[d] != '\\' && text[d] != ')') {
                    ++d;
                }
                if (d < n && text[d] == '(') {
                    const std::string closing = ")" + std::string(text.substr(i + 1, d - (i + 1))) + "\"";
                    const std::size_t end = text.find(closing, d + 1);
                    const std::size_t stop = end == std::string_view::npos ? n : end + closing.size();
                    out.push_back('"');
                    for (std::size_t k = d + 1; k < stop; ++k) {
                        if (text[k] == '\n') out.push_back('\n');
                    }
                    out.push_back('"');
                    i = stop;
                    continue;
                }
            }
            out.push_back(c);
            ++i;
            while (i < n && text[i] != '"' && text[i] != '\n') {
                if (text[i] == '\\' && i + 1 < n && text[i + 1] != '\n') {
                    out.push_back(text[i++]);
                }
                out.push_back(text[i++]);
            }
            if (i < n && text[i] == '"') {
                out.push_back(text[i++]);
            }
            continue;
        }
        if (c == '\'') {
            // 1'000 is a digit separator; L'a', u8'a', '\n' are character literals.
            const std::string_view tok = trailing_token(out);
            if (!tok.empty() && std::isdigit(static_cast<unsigned char>(tok.front())) != 0) {
                out.push_back(c);
                ++i;
                continue;
            }
            out.push_back(c);
            ++i;
            while (i < n && text[i] != '\'' && text[i] != '\n') {
                if (text[i] == '\\' && i + 1 < n && text[i + 1] != '\n') {
                    out.push_back(text[i++]);
                }
                out.push_back(text[i++]);
            }
            if (i < n && text[i] == '\'') {
                out.push_back(text[i++]);
            }
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

}  // namespace

std::vector<IncludeDirective> parse_includes(std::string_view text) {
    std::vector<IncludeDirective> out;
    const std::string clean = strip_comments(text);
    std::size_t pos = 0;
    while (pos < clean.size()) {
        std::size_t end = clean.find('\n', pos);
        if (end == std::string::npos) {
            end = clean.size();
        }
        const std::string_view line = std::string_view(clean).substr(pos, end - pos);
        pos = end + 1;

        std::size_t i = 0;
        while (i < line.size() && is_space(line[i])) ++i;
        if (i >= line.size() || line[i] != '#') continue;
        ++i;
        while (i < line.size() && is_space(line[i])) ++i;
        const std::size_t word_start = i;
        while (i < line.size() && is_ident(line[i])) ++i;
        const std::string_view word = line.substr(word_start, i - word_start);
        if (word != "include" && word != "include_next" && word != "import") continue;
        while (i < line.size() && is_space(line[i])) ++i;
        if (i >= line.size()) continue;
        const char open = line[i];
        if (open != '"' && open != '<') continue;
        const char close = open == '"' ? '"' : '>';
        const std::size_t name_start = i + 1;
        const std::size_t name_end = line.find(close, name_start);
        if (name_end == std::string_view::npos || name_end == name_start) continue;
        out.push_back({std::string(line.substr(name_start, name_end - name_start)), open == '<'});
    }
    return out;
}

ScanRoots scan_roots_from_record(const toolchain::CompileRecord& record) {
    ScanRoots roots;
    roots.source = record.source;
    roots.cwd = record.cwd;
    std::vector<std::string> args = record.args;
    Result<std::vector<std::string>> expanded = toolchain::expand_response_files(record.args, record.cwd, toolchain::default_file_reader());
    if (expanded) {
        args = std::move(*expanded);
    }
    // Options whose value is either joined ("-isystemDIR") or the next argument ("-isystem", "DIR"), longest prefix first.
    static const char* const kDirOptions[] = {"external:I", "idirafter", "isystem", "iquote", "imsvc"};
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string t = args[i];
        if (t.size() < 2 || (t[0] != '-' && t[0] != '/')) {
            continue;
        }
        const std::string_view opt = std::string_view(t).substr(1);
        const auto starts = [&](std::string_view p) { return opt.substr(0, p.size()) == p; };
        const auto value = [&](std::string_view prefix) {
            if (opt.size() > prefix.size()) {
                return std::string(opt.substr(prefix.size()));
            }
            return i + 1 < args.size() ? args[++i] : std::string();
        };
        bool done = false;
        for (const char* name : kDirOptions) {
            if (starts(name)) {
                std::string v = value(name);
                if (!v.empty()) roots.include_dirs.push_back(std::move(v));
                done = true;
                break;
            }
        }
        if (done) {
            continue;
        }
        if (opt[0] == 'I') {
            std::string v = value("I");
            if (!v.empty()) roots.include_dirs.push_back(std::move(v));
        } else if (opt == "include" || starts("FI")) {
            std::string v = value(opt == "include" ? "include" : "FI");
            if (!v.empty()) roots.forced_includes.push_back(std::move(v));
        } else if (starts("Yc") || opt == "emit-pch" || opt == "building-pch-with-obj") {
            roots.creates_pch = true;
            if (starts("Yc") && opt.size() > 2) roots.pch_header = std::string(opt.substr(2));
        } else if (starts("Yu") || opt == "include-pch") {
            roots.uses_pch = true;
            if (starts("Yu") && opt.size() > 2) roots.pch_header = std::string(opt.substr(2));
        }
    }
    return roots;
}

ScanFileReader disk_file_reader() {
    return [](const std::string& path) -> std::optional<std::string> {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            return std::nullopt;
        }
        const std::streamoff size = in.tellg();
        if (size < 0 || static_cast<std::uintmax_t>(size) > kMaxFileBytes) {
            return std::nullopt;
        }
        in.seekg(0);
        std::string text(static_cast<std::size_t>(size), '\0');
        if (size > 0 && !in.read(text.data(), size)) {
            return std::nullopt;
        }
        return text;
    };
}

IncludeScanner::IncludeScanner(ScanFileReader reader) : reader_(std::move(reader)) {}

const std::vector<IncludeDirective>* IncludeScanner::includes_of(const std::string& normalised_path) {
    auto it = parsed_.find(normalised_path);
    if (it == parsed_.end()) {
        std::optional<std::vector<IncludeDirective>> parsed;
        if (std::optional<std::string> text = reader_(normalised_path)) {
            parsed = parse_includes(*text);
        }
        it = parsed_.emplace(normalised_path, std::move(parsed)).first;
    }
    return it->second ? &*it->second : nullptr;
}

std::vector<std::string> IncludeScanner::normalised_dirs(const ScanRoots& roots) const {
    std::vector<std::string> dirs;
    for (const std::string& d : roots.include_dirs) {
        dirs.push_back(pdb::normalize_path(d, roots.cwd));
    }
    return dirs;
}

std::string IncludeScanner::resolve_forced(const ScanRoots& roots, const std::string& name) {
    if (name.empty()) {
        return {};
    }
    const std::string source = roots.source.empty() ? std::string() : pdb::normalize_path(roots.source, roots.cwd);
    const std::string source_dir(path::parent_directory(std::string_view(source)));
    std::vector<std::string> bases;
    if (!source_dir.empty()) bases.push_back(source_dir);
    if (!roots.cwd.empty()) bases.push_back(roots.cwd);
    for (const std::string& d : normalised_dirs(roots)) bases.push_back(d);
    for (const std::string& base : bases) {
        const std::string c = pdb::normalize_path(name, base);
        if (includes_of(c) != nullptr) return c;
    }
    return {};
}

std::set<std::string> IncludeScanner::walk(const std::vector<Start>& starts, const std::vector<std::string>& dirs) {
    // Every file keeps the union of the directories of the files that include it, directly or not: a quoted include is
    // searched in the including file's directory, then in those of the files further up the include stack, then in -I.
    std::map<std::string, std::set<std::string>> stack_dirs;
    std::vector<std::string> work;
    std::set<std::string> found;
    const auto reach = [&](const std::string& file, const std::set<std::string>& inherited) {
        if (is_system_path(file) || includes_of(file) == nullptr) {
            return;
        }
        std::set<std::string>& mine = stack_dirs[file];
        const std::size_t before = mine.size();
        mine.insert(inherited.begin(), inherited.end());
        mine.insert(std::string(path::parent_directory(std::string_view(file))));
        const bool is_new = found.insert(file).second;
        if (is_new || mine.size() != before) {
            work.push_back(file);  // new, or reached with directories it has not searched yet
        }
    };
    for (const Start& s : starts) {
        reach(s.file, s.inherited_dirs);
    }
    for (std::size_t next = 0; next < work.size(); ++next) {
        const std::string current = work[next];
        const std::vector<IncludeDirective>* includes = includes_of(current);
        if (includes == nullptr) {
            continue;
        }
        const std::set<std::string> searched = stack_dirs[current];  // copy: reach() mutates the map
        const std::string here(path::parent_directory(std::string_view(current)));
        for (const IncludeDirective& inc : *includes) {
            std::string hit;
            const auto try_dir = [&](const std::string& d) {
                if (!hit.empty()) return;
                const std::string c = pdb::normalize_path(inc.name, d);
                if (includes_of(c) != nullptr) hit = c;
            };
            if (!inc.angled) {
                try_dir(here);
                for (const std::string& d : searched) try_dir(d);
            }
            for (const std::string& d : dirs) try_dir(d);
            if (!hit.empty()) {
                reach(hit, searched);
            }
        }
    }
    return found;
}

std::set<std::string> IncludeScanner::dependencies(const ScanRoots& roots) {
    if (roots.source.empty()) {
        return {};
    }
    const std::string source = pdb::normalize_path(roots.source, roots.cwd);
    const std::string source_dir(path::parent_directory(std::string_view(source)));
    std::vector<Start> starts;
    starts.push_back({source, {}});
    for (const std::string& f : roots.forced_includes) {
        const std::string hit = resolve_forced(roots, f);
        if (!hit.empty()) starts.push_back({hit, {source_dir}});
    }
    std::set<std::string> found = walk(starts, normalised_dirs(roots));
    found.erase(source);
    return found;
}

std::set<std::string> IncludeScanner::pch_files(const ScanRoots& roots) {
    const std::string source = roots.source.empty() ? std::string() : pdb::normalize_path(roots.source, roots.cwd);
    const std::string source_dir(path::parent_directory(std::string_view(source)));
    std::vector<Start> starts;
    if (const std::string named = resolve_forced(roots, roots.pch_header); !named.empty()) {
        starts.push_back({named, {source_dir}});
    }
    for (const std::string& f : roots.forced_includes) {
        const std::string hit = resolve_forced(roots, f);
        if (!hit.empty()) starts.push_back({hit, {source_dir}});
    }
    std::set<std::string> found = walk(starts, normalised_dirs(roots));
    found.erase(source);
    return found;
}

}  // namespace cml::core
