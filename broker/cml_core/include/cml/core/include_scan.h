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

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
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
// std::ifstream-based; files over 8 MiB are treated as unreadable.
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

    std::size_t files_read() const noexcept { return parsed_.size(); }

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

    ScanFileReader reader_;
    std::map<std::string, std::optional<std::vector<IncludeDirective>>> parsed_;  // nullopt: unreadable
};

}  // namespace cml::core
