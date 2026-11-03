#include "cml/core/save_watcher.h"

#include <thread>

#include "cml/util/log.h"
#include "cml/watch/watch_diag_ids.h"

namespace cml::core {

namespace wd = watch::diag;

namespace {

bool same_roots(const std::vector<watch::WatchRoot>& a, const std::vector<watch::WatchRoot>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].directory != b[i].directory || a[i].recursive != b[i].recursive) {
            return false;
        }
    }
    return true;
}

}  // namespace

SaveWatcher::SaveWatcher(SaveWatcherHooks hooks, SaveWatcherOptions options) : hooks_(std::move(hooks)), options_(std::move(options)) {
    if (!options_.factory) {
        options_.factory = [](const std::vector<watch::WatchRoot>& roots, watch::DirectoryWatcher::Sink sink, std::vector<Diag>* skipped) {
            return watch::DirectoryWatcher::start(roots, std::move(sink), skipped);
        };
    }
    dispatcher_ = std::make_unique<watch::ChangeDispatcher>(options_.coalesce, [this](const watch::ChangeBatch& batch) { on_batch(batch); });
}

SaveWatcher::~SaveWatcher() {
    stop();
    dispatcher_.reset();  // joins the thread, which may be waiting to retry a busy reload
}

std::vector<watch::WatchRoot> SaveWatcher::roots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return roots_;
}

void SaveWatcher::report_once(const std::string& key, const Diag& diag, const char* severity, std::vector<std::pair<Diag, const char*>>* out) {
    if (reported_.insert(key).second) {
        out->emplace_back(diag, severity);
    }
}

void SaveWatcher::flush_reports(const std::vector<std::pair<Diag, const char*>>& out) {
    for (const auto& r : out) {
        if (r.second[0] == 'i') {
            CML_LOG(Info, "compile-on-save: %s", r.first.format().c_str());
        } else {
            CML_LOG(Warn, "compile-on-save: %s", r.first.format().c_str());
        }
        if (hooks_.report) {
            hooks_.report(r.first, r.second);
        }
    }
}

void SaveWatcher::stop() {
    stopping_.store(true);
    std::unique_ptr<watch::DirectoryWatcher> old;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        old = std::move(watcher_);
    }
    old.reset();
}

std::optional<Diag> SaveWatcher::rearm(bool force) {
    if (stopping_.load()) {
        return std::nullopt;
    }
    const std::vector<std::string> files = hooks_.tracked_files ? hooks_.tracked_files() : std::vector<std::string>();
    std::unordered_set<std::string> tracked;
    tracked.reserve(files.size());
    for (const std::string& f : files) {
        tracked.insert(watch::watch_key(f));
    }
    const std::vector<watch::WatchRoot> roots = watch::plan_watch_roots(files);

    std::vector<std::pair<Diag, const char*>> reports;
    std::optional<Diag> failure;
    std::string first_file;
    std::unique_ptr<watch::DirectoryWatcher> old;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tracked_ = std::move(tracked);
        if (roots.empty()) {
            old = std::move(watcher_);
            roots_.clear();
            degraded_ = false;
            report_once("nothing", Diag::make(wd::kNothingToWatch, "compile-on-save is on but no source directory is known yet",
                                              "Enable a module; the directories are watched as soon as its PDB index is ready."),
                        "info", &reports);
        } else if (watcher_ && !force && !degraded_ && same_roots(roots, roots_)) {
            // Nothing changed.
        } else {
            reported_.erase("nothing");
            std::vector<Diag> skipped;
            Result<std::unique_ptr<watch::DirectoryWatcher>> started = options_.factory(
                roots, [this](const watch::FsEvent& event) { dispatcher_->push(event); }, &skipped);
            for (const Diag& d : skipped) {
                report_once("skip:" + d.message, d, "warning", &reports);
            }
            if (!started) {
                report_once("failed:" + started.error().message, started.error(), "warning", &reports);
                degraded_ = true;
                failure = started.error();
            } else {
                old = std::move(watcher_);  // the old watcher goes away after the new one is armed: no gap
                watcher_ = std::move(*started);
                roots_ = roots;
                degraded_ = !skipped.empty();
                if (!degraded_) {
                    reported_.clear();  // recovered: a later problem is a new one and is reported again
                }
                first_file = watch::watch_key(files.front());
                CML_LOG(Info, "compile-on-save: watching %zu directories for %zu tracked files", roots_.size(), tracked_.size());
                for (const watch::WatchRoot& r : roots_) {
                    CML_LOG(Debug, "compile-on-save: watching %s%s", r.directory.c_str(), r.recursive ? " (recursive)" : "");
                }
            }
        }
    }
    old.reset();
    flush_reports(reports);
    if (!first_file.empty()) {
        // Saves made before the directories were watched (between the baseline and now) are picked up by one sweep.
        dispatcher_->push(watch::FsEvent{watch::FsAction::Modified, first_file});
    }
    return failure;
}

void SaveWatcher::on_batch(const watch::ChangeBatch& batch) {
    if (stopping_.load()) {
        return;
    }
    if (!batch.lost_roots.empty()) {
        // A watched directory stopped delivering events (deleted, renamed, access lost). Report it once and rebuild
        // the watch; a directory that cannot be reopened is reported as unwatched and retried by every later rearm.
        std::vector<std::pair<Diag, const char*>> reports;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const std::string& root : batch.lost_roots) {
                report_once("lost:" + root,
                            Diag::make(wd::kRootNotWatched, "the directory '" + root + "' stopped delivering change events",
                                       "Saves there are picked up by the next `reload`; the watch is being rebuilt."),
                            "warning", &reports);
            }
        }
        flush_reports(reports);
        rearm(true);
    }
    bool relevant = batch.overflow;
    std::size_t hits = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const std::string& p : batch.present) {
            if (tracked_.count(p) != 0) {
                ++hits;
                CML_LOG(Trace, "compile-on-save: %s changed", p.c_str());
            }
        }
    }
    relevant = relevant || hits != 0;
    if (batch.overflow && batch.lost_roots.empty()) {
        CML_LOG(Warn, "compile-on-save: file events were lost; every tracked file is re-checked");
        if (hooks_.report) {
            hooks_.report(Diag::make(wd::kOverflow, "the directory watcher lost events (buffer overflow)",
                                     "Nothing to do: every tracked source is re-checked by content hash."),
                          "warning");
        }
    }
    if (!relevant) {
        CML_LOG(Trace, "compile-on-save: batch of %zu paths touches no tracked file", batch.present.size() + batch.gone.size());
        return;
    }
    CML_LOG(Debug, "compile-on-save: %zu tracked file(s) saved; reloading", hits);
    for (;;) {
        if (stopping_.load()) {
            return;
        }
        reloads_.fetch_add(1);
        const SaveWatcherHooks::ReloadResult r = hooks_.reload ? hooks_.reload() : SaveWatcherHooks::ReloadResult::Done;
        if (r == SaveWatcherHooks::ReloadResult::Done) {
            return;
        }
        if (r == SaveWatcherHooks::ReloadResult::Failed) {
            CML_LOG(Warn, "compile-on-save: the reload failed (its diagnostics were published)");
            if (hooks_.report) {
                hooks_.report(Diag::make(wd::kAutoReloadFailed, "the reload started by a save failed",
                                         "Fix the error reported just before this and save again, or run `reload`."),
                              "warning");
            }
            return;
        }
        reloads_.fetch_sub(1);  // busy: not started
        if (stopping_.load()) {
            return;
        }
        std::this_thread::sleep_for(options_.busy_retry);
    }
}

}  // namespace cml::core
