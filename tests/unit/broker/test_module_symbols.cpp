// FileProcessModules (WP-12, review item 4): the exports of every other loaded module and the IAT slots of the
// patched one, read from the module files on disk and turned into addresses with the reported load bases.
#include "cml/core/module_symbols.h"

#include <fstream>
#include <memory>

#include "../pdb/test_util.h"
#include "../pe/pe_builder.h"
#include "cml/util/log.h"
#include "doctest.h"

using namespace cml;
using namespace cml::core;

namespace {

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

// The synthetic PE (exports alpha at RVA 0x1000 and a forwarder beta; imports KERNEL32!GetTickCount64 with its
// IAT slot at RVA 0x2140) written under `name`.
std::string write_pe(const testpdb::TempDir& dir, const std::string& name) {
    const cml_test::PeBytes bytes = cml_test::build_pe_on_disk();
    const std::string path = dir.file(name);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

}  // namespace

TEST_CASE("exports of other loaded modules become addresses at their load base") {
    QuietLogs quiet;
    testpdb::TempDir dir;
    const std::string exe = write_pe(dir, "app.exe");
    const std::string dll = write_pe(dir, "ntdll_like.dll");
    FileProcessModules modules({LoadedModule{exe, 0x140000000ull, 0x4000}, LoadedModule{dll, 0x7FF800000000ull, 0x4000}}, LoadedModule{exe, 0x140000000ull, 0x4000});
    const auto alpha = modules.find_export("alpha");
    REQUIRE(alpha.has_value());
    CHECK(alpha->va == 0x7FF800000000ull + 0x1000);
    CHECK(alpha->is_function);  // the RVA lies in the executable .text
    CHECK(alpha->origin == dll);
    CHECK_FALSE(modules.find_export("beta").has_value());      // a forwarder is not code of that module
    CHECK_FALSE(modules.find_export("gamma").has_value());     // not exported anywhere
    CHECK_FALSE(modules.find_export("").has_value());
}

TEST_CASE("the patched module's own exports are not offered") {
    QuietLogs quiet;
    testpdb::TempDir dir;
    const std::string exe = write_pe(dir, "app.exe");
    FileProcessModules modules({LoadedModule{exe, 0x140000000ull, 0x4000}}, LoadedModule{exe, 0x140000000ull, 0x4000});
    CHECK_FALSE(modules.find_export("alpha").has_value());
}

TEST_CASE("IAT slots of the patched module") {
    QuietLogs quiet;
    testpdb::TempDir dir;
    const std::string exe = write_pe(dir, "app.exe");
    FileProcessModules modules({LoadedModule{exe, 0x140000000ull, 0x4000}}, LoadedModule{exe, 0x140000000ull, 0x4000});
    const auto slot = modules.iat_slot("app.exe", "GetTickCount64");
    REQUIRE(slot.has_value());
    CHECK(slot->va == 0x140000000ull + cml_test::pe_layout::kIat);
    CHECK_FALSE(slot->is_function);  // the slot is data
    CHECK_FALSE(modules.iat_slot("app.exe", "NoSuchImport").has_value());
}

TEST_CASE("an unreadable module is skipped with a diagnostic, not an error") {
    QuietLogs quiet;
    testpdb::TempDir dir;
    const std::string exe = write_pe(dir, "app.exe");
    const std::string dll = write_pe(dir, "good.dll");
    FileProcessModules modules({LoadedModule{dir.file("missing.dll"), 0x1000000, 0x1000}, LoadedModule{dll, 0x7FF800000000ull, 0x4000}},
                               LoadedModule{exe, 0x140000000ull, 0x4000});
    CHECK(modules.find_export("alpha").has_value());  // found in the second module
    const std::vector<Diag> skipped = modules.skipped();
    REQUIRE(skipped.size() == 1);
    CHECK(skipped[0].id() == "CML5313");
    CHECK(skipped[0].message.find("missing.dll") != std::string::npos);
}

TEST_CASE("the export cache parses a file once and again when it changed") {
    QuietLogs quiet;
    testpdb::TempDir dir;
    const std::string dll = write_pe(dir, "a.dll");
    ExportCache cache;
    auto first = cache.get(dll);
    auto second = cache.get(dll);
    CHECK(first == second);
    CHECK(cache.parses() == 1);
    CHECK(first->ok);
    CHECK(first->by_name.count("alpha") == 1);
    // A different size is a different file.
    {
        std::ofstream out(dll, std::ios::binary | std::ios::app);
        out << "padding";
    }
    auto third = cache.get(dll);
    CHECK(cache.parses() == 2);
    CHECK(third != first);
    // An unreadable file is cached as failed (and reported by the diagnostic, not a crash).
    auto missing = cache.get(dir.file("nothing.dll"));
    CHECK_FALSE(missing->ok);
    CHECK(missing->error.code != 0);
}
