// WP-26b (ADR-0036 decision 1): static-library members are reloadable. Synthetic module with a plain object, archive members with and
// without a recorded build environment, an import stub and a CRT member: which compilands count as source compilands, the
// enable-time warning (CML5920), which files are tracked, and the not-owned changed file (CML5921, the sweep side).
#if defined(_WIN32)

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

#include "../pdb/pdb_builder.h"
#include "../pdb/test_util.h"
#include "cml/core/module_index.h"
#include "cml/core/static_lib_diag_ids.h"
#include "cml/util/log.h"
#include "doctest.h"

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
}

#endif
