// Compile on save (WP-14): SaveWatcher with a fake directory watcher and fake reloads, plus the content-hash
// confirmation through a real SourceBaseline.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>

#include "../pdb/test_util.h"
#include "cml/core/save_watcher.h"
#include "cml/core/source_baseline.h"
#include "cml/util/log.h"
#include "doctest.h"

using namespace cml;
using namespace cml::core;
using namespace cml::watch;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

class FakeWatcher final : public DirectoryWatcher {};

// Counts factory calls and keeps the roots it was asked for.
struct FakeFactory {
    std::mutex m;
    std::vector<std::vector<WatchRoot>> calls;
    bool fail = false;
    WatcherFactory factory() {
        return [this](const std::vector<WatchRoot>& roots, DirectoryWatcher::Sink, std::vector<Diag>*) -> Result<std::unique_ptr<DirectoryWatcher>> {
            std::lock_guard<std::mutex> lock(m);
            calls.push_back(roots);
            if (fail) {
                return Diag::make(5503, "fake failure", "none");
            }
            return std::unique_ptr<DirectoryWatcher>(std::make_unique<FakeWatcher>());
        };
    }
    std::size_t count() {
        std::lock_guard<std::mutex> lock(m);
        return calls.size();
    }
};

SaveWatcherOptions fast(WatcherFactory factory) {
    SaveWatcherOptions o;
    o.coalesce.quiet = 30ms;
    o.coalesce.max_wait = 500ms;
    o.busy_retry = 5ms;
    o.factory = std::move(factory);
    return o;
}

template <class Pred>
bool wait_until(Pred pred, std::chrono::milliseconds limit = 10s) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

FsEvent modified(const std::string& path) { return FsEvent{FsAction::Modified, path}; }

void write_text(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

void bump_mtime(const std::string& path) {
    std::error_code ec;
    fs::last_write_time(fs::path(path), fs::last_write_time(fs::path(path), ec) + std::chrono::seconds(2), ec);
}

}  // namespace

TEST_CASE("save watcher: a burst of events for a tracked file is one reload after the debounce") {
    QuietLogs quiet;
    FakeFactory fake;
    std::atomic<int> reloads{0};
    SaveWatcherHooks hooks;
    hooks.tracked_files = [] { return std::vector<std::string>{"c:\\p\\src\\main.cpp", "c:\\p\\inc\\a.h"}; };
    hooks.reload = [&] {
        ++reloads;
        return SaveWatcherHooks::ReloadResult::Done;
    };
    SaveWatcher w(hooks, fast(fake.factory()));
    w.rearm();
    REQUIRE(wait_until([&] { return reloads.load() >= 1; }));  // the catch-up sweep after arming
    std::this_thread::sleep_for(100ms);
    const int before = reloads.load();
    for (int i = 0; i < 20; ++i) {
        w.inject(modified("C:\\P\\Src\\Main.cpp"));
    }
    REQUIRE(wait_until([&] { return reloads.load() > before; }));
    std::this_thread::sleep_for(150ms);
    CHECK(reloads.load() == before + 1);
}

TEST_CASE("save watcher: files that are not tracked, and temporaries, start nothing") {
    QuietLogs quiet;
    FakeFactory fake;
    std::atomic<int> reloads{0};
    SaveWatcherHooks hooks;
    hooks.tracked_files = [] { return std::vector<std::string>{"c:\\p\\src\\main.cpp"}; };
    hooks.reload = [&] {
        ++reloads;
        return SaveWatcherHooks::ReloadResult::Done;
    };
    SaveWatcher w(hooks, fast(fake.factory()));
    w.rearm();
    REQUIRE(wait_until([&] { return reloads.load() >= 1; }));
    std::this_thread::sleep_for(100ms);
    const int before = reloads.load();
    w.inject(modified("c:\\p\\src\\notes.txt"));
    w.inject(FsEvent{FsAction::Added, "c:\\p\\src\\main.cpp~RF1.TMP"});
    w.inject(FsEvent{FsAction::Removed, "c:\\p\\src\\main.cpp"});  // deleted: not a save
    std::this_thread::sleep_for(300ms);
    CHECK(reloads.load() == before);
}

TEST_CASE("save watcher: a save that arrives while a reload runs is not lost and runs one follow-up") {
    QuietLogs quiet;
    FakeFactory fake;
    std::mutex m;
    std::condition_variable cv;
    bool release = false;
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};
    SaveWatcherHooks hooks;
    hooks.tracked_files = [] { return std::vector<std::string>{"c:\\p\\main.cpp"}; };
    hooks.reload = [&] {
        const int n = ++started;
        const int c = ++concurrent;
        max_concurrent = std::max(max_concurrent.load(), c);
        if (n == 2) {  // the first one after the catch-up: hold it
            std::unique_lock<std::mutex> lock(m);
            cv.wait_for(lock, 10s, [&] { return release; });
        }
        --concurrent;
        ++finished;
        return SaveWatcherHooks::ReloadResult::Done;
    };
    SaveWatcher w(hooks, fast(fake.factory()));
    w.rearm();
    REQUIRE(wait_until([&] { return finished.load() >= 1; }));  // catch-up
    w.inject(modified("c:\\p\\main.cpp"));
    REQUIRE(wait_until([&] { return started.load() == 2; }));
    // While reload 2 is held: three more saves.
    for (int i = 0; i < 3; ++i) {
        w.inject(modified("c:\\p\\main.cpp"));
        std::this_thread::sleep_for(40ms);
    }
    {
        std::lock_guard<std::mutex> lock(m);
        release = true;
    }
    cv.notify_all();
    REQUIRE(wait_until([&] { return finished.load() >= 3; }));
    std::this_thread::sleep_for(200ms);
    CHECK(started.load() == 3);  // exactly one follow-up, not three
    CHECK(max_concurrent.load() == 1);
}

TEST_CASE("save watcher: a busy reload is retried until it can run") {
    QuietLogs quiet;
    FakeFactory fake;
    std::atomic<int> calls{0};
    std::atomic<int> done{0};
    SaveWatcherHooks hooks;
    hooks.tracked_files = [] { return std::vector<std::string>{"c:\\p\\main.cpp"}; };
    hooks.reload = [&] {
        const int n = ++calls;
        if (n <= 4) {
            return SaveWatcherHooks::ReloadResult::Busy;
        }
        ++done;
        return SaveWatcherHooks::ReloadResult::Done;
    };
    SaveWatcher w(hooks, fast(fake.factory()));
    w.rearm();
    REQUIRE(wait_until([&] { return done.load() >= 1; }));
    CHECK(calls.load() == 5);
    CHECK(w.reloads_started() == 1);  // busy attempts do not count as started
}

TEST_CASE("save watcher: a failed reload is reported and does not stop the watcher") {
    QuietLogs quiet;
    FakeFactory fake;
    std::atomic<int> reloads{0};
    std::mutex m;
    std::vector<std::uint32_t> reported;
    SaveWatcherHooks hooks;
    hooks.tracked_files = [] { return std::vector<std::string>{"c:\\p\\main.cpp"}; };
    hooks.reload = [&] {
        ++reloads;
        return SaveWatcherHooks::ReloadResult::Failed;
    };
    hooks.report = [&](const Diag& d, const char*) {
        std::lock_guard<std::mutex> lock(m);
        reported.push_back(d.code);
    };
    SaveWatcher w(hooks, fast(fake.factory()));
    w.rearm();
    REQUIRE(wait_until([&] { return reloads.load() >= 1; }));
    w.inject(modified("c:\\p\\main.cpp"));
    REQUIRE(wait_until([&] { return reloads.load() >= 2; }));
    std::lock_guard<std::mutex> lock(m);
    REQUIRE(!reported.empty());
    CHECK(reported[0] == 5505);
}

TEST_CASE("save watcher: rearm watches the directories, and only restarts the watcher when they change") {
    QuietLogs quiet;
    FakeFactory fake;
    std::mutex m;
    std::vector<std::string> files = {"c:\\p\\src\\a.cpp", "c:\\p\\inc\\a.h"};
    SaveWatcherHooks hooks;
    hooks.tracked_files = [&] {
        std::lock_guard<std::mutex> lock(m);
        return files;
    };
    hooks.reload = [] { return SaveWatcherHooks::ReloadResult::Done; };
    SaveWatcher w(hooks, fast(fake.factory()));
    w.rearm();
    REQUIRE(fake.count() == 1);
    REQUIRE(w.roots().size() == 2);
    CHECK(w.roots()[0].directory == "c:\\p\\inc");
    CHECK(w.roots()[1].directory == "c:\\p\\src");
    w.rearm();  // same directories
    CHECK(fake.count() == 1);
    {
        std::lock_guard<std::mutex> lock(m);
        files.push_back("c:\\p\\src\\more\\b.cpp");
    }
    w.rearm();
    CHECK(fake.count() == 2);
    CHECK(w.roots().size() == 3);
}

TEST_CASE("save watcher: no tracked files means nothing is watched; a factory failure is reported") {
    QuietLogs quiet;
    FakeFactory fake;
    std::vector<std::uint32_t> reported;
    std::vector<std::string> files;
    SaveWatcherHooks hooks;
    hooks.tracked_files = [&] { return files; };
    hooks.reload = [] { return SaveWatcherHooks::ReloadResult::Done; };
    hooks.report = [&](const Diag& d, const char*) { reported.push_back(d.code); };
    SaveWatcher w(hooks, fast(fake.factory()));
    w.rearm();
    CHECK(fake.count() == 0);
    REQUIRE(reported.size() == 1);
    CHECK(reported[0] == 5504);
    files = {"c:\\p\\a.cpp"};
    fake.fail = true;
    w.rearm();
    REQUIRE(reported.size() == 2);
    CHECK(reported[1] == 5503);
    CHECK(w.roots().empty());
}

TEST_CASE("save watcher: an overflow re-checks everything even when no tracked path was named") {
    QuietLogs quiet;
    FakeFactory fake;
    std::atomic<int> reloads{0};
    std::vector<std::uint32_t> reported;
    std::mutex m;
    SaveWatcherHooks hooks;
    hooks.tracked_files = [] { return std::vector<std::string>{"c:\\p\\a.cpp"}; };
    hooks.reload = [&] {
        ++reloads;
        return SaveWatcherHooks::ReloadResult::Done;
    };
    hooks.report = [&](const Diag& d, const char*) {
        std::lock_guard<std::mutex> lock(m);
        reported.push_back(d.code);
    };
    SaveWatcher w(hooks, fast(fake.factory()));
    w.rearm();
    REQUIRE(wait_until([&] { return reloads.load() >= 1; }));
    std::this_thread::sleep_for(100ms);
    const int before = reloads.load();
    w.inject(FsEvent{FsAction::Overflow, {}});
    REQUIRE(wait_until([&] { return reloads.load() > before; }));
    std::lock_guard<std::mutex> lock(m);
    REQUIRE(!reported.empty());
    CHECK(reported.back() == 5501);
}

// The confirmation by content hash: the reload of a save sweeps the SourceBaseline, so a save that leaves the
// content as it was compiles nothing.
TEST_CASE("save watcher: a save that does not change the content produces no compile") {
    QuietLogs quiet;
    testpdb::TempDir dir;
    const std::string file = dir.file("main.cpp");
    write_text(file, "int f() { return 1; }\n");
    SourceBaseline baseline;
    baseline.take({file}, BaselineOptions{}, nullptr);

    FakeFactory fake;
    std::atomic<int> sweeps{0};
    std::atomic<int> compiles{0};
    SaveWatcherHooks hooks;
    hooks.tracked_files = [&] { return std::vector<std::string>{file}; };
    hooks.reload = [&] {
        ++sweeps;
        const SourceBaseline::SweepResult r = baseline.sweep();
        if (!r.changed.empty()) {
            ++compiles;
            baseline.accept(r.stamps);
        }
        return SaveWatcherHooks::ReloadResult::Done;
    };
    SaveWatcher w(hooks, fast(fake.factory()));
    w.rearm();
    REQUIRE(wait_until([&] { return sweeps.load() >= 1; }));
    CHECK(compiles.load() == 0);

    // Formatter no-op: rewritten with the same bytes, newer time.
    write_text(file, "int f() { return 1; }\n");
    bump_mtime(file);
    w.inject(modified(file));
    REQUIRE(wait_until([&] { return sweeps.load() >= 2; }));
    std::this_thread::sleep_for(100ms);
    CHECK(compiles.load() == 0);

    // A real edit.
    write_text(file, "int f() { return 2; }\n");
    bump_mtime(file);
    w.inject(modified(file));
    REQUIRE(wait_until([&] { return compiles.load() == 1; }));
    // Saved again unchanged afterwards: still one compile.
    write_text(file, "int f() { return 2; }\n");
    bump_mtime(file);
    w.inject(modified(file));
    REQUIRE(wait_until([&] { return sweeps.load() >= 4; }));
    std::this_thread::sleep_for(100ms);
    CHECK(compiles.load() == 1);
}

TEST_CASE("save watcher: destruction while a reload waits to retry returns promptly") {
    QuietLogs quiet;
    FakeFactory fake;
    std::atomic<int> calls{0};
    {
        SaveWatcherHooks hooks;
        hooks.tracked_files = [] { return std::vector<std::string>{"c:\\p\\a.cpp"}; };
        hooks.reload = [&] {
            ++calls;
            return SaveWatcherHooks::ReloadResult::Busy;
        };
        SaveWatcher w(hooks, fast(fake.factory()));
        w.rearm();
        REQUIRE(wait_until([&] { return calls.load() >= 3; }));
    }
    CHECK(calls.load() >= 3);
}
