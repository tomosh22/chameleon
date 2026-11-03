#pragma once

// The source-hash baseline and the `reload` sweep (plan section 3 step 1, WP-12).
//
// There is no file watcher in M1 (WP-14). When a module is enabled the broker records, for every source
// and header file its compilands were built from, the XXH3 hash of the content (cml_util), the size and
// the modification time. `reload` sweeps the same files: a file whose size and time are unchanged is
// unchanged (cheap exit for the thousands of headers of a big project); otherwise it is read and hashed,
// and it counts as changed only when the hash differs from the baseline (touching a file does not
// trigger a rebuild). After a successful patch the caller accepts the processed files, which re-baselines
// them; files of a failed reload stay "changed" and are processed again by the next reload.
//
// System headers (the compiler's own include directories, the Windows SDK, the LLVM/MSVC trees) are not
// tracked by default: nobody edits them, and they are most of a typical file list. `track_system` brings
// them back.
//
// Portable (no Win32); thread-safe.

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "cml/util/result.h"

namespace cml::core {

struct BaselineOptions {
    bool track_system = false;
    // Called with (files done, files total) while the baseline is taken; may be empty.
    std::function<void(std::size_t done, std::size_t total)> progress;
    // Polled between files; true abandons the baseline (the result is then incomplete).
    std::function<bool()> cancelled;
};

// True for paths under the toolchain and SDK trees (compared case-insensitively, either separator).
bool is_system_path(std::string_view path);

struct FileStamp {
    std::uint64_t hash = 0;
    std::uint64_t size = 0;
    long long mtime = 0;
};

class SourceBaseline {
public:
    // Hashes every existing file of `files` (a file that cannot be read gets a warning and is not tracked).
    // `warnings` may be null.
    void take(const std::vector<std::string>& files, const BaselineOptions& options, std::vector<Diag>* warnings);

    struct SweepResult {
        std::vector<std::string> changed;  // content differs from the baseline
        // The stamp each changed file had when the sweep read it: accept() records exactly these, so an edit
        // made while the patch was being built is not swallowed.
        std::map<std::string, FileStamp> stamps;
        std::vector<std::string> missing;  // tracked but gone now
        std::size_t hashed = 0;            // files that had to be read (size or time differed)
        std::size_t checked = 0;
    };
    SweepResult sweep() const;

    // Re-baselines files with the stamps the sweep saw (after their patch was applied).
    void accept(const std::map<std::string, FileStamp>& stamps);

    std::size_t size() const;
    bool tracks(const std::string& path) const;
    // Every tracked path (WP-14: the directories the file watcher watches are derived from it).
    std::vector<std::string> files() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, FileStamp> stamps_;
};

// Stamps of one file; false when it cannot be read.
bool stamp_file(const std::string& path, FileStamp* out);

}  // namespace cml::core
