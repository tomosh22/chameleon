// WP-07: from the plan's (section, offset, kind, S, A) to the (rva, kind, value) triples the agent
// applies - the pure function, its range checks, and the layout lookup in the patch PDB.
#include <cstdint>
#include <string>

#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_plan.h"
#include "doctest.h"
#include "synth.h"
#include "worlds.h"

using namespace cml_bind_test;
using namespace cml::core;
using namespace cml::core::bind;

namespace {

constexpr std::uint64_t kPatchBase = 0x180000000ull;

struct Scenario {
    SynthWorld world = basic_world();
    Built built;
    BindPlan plan;
    std::uint32_t text_section = 0;
    std::uint32_t data_section = 0;

    Scenario() {
        SynthObj o;
        auto f = o.function("?compute_value@@YAHXZ", true, 0x40);
        text_section = f.section;
        const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
        const std::uint32_t note = o.undefined("fixture_note", true);
        data_section = o.section(".data", kData, 0x10);
        o.reloc(f.section, 3, tick, kRel32_1, 0);     // cmp dword ptr [g_tick], imm8   (cl)
        o.reloc(f.section, 0x10, tick, kRel32, -1);   // the same access as clang emits it
        o.reloc(f.section, 0x20, tick, coff::RelocType::Rel32_4, 0);
        o.reloc(f.section, 0x30, note, kRel32);       // a call: direct to the old function, the thunk is the fallback
        o.reloc(data_section, 8, note, kAddr64, 0);
        o.reloc(data_section, 0, tick, kAddr64, 0x20);
        built = o.build();
        auto r = plan_bind(world.input(built));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        plan = std::move(*r);
    }

    PatchLayout layout() const {
        PatchLayout l;
        l.patch_base = kPatchBase;
        l.section_rva[text_section] = 0x5000;
        l.section_rva[data_section] = 0x7000;
        l.symbol_rva["__cml_slot_fixture_note"] = 0x7100;
        return l;
    }
};

const FixupTriple* at(const std::vector<FixupTriple>& v, std::uint32_t rva) {
    for (const FixupTriple& t : v) {
        if (t.rva == rva) {
            return &t;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("fix-up triples: REL32 family, ADDR64 and slot writes follow target = S + A, value = target - (site + 4 + N)") {
    Scenario s;
    auto t = make_fixup_triples(s.plan, s.layout());
    REQUIRE_MESSAGE(t.has_value(), err(t));
    REQUIRE(t->size() == 7);
    const std::uint64_t g_tick = SynthWorld::kBase + 0x3080;
    const std::uint64_t note = SynthWorld::kBase + 0x1100;

    const FixupTriple* a = at(*t, 0x5003);  // REL32_1, A = 0
    REQUIRE(a != nullptr);
    CHECK(a->kind == FixupKind::Rel32_1);
    CHECK(a->value == static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int64_t>(g_tick + 0 - (kPatchBase + 0x5003 + 4 + 1)))));

    const FixupTriple* b = at(*t, 0x5010);  // REL32, A = -1 (clang)
    REQUIRE(b != nullptr);
    CHECK(b->kind == FixupKind::Rel32);
    CHECK(b->value == static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int64_t>(g_tick - 1 - (kPatchBase + 0x5010 + 4)))));

    // cl's REL32_1 with A = 0 and clang's REL32 with A = -1 address the SAME byte: the byte after the
    // 4-byte field plus one is what the instruction ends at, and S + A - (site + 4 + N) agrees.
    const std::int64_t disp_cl = static_cast<std::int32_t>(a->value) + static_cast<std::int64_t>(kPatchBase + 0x5003 + 4 + 1);
    const std::int64_t disp_clang = static_cast<std::int32_t>(b->value) + static_cast<std::int64_t>(kPatchBase + 0x5010 + 4);
    CHECK(disp_cl == static_cast<std::int64_t>(g_tick));
    CHECK(disp_clang == static_cast<std::int64_t>(g_tick) - 1);

    const FixupTriple* c = at(*t, 0x5020);  // REL32_4
    REQUIRE(c != nullptr);
    CHECK(c->kind == FixupKind::Rel32_4);
    CHECK(static_cast<std::int32_t>(c->value) == static_cast<std::int32_t>(g_tick - (kPatchBase + 0x5020 + 4 + 4)));

    const FixupTriple* d = at(*t, 0x7000);  // ADDR64 with in-place addend 0x20
    REQUIRE(d != nullptr);
    CHECK(d->kind == FixupKind::Addr64);
    CHECK(d->value == g_tick + 0x20);

    const FixupTriple* e = at(*t, 0x7008);  // &fixture_note stored in data: the real address
    REQUIRE(e != nullptr);
    CHECK(e->kind == FixupKind::Addr64);
    CHECK(e->value == note);

    const FixupTriple* f = at(*t, 0x7100);  // the thunk's slot
    REQUIRE(f != nullptr);
    CHECK(f->kind == FixupKind::Slot);
    CHECK(f->value == note);

    for (std::size_t i = 1; i < t->size(); ++i) {
        CHECK((*t)[i - 1].rva < (*t)[i].rva);
    }
}

TEST_CASE("fix-up triples: a REL32 that does not fit 32 bits fails the patch, nothing is written") {
    Scenario s;
    PatchLayout l = s.layout();
    l.patch_base = 0x7FF000000000ull;  // far away from the old module (0x140000000)
    auto t = make_fixup_triples(s.plan, l);
    REQUIRE(!t.has_value());
    CHECK(t.error().code == diag::kRel32OutOfRange);
    CHECK(t.error().message.find("?g_tick@@3HA") != std::string::npos);
    CHECK(t.error().message.find("REL32") != std::string::npos);

    // The boundary: exactly at +2 GiB - 1 passes, one more fails.
    BindPlan p;
    Fixup f;
    f.section_index = 1;
    f.offset = 0;
    f.kind = FixupKind::Rel32;
    f.addend = 0;
    f.symbol = "x";
    p.fixups.push_back(f);
    PatchLayout pl;
    pl.patch_base = 0x100000000ull;
    pl.section_rva[1] = 0x1000;
    const std::uint64_t site = pl.patch_base + 0x1000;
    p.fixups[0].target_va = site + 4 + 0x7FFFFFFFull;
    CHECK(make_fixup_triples(p, pl).has_value());
    p.fixups[0].target_va = site + 4 + 0x80000000ull;
    CHECK_FALSE(make_fixup_triples(p, pl).has_value());
    p.fixups[0].target_va = site + 4 - 0x80000000ull;  // -2 GiB exactly fits
    auto neg = make_fixup_triples(p, pl);
    REQUIRE_MESSAGE(neg.has_value(), err(neg));
    CHECK((*neg)[0].value == 0x80000000u);
    p.fixups[0].target_va = site + 4 - 0x80000001ull;
    CHECK_FALSE(make_fixup_triples(p, pl).has_value());
}

TEST_CASE("fix-up triples: an incomplete layout is a diagnostic, never a guess") {
    Scenario s;
    PatchLayout l = s.layout();
    l.section_rva.erase(s.data_section);
    auto t = make_fixup_triples(s.plan, l);
    REQUIRE(!t.has_value());
    CHECK(t.error().code == diag::kNoLocator);
    CHECK(t.error().message.find("?g_tick@@3HA") != std::string::npos);

    PatchLayout l2 = s.layout();
    l2.symbol_rva.clear();
    auto t2 = make_fixup_triples(s.plan, l2);
    REQUIRE(!t2.has_value());
    CHECK(t2.error().code == diag::kLayoutIncomplete);
    CHECK(t2.error().message.find("__cml_slot_fixture_note") != std::string::npos);
}

TEST_CASE("fix-up triples: JSON form and an empty plan") {
    Scenario s;
    auto t = make_fixup_triples(s.plan, s.layout());
    REQUIRE_MESSAGE(t.has_value(), err(t));
    const std::string json = to_json(*t);
    CHECK(json.find("\"kind\": \"REL32_1\"") != std::string::npos);
    CHECK(json.find("\"kind\": \"SLOT\"") != std::string::npos);
    BindPlan empty;
    auto none = make_fixup_triples(empty, PatchLayout{});
    REQUIRE(none.has_value());
    CHECK(none->empty());
    CHECK(to_json(*none) == "[]\n");
}

TEST_CASE("layout_from_pdb: section start = locator RVA - locator offset; slots are publics") {
    Scenario s;
    // The patch PDB: an anchor for the .data section, the COMDAT leader of the function section (leader
    // value 0x10 inside its section), the slot of fixture_note.
    tp::Spec spec;
    spec.sections = {{".text", 0x1000, 0x1000, 0x1000, 0x60000020}, {".data", 0x1000, 0x2000, 0x1000, 0xC0000040}};
    spec.symrec.pub32("?compute_value@@YAHXZ", 0x3, 1, 0x210);
    spec.symrec.pub32("__cml_anchor_0123456789abcdef_" + std::to_string(s.data_section), 0, 2, 0x300);
    spec.symrec.pub32("__cml_slot_fixture_note", 0, 2, 0x40);
    tp::Module m;
    m.name = "patch.obj";
    m.symbols.objname(m.name);
    spec.modules.push_back(std::move(m));
    tp::TempDir dir;
    tp::write_file(dir.file("patch.pdb"), tp::build(spec));
    cml::pdb::OpenOptions oo;
    oo.use_cache = false;
    auto pdb = cml::pdb::PdbIndex::open_pdb(dir.file("patch.pdb"), oo);
    REQUIRE_MESSAGE(pdb.has_value(), err(pdb));

    std::vector<coff::SectionLocator> locators(2);
    locators[s.text_section - 1].section_index = s.text_section;
    locators[s.text_section - 1].kind = coff::LocatorKind::ComdatLeader;
    locators[s.text_section - 1].symbol_name = "?compute_value@@YAHXZ";
    locators[s.text_section - 1].offset = 0x10;
    locators[s.data_section - 1].section_index = s.data_section;
    locators[s.data_section - 1].kind = coff::LocatorKind::Anchor;
    locators[s.data_section - 1].symbol_name = "__cml_anchor_0123456789abcdef_" + std::to_string(s.data_section);
    auto l = layout_from_pdb(s.plan, locators, **pdb, kPatchBase);
    REQUIRE_MESSAGE(l.has_value(), err(l));
    CHECK(l->patch_base == kPatchBase);
    CHECK(l->section_rva.at(s.text_section) == 0x1210 - 0x10);
    CHECK(l->section_rva.at(s.data_section) == 0x2300);
    CHECK(l->symbol_rva.at("__cml_slot_fixture_note") == 0x2040);

    // Together with make_fixup_triples: the full path (S + A, site from the locators).
    auto t = make_fixup_triples(s.plan, *l);
    REQUIRE_MESSAGE(t.has_value(), err(t));
    CHECK(at(*t, 0x1203) != nullptr);
    CHECK(at(*t, 0x2300) != nullptr);
    CHECK(at(*t, 0x2040) != nullptr);

    // A section without locator that needs a fix-up.
    std::vector<coff::SectionLocator> bad = locators;
    bad[s.data_section - 1].kind = coff::LocatorKind::None;
    bad[s.data_section - 1].reason = coff::NoLocatorReason::Associative;
    auto lb = layout_from_pdb(s.plan, bad, **pdb, kPatchBase);
    REQUIRE(!lb.has_value());
    CHECK(lb.error().code == diag::kNoLocator);

    // A locator that is not a public of the patch PDB.
    std::vector<coff::SectionLocator> missing = locators;
    missing[s.data_section - 1].symbol_name = "__cml_anchor_nothere_2";
    auto lm = layout_from_pdb(s.plan, missing, **pdb, kPatchBase);
    REQUIRE(!lm.has_value());
    CHECK(lm.error().code == diag::kLayoutIncomplete);
    CHECK(lm.error().message.find("__cml_anchor_nothere_2") != std::string::npos);
}
