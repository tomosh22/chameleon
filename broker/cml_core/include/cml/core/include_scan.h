#pragma once

// Header -> compiland mapping that does not depend on the PDB's file list (WP-15, ADR-0013).
//
// cl lists every header it opened in a compiland's file list; clang-cl lists only files that contributed line
// information, so a header holding declarations only is absent and a change to it would map to nothing. The broker
// therefore scans the include graph itself when a module is indexed: for each compiland, from its recorded command
// line (include directories, forced includes) and its source file, it follows `#include` directives through the
// files on disk. The result is an over-approximation by design (conditional includes are followed whichever way the
// condition goes): an extra recompile is harmless, a missed one is not.
//
// The scan is independent of the compiler, so it also serves cl, where it adds headers the PDB list misses (a
// forced include, a header that only declares).
//
// A quoted include is searched like cl and clang-cl do: in the directory of the including file, then in the directories
// of the files further up the include stack, then in the -I list.
//
// Portable (the file reader is injectable); not thread-safe, one scanner per index run.
//
// WP-50 (R-55): what one scanner has learnt serves every translation unit it walks: each distinct path is read (or found
// missing) once, and each (directory, include name) question of the search is answered once. Paths and directories are
// interned, so a walk compares ids instead of building and normalising candidate paths again; the result is the same.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cml/toolchain/record.h"

namespace cml::core {

struct IncludeDirective {
    std::string name;
    bool angled = false;  // <name> instead of "name"
};

// The `#include` / `#include_next` / `#import` directives of a C or C++ file. Comments are skipped; a directive
// whose operand is a macro is ignored (it cannot be resolved without preprocessing).
std::vector<IncludeDirective> parse_includes(std::string_view text);

// What a compile command contributes to the include search.
struct ScanRoots {
    std::string source;                        // the translation unit (absolute or relative to cwd)
    std::string cwd;
    std::vector<std::string> include_dirs;     // -I / /I / -isystem / /external:I / /imsvc, in command-line order
    std::vector<std::string> forced_includes;  // /FI, -include
    bool creates_pch = false;                  // /Yc, -emit-pch, -building-pch-with-obj
    bool uses_pch = false;                     // /Yu, -include-pch
    std::string pch_header;                    // the header named by /Yc<hdr> or /Yu<hdr> ("" when none is named)
};

// Reads the roots out of a recorded command (response files are expanded; an unreadable one is skipped).
ScanRoots scan_roots_from_record(const toolchain::CompileRecord& record);

using ScanFileReader = std::function<std::optional<std::string>(const std::string& path)>;
// The size over which disk_file_reader() does not read a file (8 MiB).
inline constexpr std::uint64_t kScanMaxFileBytes = 8u * 1024u * 1024u;
// std::ifstream-based; files over kScanMaxFileBytes are treated as unreadable.
ScanFileReader disk_file_reader();

class IncludeScanner {
public:
    explicit IncludeScanner(ScanFileReader reader = disk_file_reader());

    // Normalised (pdb::normalize_path) paths of every file the translation unit includes, directly or not, that
    // exists. The source itself is not part of the result. System paths (is_system_path) are neither returned nor
    // followed.
    std::set<std::string> dependencies(const ScanRoots& roots);

    // The files that make up the precompiled header this command builds or uses: the header named by /Yc / /Yu and the
    // forced includes, with everything they include. Independent of the translation unit's own includes; empty when
    // none of them can be found on disk.
    std::set<std::string> pch_files(const ScanRoots& roots);

    std::size_t files_read() const noexcept { return files_.size(); }
    // WP-50: the directories of every path the scan asked about (found or not), normalised. A file appearing in or vanishing
    // from one of them can change what an include resolves to.
    const std::vector<std::string>& directories() const noexcept { return dirs_; }

    // WP-50 (R-55): what the scan cost. Every distinct path the scan asked about is read once (`files_read()` = found + missing);
    // `read_ms` is the time spent in the file reader (opening, reading, and failing to open the paths that do not exist), `parse_ms`
    // in parse_includes, `lookups` counts the (directory, include name) questions of the search, `resolved` those of them that had to
    // build and look up a candidate path (the rest came from the scanner's memory).
    struct Stats {
        std::size_t found = 0;    // paths that could be read
        std::size_t missing = 0;  // paths that could not (no such file, or unreadable): candidates of the include search
        std::size_t lookups = 0;
        std::size_t resolved = 0;
        std::size_t walks = 0;    // dependencies() / pch_files() calls
        double read_ms = 0;
        double parse_ms = 0;
    };
    const Stats& stats() const noexcept { return stats_; }

    // WP-55 (R-58): what one walk depended on. While tracing, every path a walk asks about is noted (read now or answered from
    // the scanner's memory, found or not), once; take_touched() hands over the ids noted since the last call. The answer of a
    // walk depends on nothing but these paths: whether each exists and can be read, and the contents of those that can.
    void set_tracing(bool on) noexcept { tracing_ = on; }
    std::vector<std::uint32_t> take_touched();
    const std::string& path_of(std::uint32_t file) const { return files_[file].path; }
    bool readable(std::uint32_t file) const { return files_[file].readable; }
    // The normalised directory of the path (path::parent_directory; may be empty).
    const std::string& directory_of(std::uint32_t file) const { return dirs_[files_[file].dir]; }

private:
    struct Start {
        std::string file;
        std::set<std::string> inherited_dirs;  // directories of the "including" files above this one
    };
    std::vector<std::string> normalised_dirs(const ScanRoots& roots) const;
    // Finds a forced include / PCH header: next to the source, in the cwd, then in the -I list ("" when absent).
    std::string resolve_forced(const ScanRoots& roots, const std::string& name);
    // Everything reachable from `starts` (the starts included).
    std::set<std::string> walk(const std::vector<Start>& starts, const std::vector<std::string>& dirs);

    const std::vector<IncludeDirective>* includes_of(const std::string& normalised_path);

    // One path the scan asked about, read once.
    struct File {
        std::string path;           // normalised
        bool readable = false;
        bool system = false;        // is_system_path(path): neither returned nor followed
        std::uint32_t dir = 0;      // id of its directory (path::parent_directory)
        std::vector<IncludeDirective> includes;
    };
    std::uint32_t file_id(const std::string& normalised_path);  // reads the file the first time
    std::uint32_t dir_id(const std::string& normalised_dir);
    // The id of `name` searched in directory `dir` when that file can be read, else -1. Answered once per (dir, name).
    std::int64_t resolve(std::uint32_t dir, const std::string& name);
    void touch(std::uint32_t file) {
        if (tracing_) {
            if (touched_mark_.size() <= file) {
                touched_mark_.resize(files_.size() + 1, 0);
            }
            if (touched_mark_[file] == 0) {
                touched_mark_[file] = 1;
                touched_.push_back(file);
            }
        }
    }

    ScanFileReader reader_;
    std::vector<File> files_;
    std::unordered_map<std::string, std::uint32_t> file_ids_;
    std::vector<std::string> dirs_;
    std::unordered_map<std::string, std::uint32_t> dir_ids_;
    std::vector<std::unordered_map<std::string, std::uint32_t>> resolved_;  // by directory id: include name -> file id
    Stats stats_;
    bool tracing_ = false;
    std::vector<std::uint32_t> touched_;
    std::vector<char> touched_mark_;  // by file id: noted in touched_
};

}  // namespace cml::core
