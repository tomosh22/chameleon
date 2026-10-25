#include "module_diff.h"

#include <string>
#include <vector>

#include "doctest.h"

using cml::agent::diff_modules;
using cml::ipc::ModuleEntry;

namespace {
ModuleEntry module(std::uint64_t base, std::uint64_t size, const char* path) {
    return ModuleEntry{base, size, path};
}
}  // namespace

TEST_CASE("identical module lists have no delta") {
    const std::vector<ModuleEntry> list = {module(0x1000, 0x100, "a.dll"), module(0x2000, 0x100, "b.dll")};
    CHECK(diff_modules(list, list).empty());
    CHECK(diff_modules({}, {}).empty());
}

TEST_CASE("new and removed modules are reported") {
    const std::vector<ModuleEntry> before = {module(0x1000, 0x100, "a.dll"), module(0x2000, 0x100, "b.dll")};
    const std::vector<ModuleEntry> after = {module(0x2000, 0x100, "b.dll"), module(0x3000, 0x100, "c.dll")};
    const auto delta = diff_modules(before, after);
    REQUIRE(delta.loaded.size() == 1);
    REQUIRE(delta.unloaded.size() == 1);
    CHECK(delta.loaded[0].path == "c.dll");
    CHECK(delta.unloaded[0].path == "a.dll");
}

TEST_CASE("everything is new against an empty snapshot") {
    const std::vector<ModuleEntry> after = {module(0x1000, 0x100, "a.dll"), module(0x2000, 0x100, "b.dll")};
    const auto delta = diff_modules({}, after);
    CHECK(delta.loaded.size() == 2);
    CHECK(delta.unloaded.empty());
    const auto gone = diff_modules(after, {});
    CHECK(gone.unloaded.size() == 2);
    CHECK(gone.loaded.empty());
}

TEST_CASE("a different image at the same base counts as unload plus load") {
    const auto delta = diff_modules({module(0x1000, 0x100, "a.dll")}, {module(0x1000, 0x100, "other.dll")});
    REQUIRE(delta.loaded.size() == 1);
    REQUIRE(delta.unloaded.size() == 1);
    CHECK(delta.loaded[0].path == "other.dll");
    CHECK(delta.unloaded[0].path == "a.dll");
    // Same path and base but another size is also a different image.
    const auto resized = diff_modules({module(0x1000, 0x100, "a.dll")}, {module(0x1000, 0x200, "a.dll")});
    CHECK(resized.loaded.size() == 1);
    CHECK(resized.unloaded.size() == 1);
}

TEST_CASE("the same module moving to a new base is unload plus load") {
    const auto delta = diff_modules({module(0x1000, 0x100, "a.dll")}, {module(0x5000, 0x100, "a.dll")});
    CHECK(delta.loaded.size() == 1);
    CHECK(delta.unloaded.size() == 1);
}
