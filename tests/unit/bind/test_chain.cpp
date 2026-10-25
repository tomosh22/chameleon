// WP-07: the resolution chain of plan 4.5 (registry, compiland statics, PDB publics/globals, import
// slots, other modules' exports) and the bridge from the object's decorated names of statics to the
// PDB's records (ADR-0010).
#include <string>

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

// An object whose only function references the given undefined symbols with REL32.
struct Caller {
    SynthObj o;
    SynthObj::Func f;
    std::uint32_t next_offset = 0;
    Caller() { f = o.function("?caller@@YAHXZ", true, 0x40); }
    void use(const std::string& name, bool is_function, coff::RelocType type = kRel32) {
        const std::uint32_t s = o.undefined(name, is_function);
        o.reloc(f.section, next_offset, s, type);
        next_offset += 8;
    }
};

}  // namespace

TEST_CASE("chain (1): the process symbol registry wins and serves functions and data of earlier patches") {
    SynthWorld w = basic_world();
    InMemorySymbolRegistry reg;
    reg.add("?patched_helper@@YAHXZ", {0x7FF000001000ull, true, "patch 1"});
    reg.add("?patch_global@@3HA", {0x7FF000002000ull, false, "patch 1"});
    reg.add("?g_tick@@3HA", {0x7FF000003000ull, false, "patch 1"});  // also a PDB public: the registry is first
    w.registry = &reg;
    Caller c;
    c.use("?patched_helper@@YAHXZ", true);
    c.use("?patch_global@@3HA", false);
    c.use("?g_tick@@3HA", false);
    Built b = c.o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* fn = find(*r, "?patched_helper@@YAHXZ");
    REQUIRE(fn != nullptr);
    CHECK(fn->decision == DecisionKind::BindOldFunction);
    CHECK(fn->source == BindSource::Registry);
    CHECK(fn->old_address.va == 0x7FF000001000ull);
    CHECK(fn->old_address.rva == kNoRva);
    const SymbolDecision* g = find(*r, "?patch_global@@3HA");
    REQUIRE(g != nullptr);
    CHECK(g->decision == DecisionKind::BindOldData);
    CHECK(g->source == BindSource::Registry);
    const SymbolDecision* t = find(*r, "?g_tick@@3HA");
    CHECK(t->source == BindSource::Registry);
    CHECK(t->old_address.va == 0x7FF000003000ull);
    // The data fix-ups point at the registry addresses.
    for (const Fixup& f : r->fixups) {
        if (f.symbol == "?g_tick@@3HA") {
            CHECK(f.target_va == 0x7FF000003000ull);
            CHECK(f.target_rva == kNoRva);
        }
    }
}

TEST_CASE("chain (1): a second patch of the same TU hooks / binds the first patch's definitions") {
    SynthWorld w = basic_world();
    InMemorySymbolRegistry reg;
    reg.add("?new_helper@@YAHXZ", {0x7FF000001000ull, true, "patch 1"});
    reg.add("?g_new@@3HA", {0x7FF000002000ull, false, "patch 1"});
    w.registry = &reg;
    SynthObj o;
    o.function("?new_helper@@YAHXZ");
    const std::uint32_t data = o.section(".data", kData, 8);
    o.symbol("?g_new@@3HA", 0, static_cast<std::int32_t>(data), false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* fn = find(*r, "?new_helper@@YAHXZ");
    CHECK(fn->decision == DecisionKind::PatchHooked);
    CHECK(fn->source == BindSource::Registry);
    REQUIRE(r->hooks.size() == 1);
    CHECK(r->hooks[0].old_va == 0x7FF000001000ull);
    CHECK(r->hooks[0].old_rva == kNoRva);
    CHECK(find(*r, "?g_new@@3HA")->decision == DecisionKind::BindOldData);
    CHECK(r->new_globals.empty());
    CHECK(r->new_functions.empty());
}

TEST_CASE("chain (2): the compiland's own statics resolve names that can only denote TU-private entities") {
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "`anonymous namespace'::s_calls", 0x10);
    w.proc_with_statics(w.tu(), "statics_value", 0x50, {{"g", 0x20}});
    w.open();
    Caller c;
    c.use("?s_calls@?A0xdeadbeef@@3HA", false);
    c.use("?g@?1??statics_value@@YAHXZ@4UGuarded@@A", false);
    Built b = c.o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* s = find(*r, "?s_calls@?A0xdeadbeef@@3HA");
    REQUIRE(s != nullptr);
    CHECK(s->decision == DecisionKind::BindOldData);
    CHECK(s->source == BindSource::CompilandStatic);
    CHECK(s->old_address.rva == 0x3010);
    const SymbolDecision* g = find(*r, "?g@?1??statics_value@@YAHXZ@4UGuarded@@A");
    REQUIRE(g != nullptr);
    CHECK(g->source == BindSource::CompilandStatic);
    CHECK(g->old_address.rva == 0x3020);
    CHECK(g->display == "statics_value::g");
}

TEST_CASE("chain (2): an ordinary external name is never mistaken for a compiland static of the same identifier") {
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "g_tick", 0x40);  // a local static called g_tick in the TU, unrelated to ?g_tick@@3HA
    w.open();
    Caller c;
    c.use("?g_tick@@3HA", false);
    Built b = c.o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* d = find(*r, "?g_tick@@3HA");
    CHECK(d->source == BindSource::PdbPublic);
    CHECK(d->old_address.rva == 0x3080);
}

TEST_CASE("chain (3): PDB publics, then PDB global data (C names without a public)") {
    SynthWorld w = basic_world(false);
    w.spec.symrec.data(0x110D, "c_global", SynthWorld::kDataSec, 0x120);
    w.open();
    Caller c;
    c.use("?total@Accum@@2HA", false);
    c.use("c_global", false);
    c.use("fixture_note", true);
    Built b = c.o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(find(*r, "?total@Accum@@2HA")->source == BindSource::PdbPublic);
    CHECK(find(*r, "?total@Accum@@2HA")->old_address.rva == 0x3090);
    const SymbolDecision* g = find(*r, "c_global");
    REQUIRE(g != nullptr);
    CHECK(g->source == BindSource::PdbGlobal);
    CHECK(g->decision == DecisionKind::BindOldData);
    CHECK(g->old_address.rva == 0x3120);
    CHECK(find(*r, "fixture_note")->decision == DecisionKind::BindOldFunction);
}

TEST_CASE("chain (4): __imp_X is the module's IAT slot: PDB public first, else the import directory in memory") {
    SynthWorld w = basic_world();
    InMemoryProcessModules mods;
    mods.add_iat_slot("OLD.EXE", "GetTickCount", 0x140002030ull);  // module names compare case-insensitively
    mods.add_iat_slot("other.dll", "Elsewhere", 0x1ull);           // another module's import: not ours
    w.modules = &mods;
    Caller c;
    c.use("__imp_Sleep", false);
    c.use("__imp_GetTickCount", false);
    c.use("__imp_Elsewhere", false);
    Built b = c.o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(w.input(b), &errors);
    REQUIRE(!r.has_value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kUnresolved);
    CHECK(errors[0].message.find("__imp_Elsewhere") != std::string::npos);

    Caller c2;
    c2.use("__imp_Sleep", false);
    c2.use("__imp_GetTickCount", false);
    Built b2 = c2.o.build();
    auto r2 = plan_bind(w.input(b2));
    REQUIRE_MESSAGE(r2.has_value(), err(r2));
    const SymbolDecision* sleep = find(*r2, "__imp_Sleep");
    CHECK(sleep->decision == DecisionKind::BindOldData);
    CHECK(sleep->source == BindSource::ImportSlotPdb);
    CHECK(sleep->old_address.rva == 0x2010);
    const SymbolDecision* tick = find(*r2, "__imp_GetTickCount");
    CHECK(tick->decision == DecisionKind::BindOldData);
    CHECK(tick->source == BindSource::ImportSlotModule);
    CHECK(tick->old_address.va == 0x140002030ull);
    CHECK(tick->old_address.module == "old.exe");
    // Both are data placeholders whose references (call [rip+__imp_X]) are retargeted to the slot.
    CHECK(r2->placeholders.size() == 2);
    CHECK(r2->thunks.empty());
    CHECK(r2->fixups.size() == 2);
}

TEST_CASE("chain (5): exports of other loaded modules; an export the module does not import gets a synthesized slot") {
    SynthWorld w = basic_world();
    InMemoryProcessModules mods;
    mods.add_export("kernel32.dll", "Beep", 0x7FFB00001000ull, true);
    mods.add_export("some.dll", "exported_counter", 0x150000000ull, false);  // within rel32 reach of the module
    w.modules = &mods;
    Caller c;
    c.use("Beep", true);
    c.use("exported_counter", false);
    c.use("__imp_Beep", false);
    Built b = c.o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* f = find(*r, "Beep");
    CHECK(f->decision == DecisionKind::BindOldFunction);
    CHECK(f->source == BindSource::ExportOtherModule);
    CHECK(f->old_address.va == 0x7FFB00001000ull);
    CHECK(f->old_address.module == "kernel32.dll");
    const SymbolDecision* d = find(*r, "exported_counter");
    CHECK(d->decision == DecisionKind::BindOldData);
    CHECK(d->source == BindSource::ExportOtherModule);
    const SymbolDecision* imp = find(*r, "__imp_Beep");
    CHECK(imp->decision == DecisionKind::BindOldData);
    CHECK(imp->source == BindSource::SynthesizedImportSlot);
    CHECK(imp->old_address.va == 0x7FFB00001000ull);

    // The synthesized slot: placeholder flagged, filled by a Slot write, references NOT retargeted.
    bool found = false;
    for (const PlaceholderEntry& p : r->placeholders) {
        if (p.name == "__imp_Beep") {
            found = true;
            CHECK(p.is_synthesized_slot);
        }
    }
    CHECK(found);
    CHECK(has_fixup(*r, FixupKind::Slot, 0, 0, "__imp_Beep"));
    CHECK_FALSE(has_fixup(*r, FixupKind::Rel32, c.f.section, 16, "__imp_Beep"));
    // The data export is retargeted like any data.
    CHECK(has_fixup(*r, FixupKind::Rel32, c.f.section, 8, "exported_counter"));
}

TEST_CASE("chain order: PDB beats exports; registry beats everything") {
    SynthWorld w = basic_world();
    InMemoryProcessModules mods;
    mods.add_export("other.dll", "fixture_note", 0x7FFB00009000ull, true);
    w.modules = &mods;
    Caller c;
    c.use("fixture_note", true);
    Built b = c.o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(find(*r, "fixture_note")->source == BindSource::PdbPublic);
    CHECK(find(*r, "fixture_note")->old_address.rva == 0x1100);
}

TEST_CASE("chain: nothing resolves it -> CML3200 naming the symbol and the referencing function") {
    SynthWorld w = basic_world();
    Caller c;
    c.use("?missing_one@@YAHXZ", true);
    c.use("?missing_two@@3HA", false);
    c.use("fixture_note", true);
    Built b = c.o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(w.input(b), &errors);
    REQUIRE(!r.has_value());
    CHECK(r.error().code == diag::kUnresolved);
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].code == diag::kUnresolved);
    CHECK(errors[0].id() == "CML3200");
    CHECK(errors[0].message.find("missing_one") != std::string::npos);
    CHECK(errors[0].message.find("?missing_one@@YAHXZ") != std::string::npos);
    CHECK(errors[0].message.find("caller") != std::string::npos);
    CHECK(errors[0].message.find("?caller@@YAHXZ") != std::string::npos);
    CHECK(errors[0].message.find("logic.cpp.obj") != std::string::npos);
    CHECK_FALSE(errors[0].what_to_do.empty());
    CHECK(errors[1].message.find("missing_two") != std::string::npos);
}

// ---- statics (ADR-0010) ----------------------------------------------------------------------

TEST_CASE("statics: a unique file-static is bound by (name, compiland); a new one is new") {
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "`anonymous namespace'::s_calls", 0x10);
    w.data(w.main_obj(), "`anonymous namespace'::s_calls", 0x90);  // same name in ANOTHER compiland: ignored
    w.open();
    SynthObj o;
    const std::uint32_t data = o.section(".data", kData, 0x40);
    o.symbol("?s_calls@?A0x11111111@@3HA", 0x10, static_cast<std::int32_t>(data), false, false);
    o.symbol("?s_fresh@?A0x11111111@@3HA", 0x20, static_cast<std::int32_t>(data), false, false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* s = find(*r, "?s_calls@?A0x11111111@@3HA");
    CHECK(s->decision == DecisionKind::BindOldData);
    CHECK(s->source == BindSource::CompilandStatic);
    CHECK(s->old_address.rva == 0x3010);  // the TU's own record, not main.cpp's
    CHECK(s->is_static);
    CHECK(find(*r, "?s_fresh@?A0x11111111@@3HA")->decision == DecisionKind::PatchNewData);
    REQUIRE(r->new_globals.size() == 1);
    CHECK(r->new_globals[0].is_static);
}

TEST_CASE("statics: a function-local static and its guard variable are bound; the guard comes from the unchanged layout") {
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "`anonymous namespace'::s_calls", 0x10);
    w.proc_with_statics(w.tu(), "statics_value", 0x50, {{"g", 0x20}});
    w.open();
    SynthObj o;
    const std::uint32_t bss = o.section(".data", kData, 0x40);
    const std::int32_t sec = static_cast<std::int32_t>(bss);
    o.symbol("?s_calls@?A0x1@@3HA", 0x10, sec, false, false);
    o.symbol("?g@?1??statics_value@@YAHXZ@4UGuarded@@A", 0x20, sec, false, false);
    o.symbol("?$TSS0@?1??statics_value@@YAHXZ@4HA", 0x24, sec, false, false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* g = find(*r, "?g@?1??statics_value@@YAHXZ@4UGuarded@@A");
    CHECK(g->decision == DecisionKind::BindOldData);
    CHECK(g->source == BindSource::CompilandStatic);
    CHECK(g->old_address.rva == 0x3020);
    const SymbolDecision* guard = find(*r, "?$TSS0@?1??statics_value@@YAHXZ@4HA");
    REQUIRE(guard != nullptr);
    CHECK(guard->decision == DecisionKind::BindOldData);
    CHECK(guard->source == BindSource::CompilandLayout);
    CHECK(guard->old_address.rva == 0x3024);
    CHECK(r->warnings.empty());
    CHECK(r->new_globals.empty());
}

TEST_CASE("statics: cl records the guard variable in the PDB: it is matched by name, no inference needed") {
    SynthWorld w = basic_world(false);
    w.proc_with_statics(w.tu(), "statics_value", 0x50, {{"g", 0x20}, {"$TSS0", 0x24}});
    w.open();
    SynthObj o;
    const std::uint32_t g_sec = o.section(".bss", kData | coff::scn::LnkComdat, 4, coff::ComdatSelection::Any);
    o.symbol("?g@?1??statics_value@@YAHXZ@4UGuarded@@A", 0, static_cast<std::int32_t>(g_sec), false, false);
    const std::uint32_t t_sec = o.section(".bss", kData | coff::scn::LnkComdat, 4, coff::ComdatSelection::Any);
    o.symbol("?$TSS0@?1??statics_value@@YAHXZ@4HA", 0, static_cast<std::int32_t>(t_sec), false, false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* guard = find(*r, "?$TSS0@?1??statics_value@@YAHXZ@4HA");
    CHECK(guard->decision == DecisionKind::BindOldData);
    CHECK(guard->source == BindSource::CompilandStatic);
    CHECK(guard->old_address.rva == 0x3024);
    CHECK(r->warnings.empty());
}

TEST_CASE("statics: when the layout changed the guard is NOT guessed - it is new, with a warning") {
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "`anonymous namespace'::s_calls", 0x10);
    w.proc_with_statics(w.tu(), "statics_value", 0x50, {{"g", 0x20}});
    w.open();
    SynthObj o;
    const std::uint32_t bss = o.section(".data", kData, 0x40);
    const std::int32_t sec = static_cast<std::int32_t>(bss);
    o.symbol("?s_calls@?A0x1@@3HA", 0x14, sec, false, false);  // moved by 4 bytes: deltas disagree
    o.symbol("?g@?1??statics_value@@YAHXZ@4UGuarded@@A", 0x20, sec, false, false);
    o.symbol("?$TSS0@?1??statics_value@@YAHXZ@4HA", 0x24, sec, false, false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(find(*r, "?$TSS0@?1??statics_value@@YAHXZ@4HA")->decision == DecisionKind::PatchNewData);
    REQUIRE(r->warnings.size() == 1);
    CHECK(r->warnings[0].code == diag::kGuardUnmatched);
    CHECK(r->warnings[0].message.find("TSS0") != std::string::npos);
}

TEST_CASE("statics: static functions are matched to S_LPROC32 and hooked") {
    SynthWorld w = basic_world();
    SynthObj o;
    o.function("?helper@@YAHXZ", false);
    o.function("?brand_new_static@@YAHXZ", false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* h = find(*r, "?helper@@YAHXZ");
    CHECK(h->decision == DecisionKind::PatchHooked);
    CHECK(h->source == BindSource::CompilandStatic);
    CHECK(h->old_address.rva == 0x1040);
    REQUIRE(r->hooks.size() == 1);
    CHECK(r->hooks[0].old_size == 0x10);
    CHECK(find(*r, "?brand_new_static@@YAHXZ")->decision == DecisionKind::PatchNewFunction);
}

TEST_CASE("statics: same-named statics are told apart by the RVA the unchanged layout predicts, never by position") {
    SynthWorld w = basic_world(false);
    // Two block-scope statics called tmp in f.
    w.data(w.tu(), "`anonymous namespace'::anchor", 0x10);
    w.proc_with_statics(w.tu(), "f", 0x80, {{"tmp", 0x34}, {"tmp", 0x30}});
    w.open();
    SynthObj o;
    const std::uint32_t d = o.section(".data", kData, 0x40);
    const std::int32_t sec = static_cast<std::int32_t>(d);
    o.symbol("?anchor@?A0x1@@3HA", 0x10, sec, false, false);
    o.symbol("?tmp@?1??f@@YAXXZ@4HA", 0x30, sec, false, false);
    o.symbol("?tmp@?2??f@@YAXXZ@4HA", 0x34, sec, false, false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    // Data: the unchanged section layout (anchor 0x10 -> 0x3010) gives the exact RVAs, whatever the record order.
    const SymbolDecision* t1 = find(*r, "?tmp@?1??f@@YAXXZ@4HA");
    const SymbolDecision* t2 = find(*r, "?tmp@?2??f@@YAXXZ@4HA");
    CHECK(t1->source == BindSource::CompilandRva);
    CHECK(t1->old_address.rva == 0x3030);
    CHECK(t2->source == BindSource::CompilandRva);
    CHECK(t2->old_address.rva == 0x3034);
}

TEST_CASE("statics: ambiguous static data is CML3207, ambiguous static overloads are CML3214") {
    SynthWorld w = basic_world(false);
    w.proc(w.tu(), "pick", 0x60, 0x10, true);
    w.proc(w.tu(), "pick", 0x70, 0x10, true);
    w.proc_with_statics(w.tu(), "f", 0x80, {{"tmp", 0x30}, {"tmp", 0x34}});
    w.open();
    {
        SynthObj o;
        o.function("?pick@@YAHH@Z", false);  // the object keeps only one of the two overloads
        Built b = o.build();
        std::vector<cml::Diag> errors;
        auto r = plan_bind(w.input(b), &errors);
        REQUIRE(!r.has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kAmbiguousOverloads);
        CHECK(errors[0].id() == "CML3214");
        CHECK(errors[0].message.find("ambiguous overloads of static") != std::string::npos);
        CHECK(errors[0].message.find("pick") != std::string::npos);
        CHECK(errors[0].message.find("logic.cpp.obj") != std::string::npos);
    }
    {
        SynthObj o;  // one tmp in the object, two in the old compiland, no unique static to fix the layout
        const std::uint32_t d = o.section(".data", kData, 0x40);
        o.symbol("?tmp@?1??f@@YAXXZ@4HA", 0x30, static_cast<std::int32_t>(d), false, false);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        auto r = plan_bind(w.input(b), &errors);
        REQUIRE(!r.has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kAmbiguousStatic);
        CHECK(errors[0].message.find("tmp") != std::string::npos);
        CHECK(errors[0].message.find("2 records") != std::string::npos);
    }
}

TEST_CASE("statics: static function templates are not matched by base name; operators and ctors match by qualified name") {
    {
        SynthWorld w = basic_world(false);
        w.proc(w.tu(), "forward", 0x60, 0x10, true);  // some unrelated instantiation of forward<>
        w.proc(w.tu(), "lambdas_init::<lambda_0>::operator()", 0x70, 0x10, true);  // clang's spelling
        w.proc(w.tu(), "`anonymous namespace'::Tracker::Tracker", 0x80, 0x10, true);
        w.open();
        SynthObj o;
        o.function("??$forward@AEBV<lambda_0>@?0??lambdas_init@@YAXH@Z@@std@@YAAEBV<lambda_0>@?0??lambdas_init@@YAXH@Z@AEBV1?0??2@YAXH@Z@@Z", false);
        o.function("??R<lambda_0>@?0??lambdas_init@@YAXH@Z@QEBA?A?<auto>@@H@Z", false);
        o.function("??0Tracker@?A0x1@@QEAA@PEBD@Z", false);
        Built b = o.build();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(r->decisions[0].decision == DecisionKind::PatchNewFunction);  // the template: left alone
        CHECK(r->decisions[1].decision == DecisionKind::PatchHooked);
        CHECK(r->decisions[1].old_address.rva == 0x1070);
        CHECK(r->decisions[2].decision == DecisionKind::PatchHooked);
        CHECK(r->decisions[2].old_address.rva == 0x1080);
        CHECK(r->hooks.size() == 2);
    }
    {
        SynthWorld w = basic_world(false);  // cl: hashed lambda, quoted scopes; std::function::operator() is another record
        w.proc(w.tu(), "`lambdas_init'::`2'::<lambda_9c7f3a1b2d4e5f60718293a4b5c6d7e8>::operator()", 0x90, 0x10, true);
        w.proc(w.tu(), "std::function<int __cdecl(int)>::operator()", 0xA0, 0x10, true);
        w.open();
        SynthObj o;
        o.function("??R<lambda_1>@?1??lambdas_init@@YAXH@Z@QEBA@H@Z", false);
        Built b = o.build();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(r->decisions[0].decision == DecisionKind::PatchHooked);  // not confused with std::function::operator()
        CHECK(r->decisions[0].old_address.rva == 0x1090);
        CHECK(r->warnings.empty());
    }
}

TEST_CASE("compiland lookup: by object name, by stem, by index; unknown and ambiguous TUs are diagnosed") {
    SynthWorld w = basic_world();
    SynthObj o;
    o.function("?compute_value@@YAHXZ");
    Built b = o.build();
    for (const char* name : {"logic.cpp.obj", "LOGIC.CPP.OBJ", "logic", "C:\\proj\\obj\\logic.cpp.obj"}) {
        auto r = plan_bind(w.input(b, name));
        REQUIRE_MESSAGE(r.has_value(), name << ": " << err(r));
        CHECK(r->compiland_index == 0);
        CHECK(r->compiland == "C:\\proj\\obj\\logic.cpp.obj");
    }
    PlanInput by_index = w.input(b);
    by_index.compiland.clear();
    by_index.compiland_index = 1;
    auto r1 = plan_bind(by_index);
    REQUIRE_MESSAGE(r1.has_value(), err(r1));
    CHECK(r1->compiland_index == 1);
    // compiland 1 is main.cpp.obj: compute_value is then looked up as a hook of the public anyway.
    CHECK(find(*r1, "?compute_value@@YAHXZ")->decision == DecisionKind::PatchHooked);

    auto missing = plan_bind(w.input(b, "nothere.cpp.obj"));
    REQUIRE(!missing.has_value());
    CHECK(missing.error().code == diag::kCompilandNotFound);
    CHECK(missing.error().message.find("nothere.cpp.obj") != std::string::npos);

    PlanInput out_of_range = w.input(b);
    out_of_range.compiland_index = 99;
    auto oor = plan_bind(out_of_range);
    REQUIRE(!oor.has_value());
    CHECK(oor.error().code == diag::kCompilandNotFound);

    SynthWorld dup = basic_world(false);
    tp::Module again;
    again.name = "C:\\other\\obj\\logic.cpp.obj";
    again.symbols.objname(again.name);
    dup.spec.modules.push_back(std::move(again));
    dup.open();
    auto amb = plan_bind(dup.input(b));
    REQUIRE(!amb.has_value());
    CHECK(amb.error().code == diag::kCompilandAmbiguous);
}
