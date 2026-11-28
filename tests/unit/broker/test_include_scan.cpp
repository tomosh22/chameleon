// WP-15: the include scan that maps headers to compilands independently of the PDB's file list (ADR-0013).
#include "doctest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>

#include "cml/core/include_scan.h"
#include "cml/core/module_index.h"

using namespace cml;
using namespace cml::core;

namespace {

// A fake file system keyed by normalised path (lower case, backslashes).
struct FakeFs {
    std::map<std::string, std::string> files;
    ScanFileReader reader() {
        return [this](const std::string& path) -> std::optional<std::string> {
            const auto it = files.find(path);
            if (it == files.end()) return std::nullopt;
            return it->second;
        };
    }
};

}  // namespace

TEST_CASE("parse_includes finds quoted and angled directives and skips comments") {
    const auto v = parse_includes(
        "#pragma once\n"
        "#include \"a.h\"\n"
        "  #  include <b/c.h>   // trailing\n"
        "// #include \"commented.h\"\n"
        "/* #include \"block.h\"\n"
        "   #include \"block2.h\" */\n"
        "#include_next <next.h>\n"
        "#import \"imp.h\"\n"
        "#include MACRO_NAME\n"
        "#define X \"not.h\"\n"
        "int x; /* c */ \n"
        "#include \"last.h\"");
    REQUIRE(v.size() == 5);
    CHECK(v[0].name == "a.h");
    CHECK_FALSE(v[0].angled);
    CHECK(v[1].name == "b/c.h");
    CHECK(v[1].angled);
    CHECK(v[2].name == "next.h");
    CHECK(v[3].name == "imp.h");
    CHECK(v[4].name == "last.h");
}

TEST_CASE("the scan follows includes across directories, -I lists and forced includes") {
    FakeFs fs;
    fs.files["c:\\proj\\src\\main.cpp"] = "#include \"util.h\"\n#include <third.h>\n";
    fs.files["c:\\proj\\src\\util.h"] = "#include \"sub/deep.h\"\n#include \"util.h\"\n";  // includes itself: a cycle
    fs.files["c:\\proj\\src\\sub\\deep.h"] = "#include \"../common.h\"\n#include \"missing.h\"\n";
    fs.files["c:\\proj\\src\\common.h"] = "// no includes\n";
    fs.files["c:\\proj\\inc\\third.h"] = "#include <fourth.h>\n";
    fs.files["c:\\proj\\inc\\fourth.h"] = "";
    fs.files["c:\\proj\\build\\pch_force.h"] = "#include \"util.h\"\n";
    fs.files["c:\\proj\\src\\unrelated.h"] = "";

    IncludeScanner scanner(fs.reader());
    ScanRoots roots;
    roots.source = "c:\\proj\\src\\main.cpp";
    roots.cwd = "c:\\proj\\build";
    roots.include_dirs = {"..\\inc", "c:\\proj\\build"};

    const std::set<std::string> deps = scanner.dependencies(roots);
    CHECK(deps == std::set<std::string>{"c:\\proj\\src\\util.h", "c:\\proj\\src\\sub\\deep.h", "c:\\proj\\src\\common.h", "c:\\proj\\inc\\third.h",
                                        "c:\\proj\\inc\\fourth.h"});

    roots.forced_includes = {"pch_force.h"};  // found next to the cwd / in the -I list
    const std::set<std::string> forced = scanner.dependencies(roots);
    CHECK(forced.count("c:\\proj\\build\\pch_force.h") == 1);
    CHECK(forced.count("c:\\proj\\src\\unrelated.h") == 0);
}

TEST_CASE("an angled include is not searched in the including file's directory; a quoted one falls back to -I") {
    FakeFs fs;
    fs.files["c:\\p\\a.cpp"] = "#include <local.h>\n#include \"shared.h\"\n";
    fs.files["c:\\p\\local.h"] = "";
    fs.files["c:\\p\\inc\\shared.h"] = "";
    IncludeScanner scanner(fs.reader());
    ScanRoots roots;
    roots.source = "c:\\p\\a.cpp";
    roots.include_dirs = {"c:\\p\\inc"};
    CHECK(scanner.dependencies(roots) == std::set<std::string>{"c:\\p\\inc\\shared.h"});
}

TEST_CASE("a header that holds no code is found through the scan (the clang-cl case of ADR-0013)") {
    FakeFs fs;
    fs.files["c:\\p\\one.cpp"] = "#include \"decls.h\"\n";
    fs.files["c:\\p\\two.cpp"] = "#include \"decls.h\"\n";
    fs.files["c:\\p\\three.cpp"] = "int x;\n";
    fs.files["c:\\p\\decls.h"] = "struct S;\n";
    IncludeScanner scanner(fs.reader());
    for (const char* tu : {"c:\\p\\one.cpp", "c:\\p\\two.cpp"}) {
        ScanRoots roots;
        roots.source = tu;
        CHECK(scanner.dependencies(roots).count("c:\\p\\decls.h") == 1);
    }
    ScanRoots three;
    three.source = "c:\\p\\three.cpp";
    CHECK(scanner.dependencies(three).empty());
}

TEST_CASE("scan_roots_from_record reads cl, clang-cl and cc1 spellings") {
    toolchain::CompileRecord cl;
    cl.cwd = "c:\\build";
    cl.source = "c:\\src\\a.cpp";
    cl.args = {"/nologo", "/c", "/Zi", "/Ic:\\inc1", "/I", "c:\\inc2", "-Ic:\\inc3", "/external:Ic:\\ext", "/FIpch.h", "/FI", "other.h", "/Yupch.h", "/Fpx.pch"};
    const ScanRoots a = scan_roots_from_record(cl);
    CHECK(a.include_dirs == std::vector<std::string>{"c:\\inc1", "c:\\inc2", "c:\\inc3", "c:\\ext"});
    CHECK(a.forced_includes == std::vector<std::string>{"pch.h", "other.h"});
    CHECK(a.uses_pch);
    CHECK_FALSE(a.creates_pch);

    toolchain::CompileRecord yc = cl;
    yc.args = {"/c", "/Ycpch.h", "/FIpch.h"};
    CHECK(scan_roots_from_record(yc).creates_pch);

    toolchain::CompileRecord cc1;
    cc1.cwd = "c:\\build";
    cc1.source = "c:\\src\\a.cpp";
    cc1.args = {"-cc1", "-triple", "x86_64-pc-windows-msvc", "-I", "c:\\inc", "-isystem", "c:\\sys", "-include", "pch.h", "-include-pch", "x.pch"};
    const ScanRoots b = scan_roots_from_record(cc1);
    CHECK(b.include_dirs == std::vector<std::string>{"c:\\inc", "c:\\sys"});
    CHECK(b.forced_includes == std::vector<std::string>{"pch.h"});
    CHECK(b.uses_pch);

    toolchain::CompileRecord emit = cc1;
    emit.args = {"-cc1", "-emit-pch", "-o", "x.pch"};
    CHECK(scan_roots_from_record(emit).creates_pch);
}

TEST_CASE("comment openers inside string and character literals do not hide later includes") {
    const auto v = parse_includes(
        "const char* a = \"assets/*\";\n"
        "#include \"after_string.h\"\n"
        "char c = '\"'; char d = '/'; char e = '*';\n"
        "#include \"after_chars.h\"\n"
        "const char* r = R\"x(/* #include \"inside_raw.h\" \n  )x\";\n"
        "#include \"after_raw.h\"\n"
        "const char* s = \"//\";\n"
        "int n = 1'000'000;\n"
        "#include \"after_separator.h\"\n"
        "/* a comment with \"a quote */\n"
        "#include \"after_comment.h\"\n"
        "// a line comment with a backslash \\\n"
        "#include \"continued_comment.h\"\n"
        "#include \"last.h\"\n");
    std::vector<std::string> names;
    for (const IncludeDirective& d : v) names.push_back(d.name);
    CHECK(names == std::vector<std::string>{"after_string.h", "after_chars.h", "after_raw.h", "after_separator.h", "after_comment.h", "last.h"});
}

TEST_CASE("a quoted include is also searched in the directories of the files further up the include stack") {
    FakeFs fs;
    fs.files["c:\\p\\a.cpp"] = "#include \"sub/x.h\"\n";
    fs.files["c:\\p\\sub\\x.h"] = "#include \"y.h\"\n";
    fs.files["c:\\p\\y.h"] = "";  // exists only next to a.cpp
    IncludeScanner scanner(fs.reader());
    ScanRoots roots;
    roots.source = "c:\\p\\a.cpp";
    CHECK(scanner.dependencies(roots) == std::set<std::string>{"c:\\p\\sub\\x.h", "c:\\p\\y.h"});
}

TEST_CASE("a header reached again through a longer include stack is searched in the new directories") {
    FakeFs fs;
    fs.files["c:\\p\\a.cpp"] = "#include \"shared.h\"\n#include \"deep/d.h\"\n";
    fs.files["c:\\p\\shared.h"] = "#include \"needs_stack.h\"\n";
    fs.files["c:\\p\\deep\\d.h"] = "#include \"../shared.h\"\n";
    fs.files["c:\\p\\deep\\needs_stack.h"] = "";  // only resolvable when shared.h is reached from deep/d.h
    IncludeScanner scanner(fs.reader());
    ScanRoots roots;
    roots.source = "c:\\p\\a.cpp";
    CHECK(scanner.dependencies(roots).count("c:\\p\\deep\\needs_stack.h") == 1);
}

TEST_CASE("scan_roots_from_record reads joined and separate directory options of every kind") {
    toolchain::CompileRecord r;
    r.cwd = "c:\\b";
    r.source = "c:\\s\\a.cpp";
    r.args = {"-iquotec:\\q1", "-iquote", "c:\\q2", "-isystemc:\\s1", "-isystem", "c:\\s2", "-idirafterc:\\d1", "-idirafter", "c:\\d2",
              "/external:Ic:\\e1", "/external:I", "c:\\e2", "/imsvcc:\\m1", "/imsvc", "c:\\m2", "-Ic:\\i1", "-I", "c:\\i2"};
    const ScanRoots roots = scan_roots_from_record(r);
    CHECK(roots.include_dirs == std::vector<std::string>{"c:\\q1", "c:\\q2", "c:\\s1", "c:\\s2", "c:\\d1", "c:\\d2", "c:\\e1", "c:\\e2", "c:\\m1", "c:\\m2",
                                                         "c:\\i1", "c:\\i2"});
}

TEST_CASE("the precompiled header is the named header and the forced includes with everything they include") {
    FakeFs fs;
    fs.files["c:\\b\\cmake_pch.hxx"] = "#include \"c:/p/pch.h\"\n";
    fs.files["c:\\p\\pch.h"] = "#include \"base.h\"\n";
    fs.files["c:\\p\\base.h"] = "";
    fs.files["c:\\p\\main.cpp"] = "#include \"other.h\"\n";
    fs.files["c:\\p\\other.h"] = "";
    IncludeScanner scanner(fs.reader());

    toolchain::CompileRecord yu;
    yu.cwd = "c:\\b";
    yu.source = "c:\\p\\main.cpp";
    yu.args = {"/Yuc:\\b\\cmake_pch.hxx", "/FIc:\\b\\cmake_pch.hxx", "/Fpc:\\b\\x.pch"};
    ScanRoots roots = scan_roots_from_record(yu);
    CHECK(roots.pch_header == "c:\\b\\cmake_pch.hxx");
    CHECK(scanner.pch_files(roots) == std::set<std::string>{"c:\\b\\cmake_pch.hxx", "c:\\p\\pch.h", "c:\\p\\base.h"});

    // A relative name is looked up next to the source, then in the cwd.
    yu.args = {"/Yucmake_pch.hxx"};
    roots = scan_roots_from_record(yu);
    CHECK(scanner.pch_files(roots).count("c:\\p\\pch.h") == 1);

    // Nothing named and nothing forced: unknown.
    yu.args = {"/Yu"};
    CHECK(scanner.pch_files(scan_roots_from_record(yu)).empty());

    toolchain::CompileRecord yc = yu;
    yc.args = {"/Yccmake_pch.hxx"};
    roots = scan_roots_from_record(yc);
    CHECK(roots.creates_pch);
    CHECK(roots.pch_header == "cmake_pch.hxx");
}

// WP-50: the scanner expands a header reached again with new stack directories by looking at those alone. These are the three
// ways a new directory can relate to the earlier answer; the expected sets are what the scanner before WP-50 (full search on
// every expansion) returns for the same trees.
TEST_CASE("WP-50: a header reached again with a new stack directory resolves its includes as a full search would") {
    const auto deps = [](const std::map<std::string, std::string>& files, const std::string& tu) {
        FakeFs fs;
        fs.files = files;
        IncludeScanner scanner(fs.reader());
        ScanRoots roots;
        roots.source = tu;
        roots.cwd = "c:\\p";
        roots.include_dirs = {"c:\\p\\inc"};
        return scanner.dependencies(roots);
    };
    SUBCASE("answered by -I first; the new directory has it: found there too") {
        CHECK(deps({{"c:\\p\\t\\tu.cpp", "#include \"../l/one.h\"\n#include \"../m/two.h\"\n"},
                    {"c:\\p\\l\\one.h", "#include \"c.h\"\n"},
                    {"c:\\p\\m\\two.h", "#include \"../l/one.h\"\n"},
                    {"c:\\p\\inc\\c.h", ""},
                    {"c:\\p\\m\\c.h", ""}},
                   "c:\\p\\t\\tu.cpp") ==
              std::set<std::string>{"c:\\p\\inc\\c.h", "c:\\p\\l\\one.h", "c:\\p\\m\\c.h", "c:\\p\\m\\two.h"});
    }
    SUBCASE("answered by a stack directory that sorts before the new one: kept") {
        CHECK(deps({{"c:\\p\\a\\tu.cpp", "#include \"../l/one.h\"\n#include \"../m/two.h\"\n"},
                    {"c:\\p\\l\\one.h", "#include \"c.h\"\n"},
                    {"c:\\p\\m\\two.h", "#include \"../l/one.h\"\n"},
                    {"c:\\p\\a\\c.h", ""},
                    {"c:\\p\\m\\c.h", ""},
                    {"c:\\p\\inc\\c.h", ""}},
                   "c:\\p\\a\\tu.cpp") == std::set<std::string>{"c:\\p\\a\\c.h", "c:\\p\\l\\one.h", "c:\\p\\m\\two.h"});
    }
    SUBCASE("answered by a stack directory that sorts after the new one: the new one answers too") {
        CHECK(deps({{"c:\\p\\z\\tu.cpp", "#include \"../l/one.h\"\n#include \"../m/two.h\"\n"},
                    {"c:\\p\\l\\one.h", "#include \"c.h\"\n"},
                    {"c:\\p\\m\\two.h", "#include \"../l/one.h\"\n"},
                    {"c:\\p\\z\\c.h", ""},
                    {"c:\\p\\m\\c.h", ""}},
                   "c:\\p\\z\\tu.cpp") ==
              std::set<std::string>{"c:\\p\\l\\one.h", "c:\\p\\m\\c.h", "c:\\p\\m\\two.h", "c:\\p\\z\\c.h"});
    }
}

TEST_CASE("WP-50: the scan's statistics count each path read once, found or missing, and the questions answered from the cache") {
    FakeFs fs;
    fs.files["c:\\proj\\src\\main.cpp"] = "#include \"a.h\"\n#include <b.h>\n";
    fs.files["c:\\proj\\src\\a.h"] = "#include <b.h>\n";
    fs.files["c:\\proj\\inc2\\b.h"] = "";
    std::size_t calls = 0;
    std::size_t hits = 0;
    const ScanFileReader inner = fs.reader();
    IncludeScanner scanner([&](const std::string& path) {
        ++calls;
        std::optional<std::string> text = inner(path);
        hits += text ? 1 : 0;
        return text;
    });
    ScanRoots roots;
    roots.source = "c:\\proj\\src\\main.cpp";
    roots.cwd = "c:\\proj";
    roots.include_dirs = {"c:\\proj\\inc1", "c:\\proj\\inc2"};  // <b.h> is looked for in inc1 first: a missing path

    CHECK(scanner.dependencies(roots) == std::set<std::string>{"c:\\proj\\src\\a.h", "c:\\proj\\inc2\\b.h"});
    const IncludeScanner::Stats first = scanner.stats();
    CHECK(first.walks == 1);
    CHECK(first.found == hits);
    CHECK(first.missing == calls - hits);
    CHECK(first.found == 3);    // main.cpp, a.h, inc2\b.h
    CHECK(first.missing >= 1);  // at least inc1\b.h
    CHECK(first.found + first.missing == scanner.files_read());
    CHECK(first.lookups > scanner.files_read());  // <b.h> twice: the second time from the cache
    CHECK(first.read_ms >= 0.0);
    CHECK(first.parse_ms >= 0.0);

    // The same translation unit again: nothing is read again, every question comes from the cache.
    scanner.dependencies(roots);
    const IncludeScanner::Stats second = scanner.stats();
    CHECK(second.walks == 2);
    CHECK(second.found == first.found);
    CHECK(second.missing == first.missing);
    CHECK(calls == first.found + first.missing);
    CHECK(second.lookups > first.lookups);
}

TEST_CASE("WP-50: the scan cache hits only for the same module stamp; a changed size, time, PDB or option misses and the new scan replaces it") {
    ScanCache cache;
    ModuleFileStamp stamp;
    stamp.size = 119u * 1024u * 1024u;
    stamp.mtime = 1700000000;
    stamp.pdb.guid[0] = 0x3C;
    stamp.pdb.age = 1;
    std::string reason;
    CHECK((cache.find("C:\\Game\\bin\\game.exe", stamp, &reason) == nullptr));
    CHECK(reason == "absent");

    auto first = std::make_shared<ScanResult>();
    first->files = {"c:\\game\\src\\a.cpp", "c:\\game\\src\\a.h"};
    first->header_deps["c:\\game\\src\\a.h"] = {0};
    cache.store("C:\\Game\\bin\\game.exe", stamp, first);
    CHECK(cache.size() == 1);

    // The same file, spelled differently: a hit, the very result stored.
    std::shared_ptr<const ScanResult> hit = cache.find("c:/game/BIN/game.exe", stamp, &reason);
    CHECK((hit == first));
    CHECK(reason.empty());

    const auto misses_with = [&](ModuleFileStamp changed, const char* expected) {
        std::string why;
        CHECK((cache.find("C:\\Game\\bin\\game.exe", changed, &why) == nullptr));
        CHECK(why == expected);
    };
    ModuleFileStamp s = stamp;
    s.size += 1;
    misses_with(s, "size");
    s = stamp;
    s.mtime += 2;
    misses_with(s, "mtime");
    s = stamp;
    s.pdb.age = 2;
    misses_with(s, "pdb");
    s = stamp;
    s.pdb.guid[15] = 0x01;
    misses_with(s, "pdb");
    s = stamp;
    s.scan_includes = false;
    misses_with(s, "options");
    CHECK(stamp_difference(stamp, stamp).empty());

    // A relinked module: its new scan replaces the entry, and the old stamp misses from then on.
    ModuleFileStamp relinked = stamp;
    relinked.mtime += 60;
    auto second = std::make_shared<ScanResult>();
    cache.store("C:\\Game\\bin\\game.exe", relinked, second);
    CHECK(cache.size() == 1);
    CHECK((cache.find("C:\\Game\\bin\\game.exe", relinked, &reason) == second));
    misses_with(stamp, "mtime");

    // Bounded: the least recently used module file goes.
    for (std::size_t i = 0; i < ScanCache::kMaxEntries + 4; ++i) {
        cache.store("C:\\other\\m" + std::to_string(i) + ".dll", stamp, std::make_shared<ScanResult>());
        if (i % 4 == 0) {
            CHECK((cache.find("C:\\Game\\bin\\game.exe", relinked, &reason) == second));  // kept in use
        }
    }
    CHECK(cache.size() == ScanCache::kMaxEntries);
    CHECK((cache.find("C:\\Game\\bin\\game.exe", relinked, &reason) == second));
    CHECK((cache.find("C:\\other\\m1.dll", stamp, &reason) == nullptr));
    CHECK(reason == "absent");
}

TEST_CASE("WP-50 (review): a scan cache hit re-checks every input: an edited source, a created or removed file, a changed directory miss") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
                          ("cml_scan_inputs_" + std::to_string(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFFF)));
    fs::create_directories(root / "src");
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
    } cleanup{root};
    const auto write = [](const fs::path& p, const std::string& text) {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << text;
    };
    const std::string source = (root / "src" / "a.cpp").string();
    const std::string header = (root / "src" / "a.h").string();
    const std::string later = (root / "src" / "later.cpp").string();  // absent when scanned
    const std::string dir = (root / "src").string();
    write(source, "#include \"a.h\"\nint a() { return 1; }\n");
    write(header, "#pragma once\n");

    ScanCache cache;
    ModuleFileStamp stamp;
    stamp.size = 4096;
    stamp.mtime = 1700000000;
    const std::string exe = "C:\\Game\\bin\\game.exe";
    const auto stored = [&]() {
        auto result = std::make_shared<ScanResult>();
        result->inputs = {probe_scan_input(source), probe_scan_input(header), probe_scan_input(later), probe_scan_input(dir)};
        cache.store(exe, stamp, result);
        return result;
    };
    auto entry = stored();
    CHECK(entry->inputs[0].exists);
    CHECK_FALSE(entry->inputs[0].directory);
    CHECK(entry->inputs[0].size > 0);
    CHECK_FALSE(entry->inputs[2].exists);
    CHECK(entry->inputs[3].directory);
    CHECK(changed_scan_input(entry->inputs).empty());
    std::string reason;
    CHECK((cache.find(exe, stamp, &reason) == entry));
    CHECK(reason.empty());

    // The source gains `#include "new.h"` (and new.h is created): the same module stamp, a miss naming the source.
    write(source, "#include \"a.h\"\n#include \"new.h\"\nint a() { return 1; }\n");
    write(root / "src" / "new.h", "#pragma once\n");
    CHECK((cache.find(exe, stamp, &reason) == nullptr));
    CHECK(reason == "file-changed:" + source);
    CHECK(cache.size() == 1);  // kept until the new scan replaces it

    // Only the time changed (same size): a miss.
    entry = stored();
    std::error_code ec;
    fs::last_write_time(fs::path(header), fs::last_write_time(fs::path(header), ec) + std::chrono::seconds(2), ec);
    CHECK((cache.find(exe, stamp, &reason) == nullptr));
    CHECK(reason == "file-changed:" + header);

    // A path the result found absent appears.
    entry = stored();
    write(later, "int later() { return 2; }\n");
    CHECK((cache.find(exe, stamp, &reason) == nullptr));
    CHECK(reason == "file-changed:" + later);

    // A file the result read disappears.
    entry = stored();
    fs::remove(fs::path(header), ec);
    CHECK((cache.find(exe, stamp, &reason) == nullptr));
    CHECK(reason == "file-changed:" + header);

    // Only the directory changed (a file created or removed in it, where an include could now resolve): a miss. The time is set
    // explicitly so that the test does not depend on the file system's timestamp resolution.
    entry = stored();
    CHECK((cache.find(exe, stamp, &reason) == entry));
    fs::last_write_time(fs::path(dir), fs::last_write_time(fs::path(dir), ec) + std::chrono::seconds(2), ec);
    CHECK((cache.find(exe, stamp, &reason) == nullptr));
    CHECK(reason == "file-changed:" + dir);
    entry = stored();
    CHECK((cache.find(exe, stamp, &reason) == entry));
}

namespace {
struct ScratchDir {
    std::filesystem::path root;
    ScratchDir() {
        root = std::filesystem::temp_directory_path() /
               ("cml_scan_rec_" + std::to_string(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFFF)));
        std::filesystem::create_directories(root / "src");
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    std::string path(const std::string& name) const { return (root / "src" / name).string(); }
    std::string dir() const { return (root / "src").string(); }
};
void write_file(const std::string& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}
}  // namespace

TEST_CASE("WP-50 (second review): an input edited after the scan read it makes the scan uncacheable, and its stored stamp is the one from before") {
    ScratchDir d;
    const std::string source = d.path("a.cpp");
    write_file(source, "#include \"a.h\"\nint a() { return 1; }\n");
    write_file(d.path("a.h"), "#pragma once\n");
    const std::string nodir = (d.root / "nodir" / "x.h").string();
    const auto run_scan = [&](ScanInputRecorder& rec) {
        const ScanFileReader reader = rec.wrap(disk_file_reader());
        CHECK(reader(source).has_value());
        CHECK(reader(d.path("a.h")).has_value());
        CHECK_FALSE(reader(d.path("missing.h")).has_value());  // a candidate that does not exist
        CHECK_FALSE(reader(nodir).has_value());                // in a directory that does not exist
    };

    // Nothing changed: cacheable.
    {
        ScanInputRecorder rec;
        run_scan(rec);
        for (const std::string& p : {source, d.path("a.h"), d.path("missing.h"), d.dir()}) {
            CHECK(rec.not_cacheable(p).empty());
        }
        REQUIRE((rec.stamped(source) != nullptr));
        CHECK(rec.stamped(source)->exists);
        REQUIRE((rec.stamped(d.dir()) != nullptr));
        CHECK(rec.stamped(d.dir())->directory);
        CHECK((rec.stamped(nodir) == nullptr));  // its directory's stamp covers it
        REQUIRE((rec.stamped((d.root / "nodir").string()) != nullptr));
        CHECK_FALSE(rec.stamped((d.root / "nodir").string())->exists);
    }

    // The source gains #include "new.h" after the scan read it, before the result is stored.
    ScanInputRecorder rec;
    run_scan(rec);
    const std::uintmax_t old_size = std::filesystem::file_size(source);
    write_file(source, "#include \"a.h\"\n#include \"new.h\"\nint a() { return 1; }\n");
    std::error_code ec;
    std::filesystem::last_write_time(source, std::filesystem::last_write_time(source, ec) + std::chrono::seconds(2), ec);
    CHECK(rec.not_cacheable(source) == "changed-during-scan:" + source);
    CHECK(rec.not_cacheable(d.path("a.h")).empty());
    REQUIRE((rec.stamped(source) != nullptr));
    CHECK(rec.stamped(source)->size == old_size);  // the stamp from before the read, not the edited file's

    // Even if it were stored, such a part could never be reused: the next lookup sees the source changed.
    ScanCache cache;
    ModuleFileStamp stamp;
    stamp.size = 1;
    auto result = std::make_shared<ScanResult>();
    result->inputs = {*rec.stamped(source), *rec.stamped(d.path("a.h")), *rec.stamped(d.dir())};
    CompilandScan part;
    part.inputs = {0, 1, 2};
    result->compilands.push_back(part);
    cache.store("C:\\bin\\m.exe", stamp, result);
    std::string reason;
    CHECK((cache.find("C:\\bin\\m.exe", stamp, &reason) == nullptr));
    CHECK(reason == "file-changed:" + source);
    const ScanCache::Lookup found = cache.lookup("C:\\bin\\m.exe", stamp);
    REQUIRE(found.stale.size() == 1);
    CHECK(found.stale[0] == "file-changed:" + source);
    CHECK(found.reusable == 0);
}

TEST_CASE("WP-55 (R-63): a file over the size limit is an input with its stamp; only a read that may succeed later makes a part uncacheable") {
    ScratchDir d;
    write_file(d.path("ok.h"), "#pragma once\n");
    write_file(d.path("big.h"), std::string(9u * 1024u * 1024u, ' '));  // over the scanner's 8 MiB limit
    write_file(d.path("locked.h"), "#pragma once\n");                     // stands for a sharing violation or a read error
    ScanInputRecorder rec;
    const ScanFileReader disk = disk_file_reader();
    const std::string locked = d.path("locked.h");
    const ScanFileReader reader = rec.wrap([&](const std::string& p) -> std::optional<std::string> {
        if (p == locked) {
            return std::nullopt;
        }
        return disk(p);
    });
    CHECK(reader(d.path("ok.h")).has_value());
    CHECK_FALSE(reader(d.path("big.h")).has_value());
    CHECK_FALSE(reader(d.path("missing.h")).has_value());
    CHECK_FALSE(reader(d.dir()).has_value());  // an include naming a directory
    CHECK_FALSE(reader(locked).has_value());

    CHECK(rec.not_cacheable(d.path("ok.h")).empty());
    CHECK(rec.not_cacheable(d.path("missing.h")).empty());
    CHECK(rec.not_cacheable(d.dir()).empty());
    // Over the limit: never read, whatever the attempt, so the same answer every time its stamp holds (was: unreadable:).
    CHECK(rec.not_cacheable(d.path("big.h")).empty());
    REQUIRE((rec.stamped(d.path("big.h")) != nullptr));
    CHECK(rec.stamped(d.path("big.h"))->exists);
    CHECK(rec.stamped(d.path("big.h"))->size > kScanMaxFileBytes);
    // A file that exists, is under the limit and still could not be read: may succeed next time.
    CHECK(rec.not_cacheable(locked) == "unreadable:" + locked);

    // The big file shrinks (and now includes something): its stamp changes, so a part that recorded it is scanned again.
    ScanCache cache;
    ModuleFileStamp stamp;
    stamp.size = 1;
    auto result = std::make_shared<ScanResult>();
    result->inputs = {*rec.stamped(d.path("ok.h")), *rec.stamped(d.path("big.h"))};
    CompilandScan part;
    part.inputs = {0, 1};
    result->compilands.push_back(part);
    cache.store("C:\\bin\\m.exe", stamp, result);
    std::string reason;
    CHECK((cache.find("C:\\bin\\m.exe", stamp, &reason) == result));
    write_file(d.path("big.h"), "#pragma once\n#include \"ok.h\"\n");
    CHECK((cache.find("C:\\bin\\m.exe", stamp, &reason) == nullptr));
    CHECK(reason == "file-changed:" + d.path("big.h"));
}

TEST_CASE("WP-55 (R-58): a scan cache lookup reuses each compiland whose inputs are unchanged; a changed input, an uncached part or another stamp rescans") {
    ScratchDir d;
    const std::string a = d.path("a.cpp"), ah = d.path("a.h"), b = d.path("b.cpp"), shared = d.path("shared.h");
    write_file(a, "#include \"a.h\"\n#include \"shared.h\"\n");
    write_file(ah, "#pragma once\n");
    write_file(b, "#include \"shared.h\"\n");
    write_file(shared, "#pragma once\n");
    std::error_code ec;
    const auto bump = [&](const std::string& p) {
        std::filesystem::last_write_time(p, std::filesystem::last_write_time(p, ec) + std::chrono::seconds(2), ec);
    };
    ModuleFileStamp stamp;
    stamp.size = 4096;
    stamp.mtime = 1700000000;
    const std::string exe = "C:\\Game\\bin\\game.exe";
    ScanCache cache;
    const auto stored = [&](bool with_uncached) {
        auto r = std::make_shared<ScanResult>();
        r->inputs = {probe_scan_input(a), probe_scan_input(ah), probe_scan_input(b), probe_scan_input(shared), probe_scan_input(d.dir())};
        CompilandScan c0;  // a.cpp: includes a.h and shared.h
        c0.compiland = 0;
        c0.inputs = {0, 1, 3};
        c0.files = {0, 1, 3};
        c0.deps = {1, 3};
        CompilandScan c1;  // b.cpp: includes shared.h, and looked for a missing header in the directory
        c1.compiland = 1;
        c1.inputs = {2, 3, 4};
        c1.files = {2, 3};
        c1.deps = {3};
        c1.uses_pch = true;  // a /Yu compiland whose precompiled header is unknown
        CompilandScan c2;  // a toolchain archive member: nothing
        c2.compiland = 2;
        c2.toolchain = true;
        r->compilands = {c0, c1, c2};
        if (with_uncached) {
            CompilandScan c3;
            c3.compiland = 3;
            c3.not_cacheable = "unreadable:" + ah;
            c3.skipped = SkippedCompiland{3, "c.obj", "", "no build environment recorded"};
            r->compilands.push_back(c3);
        }
        assemble_scan_result(*r);
        cache.store(exe, stamp, r);
        return r;
    };

    // What the parts assemble to.
    {
        const auto r = stored(true);
        CHECK(r->files == std::set<std::string>{a, ah, b, shared});
        CHECK(r->header_deps.at(shared) == std::vector<std::uint32_t>{0, 1});
        CHECK(r->header_deps.at(ah) == std::vector<std::uint32_t>{0});
        CHECK(r->skipped_set == std::set<std::uint32_t>{2, 3});
        REQUIRE(r->skipped.size() == 1);
        CHECK(r->skipped[0].compiland == 3);
        CHECK(r->pch_unverified == std::set<std::uint32_t>{1});
        // A part that could not be cached is scanned again, the others are reused.
        const ScanCache::Lookup l = cache.lookup(exe, stamp);
        CHECK((l.result == r));
        REQUIRE(l.stale.size() == 4);
        CHECK(l.stale[0].empty());
        CHECK(l.stale[1].empty());
        CHECK(l.stale[2].empty());
        CHECK(l.stale[3] == "not-cached:unreadable:" + ah);
        CHECK(l.reusable == 3);
        CHECK(l.reason == "not-cached:unreadable:" + ah);
        std::string reason;
        CHECK((cache.find(exe, stamp, &reason) == nullptr));
    }
    const auto r = stored(false);
    std::string reason;
    CHECK((cache.find(exe, stamp, &reason) == r));
    CHECK(reason.empty());

    // a.cpp edited: only its compiland.
    write_file(a, "#include \"a.h\"\n#include \"shared.h\"\n#include \"b2.h\"\n");
    bump(a);
    ScanCache::Lookup l = cache.lookup(exe, stamp);
    REQUIRE(l.stale.size() == 3);
    CHECK(l.stale[0] == "file-changed:" + a);
    CHECK(l.stale[1].empty());
    CHECK(l.stale[2].empty());
    CHECK(l.reusable == 2);
    CHECK(l.reason == "file-changed:" + a);

    // shared.h edited: both compilands that include it.
    stored(false);
    bump(shared);
    l = cache.lookup(exe, stamp);
    CHECK(l.stale[0] == "file-changed:" + shared);
    CHECK(l.stale[1] == "file-changed:" + shared);
    CHECK(l.stale[2].empty());
    CHECK(l.reusable == 1);

    // The directory changed (a file created where b.cpp's walk found nothing): only b.cpp's compiland.
    stored(false);
    bump(d.dir());
    l = cache.lookup(exe, stamp);
    CHECK(l.stale[0].empty());
    CHECK(l.stale[1] == "file-changed:" + d.dir());
    CHECK(l.reusable == 2);

    // Another module stamp: nothing is reused.
    stored(false);
    ModuleFileStamp relinked = stamp;
    relinked.mtime += 60;
    l = cache.lookup(exe, relinked);
    CHECK((l.result == nullptr));
    CHECK(l.reusable == 0);
    CHECK(l.reason == "mtime");
}

TEST_CASE("WP-55 (R-58): the scanner traces every path a walk asked about, answers from its memory included") {
    FakeFs fs;
    fs.files["c:\\proj\\src\\a.cpp"] = "#include \"common.h\"\n#include <lib.h>\n";
    fs.files["c:\\proj\\src\\b.cpp"] = "#include \"common.h\"\n";
    fs.files["c:\\proj\\src\\common.h"] = "#include \"inner.h\"\n";
    fs.files["c:\\proj\\inc\\inner.h"] = "";
    fs.files["c:\\proj\\inc\\lib.h"] = "";
    IncludeScanner scanner(fs.reader());
    scanner.set_tracing(true);
    const auto touched = [&]() {
        std::set<std::string> out;
        for (const std::uint32_t id : scanner.take_touched()) {
            out.insert(scanner.path_of(id));
        }
        return out;
    };
    ScanRoots ra;
    ra.source = "c:\\proj\\src\\a.cpp";
    ra.cwd = "c:\\proj\\src";
    ra.include_dirs = {"c:\\proj\\inc"};
    ScanRoots rb = ra;
    rb.source = "c:\\proj\\src\\b.cpp";

    CHECK(scanner.dependencies(ra) == std::set<std::string>{"c:\\proj\\src\\common.h", "c:\\proj\\inc\\inner.h", "c:\\proj\\inc\\lib.h"});
    const std::set<std::string> of_a = touched();
    CHECK(of_a.count("c:\\proj\\src\\a.cpp") == 1);
    CHECK(of_a.count("c:\\proj\\src\\common.h") == 1);
    CHECK(of_a.count("c:\\proj\\src\\inner.h") == 1);  // looked for next to common.h first: missing
    CHECK(of_a.count("c:\\proj\\inc\\inner.h") == 1);
    CHECK(of_a.count("c:\\proj\\inc\\lib.h") == 1);
    const std::size_t read_after_a = scanner.files_read();

    // b.cpp asks the same questions about common.h: answered from memory, nothing read, still traced.
    CHECK(scanner.dependencies(rb) == std::set<std::string>{"c:\\proj\\src\\common.h", "c:\\proj\\inc\\inner.h"});
    const std::set<std::string> of_b = touched();
    CHECK(scanner.files_read() == read_after_a + 1);  // b.cpp itself
    CHECK(of_b == std::set<std::string>{"c:\\proj\\src\\b.cpp", "c:\\proj\\src\\common.h", "c:\\proj\\src\\inner.h", "c:\\proj\\inc\\inner.h"});
    CHECK(touched().empty());  // handed over once

    // Not tracing: nothing is noted.
    scanner.set_tracing(false);
    scanner.dependencies(ra);
    CHECK(touched().empty());
}
