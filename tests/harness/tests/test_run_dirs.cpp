// cmltest never deletes a directory it did not create (review fix 7).
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "cml/ipc/control_ids.h"
#include "doctest.h"
#include "run_dirs.h"

using namespace cmltest;
namespace fs = std::filesystem;

namespace {

struct Sandbox {
    fs::path root;
    fs::path previous_cwd;
    Sandbox() {
        static int counter = 0;
        std::error_code ec;
        previous_cwd = fs::current_path(ec);
        root = fs::temp_directory_path(ec) / ("cmltest_rundirs_" + std::to_string(counter++) + "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
    }
    ~Sandbox() {
        std::error_code ec;
        fs::current_path(previous_cwd, ec);
        fs::remove_all(root, ec);
    }
    void touch(const fs::path& relative, const std::string& text = "x") const {
        fs::create_directories((root / relative).parent_path());
        std::ofstream out(root / relative, std::ios::binary);
        out << text;
    }
};

}  // namespace

TEST_CASE("run dirs: a fresh marked subdirectory is created below the base") {
    Sandbox box;
    cml::Result<fs::path> dir = create_run_directory(box.root / "out", "basic");
    REQUIRE_MESSAGE(dir.has_value(), (dir.has_value() ? "" : dir.error().format()));
    CHECK(dir->parent_path() == box.root / "out");
    CHECK(dir->filename().string().rfind("cmltest-basic-", 0) == 0);
    CHECK(fs::exists(*dir / kRunMarkerFile));
    // a second call never reuses the directory
    cml::Result<fs::path> again = create_run_directory(box.root / "out", "basic");
    REQUIRE(again.has_value());
    CHECK(*again != *dir);
}

TEST_CASE("run dirs: --artifacts . deletes nothing outside the new subdirectory") {
    Sandbox box;
    box.touch("keep.txt");
    box.touch("sub/precious.dat");
    box.touch("sub/deeper/also.dat");
    std::error_code ec;
    fs::current_path(box.root, ec);
    REQUIRE_FALSE(ec);
    // "." as the artifacts base, exactly like `cmltest --artifacts .`
    cml::Result<fs::path> dir = create_run_directory(fs::absolute(fs::path("."), ec), "basic");
    REQUIRE_MESSAGE(dir.has_value(), (dir.has_value() ? "" : dir.error().format()));
    CHECK(fs::equivalent(dir->parent_path(), box.root, ec));
    // The base itself cannot be removed: it has no marker.
    cml::Result<void> refused = remove_run_directory(box.root);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == cml::ids::kHarnessUsage);
    CHECK(refused.error().message.find("refusing to delete") != std::string::npos);
    CHECK(fs::exists(box.root / "keep.txt"));
    CHECK(fs::exists(box.root / "sub" / "precious.dat"));
    CHECK(fs::exists(box.root / "sub" / "deeper" / "also.dat"));
    // Other directories without the marker are refused as well, relative or not.
    CHECK_FALSE(remove_run_directory(fs::path(".")).has_value());
    CHECK_FALSE(remove_run_directory(box.root / "sub").has_value());
    CHECK(fs::exists(box.root / "sub" / "precious.dat"));
    // Only the directory cmltest created can go.
    CHECK(remove_run_directory(*dir).has_value());
    CHECK_FALSE(fs::exists(*dir));
    CHECK(fs::exists(box.root / "keep.txt"));
    CHECK(fs::exists(box.root / "sub" / "deeper" / "also.dat"));
}

TEST_CASE("run dirs: files, missing paths, markers that are directories and roots are refused") {
    Sandbox box;
    box.touch("file.txt");
    CHECK_FALSE(remove_run_directory(box.root / "file.txt").has_value());
    CHECK_FALSE(remove_run_directory(box.root / "missing").has_value());
    CHECK_FALSE(remove_run_directory(fs::path()).has_value());
    fs::create_directories(box.root / "fake" / kRunMarkerFile);  // a directory named like the marker
    box.touch("fake/data.txt");
    CHECK_FALSE(remove_run_directory(box.root / "fake").has_value());
    CHECK(fs::exists(box.root / "fake" / "data.txt"));
    CHECK_FALSE(remove_run_directory(box.root.root_path()).has_value());
}

TEST_CASE("run dirs: a base that cannot be created is a diagnostic") {
    Sandbox box;
    box.touch("a_file");
    cml::Result<fs::path> dir = create_run_directory(box.root / "a_file" / "sub", "basic");
    REQUIRE_FALSE(dir.has_value());
    CHECK(dir.error().code == cml::ids::kHarnessUsage);
}
