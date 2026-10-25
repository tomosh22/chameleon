#pragma once

// compile_commands.json support (plan section 4.3 fallback, ADR-0012): a small bounds-checked JSON
// reader (no third-party dependency) and lookup by absolute source path.

#include <string>
#include <string_view>
#include <vector>

#include "cml/toolchain/record.h"
#include "cml/util/result.h"

namespace cml::toolchain {

struct CompileCommandEntry {
    std::string directory;
    std::string file;
    std::string output;
    std::string command;                // "command": one string, program first
    std::vector<std::string> arguments; // "arguments": argv, program first
    bool has_arguments = false;
};

inline constexpr std::size_t kMaxJsonBytes = 512u * 1024u * 1024u;
inline constexpr int kMaxJsonDepth = 64;

// Parses the whole document. CML1505 on any syntax error (message carries the byte offset),
// document larger than kMaxJsonBytes, or nesting deeper than kMaxJsonDepth. Entries without a
// "file" are skipped; the top level must be an array of objects.
Result<std::vector<CompileCommandEntry>> parse_compile_commands(std::string_view json);
// Reads and parses a file. CML1506 when it cannot be read.
Result<std::vector<CompileCommandEntry>> load_compile_commands(const std::string& path);

// First entry whose file (resolved against its directory) equals `source` (resolved against
// `cwd_hint`); comparison is path-normalised and ASCII case-insensitive. nullptr if none.
const CompileCommandEntry* find_compile_command(const std::vector<CompileCommandEntry>& entries,
                                                std::string_view source, std::string_view cwd_hint = {});

// Builds a record from an entry: cwd = directory, compiler = first token, args = the rest, source =
// the entry's file made absolute. recorded_pdb stays empty. CML1507 if `entry` has no tokens.
Result<CompileRecord> record_from_compile_command(const CompileCommandEntry& entry);

// load + find + record in one step. CML1507 when `source` has no entry.
Result<CompileRecord> record_from_compile_commands_file(const std::string& compile_commands_path,
                                                        std::string_view source);

}  // namespace cml::toolchain
