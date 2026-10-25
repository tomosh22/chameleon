// cml_coff on hand-built objects: structure, typed aux records, COMDAT queries, /bigobj with
// 32-bit section numbers, extended relocations, directives, and malformed input.
#include "cml/coff/coff.h"

#include <cstdint>
#include <string>
#include <vector>

#include "doctest.h"
#include "obj_builder.h"

using namespace cml::coff;
using namespace cml_test;

namespace {

constexpr std::uint32_t kAlign16 = 0x00500000u;
constexpr std::uint32_t kTextChars = scn::CntCode | scn::MemExecute | scn::MemRead | kAlign16;

SymbolSpec sym(std::string name, std::int32_t section, std::uint32_t value = 0, std::uint8_t cls = 2, std::uint16_t type = 0) {
    SymbolSpec s;
    s.name = std::move(name);
    s.section = section;
    s.value = value;
    s.storage_class = cls;
    s.type = type;
    return s;
}

// A regular object that touches most features. Raw symbol indices (primary + aux):
//   0 .text (aux 1)   2 ?func@@YAXXZ   3 undefined_target   4 weak_ext (aux 5)   6 weak_default
//   7 .file (aux 8)   9 @feat.00   10 .data (aux 11)   12 .drectve (aux 13)   14 a_rather_long_symbol_name
ObjSpec basic_spec(bool big = false) {
    ObjSpec o;
    o.bigobj = big;

    SectionSpec text;
    text.name = ".text";
    text.characteristics = kTextChars | scn::LnkComdat;
    text.data = Bytes(16, 0x90);
    text.relocs = {{0x4, 3, 4}, {0xC, 2, 3}};
    o.sections.push_back(text);

    SectionSpec data;
    data.name = ".data";
    data.characteristics = scn::CntInitializedData | scn::MemRead | scn::MemWrite | kAlign16;
    data.data = Bytes{1, 2, 3, 4, 5, 6, 7, 8};
    o.sections.push_back(data);

    const std::string drectve = " /DEFAULTLIB:\"LIBCMT\" /alternatename:foo=bar /EXPORT:baz /INCLUDE:_fltused "
                                "/FAILIFMISMATCH:\"_ITERATOR_DEBUG_LEVEL=0\"";
    SectionSpec dr;
    dr.name = ".drectve";
    dr.characteristics = scn::LnkInfo | scn::LnkRemove;
    dr.data = Bytes(drectve.begin(), drectve.end());
    o.sections.push_back(dr);

    SectionSpec longname;
    longname.name = ".a_section_with_a_long_name";
    longname.characteristics = scn::CntInitializedData | scn::MemRead;
    longname.data = Bytes{9};
    o.sections.push_back(longname);

    SectionSpec bss;
    bss.name = ".bss";
    bss.characteristics = scn::CntUninitializedData | scn::MemRead | scn::MemWrite;
    bss.bss_size = 64;
    o.sections.push_back(bss);

    SymbolSpec text_sym = sym(".text", 1, 0, 3);
    text_sym.aux.push_back(aux_section_def(big, 16, 2, 0, 0xABCD, 0, 2));  // COMDAT Any
    o.symbols.push_back(text_sym);
    o.symbols.push_back(sym("?func@@YAXXZ", 1, 0, 2, 0x20));
    o.symbols.push_back(sym("undefined_target", 0));
    SymbolSpec weak = sym("weak_ext", 0, 0, 105);
    weak.aux.push_back(aux_weak(6, 3));
    o.symbols.push_back(weak);
    o.symbols.push_back(sym("weak_default", 1, 8));
    SymbolSpec file = sym(".file", -2, 0, 103);
    file.aux.push_back(aux_file("test.cpp"));
    o.symbols.push_back(file);
    o.symbols.push_back(sym("@feat.00", -1, 0x80010, 3));
    SymbolSpec data_sym = sym(".data", 2, 0, 3);
    data_sym.aux.push_back(aux_section_def(big, 8, 0, 0, 0, 0, 0));
    o.symbols.push_back(data_sym);
    SymbolSpec dr_sym = sym(".drectve", 3, 0, 3);
    dr_sym.aux.push_back(aux_section_def(big, static_cast<std::uint32_t>(drectve.size()), 0, 0, 0, 0, 0));
    o.symbols.push_back(dr_sym);
    o.symbols.push_back(sym("a_rather_long_symbol_name", 4, 0, 3));
    return o;
}

// Touches every accessor so malformed-but-accepted input cannot hide a latent out-of-bounds read.
void exercise(const Object& o) {
    for (const Section& s : o.sections()) {
        (void)s.alignment();
        (void)o.comdat(s);
        (void)o.associative_children(s);
        const auto r = o.resolve_relocations(s);
        (void)r.has_value();
        for (const Relocation& rel : o.relocations(s)) {
            (void)o.symbol_at(rel.symbol_index);
            (void)reloc_type_name(rel.type);
        }
    }
    for (const Symbol& s : o.symbols()) {
        (void)o.section_of(s);
        (void)o.section_definition(s);
        (void)o.function_definition(s);
        (void)o.weak_external(s);
        (void)o.weak_external_default(s);
        (void)o.file_name(s);
        (void)o.find_symbol(s.name);
    }
    (void)o.directives();
}

}  // namespace

TEST_CASE("coff: regular header, sections and long section names") {
    const Built b = build_object(basic_spec());
    const auto r = Object::parse(as_span(b.bytes));
    REQUIRE(r.has_value());
    const Object& o = *r;

    CHECK_FALSE(o.is_bigobj());
    CHECK(o.machine() == Machine::Amd64);
    CHECK(o.header().header_size == 20u);
    CHECK(o.header().symbol_record_size == 18u);
    CHECK(o.header().section_count == 5u);
    CHECK(o.header().symbol_count == 15u);  // 10 primary + 5 aux records
    REQUIRE(o.sections().size() == 5u);

    const Section& text = o.sections()[0];
    CHECK(text.index == 1u);
    CHECK(text.name == ".text");
    CHECK(text.is_code());
    CHECK(text.is_comdat());
    CHECK(text.alignment() == 16u);
    CHECK(text.data.size() == 16u);
    CHECK(text.relocation_count == 2u);
    CHECK(text.relocation_bytes.size() == 20u);

    CHECK(o.sections()[3].name == ".a_section_with_a_long_name");
    CHECK(o.find_section(".a_section_with_a_long_name") == &o.sections()[3]);
    CHECK(o.find_section(".nope") == nullptr);
    CHECK(o.section(0) == nullptr);
    CHECK(o.section(6) == nullptr);

    const Section& bss = o.sections()[4];
    CHECK(bss.name == ".bss");
    CHECK(bss.size_of_raw_data == 64u);
    CHECK(bss.data.empty());
    CHECK((bss.characteristics & scn::CntUninitializedData) != 0u);

    CHECK(o.header_bytes().size() == 20u);
    CHECK(o.section_table_bytes().size() == 5u * 40u);
    CHECK(o.symbol_table_bytes().size() == 15u * 18u);
    CHECK(o.string_table_bytes().size() == b.strtab_size);
    exercise(o);
}

TEST_CASE("coff: symbols keep raw indices across aux records") {
    const Built b = build_object(basic_spec());
    const auto r = Object::parse(as_span(b.bytes));
    REQUIRE(r.has_value());
    const Object& o = *r;

    REQUIRE(o.symbols().size() == 10u);
    CHECK(o.symbol_at(0) != nullptr);
    CHECK(o.symbol_at(1) == nullptr);  // aux of .text
    REQUIRE(o.symbol_at(2) != nullptr);
    CHECK(o.symbol_at(2)->name == "?func@@YAXXZ");
    CHECK(o.symbol_at(2)->index == 2u);
    CHECK(o.symbol_at(5) == nullptr);  // aux of weak_ext
    CHECK(o.symbol_at(14) != nullptr);
    CHECK(o.symbol_at(15) == nullptr);  // one past the end
    CHECK(o.symbol_at(0xFFFFFFFFu) == nullptr);

    const Symbol* longsym = o.find_symbol("a_rather_long_symbol_name");
    REQUIRE(longsym != nullptr);
    CHECK(longsym->name_in_string_table);
    CHECK(longsym->index == 14u);
    CHECK(longsym->section_number == 4);
    CHECK(o.section_of(*longsym) == &o.sections()[3]);

    const Symbol* func = o.find_symbol("?func@@YAXXZ");
    REQUIRE(func != nullptr);
    CHECK(func->name_in_string_table);  // 12 characters: stored in the string table
    CHECK(func->name_offset >= 4u);
    REQUIRE(o.find_symbol("weak_ext") != nullptr);
    CHECK_FALSE(o.find_symbol("weak_ext")->name_in_string_table);  // exactly 8: inline
    CHECK(func->is_function());
    CHECK(func->is_external());
    CHECK_FALSE(func->is_undefined());

    const Symbol* undef = o.find_symbol("undefined_target");
    REQUIRE(undef != nullptr);
    CHECK(undef->is_undefined());

    const Symbol* absolute = o.find_symbol("@feat.00");
    REQUIRE(absolute != nullptr);
    CHECK(absolute->is_absolute());
    CHECK(absolute->section_number == kSectionAbsolute);
    CHECK(absolute->value == 0x80010u);
    CHECK(o.section_of(*absolute) == nullptr);

    // ".text" is both a section symbol and nothing else; find_symbol returns it.
    CHECK(o.find_symbol(".text") == o.symbol_at(0));
    CHECK(o.find_symbol("missing") == nullptr);
}

TEST_CASE("coff: typed aux records") {
    const Built b = build_object(basic_spec());
    const auto r = Object::parse(as_span(b.bytes));
    REQUIRE(r.has_value());
    const Object& o = *r;

    const auto def = o.section_definition(*o.symbol_at(0));
    REQUIRE(def.has_value());
    CHECK(def->length == 16u);
    CHECK(def->relocation_count == 2u);
    CHECK(def->checksum == 0xABCDu);
    CHECK(def->selection == ComdatSelection::Any);
    CHECK(def->selection_raw == 2u);
    CHECK_FALSE(o.section_definition(*o.symbol_at(2)).has_value());  // a function is not a section symbol

    const Symbol* weak = o.find_symbol("weak_ext");
    REQUIRE(weak != nullptr);
    CHECK(weak->is_weak_external());
    REQUIRE(weak->aux_count == 1u);
    const auto w = o.weak_external(*weak);
    REQUIRE(w.has_value());
    CHECK(w->tag_index == 6u);
    CHECK(w->characteristics == static_cast<std::uint32_t>(WeakCharacteristics::SearchAlias));
    CHECK(o.weak_external_default(*weak) == o.find_symbol("weak_default"));
    CHECK_FALSE(o.weak_external(*o.symbol_at(2)).has_value());

    const Symbol* file = o.find_symbol(".file");
    REQUIRE(file != nullptr);
    CHECK(file->section_number == kSectionDebug);
    const auto fname = o.file_name(*file);
    REQUIRE(fname.has_value());
    CHECK(*fname == "test.cpp");
    CHECK_FALSE(o.file_name(*o.symbol_at(2)).has_value());
}

TEST_CASE("coff: relocations resolve to target symbols") {
    const Built b = build_object(basic_spec());
    const auto r = Object::parse(as_span(b.bytes));
    REQUIRE(r.has_value());
    const Object& o = *r;

    const auto resolved = o.resolve_relocations(o.sections()[0]);
    REQUIRE(resolved.has_value());
    REQUIRE(resolved->size() == 2u);
    CHECK((*resolved)[0].reloc.virtual_address == 4u);
    CHECK((*resolved)[0].reloc.type == RelocType::Rel32);
    CHECK(is_rel32_family((*resolved)[0].reloc.type));
    CHECK((*resolved)[0].target->name == "undefined_target");
    CHECK((*resolved)[1].reloc.type == RelocType::Addr32Nb);
    CHECK_FALSE(is_rel32_family((*resolved)[1].reloc.type));
    CHECK((*resolved)[1].target->name == "?func@@YAXXZ");
    CHECK(reloc_type_name(RelocType::Addr32Nb) == "IMAGE_REL_AMD64_ADDR32NB");
    CHECK(reloc_type_name(static_cast<RelocType>(0x99)) == "IMAGE_REL_AMD64_UNKNOWN");

    CHECK(o.resolve_relocations(o.sections()[1])->empty());
}

TEST_CASE("coff: every AMD64 relocation type decodes") {
    ObjSpec spec = basic_spec();
    spec.sections[0].relocs.clear();
    for (std::uint16_t t = 0; t <= 0x10; ++t) {
        spec.sections[0].relocs.push_back({static_cast<std::uint32_t>(t) * 4u, 2, t});
    }
    spec.sections[0].relocs.push_back({0x40, 2, 0x7777});  // outside the enumeration: preserved raw
    const Built b = build_object(spec);
    const auto r = Object::parse(as_span(b.bytes));
    REQUIRE(r.has_value());
    const std::vector<Relocation> rel = r->relocations(r->sections()[0]);
    REQUIRE(rel.size() == 18u);
    for (std::uint16_t t = 0; t <= 0x10; ++t) {
        CHECK(rel[t].raw_type == t);
        CHECK(static_cast<std::uint16_t>(rel[t].type) == t);
        CHECK(reloc_type_name(rel[t].type) != "IMAGE_REL_AMD64_UNKNOWN");
    }
    CHECK(rel[17].raw_type == 0x7777u);
    CHECK(reloc_type_name(rel[17].type) == "IMAGE_REL_AMD64_UNKNOWN");
}

TEST_CASE("coff: COMDAT queries") {
    // sections: 1 text (comdat any, leader "leader"), 2 xdata (associative -> 1), 3 rdata (comdat
    // largest, leader), 4 plain .data.
    ObjSpec o;
    for (const char* n : {".text", ".xdata", ".rdata", ".data"}) {
        SectionSpec s;
        s.name = n;
        s.characteristics = scn::CntInitializedData | scn::MemRead | scn::LnkComdat;
        s.data = Bytes(4, 0);
        o.sections.push_back(s);
    }
    o.sections[3].characteristics = scn::CntInitializedData | scn::MemRead;

    SymbolSpec t = sym(".text", 1, 0, 3);
    t.aux.push_back(aux_section_def(false, 4, 0, 0, 0, 0, 2));
    o.symbols.push_back(t);                    // 0,1
    o.symbols.push_back(sym("leader", 1, 0));  // 2
    SymbolSpec x = sym(".xdata", 2, 0, 3);
    x.aux.push_back(aux_section_def(false, 4, 0, 0, 0, 1, 5));  // associative to section 1
    o.symbols.push_back(x);                                      // 3,4
    o.symbols.push_back(sym("$unwind$leader", 2, 0, 3));         // 5, a static label, not a leader
    SymbolSpec ro = sym(".rdata", 3, 0, 3);
    ro.aux.push_back(aux_section_def(false, 4, 0, 0, 0, 0, 6));
    o.symbols.push_back(ro);                       // 6,7
    o.symbols.push_back(sym("big_table", 3, 0));   // 8
    SymbolSpec d = sym(".data", 4, 0, 3);
    d.aux.push_back(aux_section_def(false, 4, 0, 0, 0, 0, 0));
    o.symbols.push_back(d);  // 9,10

    const Built b = build_object(o);
    const auto r = Object::parse(as_span(b.bytes));
    REQUIRE(r.has_value());
    const Object& obj = *r;

    const auto c1 = obj.comdat(obj.sections()[0]);
    REQUIRE(c1.has_value());
    CHECK(c1->selection == ComdatSelection::Any);
    REQUIRE(c1->leader != nullptr);
    CHECK(c1->leader->name == "leader");
    CHECK(c1->section_symbol == obj.symbol_at(0));

    const auto c2 = obj.comdat(obj.sections()[1]);
    REQUIRE(c2.has_value());
    CHECK(c2->selection == ComdatSelection::Associative);
    CHECK(c2->associated_section == 1u);
    CHECK(c2->leader == nullptr);

    const auto c3 = obj.comdat(obj.sections()[2]);
    REQUIRE(c3.has_value());
    CHECK(c3->selection == ComdatSelection::Largest);
    REQUIRE(c3->leader != nullptr);
    CHECK(c3->leader->name == "big_table");

    CHECK_FALSE(obj.comdat(obj.sections()[3]).has_value());

    const std::vector<const Section*> kids = obj.associative_children(obj.sections()[0]);
    REQUIRE(kids.size() == 1u);
    CHECK(kids[0] == &obj.sections()[1]);
    CHECK(obj.associative_children(obj.sections()[2]).empty());
}

TEST_CASE("coff: .drectve directives") {
    const Built b = build_object(basic_spec());
    const auto r = Object::parse(as_span(b.bytes));
    REQUIRE(r.has_value());
    const std::vector<Directive> d = r->directives();
    REQUIRE(d.size() == 5u);

    CHECK(d[0].kind == DirectiveKind::DefaultLib);
    CHECK(d[0].name == "defaultlib");
    CHECK(d[0].value == "LIBCMT");
    CHECK(d[0].text == "/DEFAULTLIB:\"LIBCMT\"");

    CHECK(d[1].kind == DirectiveKind::AlternateName);
    REQUIRE(d[1].has_pair);
    CHECK(d[1].lhs == "foo");
    CHECK(d[1].rhs == "bar");

    CHECK(d[2].kind == DirectiveKind::Export);
    CHECK(d[2].value == "baz");
    CHECK(d[3].kind == DirectiveKind::Include);
    CHECK(d[3].value == "_fltused");
    CHECK(d[4].kind == DirectiveKind::FailIfMismatch);
    REQUIRE(d[4].has_pair);
    CHECK(d[4].lhs == "_ITERATOR_DEBUG_LEVEL");
    CHECK(d[4].rhs == "0");
}

TEST_CASE("coff: directive tokenizer edge cases") {
    // UTF-8 BOM, tabs, newlines, trailing NULs, dash prefix, quoted value with a space.
    static const char kTail[] = "-export:\"a b\"\t/ENTRY:main\r\n/subsystem:console\0\0 junk";
    const std::string text = std::string("\xEF\xBB\xBF") + std::string(kTail, sizeof(kTail) - 1);
    const std::string_view view(text.data(), text.size());
    const std::vector<Directive> d = parse_directives(view);
    REQUIRE(d.size() == 4u);
    CHECK(d[0].kind == DirectiveKind::Export);
    CHECK(d[0].value == "a b");
    CHECK(d[1].kind == DirectiveKind::Entry);
    CHECK(d[1].value == "main");
    CHECK(d[2].kind == DirectiveKind::Subsystem);
    CHECK(d[3].kind == DirectiveKind::Other);
    CHECK(d[3].name == "junk");

    CHECK(parse_directives("").empty());
    CHECK(parse_directives("   \t\n").empty());
    const std::vector<Directive> unterminated = parse_directives("/DEFAULTLIB:\"abc");
    REQUIRE(unterminated.size() == 1u);
    CHECK(unterminated[0].kind == DirectiveKind::DefaultLib);
}

TEST_CASE("coff: /bigobj header, 32-bit section numbers and 32-bit aux numbers") {
    ObjSpec o;
    o.bigobj = true;
    constexpr std::uint32_t kSections = 70000;
    for (std::uint32_t i = 0; i < kSections; ++i) {
        SectionSpec s;
        s.name = ".text";
        s.characteristics = scn::CntCode | scn::MemExecute | scn::MemRead | scn::LnkComdat;
        s.data = Bytes{0xC3};
        o.sections.push_back(std::move(s));
    }
    // Section 69999: COMDAT any with leader. Section 70000: associative child of 69999.
    SymbolSpec d1 = sym(".text", 69999, 0, 3);
    d1.aux.push_back(aux_section_def(true, 1, 0, 0, 0, 0, 2));
    o.symbols.push_back(d1);
    o.symbols.push_back(sym("high_section_leader", 69999));
    SymbolSpec d2 = sym(".text", 70000, 0, 3);
    d2.aux.push_back(aux_section_def(true, 1, 0, 0, 0, 69999, 5));
    o.symbols.push_back(d2);
    o.symbols.push_back(sym("in_last_section", 70000, 0, 3));
    o.symbols.push_back(sym("undef", 0));
    o.sections[69999 - 1].relocs = {{0, 6, 4}};  // raw index 6 is "undef" (d2 occupies 3 and 4)

    const Built b = build_object(o);
    const auto r = Object::parse(as_span(b.bytes));
    REQUIRE(r.has_value());
    const Object& obj = *r;

    CHECK(obj.is_bigobj());
    CHECK(obj.header().header_size == 56u);
    CHECK(obj.header().symbol_record_size == 20u);
    CHECK(obj.header().bigobj_version == 2u);
    CHECK(obj.header().bigobj_class_id == kBigobjClassId);
    CHECK(obj.machine() == Machine::Amd64);
    CHECK(obj.header().section_count == kSections);
    CHECK(obj.sections().size() == static_cast<std::size_t>(kSections));
    CHECK(obj.header_bytes().size() == 56u);

    const Symbol* leader = obj.find_symbol("high_section_leader");
    REQUIRE(leader != nullptr);
    CHECK(leader->section_number == 69999);  // does not fit in 16 bits
    CHECK(obj.section_of(*leader) == obj.section(69999));
    const Symbol* last = obj.find_symbol("in_last_section");
    REQUIRE(last != nullptr);
    CHECK(last->section_number == 70000);

    const auto c1 = obj.comdat(*obj.section(69999));
    REQUIRE(c1.has_value());
    CHECK(c1->selection == ComdatSelection::Any);
    REQUIRE(c1->leader != nullptr);
    CHECK(c1->leader->name == "high_section_leader");
    const auto c2 = obj.comdat(*obj.section(70000));
    REQUIRE(c2.has_value());
    CHECK(c2->selection == ComdatSelection::Associative);
    CHECK(c2->associated_section == 69999u);  // needs NumberHighPart
    const auto kids = obj.associative_children(*obj.section(69999));
    REQUIRE(kids.size() == 1u);
    CHECK(kids[0]->index == 70000u);

    const auto rel = obj.resolve_relocations(*obj.section(69999));
    REQUIRE(rel.has_value());
    REQUIRE(rel->size() == 1u);
    CHECK((*rel)[0].target->name == "undef");
}

TEST_CASE("coff: extended relocation count (IMAGE_SCN_LNK_NRELOC_OVFL)") {
    ObjSpec o = basic_spec();
    constexpr std::uint32_t kCount = 70000;
    o.sections[0].relocs.clear();
    for (std::uint32_t i = 0; i < kCount; ++i) {
        o.sections[0].relocs.push_back({i, 3, 4});
    }
    const Built b = build_object(o);
    const auto r = Object::parse(as_span(b.bytes));
    REQUIRE(r.has_value());
    const Section& s = r->sections()[0];
    CHECK(s.has_extended_relocations);
    CHECK(s.raw_relocation_count == 0xFFFFu);
    CHECK(s.relocation_count == kCount);
    CHECK(s.relocation_table.size() == (static_cast<std::size_t>(kCount) + 1u) * 10u);
    CHECK(s.relocation_bytes.size() == static_cast<std::size_t>(kCount) * 10u);
    const std::vector<Relocation> rel = r->relocations(s);
    REQUIRE(rel.size() == static_cast<std::size_t>(kCount));
    CHECK(rel.front().virtual_address == 0u);
    CHECK(rel.back().virtual_address == kCount - 1u);
    CHECK(rel.back().symbol_index == 3u);

    // Flag set with a small real count is also legal and keeps the overflow record.
    ObjSpec small = basic_spec();
    small.sections[0].extended_relocs = true;
    const Built bs = build_object(small);
    const auto rs = Object::parse(as_span(bs.bytes));
    REQUIRE(rs.has_value());
    CHECK(rs->sections()[0].has_extended_relocations);
    CHECK(rs->sections()[0].relocation_count == 2u);
    CHECK(rs->relocations(rs->sections()[0]).size() == 2u);
}

// ---- malformed input -------------------------------------------------------------------------

namespace {

void expect_code(const Bytes& bytes, std::uint32_t code) {
    const auto r = Object::parse(as_span(bytes));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == code);
    CHECK_FALSE(r.error().message.empty());
    CHECK_FALSE(r.error().what_to_do.empty());
}

}  // namespace

TEST_CASE("coff malformed: truncated headers") {
    const Built b = build_object(basic_spec());
    expect_code(Bytes(), 1201);
    expect_code(Bytes(19, 0), 1201);
    Bytes bb = build_object(basic_spec(true)).bytes;
    bb.resize(55);
    expect_code(bb, 1201);  // bigobj signature but short header

    Bytes t = b.bytes;
    t.resize(b.header_size + 39);  // section table cut mid-header
    expect_code(t, 1202);
    t = b.bytes;
    t.resize(b.header_size + 5 * 40);  // section table intact, no symbol table
    expect_code(t, 1207);              // the symbol table offset points past the end
}

TEST_CASE("coff malformed: bigobj with a bad version or class id") {
    for (std::uint32_t version : {0u, 1u, 3u, 0xFFFFu}) {
        ObjSpec s = basic_spec(true);
        s.bigobj_version = static_cast<std::uint16_t>(version);
        const auto r = Object::parse(as_span(build_object(s).bytes));
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == 1203u);
        CHECK(r.error().message.find("version") != std::string::npos);
    }
    Bytes b = build_object(basic_spec(true)).bytes;
    b[12] = static_cast<std::uint8_t>(b[12] ^ 0xFFu);  // corrupt the class id
    expect_code(b, 1204);
}

TEST_CASE("coff malformed: section data and relocation ranges") {
    const Built b = build_object(basic_spec());
    Bytes t = b.bytes;
    set32(t, b.section_table_off + 20, static_cast<std::uint32_t>(t.size()) - 4);  // data pointer near EOF, size 16
    expect_code(t, 1205);

    t = b.bytes;
    set32(t, b.section_table_off + 20, 0xFFFFFFF0u);  // pointer + size would overflow 32 bits
    expect_code(t, 1205);

    t = b.bytes;
    set16(t, b.section_table_off + 32, 0xFF00);  // relocation count far past the end of the file
    expect_code(t, 1206);

    t = b.bytes;
    set32(t, b.section_table_off + 24, 0);  // relocations declared but no pointer
    expect_code(t, 1206);

    // LNK_NRELOC_OVFL with an overflow record outside the file.
    t = b.bytes;
    set16(t, b.section_table_off + 32, 0xFFFF);
    set32(t, b.section_table_off + 36, 0x01000000u | kTextChars | scn::LnkComdat);
    set32(t, b.section_table_off + 24, static_cast<std::uint32_t>(t.size()) - 3);
    expect_code(t, 1212);

    // ... and with a zero total count.
    t = b.bytes;
    set16(t, b.section_table_off + 32, 0xFFFF);
    set32(t, b.section_table_off + 36, 0x01000000u | kTextChars | scn::LnkComdat);
    set32(t, b.reloc_off[0], 0);
    expect_code(t, 1212);

    // ... and with a huge total count.
    t = b.bytes;
    set16(t, b.section_table_off + 32, 0xFFFF);
    set32(t, b.section_table_off + 36, 0x01000000u | kTextChars | scn::LnkComdat);
    set32(t, b.reloc_off[0], 0xFFFFFFFFu);
    expect_code(t, 1206);
}

TEST_CASE("coff malformed: symbol table and string table") {
    const Built b = build_object(basic_spec());

    Bytes t = b.bytes;
    set32(t, 12, 0x7FFFFFFFu);  // symbol count far past the file
    expect_code(t, 1207);

    t = b.bytes;
    set32(t, 12, 5);
    set32(t, 8, 0);  // count without a table
    expect_code(t, 1207);

    t = b.bytes;
    t.resize(b.symtab_off + 18 * 6 + 3);  // cut inside the symbol table
    expect_code(t, 1207);

    t = b.bytes;
    t.resize(b.strtab_off + 2);  // string table size field cut in half
    expect_code(t, 1208);

    t = b.bytes;
    set32(t, b.strtab_off, static_cast<std::uint32_t>(b.strtab_size) + 100);  // claims more than present
    expect_code(t, 1208);

    t = b.bytes;
    set32(t, b.strtab_off, 3);  // below the 4-byte minimum
    expect_code(t, 1208);

    // The long symbol name is the last symbol; its name offset is the second dword of its name field.
    const std::size_t long_sym = b.symbol_off.back();
    t = b.bytes;
    set32(t, long_sym + 4, 0x7FFFFFFFu);
    expect_code(t, 1209);

    t = b.bytes;
    set32(t, long_sym + 4, 2);  // inside the size field
    expect_code(t, 1209);

    t = b.bytes;
    set32(t, long_sym + 4, static_cast<std::uint32_t>(b.strtab_size));  // one past the table
    expect_code(t, 1209);

    t = b.bytes;
    t[t.size() - 1] = static_cast<std::uint8_t>('x');  // the last string loses its terminator
    expect_code(t, 1209);
}

TEST_CASE("coff malformed: aux records, section references, long section names") {
    const Built b = build_object(basic_spec());

    Bytes t = b.bytes;
    t[b.symbol_off.back() + 17] = 9;  // last symbol claims 9 aux records
    expect_code(t, 1210);

    t = b.bytes;
    set16(t, b.symbol_off[1] + 12, 99);  // ?func@@YAXXZ lives in section 99 of 5
    expect_code(t, 1213);

    t = b.bytes;
    set16(t, b.symbol_off[1] + 12, 0xFFF0);  // -16
    expect_code(t, 1213);

    // Long section name "/NNN" that points outside the string table.
    t = b.bytes;
    const std::size_t name_off = b.section_table_off + 3 * 40;
    const char bad[] = "/9999999";
    for (std::size_t i = 0; i < 8; ++i) {
        t[name_off + i] = static_cast<std::uint8_t>(bad[i]);
    }
    expect_code(t, 1211);
    t = b.bytes;
    const char junk[] = "/12ab";
    for (std::size_t i = 0; i < 6; ++i) {
        t[name_off + i] = static_cast<std::uint8_t>(junk[i]);
    }
    expect_code(t, 1211);
}

TEST_CASE("coff malformed: dangling relocation targets are reported on resolve") {
    ObjSpec spec = basic_spec();
    spec.sections[0].relocs = {{0, 1, 4}};  // symbol index 1 is the aux record of .text
    Built b = build_object(spec);
    auto r = Object::parse(as_span(b.bytes));
    REQUIRE(r.has_value());
    auto resolved = r->resolve_relocations(r->sections()[0]);
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().code == 1215u);
    CHECK(r->relocations(r->sections()[0]).size() == 1u);  // the raw list is still available

    spec.sections[0].relocs = {{0, 500, 4}};
    b = build_object(spec);
    r = Object::parse(as_span(b.bytes));
    REQUIRE(r.has_value());
    resolved = r->resolve_relocations(r->sections()[0]);
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().code == 1214u);
}

TEST_CASE("coff malformed: missing file") {
    const auto r = load_object_file("/definitely/not/here.obj");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == 1200u);
    CHECK(r.error().message.find("here.obj") != std::string::npos);
}

TEST_CASE("coff robustness: every truncation and every byte flip is handled") {
    for (bool big : {false, true}) {
        ObjSpec spec = basic_spec(big);
        spec.sections[0].extended_relocs = true;
        const Built b = build_object(spec);
        // Sanity: the pristine object parses.
        REQUIRE(Object::parse(as_span(b.bytes)).has_value());

        for (std::size_t len = 0; len < b.bytes.size(); ++len) {
            const Bytes t(b.bytes.begin(), b.bytes.begin() + static_cast<std::ptrdiff_t>(len));
            const auto r = Object::parse(as_span(t));
            if (r.has_value()) {
                exercise(*r);
            } else {
                CHECK(r.error().valid());
            }
        }
        for (std::size_t pos = 0; pos < b.bytes.size(); ++pos) {
            for (std::uint8_t mask : {std::uint8_t{0xFF}, std::uint8_t{0x01}, std::uint8_t{0x80}}) {
                Bytes t = b.bytes;
                t[pos] = static_cast<std::uint8_t>(t[pos] ^ mask);
                const auto r = Object::parse(as_span(t));
                if (r.has_value()) {
                    exercise(*r);
                } else {
                    CHECK(r.error().valid());
                }
            }
        }
    }
}
