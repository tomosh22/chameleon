#include "unified_diff.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#include "cml/ipc/control_ids.h"

namespace cmltest {

namespace fs = std::filesystem;

namespace {

cml::Diag malformed(std::size_t line, const std::string& what) {
    return cml::Diag::make(cml::ids::kPatchMalformed,
                           "cannot read the patch at line " + std::to_string(line) + ": " + what,
                           "Provide a unified diff (`diff -u` or `git diff` output).");
}

std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        const bool has_newline = end != std::string_view::npos;
        if (!has_newline) {
            end = text.size();
        }
        std::string_view line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        pos = has_newline ? end + 1 : end;
    }
    return lines;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

// "path\ttimestamp" -> "path"; also strips surrounding quotes.
std::string header_path(std::string_view rest) {
    const std::size_t tab = rest.find('\t');
    if (tab != std::string_view::npos) {
        rest = rest.substr(0, tab);
    }
    while (!rest.empty() && rest.back() == ' ') {
        rest.remove_suffix(1);
    }
    if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') {
        rest = rest.substr(1, rest.size() - 2);
    }
    return std::string(rest);
}

bool parse_range(std::string_view text, long* start, long* count) {
    // "12,3" or "12"
    const std::size_t comma = text.find(',');
    const std::string first(text.substr(0, comma));
    if (first.empty() || first.find_first_not_of("0123456789") != std::string::npos || first.size() > 9) {
        return false;
    }
    *start = std::strtol(first.c_str(), nullptr, 10);
    if (comma == std::string_view::npos) {
        *count = 1;
        return true;
    }
    const std::string second(text.substr(comma + 1));
    if (second.empty() || second.find_first_not_of("0123456789") != std::string::npos || second.size() > 9) {
        return false;
    }
    *count = std::strtol(second.c_str(), nullptr, 10);
    return true;
}

// "@@ -a,b +c,d @@ ..." -> hunk header fields
bool parse_hunk_header(std::string_view line, Hunk* hunk) {
    if (!starts_with(line, "@@ -")) {
        return false;
    }
    const std::size_t plus = line.find(" +", 4);
    if (plus == std::string_view::npos) {
        return false;
    }
    const std::size_t end = line.find(" @@", plus + 2);
    if (end == std::string_view::npos) {
        return false;
    }
    return parse_range(line.substr(4, plus - 4), &hunk->old_start, &hunk->old_count) &&
           parse_range(line.substr(plus + 2, end - (plus + 2)), &hunk->new_start, &hunk->new_count);
}

}  // namespace

cml::Result<std::vector<FilePatch>> parse_unified_diff(std::string_view text) {
    const std::vector<std::string_view> lines = split_lines(text);
    std::vector<FilePatch> patches;
    std::size_t i = 0;
    while (i < lines.size()) {
        const std::string_view line = lines[i];
        if (!starts_with(line, "--- ")) {
            ++i;  // prose, "diff --git", "index ...", mode lines, blank lines
            continue;
        }
        if (i + 1 >= lines.size() || !starts_with(lines[i + 1], "+++ ")) {
            return malformed(i + 1, "'--- ' header is not followed by '+++ '");
        }
        FilePatch patch;
        patch.old_path = header_path(line.substr(4));
        patch.new_path = header_path(lines[i + 1].substr(4));
        i += 2;
        while (i < lines.size() && starts_with(lines[i], "@@")) {
            Hunk hunk;
            if (!parse_hunk_header(lines[i], &hunk)) {
                return malformed(i + 1, "bad hunk header '" + std::string(lines[i]) + "'");
            }
            ++i;
            long old_seen = 0;
            long new_seen = 0;
            while ((old_seen < hunk.old_count || new_seen < hunk.new_count) && i < lines.size()) {
                const std::string_view body = lines[i];
                HunkLine hl;
                if (starts_with(body, "\\")) {
                    if (hunk.lines.empty()) {
                        return malformed(i + 1, "'\\ No newline' marker before any line");
                    }
                    hunk.lines.back().no_newline = true;
                    ++i;
                    continue;
                }
                if (body.empty()) {
                    hl.op = ' ';  // an empty context line whose trailing space was stripped
                } else if (body[0] == ' ' || body[0] == '-' || body[0] == '+') {
                    hl.op = body[0];
                    hl.text = std::string(body.substr(1));
                } else {
                    return malformed(i + 1, "unexpected line inside a hunk (counts in the @@ header too large?)");
                }
                if (hl.op != '+') {
                    ++old_seen;
                }
                if (hl.op != '-') {
                    ++new_seen;
                }
                if (old_seen > hunk.old_count || new_seen > hunk.new_count) {
                    return malformed(i + 1, "hunk has more lines than its @@ header says");
                }
                hunk.lines.push_back(std::move(hl));
                ++i;
            }
            if (old_seen != hunk.old_count || new_seen != hunk.new_count) {
                return malformed(i + 1, "the patch ends inside a hunk (counts in the @@ header too large?)");
            }
            // A "\ No newline" marker right after the last counted line.
            while (i < lines.size() && starts_with(lines[i], "\\")) {
                if (!hunk.lines.empty()) {
                    hunk.lines.back().no_newline = true;
                }
                ++i;
            }
            // More hunk lines than the header announced (a stale or hand-edited @@ line) must not be dropped silently.
            if (i < lines.size() && !lines[i].empty()) {
                const char next = lines[i][0];
                const bool next_is_file_header = starts_with(lines[i], "--- ") && i + 1 < lines.size() && starts_with(lines[i + 1], "+++ ");
                if (next == ' ' || next == '+' || (next == '-' && !next_is_file_header)) {
                    return malformed(i + 1, "hunk has more lines than its @@ header says");
                }
            }
            patch.hunks.push_back(std::move(hunk));
        }
        if (patch.hunks.empty()) {
            return malformed(i + 1, "file '" + patch.new_path + "' has no hunks");
        }
        patches.push_back(std::move(patch));
    }
    if (patches.empty()) {
        return malformed(1, "no '--- ' / '+++ ' file headers found");
    }
    return patches;
}

namespace {

struct FileLine {
    std::string text;
    std::string eol;  // "\n", "\r\n" or "" (last line without terminator)
};

std::vector<FileLine> split_file(std::string_view data) {
    std::vector<FileLine> out;
    std::size_t pos = 0;
    while (pos < data.size()) {
        const std::size_t nl = data.find('\n', pos);
        FileLine line;
        if (nl == std::string_view::npos) {
            line.text = std::string(data.substr(pos));
            pos = data.size();
        } else {
            std::size_t text_end = nl;
            line.eol = "\n";
            if (text_end > pos && data[text_end - 1] == '\r') {
                --text_end;
                line.eol = "\r\n";
            }
            line.text = std::string(data.substr(pos, text_end - pos));
            pos = nl + 1;
        }
        out.push_back(std::move(line));
    }
    return out;
}

std::string dominant_eol(const std::vector<FileLine>& lines) {
    std::size_t crlf = 0;
    std::size_t lf = 0;
    for (const FileLine& l : lines) {
        if (l.eol == "\r\n") ++crlf;
        else if (l.eol == "\n") ++lf;
    }
    if (crlf == 0 && lf == 0) {
        return "\n";
    }
    return crlf > lf ? "\r\n" : "\n";
}

// Index of the first line of `hunk`'s old side that does not match at `at`, or -1 when all match.
long first_mismatch(const std::vector<FileLine>& file, const Hunk& hunk, std::size_t at) {
    std::size_t k = at;
    long n = 0;
    for (const HunkLine& hl : hunk.lines) {
        if (hl.op == '+') {
            continue;
        }
        if (k >= file.size() || file[k].text != hl.text) {
            return n;
        }
        ++k;
        ++n;
    }
    return -1;
}

}  // namespace

cml::Result<std::string> apply_file_patch(std::string_view original, const FilePatch& patch,
                                          std::vector<std::string>* notes) {
    std::vector<FileLine> file = split_file(original);
    const std::string eol = dominant_eol(file);
    long delta = 0;  // lines added so far (new positions shift)
    int hunk_number = 0;
    for (const Hunk& hunk : patch.hunks) {
        ++hunk_number;
        // Position of the first old-side line, 0-based. For a pure insertion (old_count == 0) the
        // header's start is the line AFTER which to insert.
        long stated = (hunk.old_count == 0 ? hunk.old_start : hunk.old_start - 1) + delta;
        if (stated < 0) {
            stated = 0;
        }
        std::size_t old_lines = 0;
        for (const HunkLine& hl : hunk.lines) {
            if (hl.op != '+') ++old_lines;
        }
        long found = -1;
        long offset = 0;
        const long file_size = static_cast<long>(file.size());
        for (long d = 0; d <= file_size + 1 && found < 0; ++d) {
            for (const long candidate : {stated - d, stated + d}) {
                if (candidate < 0 || candidate > file_size) {
                    continue;
                }
                if (first_mismatch(file, hunk, static_cast<std::size_t>(candidate)) < 0) {
                    found = candidate;
                    offset = candidate - stated;
                    break;
                }
                if (d == 0) {
                    break;  // stated - 0 == stated + 0
                }
            }
        }
        if (found < 0) {
            const long bad = first_mismatch(file, hunk, static_cast<std::size_t>(std::min<long>(stated, file_size)));
            std::string expected;
            long n = 0;
            for (const HunkLine& hl : hunk.lines) {
                if (hl.op == '+') continue;
                if (n == bad) {
                    expected = hl.text;
                    break;
                }
                ++n;
            }
            const std::size_t line_index = static_cast<std::size_t>(std::min<long>(stated, file_size)) + static_cast<std::size_t>(std::max<long>(bad, 0));
            const std::string actual = line_index < file.size() ? file[line_index].text : "<end of file>";
            return cml::Diag::make(
                cml::ids::kPatchMismatch,
                "hunk " + std::to_string(hunk_number) + " of '" + patch.new_path + "' does not match the file near line " +
                    std::to_string(stated + 1) + ": expected \"" + expected + "\" but the file has \"" + actual + "\"",
                "The fixture source changed; regenerate the .edit file against the current sources.");
        }
        if (offset != 0 && notes != nullptr) {
            notes->push_back("hunk " + std::to_string(hunk_number) + " of '" + patch.new_path + "' applied at offset " +
                             (offset > 0 ? "+" : "") + std::to_string(offset));
        }
        std::vector<FileLine> replacement;
        std::size_t k = static_cast<std::size_t>(found);
        for (const HunkLine& hl : hunk.lines) {
            if (hl.op == ' ') {
                replacement.push_back(file[k++]);  // keep the file's own line (and terminator)
            } else if (hl.op == '-') {
                ++k;
            } else {
                FileLine added;
                added.text = hl.text;
                added.eol = hl.no_newline ? std::string() : eol;
                replacement.push_back(std::move(added));
            }
        }
        // A replaced last line that had no terminator and is followed by nothing keeps none only if
        // the patch says so; a context line keeps its own terminator.
        file.erase(file.begin() + found, file.begin() + found + static_cast<long>(old_lines));
        file.insert(file.begin() + found, replacement.begin(), replacement.end());
        delta += static_cast<long>(replacement.size()) - static_cast<long>(old_lines);
    }
    std::string result;
    for (const FileLine& l : file) {
        result += l.text;
        result += l.eol;
    }
    return result;
}

namespace {

cml::Result<std::string> strip_path(const std::string& path, int strip) {
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos <= normalized.size()) {
        std::size_t slash = normalized.find('/', pos);
        if (slash == std::string::npos) {
            slash = normalized.size();
        }
        if (slash > pos) {
            parts.push_back(normalized.substr(pos, slash - pos));
        }
        pos = slash + 1;
    }
    if (static_cast<int>(parts.size()) <= strip) {
        return cml::Diag::make(cml::ids::kPatchMalformed, "patch path '" + path + "' has no component left after stripping " + std::to_string(strip),
                               "Check the file headers of the .edit file.");
    }
    std::string out;
    for (std::size_t i = static_cast<std::size_t>(strip); i < parts.size(); ++i) {
        if (parts[i] == ".." || parts[i] == "." || parts[i].find(':') != std::string::npos) {
            return cml::Diag::make(cml::ids::kPatchMalformed, "patch path '" + path + "' leaves the fixture directory or is not relative",
                                   "Patches may only touch files below the fixture directory.");
        }
        if (!out.empty()) out += '/';
        out += parts[i];
    }
    if (!normalized.empty() && normalized[0] == '/') {
        return cml::Diag::make(cml::ids::kPatchMalformed, "patch path '" + path + "' is absolute",
                               "Patches may only touch files below the fixture directory.");
    }
    return out;
}

}  // namespace

cml::Result<AppliedPatch> apply_patch_to_tree(const fs::path& root, std::string_view patch_text, int strip) {
    cml::Result<std::vector<FilePatch>> patches = parse_unified_diff(patch_text);
    if (!patches) {
        return patches.error();
    }
    struct Pending {
        fs::path file;
        std::string relative;
        std::string content;
        bool remove = false;
    };
    std::vector<Pending> pending;
    AppliedPatch applied;
    for (const FilePatch& patch : *patches) {
        const bool is_create = patch.old_path == "/dev/null";
        const bool is_delete = patch.new_path == "/dev/null";
        const std::string& target_name = is_delete ? patch.old_path : patch.new_path;
        cml::Result<std::string> relative = strip_path(target_name, strip);
        if (!relative) {
            return relative.error();
        }
        Pending item;
        item.relative = *relative;
        item.file = root / fs::path(*relative);
        std::string original;
        if (!is_create) {
            std::ifstream in(item.file, std::ios::binary);
            if (!in) {
                return cml::Diag::make(cml::ids::kEditFailed, "cannot read '" + item.file.string() + "' to apply the patch",
                                       "Check that the fixture copy contains the file named in the .edit file.");
            }
            std::ostringstream buffer;
            buffer << in.rdbuf();
            original = buffer.str();
        }
        FilePatch named = patch;
        named.new_path = *relative;
        cml::Result<std::string> patched = apply_file_patch(original, named, &applied.notes);
        if (!patched) {
            return patched.error();
        }
        item.content = std::move(*patched);
        item.remove = is_delete;
        applied.files.push_back(item.relative);
        pending.push_back(std::move(item));
    }
    for (const Pending& item : pending) {
        std::error_code ec;
        if (item.remove) {
            fs::remove(item.file, ec);
        } else {
            fs::create_directories(item.file.parent_path(), ec);
            std::ofstream out(item.file, std::ios::binary | std::ios::trunc);
            out.write(item.content.data(), static_cast<std::streamsize>(item.content.size()));
            out.close();
            if (!out) {
                return cml::Diag::make(cml::ids::kEditFailed, "cannot write '" + item.file.string() + "' while applying the patch",
                                       "Check that the scratch directory is writable and not locked by another process.");
            }
        }
    }
    return applied;
}

}  // namespace cmltest
