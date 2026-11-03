// Debounce, coalescing and the atomic-save patterns of editors (WP-14, plan 4.14). Portable: the event sequences
// below are what ReadDirectoryChangesW reports for each editor's save; the coalescer is fed them with synthetic time.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cml/watch/watch.h"
#include "doctest.h"

using namespace cml::watch;
using Clock = ChangeCoalescer::Clock;
using namespace std::chrono_literals;

namespace {

const std::string kDir = "C:\\Proj\\src\\";

FsEvent ev(FsAction action, const std::string& name) {
    return FsEvent{action, kDir + name};
}

// Feeds `events` one millisecond apart and returns the batch released after the quiet period.
ChangeBatch coalesce(const std::vector<FsEvent>& events) {
    ChangeCoalescer c;
    const Clock::time_point t0 = Clock::now();
    Clock::time_point t = t0;
    for (const FsEvent& e : events) {
        c.add(e, t);
        t += 1ms;
    }
    ChangeBatch batch;
    CHECK(!c.take(t + 100ms, &batch));  // not before 150 ms of quiet
    REQUIRE(c.take(t + 200ms, &batch));
    return batch;
}

bool has(const std::vector<std::string>& v, const std::string& name) {
    return std::find(v.begin(), v.end(), watch_key(kDir + name)) != v.end();
}

}  // namespace

TEST_CASE("watch_key folds case, separators and the extended prefix") {
    CHECK(watch_key("C:/Proj/Src/A.CPP") == "c:\\proj\\src\\a.cpp");
    CHECK(watch_key("\\\\?\\C:\\Proj\\A.h") == "c:\\proj\\a.h");
}

TEST_CASE("debounce: the batch is ready 150 ms after the last event") {
    ChangeCoalescer c;
    const Clock::time_point t0 = Clock::now();
    ChangeBatch batch;
    CHECK(!c.pending());
    CHECK(!c.take(t0, &batch));
    c.add(ev(FsAction::Modified, "a.cpp"), t0);
    CHECK(c.pending());
    CHECK(!c.take(t0 + 149ms, &batch));
    c.add(ev(FsAction::Modified, "a.cpp"), t0 + 100ms);  // a new event restarts the quiet period
    CHECK(!c.take(t0 + 249ms, &batch));
    REQUIRE(c.take(t0 + 250ms, &batch));
    CHECK(batch.present.size() == 1);
    CHECK(!c.pending());
    CHECK(!c.take(t0 + 10s, &batch));  // consumed
}

TEST_CASE("debounce: a steady stream of events is cut off after max_wait") {
    CoalesceOptions options;
    options.quiet = 150ms;
    options.max_wait = 1000ms;
    ChangeCoalescer c(options);
    const Clock::time_point t0 = Clock::now();
    ChangeBatch batch;
    for (int i = 0; i < 20; ++i) {
        c.add(ev(FsAction::Modified, "a.cpp"), t0 + std::chrono::milliseconds(i * 100));
    }
    CHECK(c.ready_at() == t0 + 1000ms);
    CHECK(!c.take(t0 + 999ms, &batch));
    CHECK(c.take(t0 + 1000ms, &batch));
}

TEST_CASE("coalescing: many writes of one file are one path") {
    std::vector<FsEvent> events;
    for (int i = 0; i < 50; ++i) {
        events.push_back(ev(FsAction::Modified, i % 2 ? "Main.cpp" : "main.CPP"));
    }
    const ChangeBatch batch = coalesce(events);
    REQUIRE(batch.present.size() == 1);
    CHECK(batch.present[0] == "c:\\proj\\src\\main.cpp");
    CHECK(batch.gone.empty());
    CHECK(!batch.overflow);
}

TEST_CASE("atomic save: Visual Studio (temporary file, rename the original away, rename into place, delete)") {
    const ChangeBatch b = coalesce({
        ev(FsAction::Added, "main.cpp~RF1a2b.TMP"),
        ev(FsAction::Modified, "main.cpp~RF1a2b.TMP"),
        ev(FsAction::RenamedFrom, "main.cpp"),
        ev(FsAction::RenamedTo, "main.cpp~RF1a2c.TMP"),
        ev(FsAction::RenamedFrom, "main.cpp~RF1a2b.TMP"),
        ev(FsAction::RenamedTo, "main.cpp"),
        ev(FsAction::Removed, "main.cpp~RF1a2c.TMP"),
    });
    CHECK(b.present == std::vector<std::string>{watch_key(kDir + "main.cpp")});
    CHECK(has(b.gone, "main.cpp~RF1a2b.TMP"));
    CHECK(has(b.gone, "main.cpp~RF1a2c.TMP"));
}

TEST_CASE("atomic save: Visual Studio Code (in-place write, sometimes a truncate first)") {
    const ChangeBatch b = coalesce({
        ev(FsAction::Modified, "main.cpp"),  // truncate
        ev(FsAction::Modified, "main.cpp"),  // content
        ev(FsAction::Modified, "main.cpp"),  // attributes / size
    });
    CHECK(b.present == std::vector<std::string>{watch_key(kDir + "main.cpp")});
    CHECK(b.gone.empty());
}

TEST_CASE("atomic save: Rider / IntelliJ safe write (___jb_tmp___ and ___jb_old___)") {
    const ChangeBatch b = coalesce({
        ev(FsAction::Added, "main.cpp___jb_tmp___"),
        ev(FsAction::Modified, "main.cpp___jb_tmp___"),
        ev(FsAction::RenamedFrom, "main.cpp"),
        ev(FsAction::RenamedTo, "main.cpp___jb_old___"),
        ev(FsAction::RenamedFrom, "main.cpp___jb_tmp___"),
        ev(FsAction::RenamedTo, "main.cpp"),
        ev(FsAction::Removed, "main.cpp___jb_old___"),
    });
    CHECK(b.present == std::vector<std::string>{watch_key(kDir + "main.cpp")});
    CHECK(has(b.gone, "main.cpp___jb_tmp___"));
    CHECK(has(b.gone, "main.cpp___jb_old___"));
}

TEST_CASE("atomic save: vim with a backup (probe file 4913, rename to ~, write a new file, remove the backup)") {
    const ChangeBatch b = coalesce({
        ev(FsAction::Added, "4913"),
        ev(FsAction::Removed, "4913"),
        ev(FsAction::RenamedFrom, "main.cpp"),
        ev(FsAction::RenamedTo, "main.cpp~"),
        ev(FsAction::Added, "main.cpp"),
        ev(FsAction::Modified, "main.cpp"),
        ev(FsAction::Removed, "main.cpp~"),
    });
    CHECK(b.present == std::vector<std::string>{watch_key(kDir + "main.cpp")});
    CHECK(has(b.gone, "4913"));
    CHECK(has(b.gone, "main.cpp~"));
}

TEST_CASE("atomic save: vim writing in place (nobackup, nowritebackup)") {
    const ChangeBatch b = coalesce({
        ev(FsAction::Added, "4913"),
        ev(FsAction::Removed, "4913"),
        ev(FsAction::Modified, "main.cpp"),
        ev(FsAction::Modified, "main.cpp"),
    });
    CHECK(b.present == std::vector<std::string>{watch_key(kDir + "main.cpp")});
}

TEST_CASE("a file that is deleted and not recreated ends up gone") {
    const ChangeBatch b = coalesce({
        ev(FsAction::Modified, "old.cpp"),
        ev(FsAction::Removed, "old.cpp"),
    });
    CHECK(b.present.empty());
    CHECK(has(b.gone, "old.cpp"));
}

TEST_CASE("two files saved together (save all) are both reported, sorted") {
    const ChangeBatch b = coalesce({
        ev(FsAction::Modified, "b.cpp"),
        ev(FsAction::Modified, "a.h"),
    });
    REQUIRE(b.present.size() == 2);
    CHECK(b.present[0] == watch_key(kDir + "a.h"));
    CHECK(b.present[1] == watch_key(kDir + "b.cpp"));
}

TEST_CASE("an overflow is reported and clears with the batch") {
    ChangeCoalescer c;
    const Clock::time_point t0 = Clock::now();
    c.add(FsEvent{FsAction::Overflow, {}}, t0);
    ChangeBatch batch;
    REQUIRE(c.take(t0 + 200ms, &batch));
    CHECK(batch.overflow);
    CHECK(batch.present.empty());
    c.add(ev(FsAction::Modified, "a.cpp"), t0 + 300ms);
    batch = c.flush();
    CHECK(!batch.overflow);
}

TEST_CASE("plan_watch_roots: the union of the directories of the files") {
    const std::vector<WatchRoot> roots = plan_watch_roots({
        "C:\\p\\src\\a.cpp",
        "c:\\P\\src\\b.cpp",
        "C:\\p\\include\\a.h",
        "D:\\lib\\x.h",
    });
    REQUIRE(roots.size() == 3);
    CHECK(roots[0].directory == "c:\\p\\include");
    CHECK(roots[1].directory == "c:\\p\\src");
    CHECK(roots[2].directory == "d:\\lib");
    for (const WatchRoot& r : roots) {
        CHECK(!r.recursive);
    }
}

TEST_CASE("plan_watch_roots: too many directories collapse to recursive parents") {
    std::vector<std::string> files;
    for (int i = 0; i < 100; ++i) {
        files.push_back("C:\\big\\mod" + std::to_string(i % 4) + "\\sub" + std::to_string(i) + "\\f.cpp");
    }
    files.push_back("C:\\big\\mod0\\top.cpp");
    const std::vector<WatchRoot> roots = plan_watch_roots(files, 10);
    REQUIRE(roots.size() == 4);
    for (const WatchRoot& r : roots) {
        CHECK(r.recursive);
        CHECK(r.directory.rfind("c:\\big\\mod", 0) == 0);
    }
}

TEST_CASE("plan_watch_roots: never climbs above min_depth") {
    std::vector<std::string> files;
    for (int i = 0; i < 30; ++i) {
        files.push_back("C:\\d" + std::to_string(i) + "\\f.cpp");
    }
    const std::vector<WatchRoot> roots = plan_watch_roots(files, 5);
    CHECK(roots.size() == 30);  // c:\dN are depth 2 already: kept, the drive root is never watched
    for (const WatchRoot& r : roots) {
        CHECK(!r.recursive);
    }
}

TEST_CASE("ChangeDispatcher delivers one batch per burst on its own thread") {
    std::mutex m;
    std::condition_variable cv;
    std::vector<ChangeBatch> batches;
    CoalesceOptions options;
    options.quiet = 40ms;
    options.max_wait = 2000ms;
    ChangeDispatcher d(options, [&](const ChangeBatch& b) {
        std::lock_guard<std::mutex> lock(m);
        batches.push_back(b);
        cv.notify_all();
    });
    for (int i = 0; i < 10; ++i) {
        d.push(ev(FsAction::Modified, "a.cpp"));
        std::this_thread::sleep_for(5ms);
    }
    {
        std::unique_lock<std::mutex> lock(m);
        REQUIRE(cv.wait_for(lock, 10s, [&] { return !batches.empty(); }));
    }
    d.push(ev(FsAction::Modified, "b.cpp"));
    {
        std::unique_lock<std::mutex> lock(m);
        REQUIRE(cv.wait_for(lock, 10s, [&] { return batches.size() >= 2; }));
        CHECK(batches[0].present == std::vector<std::string>{watch_key(kDir + "a.cpp")});
        CHECK(batches[1].present == std::vector<std::string>{watch_key(kDir + "b.cpp")});
    }
}
