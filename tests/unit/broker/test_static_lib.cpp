// WP-26b (ADR-0036 decision 1): static-library members are reloadable. Synthetic module with a plain object, archive members with and
// without a recorded build environment, an import stub and a CRT member: which compilands count as source compilands, the
// enable-time warning (CML5920), which files are tracked, and the not-owned changed file (CML5921, the sweep side).
#if defined(_WIN32)

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>

#include "../pdb/pdb_builder.h"
#include "../pdb/test_util.h"
#include "cml/core/module_index.h"
#include "cml/core/static_lib_diag_ids.h"
#include "cml/util/log.h"
#include "doctest.h"

#include <windows.h>

using namespace cml;
using namespace cml::core;
namespace tp = cml::testpdb;
namespace fs = std::filesystem;

namespace {

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

const std::array<std::uint8_t, 16> kGuid = {0x3C, 0x83, 0xC3, 0x8C, 0x00, 0x87, 0x0D, 0xEB, 0x4C, 0x4C, 0x44, 0x20, 0x50, 0x44, 0x42, 0x2E};

void write_text(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

void bump_mtime(const std::string& path) {
    std::error_code ec;
    fs::last_write_time(fs::path(path), fs::last_write_time(fs::path(path), ec) + std::chrono::seconds(2), ec);
}

// Compiland indices of the module below.
constexpr std::uint32_t kPlain = 0, kLibGood = 1, kLibNoEnv = 2, kLibMissing = 3, kImport = 4, kCrt = 5;

struct World {
    tp::TempDir dir;
    tp::Spec spec;
    std::string exe;

    World() {
        write_text(dir.file("app.cpp"), "int app() { return 1; }\n");
        write_text(dir.file("good.cpp"), "int good() { return 1; }\n");
        write_text(dir.file("noenv.cpp"), "int noenv() { return 1; }\n");
        spec.guid = kGuid;
        spec.age = 1;
        spec.sections = {{".text", 0x1000, 0x1000, 0x1000, 0x60000020}, {".data", 0x800, 0x2000, 0x800, 0xC0000040}};
        add("C:\\proj\\obj\\app.cpp.obj", "", dir.file("app.cpp"), true);
        add("C:\\libs\\obj\\good.cpp.obj", "C:\\libs\\foo.lib", dir.file("good.cpp"), true);
        add("C:\\libs\\obj\\noenv.cpp.obj", "C:\\libs\\foo.lib", dir.file("noenv.cpp"), false);
        add("C:\\libs\\obj\\missing.cpp.obj", "C:\\libs\\foo.lib", dir.file("missing.cpp"), true);  // the source is not on disk
        add("Import:KERNEL32.dll", "C:\\sdk\\kernel32.lib", "", false);
        add("f:\\dd\\vctools\\crt\\vcstartup\\src\\startup\\exe_main.obj", "C:\\VS\\VC\\Tools\\MSVC\\14.44\\lib\\x64\\libcmt.lib", dir.file("noenv.cpp"), false);
        spec.contributions = {{1, 0x000, 0x100, 0}, {1, 0x100, 0x100, 1}};
        spec.symrec.pub32("app_func", 0x3, 1, 0x10);
        write_file_pair();
    }

    void add(const std::string& obj, const std::string& lib, const std::string& src, bool with_env) {
        tp::Module m;
        m.name = obj;
        m.lib = lib;
        m.symbols.objname(obj);
        m.symbols.compile3(1, 0, 0xD0, "Test Compiler 19.44");
        if (with_env) {
            m.symbols.envblock({{"cwd", dir.dir()}, {"cl", "C:\\vc\\bin\\cl.exe"}, {"cmd", "/c /Z7 /Gy " + src}, {"src", src}});
        }
        if (!src.empty()) {
            m.files = {src};
        }
        spec.modules.push_back(std::move(m));
    }
    void write_file_pair() {
        const std::string pdb = dir.file("app.pdb");
        exe = dir.file("app.exe");
        tp::write_file(pdb, tp::build(spec));
        tp::write_file(exe, tp::build_pe(kGuid, 1, pdb));
    }
};

}  // namespace

TEST_CASE("static libraries (WP-26b): the build environment decides, not the archive; skipped members are counted and warned about once") {
    QuietLogs quiet;
    World w;
    std::mutex mutex;
    std::vector<Diag> warnings;
    ModuleIndexSpec spec;
    spec.module_path = w.exe;
    spec.scan_includes = false;
    spec.on_skipped_archive_members = [&](const Diag& d) {
        std::lock_guard<std::mutex> lock(mutex);
        warnings.push_back(d);
    };
    std::shared_ptr<ModuleIndex> index = ModuleIndex::start(spec);
    REQUIRE(index->wait(30000));
    REQUIRE(index->status().state == IndexState::Ready);
    const std::shared_ptr<const pdb::PdbIndex> pdb = index->pdb();
    REQUIRE(pdb != nullptr);

    CHECK(ModuleIndex::is_source_compiland(*pdb, kPlain));
    CHECK(ModuleIndex::is_source_compiland(*pdb, kLibGood));  // an archive member with a build environment (was: never)
    CHECK_FALSE(ModuleIndex::is_source_compiland(*pdb, kLibNoEnv));
    CHECK_FALSE(ModuleIndex::is_source_compiland(*pdb, kImport));
    CHECK_FALSE(ModuleIndex::is_source_compiland(*pdb, kCrt));
    CHECK(ModuleIndex::is_object_compiland(*pdb, kLibNoEnv));
    CHECK_FALSE(ModuleIndex::is_object_compiland(*pdb, kImport));
    (void)kLibMissing;

    // Counted: the member without an environment and the member whose source is gone. Not counted: the import stub, the CRT member.
    const std::vector<SkippedCompiland> skipped = index->skipped_compilands();
    REQUIRE(skipped.size() == 2);
    CHECK(skipped[0].compiland == kLibNoEnv);
    CHECK(skipped[0].name == "noenv.cpp.obj");
    CHECK(skipped[1].compiland == kLibMissing);

    // One warning, CML5920, with the number and the names.
    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].code == staticlib::diag::kSkippedArchiveCompilands);
    CHECK(warnings[0].message.find("2 object(s)") != std::string::npos);
    CHECK(warnings[0].message.find("noenv.cpp.obj") != std::string::npos);
    CHECK(warnings[0].message.find("missing.cpp.obj") != std::string::npos);
    CHECK(warnings[0].message.find("kernel32") == std::string::npos);

    // The member with an environment is tracked like any source; the skipped member's own source is tracked too (to report it).
    const std::vector<std::string> tracked = index->tracked_files();
    const auto has = [&](const std::string& f) { return std::find(tracked.begin(), tracked.end(), pdb::normalize_path(f)) != tracked.end(); };
    CHECK(has(w.dir.file("good.cpp")));
    CHECK(has(w.dir.file("noenv.cpp")));
    CHECK_FALSE(has(w.dir.file("missing.cpp")));
}

TEST_CASE("static libraries (WP-26b): a changed archive member is swept to its compiland; a changed file only skipped members own is CML5921, not unmapped") {
    QuietLogs quiet;
    World w;
    ModuleIndexSpec spec;
    spec.module_path = w.exe;
    spec.scan_includes = false;
    std::shared_ptr<ModuleIndex> index = ModuleIndex::start(spec);
    REQUIRE(index->wait(30000));
    REQUIRE(index->status().state == IndexState::Ready);

    write_text(w.dir.file("good.cpp"), "int good() { return 2; }\n");
    bump_mtime(w.dir.file("good.cpp"));
    Result<ModuleIndex::Sweep> first = index->sweep();
    REQUIRE(first.has_value());
    REQUIRE(first->compilands.size() == 1);
    CHECK(first->compilands[0] == kLibGood);
    CHECK(first->unmapped.empty());
    CHECK(first->not_owned.empty());
    index->accept(first->stamps);

    write_text(w.dir.file("noenv.cpp"), "int noenv() { return 2; }\n");
    bump_mtime(w.dir.file("noenv.cpp"));
    Result<ModuleIndex::Sweep> second = index->sweep();
    REQUIRE(second.has_value());
    CHECK(second->compilands.empty());
    CHECK(second->unmapped.empty());
    REQUIRE(second->not_owned.size() == 1);
    CHECK(second->not_owned[0].file == pdb::normalize_path(w.dir.file("noenv.cpp")));
    CHECK(second->not_owned[0].detail.find("noenv.cpp.obj") != std::string::npos);
    CHECK(second->not_owned[0].detail.find("foo.lib") != std::string::npos);
    CHECK(second->not_owned[0].detail.find("no build environment") != std::string::npos);

    // Once accepted, the same edit is not reported again.
    index->accept(second->stamps);
    Result<ModuleIndex::Sweep> third = index->sweep();
    REQUIRE(third.has_value());
    CHECK(third->not_owned.empty());
    CHECK(third->unmapped.empty());
    CHECK(third->changed.empty());
}

// ADR-0036 / WP-26b, review: a plain object without an env block is skipped, but what it lists is still tracked (headers included), and
// a precompiled header it builds is still found. A change to such a header is CML5921 (or CML5550 for the precompiled header), not `patched: 0`.
TEST_CASE("static libraries (WP-26b): a skipped plain object keeps its headers tracked and its precompiled header detected") {
    QuietLogs quiet;
    tp::TempDir dir;
    write_text(dir.file("ok.cpp"), "int ok() { return 1; }\n");
    write_text(dir.file("plain.cpp"), "#include \"plain.h\"\nint plain() { return PLAIN; }\n");
    write_text(dir.file("plain.h"), "#define PLAIN 1\n");
    write_text(dir.file("pchmk.cpp"), "#include \"pch.h\"\n");
    write_text(dir.file("pch.h"), "#pragma once\n");
    tp::Spec spec;
    spec.guid = kGuid;
    spec.age = 1;
    spec.sections = {{".text", 0x1000, 0x1000, 0x1000, 0x60000020}, {".data", 0x800, 0x2000, 0x800, 0xC0000040}};
    const auto add = [&](const std::string& obj, const std::vector<std::string>& files, bool env, const std::string& src, const std::string& cmd) {
        tp::Module m;
        m.name = obj;
        m.symbols.objname(obj);
        m.symbols.compile3(1, 0, 0xD0, "Test Compiler 19.44");
        if (env) {
            std::vector<std::pair<std::string, std::string>> pairs = {{"cwd", dir.dir()}, {"cl", "C:\\vc\\bin\\cl.exe"}, {"cmd", cmd}};
            if (!src.empty()) {
                pairs.emplace_back("src", src);
            }
            m.symbols.envblock(pairs);
        }
        m.files = files;
        spec.modules.push_back(std::move(m));
    };
    add("C:\\obj\\ok.cpp.obj", {dir.file("ok.cpp")}, true, dir.file("ok.cpp"), "/c /Z7 /Gy ok.cpp");
    add("C:\\obj\\plain.cpp.obj", {dir.file("plain.cpp"), dir.file("plain.h")}, false, "", "");  // no env block at all
    add("C:\\obj\\pchmk.cpp.obj", {dir.file("pchmk.cpp")}, true, "", "/c /Z7 /Ycpch.h pchmk.cpp");  // a command but no source recorded
    spec.contributions = {{1, 0x000, 0x100, 0}};
    spec.symrec.pub32("ok_func", 0x3, 1, 0x10);
    const std::string pdb = dir.file("app.pdb");
    tp::write_file(pdb, tp::build(spec));
    const std::string exe = dir.file("app.exe");
    tp::write_file(exe, tp::build_pe(kGuid, 1, pdb));

    ModuleIndexSpec ispec;
    ispec.module_path = exe;
    ispec.scan_includes = false;
    std::shared_ptr<ModuleIndex> index = ModuleIndex::start(ispec);
    REQUIRE(index->wait(30000));
    REQUIRE(index->status().state == IndexState::Ready);
    REQUIRE(index->skipped_compilands().size() == 2);  // plain.cpp.obj and pchmk.cpp.obj; no archive member, so no CML5920

    const std::vector<std::string> tracked = index->tracked_files();
    const auto has = [&](const std::string& f) { return std::find(tracked.begin(), tracked.end(), pdb::normalize_path(f)) != tracked.end(); };
    CHECK(has(dir.file("plain.h")));  // the header only the skipped object lists
    CHECK(has(dir.file("pch.h")));
    CHECK(index->is_pch_header(pdb::normalize_path(dir.file("pch.h"))));  // /Yc found without a recorded source

    write_text(dir.file("plain.h"), "#define PLAIN 2\n");
    bump_mtime(dir.file("plain.h"));
    Result<ModuleIndex::Sweep> swept = index->sweep();
    REQUIRE(swept.has_value());
    CHECK(swept->compilands.empty());
    CHECK(swept->unmapped.empty());
    REQUIRE(swept->not_owned.size() == 1);
    CHECK(swept->not_owned[0].file == pdb::normalize_path(dir.file("plain.h")));
    CHECK(swept->not_owned[0].detail.find("plain.cpp.obj") != std::string::npos);
    index->accept(swept->stamps);

    write_text(dir.file("pch.h"), "#pragma once\n// changed\n");
    bump_mtime(dir.file("pch.h"));
    Result<ModuleIndex::Sweep> pch = index->sweep();
    REQUIRE(pch.has_value());
    REQUIRE(pch->pch_headers.size() == 1);  // the pipeline refuses it with CML5550
    CHECK(pch->pch_headers[0] == pdb::normalize_path(dir.file("pch.h")));
}

TEST_CASE("WP-50: an index of the same module file takes the file lists from the scan cache; a changed module stamp scans again") {
    QuietLogs quiet;
    World w;
    const auto cache = std::make_shared<ScanCache>();
    const auto build = [&]() {
        ModuleIndexSpec spec;
        spec.module_path = w.exe;
        spec.scan_includes = false;
        spec.scan_cache = cache;
        std::shared_ptr<ModuleIndex> index = ModuleIndex::start(spec);
        REQUIRE(index->wait(30000));
        REQUIRE(index->status().state == IndexState::Ready);
        return index;
    };
    const std::shared_ptr<ModuleIndex> first = build();
    CHECK(first->status().scan_cache == "cold");
    CHECK(cache->size() == 1);

    const std::shared_ptr<ModuleIndex> second = build();
    CHECK(second->status().scan_cache == "warm");
    CHECK(second->tracked_files() == first->tracked_files());
    const std::vector<SkippedCompiland> a = first->skipped_compilands();
    const std::vector<SkippedCompiland> b = second->skipped_compilands();
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].compiland == b[i].compiland);
        CHECK(a[i].reason == b[i].reason);
    }
    CHECK(second->status().baseline_files == first->status().baseline_files);  // the sources are hashed afresh either way

    // The module file was relinked (here: only its time changed): scanned again.
    bump_mtime(w.exe);
    const std::shared_ptr<ModuleIndex> third = build();
    CHECK(third->status().scan_cache == "cold");
    CHECK(third->tracked_files() == first->tracked_files());
    const std::shared_ptr<ModuleIndex> fourth = build();
    CHECK(fourth->status().scan_cache == "warm");  // the new stamp replaced the entry

    // Without a cache nothing is reused.
    ModuleIndexSpec plain;
    plain.module_path = w.exe;
    plain.scan_includes = false;
    std::shared_ptr<ModuleIndex> uncached = ModuleIndex::start(plain);
    REQUIRE(uncached->wait(30000));
    CHECK(uncached->status().scan_cache == "off");
}

TEST_CASE("WP-55 (R-58): after an edit only the compilands whose inputs changed are scanned again; a skipped member's source that appears rescans that member") {
    QuietLogs quiet;
    World w;
    const auto cache = std::make_shared<ScanCache>();
    const auto build = [&]() {
        ModuleIndexSpec spec;
        spec.module_path = w.exe;
        spec.scan_includes = true;
        spec.scan_cache = cache;
        std::shared_ptr<ModuleIndex> index = ModuleIndex::start(spec);
        REQUIRE(index->wait(30000));
        REQUIRE(index->status().state == IndexState::Ready);
        return index;
    };
    const std::string header = pdb::normalize_path(w.dir.file("new.h"));
    const auto tracks = [](const std::shared_ptr<ModuleIndex>& index, const std::string& f) {
        const std::vector<std::string> tracked = index->tracked_files();
        return std::find(tracked.begin(), tracked.end(), f) != tracked.end();
    };
    // The object compilands: app, good, noenv, missing and the CRT member (the import stub has no part).
    constexpr std::size_t kParts = 5;
    const std::shared_ptr<ModuleIndex> first = build();
    CHECK(first->status().scan_cache == "cold");
    CHECK(first->status().scan_rescanned == kParts);
    CHECK(first->status().scan_reused == 0);
    CHECK_FALSE(tracks(first, header));
    const std::shared_ptr<ModuleIndex> warm = build();
    CHECK(warm->status().scan_cache == "warm");
    CHECK(warm->status().scan_reused == kParts);
    CHECK(warm->status().scan_rescanned == 0);

    // The source gains an include of a header created for it (as a reload before a hot-restart would have compiled): the module
    // file is the same, app.cpp's part is stale (the bug of WP-50 round 6: the stored graph would miss new.h), the others are not.
    write_text(w.dir.file("new.h"), "#pragma once\nint fresh();\n");
    write_text(w.dir.file("app.cpp"), "#include \"new.h\"\nint app() { return 1; }\n");
    bump_mtime(w.dir.file("app.cpp"));
    const std::shared_ptr<ModuleIndex> edited = build();
    CHECK(edited->status().scan_cache == "partial");
    CHECK(edited->status().scan_rescanned == 1);
    CHECK(edited->status().scan_reused == kParts - 1);
    CHECK(tracks(edited, header));
    CHECK(edited->tracked_files().size() == first->tracked_files().size() + 1);
    const std::shared_ptr<ModuleIndex> again = build();
    CHECK(again->status().scan_cache == "warm");
    CHECK(tracks(again, header));
    // An edit of the new header reaches the compiland that includes it.
    write_text(w.dir.file("new.h"), "#pragma once\nint fresh(int);\n");
    bump_mtime(w.dir.file("new.h"));
    Result<ModuleIndex::Sweep> swept = again->sweep();
    REQUIRE(swept.has_value());
    CHECK(std::find(swept->compilands.begin(), swept->compilands.end(), kPlain) != swept->compilands.end());
    again->accept(swept->stamps);

    const std::shared_ptr<ModuleIndex> header_edited = build();  // new.h changed since the entry was stored: app.cpp's part only
    CHECK(header_edited->status().scan_cache == "partial");
    CHECK(header_edited->status().scan_rescanned == 1);
    const std::shared_ptr<ModuleIndex> settled = build();
    CHECK(settled->status().scan_cache == "warm");

    // The archive member skipped because its source was missing: the source appears, the member is no longer skipped, and only it
    // is scanned again.
    REQUIRE(settled->skipped_compilands().size() == 2);
    write_text(w.dir.file("missing.cpp"), "int missing() { return 1; }\n");
    const std::shared_ptr<ModuleIndex> appeared = build();
    CHECK(appeared->status().scan_cache == "partial");
    CHECK(appeared->status().scan_rescanned == 1);
    REQUIRE(appeared->skipped_compilands().size() == 1);
    CHECK(appeared->skipped_compilands()[0].compiland == kLibNoEnv);
    CHECK(tracks(appeared, header));  // the reused part of app.cpp still includes new.h
    // The merged result is what a full scan gives.
    const auto full = [&]() {
        ModuleIndexSpec spec;
        spec.module_path = w.exe;
        spec.scan_includes = true;
        std::shared_ptr<ModuleIndex> index = ModuleIndex::start(spec);
        REQUIRE(index->wait(30000));
        return index;
    };
    const std::shared_ptr<ModuleIndex> reference = full();
    CHECK(reference->status().scan_cache == "off");
    CHECK(reference->tracked_files() == appeared->tracked_files());
    CHECK(reference->scanned_headers() == appeared->scanned_headers());
    CHECK(build()->status().scan_cache == "warm");
}

namespace {
std::shared_ptr<ModuleIndex> build_index(const World& w, const std::shared_ptr<ScanCache>& cache, std::function<void()> after_scan = nullptr,
                                        std::function<void(int)> before_scan = nullptr) {
    ModuleIndexSpec spec;
    spec.module_path = w.exe;
    spec.scan_includes = true;
    spec.scan_cache = cache;
    spec.after_scan_for_testing = std::move(after_scan);
    spec.before_scan_for_testing = std::move(before_scan);
    std::shared_ptr<ModuleIndex> index = ModuleIndex::start(spec);
    REQUIRE(index->wait(30000));
    REQUIRE(index->status().state == IndexState::Ready);
    return index;
}
bool tracks_file(const std::shared_ptr<ModuleIndex>& index, const std::string& f) {
    const std::vector<std::string> tracked = index->tracked_files();
    return std::find(tracked.begin(), tracked.end(), pdb::normalize_path(f)) != tracked.end();
}
}  // namespace

TEST_CASE("WP-50 (second review): a source edited while the index scans is not cached; the next index scans it again and sees the new include") {
    QuietLogs quiet;
    World w;
    const auto cache = std::make_shared<ScanCache>();
    // Between the scan (which read the old app.cpp) and the store: app.cpp gains #include "new.h".
    const std::shared_ptr<ModuleIndex> first = build_index(w, cache, [&w] {
        write_text(w.dir.file("new.h"), "#pragma once\nint fresh();\n");
        write_text(w.dir.file("app.cpp"), "#include \"new.h\"\nint app() { return 1; }\n");
        bump_mtime(w.dir.file("app.cpp"));
    });
    CHECK(first->status().scan_cache == "cold");
    CHECK_FALSE(tracks_file(first, w.dir.file("new.h")));  // the scan saw the old contents
    CHECK(cache->size() == 1);                             // WP-55: stored, without app.cpp's part

    const std::shared_ptr<ModuleIndex> second = build_index(w, cache);
    CHECK(second->status().scan_cache == "partial");  // app.cpp's part scanned again: nothing stale to hit
    CHECK(second->status().scan_rescanned == 1);
    CHECK(tracks_file(second, w.dir.file("new.h")));
    const std::shared_ptr<ModuleIndex> third = build_index(w, cache);
    CHECK(third->status().scan_cache == "warm");
    CHECK(tracks_file(third, w.dir.file("new.h")));
}

TEST_CASE("WP-55 (review): a reused part disproved by an input that changed after the lookup is scanned again before it reaches the index") {
    QuietLogs quiet;
    World w;
    const auto cache = std::make_shared<ScanCache>();
    // app.cpp and good.cpp both include shared.h.
    write_text(w.dir.file("shared.h"), "#pragma once\n");
    write_text(w.dir.file("app.cpp"), "#include \"shared.h\"\nint app() { return 1; }\n");
    write_text(w.dir.file("good.cpp"), "#include \"shared.h\"\nint good() { return 1; }\n");
    bump_mtime(w.dir.file("app.cpp"));
    bump_mtime(w.dir.file("good.cpp"));
    CHECK(build_index(w, cache)->status().scan_cache == "cold");

    // app.cpp is edited (its part is stale); good.cpp's part is vouched for by the lookup. Between the lookup and the scan,
    // shared.h gains #include "new.h". The scan of app.cpp sees the new shared.h, which disproves good.cpp's part: without the
    // second pass good.cpp's compiland would miss new.h for the session.
    write_text(w.dir.file("app.cpp"), "#include \"shared.h\"\nint app() { return 2; }\n");
    bump_mtime(w.dir.file("app.cpp"));
    std::vector<int> passes;
    const std::shared_ptr<ModuleIndex> index = build_index(w, cache, nullptr, [&](int pass) {
        passes.push_back(pass);
        if (pass == 1) {
            write_text(w.dir.file("new.h"), "#pragma once\nint fresh();\n");
            write_text(w.dir.file("shared.h"), "#pragma once\n#include \"new.h\"\n");
            bump_mtime(w.dir.file("shared.h"));
        }
    });
    CHECK(passes == std::vector<int>{1, 2});
    CHECK(index->status().scan_cache == "partial");
    CHECK(index->status().scan_rescanned == 2);  // app.cpp's, and good.cpp's in the second pass
    CHECK(index->status().scan_reused == 3);
    write_text(w.dir.file("new.h"), "#pragma once\nint fresh(int);\n");
    bump_mtime(w.dir.file("new.h"));
    Result<ModuleIndex::Sweep> swept = index->sweep();
    REQUIRE(swept.has_value());
    CHECK(std::find(swept->compilands.begin(), swept->compilands.end(), kPlain) != swept->compilands.end());
    CHECK(std::find(swept->compilands.begin(), swept->compilands.end(), kLibGood) != swept->compilands.end());  // the bug: absent
    // The second pass's result was stored: the next index (new.h changed since) rescans the two parts that include it.
    const std::shared_ptr<ModuleIndex> next = build_index(w, cache);
    CHECK(next->status().scan_cache == "partial");
    CHECK(next->status().scan_rescanned == 2);
    CHECK(build_index(w, cache)->status().scan_cache == "warm");
}

TEST_CASE("WP-55 (review): when the second pass is disproved too, nothing is reused for that index and nothing is stored") {
    QuietLogs quiet;
    World w;
    const auto cache = std::make_shared<ScanCache>();
    // app.cpp includes shared.h (as good.cpp does) and noenv.cpp, the source the skipped member lists.
    write_text(w.dir.file("shared.h"), "#pragma once\n");
    write_text(w.dir.file("app.cpp"), "#include \"shared.h\"\n#include \"noenv.cpp\"\nint app() { return 1; }\n");
    write_text(w.dir.file("good.cpp"), "#include \"shared.h\"\nint good() { return 1; }\n");
    bump_mtime(w.dir.file("app.cpp"));
    bump_mtime(w.dir.file("good.cpp"));
    CHECK(build_index(w, cache)->status().scan_cache == "cold");

    write_text(w.dir.file("app.cpp"), "#include \"shared.h\"\n#include \"noenv.cpp\"\nint app() { return 2; }\n");
    bump_mtime(w.dir.file("app.cpp"));
    std::vector<int> passes;
    const std::shared_ptr<ModuleIndex> index = build_index(w, cache, nullptr, [&](int pass) {
        passes.push_back(pass);
        if (pass == 1) {  // disproves good.cpp's part
            write_text(w.dir.file("shared.h"), "#pragma once\n// 1\n");
            bump_mtime(w.dir.file("shared.h"));
        } else if (pass == 2) {  // disproves noenv.cpp's part, reused in the second pass
            write_text(w.dir.file("noenv.cpp"), "int noenv() { return 2; }\n");
            bump_mtime(w.dir.file("noenv.cpp"));
        }
    });
    CHECK(passes == std::vector<int>{1, 2, 3});
    CHECK(index->status().scan_cache == "cold");
    CHECK(index->status().scan_reused == 0);
    CHECK(index->status().scan_rescanned == 5);
    // Not stored: the entry of the first index is still the one looked up, so app, good and noenv are stale against it.
    const std::shared_ptr<ModuleIndex> next = build_index(w, cache);
    CHECK(next->status().scan_cache == "partial");
    CHECK(next->status().scan_rescanned == 3);
    CHECK(build_index(w, cache)->status().scan_cache == "warm");
}

TEST_CASE("WP-55 (R-63): a header over the size limit is cached with its stamp; a file locked against reading keeps its compiland out of the cache") {
    QuietLogs quiet;
    World w;
    const auto cache = std::make_shared<ScanCache>();
    // Over the scanner's size limit (8 MiB): never read, the same answer every time while its stamp holds (WP-50: never cached).
    write_text(w.dir.file("big.h"), "#pragma once\n" + std::string(9u * 1024u * 1024u, ' ') + "\n");
    write_text(w.dir.file("app.cpp"), "#include \"big.h\"\nint app() { return 1; }\n");
    bump_mtime(w.dir.file("app.cpp"));
    CHECK(build_index(w, cache)->status().scan_cache == "cold");
    CHECK(cache->size() == 1);
    CHECK(build_index(w, cache)->status().scan_cache == "warm");

    // Under the limit again (and including a header now): its stamp changed, so app.cpp's part is scanned again.
    write_text(w.dir.file("inner.h"), "#pragma once\n");
    write_text(w.dir.file("big.h"), "#pragma once\n#include \"inner.h\"\n");
    bump_mtime(w.dir.file("big.h"));
    const std::shared_ptr<ModuleIndex> readable = build_index(w, cache);
    CHECK(readable->status().scan_cache == "partial");
    CHECK(readable->status().scan_rescanned == 1);
    CHECK(tracks_file(readable, w.dir.file("inner.h")));
    CHECK(build_index(w, cache)->status().scan_cache == "warm");

    // A sharing violation: inner.h is open without read sharing while app.cpp's part is scanned (big.h changes, so that part is
    // stale). The read may succeed next time, so the part is not kept; the other compilands are.
    write_text(w.dir.file("big.h"), "#pragma once\n#include \"inner.h\"\n// edited\n");
    bump_mtime(w.dir.file("big.h"));
    const std::wstring locked_path = fs::path(w.dir.file("inner.h")).wstring();
    HANDLE locked = CreateFileW(locked_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(locked != INVALID_HANDLE_VALUE);
    const std::shared_ptr<ModuleIndex> while_locked = build_index(w, cache);
    CHECK(while_locked->status().scan_cache == "partial");
    CHECK(while_locked->status().scan_rescanned == 1);
    CHECK(cache->size() == 1);
    // Still locked: the part is not reused although nothing it depends on shows a change.
    const std::shared_ptr<ModuleIndex> still = build_index(w, cache);
    CHECK(still->status().scan_cache == "partial");
    CHECK(still->status().scan_rescanned == 1);
    CloseHandle(locked);
    const std::shared_ptr<ModuleIndex> unlocked = build_index(w, cache);
    CHECK(unlocked->status().scan_cache == "partial");
    CHECK(tracks_file(unlocked, w.dir.file("inner.h")));
    CHECK(build_index(w, cache)->status().scan_cache == "warm");
}

#endif
