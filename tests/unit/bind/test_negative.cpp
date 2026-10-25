// WP-07 negative tests: every unsupported situation is a diagnostic naming the symbol and the
// referencing function, never a silently wrong plan.
#include <string>

#include "cml/coff/anchors.h"
#include "cml/coff/writer.h"
#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_plan.h"
#include "cml/core/symbol_registry.h"
#include "doctest.h"
#include "synth.h"
#include "worlds.h"

using namespace cml_bind_test;
using namespace cml::core;
using namespace cml::core::bind;

namespace {

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

const cml::Diag* with_code(const std::vector<cml::Diag>& v, std::uint32_t code) {
    for (const cml::Diag& d : v) {
        if (d.code == code) {
            return &d;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("ADDR32NB to old data (undefined or defined here) is CML3101; ADDR32NB to a thunk is fine") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ");
    const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
    const std::uint32_t note = o.undefined("fixture_note", true);
    const std::uint32_t total_sec = o.section(".data", kData, 8);
    const std::uint32_t total = o.symbol("?total@Accum@@2HA", 0, static_cast<std::int32_t>(total_sec), false);
    const std::uint32_t xdata = o.section(".xdata", kRdata, 0x10);
    o.reloc(xdata, 0, tick, kAddr32Nb);
    o.reloc(xdata, 4, note, kAddr32Nb);   // legal: through the thunk
    o.reloc(xdata, 8, total, kAddr32Nb);
    (void)f;
    Built b = o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(w.input(b), &errors);
    REQUIRE(!r.has_value());
    REQUIRE(errors.size() == 2);
    for (const cml::Diag& d : errors) {
        CHECK(d.code == diag::kAddr32NbToOldData);
        CHECK(d.id() == "CML3101");
        CHECK(contains(d.message, "ADDR32NB"));
        CHECK_FALSE(d.what_to_do.empty());
    }
    CHECK(contains(errors[0].message, "?g_tick@@3HA"));
    CHECK(contains(errors[1].message, "?total@Accum@@2HA"));
    // The .xdata section is not the function: the message names the section when nothing better exists.
    CHECK(contains(errors[0].message, "xdata"));
}

TEST_CASE("ADDR32NB inside an associative .xdata names the function that owns it") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ");
    const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
    const std::uint32_t xdata = o.section(".xdata", kRdata | coff::scn::LnkComdat, 0x10, coff::ComdatSelection::Associative, f.section);
    o.reloc(xdata, 0, tick, kAddr32Nb);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(w.input(b), &errors);
    REQUIRE(!r.has_value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kAddr32NbToOldData);
    CHECK(contains(errors[0].message, "compute_value"));
    CHECK(contains(errors[0].message, "?g_tick@@3HA"));
    CHECK(contains(errors[0].message, "old.exe"));
}

TEST_CASE("an unsupported relocation kind against a bound symbol is CML3202") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x20);
    const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
    const std::uint32_t note = o.undefined("fixture_note", true);
    o.reloc(f.section, 0, tick, coff::RelocType::Addr32);        // 32-bit absolute address
    o.reloc(f.section, 8, note, coff::RelocType::Addr32);
    o.reloc(f.section, 0x10, tick, coff::RelocType::SRel32);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(w.input(b), &errors);
    REQUIRE(!r.has_value());
    REQUIRE(errors.size() == 3);
    for (const cml::Diag& d : errors) {
        CHECK(d.code == diag::kUnsupportedReloc);
        CHECK(contains(d.message, "compute_value"));
    }
    CHECK(contains(errors[0].message, "IMAGE_REL_AMD64_ADDR32"));
    CHECK(contains(errors[0].message, "?g_tick@@3HA"));
    CHECK(contains(errors[1].message, "fixture_note"));
    CHECK(contains(errors[2].message, "SREL32"));
}

TEST_CASE("the same relocation kinds against patch-local symbols are not our business") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x20);
    auto g = o.function("?local_only@@YAHXZ", true, 0x10);
    o.reloc(f.section, 0, g.symbol, coff::RelocType::Addr32);
    o.reloc(f.section, 8, g.symbol, coff::RelocType::SecRel);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(r->fixups.empty());
}

TEST_CASE("SECREL against module data is retargeted (the CRT's thread-safe-static epoch); other TLS cases are CML3102") {
    SynthWorld w = basic_world();
    {
        SynthObj o;
        auto f = o.function("?compute_value@@YAHXZ", true, 0x20);
        const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
        o.reloc(f.section, 0, tick, coff::RelocType::SecRel, 4);
        Built b = o.build();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        REQUIRE(r->fixups.size() == 1);
        const Fixup& fx = r->fixups[0];
        CHECK(fx.kind == FixupKind::SecRel32);
        CHECK(fx.target_section_offset == 0x80);  // 0x3080 inside .data (rva 0x3000)
        CHECK(fx.addend == 4);
        PatchLayout l;
        l.patch_base = 0x150000000ull;
        l.section_rva[f.section] = 0x1000;
        auto t = make_fixup_triples(*r, l);
        REQUIRE_MESSAGE(t.has_value(), err(t));
        REQUIRE(t->size() == 1);
        CHECK((*t)[0].kind == FixupKind::SecRel32);
        CHECK((*t)[0].rva == 0x1000);
        CHECK((*t)[0].value == 0x84);
    }
    {
        // SECREL against a symbol that is not in the patched module (registry): cannot be computed.
        InMemorySymbolRegistry reg;
        reg.add("?patch_tls@@3HA", {0x7FF000002000ull, false, "patch 1"});
        w.registry = &reg;
        SynthObj o;
        auto f = o.function("?compute_value@@YAHXZ", true, 0x20);
        const std::uint32_t t = o.undefined("?patch_tls@@3HA", false);
        o.reloc(f.section, 0, t, coff::RelocType::SecRel);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        auto r = plan_bind(w.input(b), &errors);
        REQUIRE(!r.has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kThreadLocal);
        w.registry = nullptr;
    }
    {
        SynthObj o;
        auto f = o.function("?compute_value@@YAHXZ", true, 0x20);
        const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
        o.reloc(f.section, 8, tick, coff::RelocType::Section);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        auto r = plan_bind(w.input(b), &errors);
        REQUIRE(!r.has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kThreadLocal);
        CHECK(errors[0].id() == "CML3102");
        CHECK(contains(errors[0].message, "?g_tick@@3HA"));
    }
    {
        SynthObj o;
        o.function("?compute_value@@YAHXZ");
        const std::uint32_t tls = o.section(".tls$", kData, 8);
        o.symbol("?t_counter@@3HA", 0, static_cast<std::int32_t>(tls), false);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        auto r = plan_bind(w.input(b), &errors);
        REQUIRE(!r.has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kThreadLocal);
        CHECK(contains(errors[0].message, "t_counter"));
    }
}

TEST_CASE("a fix-up inside an associative COMDAT (cl's .text$x funclets) asks for a locator instead of failing") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ");
    const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
    const std::uint32_t funclet = o.section(".text$x", kTextComdat, 0x20, coff::ComdatSelection::Associative, f.section);
    o.symbol("?dtor$0@?0??compute_value@@YAHXZ@4HA", 0, static_cast<std::int32_t>(funclet), true, false);
    o.reloc(funclet, 8, tick, kRel32);
    o.reloc(f.section, 3, tick, kRel32);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(find(*r, "?dtor$0@?0??compute_value@@YAHXZ@4HA")->decision == DecisionKind::PatchFresh);
    REQUIRE(r->fixups.size() == 2);
    CHECK(r->locator_sections == std::vector<std::uint32_t>({f.section, funclet}));
    CHECK(r->associative_locator_sections == std::vector<std::uint32_t>({funclet}));

    // Without the request the section has no locator (ADR-0024) ...
    auto model = coff::ObjectModel::from_object(b.obj());
    REQUIRE_MESSAGE(model.has_value(), err(model));
    coff::AnchorOptions plain;
    plain.tag = "t1";
    auto none = coff::add_anchors(*model, plain);
    REQUIRE_MESSAGE(none.has_value(), err(none));
    CHECK((*none)[funclet - 1].kind == coff::LocatorKind::None);
    // ... with it, the funclet is located like any COMDAT (its leader is static: an anchor).
    auto model2 = coff::ObjectModel::from_object(b.obj());
    REQUIRE_MESSAGE(model2.has_value(), err(model2));
    coff::AnchorOptions asked;
    asked.tag = "t2";
    asked.anchor_associative = r->associative_locator_sections;
    auto located = coff::add_anchors(*model2, asked);
    REQUIRE_MESSAGE(located.has_value(), err(located));
    CHECK((*located)[funclet - 1].kind == coff::LocatorKind::Anchor);
    CHECK((*located)[funclet - 1].symbol_name == "__cml_anchor_t2_" + std::to_string(funclet));

    // A layout without that section fails loudly in make_fixup_triples.
    PatchLayout l;
    l.patch_base = 0x150000000ull;
    l.section_rva[f.section] = 0x1000;
    auto t = make_fixup_triples(*r, l);
    REQUIRE(!t.has_value());
    CHECK(t.error().code == diag::kNoLocator);
}

TEST_CASE("relocations in debug sections are never retargeted and never diagnosed") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ");
    const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
    const std::uint32_t dbg = o.section(".debug$S", coff::scn::CntInitializedData | coff::scn::MemRead | coff::scn::MemDiscardable, 0x20);
    o.reloc(dbg, 4, tick, coff::RelocType::SecRel);
    o.reloc(dbg, 8, tick, coff::RelocType::Section);
    o.reloc(f.section, 3, tick, kRel32);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(r->fixups.size() == 1);
}

TEST_CASE("a relocation outside its section data is CML3208") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x20);
    const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
    o.reloc(f.section, 0x18, tick, kRel32);
    o.model().sections[f.section - 1].data.resize(0x1A);  // the site now ends beyond the data
    Built b = o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(w.input(b), &errors);
    REQUIRE(!r.has_value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kBadRelocationSite);
}

TEST_CASE("only x64 objects are planned (CML3203)") {
    SynthWorld w = basic_world();
    cml::coff::ObjectModel m = cml::coff::ObjectModel::make_empty(cml::coff::Machine::I386);
    auto bytes = cml::coff::write(m);
    REQUIRE_MESSAGE(bytes.has_value(), err(bytes));
    std::vector<std::byte> v(bytes->begin(), bytes->end());
    auto obj = cml::coff::Object::parse(std::span<const std::byte>(v.data(), v.size()));
    REQUIRE_MESSAGE(obj.has_value(), err(obj));
    SynthObj any;
    any.function("?compute_value@@YAHXZ");
    Built placeholder = any.build();
    PlanInput in = w.input(placeholder);
    in.object = &*obj;
    auto r = plan_bind(in);
    REQUIRE(!r.has_value());
    CHECK(r.error().code == diag::kUnsupportedObject);
}

TEST_CASE("errors are collected: unresolved symbols and unsupported kinds are all reported") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x30);
    const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
    const std::uint32_t gone = o.undefined("?gone@@3HA", false);
    const std::uint32_t xdata = o.section(".xdata", kRdata, 8);
    o.reloc(f.section, 0, gone, kRel32);
    o.reloc(f.section, 8, tick, coff::RelocType::Addr32);
    o.reloc(xdata, 0, tick, kAddr32Nb);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(w.input(b), &errors);
    REQUIRE(!r.has_value());
    CHECK(errors.size() == 3);
    CHECK(with_code(errors, diag::kUnresolved) != nullptr);
    CHECK(with_code(errors, diag::kUnsupportedReloc) != nullptr);
    CHECK(with_code(errors, diag::kAddr32NbToOldData) != nullptr);
    CHECK(r.error().code == errors[0].code);
    for (const cml::Diag& d : errors) {
        CHECK(d.valid());
        CHECK(d.category() == cml::DiagCategory::Link);
    }
}
