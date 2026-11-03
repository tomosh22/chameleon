#pragma once

#include <string>

#include "cml/ui/editor.h"
#include "cml/util/result.h"

namespace cml::ui::win {

// Resolves `program` (a name such as "code" or "devenv", or a path) to an .exe: PATH lookup with SearchPathW, and
// for "code" the Visual Studio Code install locations. Batch files (.cmd, .bat) are never started: CML5712.
// CML5704 when nothing is found.
Result<std::wstring> resolve_editor_program(const std::string& program);

// Starts the editor on `location` with CreateProcessW (no shell). Returns once the process is created.
Result<void> launch_editor(const EditorConfig& config, const SourceLocation& location);

}  // namespace cml::ui::win
