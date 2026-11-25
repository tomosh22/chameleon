#pragma once

// Unified-diff reader and applier (no dependency on patch.exe, which is not reliably present on
// Windows images). Used by the scenario `edit` step.
//
// Supported: `--- a/path` / `+++ b/path` file headers (optional timestamps after a tab), any
// number of files and hunks, `@@ -a,b +c,d @@` headers (counts default to 1), context / removed /
// added lines, `\ No newline at end of file`, and git extended headers (`diff --git`, `index`,
// mode lines are skipped). File creation (`--- /dev/null`) and deletion (`+++ /dev/null`) work.
//
// Applying is context checked: every context and removed line must equal the file's line, ignoring
// only the line terminator (so a CRLF checkout matches an LF patch). A hunk that is not at its
// stated position is searched for nearby (like `patch`, reported as an offset); no fuzz. Added
// lines get the file's dominant line terminator. A mismatch fails with CML5413 naming the file, the
// hunk and the first line that differs; nothing is written unless every hunk of every file applies.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cml/util/result.h"

namespace cmltest {

struct HunkLine {
    char op = ' ';  // ' ' context, '-' removed, '+' added
    std::string text;
    bool no_newline = false;  // followed by "\ No newline at end of file"
};

struct Hunk {
    long old_start = 0;
    long old_count = 0;
    long new_start = 0;
    long new_count = 0;
    std::vector<HunkLine> lines;
};

struct FilePatch {
    std::string old_path;
    std::string new_path;
    std::vector<Hunk> hunks;
};

// CML5412 for text that is not a unified diff or is truncated (names the line).
cml::Result<std::vector<FilePatch>> parse_unified_diff(std::string_view text);

// Applies the hunks of one file to `original`. `notes` (optional) receives one line per hunk that
// applied at an offset. CML5413 on mismatch.
cml::Result<std::string> apply_file_patch(std::string_view original, const FilePatch& patch,
                                          std::vector<std::string>* notes = nullptr);

struct AppliedPatch {
    std::vector<std::string> files;  // paths relative to the root, as changed
    std::vector<std::string> notes;  // "hunk 2 of logic.cpp applied at offset +3"
    // WP-39: what each file of `files` held before the patch (no value: the patch created it). The runner uses it to put an
    // external source tree back after the run; for a scratch copy of a fixture nobody reads it.
    std::vector<std::optional<std::string>> originals;
    // WP-44 (R-49): the directories the patch created for its new files (absolute, the deepest first). Putting an external tree back
    // removes them again (restore_tree).
    std::vector<std::filesystem::path> created_dirs;
};

// Parses `patch_text` and applies it to the tree below `root` (like `patch -p<strip>`). Paths that
// are absolute or contain `..` are refused (CML5412). CML5422 when a file cannot be read or written.
// Nothing is written unless every hunk of every file applies; a WRITE can still fail half way (a locked file, a full disk).
// `partial` (optional, WP-39b) then receives the files that were written, removed or damaged before the failure (the file
// whose write failed included) with what they held before, in the shape of the result, so that the caller can put them
// back. It is left empty when the function succeeds or fails before the first write.
cml::Result<AppliedPatch> apply_patch_to_tree(const std::filesystem::path& root, std::string_view patch_text, int strip = 1,
                                              AppliedPatch* partial = nullptr);

// WP-44 (R-49): puts a tree that patches edited in place back: each file of `originals` gets its content again (no value: the patch
// created it, it is removed), then each directory of `created_dirs` is removed, the deepest first, when it is empty (a directory that
// holds something else stays, and is not a problem). Returns the problems, "; "-separated ("" when there were none); `restored` counts
// the files put back.
std::string restore_tree(const std::map<std::filesystem::path, std::optional<std::string>>& originals, const std::vector<std::filesystem::path>& created_dirs,
                         std::size_t* restored);

}  // namespace cmltest
