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

}  // namespace

std::vector<IncludeDirective> parse_includes(std::string_view text) {
    std::vector<IncludeDirective> out;
    bool in_block_comment = false;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;

        // Comments are removed first (a block comment may span lines). String literals are not tracked, which only
        // matters for a "//" inside a string on a line that is a directive, and an #include operand has none.
        std::string clean;
        clean.reserve(line.size());
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (in_block_comment) {
                if (line[i] == '*' && i + 1 < line.size() && line[i + 1] == '/') {
                    in_block_comment = false;
                    ++i;
                    clean.push_back(' ');
                }
                continue;
            }
            if (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '*') {
                in_block_comment = true;
                ++i;
                continue;
            }
            if (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/') {
                break;
            }
            clean.push_back(line[i]);
        }

        std::size_t i = 0;
        while (i < clean.size() && is_space(clean[i])) ++i;
        if (i >= clean.size() || clean[i] != '#') continue;
        ++i;
        while (i < clean.size() && is_space(clean[i])) ++i;
        const std::size_t word_start = i;
        while (i < clean.size() && (std::isalnum(static_cast<unsigned char>(clean[i])) != 0 || clean[i] == '_')) ++i;
        const std::string_view word(clean.data() + word_start, i - word_start);
        if (word != "include" && word != "include_next" && word != "import") continue;
        while (i < clean.size() && is_space(clean[i])) ++i;
        if (i >= clean.size()) continue;
        const char open = clean[i];
        if (open != '"' && open != '<') continue;
        const char close = open == '"' ? '"' : '>';
        const std::size_t name_start = i + 1;
        const std::size_t name_end = clean.find(close, name_start);
        if (name_end == std::string::npos || name_end == name_start) continue;
        out.push_back({clean.substr(name_start, name_end - name_start), open == '<'});
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
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string t = args[i];
        if (t.size() < 2 || (t[0] != '-' && t[0] != '/')) {
            continue;
        }
        const std::string_view opt = std::string_view(t).substr(1);
        const auto starts = [&](std::string_view p) { return opt.substr(0, p.size()) == p; };
        // The value of an option that is either joined ("-Idir") or the next argument ("-I", "dir").
        const auto value = [&](std::string_view prefix) {
            if (opt.size() > prefix.size()) {
                return std::string(opt.substr(prefix.size()));
            }
            return i + 1 < args.size() ? args[++i] : std::string();
        };
        std::string v;
        if (opt == "isystem" || opt == "iquote" || opt == "idirafter" || starts("external:I") || starts("imsvc")) {
            const std::string_view prefix = starts("external:I") ? "external:I" : starts("imsvc") ? "imsvc" : opt;
            v = value(prefix);
            if (!v.empty()) roots.include_dirs.push_back(std::move(v));
        } else if (opt[0] == 'I') {
            v = value("I");
            if (!v.empty()) roots.include_dirs.push_back(std::move(v));
        } else if (opt == "include" || starts("FI")) {
            v = value(opt == "include" ? "include" : "FI");
            if (!v.empty()) roots.forced_includes.push_back(std::move(v));
        } else if (starts("Yc") || opt == "emit-pch" || opt == "building-pch-with-obj") {
            roots.creates_pch = true;
        } else if (starts("Yu") || opt == "include-pch") {
            roots.uses_pch = true;
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

std::set<std::string> IncludeScanner::dependencies(const ScanRoots& roots) {
    std::set<std::string> found;
    if (roots.source.empty()) {
        return found;
    }
    const std::string source = pdb::normalize_path(roots.source, roots.cwd);
    std::vector<std::string> dirs;
    for (const std::string& d : roots.include_dirs) {
        dirs.push_back(pdb::normalize_path(d, roots.cwd));
    }

    std::vector<std::string> work{source};  // files whose includes still have to be followed; the source first
    std::set<std::string> visited{source};
    const auto add = [&](const std::string& normalised) {
        if (!visited.insert(normalised).second) {
            return;
        }
        if (is_system_path(normalised)) {
            return;  // not tracked, not followed
        }
        found.insert(normalised);
        work.push_back(normalised);
    };
    // The first existing candidate of a name: the including file's directory (quoted form only), then -I in order.
    const auto resolve = [&](const std::string& name, const std::string* first_dir, const std::string* second_dir) -> std::string {
        if (first_dir != nullptr) {
            const std::string c = pdb::normalize_path(name, *first_dir);
            if (includes_of(c) != nullptr) return c;
        }
        if (second_dir != nullptr) {
            const std::string c = pdb::normalize_path(name, *second_dir);
            if (includes_of(c) != nullptr) return c;
        }
        for (const std::string& d : dirs) {
            const std::string c = pdb::normalize_path(name, d);
            if (includes_of(c) != nullptr) return c;
        }
        return {};
    };

    const std::string source_dir(path::parent_directory(std::string_view(source)));
    for (const std::string& f : roots.forced_includes) {
        const std::string hit = resolve(f, &source_dir, &roots.cwd);
        if (!hit.empty()) add(hit);
    }
    for (std::size_t next = 0; next < work.size(); ++next) {
        const std::string current = work[next];  // copy: add() grows `work`
        const std::vector<IncludeDirective>* includes = includes_of(current);
        if (includes == nullptr) {
            continue;
        }
        const std::string here(path::parent_directory(std::string_view(current)));
        for (const IncludeDirective& inc : *includes) {
            const std::string hit = resolve(inc.name, inc.angled ? nullptr : &here, nullptr);
            if (!hit.empty()) add(hit);
        }
    }
    return found;
}

}  // namespace cml::core
