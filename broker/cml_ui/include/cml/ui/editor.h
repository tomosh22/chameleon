#pragma once

// "Open this error in my editor". The command is built as an argument vector (never a shell string) and started
// with CreateProcessW by the Windows layer, so a file name cannot inject anything. Presets:
//
//   VSCode        code -g {file}:{line}:{col}
//   VisualStudio  devenv /edit {file} /command "Edit.GoTo {line}"
//   Custom        the user's template, split into arguments first and substituted second, with {file} {line} {col}
//                 ({{ and }} are literal braces). The program (first word) must not contain a placeholder.
//
// Portable; resolving the program on PATH is the launcher's job (src/win/editor_launch.cpp).

#include <string>
#include <string_view>
#include <vector>

#include "cml/ui/compile_output.h"
#include "cml/util/result.h"

namespace cml::ui {

enum class EditorKind { VSCode, VisualStudio, Custom };

struct EditorConfig {
    EditorKind kind = EditorKind::VSCode;
    std::string custom_template;  // used when kind == Custom
    std::string program;          // optional full path replacing the preset's program (code / devenv)
};

struct EditorLaunch {
    std::vector<std::string> argv;  // UTF-8; argv[0] is the program name or path, not yet resolved
};

// CML5703 for a bad template or an unusable location (empty file, control characters).
Result<EditorLaunch> make_editor_launch(const EditorConfig& config, const SourceLocation& location);

// Whitespace-separated words; "double quotes" group (a quote inside quotes is written \"). false when a quote is
// left open or the template is empty.
bool split_command_template(std::string_view text, std::vector<std::string>* words);

// Substitutes {file} {line} {col} in one word. nullopt-like failure through `error` (unknown placeholder, lone brace).
bool substitute_placeholders(std::string_view word, const SourceLocation& location, std::string* out, std::string* error);

// Quotes one argument for CommandLineToArgvW / the MSVC runtime.
std::string quote_argument(std::string_view argument);
// Program and arguments, each quoted, joined with spaces.
std::string join_command_line(const std::vector<std::string>& argv);

const char* to_string(EditorKind kind) noexcept;
bool editor_kind_from_string(std::string_view text, EditorKind* kind) noexcept;

}  // namespace cml::ui
