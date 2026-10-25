// Unified-diff reader and applier: the real fixture edit, CRLF files, offsets, mismatches, bad paths.
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cml/ipc/control_ids.h"
#include "doctest.h"
#include "unified_diff.h"

using namespace cmltest;
namespace fs = std::filesystem;

namespace {

const char* kLogic =
    "#include \"logic.h\"\n"
    "\n"
    "int compute_value()\n"
    "{\n"
    "    if (g_tick == 0) {\n"
    "        fixture_note(\"logic-start\");\n"
    "    }\n"
    "    return 1;\n"
    "}\n";

// The same change as tests/fixtures/basic/edits/value_2.edit (shape, not content: the fixture
// sources have more lines above).
const char* kPatch =
    "--- a/logic.cpp\n"
    "+++ b/logic.cpp\n"
    "@@ -5,4 +5,4 @@\n"
    "     if (g_tick == 0) {\n"
    "         fixture_note(\"logic-start\");\n"
    "     }\n"
    "-    return 1;\n"
    "+    return 2;\n";

std::string apply_ok(const std::string& original, const std::string& patch_text, std::vector<std::string>* notes = nullptr) {
    cml::Result<std::vector<FilePatch>> patches = parse_unified_diff(patch_text);
    REQUIRE_MESSAGE(patches.has_value(), (patches.has_value() ? "" : patches.error().format()));
    REQUIRE(patches->size() == 1);
    cml::Result<std::string> out = apply_file_patch(original, (*patches)[0], notes);
    REQUIRE_MESSAGE(out.has_value(), (out.has_value() ? "" : out.error().format()));
    return *out;
}

std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    for (std::size_t pos = 0; (pos = text.find(from, pos)) != std::string::npos; pos += to.size()) {
        text.replace(pos, from.size(), to);
    }
    return text;
}

struct TempDir {
    fs::path path;
    TempDir() {
        static int counter = 0;
        std::error_code ec;
        path = fs::temp_directory_path(ec) / ("cmltest_ut_" + std::to_string(counter++) + "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void write(const std::string& name, const std::string& content) const {
        fs::create_directories((path / name).parent_path());
        std::ofstream out(path / name, std::ios::binary);
        out << content;
    }
    std::string read(const std::string& name) const {
        std::ifstream in(path / name, std::ios::binary);
        std::ostringstream s;
        s << in.rdbuf();
        return s.str();
    }
};

}  // namespace

TEST_CASE("diff: parse the headers and hunks") {
    cml::Result<std::vector<FilePatch>> p = parse_unified_diff(kPatch);
    REQUIRE(p.has_value());
    REQUIRE(p->size() == 1);
    const FilePatch& f = (*p)[0];
    CHECK(f.old_path == "a/logic.cpp");
    CHECK(f.new_path == "b/logic.cpp");
    REQUIRE(f.hunks.size() == 1);
    CHECK(f.hunks[0].old_start == 5);
    CHECK(f.hunks[0].old_count == 4);
    CHECK(f.hunks[0].new_count == 4);
    REQUIRE(f.hunks[0].lines.size() == 5);
    CHECK(f.hunks[0].lines[3].op == '-');
    CHECK(f.hunks[0].lines[3].text == "    return 1;");
    CHECK(f.hunks[0].lines[4].op == '+');
}

TEST_CASE("diff: git extended headers, timestamps, several files and hunks") {
    const std::string text =
        "diff --git a/x.txt b/x.txt\n"
        "index 83db48f..bf2a4b1 100644\n"
        "--- a/x.txt\t2026-01-01 10:00:00.000000000 +0000\n"
        "+++ b/x.txt\t2026-01-02 10:00:00.000000000 +0000\n"
        "@@ -1,3 +1,3 @@ function header text\n"
        " one\n"
        "-two\n"
        "+TWO\n"
        " three\n"
        "@@ -10 +10,2 @@\n"
        "-ten\n"
        "+ten\n"
        "+eleven\n"
        "diff --git a/y.txt b/y.txt\n"
        "--- a/y.txt\n"
        "+++ b/y.txt\n"
        "@@ -1 +1 @@\n"
        "-a\n"
        "+b\n";
    cml::Result<std::vector<FilePatch>> p = parse_unified_diff(text);
    REQUIRE_MESSAGE(p.has_value(), (p.has_value() ? "" : p.error().format()));
    REQUIRE(p->size() == 2);
    CHECK((*p)[0].old_path == "a/x.txt");
    CHECK((*p)[0].hunks.size() == 2);
    CHECK((*p)[0].hunks[1].old_count == 1);
    CHECK((*p)[0].hunks[1].new_count == 2);
    CHECK((*p)[1].new_path == "b/y.txt");
}

TEST_CASE("diff: apply the value_2 style edit") {
    CHECK(apply_ok(kLogic, kPatch) ==
          std::string(kLogic).replace(std::string(kLogic).find("return 1;"), 9, "return 2;"));
}

TEST_CASE("diff: CRLF files take an LF patch and keep their line ends; added lines get CRLF") {
    const std::string crlf = replace_all(kLogic, "\n", "\r\n");
    const std::string out = apply_ok(crlf, kPatch);
    CHECK(out.find("return 2;\r\n") != std::string::npos);
    CHECK(replace_all(out, "\r\n", "") .find('\n') == std::string::npos);  // no bare LF anywhere
    CHECK(out == replace_all(apply_ok(kLogic, kPatch), "\n", "\r\n"));
    // and a CRLF patch on an LF file
    const std::string crlf_patch = replace_all(kPatch, "\n", "\r\n");
    CHECK(apply_ok(kLogic, crlf_patch) == apply_ok(kLogic, kPatch));
}

TEST_CASE("diff: a hunk at the wrong line number is found nearby and reported as an offset") {
    std::string shifted = "// new line 1\n// new line 2\n// new line 3\n" + std::string(kLogic);
    std::vector<std::string> notes;
    const std::string out = apply_ok(shifted, kPatch, &notes);
    CHECK(out.find("return 2;") != std::string::npos);
    CHECK(out.find("return 1;") == std::string::npos);
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].find("offset +3") != std::string::npos);
    // and upwards
    std::string trimmed = std::string(kLogic).substr(std::string(kLogic).find("int compute_value"));
    std::vector<std::string> up_notes;
    CHECK(apply_ok(trimmed, kPatch, &up_notes).find("return 2;") != std::string::npos);
    REQUIRE(up_notes.size() == 1);
    CHECK(up_notes[0].find("offset -") != std::string::npos);
}

TEST_CASE("diff: context mismatch fails with CML5413 naming the hunk and the differing line") {
    const std::string changed = replace_all(kLogic, "fixture_note(\"logic-start\");", "fixture_note(\"changed\");");
    cml::Result<std::vector<FilePatch>> patches = parse_unified_diff(kPatch);
    REQUIRE(patches.has_value());
    cml::Result<std::string> out = apply_file_patch(changed, (*patches)[0]);
    REQUIRE_FALSE(out.has_value());
    CHECK(out.error().code == cml::ids::kPatchMismatch);
    CHECK(out.error().message.find("hunk 1") != std::string::npos);
    CHECK(out.error().message.find("logic-start") != std::string::npos);
    CHECK_FALSE(out.error().what_to_do.empty());

    const std::string no_return = replace_all(kLogic, "return 1;", "return 3;");
    cml::Result<std::string> removed_differs = apply_file_patch(no_return, (*patches)[0]);
    REQUIRE_FALSE(removed_differs.has_value());
    CHECK(removed_differs.error().code == cml::ids::kPatchMismatch);

    // whitespace matters
    const std::string spaced = replace_all(kLogic, "    return 1;", "  return 1;");
    CHECK_FALSE(apply_file_patch(spaced, (*patches)[0]).has_value());
    // an empty file
    CHECK_FALSE(apply_file_patch("", (*patches)[0]).has_value());
}

TEST_CASE("diff: pure insertion, pure deletion, multiple hunks, creation") {
    const std::string base = "a\nb\nc\nd\ne\nf\ng\nh\n";
    const std::string insert =
        "--- a/f\n+++ b/f\n@@ -2,0 +3,1 @@\n+NEW\n";
    CHECK(apply_ok(base, insert) == "a\nb\nNEW\nc\nd\ne\nf\ng\nh\n");
    const std::string remove = "--- a/f\n+++ b/f\n@@ -3,2 +3,0 @@\n-c\n-d\n";
    CHECK(apply_ok(base, remove) == "a\nb\ne\nf\ng\nh\n");
    const std::string two =
        "--- a/f\n+++ b/f\n"
        "@@ -1,2 +1,3 @@\n a\n+a2\n b\n"
        "@@ -7,2 +8,1 @@\n-g\n h\n";
    CHECK(apply_ok(base, two) == "a\na2\nb\nc\nd\ne\nf\nh\n");
    const std::string create = "--- /dev/null\n+++ b/new.txt\n@@ -0,0 +1,2 @@\n+x\n+y\n";
    CHECK(apply_ok("", create) == "x\ny\n");
}

TEST_CASE("diff: 'No newline at end of file' markers") {
    const std::string base = "a\nb";  // no final newline
    const std::string patch = "--- a/f\n+++ b/f\n@@ -1,2 +1,2 @@\n a\n-b\n\\ No newline at end of file\n+c\n\\ No newline at end of file\n";
    CHECK(apply_ok(base, patch) == "a\nc");
}

TEST_CASE("diff: malformed patches are CML5412 with a line number") {
    const std::vector<std::string> bad = {
        "",
        "just some prose\n",
        "--- a/f\n",
        "--- a/f\n+++ b/f\n",
        "--- a/f\n+++ b/f\n@@ bogus @@\n",
        "--- a/f\n+++ b/f\n@@ -1,2 +1,2 @@\n a\n",
        "--- a/f\n+++ b/f\n@@ -1,1 +1,1 @@\n a\n b\n",
        "--- a/f\n+++ b/f\n@@ -1,1 +1,1 @@\n?x\n",
        "--- a/f\n+++ b/f\n@@ -1,1 +1,1 @@\n\\ No newline\n",
        "--- a/f\n+++ b/f\n@@ -x,1 +1,1 @@\n-a\n+b\n",
        "--- a/f\n+++ b/f\n@@ -1,1 +1,1 @@\n-a\n-a\n+b\n",
    };
    for (const std::string& text : bad) {
        INFO("patch: " << text);
        cml::Result<std::vector<FilePatch>> p = parse_unified_diff(text);
        REQUIRE_FALSE(p.has_value());
        CHECK(p.error().code == cml::ids::kPatchMalformed);
        CHECK(p.error().message.find("line ") != std::string::npos);
    }
}

TEST_CASE("diff: truncating a valid patch anywhere never crashes") {
    const std::string patch = kPatch;
    for (std::size_t n = 0; n <= patch.size(); ++n) {
        (void)parse_unified_diff(patch.substr(0, n));
    }
}

TEST_CASE("diff: apply to a tree changes only the copy, strips one component, and is all-or-nothing") {
    TempDir dir;
    dir.write("logic.cpp", kLogic);
    dir.write("other.txt", "keep\n");
    cml::Result<AppliedPatch> r = apply_patch_to_tree(dir.path, kPatch, 1);
    REQUIRE_MESSAGE(r.has_value(), (r.has_value() ? "" : r.error().format()));
    REQUIRE(r->files.size() == 1);
    CHECK(r->files[0] == "logic.cpp");
    CHECK(dir.read("logic.cpp").find("return 2;") != std::string::npos);
    CHECK(dir.read("other.txt") == "keep\n");

    // applying the same patch again fails (already applied) and changes nothing
    const std::string before = dir.read("logic.cpp");
    cml::Result<AppliedPatch> again = apply_patch_to_tree(dir.path, kPatch, 1);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == cml::ids::kPatchMismatch);
    CHECK(dir.read("logic.cpp") == before);

    // two files, the second does not match: the first must stay untouched
    TempDir dir2;
    dir2.write("a.txt", "one\n");
    dir2.write("b.txt", "two\n");
    const std::string two_files =
        "--- a/a.txt\n+++ b/a.txt\n@@ -1 +1 @@\n-one\n+ONE\n"
        "--- a/b.txt\n+++ b/b.txt\n@@ -1 +1 @@\n-nope\n+NOPE\n";
    CHECK_FALSE(apply_patch_to_tree(dir2.path, two_files, 1).has_value());
    CHECK(dir2.read("a.txt") == "one\n");

    // missing target file
    cml::Result<AppliedPatch> missing = apply_patch_to_tree(dir2.path, "--- a/zzz.txt\n+++ b/zzz.txt\n@@ -1 +1 @@\n-x\n+y\n", 1);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == cml::ids::kEditFailed);

    // strip = 0 keeps the a/ b/ prefix
    TempDir dir3;
    dir3.write("a/f.txt", "x\n");
    CHECK(apply_patch_to_tree(dir3.path, "--- a/f.txt\n+++ a/f.txt\n@@ -1 +1 @@\n-x\n+y\n", 0).has_value());
    CHECK(dir3.read("a/f.txt") == "y\n");
}

TEST_CASE("diff: creation and deletion in a tree") {
    TempDir dir;
    dir.write("gone.txt", "bye\n");
    CHECK(apply_patch_to_tree(dir.path, "--- a/gone.txt\n+++ /dev/null\n@@ -1 +0,0 @@\n-bye\n", 1).has_value());
    CHECK_FALSE(fs::exists(dir.path / "gone.txt"));
    CHECK(apply_patch_to_tree(dir.path, "--- /dev/null\n+++ b/sub/new.txt\n@@ -0,0 +1 @@\n+hello\n", 1).has_value());
    CHECK(dir.read("sub/new.txt") == "hello\n");
}

TEST_CASE("diff: paths that leave the tree are refused") {
    TempDir dir;
    dir.write("f.txt", "x\n");
    for (const char* path : {"b/../../evil.txt", "b/../f.txt", "/abs/evil.txt", "b/C:/evil.txt"}) {
        const std::string patch = std::string("--- a/f.txt\n+++ ") + path + "\n@@ -1 +1 @@\n-x\n+y\n";
        INFO("path: " << path);
        cml::Result<AppliedPatch> r = apply_patch_to_tree(dir.path, patch, 1);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == cml::ids::kPatchMalformed);
    }
    CHECK(dir.read("f.txt") == "x\n");
    // too few components for the strip count
    CHECK_FALSE(apply_patch_to_tree(dir.path, "--- f.txt\n+++ f.txt\n@@ -1 +1 @@\n-x\n+y\n", 1).has_value());
}

TEST_CASE("diff: the fixture's real edit file parses") {
#ifdef CML_FIXTURE_BASIC_EDIT
    std::ifstream in(CML_FIXTURE_BASIC_EDIT, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream s;
    s << in.rdbuf();
    cml::Result<std::vector<FilePatch>> p = parse_unified_diff(s.str());
    REQUIRE_MESSAGE(p.has_value(), (p.has_value() ? "" : p.error().format()));
    REQUIRE(p->size() == 1);
    CHECK((*p)[0].new_path == "b/logic.cpp");
#ifdef CML_FIXTURE_BASIC_DIR
    // Apply it to a COPY of the real fixture sources: the repository stays untouched.
    TempDir copy;
    std::error_code ec;
    fs::copy(CML_FIXTURE_BASIC_DIR, copy.path, fs::copy_options::recursive, ec);
    REQUIRE_FALSE(ec);
    const std::string before = copy.read("logic.cpp");
    REQUIRE(before.find("return 1;") != std::string::npos);
    cml::Result<AppliedPatch> applied = apply_patch_to_tree(copy.path, s.str(), 1);
    REQUIRE_MESSAGE(applied.has_value(), (applied.has_value() ? "" : applied.error().format()));
    CHECK(copy.read("logic.cpp").find("return 2;") != std::string::npos);
    CHECK(copy.read("logic.cpp").find("return 1;") == std::string::npos);
    std::ifstream original(std::string(CML_FIXTURE_BASIC_DIR) + "/logic.cpp", std::ios::binary);
    std::ostringstream original_text;
    original_text << original.rdbuf();
    CHECK(original_text.str().find("return 1;") != std::string::npos);  // the repository copy is unchanged
#endif
#endif
}

TEST_CASE("diff: a stated start past EOF is clamped and the whole remaining range searched (review fix 4)") {
    const std::string base = "l1\nl2\nl3\nl4\nl5\n";
    const std::string far =
        "--- a/f\n+++ b/f\n@@ -50,2 +50,2 @@\n l4\n-l5\n+L5\n";
    std::vector<std::string> notes;
    CHECK(apply_ok(base, far, &notes) == "l1\nl2\nl3\nl4\nL5\n");
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].find("offset -") != std::string::npos);
    // a pure insertion stated past EOF appends at EOF
    const std::string append = "--- a/f\n+++ b/f\n@@ -99,0 +100,1 @@\n+tail\n";
    CHECK(apply_ok(base, append) == "l1\nl2\nl3\nl4\nl5\ntail\n");
    // far past EOF in a short file, match at the very start
    const std::string start = "--- a/f\n+++ b/f\n@@ -1000,1 +1000,1 @@\n-l1\n+L1\n";
    CHECK(apply_ok(base, start) == "L1\nl2\nl3\nl4\nl5\n");
    // and still a clean mismatch when it is nowhere
    cml::Result<std::vector<FilePatch>> patches = parse_unified_diff("--- a/f\n+++ b/f\n@@ -500,1 +500,1 @@\n-nothing\n+x\n");
    REQUIRE(patches.has_value());
    CHECK_FALSE(apply_file_patch(base, (*patches)[0]).has_value());
}

TEST_CASE("diff: a later hunk never matches before the end of the previous hunk's region (review fix 5)") {
    const std::string base = "x\ny\nx\ny\n";
    // hunk 2's header points at the first 'y' (line 2) but hunk 1 already covered up to line 3
    const std::string overlapping =
        "--- a/f\n+++ b/f\n"
        "@@ -3,1 +3,1 @@\n-x\n+X\n"
        "@@ -2,1 +2,1 @@\n-y\n+Y\n";
    CHECK(apply_ok(base, overlapping) == "x\ny\nX\nY\n");  // the LAST y, not the one before hunk 1
    // matches only before the floor: refused
    const std::string only_before =
        "--- a/f\n+++ b/f\n"
        "@@ -3,1 +3,1 @@\n-x\n+X\n"
        "@@ -1,1 +1,1 @@\n-x\n+Z\n";
    cml::Result<std::vector<FilePatch>> patches = parse_unified_diff(only_before);
    REQUIRE(patches.has_value());
    cml::Result<std::string> out = apply_file_patch("x\ny\nx\ny\n", (*patches)[0]);
    REQUIRE_FALSE(out.has_value());
    CHECK(out.error().code == cml::ids::kPatchMismatch);
    CHECK(out.error().message.find("hunk 2") != std::string::npos);
    // hunks that grow the file: the floor follows the new layout
    const std::string growing =
        "--- a/f\n+++ b/f\n"
        "@@ -1,1 +1,3 @@\n x\n+n1\n+n2\n"
        "@@ -3,1 +5,1 @@\n-x\n+X\n";
    CHECK(apply_ok(base, growing) == "x\nn1\nn2\ny\nX\ny\n");
}

TEST_CASE("diff: creation of an existing file and deletion of a different file are refused (review fix 6)") {
    TempDir dir;
    dir.write("exists.txt", "old\n");
    cml::Result<AppliedPatch> create = apply_patch_to_tree(dir.path, "--- /dev/null\n+++ b/exists.txt\n@@ -0,0 +1 @@\n+new\n", 1);
    REQUIRE_FALSE(create.has_value());
    CHECK(create.error().code == cml::ids::kPatchMismatch);
    CHECK(create.error().message.find("file already exists") != std::string::npos);
    CHECK(dir.read("exists.txt") == "old\n");

    // deletion: content differs -> refused, file kept
    dir.write("victim.txt", "one\ntwo\n");
    cml::Result<AppliedPatch> wrong = apply_patch_to_tree(dir.path, "--- a/victim.txt\n+++ /dev/null\n@@ -1 +0,0 @@\n-other\n", 1);
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error().code == cml::ids::kPatchMismatch);
    CHECK(fs::exists(dir.path / "victim.txt"));
    // deletion that shows only part of the file: refused too
    cml::Result<AppliedPatch> partial = apply_patch_to_tree(dir.path, "--- a/victim.txt\n+++ /dev/null\n@@ -1 +0,0 @@\n-one\n", 1);
    REQUIRE_FALSE(partial.has_value());
    CHECK(partial.error().message.find("does not remove the whole file") != std::string::npos);
    CHECK(fs::exists(dir.path / "victim.txt"));
    // the exact deletion works
    CHECK(apply_patch_to_tree(dir.path, "--- a/victim.txt\n+++ /dev/null\n@@ -1,2 +0,0 @@\n-one\n-two\n", 1).has_value());
    CHECK_FALSE(fs::exists(dir.path / "victim.txt"));

    // a deletion whose remove fails reports CML5422 (a directory that is not empty cannot be removed as a file)
    fs::create_directories(dir.path / "d" / "inner");
    std::error_code ec;
    cml::Result<AppliedPatch> not_a_file = apply_patch_to_tree(dir.path, "--- a/d\n+++ /dev/null\n@@ -1 +0,0 @@\n-x\n", 1);
    REQUIRE_FALSE(not_a_file.has_value());
    CHECK((not_a_file.error().code == cml::ids::kEditFailed || not_a_file.error().code == cml::ids::kPatchMismatch));
}
