// WP-07: every row of the plan 4.5 table, on synthetic objects (WP-06 writer) planned against a
// synthetic old module (WP-04 PDB builder). No compiler is involved, so every expectation is exact.
#include <string>

#include "cml/coff/binding.h"
#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_plan.h"
#include "doctest.h"
#include "synth.h"
#include "worlds.h"

using namespace cml_bind_test;
using namespace cml::core;
using namespace cml::core::bind;


TEST_CASE("row 1: a defined function present in the old compiland is hooked") {
    SynthWorld w = basic_world();
    SynthObj o;
    o.function("?compute_value@@YAHXZ");
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* d = find(*r, "?compute_value@@YAHXZ");
    REQUIRE(d != nullptr);
    CHECK(d->decision == DecisionKind::PatchHooked);
    CHECK(d->type == SymbolType::Function);
    CHECK(d->defined);
    CHECK(d->source == BindSource::PdbPublic);
    CHECK(d->old_address.rva == 0x1010);
    CHECK(d->old_address.va == SynthWorld::kBase + 0x1010);
    CHECK(d->display == "compute_value");
    REQUIRE(r->hooks.size() == 1);
    CHECK(r->hooks[0].new_symbol == "?compute_value@@YAHXZ");
    CHECK(r->hooks[0].old_rva == 0x1010);
    CHECK(r->hooks[0].old_size == 0x20);
    CHECK(r->hooks[0].module == "old.exe");
    CHECK(r->thunks.empty());
    CHECK(r->placeholders.empty());
    CHECK(r->fixups.empty());
    CHECK(r->new_functions.empty());
    CHECK(r->compiland_index == 0);
}

TEST_CASE("row 2: a defined function that is not in the old image is a new canonical symbol, not hooked") {
    SynthWorld w = basic_world();
    SynthObj o;
    o.function("?new_helper@@YAHXZ");
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* d = find(*r, "?new_helper@@YAHXZ");
    REQUIRE(d != nullptr);
    CHECK(d->decision == DecisionKind::PatchNewFunction);
    CHECK(r->hooks.empty());
    REQUIRE(r->new_functions.size() == 1);
    CHECK(r->new_functions[0].name == "?new_helper@@YAHXZ");
    CHECK(r->new_functions[0].display == "new_helper");
    CHECK_FALSE(r->new_functions[0].is_static);
}

TEST_CASE("row 3: defined external data present in the old image is bound to it, references retargeted") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ");
    const std::uint32_t data = o.section(".data", kData, 0x10);
    const std::uint32_t tick = o.symbol("?g_tick@@3HA", 0, static_cast<std::int32_t>(data), false);
    o.reloc(f.section, 3, tick, kRel32_1, 0);       // cl style: REL32_1, addend 0
    o.reloc(f.section, 0xA, tick, kRel32, -1);      // clang style: REL32 with addend -1
    o.reloc(data, 8, tick, kAddr64, 0x10);          // a pointer table entry g_tick + 16
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* d = find(*r, "?g_tick@@3HA");
    REQUIRE(d != nullptr);
    CHECK(d->decision == DecisionKind::BindOldData);
    CHECK(d->defined);
    CHECK(d->old_address.rva == 0x3080);
    CHECK(d->source == BindSource::PdbPublic);
    CHECK(r->placeholders.empty());  // the object's own definition satisfies the linker
    CHECK(r->thunks.empty());
    REQUIRE(r->fixups.size() == 3);
    CHECK(has_fixup(*r, FixupKind::Rel32_1, f.section, 3, "?g_tick@@3HA"));
    CHECK(has_fixup(*r, FixupKind::Rel32, f.section, 0xA, "?g_tick@@3HA"));
    CHECK(has_fixup(*r, FixupKind::Addr64, data, 8, "?g_tick@@3HA"));
    for (const Fixup& fx : r->fixups) {
        CHECK(fx.target_va == SynthWorld::kBase + 0x3080);
        if (fx.kind == FixupKind::Rel32) {
            CHECK(fx.addend == -1);
        }
        if (fx.kind == FixupKind::Addr64) {
            CHECK(fx.addend == 0x10);
        }
    }
    CHECK(r->summary().bind_old_data == 1);
}

TEST_CASE("row 4: defined data not in the old image is new; its cl initialiser (.CRT$XCU) runs once") {
    SynthWorld w = basic_world();
    SynthObj o;
    const std::uint32_t data = o.section(".data", kData, 0x20);
    o.symbol("?g_tracker@?A0x1@@3UTracker@@A", 0, static_cast<std::int32_t>(data), false, false);
    auto init = o.function("??__Eg_tracker@?A0x1@@YAXXZ", false);
    auto fini = o.function("??__Fg_tracker@?A0x1@@YAXXZ", false);
    const std::uint32_t xcu = o.section(".CRT$XCU", kRdata, 8);
    o.reloc(xcu, 0, init.symbol, kAddr64);
    (void)fini;
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* d = find(*r, "?g_tracker@?A0x1@@3UTracker@@A");
    REQUIRE(d != nullptr);
    CHECK(d->decision == DecisionKind::PatchNewData);
    CHECK(d->is_static);
    CHECK(find(*r, "??__Eg_tracker@?A0x1@@YAXXZ")->decision == DecisionKind::PatchFresh);
    CHECK(find(*r, "??__Fg_tracker@?A0x1@@YAXXZ")->decision == DecisionKind::PatchFresh);
    CHECK(r->hooks.empty());
    REQUIRE(r->new_globals.size() == 1);
    CHECK(r->new_globals[0].display == "(anonymous namespace)::g_tracker");
    REQUIRE(r->new_globals[0].initialisers.size() == 1);
    CHECK(r->new_globals[0].initialisers[0] == "??__Eg_tracker@?A0x1@@YAXXZ");
    REQUIRE(r->initialisers.size() == 1);
    CHECK(r->initialisers[0].function == "??__Eg_tracker@?A0x1@@YAXXZ");
    CHECK(r->initialisers[0].table_section_name == ".CRT$XCU");
    CHECK(r->initialisers[0].table_section == xcu);
    CHECK(r->initialisers[0].table_offset == 0);
    CHECK_FALSE(r->initialisers[0].tu_wide);
    CHECK(r->warnings.empty());
}

TEST_CASE("row 4b: the initialiser of a global that is bound to the old image is not run") {
    SynthWorld w = basic_world(false);
    w.spec.modules[0].symbols.data(0x110C, "`anonymous namespace'::s_old", SynthWorld::kDataSec, 0x40);
    w.open();
    SynthObj o;
    const std::uint32_t data = o.section(".data", kData, 0x60);
    o.symbol("?s_old@?A0x1@@3UObj@@A", 0x40, static_cast<std::int32_t>(data), false, false);
    o.symbol("?s_new@?A0x1@@3UObj@@A", 0x50, static_cast<std::int32_t>(data), false, false);
    auto init_old = o.function("??__Es_old@?A0x1@@YAXXZ", false);
    auto init_new = o.function("??__Es_new@?A0x1@@YAXXZ", false);
    const std::uint32_t xcu = o.section(".CRT$XCU", kRdata, 16);
    o.reloc(xcu, 0, init_old.symbol, kAddr64);
    o.reloc(xcu, 8, init_new.symbol, kAddr64);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(find(*r, "?s_old@?A0x1@@3UObj@@A")->decision == DecisionKind::BindOldData);
    CHECK(find(*r, "?s_new@?A0x1@@3UObj@@A")->decision == DecisionKind::PatchNewData);
    REQUIRE(r->initialisers.size() == 1);
    CHECK(r->initialisers[0].function == "??__Es_new@?A0x1@@YAXXZ");
    CHECK(r->initialisers[0].table_offset == 8);
}

TEST_CASE("row 4c: clang's TU-wide initialiser runs when the TU has new globals; shared with bound data it warns") {
    SynthWorld w = basic_world(false);
    w.spec.modules[0].symbols.data(0x110C, "`anonymous namespace'::s_old", SynthWorld::kDataSec, 0x40);
    w.open();
    SynthObj o;
    const std::uint32_t data = o.section(".data", kData, 0x60);
    o.symbol("?s_old@?A0x1@@3HA", 0x40, static_cast<std::int32_t>(data), false, false);
    o.symbol("?s_new@?A0x1@@3HA", 0x50, static_cast<std::int32_t>(data), false, false);
    auto init = o.function("_GLOBAL__sub_I_logic.cpp", false);
    const std::uint32_t xcu = o.section(".CRT$XCU", kRdata, 8);
    o.reloc(xcu, 0, init.symbol, kAddr64);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    REQUIRE(r->initialisers.size() == 1);
    CHECK(r->initialisers[0].tu_wide);
    REQUIRE(r->initialisers[0].globals.size() == 1);
    CHECK(r->initialisers[0].globals[0] == "?s_new@?A0x1@@3HA");
    REQUIRE(r->warnings.size() == 1);
    CHECK(r->warnings[0].code == diag::kSharedInitialiser);

    // No new global at all: nothing to run.
    SynthObj o2;
    const std::uint32_t d2 = o2.section(".data", kData, 0x60);
    o2.symbol("?s_old@?A0x1@@3HA", 0x40, static_cast<std::int32_t>(d2), false, false);
    auto init2 = o2.function("_GLOBAL__sub_I_logic.cpp", false);
    const std::uint32_t xcu2 = o2.section(".CRT$XCU", kRdata, 8);
    o2.reloc(xcu2, 0, init2.symbol, kAddr64);
    Built b2 = o2.build();
    auto r2 = plan_bind(w.input(b2));
    REQUIRE_MESSAGE(r2.has_value(), err(r2));
    CHECK(r2->initialisers.empty());
    CHECK(r2->warnings.empty());
}

TEST_CASE("row 5: vtables, RTTI, string and FP constants, unwind data and funclets are fresh patch-local copies") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?area@Square@@UEBAHXZ");
    const std::uint32_t rdata = o.section(".rdata", kRdata, 0x40);
    o.symbol("??_7Square@@6B@", 0, static_cast<std::int32_t>(rdata), false);
    o.symbol("??_R4Square@@6B@", 8, static_cast<std::int32_t>(rdata), false);
    o.symbol("$SG1234", 0x10, static_cast<std::int32_t>(rdata), false, false);
    o.symbol("__real@3ff0000000000000", 0x18, static_cast<std::int32_t>(rdata), false);
    const std::uint32_t xdata = o.section(".xdata", kRdata, 8);
    o.symbol("$unwind$?area@Square@@UEBAHXZ", 0, static_cast<std::int32_t>(xdata), false, false);
    const std::uint32_t pdata = o.section(".pdata", kRdata, 12);
    o.symbol("$pdata$?area@Square@@UEBAHXZ", 0, static_cast<std::int32_t>(pdata), false, false);
    o.function("?dtor$0@?0??area@Square@@UEBAHXZ@4HA", false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    for (const char* n : {"??_7Square@@6B@", "??_R4Square@@6B@", "$SG1234", "__real@3ff0000000000000", "$unwind$?area@Square@@UEBAHXZ",
                          "$pdata$?area@Square@@UEBAHXZ", "?dtor$0@?0??area@Square@@UEBAHXZ@4HA"}) {
        const SymbolDecision* d = find(*r, n);
        REQUIRE_MESSAGE(d != nullptr, n);
        CHECK_MESSAGE(d->decision == DecisionKind::PatchFresh, n);
    }
    CHECK(find(*r, "?area@Square@@UEBAHXZ")->decision == DecisionKind::PatchNewFunction);  // not in this world's PDB
    CHECK(r->new_globals.empty());
    CHECK(r->hooks.empty());
    CHECK(r->fixups.empty());
    (void)f;
}

TEST_CASE("row 6: an undefined function becomes a thunk and a slot; calls need no fix-up, function pointers do") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ");
    const std::uint32_t note = o.undefined("fixture_note", true);
    const std::uint32_t chk = o.undefined("__security_check_cookie", false);  // clang: not typed as a function
    const std::uint32_t eh = o.undefined("__CxxFrameHandler4", true);
    const std::uint32_t rdata = o.section(".rdata", kRdata, 0x10);
    const std::uint32_t xdata = o.section(".xdata", kRdata, 0x10);
    o.reloc(f.section, 1, note, kRel32);
    o.reloc(f.section, 6, chk, kRel32);
    o.reloc(rdata, 0, note, kAddr64);        // &fixture_note stored in a table: the real address
    o.reloc(xdata, 4, eh, kAddr32Nb);        // handler RVA in unwind data: through the thunk
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    for (const char* n : {"fixture_note", "__security_check_cookie", "__CxxFrameHandler4"}) {
        const SymbolDecision* d = find(*r, n);
        REQUIRE_MESSAGE(d != nullptr, n);
        CHECK_MESSAGE(d->decision == DecisionKind::BindOldFunction, n);
        CHECK_MESSAGE(d->type == SymbolType::Function, n);  // from the PDB, not from the object's type field
    }
    REQUIRE(r->thunks.size() == 3);
    CHECK(r->thunks[0].name == "fixture_note");
    CHECK(r->thunks[0].slot == "__cml_slot_fixture_note");
    CHECK(r->thunks[0].target.va == SynthWorld::kBase + 0x1100);
    CHECK(r->placeholders.empty());
    // One ADDR64 retarget plus three slot writes; no REL32 / ADDR32NB fix-up.
    REQUIRE(r->fixups.size() == 4);
    CHECK(has_fixup(*r, FixupKind::Addr64, rdata, 0, "fixture_note"));
    CHECK(has_fixup(*r, FixupKind::Slot, 0, 0, "__cml_slot_fixture_note"));
    CHECK(has_fixup(*r, FixupKind::Slot, 0, 0, "__cml_slot___security_check_cookie"));
    CHECK(has_fixup(*r, FixupKind::Slot, 0, 0, "__cml_slot___CxxFrameHandler4"));
    for (const Fixup& fx : r->fixups) {
        if (fx.kind == FixupKind::Slot && fx.symbol == "__cml_slot___CxxFrameHandler4") {
            CHECK(fx.target_va == SynthWorld::kBase + 0x1210);
        }
    }
}

TEST_CASE("row 7: an undefined data symbol becomes a placeholder; every reference is retargeted with its in-place addend") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ");
    const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
    const std::uint32_t cookie = o.undefined("__security_cookie", false);
    const std::uint32_t data = o.section(".data", kData, 0x10);
    o.reloc(f.section, 3, tick, kRel32_1, 0);
    o.reloc(f.section, 0xA, tick, kRel32, -1);
    o.reloc(f.section, 0x10, cookie, kRel32, 0);
    o.reloc(data, 0, tick, kAddr64, 4);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* d = find(*r, "?g_tick@@3HA");
    REQUIRE(d != nullptr);
    CHECK(d->decision == DecisionKind::BindOldData);
    CHECK_FALSE(d->defined);
    CHECK(d->type == SymbolType::Data);
    CHECK(d->old_address.rva == 0x3080);
    REQUIRE(r->placeholders.size() == 2);
    CHECK(r->placeholders[0].name == "?g_tick@@3HA");
    CHECK(r->placeholders[1].name == "__security_cookie");
    CHECK(r->thunks.empty());
    REQUIRE(r->fixups.size() == 4);
    CHECK(has_fixup(*r, FixupKind::Rel32_1, f.section, 3, "?g_tick@@3HA"));
    CHECK(has_fixup(*r, FixupKind::Rel32, f.section, 0xA, "?g_tick@@3HA"));
    CHECK(has_fixup(*r, FixupKind::Rel32, f.section, 0x10, "__security_cookie"));
    CHECK(has_fixup(*r, FixupKind::Addr64, data, 0, "?g_tick@@3HA"));
    // Fix-ups are ordered by section, then offset.
    CHECK(r->fixups[0].section_index <= r->fixups[1].section_index);
    for (const Fixup& fx : r->fixups) {
        if (fx.symbol == "__security_cookie") {
            CHECK(fx.target_va == SynthWorld::kBase + 0x3100);
        }
        if (fx.kind == FixupKind::Addr64) {
            CHECK(fx.addend == 4);
        }
    }
    CHECK(r->summary().bind_old_data == 2);
}

TEST_CASE("the plan feeds WP-06's binding object writer: thunks first, then placeholders") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ");
    const std::uint32_t note = o.undefined("fixture_note", true);
    const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
    o.reloc(f.section, 1, note, kRel32);
    o.reloc(f.section, 3, tick, kRel32_1);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const std::vector<cml::coff::BindSymbol> syms = r->binding_symbols();
    REQUIRE(syms.size() == 2);
    CHECK(syms[0].name == "fixture_note");
    CHECK(syms[0].kind == cml::coff::BindKind::Function);
    CHECK(syms[1].name == "?g_tick@@3HA");
    CHECK(syms[1].kind == cml::coff::BindKind::Data);
    auto bytes = cml::coff::make_binding_object(syms);
    REQUIRE_MESSAGE(bytes.has_value(), err(bytes));
    auto obj = cml::coff::Object::parse(std::span<const std::byte>(bytes->data(), bytes->size()));
    REQUIRE_MESSAGE(obj.has_value(), err(obj));
    CHECK(obj->find_symbol("fixture_note") != nullptr);
    CHECK(obj->find_symbol("__cml_slot_fixture_note") != nullptr);
    CHECK(obj->find_symbol("?g_tick@@3HA") != nullptr);
}

TEST_CASE("unreferenced undefined symbols and __ImageBase need no binding") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ");
    o.undefined("?never_used@@3HA", false);
    const std::uint32_t base = o.undefined("__ImageBase", false);
    o.reloc(f.section, 4, base, kAddr32Nb);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(find(*r, "?never_used@@3HA") == nullptr);
    const SymbolDecision* d = find(*r, "__ImageBase");
    REQUIRE(d != nullptr);
    CHECK(d->decision == DecisionKind::LinkerProvided);
    CHECK(r->thunks.empty());
    CHECK(r->placeholders.empty());
    CHECK(r->fixups.empty());
}

TEST_CASE("weak externals: bound when something resolves them, otherwise their default is used") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ");
    const std::uint32_t def = o.symbol("default_impl", 8, static_cast<std::int32_t>(f.section), true);
    // weak external "fixture_note" -> default_impl (resolvable in the PDB) and "no_such_weak" -> default_impl
    for (const char* name : {"fixture_note", "no_such_weak"}) {
        cml::coff::ByteVector aux(18, std::byte{0});
        aux[0] = static_cast<std::byte>(def & 0xFF);
        aux[4] = static_cast<std::byte>(3);  // IMAGE_WEAK_EXTERN_SEARCH_ALIAS
        auto s = o.model().add_symbol(name, 0, 0, kFnType, cml::coff::StorageClass::WeakExternal, std::move(aux));
        REQUIRE_MESSAGE(s.has_value(), err(s));
        if (std::string(name) == "fixture_note") {
            o.reloc(f.section, 1, *s, kRel32);
        } else {
            o.reloc(f.section, 6, *s, kRel32);
        }
    }
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(find(*r, "fixture_note")->decision == DecisionKind::BindOldFunction);
    CHECK(find(*r, "no_such_weak")->decision == DecisionKind::WeakDefault);
    REQUIRE(r->thunks.size() == 1);
    CHECK(r->thunks[0].name == "fixture_note");
}
