// WP-07 review fixes: statics match only within their exact scope, overloads are never paired by
// position, read-only data is fresh, REL32 to bound functions goes straight to the old address,
// unreachable data and unmatched initialisers are diagnosed, finish_errors keeps everything.
#include <string>
#include <vector>

#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_names.h"
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
}  // namespace

// ---- 1. scope-exact matching ------------------------------------------------------------------

TEST_CASE("scope: f::counter in the old image is not g::counter in the patch") {
    SynthWorld w = basic_world(false);
    w.proc_with_statics(w.tu(), "f", 0x50, {{"counter", 0x20}});
    w.open();
    {
        SynthObj o;  // control: the same scope still binds
        const std::uint32_t d = o.section(".data", kData, 0x40);
        o.symbol("?counter@?1??f@@YAXXZ@4HA", 0x20, static_cast<std::int32_t>(d), false, false);
        Built b = o.build();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(find(*r, "?counter@?1??f@@YAXXZ@4HA")->decision == DecisionKind::BindOldData);
    }
    SynthObj o;
    const std::uint32_t d = o.section(".data", kData, 0x40);
    const std::uint32_t c = o.symbol("?counter@?1??g@@YAXXZ@4HA", 0x20, static_cast<std::int32_t>(d), false, false);
    auto fn = o.function("?g@@YAXXZ");
    o.reloc(fn.section, 3, c, kRel32);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* s = find(*r, "?counter@?1??g@@YAXXZ@4HA");
    REQUIRE(s != nullptr);
    CHECK(s->decision == DecisionKind::PatchNewData);
    CHECK(s->source == BindSource::None);
    CHECK(r->fixups.empty());  // its own copy is used
    CHECK(r->new_globals.size() == 1);
}

TEST_CASE("scope: a lambda moved from g() to f() is a new function, not a hook of g's lambda") {
    SynthWorld w = basic_world(false);
    w.proc(w.tu(), "`g'::`2'::<lambda_9c7f3a1b2d4e5f60718293a4b5c6d7e8>::operator()", 0x60, 0x10, true);  // cl
    w.proc(w.tu(), "g::<lambda_1>::operator()", 0x70, 0x10, true);                                      // clang
    w.open();
    {
        SynthObj o;  // controls: staying in g() hooks (cl and clang spelling)
        o.function("??R<lambda_1>@?1??g@@YAXH@Z@QEBA@H@Z", false);
        o.function("??R<lambda_2>@?1??g@@YAXH@Z@QEBA@H@Z", false);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        auto r = plan_bind(w.input(b), &errors);
        // two lambdas of g() against two records that spell them differently: exact names decide or it is
        // ambiguous, but never a hook of an unrelated scope.
        if (r) {
            for (const HookEntry& h : r->hooks) {
                CHECK((h.old_rva == 0x1060 || h.old_rva == 0x1070));
            }
        }
    }
    SynthObj o;
    o.function("??R<lambda_1>@?1??f@@YAXH@Z@QEBA@H@Z", false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* d = find(*r, "??R<lambda_1>@?1??f@@YAXH@Z@QEBA@H@Z");
    REQUIRE(d != nullptr);
    CHECK(d->decision == DecisionKind::PatchNewFunction);
    CHECK(r->hooks.empty());
}

TEST_CASE("scope: a new file-scope static count is not the old f::count") {
    SynthWorld w = basic_world(false);
    w.proc_with_statics(w.tu(), "f", 0x50, {{"count", 0x20}});
    w.open();
    SynthObj o;
    const std::uint32_t d = o.section(".data", kData, 0x40);
    o.symbol("?count@?A0x1@@3HA", 0x20, static_cast<std::int32_t>(d), false, false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(find(*r, "?count@?A0x1@@3HA")->decision == DecisionKind::PatchNewData);
    CHECK(r->new_globals.size() == 1);

    // And the other way round: a function-local count in the patch is not the old file-scope count.
    SynthWorld w2 = basic_world(false);
    w2.data(w2.tu(), "`anonymous namespace'::count", 0x20);
    w2.open();
    SynthObj o2;
    const std::uint32_t d2 = o2.section(".data", kData, 0x40);
    o2.symbol("?count@?1??f@@YAXXZ@4HA", 0x20, static_cast<std::int32_t>(d2), false, false);
    Built b2 = o2.build();
    auto r2 = plan_bind(w2.input(b2));
    REQUIRE_MESSAGE(r2.has_value(), err(r2));
    CHECK(find(*r2, "?count@?1??f@@YAXXZ@4HA")->decision == DecisionKind::PatchNewData);
}

// ---- 2. overloads ----------------------------------------------------------------------------------

TEST_CASE("overloads: swapped helper(int) / helper(double) are never paired by position (CML3214)") {
    SynthWorld w = basic_world(false);
    w.proc(w.tu(), "helper", 0x60, 0x10, true);  // old helper(int)
    w.proc(w.tu(), "helper", 0x70, 0x10, true);  // old helper(double)
    w.open();
    SynthObj o;
    o.function("?helper@@YANN@Z", false);  // the patch defines helper(double) first ...
    o.function("?helper@@YAHH@Z", false);  // ... then helper(int)
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE(!r.has_value());
    REQUIRE(errors.size() == 2);
    for (const cml::Diag& e : errors) {
        CHECK(e.code == diag::kAmbiguousOverloads);
        CHECK(e.id() == "CML3214");
        CHECK(contains(e.message, "ambiguous overloads of static"));
        CHECK(contains(e.message, "cannot tell which old function each new one replaces"));
        CHECK(contains(e.message, "logic.cpp.obj"));
        CHECK(e.what_to_do == "Rename one overload, or hot-restart.");
    }
    CHECK(partial.hooks.empty());
}

// ---- 3. read-only data ---------------------------------------------------------------------------------

TEST_CASE("read-only data is fresh: an edited const table takes effect") {
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "k_weights", 0x40);
    w.pub_data("?tbl@@3QBHB", 0x50, SynthWorld::kRdataSec);
    w.open();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ");
    const std::uint32_t ro = o.section(".rdata", kRdata, 0x40);
    const std::uint32_t weights = o.symbol("?k_weights@@3QBHB", 0, static_cast<std::int32_t>(ro), false, false);
    const std::uint32_t tbl = o.symbol("?tbl@@3QBHB", 0x10, static_cast<std::int32_t>(ro), false, true);
    const std::uint32_t rw = o.section(".data", kData, 0x10);
    const std::uint32_t state = o.symbol("?total@Accum@@2HA", 0, static_cast<std::int32_t>(rw), false, true);
    o.reloc(f.section, 3, weights, kRel32);
    o.reloc(f.section, 9, tbl, kRel32);
    o.reloc(f.section, 0xF, state, kRel32);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(find(*r, "?k_weights@@3QBHB")->decision == DecisionKind::PatchFresh);  // static, record exists in the old compiland
    CHECK(find(*r, "?tbl@@3QBHB")->decision == DecisionKind::PatchFresh);        // external, public exists in the old image
    CHECK(find(*r, "?total@Accum@@2HA")->decision == DecisionKind::BindOldData); // writable: state stays
    CHECK(has_fixup(*r, FixupKind::Rel32, f.section, 0xF, "?total@Accum@@2HA"));
    CHECK_FALSE(has_fixup(*r, FixupKind::Rel32, f.section, 3, "?k_weights@@3QBHB"));
    CHECK_FALSE(has_fixup(*r, FixupKind::Rel32, f.section, 9, "?tbl@@3QBHB"));
    CHECK(r->fixups.size() == 1);
}

// ---- 4. REL32 to bound functions ---------------------------------------------------------------------

TEST_CASE("REL32 to a bound function goes straight to the old address (call and lea alike); ADDR32NB keeps the thunk") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x40);
    const std::uint32_t note = o.undefined("fixture_note", true);
    const std::uint32_t xdata = o.section(".xdata", kRdata, 8);
    o.reloc(f.section, 1, note, kRel32, 0);        // call fixture_note
    o.reloc(f.section, 0x10, note, kRel32, 0);     // lea rax, [fixture_note]: address taken
    o.reloc(xdata, 0, note, kAddr32Nb);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(has_fixup(*r, FixupKind::Rel32, f.section, 1, "fixture_note"));
    CHECK(has_fixup(*r, FixupKind::Rel32, f.section, 0x10, "fixture_note"));
    CHECK(r->thunks.size() == 1);  // the linker still needs the symbol
    for (const Fixup& fx : r->fixups) {
        if (fx.kind == FixupKind::Rel32) {
            CHECK(fx.thunk_fallback);
        }
    }
    PatchLayout l;
    l.patch_base = SynthWorld::kBase + 0x10000000ull;
    l.section_rva[f.section] = 0x2000;
    l.symbol_rva["__cml_slot_fixture_note"] = 0x3000;
    auto t = make_fixup_triples(*r, l);
    REQUIRE_MESSAGE(t.has_value(), err(t));
    bool lea_checked = false;
    for (const FixupTriple& tr : *t) {
        if (tr.kind == FixupKind::Rel32 && tr.rva == 0x2010) {
            const std::uint64_t site = l.patch_base + 0x2010;
            // the displacement resolves to the OLD function's real address
            CHECK(static_cast<std::int64_t>(site + 4) + static_cast<std::int32_t>(tr.value) == static_cast<std::int64_t>(SynthWorld::kBase + 0x1100));
            lea_checked = true;
        }
    }
    CHECK(lea_checked);

    // A patch base the old function cannot be reached from: the thunk stays, nothing fails.
    PatchLayout far = l;
    far.patch_base = 0x7FF000000000ull;
    auto tf = make_fixup_triples(*r, far);
    REQUIRE_MESSAGE(tf.has_value(), err(tf));
    for (const FixupTriple& tr : *tf) {
        CHECK(tr.kind == FixupKind::Slot);
    }
}

// ---- 5. data of other modules out of rel32 reach -------------------------------------------------------

TEST_CASE("REL32 to another module's data export beyond +-2 GiB is diagnosed at plan time; functions keep the thunk") {
    SynthWorld w = basic_world();
    InMemoryProcessModules mods;
    mods.add_export("other.dll", "far_counter", 0x7FFB00002000ull, false);
    mods.add_export("other.dll", "far_function", 0x7FFB00001000ull, true);
    w.modules = &mods;
    {
        SynthObj o;
        auto f = o.function("?compute_value@@YAHXZ", true, 0x40);
        const std::uint32_t d = o.undefined("far_counter", false);
        o.reloc(f.section, 3, d, kRel32);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        auto r = plan_bind(w.input(b), &errors);
        REQUIRE(!r.has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kRel32OutOfRange);
        CHECK(contains(errors[0].message, "far_counter"));
        CHECK(contains(errors[0].message, "compute_value"));
    }
    {
        SynthObj o;  // ADDR64 to the same data, and calls to a far function
        auto f = o.function("?compute_value@@YAHXZ", true, 0x40);
        const std::uint32_t d = o.undefined("far_counter", false);
        const std::uint32_t fn = o.undefined("far_function", true);
        const std::uint32_t data = o.section(".data", kData, 0x10);
        o.reloc(data, 0, d, kAddr64);
        o.reloc(f.section, 3, fn, kRel32);
        Built b = o.build();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(has_fixup(*r, FixupKind::Addr64, data, 0, "far_counter"));
        CHECK_FALSE(has_fixup(*r, FixupKind::Rel32, f.section, 3, "far_function"));  // through the thunk
        CHECK(r->thunks.size() == 1);
    }
}

// ---- 6. initialisers --------------------------------------------------------------------------------------

TEST_CASE("initialiser names match variable templates and static data members") {
    CHECK(initializer_matches_global("??__E?$v@H@@YAXXZ", "??$v@H@@3HA"));
    CHECK_FALSE(initializer_matches_global("??__E?$v@H@@YAXXZ", "??$v@N@@3NA"));
    CHECK(initializer_matches_global("??__E?x@Foo@@2HA@@YAXXZ", "?x@Foo@@2HA"));
    CHECK_FALSE(initializer_matches_global("??__E?x@Foo@@2HA@@YAXXZ", "?y@Foo@@2HA"));
    CHECK(initializer_matches_global("??__Eg_tracker@?A0x1@@YAXXZ", "?g_tracker@?A0x1@@3UTracker@@A"));
    CHECK_FALSE(initializer_matches_global("??__Eg_tracker@?A0x1@@YAXXZ", "?g_other@?A0x1@@3HA"));
}

TEST_CASE("initialisers of variable templates and static members are run; an unmatched one is CML3215") {
    SynthWorld w = basic_world();
    {
        SynthObj o;
        const std::uint32_t data = o.section(".data", kData, 0x20);
        o.symbol("??$v@H@@3HA", 0, static_cast<std::int32_t>(data), false, true);
        o.symbol("?x@Foo@@2HA", 8, static_cast<std::int32_t>(data), false, true);
        auto i1 = o.function("??__E?$v@H@@YAXXZ", false);
        auto i2 = o.function("??__E?x@Foo@@2HA@@YAXXZ", false);
        const std::uint32_t xcu = o.section(".CRT$XCU", kRdata, 16);
        o.reloc(xcu, 0, i1.symbol, kAddr64);
        o.reloc(xcu, 8, i2.symbol, kAddr64);
        Built b = o.build();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        REQUIRE(r->initialisers.size() == 2);
        CHECK(r->initialisers[0].function == "??__E?$v@H@@YAXXZ");
        CHECK(r->initialisers[0].globals == std::vector<std::string>({"??$v@H@@3HA"}));
        CHECK(r->initialisers[1].globals == std::vector<std::string>({"?x@Foo@@2HA"}));
    }
    {
        SynthObj o;
        const std::uint32_t data = o.section(".data", kData, 0x20);
        o.symbol("?g_real@?A0x1@@3HA", 0, static_cast<std::int32_t>(data), false, false);
        auto orphan = o.function("??__Eorphan@?A0x1@@YAXXZ", false);
        const std::uint32_t xcu = o.section(".CRT$XCU", kRdata, 8);
        o.reloc(xcu, 0, orphan.symbol, kAddr64);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        auto r = plan_bind(w.input(b), &errors);
        REQUIRE(!r.has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kUnmatchedInitialiser);
        CHECK(errors[0].id() == "CML3215");
        CHECK(contains(errors[0].message, "dynamic initialiser '??__Eorphan@?A0x1@@YAXXZ'"));
        CHECK(contains(errors[0].message, "could not be matched to a new global"));
    }
}

// ---- 7. finish_errors -------------------------------------------------------------------------------------

TEST_CASE("a fatal error does not hide the diagnostics collected before it, and the partial plan is filled") {
    SynthWorld w = basic_world(false);
    w.tu().symbols.raw(tp::Bytes{0xF0, 0xFF, 0x10, 0x11, 0, 0, 0, 0});  // corrupt record in the TU's symbol stream
    w.open();
    SynthObj o;
    o.function("?compute_value@@YAHXZ");
    const std::uint32_t tls = o.section(".tls$", kData, 8);
    o.symbol("?t_counter@@3HA", 0, static_cast<std::int32_t>(tls), false);  // CML3102, collected first
    const std::uint32_t d = o.section(".data", kData, 0x10);
    o.symbol("?s@?A0x1@@3HA", 0, static_cast<std::int32_t>(d), false, false);  // needs the TU's records: fatal
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE(!r.has_value());
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].code == diag::kThreadLocal);
    CHECK(errors[1].code == 1413);  // the fatal one, last
    CHECK(r.error().code == 1413);
    CHECK_FALSE(partial.decisions.empty());
    CHECK(partial.find_decision("?t_counter@@3HA") != nullptr);
    CHECK(partial.find_decision("?compute_value@@YAHXZ") != nullptr);
}
