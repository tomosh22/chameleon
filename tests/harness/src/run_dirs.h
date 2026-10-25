#pragma once

// Directories cmltest creates for a run (artifacts, scratch). The user-supplied base directory
// (`--artifacts DIR`, `--work-dir DIR`) is never deleted and never cleaned: cmltest creates its own
// subdirectory `<base>/cmltest-<scenario>-<timestamp>` containing the marker file
// `.cmltest-artifacts`, and only ever removes directories that contain that marker.

#include <filesystem>
#include <string>

#include "cml/util/result.h"

namespace cmltest {

inline constexpr const char* kRunMarkerFile = ".cmltest-artifacts";

// Creates `base` (if needed) and a fresh `cmltest-<scenario>-<UTC timestamp>-p<pid>[-n]` directory
// below it with the marker file. Returns the new directory. CML5421 on failure.
cml::Result<std::filesystem::path> create_run_directory(const std::filesystem::path& base, const std::string& scenario);

// Removes `dir` recursively, but only if it directly contains the marker file and is not a
// filesystem root. Otherwise nothing is touched and CML5421 is returned.
cml::Result<void> remove_run_directory(const std::filesystem::path& dir);

}  // namespace cmltest
