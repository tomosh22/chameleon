// The real directory watcher (ReadDirectoryChangesW): runs on Windows in CI; elsewhere it must refuse cleanly.
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "cml/watch/watch.h"
#include "cml/watch/watch_diag_ids.h"
#include "doctest.h"

using namespace cml::watch;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

#if !defined(_WIN32)

TEST_CASE("DirectoryWatcher refuses with CML5502 off Windows") {
    std::vector<cml::Diag> skipped;
    auto r = DirectoryWatcher::start({WatchRoot{"/tmp", false}}, [](const FsEvent&) {}, &skipped);
    REQUIRE(!r);
    CHECK(r.error().code == cml::watch::diag::kUnsupported);
}

#else

namespace {

struct TempTree {
    fs::path root;
    TempTree() {
        root = fs::temp_directory_path() / ("cml_watch_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007ll));
        fs::create_directories(root);
        root = fs::canonical(root);
    }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

void write_text(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

// A watcher feeding a dispatcher, collecting batches.
struct Rig {
    std::mutex m;
    std::condition_variable cv;
    std::vector<ChangeBatch> batches;
    std::unique_ptr<ChangeDispatcher> dispatcher;
    std::unique_ptr<DirectoryWatcher> watcher;

    explicit Rig(const std::vector<WatchRoot>& roots) {
        CoalesceOptions options;
        options.quiet = 150ms;
        dispatcher = std::make_unique<ChangeDispatcher>(options, [this](const ChangeBatch& b) {
            std::lock_guard<std::mutex> lock(m);
            batches.push_back(b);
            cv.notify_all();
        });
        std::vector<cml::Diag> skipped;
        auto started = DirectoryWatcher::start(roots, [this](const FsEvent& e) { dispatcher->push(e); }, &skipped);
        REQUIRE_MESSAGE(started, started.error().format());
        watcher = std::move(*started);
    }
    ~Rig() {
        watcher.reset();
        dispatcher.reset();
    }
    bool wait_for_batches(std::size_t n, std::chrono::seconds limit = 10s) {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, limit, [&] { return batches.size() >= n; });
    }
    bool saw(const std::string& key) {
        std::lock_guard<std::mutex> lock(m);
        for (const ChangeBatch& b : batches) {
            for (const std::string& p : b.present) {
                if (p == key) return true;
            }
        }
        return false;
    }
};

}  // namespace

TEST_CASE("DirectoryWatcher reports a write and the debounced batch holds the file once") {
    TempTree tree;
    write_text(tree.root / "a.cpp", "int a;\n");
    Rig rig({WatchRoot{watch_key(tree.root.string()), false}});
    write_text(tree.root / "a.cpp", "int a = 1;\n");
    write_text(tree.root / "a.cpp", "int a = 2;\n");
    REQUIRE(rig.wait_for_batches(1));
    CHECK(rig.saw(watch_key((tree.root / "a.cpp").string())));
}

TEST_CASE("DirectoryWatcher: a safe save (temporary file, rename the original away, rename into place) ends with the file present") {
    TempTree tree;
    const fs::path file = tree.root / "main.cpp";
    write_text(file, "old\n");
    Rig rig({WatchRoot{watch_key(tree.root.string()), false}});
    const fs::path tmp = tree.root / "main.cpp~RF1.TMP";
    const fs::path old = tree.root / "main.cpp~RF2.TMP";
    write_text(tmp, "new\n");
    fs::rename(file, old);
    fs::rename(tmp, file);
    fs::remove(old);
    REQUIRE(rig.wait_for_batches(1));
    CHECK(rig.saw(watch_key(file.string())));
    CHECK(!rig.saw(watch_key(tmp.string())));
    CHECK(!rig.saw(watch_key(old.string())));
}

TEST_CASE("DirectoryWatcher: a recursive root sees files in subdirectories") {
    TempTree tree;
    fs::create_directories(tree.root / "sub" / "deeper");
    Rig rig({WatchRoot{watch_key(tree.root.string()), true}});
    write_text(tree.root / "sub" / "deeper" / "x.h", "#pragma once\n");
    REQUIRE(rig.wait_for_batches(1));
    CHECK(rig.saw(watch_key((tree.root / "sub" / "deeper" / "x.h").string())));
}

TEST_CASE("DirectoryWatcher: a missing root is skipped with CML5500, all missing fails with CML5503") {
    TempTree tree;
    std::vector<cml::Diag> skipped;
    auto r = DirectoryWatcher::start({WatchRoot{watch_key((tree.root / "nope").string()), false}, WatchRoot{watch_key(tree.root.string()), false}},
                                     [](const FsEvent&) {}, &skipped);
    REQUIRE(r);
    REQUIRE(skipped.size() == 1);
    CHECK(skipped[0].code == cml::watch::diag::kRootNotWatched);
    r = DirectoryWatcher::start({WatchRoot{watch_key((tree.root / "nope").string()), false}}, [](const FsEvent&) {}, &skipped);
    REQUIRE(!r);
    CHECK(r.error().code == cml::watch::diag::kWatchFailed);
}

TEST_CASE("DirectoryWatcher stops promptly while idle") {
    TempTree tree;
    const auto t0 = std::chrono::steady_clock::now();
    {
        Rig rig({WatchRoot{watch_key(tree.root.string()), false}});
    }
    CHECK(std::chrono::steady_clock::now() - t0 < 5s);
}

#endif
