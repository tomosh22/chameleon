#pragma once

// Compiler and linker output as the compile-output panel shows it: lines of the form
//   C:\src\a.cpp(12,5): error C2065: 'x': undeclared identifier        (cl, clang-cl)
//   C:\src\a.cpp(12): warning C4100: 'y': unreferenced parameter       (cl, no column)
//   C:\src\a.cpp:12:5: error: use of undeclared identifier 'x'          (clang, gcc style)
//   a.obj : error LNK2019: unresolved external symbol f                 (linker; no line, not clickable)
// are parsed into a severity, a source location (when there is one) and the text. Portable, no I/O.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cml::ui {

enum class OutputSeverity { Info, Warning, Error };

struct SourceLocation {
    std::string file;  // as printed by the tool (UTF-8); may be relative
    int line = 0;      // 1-based; 0 = unknown
    int column = 0;    // 1-based; 0 = unknown
    // A location the editor can jump to.
    bool clickable() const noexcept { return !file.empty() && line > 0; }
};

struct CompilerMessage {
    OutputSeverity severity = OutputSeverity::Info;
    SourceLocation location;
    std::string code;  // "C2065", "LNK2019", "" for clang
    std::string text;  // message without location, severity and code
};

// nullopt when `line` is not a diagnostic line (plain output, notes without a severity keyword, ...).
std::optional<CompilerMessage> parse_compiler_line(std::string_view line);

// Splits on \n (and \r\n); a trailing newline does not produce an empty last line. Empty lines inside are kept.
std::vector<std::string> split_output_lines(std::string_view text);

const char* to_string(OutputSeverity severity) noexcept;

}  // namespace cml::ui
