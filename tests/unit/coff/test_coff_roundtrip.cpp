// Independent re-parse of every object the tests know about, plus the byte-identity round-trip
// hook for the WP-06 writer (see coff_independent.h).
#include <cstdint>
#include <string>
#include <vector>

#include "cml/coff/coff.h"
#include "coff_independent.h"
#include "doctest.h"
#include "fixture_paths.h"
#include "obj_builder.h"

using namespace cml_test;

namespace {

std::vector<std::uint8_t> to_u8(const std::vector<std::byte>& b) {
    std::vector<std::uint8_t> out(b.size());
    for (std::size_t i = 0; i < b.size(); ++i) {
        out[i] = std::to_integer<std::uint8_t>(b[i]);
    }
    return out;
}

ObjSpec sample_spec(bool big, bool extended_relocs) {
    ObjSpec o;
    o.bigobj = big;
    SectionSpec text;
    text.name = ".text$mn";
    text.characteristics = cml::coff::scn::CntCode | cml::coff::scn::MemExecute | cml::coff::scn::MemRead | cml::coff::scn::LnkComdat;
    text.data = Bytes(32, 0xCC);
    text.relocs = {{1, 2, 4}, {9, 3, 3}, {17, 3, 1}};
    text.extended_relocs = extended_relocs;
    o.sections.push_back(text);
    SectionSpec named;
    named.name = ".a_very_long_section_name";
    named.characteristics = cml::coff::scn::CntInitializedData | cml::coff::scn::MemRead;
    named.data = Bytes{1, 2, 3};
    o.sections.push_back(named);
    SymbolSpec s0;
    s0.name = ".text$mn";
    s0.section = 1;
    s0.storage_class = 3;
    s0.aux.push_back(aux_section_def(big, 32, 3, 0, 0x1234, 0, 2));
    o.symbols.push_back(s0);
    SymbolSpec s1;
    s1.name = "?a_long_mangled_function_name@@YAXXZ";
    s1.section = 1;
    s1.type = 0x20;
    o.symbols.push_back(s1);
    SymbolSpec s2;
    s2.name = "ext";
    o.symbols.push_back(s2);
    return o;
}

}  // namespace

TEST_CASE("coff oracle: independent parse agrees with cml_coff on synthetic objects") {
    for (bool big : {false, true}) {
        for (bool ext : {false, true}) {
            const Built b = build_object(sample_spec(big, ext));
            const auto r = cml::coff::Object::parse(as_span(b.bytes));
            REQUIRE(r.has_value());
            const IndObject ind = independent_parse(b.bytes);
            const std::string diff = compare_with_reader(ind, *r);
            CHECK_MESSAGE(diff.empty(), (big ? "bigobj " : "regular ") << (ext ? "extended: " : "plain: ") << diff);
        }
    }
}

TEST_CASE("coff oracle: independent parse agrees with cml_coff on every fixture object") {
    const FixturePaths& p = FixturePaths::get();
    REQUIRE(p.loaded());
    std::size_t checked = 0;
    for (const char* key : {"basic_objs", "cov_extras_objs", "bigobj_objs"}) {
        const std::string list = p.value(key);
        std::size_t start = 0;
        while (start < list.size()) {
            std::size_t end = list.find(';', start);
            if (end == std::string::npos) {
                end = list.size();
            }
            const std::string path = list.substr(start, end - start);
            start = end + 1;
            if (path.empty()) {
                continue;
            }
            auto bytes = cml::coff::read_file(path);
            REQUIRE_MESSAGE(bytes.has_value(), bytes.error().format());
            const std::vector<std::uint8_t> raw = to_u8(*bytes);
            const auto obj = cml::coff::Object::parse(cml::coff::ByteSpan(bytes->data(), bytes->size()));
            REQUIRE_MESSAGE(obj.has_value(), path << ": " << obj.error().format());
            const std::string diff = compare_with_reader(independent_parse(raw), *obj);
            CHECK_MESSAGE(diff.empty(), path << ": " << diff);
            ++checked;
        }
    }
    CHECK(checked >= 6u);
}

TEST_CASE("coff round trip: byte-identity helper detects differences") {
    const Built b = build_object(sample_spec(false, false));
    std::vector<std::uint8_t> other = b.bytes;
    CHECK(first_byte_difference(b.bytes, other).empty());
    other[25] = static_cast<std::uint8_t>(other[25] ^ 1u);
    CHECK(first_byte_difference(b.bytes, other) == "first difference at offset 25");
    other = b.bytes;
    other.push_back(0);
    CHECK_FALSE(first_byte_difference(b.bytes, other).empty());
}

// WP-06 plugs its writer in here: set writer_roundtrip_hook() to a function that re-emits a parsed
// Object, and this test then demands byte-identity for synthetic and fixture objects alike.
// With no writer installed (WP-02) the loop is exercised with a copy-through rewriter so the
// plumbing itself stays covered.
TEST_CASE("coff round trip: writer hook (byte identity)") {
    const ObjectRewriter hook = writer_roundtrip_hook();
    const ObjectRewriter copy_through = +[](const cml::coff::Object& o) {
        const cml::coff::ByteSpan s = o.bytes();
        std::vector<std::uint8_t> out(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            out[i] = std::to_integer<std::uint8_t>(s[i]);
        }
        return out;
    };
    const ObjectRewriter rewrite = hook != nullptr ? hook : copy_through;
    const std::string mode = hook != nullptr ? "using the installed WP-06 writer" : "no writer installed; copy-through rewriter";
    INFO(mode);

    for (bool big : {false, true}) {
        const Built b = build_object(sample_spec(big, false));
        const auto r = cml::coff::Object::parse(as_span(b.bytes));
        REQUIRE(r.has_value());
        const std::string diff = first_byte_difference(b.bytes, rewrite(*r));
        CHECK_MESSAGE(diff.empty(), (big ? "bigobj: " : "regular: ") << diff);
    }

    const FixturePaths& p = FixturePaths::get();
    REQUIRE(p.loaded());
    for (const char* key : {"basic_objs", "cov_extras_objs"}) {
        const std::string list = p.value(key);
        std::size_t start = 0;
        while (start < list.size()) {
            std::size_t end = list.find(';', start);
            if (end == std::string::npos) {
                end = list.size();
            }
            const std::string path = list.substr(start, end - start);
            start = end + 1;
            if (path.empty()) {
                continue;
            }
            auto owned = cml::coff::load_object_file(path);
            REQUIRE_MESSAGE(owned.has_value(), owned.error().format());
            const std::string diff = first_byte_difference(to_u8(owned->bytes), rewrite(owned->object));
            CHECK_MESSAGE(diff.empty(), path << ": " << diff);
        }
    }
}
