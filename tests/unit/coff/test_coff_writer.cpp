// WP-06: COFF writer, anchors and binding object.
//
//   * installs the writer into the WP-02 round-trip hook (test_coff_roundtrip.cpp then demands byte
//     identity for every fixture object);
//   * model round trips on hand-laid-out objects (padding, optional header, line numbers, trailing
//     bytes) and on synthetic objects, regular and bigobj, incl. 70000 sections / extended relocations;
//   * error paths (CML3300..CML3312);
//   * anchors: symbols only appended, COMDAT/associative structure untouched, skip rules;
//   * binding object: structure, thunk decoding, relocations.
//
// Linking the results with link.exe / lld-link is test_coff_link.cpp (unit.coff_link).
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

#include "cml/coff/anchors.h"
#include "cml/coff/binding.h"
#include "cml/coff/coff.h"
#include "cml/coff/writer.h"
#include "coff_independent.h"
#include "doctest.h"
#include "fixture_paths.h"
#include "obj_builder.h"

using namespace cml_test;
namespace coff = cml::coff;

namespace {

std::vector<std::uint8_t> rewrite_to_u8(const coff::Object& o) {
    auto r = coff::rewrite(o);
    if (!r) {
        return {};
    }
    std::vector<std::uint8_t> out(r->size());
    for (std::size_t i = 0; i < r->size(); ++i) {
        out[i] = std::to_integer<std::uint8_t>((*r)[i]);
    }
    return out;
}

// Installs the real writer into the WP-02 hook before main().
struct HookInstaller {
    HookInstaller() { writer_roundtrip_hook() = &rewrite_to_u8; }
};
const HookInstaller g_hook_installer;

coff::ByteSpan span_of(const Bytes& b) { return as_span(b); }

std::vector<std::uint8_t> to_u8(const coff::ByteVector& b) {
    std::vector<std::uint8_t> out(b.size());
    for (std::size_t i = 0; i < b.size(); ++i) {
        out[i] = std::to_integer<std::uint8_t>(b[i]);
    }
    return out;
}

std::vector<std::uint8_t> to_u8(coff::ByteSpan b) {
    std::vector<std::uint8_t> out(b.size());
    for (std::size_t i = 0; i < b.size(); ++i) {
        out[i] = std::to_integer<std::uint8_t>(b[i]);
    }
    return out;
}

bool spans_equal(coff::ByteSpan a, coff::ByteSpan b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

// Parse + model + write; REQUIREs success and returns the bytes.
std::vector<std::uint8_t> round_trip(const Bytes& in) {
    auto obj = coff::Object::parse(span_of(in));
    REQUIRE_MESSAGE(obj.has_value(), obj.error().format());
    auto out = coff::rewrite(*obj);
    REQUIRE_MESSAGE(out.has_value(), out.error().format());
    return to_u8(*out);
}

// ---- hand-laid-out regular object: optional header, padding between regions, line numbers placed
// before the relocation table, bss with a bogus raw pointer, bytes after the string table ----------
Bytes hand_object() {
    Bytes b;
    put16(b, 0x8664);
    put16(b, 3);  // sections
    put32(b, 0x11);
    put32(b, 0);  // symbol table pointer, patched below
    put32(b, 3);  // symbol records (primary + aux + primary)
    put16(b, 4);  // optional header size
    put16(b, 0x0004);
    for (std::uint8_t v : std::initializer_list<std::uint8_t>{0xAA, 0xBB, 0xCC, 0xDD}) {
        b.push_back(v);
    }
    const std::size_t table = b.size();
    b.resize(b.size() + 3 * 40, 0);
    for (int i = 0; i < 3; ++i) {
        b.push_back(0xEE);  // padding
    }
    const std::size_t data_at = b.size();
    for (std::uint8_t v : std::initializer_list<std::uint8_t>{1, 2, 3, 4}) {
        b.push_back(v);
    }
    b.push_back(0xEF);  // padding
    const std::size_t line_at = b.size();
    for (std::uint8_t v : std::initializer_list<std::uint8_t>{0, 0, 0, 0, 1, 0}) {
        b.push_back(v);
    }
    const std::size_t rel_at = b.size();
    put32(b, 0);
    put32(b, 1);
    put16(b, 4);
    const std::size_t sym_at = b.size();
    // symbol 0: .text static with section definition aux
    for (char c : std::string(".text\0\0\0", 8)) {
        b.push_back(static_cast<std::uint8_t>(c));
    }
    put32(b, 0);
    put16(b, 1);
    put16(b, 0);
    b.push_back(3);
    b.push_back(1);
    Bytes aux = aux_section_def(false, 4, 1, 1, 0xABCD, 0, 0);
    aux.resize(18);  // the builder's helper always emits 20 bytes
    b.insert(b.end(), aux.begin(), aux.end());
    // symbol 2: long name at string-table offset 4
    put32(b, 0);
    put32(b, 4);
    put32(b, 0);
    put16(b, 1);
    put16(b, 0x20);
    b.push_back(2);
    b.push_back(0);
    const std::string name = "a_long_symbol_name";
    put32(b, static_cast<std::uint32_t>(4 + name.size() + 1));
    b.insert(b.end(), name.begin(), name.end());
    b.push_back(0);
    for (std::uint8_t v : std::initializer_list<std::uint8_t>{0x77, 0x77, 0x77}) {
        b.push_back(v);
    }
    set32(b, 8, static_cast<std::uint32_t>(sym_at));
    const auto section = [&](std::size_t i, const char* nm, std::uint32_t raw_size, std::uint32_t raw_ptr,
                             std::uint32_t rel_ptr, std::uint32_t line_ptr, std::uint32_t nrel, std::uint32_t nline,
                             std::uint32_t chars) {
        const std::size_t o = table + i * 40;
        std::memcpy(b.data() + o, nm, std::strlen(nm));
        set32(b, o + 16, raw_size);
        set32(b, o + 20, raw_ptr);
        set32(b, o + 24, rel_ptr);
        set32(b, o + 28, line_ptr);
        set16(b, o + 32, nrel);
        set16(b, o + 34, nline);
        set32(b, o + 36, chars);
    };
    section(0, ".text", 4, static_cast<std::uint32_t>(data_at), static_cast<std::uint32_t>(rel_at),
            static_cast<std::uint32_t>(line_at), 1, 1, 0x60500020u);
    section(1, ".bss", 16, 0, 0, 0, 0, 0, 0xC0300080u);
    section(2, ".junk", 8, 0x1234, 0, 0, 0, 0, 0xC0300080u);
    return b;
}

ObjSpec comdat_spec(bool big) {
    using namespace cml::coff;
    ObjSpec o;
    o.bigobj = big;
    const auto add_section = [&](const char* name, std::uint32_t chars, std::size_t data_size) {
        SectionSpec s;
        s.name = name;
        s.characteristics = chars;
        s.data = Bytes(data_size, 0x90);
        o.sections.push_back(s);
    };
    const std::uint32_t code = scn::CntCode | scn::MemExecute | scn::MemRead;
    add_section(".text$mn", code | scn::LnkComdat | (3u << scn::AlignShift), 16);           // 1  COMDAT any
    add_section(".xdata", scn::CntInitializedData | scn::MemRead | scn::LnkComdat, 8);      // 2  associative -> 1
    add_section(".pdata", scn::CntInitializedData | scn::MemRead | scn::LnkComdat, 12);     // 3  associative -> 1
    add_section(".drectve", scn::LnkInfo | scn::LnkRemove, 13);                             // 4  skipped
    add_section(".debug$S", scn::CntInitializedData | scn::MemRead | scn::MemDiscardable, 8);  // 5  skipped
    add_section(".data", scn::CntInitializedData | scn::MemRead | scn::MemWrite, 8);        // 6
    SectionSpec bss;
    bss.name = ".bss";
    bss.characteristics = scn::CntUninitializedData | scn::MemRead | scn::MemWrite;
    bss.bss_size = 16;
    o.sections.push_back(bss);                                                              // 7
    add_section(".llvm_addrsig", scn::LnkRemove, 4);                                        // 8  skipped (long name)
    add_section(".CRT$XCU", scn::CntInitializedData | scn::MemRead, 8);                    // 9
    o.sections[1].relocs = {{0, 2, 3}};   // .xdata -> ADDR32NB leader (raw symbol index 2)
    o.sections[2].relocs = {{0, 0, 3}, {4, 0, 3}};
    o.sections[0].relocs = {{2, 9, 4}};   // REL32 to the undefined external (raw index 9)

    const auto sym = [&](const char* name, std::uint32_t value, std::int32_t sec, std::uint16_t type,
                         std::uint8_t cls, std::vector<Bytes> aux = {}) {
        SymbolSpec s;
        s.name = name;
        s.value = value;
        s.section = sec;
        s.type = type;
        s.storage_class = cls;
        s.aux = std::move(aux);
        o.symbols.push_back(s);
    };
    // Raw symbol indices (aux records counted) in the comments.
    sym(".text$mn", 0, 1, 0, 3, {aux_section_def(big, 16, 1, 0, 0x1111, 0, 2)});          // 0 (+aux 1)
    sym("?leader_fn@@YAXXZ", 0, 1, 0x20, 2);                                              // 2 (COMDAT leader)
    sym(".xdata", 0, 2, 0, 3, {aux_section_def(big, 8, 1, 0, 0x2222, 1, 5)});             // 3 (+aux 4) associative -> section 1
    sym(".pdata", 0, 3, 0, 3, {aux_section_def(big, 12, 2, 0, 0x3333, 1, 5)});            // 5 (+aux 6) associative -> section 1
    sym(".data", 0, 6, 0, 3, {aux_section_def(big, 8, 0, 0, 0x4444, 0, 0)});              // 7 (+aux 8)
    sym("undefined_ext", 0, 0, 0x20, 2);                                                  // 9
    sym("weak_x", 0, 0, 0, 105, {aux_weak(2, 3)});                                        // 10 (+aux 11) default = symbol 2
    sym("g_value", 0, 6, 0, 2);                                                           // 12
    return o;
}

}  // namespace

TEST_CASE("coff writer: the round-trip hook is installed") {
    CHECK(writer_roundtrip_hook() != nullptr);
}

TEST_CASE("coff writer: hand-laid-out object round trips byte for byte") {
    const Bytes in = hand_object();
    const auto obj = coff::Object::parse(span_of(in));
    REQUIRE_MESSAGE(obj.has_value(), obj.error().format());
    CHECK(obj->sections().size() == 3);
    const std::vector<std::uint8_t> out = round_trip(in);
    CHECK_MESSAGE(first_byte_difference(in, out).empty(), first_byte_difference(in, out));
}

TEST_CASE("coff writer: model keeps layout memory, pads and trailing bytes") {
    const Bytes in = hand_object();
    auto obj = coff::Object::parse(span_of(in));
    REQUIRE(obj.has_value());
    auto m = coff::ObjectModel::from_object(*obj);
    REQUIRE(m.has_value());
    CHECK(m->optional_header.size() == 4);
    CHECK(m->sections[0].data_pad.size() == 3);
    CHECK(m->sections[0].linenumber_pad.size() == 1);
    CHECK(m->sections[0].linenumbers.size() == 6);
    CHECK(m->trailing.size() == 3);
    CHECK(m->sections[1].pointer_to_raw_data == 0);
    CHECK(m->sections[2].pointer_to_raw_data == 0x1234);
    CHECK(m->raw_symbol_count() == 3);
}

TEST_CASE("coff writer: synthetic regular and bigobj objects round trip") {
    for (bool big : {false, true}) {
        const Built b = build_object(comdat_spec(big));
        const std::vector<std::uint8_t> out = round_trip(b.bytes);
        CHECK_MESSAGE(first_byte_difference(b.bytes, out).empty(), (big ? "bigobj: " : "regular: ") << first_byte_difference(b.bytes, out));
    }
}

TEST_CASE("coff writer: extended relocations (NRELOC_OVFL) round trip and are produced for >= 65535 records") {
    for (bool big : {false, true}) {
        ObjSpec o;
        o.bigobj = big;
        SectionSpec s;
        s.name = ".text";
        s.characteristics = coff::scn::CntCode | coff::scn::MemExecute | coff::scn::MemRead;
        s.data = Bytes(16, 0xCC);
        s.extended_relocs = true;
        for (std::uint32_t i = 0; i < 70000; ++i) {
            s.relocs.push_back({i % 16, 0, 4});
        }
        o.sections.push_back(s);
        SectionSpec small = s;
        small.name = ".text2";
        small.relocs = {{1, 0, 4}, {2, 0, 4}};
        o.sections.push_back(small);
        SymbolSpec sy;
        sy.name = "sym";
        sy.section = 1;
        o.symbols.push_back(sy);
        const Built b = build_object(o);
        const std::vector<std::uint8_t> out = round_trip(b.bytes);
        CHECK_MESSAGE(first_byte_difference(b.bytes, out).empty(), (big ? "bigobj: " : "regular: ") << first_byte_difference(b.bytes, out));

        // A model whose section grows past 65534 records gets the overflow record automatically.
        auto obj = coff::Object::parse(span_of(b.bytes));
        REQUIRE(obj.has_value());
        auto m = coff::ObjectModel::from_object(*obj);
        REQUIRE(m.has_value());
        m->sections[1].relocations.assign(70000, coff::ModelRelocation{0, 0, 4});
        auto w = coff::write(*m);
        REQUIRE_MESSAGE(w.has_value(), w.error().format());
        auto reparsed = coff::Object::parse(coff::ByteSpan(w->data(), w->size()));
        REQUIRE_MESSAGE(reparsed.has_value(), reparsed.error().format());
        const coff::Section* sec = reparsed->section(2);
        REQUIRE(sec != nullptr);
        CHECK(sec->has_extended_relocations);
        CHECK(sec->relocation_count == 70000);
        CHECK(sec->raw_relocation_count == 0xFFFF);
        CHECK((sec->characteristics & coff::scn::LnkNrelocOvfl) != 0);
    }
}

TEST_CASE("coff writer: bigobj with 70000 sections round trips") {
    ObjSpec o;
    o.bigobj = true;
    for (std::uint32_t i = 0; i < 70000; ++i) {
        SectionSpec s;
        s.name = i % 100 == 0 ? ".a_long_section_name_" + std::to_string(i) : ".text";
        s.characteristics = coff::scn::CntCode | coff::scn::MemExecute | coff::scn::MemRead | coff::scn::LnkComdat;
        s.data = Bytes(1, 0xC3);
        if (i % 7 == 0) {
            s.relocs = {{0, 0, 4}};
        }
        o.sections.push_back(s);
    }
    for (std::uint32_t i = 0; i < 70000; i += 3) {
        SymbolSpec sy;
        sy.name = "f" + std::to_string(i);
        sy.section = static_cast<std::int32_t>(i + 1);
        o.symbols.push_back(sy);
    }
    const Built b = build_object(o);
    const std::vector<std::uint8_t> out = round_trip(b.bytes);
    CHECK_MESSAGE(first_byte_difference(b.bytes, out).empty(), first_byte_difference(b.bytes, out));
}

TEST_CASE("coff writer: the oracle parser agrees with a rewritten object") {
    const Built b = build_object(comdat_spec(false));
    const std::vector<std::uint8_t> out = round_trip(b.bytes);
    const auto reparsed = coff::Object::parse(span_of(out));
    REQUIRE(reparsed.has_value());
    const std::string diff = compare_with_reader(independent_parse(out), *reparsed);
    CHECK_MESSAGE(diff.empty(), diff);
}

TEST_CASE("coff writer: model edits (new section with a long name, new symbols) are well formed") {
    for (bool big : {false, true}) {
        auto m = coff::ObjectModel::make_empty(coff::Machine::Amd64, big);
        auto text = m.add_section(".text$a_very_long_section_name", coff::scn::CntCode | coff::scn::MemExecute | coff::scn::MemRead,
                                  coff::ByteVector(8, std::byte{0xC3}));
        REQUIRE(text.has_value());
        auto extra = m.add_section(".data", coff::scn::CntInitializedData | coff::scn::MemRead | coff::scn::MemWrite,
                                   coff::ByteVector(4, std::byte{1}));
        REQUIRE(extra.has_value());
        auto s0 = m.add_symbol("short", 0, 1, 0x20, coff::StorageClass::External);
        auto s1 = m.add_symbol("a_much_longer_symbol_name", 4, 2, 0, coff::StorageClass::External);
        REQUIRE(s0.has_value());
        REQUIRE(s1.has_value());
        CHECK(*s0 == 0);
        CHECK(*s1 == 1);
        auto w = coff::write(m);
        REQUIRE_MESSAGE(w.has_value(), w.error().format());
        auto o = coff::Object::parse(coff::ByteSpan(w->data(), w->size()));
        REQUIRE_MESSAGE(o.has_value(), o.error().format());
        CHECK(o->is_bigobj() == big);
        CHECK(o->sections().size() == 2);
        CHECK(o->sections()[0].name == ".text$a_very_long_section_name");
        CHECK(o->sections()[0].raw_name[0] == '/');
        CHECK(o->find_symbol("a_much_longer_symbol_name") != nullptr);
        CHECK(o->find_symbol("a_much_longer_symbol_name")->value == 4);
        CHECK(o->string_table_bytes().size() > 4);
        const auto bytes = to_u8(*w);
        CHECK(compare_with_reader(independent_parse(bytes), *o).empty());
        // And it is stable: model -> bytes -> model -> bytes is a fixed point.
        auto again = coff::rewrite(*o);
        REQUIRE(again.has_value());
        CHECK(to_u8(*again) == bytes);
    }
}

TEST_CASE("coff writer: an empty model is a valid object with a string-table size field") {
    for (bool big : {false, true}) {
        auto w = coff::write(coff::ObjectModel::make_empty(coff::Machine::Amd64, big));
        REQUIRE(w.has_value());
        auto o = coff::Object::parse(coff::ByteSpan(w->data(), w->size()));
        REQUIRE_MESSAGE(o.has_value(), o.error().format());
        CHECK(o->sections().empty());
        CHECK(o->symbols().empty());
        CHECK(o->machine() == coff::Machine::Amd64);
    }
}

TEST_CASE("coff writer: errors carry CML33xx ids") {
    SUBCASE("CML3301: regular object with more than 65279 sections") {
        auto m = coff::ObjectModel::make_empty(coff::Machine::Amd64, false);
        for (int i = 0; i < 65279; ++i) {
            REQUIRE(m.add_section(".text", coff::scn::CntCode, coff::ByteVector(1, std::byte{0xC3})).has_value());
        }
        auto too_many = m.add_section(".text", coff::scn::CntCode, coff::ByteVector(1, std::byte{0xC3}));
        REQUIRE_FALSE(too_many.has_value());
        CHECK(too_many.error().code == coff::diag::kTooManySections);
        m.sections.emplace_back();
        auto w = coff::write(m);
        REQUIRE_FALSE(w.has_value());
        CHECK(w.error().code == coff::diag::kTooManySections);
        CHECK_FALSE(w.error().what_to_do.empty());
    }
    SUBCASE("CML3303: relocation names a symbol that does not exist") {
        auto m = coff::ObjectModel::make_empty(coff::Machine::Amd64);
        REQUIRE(m.add_section(".text", coff::scn::CntCode, coff::ByteVector(8, std::byte{0}), {{0, 5, 4}}).has_value());
        REQUIRE(m.add_symbol("only", 0, 1, 0, coff::StorageClass::External).has_value());
        auto w = coff::write(m);
        REQUIRE_FALSE(w.has_value());
        CHECK(w.error().code == coff::diag::kBadRelocation);
    }
    SUBCASE("CML3302: bad aux data and bad section numbers") {
        auto m = coff::ObjectModel::make_empty(coff::Machine::Amd64);
        REQUIRE(m.add_symbol("a", 0, 0, 0, coff::StorageClass::External, coff::ByteVector(7)).has_value());
        auto w = coff::write(m);
        REQUIRE_FALSE(w.has_value());
        CHECK(w.error().code == coff::diag::kBadSymbol);

        auto m2 = coff::ObjectModel::make_empty(coff::Machine::Amd64);
        REQUIRE(m2.add_symbol("b", 0, 3, 0, coff::StorageClass::External).has_value());  // no sections
        auto w2 = coff::write(m2);
        REQUIRE_FALSE(w2.has_value());
        CHECK(w2.error().code == coff::diag::kBadSymbol);

        auto m3 = coff::ObjectModel::make_empty(coff::Machine::Amd64);
        REQUIRE(m3.add_symbol("c", 0, -7, 0, coff::StorageClass::External).has_value());
        CHECK(coff::write(m3).error().code == coff::diag::kBadSymbol);
    }
    SUBCASE("CML3304: overlapping regions cannot be reproduced") {
        Bytes in = hand_object();
        // Point the first section's data into the section table.
        set32(in, 20 + 4 + 20, 30);
        auto obj = coff::Object::parse(span_of(in));
        REQUIRE_MESSAGE(obj.has_value(), obj.error().format());
        auto m = coff::ObjectModel::from_object(*obj);
        REQUIRE_FALSE(m.has_value());
        CHECK(m.error().code == coff::diag::kUnrepresentable);
        CHECK_FALSE(coff::rewrite(*obj).has_value());
    }
    SUBCASE("CML3305: bad section names") {
        auto m = coff::ObjectModel::make_empty(coff::Machine::Amd64);
        auto r = m.add_section("", coff::scn::CntCode, {});
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == coff::diag::kBadSectionName);
    }
    SUBCASE("CML3304: model header of the wrong size") {
        auto m = coff::ObjectModel::make_empty(coff::Machine::Amd64);
        m.header.resize(7);
        CHECK(coff::write(m).error().code == coff::diag::kUnrepresentable);
    }
    SUBCASE("every error id lies in the writer range 3300..3349") {
        for (std::uint32_t id : {coff::diag::kSizeOverflow, coff::diag::kTooManySections, coff::diag::kBadSymbol,
                                 coff::diag::kBadRelocation, coff::diag::kUnrepresentable, coff::diag::kBadSectionName,
                                 coff::diag::kAnchorNameClash, coff::diag::kBadAnchorOptions, coff::diag::kBindNameInvalid,
                                 coff::diag::kBindNameDuplicate, coff::diag::kBindTooMany}) {
            CHECK(id >= 3300);
            CHECK(id <= 3349);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Anchors
// ---------------------------------------------------------------------------------------------

namespace {

// Checks everything add_anchors() promises, comparing the object before and after.
void check_anchored(const coff::Object& before, const coff::Object& after, const std::vector<coff::Anchor>& anchors,
                    const coff::AnchorOptions& options, const std::string& what) {
    INFO(what);
    REQUIRE(before.sections().size() == after.sections().size());
    CHECK(before.is_bigobj() == after.is_bigobj());

    // Sections: headers (except nothing), data and relocation tables untouched.
    for (std::size_t i = 0; i < before.sections().size(); ++i) {
        const coff::Section& a = before.sections()[i];
        const coff::Section& b = after.sections()[i];
        INFO("section " << (i + 1) << " " << std::string(a.name));
        CHECK(a.name == b.name);
        CHECK(a.characteristics == b.characteristics);
        CHECK(a.virtual_size == b.virtual_size);
        CHECK(a.size_of_raw_data == b.size_of_raw_data);
        CHECK(a.relocation_count == b.relocation_count);
        CHECK(spans_equal(a.data, b.data));
        CHECK(spans_equal(a.relocation_table, b.relocation_table));
        CHECK(a.has_extended_relocations == b.has_extended_relocations);
    }

    // Symbols: the old table is an exact prefix of the new one, appended records are the anchors.
    const coff::ByteSpan old_syms = before.symbol_table_bytes();
    const coff::ByteSpan new_syms = after.symbol_table_bytes();
    REQUIRE(new_syms.size() >= old_syms.size());
    CHECK(spans_equal(old_syms, new_syms.subspan(0, old_syms.size())));
    CHECK(after.header().symbol_count == before.header().symbol_count + anchors.size());
    // The string table only grew at its end.
    const coff::ByteSpan old_str = before.string_table_bytes();
    const coff::ByteSpan new_str = after.string_table_bytes();
    if (!old_str.empty()) {
        REQUIRE(new_str.size() >= old_str.size());
        CHECK(spans_equal(old_str.subspan(4), new_str.subspan(4, old_str.size() - 4)));
    }
    // Existing symbols: same names, indices and attributes.
    for (const coff::Symbol& s : before.symbols()) {
        const coff::Symbol* t = after.symbol_at(s.index);
        REQUIRE(t != nullptr);
        CHECK(t->name == s.name);
        CHECK(t->section_number == s.section_number);
        CHECK(t->value == s.value);
        CHECK(t->aux_count == s.aux_count);
        CHECK(t->storage_class == s.storage_class);
    }

    // COMDAT structure untouched: selection, definition symbol, leader, associated section.
    for (const coff::Section& sec : before.sections()) {
        const auto a = before.comdat(sec);
        const auto b = after.comdat(*after.section(sec.index));
        CHECK(a.has_value() == b.has_value());
        if (a && b) {
            CHECK(a->selection == b->selection);
            CHECK(a->associated_section == b->associated_section);
            CHECK((a->section_symbol == nullptr) == (b->section_symbol == nullptr));
            CHECK((a->leader == nullptr) == (b->leader == nullptr));
            if (a->section_symbol != nullptr && b->section_symbol != nullptr) {
                CHECK(a->section_symbol->index == b->section_symbol->index);
            }
            if (a->leader != nullptr && b->leader != nullptr) {
                CHECK(a->leader->index == b->leader->index);
                CHECK(a->leader->name == b->leader->name);
            }
        }
    }

    // Anchors: one per anchorable section, in section order, appended in order.
    std::vector<std::uint32_t> expected;
    for (const coff::Section& sec : before.sections()) {
        coff::ModelSection ms;
        ms.name = std::string(sec.name);
        ms.characteristics = sec.characteristics;
        if (coff::is_anchorable(ms)) {
            expected.push_back(sec.index);
        }
    }
    REQUIRE(anchors.size() == expected.size());
    std::uint32_t next = before.header().symbol_count;
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        const coff::Anchor& a = anchors[i];
        CHECK(a.section_index == expected[i]);
        CHECK(a.symbol_name == coff::anchor_name(a.section_index, options.tag));
        CHECK(a.symbol_index == next);
        const coff::Symbol* s = after.symbol_at(a.symbol_index);
        REQUIRE(s != nullptr);
        CHECK(s->name == a.symbol_name);
        CHECK(s->value == 0);
        CHECK(s->section_number == static_cast<std::int32_t>(a.section_index));
        CHECK(s->type == 0);
        CHECK(s->aux_count == 0);
        CHECK(s->storage_class == (options.external ? coff::StorageClass::External : coff::StorageClass::Static));
        ++next;
    }
}

}  // namespace

TEST_CASE("coff anchors: rules on a synthetic COMDAT object (regular and bigobj)") {
    for (bool big : {false, true}) {
        for (bool external : {false, true}) {
            const Built b = build_object(comdat_spec(big));
            auto before = coff::Object::parse(span_of(b.bytes));
            REQUIRE(before.has_value());
            auto m = coff::ObjectModel::from_object(*before);
            REQUIRE(m.has_value());
            coff::AnchorOptions opt;
            opt.external = external;
            if (external) {
                opt.tag = "t1";
            }
            auto anchors = coff::add_anchors(*m, opt);
            REQUIRE_MESSAGE(anchors.has_value(), anchors.error().format());
            // Sections 1,2,3,6,7,9 are anchored; .drectve (4), .debug$S (5), .llvm_addrsig (8) are not.
            REQUIRE(anchors->size() == 6);
            std::vector<std::uint32_t> got;
            for (const coff::Anchor& a : *anchors) {
                got.push_back(a.section_index);
            }
            CHECK(got == std::vector<std::uint32_t>{1, 2, 3, 6, 7, 9});
            CHECK((*anchors)[0].symbol_name == (external ? "__cml_anchor_t1_1" : "__cml_anchor_1"));
            auto w = coff::write(*m);
            REQUIRE_MESSAGE(w.has_value(), w.error().format());
            auto after = coff::Object::parse(coff::ByteSpan(w->data(), w->size()));
            REQUIRE_MESSAGE(after.has_value(), after.error().format());
            check_anchored(*before, *after, *anchors, opt, std::string(big ? "bigobj " : "regular ") + (external ? "external" : "static"));
            // The oracle agrees with the reader on the anchored object.
            CHECK(compare_with_reader(independent_parse(to_u8(*w)), *after).empty());

            // COMDAT leader rule spelled out: section 1's leader is still symbol 1, directly after
            // its definition symbol 0, and the associative sections have no leader.
            const auto c1 = after->comdat(*after->section(1));
            REQUIRE(c1.has_value());
            REQUIRE(c1->leader != nullptr);
            CHECK(c1->section_symbol->index == 0);
            CHECK(c1->leader->index == 2);  // definition (0) + its aux (1) + leader (2)
            CHECK(c1->leader->name == "?leader_fn@@YAXXZ");
            const auto c2 = after->comdat(*after->section(2));
            REQUIRE(c2.has_value());
            CHECK(c2->selection == coff::ComdatSelection::Associative);
            CHECK(c2->associated_section == 1);
            CHECK(c2->leader == nullptr);
        }
    }
}

TEST_CASE("coff anchors: errors") {
    const Built b = build_object(comdat_spec(false));
    auto before = coff::Object::parse(span_of(b.bytes));
    REQUIRE(before.has_value());
    auto m = coff::ObjectModel::from_object(*before);
    REQUIRE(m.has_value());

    SUBCASE("external anchors need a tag") {
        coff::AnchorOptions o;
        o.external = true;
        auto r = coff::add_anchors(*m, o);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == coff::diag::kBadAnchorOptions);
    }
    SUBCASE("tags use [A-Za-z0-9_] only") {
        coff::AnchorOptions o;
        o.tag = "a-b";
        CHECK(coff::add_anchors(*m, o).error().code == coff::diag::kBadAnchorOptions);
    }
    SUBCASE("anchoring twice is refused") {
        REQUIRE(coff::add_anchors(*m).has_value());
        auto again = coff::add_anchors(*m);
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error().code == coff::diag::kAnchorNameClash);
        CHECK(again.error().message.find("__cml_anchor_") != std::string::npos);
    }
}

TEST_CASE("coff anchors: on every fixture object, anchored objects keep the original intact") {
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
            auto owned = coff::load_object_file(path);
            REQUIRE_MESSAGE(owned.has_value(), owned.error().format());
            for (bool external : {false, true}) {
                auto m = coff::ObjectModel::from_object(owned->object);
                REQUIRE_MESSAGE(m.has_value(), m.error().format());
                coff::AnchorOptions opt;
                opt.external = external;
                if (external) {
                    opt.tag = "fx";
                }
                auto anchors = coff::add_anchors(*m, opt);
                REQUIRE_MESSAGE(anchors.has_value(), path << ": " << anchors.error().format());
                auto w = coff::write(*m);
                REQUIRE_MESSAGE(w.has_value(), path << ": " << w.error().format());
                auto after = coff::Object::parse(coff::ByteSpan(w->data(), w->size()));
                REQUIRE_MESSAGE(after.has_value(), path << ": " << after.error().format());
                check_anchored(owned->object, *after, *anchors, opt, path + (external ? " (external)" : " (static)"));
                CHECK_MESSAGE(!anchors->empty(), path);
                // Anchored output is itself a fixed point of the writer.
                auto again = coff::rewrite(*after);
                REQUIRE(again.has_value());
                CHECK(to_u8(*again) == to_u8(*w));
            }
            ++checked;
        }
    }
    CHECK(checked >= 6u);
}

// ---------------------------------------------------------------------------------------------
// Binding object
// ---------------------------------------------------------------------------------------------

namespace {

std::vector<coff::BindSymbol> sample_bind() {
    return {{"foo", coff::BindKind::Function},
            {"g_tick", coff::BindKind::Data},
            {"?bar@@YAXXZ", coff::BindKind::Function},
            {"__security_cookie", coff::BindKind::Data},
            {"memcpy", coff::BindKind::Function}};
}

}  // namespace

TEST_CASE("coff binding: layout, symbols and relocations") {
    const auto syms = sample_bind();
    auto bytes = coff::make_binding_object(syms);
    REQUIRE_MESSAGE(bytes.has_value(), bytes.error().format());
    auto obj = coff::Object::parse(coff::ByteSpan(bytes->data(), bytes->size()));
    REQUIRE_MESSAGE(obj.has_value(), obj.error().format());
    CHECK_FALSE(obj->is_bigobj());
    CHECK(obj->machine() == coff::Machine::Amd64);
    CHECK(compare_with_reader(independent_parse(to_u8(*bytes)), *obj).empty());

    const coff::Section* text = obj->find_section(".text");
    const coff::Section* data = obj->find_section(".data");
    REQUIRE(text != nullptr);
    REQUIRE(data != nullptr);
    CHECK(text->is_code());
    CHECK((text->characteristics & coff::scn::MemExecute) != 0);
    CHECK((text->characteristics & coff::scn::MemRead) != 0);
    CHECK((text->characteristics & coff::scn::MemWrite) == 0);
    CHECK(text->alignment() == 16);
    CHECK((data->characteristics & coff::scn::CntInitializedData) != 0);
    CHECK((data->characteristics & coff::scn::MemWrite) != 0);
    CHECK(data->alignment() == 8);
    CHECK(text->data.size() == 3 * coff::kThunkSize);
    CHECK(data->data.size() == 5 * coff::kSlotSize);  // 3 slots + 2 placeholders
    for (std::byte v : data->data) {
        CHECK(v == std::byte{0});
    }

    auto relocs = obj->resolve_relocations(*text);
    REQUIRE_MESSAGE(relocs.has_value(), relocs.error().format());
    REQUIRE(relocs->size() == 3);
    const char* functions[3] = {"foo", "?bar@@YAXXZ", "memcpy"};
    std::set<std::uint32_t> slot_offsets;
    for (std::uint32_t i = 0; i < 3; ++i) {
        const std::uint32_t thunk = i * coff::kThunkSize;
        // The thunk bytes: FF 25 <rel32 = 0, linker fills it> then int3 padding.
        CHECK(text->data[thunk + 0] == std::byte{0xFF});
        CHECK(text->data[thunk + 1] == std::byte{0x25});
        for (std::uint32_t k = 2; k < 6; ++k) {
            CHECK(text->data[thunk + k] == std::byte{0});
        }
        for (std::uint32_t k = 6; k < coff::kThunkSize; ++k) {
            CHECK(text->data[thunk + k] == std::byte{0xCC});
        }
        // Exactly one REL32 relocation, on the displacement field, against the slot symbol.
        const coff::ResolvedRelocation& r = (*relocs)[i];
        CHECK(r.reloc.virtual_address == thunk + 2);
        CHECK(r.reloc.type == coff::RelocType::Rel32);
        REQUIRE(r.target != nullptr);
        CHECK(r.target->name == std::string("__cml_slot_") + functions[i]);
        CHECK(r.target->is_external());
        CHECK(obj->section_of(*r.target) == data);
        CHECK(r.target->value % coff::kSlotSize == 0);
        CHECK(slot_offsets.insert(r.target->value).second);
        // The thunk symbol itself: named exactly like the function, external, a function, at the thunk.
        const coff::Symbol* t = obj->find_symbol(functions[i]);
        REQUIRE(t != nullptr);
        CHECK(t->is_external());
        CHECK(t->is_function());
        CHECK(t->value == thunk);
        CHECK(obj->section_of(*t) == text);
    }
    for (const char* name : {"g_tick", "__security_cookie"}) {
        const coff::Symbol* d = obj->find_symbol(name);
        REQUIRE(d != nullptr);
        CHECK(d->is_external());
        CHECK_FALSE(d->is_function());
        CHECK(obj->section_of(*d) == data);
        CHECK(d->value % coff::kSlotSize == 0);
        CHECK(slot_offsets.insert(d->value).second);
    }
    CHECK(slot_offsets.size() == 5);
    // Every symbol is unique and nothing is undefined: the object needs nothing from the linker.
    for (const coff::Symbol& s : obj->symbols()) {
        CHECK_FALSE(s.is_undefined());
    }
}

TEST_CASE("coff binding: decode a thunk the way the CPU does after the linker applied REL32") {
    const std::vector<coff::BindSymbol> syms = {{"first", coff::BindKind::Function}, {"second", coff::BindKind::Function}};
    auto bytes = coff::make_binding_object(syms);
    REQUIRE(bytes.has_value());
    auto obj = coff::Object::parse(coff::ByteSpan(bytes->data(), bytes->size()));
    REQUIRE(obj.has_value());
    const coff::Section* text = obj->find_section(".text");
    const coff::Section* data = obj->find_section(".data");
    REQUIRE(text != nullptr);
    REQUIRE(data != nullptr);
    // Pretend link: .text at RVA 0x1000, .data at RVA 0x3000.
    const std::uint64_t text_rva = 0x1000;
    const std::uint64_t data_rva = 0x3000;
    std::vector<std::uint8_t> image = to_u8(text->data);
    auto relocs = obj->resolve_relocations(*text);
    REQUIRE(relocs.has_value());
    for (const coff::ResolvedRelocation& r : *relocs) {
        REQUIRE(r.reloc.type == coff::RelocType::Rel32);
        const std::uint64_t target = data_rva + r.target->value;  // S (+ A = 0)
        const std::uint64_t place = text_rva + r.reloc.virtual_address;
        const std::int64_t rel = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(place + 4);  // S + A - (P + 4)
        const std::uint32_t v = static_cast<std::uint32_t>(rel);
        for (int k = 0; k < 4; ++k) {
            image[r.reloc.virtual_address + k] = static_cast<std::uint8_t>(v >> (8 * k));
        }
    }
    for (std::uint32_t i = 0; i < 2; ++i) {
        const std::uint8_t* p = image.data() + i * coff::kThunkSize;
        // opcode FF /4 with ModRM mod=00 reg=100 rm=101: jmp qword ptr [rip + disp32]
        CHECK(p[0] == 0xFF);
        const std::uint8_t modrm = p[1];
        CHECK((modrm >> 6) == 0);
        CHECK(((modrm >> 3) & 7) == 4);
        CHECK((modrm & 7) == 5);
        std::int32_t disp = 0;
        std::memcpy(&disp, p + 2, 4);
        const std::uint64_t next_insn = text_rva + i * coff::kThunkSize + 6;  // RIP after the 6-byte instruction
        const std::uint64_t slot = static_cast<std::uint64_t>(static_cast<std::int64_t>(next_insn) + disp);
        const coff::Symbol* thunk = obj->find_symbol(i == 0 ? "first" : "second");
        REQUIRE(thunk != nullptr);
        const coff::Symbol* slot_sym = obj->find_symbol(std::string("__cml_slot_") + (i == 0 ? "first" : "second"));
        REQUIRE(slot_sym != nullptr);
        CHECK(slot == data_rva + slot_sym->value);
        CHECK(thunk->value == i * coff::kThunkSize);
    }
}

TEST_CASE("coff binding: degenerate inputs") {
    SUBCASE("empty list") {
        auto b = coff::make_binding_object({});
        REQUIRE(b.has_value());
        auto o = coff::Object::parse(coff::ByteSpan(b->data(), b->size()));
        REQUIRE(o.has_value());
        CHECK(o->sections().empty());
        CHECK(o->symbols().empty());
    }
    SUBCASE("only data symbols: no .text") {
        const std::vector<coff::BindSymbol> d = {{"x", coff::BindKind::Data}, {"y", coff::BindKind::Data}};
        auto b = coff::make_binding_object(d);
        REQUIRE(b.has_value());
        auto o = coff::Object::parse(coff::ByteSpan(b->data(), b->size()));
        REQUIRE(o.has_value());
        CHECK(o->find_section(".text") == nullptr);
        REQUIRE(o->find_section(".data") != nullptr);
        CHECK(o->find_symbol("y")->value == 8);
        CHECK(o->find_symbol("y")->section_number == 1);
    }
    SUBCASE("only functions") {
        const std::vector<coff::BindSymbol> f = {{"f", coff::BindKind::Function}};
        auto b = coff::make_binding_object(f);
        REQUIRE(b.has_value());
        auto o = coff::Object::parse(coff::ByteSpan(b->data(), b->size()));
        REQUIRE(o.has_value());
        CHECK(o->sections().size() == 2);
    }
    SUBCASE("5000 symbols") {
        std::vector<coff::BindSymbol> many;
        for (int i = 0; i < 5000; ++i) {
            many.push_back({"sym_with_a_longish_name_" + std::to_string(i), i % 3 == 0 ? coff::BindKind::Data : coff::BindKind::Function});
        }
        auto b = coff::make_binding_object(many);
        REQUIRE_MESSAGE(b.has_value(), b.error().format());
        auto o = coff::Object::parse(coff::ByteSpan(b->data(), b->size()));
        REQUIRE(o.has_value());
        const coff::Section* text = o->find_section(".text");
        REQUIRE(text != nullptr);
        CHECK(text->relocation_count == 3333);
        auto rs = o->resolve_relocations(*text);
        REQUIRE(rs.has_value());
        for (const coff::ResolvedRelocation& r : *rs) {
            CHECK(r.target->name.substr(0, 11) == "__cml_slot_");
        }
    }
    SUBCASE("binding objects are a writer fixed point") {
        auto b = coff::make_binding_object(sample_bind());
        REQUIRE(b.has_value());
        auto o = coff::Object::parse(coff::ByteSpan(b->data(), b->size()));
        REQUIRE(o.has_value());
        auto again = coff::rewrite(*o);
        REQUIRE(again.has_value());
        CHECK(to_u8(*again) == to_u8(*b));
    }
}

TEST_CASE("coff binding: errors") {
    SUBCASE("CML3310 empty name") {
        const std::vector<coff::BindSymbol> s = {{"", coff::BindKind::Function}};
        auto r = coff::make_binding_object(s);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == coff::diag::kBindNameInvalid);
    }
    SUBCASE("CML3310 NUL in name") {
        const std::vector<coff::BindSymbol> s = {{std::string("a\0b", 3), coff::BindKind::Data}};
        CHECK(coff::make_binding_object(s).error().code == coff::diag::kBindNameInvalid);
    }
    SUBCASE("CML3310 linker-provided __ImageBase") {
        const std::vector<coff::BindSymbol> s = {{"__ImageBase", coff::BindKind::Data}};
        auto r = coff::make_binding_object(s);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == coff::diag::kBindNameInvalid);
        CHECK(r.error().message.find("__ImageBase") != std::string::npos);
    }
    SUBCASE("CML3311 duplicate") {
        const std::vector<coff::BindSymbol> s = {{"a", coff::BindKind::Function}, {"a", coff::BindKind::Data}};
        auto r = coff::make_binding_object(s);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == coff::diag::kBindNameDuplicate);
        CHECK(r.error().message.find("'a'") != std::string::npos);
    }
    SUBCASE("CML3311 slot name collision") {
        const std::vector<coff::BindSymbol> s = {{"a", coff::BindKind::Function}, {"__cml_slot_a", coff::BindKind::Data}};
        CHECK(coff::make_binding_object(s).error().code == coff::diag::kBindNameDuplicate);
        const std::vector<coff::BindSymbol> s2 = {{"__cml_slot_a", coff::BindKind::Data}, {"a", coff::BindKind::Function}};
        CHECK(coff::make_binding_object(s2).error().code == coff::diag::kBindNameDuplicate);
    }
}
