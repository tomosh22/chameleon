#include "cml/watch/watch.h"

#include <algorithm>
#include <set>

namespace cml::watch {

const char* library_name() noexcept {
    return "cml_watch";
}

std::string watch_key(const std::string& path) {
    std::string_view view(path);
    constexpr std::string_view kExtended = "\\\\?\\";
    if (view.size() > kExtended.size() && view.substr(0, kExtended.size()) == kExtended) {
        view.remove_prefix(kExtended.size());
    }
    std::string key;
    key.reserve(view.size());
    for (const char c : view) {
        if (c == '/') {
            key.push_back('\\');
        } else if (c >= 'A' && c <= 'Z') {
            key.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            key.push_back(c);
        }
    }
    return key;
}

// ---- ChangeCoalescer ---------------------------------------------------------------------------------------

void ChangeCoalescer::add(const FsEvent& event, Clock::time_point now) {
    if (!has_events_) {
        has_events_ = true;
        first_ = now;
    }
    last_ = now;
    if (event.action == FsAction::Overflow) {
        overflow_ = true;
        return;
    }
    const bool present = event.action == FsAction::Added || event.action == FsAction::Modified || event.action == FsAction::RenamedTo;
    final_present_[watch_key(event.path)] = present;
}

ChangeCoalescer::Clock::time_point ChangeCoalescer::ready_at() const {
    return std::min(last_ + options_.quiet, first_ + options_.max_wait);
}

bool ChangeCoalescer::take(Clock::time_point now, ChangeBatch* out) {
    if (!has_events_ || now < ready_at()) {
        return false;
    }
    *out = flush();
    return true;
}

ChangeBatch ChangeCoalescer::flush() {
    ChangeBatch batch;
    batch.overflow = overflow_;
    for (const auto& entry : final_present_) {  // std::map: sorted
        (entry.second ? batch.present : batch.gone).push_back(entry.first);
    }
    final_present_.clear();
    has_events_ = false;
    overflow_ = false;
    return batch;
}

// ---- plan_watch_roots ---------------------------------------------------------------------------------------

namespace {

std::size_t depth_of(const std::string& dir) {
    if (dir.empty()) {
        return 0;
    }
    return static_cast<std::size_t>(std::count(dir.begin(), dir.end(), '\\')) + 1;
}

std::string parent_of(const std::string& dir) {
    const std::size_t slash = dir.find_last_of('\\');
    return slash == std::string::npos ? std::string() : dir.substr(0, slash);
}

bool is_under(const std::string& dir, const std::string& root) {
    return dir.size() > root.size() && dir.compare(0, root.size(), root) == 0 && dir[root.size()] == '\\';
}

}  // namespace

std::vector<WatchRoot> plan_watch_roots(const std::vector<std::string>& files, std::size_t max_roots, std::size_t min_depth) {
    std::map<std::string, bool> roots;  // directory -> recursive
    for (const std::string& file : files) {
        const std::string dir = parent_of(watch_key(file));
        if (!dir.empty()) {
            roots.emplace(dir, false);
        }
    }
    while (roots.size() > max_roots) {
        std::size_t deepest = 0;
        for (const auto& r : roots) {
            deepest = std::max(deepest, depth_of(r.first));
        }
        if (deepest <= min_depth) {
            break;  // cannot be reduced further without watching a drive root
        }
        std::map<std::string, bool> next;
        for (const auto& r : roots) {
            if (depth_of(r.first) == deepest) {
                next[parent_of(r.first)] = true;
            } else {
                next[r.first] = next[r.first] || r.second;
            }
        }
        // A directory below a recursive root is covered by it.
        std::map<std::string, bool> pruned;
        for (const auto& r : next) {
            bool covered = false;
            for (const auto& other : next) {
                if (other.second && is_under(r.first, other.first)) {
                    covered = true;
                    break;
                }
            }
            if (!covered) {
                pruned.emplace(r.first, r.second);
            }
        }
        roots = std::move(pruned);
    }
    std::vector<WatchRoot> out;
    out.reserve(roots.size());
    for (const auto& r : roots) {
        out.push_back(WatchRoot{r.first, r.second});
    }
    return out;
}

// ---- ChangeDispatcher -----------------------------------------------------------------------------------------

ChangeDispatcher::ChangeDispatcher(CoalesceOptions options, std::function<void(const ChangeBatch&)> on_batch)
    : on_batch_(std::move(on_batch)), coalescer_(options) {
    thread_ = std::thread([this] { run(); });
}

ChangeDispatcher::~ChangeDispatcher() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void ChangeDispatcher::push(const FsEvent& event) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        coalescer_.add(event, ChangeCoalescer::Clock::now());
    }
    cv_.notify_all();
}

void ChangeDispatcher::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
        if (!coalescer_.pending()) {
            cv_.wait(lock);
            continue;
        }
        ChangeBatch batch;
        if (coalescer_.take(ChangeCoalescer::Clock::now(), &batch)) {
            lock.unlock();
            on_batch_(batch);
            lock.lock();
            continue;
        }
        cv_.wait_until(lock, coalescer_.ready_at());
    }
}

}  // namespace cml::watch
