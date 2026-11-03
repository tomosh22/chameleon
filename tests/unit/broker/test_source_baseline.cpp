// The source-hash baseline and the reload sweep (WP-12, plan step 1).
#include "cml/core/source_baseline.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "../pdb/test_util.h"
#include "cml/util/log.h"
#include "doctest.h"

using namespace cml;
using namespace cml::core;
namespace fs = std::filesystem;

namespace {

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

void write_text(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// Makes sure the modification time moves even on file systems with a coarse clock.
void bump_mtime(const std::string& path) {
    std::error_code ec;
    fs::last_write_time(fs::path(path), fs::last_write_time(fs::path(path), ec) + std::chrono::seconds(2), ec);
}

}  // namespace

TEST_CASE("an untouched tree sweeps clean and reads no file") {
    QuietLogs quiet;
    testpdb::TempDir dir;
    const std::string a = dir.file("a.cpp");
    const std::string b = dir.file("b.h");
    write_text(a, "int a;");
    write_text(b, "int b;");
    SourceBaseline baseline;
    baseline.take({a, b}, BaselineOptions{}, nullptr);
    CHECK(baseline.size() == 2);
    const SourceBaseline::SweepResult s = baseline.sweep();
    CHECK(s.checked == 2);
    CHECK(s.hashed == 0);  // size and time unchanged: nothing is read
    CHECK(s.changed.empty());
    CHECK(s.missing.empty());
}

TEST_CASE("a changed file is reported once its content hash differs") {
    QuietLogs quiet;
    testpdb::TempDir dir;
    const std::string a = dir.file("a.cpp");
    write_text(a, "int a = 1;");
    SourceBaseline baseline;
    baseline.take({a}, BaselineOptions{}, nullptr);
    write_text(a, "int a = 2;");  // same size, other content
    bump_mtime(a);
    const SourceBaseline::SweepResult s = baseline.sweep();
    REQUIRE(s.changed.size() == 1);
    CHECK(s.changed[0] == a);
    CHECK(s.hashed == 1);
    REQUIRE(s.stamps.count(a) == 1);

    // Until the patch was applied (accept) the file stays changed: a failed reload is retried.
    CHECK(baseline.sweep().changed.size() == 1);
    baseline.accept(s.stamps);
    CHECK(baseline.sweep().changed.empty());
}

TEST_CASE("touching a file without changing it does not trigger a rebuild") {
    QuietLogs quiet;
    testpdb::TempDir dir;
    const std::string a = dir.file("a.cpp");
    write_text(a, "same");
    SourceBaseline baseline;
    baseline.take({a}, BaselineOptions{}, nullptr);
    bump_mtime(a);
    const SourceBaseline::SweepResult s = baseline.sweep();
    CHECK(s.hashed == 1);  // it had to be read ...
    CHECK(s.changed.empty());  // ... and the hash is the same
}

TEST_CASE("an edit made while the patch was being built is not swallowed by accept") {
    QuietLogs quiet;
    testpdb::TempDir dir;
    const std::string a = dir.file("a.cpp");
    write_text(a, "v1");
    SourceBaseline baseline;
    baseline.take({a}, BaselineOptions{}, nullptr);
    write_text(a, "v2 edit");
    bump_mtime(a);
    const SourceBaseline::SweepResult s = baseline.sweep();
    REQUIRE(s.changed.size() == 1);
    write_text(a, "v3 another edit");  // while the patch for v2 is compiled
    bump_mtime(a);
    bump_mtime(a);
    baseline.accept(s.stamps);         // records v2, not v3
    CHECK(baseline.sweep().changed.size() == 1);
}

TEST_CASE("missing files, unreadable lists and system headers") {
    QuietLogs quiet;
    testpdb::TempDir dir;
    const std::string a = dir.file("a.cpp");
    write_text(a, "x");
    SourceBaseline baseline;
    std::vector<Diag> warnings;
    baseline.take({a, dir.file("generated_and_gone.h")}, BaselineOptions{}, &warnings);
    CHECK(baseline.size() == 1);  // a file that never existed is simply not tracked
    CHECK(warnings.empty());
    fs::remove(fs::path(a));
    const SourceBaseline::SweepResult s = baseline.sweep();
    CHECK(s.changed.empty());
    REQUIRE(s.missing.size() == 1);
    CHECK(s.missing[0] == a);

    CHECK(is_system_path("C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.22621.0\\ucrt\\stdio.h"));
    CHECK(is_system_path("c:/program files/microsoft visual studio/2022/enterprise/vc/tools/msvc/14.44/include/vector"));
    CHECK(is_system_path("C:\\Program Files\\LLVM\\lib\\clang\\20\\include\\stddef.h"));
    CHECK_FALSE(is_system_path("C:\\work\\project\\include\\vector.h"));
    CHECK_FALSE(is_system_path("C:\\work\\windows kits notes.txt"));
}

TEST_CASE("system files are not tracked unless asked for") {
    QuietLogs quiet;
    testpdb::TempDir dir;
    const fs::path sys_dir = fs::path(dir.dir()) / "Windows Kits" / "10";
    std::error_code ec;
    fs::create_directories(sys_dir, ec);
    const std::string sys_file = (sys_dir / "stdio.h").string();
    // is_system_path looks for the backslash form; build the path the way Windows spells it.
    std::string spelled = sys_file;
    for (char& c : spelled) {
        c = c == '/' ? '\\' : c;
    }
    write_text(sys_file, "x");
    SourceBaseline off;
    off.take({sys_file}, BaselineOptions{}, nullptr);
    BaselineOptions on_options;
    on_options.track_system = true;
    SourceBaseline on;
    on.take({sys_file}, on_options, nullptr);
    CHECK(on.size() == 1);
    CHECK((off.size() == 0) == is_system_path(sys_file));  // dropped exactly when the path is a system path
}

TEST_CASE("progress is reported and cancellation stops the baseline") {
    QuietLogs quiet;
    testpdb::TempDir dir;
    std::vector<std::string> files;
    for (int i = 0; i < 200; ++i) {
        files.push_back(dir.file("f" + std::to_string(i) + ".cpp"));
        write_text(files.back(), std::to_string(i));
    }
    std::size_t last_done = 0;
    std::size_t calls = 0;
    BaselineOptions options;
    options.progress = [&](std::size_t done, std::size_t total) {
        CHECK(total == 200);
        CHECK(done >= last_done);
        last_done = done;
        ++calls;
    };
    SourceBaseline full;
    full.take(files, options, nullptr);
    CHECK(full.size() == 200);
    CHECK(last_done == 200);
    CHECK(calls >= 3);

    BaselineOptions cancelling;
    int polled = 0;
    cancelling.cancelled = [&] { return ++polled > 10; };
    SourceBaseline partial;
    partial.take(files, cancelling, nullptr);
    CHECK(partial.size() <= 10);
}
