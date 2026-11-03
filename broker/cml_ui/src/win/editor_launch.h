#pragma once

#include <string>
#include <vector>

#include "cml/ui/editor.h"
#include "cml/util/result.h"

namespace cml::ui::win {

// Resolves `program` (a name such as "code" or "devenv", or a path) to an .exe: PATH lookup with SearchPathW, and
// for "code" the Visual Studio Code install locations. Batch files (.cmd, .bat) are never started: CML5712.
// CML5704 when nothing is found.
Result<std::wstring> resolve_editor_program(const std::string& program);

// Starts the editor on `location` with CreateProcessW (no shell). Returns once the process is created.
// A relative file name is looked up below each of `search_dirs` (and up to four parents of each); the first hit becomes the
// absolute file name and the editor's working directory. Without a hit the editor starts in the UI's directory.
Result<void> launch_editor(const EditorConfig& config, const SourceLocation& location, const std::vector<std::string>& search_dirs);

}  // namespace cml::ui::win
