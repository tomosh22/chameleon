#include "cml/ui/editor.h"

#include "cml/ui/ui_diag_ids.h"

namespace cml::ui {

namespace {

Diag template_error(const std::string& message) {
    return Diag::make(ids::kUiEditorTemplate, message,
                      "Fix the editor command in the settings; use {file}, {line} and {col} as placeholders.");
}

bool has_control_character(std::string_view text) noexcept {
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool split_command_template(std::string_view text, std::vector<std::string>* words) {
    words->clear();
    std::string current;
    bool in_word = false;
    bool in_quotes = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_quotes) {
            if (c == '\\' && i + 1 < text.size() && text[i + 1] == '"') {
                current.push_back('"');
                ++i;
            } else if (c == '"') {
                in_quotes = false;
            } else {
                current.push_back(c);
            }
        } else if (c == '"') {
            in_quotes = true;
            in_word = true;
        } else if (c == ' ' || c == '\t') {
            if (in_word) {
                words->push_back(std::move(current));
                current.clear();
                in_word = false;
            }
        } else {
            current.push_back(c);
            in_word = true;
        }
    }
    if (in_quotes) {
        words->clear();
        return false;
    }
    if (in_word) {
        words->push_back(std::move(current));
    }
    return !words->empty();
}

bool substitute_placeholders(std::string_view word, const SourceLocation& location, std::string* out, std::string* error) {
    out->clear();
    const int line = location.line > 0 ? location.line : 1;
    const int column = location.column > 0 ? location.column : 1;
    for (std::size_t i = 0; i < word.size(); ++i) {
        const char c = word[i];
        if (c == '{') {
            if (i + 1 < word.size() && word[i + 1] == '{') {
                out->push_back('{');
                ++i;
                continue;
            }
            const std::size_t close = word.find('}', i);
            if (close == std::string_view::npos) {
                *error = "unterminated '{' in the editor command";
                return false;
            }
            const std::string_view name = word.substr(i + 1, close - i - 1);
            if (name == "file") {
                out->append(location.file);
            } else if (name == "line") {
                out->append(std::to_string(line));
            } else if (name == "col") {
                out->append(std::to_string(column));
            } else {
                *error = "unknown placeholder '{" + std::string(name) + "}' in the editor command";
                return false;
            }
            i = close;
        } else if (c == '}') {
            if (i + 1 < word.size() && word[i + 1] == '}') {
                out->push_back('}');
                ++i;
                continue;
            }
            *error = "lone '}' in the editor command (write '}}' for a literal brace)";
            return false;
        } else {
            out->push_back(c);
        }
    }
    return true;
}

Result<EditorLaunch> make_editor_launch(const EditorConfig& config, const SourceLocation& location) {
    if (location.file.empty()) {
        return template_error("there is no source file to open");
    }
    if (has_control_character(location.file)) {
        return template_error("the source file name contains control characters and is not opened");
    }
    EditorLaunch launch;
    switch (config.kind) {
        case EditorKind::VSCode: {
            launch.argv = {config.program.empty() ? "code" : config.program, "-g",
                           location.file + ":" + std::to_string(location.line > 0 ? location.line : 1) + ":" +
                               std::to_string(location.column > 0 ? location.column : 1)};
            return launch;
        }
        case EditorKind::VisualStudio: {
            launch.argv = {config.program.empty() ? "devenv" : config.program, "/edit", location.file, "/command",
                           "Edit.GoTo " + std::to_string(location.line > 0 ? location.line : 1)};
            return launch;
        }
        case EditorKind::Custom: break;
    }
    std::vector<std::string> words;
    if (!split_command_template(config.custom_template, &words)) {
        return template_error(config.custom_template.empty() ? "the custom editor command is empty"
                                                             : "the custom editor command has an unterminated quote");
    }
    if (words.front().find('{') != std::string::npos) {
        return template_error("the program of the custom editor command must not contain placeholders");
    }
    for (const std::string& word : words) {
        std::string expanded;
        std::string error;
        if (!substitute_placeholders(word, location, &expanded, &error)) {
            return template_error(error);
        }
        launch.argv.push_back(std::move(expanded));
    }
    if (!config.program.empty()) {
        launch.argv.front() = config.program;
    }
    return launch;
}

std::string quote_argument(std::string_view argument) {
    if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string_view::npos) {
        return std::string(argument);
    }
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (const char c : argument) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(c);
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

std::string join_command_line(const std::vector<std::string>& argv) {
    std::string line;
    for (const std::string& a : argv) {
        if (!line.empty()) {
            line.push_back(' ');
        }
        line += quote_argument(a);
    }
    return line;
}

const char* to_string(EditorKind kind) noexcept {
    switch (kind) {
        case EditorKind::VSCode: return "vscode";
        case EditorKind::VisualStudio: return "visualstudio";
        case EditorKind::Custom: return "custom";
    }
    return "vscode";
}

bool editor_kind_from_string(std::string_view text, EditorKind* kind) noexcept {
    if (text == "vscode") {
        *kind = EditorKind::VSCode;
    } else if (text == "visualstudio") {
        *kind = EditorKind::VisualStudio;
    } else if (text == "custom") {
        *kind = EditorKind::Custom;
    } else {
        return false;
    }
    return true;
}

}  // namespace cml::ui
