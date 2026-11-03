#pragma once

// Change detection (plan 4.14, WP-14): what a directory watcher reports, how bursts of events are debounced
// and coalesced, how the set of watched directories is chosen, and the Windows ReadDirectoryChangesW watcher.
//
// Everything except DirectoryWatcher::start() is portable (no Win32) and unit-tested on every host. The
// confirmation by content hash is not here: it is the SourceBaseline sweep of cml_core (WP-12), which the
// consumer runs when a batch arrives.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cml/util/result.h"

namespace cml::watch {

const char* library_name() noexcept;

// ---- raw events ----------------------------------------------------------------------------------------

enum class FsAction : std::uint8_t {
    Added,        // FILE_ACTION_ADDED
    Removed,      // FILE_ACTION_REMOVED
    Modified,     // FILE_ACTION_MODIFIED
    RenamedFrom,  // FILE_ACTION_RENAMED_OLD_NAME: the path no longer exists
    RenamedTo,    // FILE_ACTION_RENAMED_NEW_NAME: the path now exists
    Overflow,     // the notification buffer overflowed: events were lost, everything must be re-checked
    RootLost,     // the watched directory itself is gone or unreadable (path = the root); its events stop
};

struct FsEvent {
    FsAction action = FsAction::Modified;
    std::string path;  // full path (any case, either separator); empty for Overflow
};

// The comparison form of a path: lower-case ASCII, backslashes, no "\\?\" prefix. This is the form the
// SourceBaseline keys use (cml::pdb::normalize_path for absolute paths).
std::string watch_key(const std::string& path);

// ---- coalescing ----------------------------------------------------------------------------------------

struct ChangeBatch {
    // Paths whose last event says they exist (written, created, renamed to). Normalised, sorted, unique.
    // The safe-save patterns of editors (write a temporary file, rename the original away, rename the
    // temporary file into place, delete the old one) all end with the saved file in this list; the
    // temporaries do not appear, because their last event says they are gone.
    std::vector<std::string> present;
    // Paths whose last event says they are gone (deleted, renamed away).
    std::vector<std::string> gone;
    bool overflow = false;  // events were lost: re-check everything
    std::vector<std::string> lost_roots;  // watched directories that stopped delivering events (normalised)
};

struct CoalesceOptions {
    std::chrono::milliseconds quiet{150};      // plan 4.14: the batch is ready this long after the last event
    std::chrono::milliseconds max_wait{2000};  // ... but never later than this after the first (a busy build tree)
};

// Collects events and releases them as one batch when the burst is over. Not thread-safe; time is passed in.
class ChangeCoalescer {
public:
    using Clock = std::chrono::steady_clock;

    explicit ChangeCoalescer(CoalesceOptions options = {}) : options_(options) {}

    void add(const FsEvent& event, Clock::time_point now);
    bool pending() const noexcept { return has_events_; }
    // When the pending batch becomes ready (only meaningful while pending()).
    Clock::time_point ready_at() const;
    // Moves the pending batch to `out` and returns true when it is ready at `now`.
    bool take(Clock::time_point now, ChangeBatch* out);
    // Moves the pending batch regardless of time.
    ChangeBatch flush();

private:
    CoalesceOptions options_;
    bool has_events_ = false;
    bool overflow_ = false;
    std::vector<std::string> lost_roots_;
    Clock::time_point first_{};
    Clock::time_point last_{};
    std::map<std::string, bool> final_present_;  // key -> the last event left it existing
};

// ---- choosing directories --------------------------------------------------------------------------------

struct WatchRoot {
    std::string directory;  // normalised
    bool recursive = false;
};

// The union of the directories containing `files` (normalised full paths). When that would be more than
// `max_roots` directories, the deepest ones are replaced by their parent watched recursively, until it fits
// or only roots of `min_depth` components remain (a drive root is never watched recursively).
std::vector<WatchRoot> plan_watch_roots(const std::vector<std::string>& files, std::size_t max_roots = 60, std::size_t min_depth = 2);

// ---- the threaded dispatcher -------------------------------------------------------------------------------

// Receives events from any thread, debounces them, and calls `on_batch` on its own thread. `on_batch` may
// block (a reload); events arriving meanwhile are coalesced into the next batch.
class ChangeDispatcher {
public:
    ChangeDispatcher(CoalesceOptions options, std::function<void(const ChangeBatch&)> on_batch);
    ~ChangeDispatcher();
    ChangeDispatcher(const ChangeDispatcher&) = delete;
    ChangeDispatcher& operator=(const ChangeDispatcher&) = delete;

    void push(const FsEvent& event);

private:
    void run();

    std::function<void(const ChangeBatch&)> on_batch_;
    std::mutex mutex_;
    std::condition_variable cv_;
    ChangeCoalescer coalescer_;
    bool stop_ = false;
    std::thread thread_;
};

// ---- the Windows watcher -------------------------------------------------------------------------------------

// One ReadDirectoryChangesW per root, served by one thread. Events are delivered on that thread; the sink
// must be quick (ChangeDispatcher::push is). Roots that cannot be opened are skipped with a warning in
// `skipped`; start() fails only when none could be opened. On other hosts start() fails with CML5502.
class DirectoryWatcher {
public:
    using Sink = std::function<void(const FsEvent&)>;
    static Result<std::unique_ptr<DirectoryWatcher>> start(const std::vector<WatchRoot>& roots, Sink sink, std::vector<Diag>* skipped);
    virtual ~DirectoryWatcher() = default;
};

}  // namespace cml::watch
