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

// Size and modification time of a file (the cheap "unchanged" test); false when it cannot be read. Shared with the export
// cache of module_symbols.cpp (FU-10).
bool stat_file(const std::string& path, std::uint64_t* size, long long* mtime);

// WP-21 (hot-restart): what a relaunched process needs to find the edits of the previous incarnation again. `stamps` is
// the baseline as it stands (files patched so far hold the stamps of their patched content); `originals` holds, for
// every file that a patch was applied for, the stamp it had BEFORE its first patch (what the original binary was
// built from), kept across restarts.
struct BaselineSnapshot {
    std::map<std::string, FileStamp> stamps;
    std::map<std::string, FileStamp> originals;
};

// The size restore() gives a file that must be read by the next sweep (no file has it).
inline constexpr std::uint64_t kRestoredSize = ~static_cast<std::uint64_t>(0);

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
    // (not const: a restored original that turns out unchanged gets its real size and time recorded, see restore())
    SweepResult sweep();

    // Re-baselines files with the stamps the sweep saw (after their patch was applied). The first time a file is accepted
    // its previous stamp is remembered as its "original" (see BaselineSnapshot).
    void accept(const std::map<std::string, FileStamp>& stamps);
    // The recorded stamps of the given files (those that are tracked). WP-22: a revert puts them back.
    std::map<std::string, FileStamp> stamps_of(const std::vector<std::string>& files) const;

    // WP-21. The state to carry into the baseline of a restarted process.
    BaselineSnapshot session_snapshot() const;
    // WP-21. Applied to the freshly taken baseline of the relaunched process: unpatched files get the stamps of the old
    // incarnation (an edit that was saved but not reloaded stays pending); every original file is set back to the
    // content it had in the original binary, so the next sweep finds the edit again. Files this baseline does not track
    // are ignored. Returns how many files will count as changed if their content differs from the original.
    std::size_t restore(const BaselineSnapshot& snapshot);

    std::size_t size() const;
    bool tracks(const std::string& path) const;
    // Every tracked path (WP-14: the directories the file watcher watches are derived from it).
    std::vector<std::string> files() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, FileStamp> stamps_;
    std::map<std::string, FileStamp> originals_;  // WP-21: stamp before the first accepted patch of a file
};

// Stamps of one file; false when it cannot be read.
bool stamp_file(const std::string& path, FileStamp* out);

}  // namespace cml::core
