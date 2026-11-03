#pragma once

// Compile on save (plan 4.14, WP-14): watches the directories of the tracked source files and starts a reload
// when a save arrives.
//
//   * The directories are the union of those holding any file the PDB indices track (ModuleIndex::tracked_files),
//     recursive where that would be too many (cml_watch plan_watch_roots). rearm() recomputes them; the
//     ReloadService calls it when the option is switched on, when an index becomes ready and when modules come
//     and go.
//   * Events are debounced (150 ms) and coalesced (cml_watch ChangeCoalescer): the temporaries and renames of an
//     editor's safe save collapse into "the file exists". A batch that touches no tracked file is dropped.
//   * The confirmation by content hash is the reload's own sweep (SourceBaseline, WP-12): a save that does not
//     change the content, or a touch, finds nothing to compile.
//   * A save that arrives while a reload runs is not lost and does not start a second concurrent reload: the
//     events are coalesced while the running reload finishes and one follow-up reload then runs.
//
// Portable: the directory watcher is behind a factory, the reload behind a hook.

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "cml/util/diag.h"
#include "cml/watch/watch.h"

namespace cml::core {

struct SaveWatcherHooks {
    // Full paths (normalised) of every tracked source and header.
    std::function<std::vector<std::string>()> tracked_files;
    enum class ReloadResult { Done, Busy, Failed };
    // Runs one reload (blocking). Busy: another reload is running, try again shortly.
    std::function<ReloadResult()> reload;
    // Watcher problems (CML5500..CML5504); `severity` is "warning" or "info". May be empty.
    std::function<void(const Diag&, const char* severity)> report;
};

using WatcherFactory = std::function<Result<std::unique_ptr<watch::DirectoryWatcher>>(
    const std::vector<watch::WatchRoot>&, watch::DirectoryWatcher::Sink, std::vector<Diag>*)>;

struct SaveWatcherOptions {
    watch::CoalesceOptions coalesce;
    std::chrono::milliseconds busy_retry{50};
    // Replaces DirectoryWatcher::start (tests).
    WatcherFactory factory;
};

class SaveWatcher {
public:
    SaveWatcher(SaveWatcherHooks hooks, SaveWatcherOptions options = {});
    ~SaveWatcher();
    SaveWatcher(const SaveWatcher&) = delete;
    SaveWatcher& operator=(const SaveWatcher&) = delete;

    // Recomputes the watched directories from hooks.tracked_files(). Thread-safe; blocks briefly.
    void rearm();
    // Feeds an event as if the directory watcher had reported it (tests).
    void inject(const watch::FsEvent& event) { dispatcher_->push(event); }

    std::size_t reloads_started() const noexcept { return reloads_.load(); }
    std::vector<watch::WatchRoot> roots() const;

private:
    void on_batch(const watch::ChangeBatch& batch);

    SaveWatcherHooks hooks_;
    SaveWatcherOptions options_;
    std::atomic<bool> stopping_{false};
    std::atomic<std::size_t> reloads_{0};

    mutable std::mutex mutex_;  // tracked_, roots_, watcher_
    std::unordered_set<std::string> tracked_;
    std::vector<watch::WatchRoot> roots_;
    std::unique_ptr<watch::ChangeDispatcher> dispatcher_;
    std::unique_ptr<watch::DirectoryWatcher> watcher_;  // destroyed before dispatcher_ (declared after it)
};

}  // namespace cml::core
