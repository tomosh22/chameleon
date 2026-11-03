// Application hooks, portable half (WP-17): record parsing, PE section lookup, and which records run.
#include "hook/user_hooks.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "doctest.h"

using namespace cml::agent::user_hooks;

namespace {

void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x >> (8 * i));
}
void put64(std::vector<std::uint8_t>& v, std::size_t at, std::uint64_t x) {
    for (int i = 0; i < 8; ++i) v[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x >> (8 * i));
}

// A fake image at 0x140000000 of 0x10000 bytes with strings at known addresses.
struct FakeImage {
    std::uint64_t base = 0x140000000ull;
    std::uint64_t size = 0x10000;
    std::map<std::uint64_t, std::string> strings;
    int reads = 0;

    static bool read(void* context, std::uint64_t address, std::string* out) {
        auto* self = static_cast<FakeImage*>(context);
        ++self->reads;
        const auto it = self->strings.find(address);
        if (it == self->strings.end()) return false;
        *out = it->second;
        return true;
    }
};

std::vector<std::uint8_t> record(std::uint32_t kind, std::uint64_t fn, std::uint64_t key, std::uint32_t magic = kMagic) {
    std::vector<std::uint8_t> r(kRecordSize, 0);
    put32(r, 0, magic);
    put32(r, 4, kind);
    put64(r, 8, fn);
    put64(r, 16, key);
    return r;
}

void append(std::vector<std::uint8_t>& v, const std::vector<std::uint8_t>& more) {
    v.insert(v.end(), more.begin(), more.end());
}

Record rec(Kind kind, std::uint64_t fn, const char* key) {
    Record r;
    r.kind = kind;
    r.fn = fn;
    r.key = key;
    return r;
}

// An enabled module (base 0x140000000 by default) or a committed patch of `target`.
Source module(const char* name, std::vector<Record> records, std::uint64_t base = 0x140000000ull) {
    Source s;
    s.patch = false;
    s.name = name;
    s.base = base;
    s.records = std::move(records);
    return s;
}

Source patch(const char* name, std::vector<Record> records, std::uint64_t base, std::uint64_t target = 0x140000000ull,
             bool committed = true) {
    Source s;
    s.patch = true;
    s.name = name;
    s.base = base;
    s.target_base = target;
    s.committed = committed;
    s.records = std::move(records);
    return s;
}

std::vector<std::uint64_t> fns(const std::vector<Record>& records) {
    std::vector<std::uint64_t> out;
    for (const Record& r : records) out.push_back(r.fn);
    return out;
}

}  // namespace

TEST_CASE("hooks: records are found by magic, validated against the image and keyed by their string") {
    FakeImage image;
    image.strings[image.base + 0x2000] = "a.cpp|OnPre";
    image.strings[image.base + 0x2100] = "a.cpp|OnPost";

    std::vector<std::uint8_t> bytes;
    append(bytes, record(1, image.base + 0x1000, image.base + 0x2000));
    bytes.resize(bytes.size() + 8, 0);  // padding between two objects' contributions
    append(bytes, record(2, image.base + 0x1100, image.base + 0x2100));

    std::vector<Record> out;
    ParseStats stats;
    parse_section(bytes.data(), bytes.size(), image.base, image.size, &FakeImage::read, &image, &out, &stats);
    REQUIRE(out.size() == 2);
    CHECK(out[0].kind == Kind::PrePatch);
    CHECK(out[0].fn == image.base + 0x1000);
    CHECK(out[0].key == "a.cpp|OnPre");
    CHECK(out[1].kind == Kind::PostPatch);
    CHECK(out[1].key == "a.cpp|OnPost");
    CHECK(stats.rejected == 0);
    CHECK_FALSE(stats.too_many);
}

TEST_CASE("hooks: a record with a wild pointer, an unknown kind or an unreadable key is rejected, not followed") {
    FakeImage image;
    image.strings[image.base + 0x2000] = "k";

    struct Case {
        const char* what;
        std::vector<std::uint8_t> bytes;
    };
    const Case cases[] = {
        {"function outside the image", record(2, image.base + image.size + 8, image.base + 0x2000)},
        {"function below the image", record(2, image.base - 8, image.base + 0x2000)},
        {"key outside the image", record(2, image.base + 0x1000, 0x1234)},
        {"key not readable", record(2, image.base + 0x1000, image.base + 0x3000)},
        {"kind 0", record(0, image.base + 0x1000, image.base + 0x2000)},
        {"kind 7", record(7, image.base + 0x1000, image.base + 0x2000)},
    };
    for (const Case& c : cases) {
        CAPTURE(c.what);
        std::vector<Record> out;
        ParseStats stats;
        parse_section(c.bytes.data(), c.bytes.size(), image.base, image.size, &FakeImage::read, &image, &out, &stats);
        CHECK(out.empty());
        CHECK(stats.rejected == 1);
    }

    SUBCASE("bytes without the magic, and a section shorter than a record, produce nothing") {
        std::vector<std::uint8_t> junk(96, 0xAB);
        std::vector<Record> out;
        ParseStats stats;
        parse_section(junk.data(), junk.size(), image.base, image.size, &FakeImage::read, &image, &out, &stats);
        CHECK(out.empty());
        CHECK(stats.rejected == 0);
        const std::vector<std::uint8_t> shortened = record(2, image.base + 0x1000, image.base + 0x2000);
        parse_section(shortened.data(), shortened.size() - 1, image.base, image.size, &FakeImage::read, &image, &out, &stats);
        CHECK(out.empty());
    }
    SUBCASE("a null buffer or output is ignored") {
        std::vector<Record> out;
        const std::vector<std::uint8_t> one = record(2, image.base + 0x1000, image.base + 0x2000);
        parse_section(nullptr, 100, image.base, image.size, &FakeImage::read, &image, &out, nullptr);
        parse_section(one.data(), one.size(), image.base, image.size, &FakeImage::read, &image, nullptr, nullptr);
        CHECK(out.empty());
    }
}

TEST_CASE("hooks: more records than the per-image limit stops the scan and says so") {
    FakeImage image;
    image.strings[image.base + 0x2000] = "k";
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < kMaxRecordsPerImage + 10; ++i) {
        append(bytes, record(2, image.base + 0x1000, image.base + 0x2000));
    }
    std::vector<Record> out;
    ParseStats stats;
    parse_section(bytes.data(), bytes.size(), image.base, image.size, &FakeImage::read, &image, &out, &stats);
    CHECK(out.size() == kMaxRecordsPerImage);
    CHECK(stats.too_many);
}

namespace {

// Minimal PE32+ headers with the given section names (each 40 bytes), RVA and VirtualSize taken from the index.
std::vector<std::uint8_t> pe_headers(const std::vector<std::string>& names, std::uint16_t magic = 0x20B) {
    std::vector<std::uint8_t> h(0x400, 0);
    h[0] = 'M';
    h[1] = 'Z';
    const std::uint32_t pe = 0x80;
    put32(h, 0x3C, pe);
    std::memcpy(&h[pe], "PE\0\0", 4);
    h[pe + 6] = static_cast<std::uint8_t>(names.size());
    const std::uint16_t optional_size = 0xF0;
    h[pe + 20] = static_cast<std::uint8_t>(optional_size & 0xFF);
    h[pe + 21] = static_cast<std::uint8_t>(optional_size >> 8);
    h[pe + 24] = static_cast<std::uint8_t>(magic & 0xFF);
    h[pe + 25] = static_cast<std::uint8_t>(magic >> 8);
    std::size_t at = pe + 24 + optional_size;
    std::uint32_t index = 0;
    for (const std::string& name : names) {
        std::memcpy(&h[at], name.data(), name.size() < 8 ? name.size() : 8);
        put32(h, at + 8, 0x100 + index);         // VirtualSize
        put32(h, at + 12, 0x1000 * (index + 1)); // VirtualAddress
        at += 40;
        ++index;
    }
    return h;
}

}  // namespace

TEST_CASE("hooks: the .cmlhk sections of a PE image are found by their exact name") {
    SUBCASE("one section among others") {
        const std::vector<std::uint8_t> h = pe_headers({".text", ".rdata", ".cmlhk", ".reloc"});
        std::vector<SectionRange> sections;
        REQUIRE(find_hook_sections(h.data(), h.size(), &sections));
        REQUIRE(sections.size() == 1);
        CHECK(sections[0].rva == 0x3000);
        CHECK(sections[0].size == 0x102);
    }
    SUBCASE("lookalikes are not ours, and an image without the section gives an empty list") {
        const std::vector<std::uint8_t> h = pe_headers({".cmlhk2", ".cmlh", ".CMLHK", ".text"});
        std::vector<SectionRange> sections;
        REQUIRE(find_hook_sections(h.data(), h.size(), &sections));
        CHECK(sections.empty());
    }
    SUBCASE("not a PE32+ image") {
        std::vector<SectionRange> sections;
        const std::vector<std::uint8_t> pe32 = pe_headers({".cmlhk"}, 0x10B);
        CHECK_FALSE(find_hook_sections(pe32.data(), pe32.size(), &sections));
        std::vector<std::uint8_t> not_mz = pe_headers({".cmlhk"});
        not_mz[0] = 'X';
        CHECK_FALSE(find_hook_sections(not_mz.data(), not_mz.size(), &sections));
        std::vector<std::uint8_t> bad_sig = pe_headers({".cmlhk"});
        bad_sig[0x80] = 'X';
        CHECK_FALSE(find_hook_sections(bad_sig.data(), bad_sig.size(), &sections));
        CHECK_FALSE(find_hook_sections(pe32.data(), 10, &sections));
        CHECK_FALSE(find_hook_sections(nullptr, 100, &sections));
        std::vector<std::uint8_t> wild_pe = pe_headers({".cmlhk"});
        put32(wild_pe, 0x3C, 0xFFFFFFF0u);
        CHECK_FALSE(find_hook_sections(wild_pe.data(), wild_pe.size(), &sections));
        CHECK(sections.empty());
    }
    SUBCASE("a section table cut off by the end of the buffer yields what is complete") {
        const std::vector<std::uint8_t> h = pe_headers({".text", ".cmlhk", ".rdata"});
        std::vector<SectionRange> sections;
        // .text ends at 0x80+24+0xF0+40 = 0x1B0 ... cut inside the second header.
        REQUIRE(find_hook_sections(h.data(), 0x80 + 24 + 0xF0 + 40 + 20, &sections));
        CHECK(sections.empty());
        sections.clear();
        REQUIRE(find_hook_sections(h.data(), 0x80 + 24 + 0xF0 + 80, &sections));
        CHECK(sections.size() == 1);
    }
}

TEST_CASE("hooks: records of an enabled module all run, in order; the kinds do not mix") {
    const std::vector<Source> sources = {
        module("game.exe",
               {rec(Kind::PrePatch, 1, "a.cpp|P1"), rec(Kind::PostPatch, 2, "a.cpp|Q1"), rec(Kind::PrePatch, 3, "b.cpp|P2"),
                rec(Kind::CompileError, 4, "b.cpp|E")}),
        module("game_ext.dll", {rec(Kind::PrePatch, 5, "c.cpp|P3")}, 0x150000000ull),
    };
    CHECK(fns(select_hooks(Kind::PrePatch, sources)) == std::vector<std::uint64_t>{1, 3, 5});
    CHECK(fns(select_hooks(Kind::PostPatch, sources)) == std::vector<std::uint64_t>{2});
    CHECK(fns(select_hooks(Kind::CompileError, sources)) == std::vector<std::uint64_t>{4});
    CHECK(select_hooks(Kind::CompileStart, sources).empty());
    CHECK(select_hooks(Kind::PreRestart, sources).empty());
    CHECK(select_hooks(Kind::PrePatch, {}).empty());
}

TEST_CASE("hooks: a patch holds a copy of every hook of the unit it recompiled - it must not run a second time") {
    // The module has OnPost and OnOther; a patch of that unit holds copies of both (different addresses).
    const Source m = module("game.exe", {rec(Kind::PostPatch, 0x1000, "a.cpp|OnPost"), rec(Kind::PostPatch, 0x1100, "a.cpp|OnOther")});
    const Source p = patch("patch #1", {rec(Kind::PostPatch, 0x9000, "a.cpp|OnPost"), rec(Kind::PostPatch, 0x9100, "a.cpp|OnOther")}, 0x7000);
    CHECK(fns(select_hooks(Kind::PostPatch, {m, p})) == std::vector<std::uint64_t>{0x1000, 0x1100});
}

TEST_CASE("hooks: the copies differ only in how __FILE__ is spelled - still one hook") {
    // The original build says "..\\src\\A.cpp", the broker's recompile says "C:/proj/src/a.cpp".
    const Source m = module("game.exe", {rec(Kind::PostPatch, 0x1000, "..\\src\\A.cpp|OnPost"), rec(Kind::PrePatch, 0x1100, "src/a.cpp|OnPre")});
    const Source p = patch("patch #1", {rec(Kind::PostPatch, 0x9000, "C:/proj/src/a.cpp|OnPost"), rec(Kind::PrePatch, 0x9100, "C:\\proj\\SRC\\a.cpp|OnPre")}, 0x7000);
    CHECK(fns(select_hooks(Kind::PostPatch, {m, p})) == std::vector<std::uint64_t>{0x1000});
    CHECK(fns(select_hooks(Kind::PrePatch, {m, p})) == std::vector<std::uint64_t>{0x1100});
    // A different file name or function name is a different hook.
    const Source other = patch("patch #2", {rec(Kind::PostPatch, 0x9200, "C:/proj/src/b.cpp|OnPost"), rec(Kind::PostPatch, 0x9300, "C:/proj/src/a.cpp|OnOther")}, 0x8000);
    CHECK(fns(select_hooks(Kind::PostPatch, {m, other})) == std::vector<std::uint64_t>{0x1000, 0x9200, 0x9300});
    CHECK(normalize_key("C:\\Proj\\Src\\A.CPP|ns::Fn") == "a.cpp|ns::Fn");
    CHECK(normalize_key("a.cpp|Fn") == "a.cpp|Fn");
    CHECK(normalize_key("nobar") == "|nobar");
}

TEST_CASE("hooks: a hook added by a patch runs once; of several patches the newest copy runs") {
    const Source m = module("game.exe", {rec(Kind::PostPatch, 0x1000, "a.cpp|OnPost")});
    const Source p1 = patch("patch #1", {rec(Kind::PostPatch, 0x9000, "a.cpp|OnPost"), rec(Kind::PostPatch, 0x9100, "a.cpp|Added")}, 0x7000);
    const Source p2 = patch("patch #2", {rec(Kind::PostPatch, 0xA000, "a.cpp|OnPost"), rec(Kind::PostPatch, 0xA100, "a.cpp|Added"),
                                         rec(Kind::PostPatch, 0xA200, "b.cpp|AddedLater")}, 0x8000);
    CHECK(fns(select_hooks(Kind::PostPatch, {m, p1})) == std::vector<std::uint64_t>{0x1000, 0x9100});
    // The newer copy replaces the older one and keeps its place.
    CHECK(fns(select_hooks(Kind::PostPatch, {m, p1, p2})) == std::vector<std::uint64_t>{0x1000, 0xA100, 0xA200});
}

TEST_CASE("hooks: the same identity under another kind is another hook") {
    const Source m = module("game.exe", {rec(Kind::PrePatch, 0x1000, "a.cpp|Fn")});
    const Source p = patch("patch #1", {rec(Kind::PostPatch, 0x9000, "a.cpp|Fn")}, 0x7000);
    CHECK(fns(select_hooks(Kind::PostPatch, {m, p})) == std::vector<std::uint64_t>{0x9000});
    CHECK(fns(select_hooks(Kind::PrePatch, {m, p})) == std::vector<std::uint64_t>{0x1000});
}

TEST_CASE("hooks: a patch that is not committed contributes nothing") {
    const Source m = module("game.exe", {rec(Kind::PostPatch, 0x1000, "a.cpp|OnPost")});
    // Loaded, but its InstallHooks has not succeeded yet (sync mode still waiting, CML5309, fix-ups running).
    const Source loading = patch("patch #1", {rec(Kind::PostPatch, 0x9100, "a.cpp|Added")}, 0x7000, 0x140000000ull, /*committed=*/false);
    CHECK(fns(select_hooks(Kind::PostPatch, {m, loading})) == std::vector<std::uint64_t>{0x1000});
    // The same patch once committed.
    Source committed = loading;
    committed.committed = true;
    CHECK(fns(select_hooks(Kind::PostPatch, {m, committed})) == std::vector<std::uint64_t>{0x1000, 0x9100});
}

TEST_CASE("hooks: the patches of a module that is not enabled (any more) contribute nothing") {
    const Source game = module("game.exe", {rec(Kind::PostPatch, 0x1000, "a.cpp|OnPost")});
    const Source ext_patch = patch("patch of ext.dll", {rec(Kind::PostPatch, 0x9100, "e.cpp|Added")}, 0x7000, /*target=*/0x150000000ull);
    // ext.dll is not among the enabled modules: no module source has its base.
    CHECK(fns(select_hooks(Kind::PostPatch, {game, ext_patch})) == std::vector<std::uint64_t>{0x1000});
    // Enabled again (a source for it, even an empty one) and its patch counts.
    const Source ext = module("ext.dll", {}, 0x150000000ull);
    CHECK(fns(select_hooks(Kind::PostPatch, {game, ext, ext_patch})) == std::vector<std::uint64_t>{0x1000, 0x9100});
    // A patch with an unknown target (0) never counts.
    const Source orphan = patch("orphan", {rec(Kind::PostPatch, 0x9200, "o.cpp|X")}, 0x8000, /*target=*/0);
    CHECK(fns(select_hooks(Kind::PostPatch, {game, ext, orphan})) == std::vector<std::uint64_t>{0x1000});
}

TEST_CASE("hooks: the identity is scoped to the module a patch belongs to") {
    // game.exe has a.cpp|OnPost; ext.dll has none. A patch of ext.dll with a.cpp|OnPost is a NEW hook of ext.dll.
    const Source game = module("game.exe", {rec(Kind::PostPatch, 0x1000, "a.cpp|OnPost")});
    const Source ext = module("ext.dll", {}, 0x150000000ull);
    const Source ext_patch = patch("patch of ext.dll", {rec(Kind::PostPatch, 0x9100, "a.cpp|OnPost")}, 0x7000, 0x150000000ull);
    CHECK(fns(select_hooks(Kind::PostPatch, {game, ext, ext_patch})) == std::vector<std::uint64_t>{0x1000, 0x9100});
    // Two patches of different modules with the same identity do not replace each other.
    const Source game_patch = patch("patch of game.exe", {rec(Kind::PostPatch, 0xA100, "z.cpp|New")}, 0x8000, 0x140000000ull);
    const Source ext_patch2 = patch("patch 2 of ext.dll", {rec(Kind::PostPatch, 0xB100, "z.cpp|New")}, 0x9000, 0x150000000ull);
    CHECK(fns(select_hooks(Kind::PostPatch, {game, ext, game_patch, ext_patch2})) == std::vector<std::uint64_t>{0x1000, 0xA100, 0xB100});
}

TEST_CASE("hooks: kind names") {
    CHECK(std::string(to_string(Kind::PrePatch)) == "pre-patch");
    CHECK(std::string(to_string(Kind::PostPatch)) == "post-patch");
    CHECK(std::string(to_string(Kind::CompileStart)) == "compile-start");
    CHECK(std::string(to_string(Kind::CompileSuccess)) == "compile-success");
    CHECK(std::string(to_string(Kind::CompileError)) == "compile-error");
    CHECK(std::string(to_string(Kind::PreRestart)) == "pre-restart");
}
