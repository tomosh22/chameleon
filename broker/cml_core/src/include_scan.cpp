#include "cml/core/include_scan.h"

#include <bit>
#include <cctype>
#include <chrono>
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

std::uint32_t IncludeScanner::dir_id(const std::string& normalised_dir) {
    const auto it = dir_ids_.find(normalised_dir);
    if (it != dir_ids_.end()) {
        return it->second;
    }
    const auto id = static_cast<std::uint32_t>(dirs_.size());
    dirs_.push_back(normalised_dir);
    resolved_.emplace_back();
    dir_ids_.emplace(normalised_dir, id);
    return id;
}

std::uint32_t IncludeScanner::file_id(const std::string& normalised_path) {
    const auto it = file_ids_.find(normalised_path);
    if (it != file_ids_.end()) {
        return it->second;
    }
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    File file;
    file.path = normalised_path;
    file.system = is_system_path(normalised_path);
    const Clock::time_point t0 = Clock::now();
    std::optional<std::string> text = reader_(normalised_path);
    const Clock::time_point t1 = Clock::now();
    stats_.read_ms += ms(t0, t1);
    if (text) {
        file.includes = parse_includes(*text);
        file.readable = true;
        stats_.parse_ms += ms(t1, Clock::now());
        ++stats_.found;
    } else {
        ++stats_.missing;
    }
    const auto id = static_cast<std::uint32_t>(files_.size());
    file.dir = dir_id(std::string(path::parent_directory(std::string_view(normalised_path))));
    files_.push_back(std::move(file));
    file_ids_.emplace(normalised_path, id);
    return id;
}

std::int64_t IncludeScanner::resolve(std::uint32_t dir, const std::string& name) {
    ++stats_.lookups;
    {
        const auto it = resolved_[dir].find(name);
        if (it != resolved_[dir].end()) {
            return it->second;
        }
    }
    ++stats_.resolved;
    const std::uint32_t id = file_id(pdb::normalize_path(name, dirs_[dir]));  // may intern a directory: resolved_ may grow
    const std::int64_t answer = files_[id].readable ? static_cast<std::int64_t>(id) : -1;
    resolved_[dir].emplace(name, answer);
    return answer;
}

const std::vector<IncludeDirective>* IncludeScanner::includes_of(const std::string& normalised_path) {
    const File& file = files_[file_id(normalised_path)];
    return file.readable ? &file.includes : nullptr;
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
    // searched in the including file's directory, then in those of the files further up the include stack (in the order of
    // their paths: the first of them, by path, that has the file answers), then in -I.
    //
    // WP-50 (R-55): directories are ids and a file's set of them is a bit set, so taking the union is a few word operations.
    // A file is expanded again whenever it is reached with directories it had not searched yet; only those can change what its
    // includes resolve to, so the second and later expansions look at them alone: an include answered by the file's own
    // directory keeps that answer (searched first); one answered by a stack directory changes only to a new stack directory
    // whose path sorts before it; one answered by -I, or not at all, changes to the new stack directory with the smallest path
    // that has it. That is the answer a full search would give. An unchanged answer is passed only the new directories (the
    // file it names has all the others already).
    using Bits = std::vector<std::uint64_t>;
    const auto set_bit = [](Bits& b, std::uint32_t i) {
        const std::size_t w = i / 64;
        if (b.size() <= w) {
            b.resize(w + 1, 0);
        }
        const std::uint64_t m = std::uint64_t{1} << (i % 64);
        const bool fresh = (b[w] & m) == 0;
        b[w] |= m;
        return fresh;
    };
    const auto merge = [](Bits& into, const Bits& from) {  // into |= from; true when into grew
        if (into.size() < from.size()) {
            into.resize(from.size(), 0);
        }
        bool grew = false;
        for (std::size_t w = 0; w < from.size(); ++w) {
            const std::uint64_t add = from[w] & ~into[w];
            if (add != 0) {
                into[w] |= add;
                grew = true;
            }
        }
        return grew;
    };
    const auto for_each_bit = [](const Bits& b, const auto& fn) {
        for (std::size_t w = 0; w < b.size(); ++w) {
            std::uint64_t word = b[w];
            while (word != 0) {
                const unsigned bit = static_cast<unsigned>(std::countr_zero(word));
                word &= word - 1;
                fn(static_cast<std::uint32_t>(w * 64 + bit));
            }
        }
    };
    const auto before_path = [this](std::uint32_t a, std::uint32_t b) { return dirs_[a] < dirs_[b]; };

    std::unordered_map<std::uint32_t, Bits> stack_dirs;
    std::vector<std::uint32_t> work;
    std::vector<char> found_flag;
    std::vector<std::uint32_t> found;
    std::vector<std::uint32_t> search;  // -I, as ids
    search.reserve(dirs.size());
    for (const std::string& d : dirs) {
        search.push_back(dir_id(d));
    }
    const auto reach = [&](std::uint32_t file, const Bits& inherited) {
        if (files_[file].system || !files_[file].readable) {
            return;
        }
        Bits& mine = stack_dirs[file];
        bool grew = merge(mine, inherited);
        grew = set_bit(mine, files_[file].dir) || grew;
        if (found_flag.size() <= file) {
            found_flag.resize(files_.size() + 1, 0);
        }
        const bool is_new = found_flag[file] == 0;
        if (is_new) {
            found_flag[file] = 1;
            found.push_back(file);
        }
        if (is_new || grew) {
            work.push_back(file);  // new, or reached with directories it has not searched yet
        }
    };
    for (const Start& s : starts) {
        Bits inherited;
        for (const std::string& d : s.inherited_dirs) {
            set_bit(inherited, dir_id(d));
        }
        reach(file_id(s.file), inherited);
    }

    enum class From : std::uint8_t { None, Here, Stack, Search };
    struct Answer {
        std::int64_t hit = -1;
        From from = From::None;
        std::uint32_t dir = 0;  // From::Stack: the directory that answered
    };
    struct Expanded {
        Bits seen;
        std::vector<Answer> answers;
    };
    std::unordered_map<std::uint32_t, Expanded> expanded;
    // The stack directory with the smallest path among `candidates` that has `name` (and, with a bound, sorts before it).
    const auto best_stack_hit = [&](const Bits& candidates, const std::string& name, const Answer* bound, Answer* out) {
        bool any = false;
        for_each_bit(candidates, [&](std::uint32_t d) {
            if (bound != nullptr && !before_path(d, bound->dir)) {
                return;
            }
            if (any && !before_path(d, out->dir)) {
                return;
            }
            const std::int64_t hit = resolve(d, name);
            if (hit >= 0) {
                out->hit = hit;
                out->from = From::Stack;
                out->dir = d;
                any = true;
            }
        });
        return any;
    };
    for (std::size_t next = 0; next < work.size(); ++next) {
        const std::uint32_t current = work[next];
        const Bits searched = stack_dirs[current];  // copy: reach() mutates the map
        const std::uint32_t here = files_[current].dir;
        const std::size_t count = files_[current].includes.size();
        const auto previous = expanded.find(current);
        if (previous == expanded.end()) {
            Expanded first{searched, std::vector<Answer>(count)};
            for (std::size_t k = 0; k < count; ++k) {
                // By index and by copy of the name: resolve() may read files (files_ grows, references into it would dangle).
                const bool angled = files_[current].includes[k].angled;
                const std::string name = files_[current].includes[k].name;
                Answer& a = first.answers[k];
                if (!angled) {
                    a.hit = resolve(here, name);
                    if (a.hit >= 0) {
                        a.from = From::Here;
                    } else {
                        best_stack_hit(searched, name, nullptr, &a);
                    }
                }
                for (std::size_t i = 0; a.hit < 0 && i < search.size(); ++i) {
                    a.hit = resolve(search[i], name);
                    if (a.hit >= 0) {
                        a.from = From::Search;
                    }
                }
                if (a.hit >= 0) {
                    reach(static_cast<std::uint32_t>(a.hit), searched);
                }
            }
            expanded.emplace(current, std::move(first));
            continue;
        }
        Expanded& again = previous->second;
        Bits added(searched.size(), 0);
        bool any_added = false;
        for (std::size_t w = 0; w < searched.size(); ++w) {
            added[w] = searched[w] & ~(w < again.seen.size() ? again.seen[w] : 0);
            any_added = any_added || added[w] != 0;
        }
        if (!any_added) {
            continue;  // queued twice before it was expanded: the first expansion already used everything
        }
        for (std::size_t k = 0; k < count; ++k) {
            const bool angled = files_[current].includes[k].angled;
            const std::string name = files_[current].includes[k].name;
            Answer& a = again.answers[k];
            bool changed = false;
            if (!angled && a.from != From::Here) {
                Answer better;
                if (best_stack_hit(added, name, a.from == From::Stack ? &a : nullptr, &better)) {
                    a = better;
                    changed = true;
                }
            }
            if (a.hit >= 0) {
                reach(static_cast<std::uint32_t>(a.hit), changed ? searched : added);
            }
        }
        again.seen = searched;
    }
    std::set<std::string> paths;
    for (const std::uint32_t id : found) {
        paths.insert(files_[id].path);
    }
    return paths;
}

std::set<std::string> IncludeScanner::dependencies(const ScanRoots& roots) {
    ++stats_.walks;
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
    ++stats_.walks;
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
