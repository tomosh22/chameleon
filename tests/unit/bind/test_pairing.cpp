// WP-40 (ADR-0043, R-48): bind-planner pairing defects that ended in a wrong pairing or a silent re-initialisation.
// Each case below failed on main (2456392) and states what main did. Synthetic objects (WP-06 writer) against
// synthetic PDBs (WP-04 builder); the shapes are the ones clang 18 + lld and the CI logs of WP-37 showed.
#include <string>
#include <vector>

#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_names.h"
#include "cml/core/bind_plan.h"
#include "doctest.h"
#include "synth.h"
#include "worlds.h"
#include "../pdb/type_builder.h"

using namespace cml_bind_test;
using namespace cml::core;
using namespace cml::core::bind;

namespace {

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

SymbolDecision decision(const BindPlan& p, const std::string& name) {
    const SymbolDecision* d = p.find_decision(name);
    REQUIRE_MESSAGE(d != nullptr, name);
    return *d;
}

std::size_t count_code(const std::vector<cml::Diag>& list, std::uint32_t code) {
    std::size_t n = 0;
    for (const cml::Diag& d : list) {
        n += d.code == code ? 1u : 0u;
    }
    return n;
}

// clang-cl's names of the static of the n-th lambda of `ls_step()` and of its guard (clang numbers the lambdas of a
// translation unit from 0 and spells the closure's scope `?0??ls_step@@YAHXZ@`).
std::string lambda_static(const std::string& name, int n, const std::string& type = "4ULsName@@A") {
    return "?" + name + "@?1???R<lambda_" + std::to_string(n) + ">@?0??ls_step@@YAHXZ@QEBA@XZ@" + type;
}
std::string lambda_guard(int n) { return lambda_static("$TSS0", n, "4HA"); }

}  // namespace

// ---- R-48 item 1: the static of an inlined lambda (clang-cl /O2 /Ob2) -------------------------------------------------

// What clang 18 at -O2 writes for `int ls_step() { LsName& a = LS_NAME(1); LsName& b = LS_NAME(2); ... }` (llvm-pdbutil
// of the fixture lambda_statics built at -O2): no procedure for either operator(), and the two `sname` as S_LDATA32 of
// `ls_step` itself, before its S_INLINESITE records. No guard is recorded. The patch object still names the statics
// after their lambda.
TEST_CASE("inlined lambdas: their statics are found in the calling procedure and placed by the section layout, with their guards") {
    // main: "static data 'sname' has no record in compiland 0: new" for both, and both guards bound to the OLD guards by
    // the section layout: the patched ls_step() would use objects that were never constructed.
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "`anonymous namespace'::s_count", 0x10);
    w.proc_with_statics(w.tu(), "ls_step", 0x60, {{"sname", 0x20}, {"sname", 0x30}});
    w.open();
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("?s_count@?A0x1@@3HA", 0x10, bss, false, false);
    for (int n = 0; n < 2; ++n) {
        o.symbol(lambda_static("sname", n), 0x20 + 0x10 * static_cast<std::uint32_t>(n), bss, false, false);
        o.symbol(lambda_guard(n), 0x28 + 0x10 * static_cast<std::uint32_t>(n), bss, false, false);
    }
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    for (int n = 0; n < 2; ++n) {
        CAPTURE(n);
        const SymbolDecision s = decision(*r, lambda_static("sname", n));
        CHECK(s.decision == DecisionKind::BindOldData);
        CHECK(s.source == BindSource::CompilandRva);
        CHECK(s.old_address.rva == 0x3020 + 0x10 * static_cast<std::uint32_t>(n));
        const SymbolDecision g = decision(*r, lambda_guard(n));
        CHECK(g.decision == DecisionKind::BindOldData);
        CHECK(g.source == BindSource::CompilandLayout);
        CHECK(g.old_address.rva == 0x3028 + 0x10 * static_cast<std::uint32_t>(n));
    }
    CHECK(r->new_globals.empty());
    CHECK(r->warnings.empty());
}

TEST_CASE("inlined lambdas: without a known layout the statics are refused, and their guards are never bound") {
    // main: both statics new, both guards new with CML3211, plan accepted (the old statics stay behind, the new ones are
    // constructed: state reset without an error).
    SynthWorld w = basic_world(false);
    w.proc_with_statics(w.tu(), "ls_step", 0x60, {{"sname", 0x20}, {"sname", 0x30}});
    w.open();
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    for (int n = 0; n < 2; ++n) {
        o.symbol(lambda_static("sname", n), 0x20 + 0x10 * static_cast<std::uint32_t>(n), bss, false, false);
        o.symbol(lambda_guard(n), 0x28 + 0x10 * static_cast<std::uint32_t>(n), bss, false, false);
    }
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE_FALSE(r.has_value());
    CHECK(count_code(errors, diag::kAmbiguousStatic) == 2);
    for (const cml::Diag& e : errors) {
        CAPTURE(e.message);
        CHECK(e.code == diag::kAmbiguousStatic);
    }
    CHECK(contains(errors[0].message, "are in the procedure of 'ls_step' itself (the compiler inlined the lambdas)"));
    for (int n = 0; n < 2; ++n) {
        CHECK(decision(partial, lambda_guard(n)).decision != DecisionKind::BindOldData);
    }
    CHECK(partial.warnings.empty());  // no CML3211 next to the errors
}

TEST_CASE("inlined lambdas: a lambda added in front is not placed by the layout (the count of the function's statics changed)") {
    // main: three new statics and three guards placed by layout, the first two on the OLD guards of other lambdas.
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "`anonymous namespace'::s_count", 0x10);
    w.proc_with_statics(w.tu(), "ls_step", 0x60, {{"sname", 0x20}, {"sname", 0x30}});
    w.open();
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x50));
    o.symbol("?s_count@?A0x1@@3HA", 0x10, bss, false, false);
    for (int n = 0; n < 3; ++n) {
        o.symbol(lambda_static("sname", n), 0x20 + 0x10 * static_cast<std::uint32_t>(n), bss, false, false);
        o.symbol(lambda_guard(n), 0x28 + 0x10 * static_cast<std::uint32_t>(n), bss, false, false);
    }
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE_FALSE(r.has_value());
    CHECK(count_code(errors, diag::kAmbiguousStatic) == 3);
    CHECK(contains(errors[0].message, "the lambdas of 'ls_step' that hold a static named 'sname' are not the same as in the old image (2 there"));
    for (int n = 0; n < 3; ++n) {
        CHECK(decision(partial, lambda_static("sname", n)).decision != DecisionKind::BindOldData);
        CHECK(decision(partial, lambda_guard(n)).decision != DecisionKind::BindOldData);
    }
}

TEST_CASE("inlined lambdas: one lambda, one record in the calling procedure: not paired by its name alone") {
    // A record "sname" of ls_step could be ls_step's own static or the inlined lambda's: pass A does not pair it by name.
    // With a known layout the RVA decides (here: it agrees).
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "`anonymous namespace'::s_count", 0x10);
    w.proc_with_statics(w.tu(), "ls_step", 0x60, {{"sname", 0x20}});
    w.open();
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("?s_count@?A0x1@@3HA", 0x10, bss, false, false);
    o.symbol(lambda_static("sname", 0), 0x20, bss, false, false);
    o.symbol(lambda_guard(0), 0x28, bss, false, false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, lambda_static("sname", 0)).source == BindSource::CompilandRva);
    CHECK(decision(*r, lambda_static("sname", 0)).old_address.rva == 0x3020);
    CHECK(decision(*r, lambda_guard(0)).old_address.rva == 0x3028);
}

// ---- R-48 item 2 and the guard rule (ADR-0043): a guard follows the statics of its scope -------------------------------

TEST_CASE("guards: the guard of a NEW static is new, even where the layout maps it onto old data of the compiland") {
    // main: pass C bound the new guard at old rva 0x3018, which is `s_state` (non-zero at run time: "initialised"), so
    // the new static `n` was never constructed.
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "`anonymous namespace'::s_a", 0x10);
    w.data(w.tu(), "`anonymous namespace'::s_state", 0x18);
    w.data(w.tu(), "`anonymous namespace'::s_more", 0x20);
    w.open();
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("?s_a@?A0x1@@3HA", 0x10, bss, false, false);
    o.symbol("?n@?1??fresh@@YAHXZ@4HA", 0x14, bss, false, false);   // new function-local static of a new function
    o.symbol("?$TSS0@?1??fresh@@YAHXZ@4HA", 0x18, bss, false, false);  // its guard: old 0x3018 is s_state
    o.symbol("?$TSS0@?1??other@@YAHXZ@4HA", 0x1C, bss, false, false);  // a guard with no static in the object at all
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, "?n@?1??fresh@@YAHXZ@4HA").decision == DecisionKind::PatchNewData);
    const SymbolDecision g = decision(*r, "?$TSS0@?1??fresh@@YAHXZ@4HA");
    CHECK(g.decision == DecisionKind::PatchNewData);
    CHECK(contains(g.reason, "guard variable of a new static"));
    // A guard without any static of its scope is not placed by the layout either (it gets the old warning).
    CHECK(decision(*r, "?$TSS0@?1??other@@YAHXZ@4HA").decision == DecisionKind::PatchNewData);
    REQUIRE(r->warnings.size() == 1);
    CHECK(r->warnings[0].code == diag::kGuardUnmatched);
    CHECK(contains(r->warnings[0].message, "other"));
}

TEST_CASE("guards: a guard of a bound static is placed by layout only inside the compiland, not on a recorded variable") {
    SUBCASE("the layout puts the guard on another recorded variable of the compiland: CML3600") {
        // main: bound at old 0x3018 (`s_state`).
        SynthWorld w = basic_world(false);
        w.data(w.tu(), "`anonymous namespace'::s_a", 0x10);
        w.data(w.tu(), "`anonymous namespace'::s_state", 0x18);
        w.proc_with_statics(w.tu(), "value", 0x50, {{"g", 0x14}});
        w.open();
        SynthObj o;
        const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
        o.symbol("?s_a@?A0x1@@3HA", 0x10, bss, false, false);
        o.symbol("?g@?1??value@@YAHXZ@4HA", 0x14, bss, false, false);
        o.symbol("?$TSS0@?1??value@@YAHXZ@4HA", 0x18, bss, false, false);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        auto r = plan_bind(w.input(b), &errors);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kGuardMismatch);
        CHECK(errors[0].id() == "CML3600");
        CHECK(contains(errors[0].message, "where the old image records '`anonymous namespace'::s_state'"));
    }
    SUBCASE("the layout puts the guard outside the compiland's contribution: CML3600") {
        // main: CML3211 (fresh guard) and the plan was accepted: the bound `g` would be constructed again.
        SynthWorld w = basic_world(false);
        w.data(w.tu(), "`anonymous namespace'::s_a", 0x70);
        w.proc_with_statics(w.tu(), "value", 0x50, {{"g", 0x74}});
        w.open();
        SynthObj o;
        const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
        o.symbol("?s_a@?A0x1@@3HA", 0x00, bss, false, false);
        o.symbol("?g@?1??value@@YAHXZ@4HA", 0x04, bss, false, false);
        o.symbol("?$TSS0@?1??value@@YAHXZ@4HA", 0x10, bss, false, false);  // old 0x3080: main.cpp's contribution
        Built b = o.build();
        std::vector<cml::Diag> errors;
        BindPlan partial;
        auto r = plan_bind(w.input(b), &errors, &partial);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kGuardMismatch);
        CHECK(contains(errors[0].message, "outside this translation unit's contribution"));
        CHECK(partial.warnings.empty());
    }
    SUBCASE("inside the contribution, on unrecorded data: placed, as before") {
        SynthWorld w = basic_world(false);
        w.data(w.tu(), "`anonymous namespace'::s_a", 0x10);
        w.proc_with_statics(w.tu(), "value", 0x50, {{"g", 0x14}});
        w.open();
        SynthObj o;
        const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
        o.symbol("?s_a@?A0x1@@3HA", 0x10, bss, false, false);
        o.symbol("?g@?1??value@@YAHXZ@4HA", 0x14, bss, false, false);
        o.symbol("?$TSS0@?1??value@@YAHXZ@4HA", 0x18, bss, false, false);
        Built b = o.build();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, "?$TSS0@?1??value@@YAHXZ@4HA").source == BindSource::CompilandLayout);
        CHECK(decision(*r, "?$TSS0@?1??value@@YAHXZ@4HA").old_address.rva == 0x3018);
    }
}

TEST_CASE("guards: cl numbers guards per function; a new guarded static in front of an old one does not take the old guard") {
    // cl records the guards. The edit declares a new `x` in a new first block of `value`: x gets `$TSS0`, the old `g`
    // `$TSS1`. main: `$TSS0` was matched by name to the OLD `$TSS0` (g's, initialised), so `x` was never constructed;
    // `$TSS1` had no record and became a fresh guard (CML3211), so `g` was constructed a second time. Plan accepted.
    SynthWorld w = basic_world(false);
    w.proc_with_statics(w.tu(), "value", 0x50, {{"g", 0x20}, {"$TSS0", 0x24}});
    w.open();
    SynthObj o;
    const auto own_section = [&o](const std::string& name) {
        const std::uint32_t sec = o.section(".bss", kDataComdat, 4, coff::ComdatSelection::Any);
        o.symbol(name, 0, static_cast<std::int32_t>(sec), false, false);
    };
    own_section("?x@?1??value@@YAHXZ@4HA");
    own_section("?$TSS0@?1??value@@YAHXZ@4HA");
    own_section("?g@?2??value@@YAHXZ@4HA");
    own_section("?$TSS1@?2??value@@YAHXZ@4HA");
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kGuardMismatch);
    CHECK(contains(errors[0].message, "$TSS1"));
    // The guard of the new static is new, never the old guard of that name.
    CHECK(decision(partial, "?$TSS0@?1??value@@YAHXZ@4HA").decision == DecisionKind::PatchNewData);
    CHECK(partial.warnings.empty());
}

TEST_CASE("guards: a block with a bound static and a new one whose guard has no record is refused (CML3600)") {
    // clang-cl (no guard records, a known layout): the edit adds `static Name y = make();` next to the bound `g` in the
    // same block. Which of the two guards is g's cannot be told from the names; main placed both by layout.
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "`anonymous namespace'::s_a", 0x10);
    w.proc_with_statics(w.tu(), "value", 0x50, {{"g", 0x14}});
    w.open();
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("?s_a@?A0x1@@3HA", 0x10, bss, false, false);
    o.symbol("?g@?1??value@@YAHXZ@4HA", 0x14, bss, false, false);
    o.symbol("?$TSS0@?1??value@@YAHXZ@4HA", 0x18, bss, false, false);
    o.symbol("?y@?1??value@@YAHXZ@4HA", 0x1C, bss, false, false);
    o.symbol("?$TSS1@?1??value@@YAHXZ@4HA", 0x20, bss, false, false);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(w.input(b), &errors);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(errors.size() == 2);
    for (const cml::Diag& e : errors) {
        CHECK(e.code == diag::kGuardMismatch);
        CHECK(contains(e.message, "some are bound to the running process"));
    }
}

TEST_CASE("guards: a guard with an old record stays bound next to a new static that needs no guard") {
    // cl: `static int calls = 0;` added to the block of the guarded `g`. Not refused.
    SynthWorld w = basic_world(false);
    w.proc_with_statics(w.tu(), "value", 0x50, {{"g", 0x20}, {"$TSS0", 0x24}});
    w.open();
    SynthObj o;
    const auto own_section = [&o](const std::string& name) {
        const std::uint32_t sec = o.section(".bss", kDataComdat, 4, coff::ComdatSelection::Any);
        o.symbol(name, 0, static_cast<std::int32_t>(sec), false, false);
    };
    own_section("?g@?1??value@@YAHXZ@4HA");
    own_section("?$TSS0@?1??value@@YAHXZ@4HA");
    own_section("?calls@?1??value@@YAHXZ@4HA");
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, "?$TSS0@?1??value@@YAHXZ@4HA").old_address.rva == 0x3024);
    CHECK(decision(*r, "?calls@?1??value@@YAHXZ@4HA").decision == DecisionKind::PatchNewData);
}

// ---- R-48 item 3: a new file-scope static function is not the old X::f ----------------------------------------------

TEST_CASE("static functions: a new file-scope `static int helper(int)` is not hooked onto the old `Foo::helper`") {
    SUBCASE("Foo::helper is external (a public of another decorated name)") {
        // main: decision patch-hooked, "matched record 'Foo::helper' at rva 0x1060 (compiland-static)": every caller of
        // Foo::helper would run the new static function.
        SynthWorld w;  // (not basic_world: it has a static `helper` of its own)
        w.proc(w.tu(), "Foo::helper", 0x60);
        w.pub_func("?helper@Foo@@SAHH@Z", 0x60);
        w.open();
        SynthObj o;
        o.function("?helper@@YAHH@Z", false);
        Built b = o.build();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        const SymbolDecision d = decision(*r, "?helper@@YAHH@Z");
        CHECK(d.decision == DecisionKind::PatchNewFunction);
        CHECK(r->hooks.empty());
    }
    SUBCASE("Foo is in an anonymous namespace (no public at all): the scope decides") {
        SynthWorld w;  // (not basic_world: it has a static `helper` of its own)
        w.proc(w.tu(), "`anonymous namespace'::Foo::helper", 0x60, 0x20, true);
        w.open();
        SynthObj o;
        o.function("?helper@@YAHH@Z", false);
        Built b = o.build();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, "?helper@@YAHH@Z").decision == DecisionKind::PatchNewFunction);
        CHECK(r->hooks.empty());
    }
    SUBCASE("the old static helper of the same scope is still hooked") {
        SynthWorld w;  // (not basic_world: it has a static `helper` of its own)
        w.proc(w.tu(), "Foo::helper", 0x60);
        w.pub_func("?helper@Foo@@SAHH@Z", 0x60);
        w.proc(w.tu(), "helper", 0x80, 0x10, true);
        w.open();
        SynthObj o;
        o.function("?helper@@YAHH@Z", false);
        Built b = o.build();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        const SymbolDecision d = decision(*r, "?helper@@YAHH@Z");
        CHECK(d.decision == DecisionKind::PatchHooked);
        CHECK(d.old_address.rva == 0x1080);
        CHECK(d.source == BindSource::CompilandStatic);
    }
}

// ---- R-48 item 4: pieces of an aggregate clang-cl split (`name.N`) -----------------------------------------------------

TEST_CASE("split aggregates: pieces without a debug record are refused (CML3601), not given fresh copies") {
    // clang-cl -O2: `namespace { int s_last[3]; }` becomes `?s_last@?A0x...@@3PAHA.0` .. `.2`; neither the old PDB nor
    // the object records a piece. main: all three `patch-new-data`, "has no record in compiland 0: new", plan accepted:
    // the counters restart from zero after every reload of the file.
    SynthWorld w = basic_world(true);
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("?s_last@?A0x6E7FF846@@3PAHA.0", 0x00, bss, false, false);
    o.symbol("?s_last@?A0x6E7FF846@@3PAHA.1", 0x04, bss, false, false);
    o.symbol("s_pt.0", 0x08, bss, false, false);  // the same in C
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(errors.size() == 3);
    for (const cml::Diag& e : errors) {
        CHECK(e.code == diag::kSplitAggregate);
        CHECK(e.id() == "CML3601");
    }
    CHECK(contains(errors[0].message, "is piece 0 of '(anonymous namespace)::s_last', which the compiler split into separate variables"));
    CHECK(contains(errors[2].message, "is piece 0 of 's_pt'"));
}

TEST_CASE("split aggregates: a piece the object's debug records name is a whole variable and binds as before") {
    // fixture c_statics under clang 22: `inc_state.0` is a one-field struct kept whole, with a record on both sides.
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "inc_state", 0x20);
    w.open();
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    const std::uint32_t piece = o.symbol("inc_state.0", 0x20, bss, false, false);
    o.debug_records_for({piece});
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, "inc_state.0").decision == DecisionKind::BindOldData);
    CHECK(decision(*r, "inc_state.0").old_address.rva == 0x3020);
}

TEST_CASE("split aggregates: read-only pieces stay fresh copies; the names that look like pieces") {
    CHECK(is_split_piece_name("?s_last@?A0x1@@3PAHA.0"));
    CHECK(is_split_piece_name("s_last.12"));
    CHECK(is_split_piece_name("next.counter.1"));
    CHECK_FALSE(is_split_piece_name("?s_last@?A0x1@@3PAHA"));
    CHECK_FALSE(is_split_piece_name(".compoundliteral.1"));
    CHECK_FALSE(is_split_piece_name("___asan_gen_.1"));
    CHECK_FALSE(is_split_piece_name("s_last.x"));
    CHECK_FALSE(is_split_piece_name("s_last."));
    CHECK_FALSE(is_split_piece_name(".1"));
    SynthWorld w = basic_world(true);
    SynthObj o;
    const std::int32_t rdata = static_cast<std::int32_t>(o.section(".rdata", kRdata, 0x10));
    o.symbol("?k_table@?A0x1@@3QBHB.0", 0x00, rdata, false, false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, "?k_table@?A0x1@@3QBHB.0").decision == DecisionKind::PatchFresh);
}

TEST_CASE("inlined lambdas: a record in the function is the own static's or an inlined lambda's; only the counts and the layout decide") {
    // WP-40 review: a record "sname" directly in ls_step() can be ls_step's own static or the static of a lambda that
    // clang-cl inlined there. Its name does not say which, so the function's own statics of that name are not paired by
    // name in pass A, and the lambdas' and the function's statics of the name are left to the section layout only if the
    // old records of the function and its lambdas are as many as the patch's statics of both.
    const auto object = [](SynthObj& o, bool own, bool lambda) {
        const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x50));
        o.symbol("?s_count@?A0x1@@3HA", 0x10, bss, false, false);
        if (own) {
            o.symbol("?sname@?1??ls_step@@YAHXZ@4ULsName@@A", 0x20, bss, false, false);
            o.symbol("?$TSS0@?1??ls_step@@YAHXZ@4HA", 0x28, bss, false, false);
        }
        if (lambda) {
            o.symbol(lambda_static("sname", 0), own ? 0x30 : 0x20, bss, false, false);
            o.symbol(lambda_guard(0), own ? 0x38 : 0x28, bss, false, false);
        }
    };
    SUBCASE("clang-cl -O2: the own static is NEW, the old record is the inlined lambda's: refused, never bound to it") {
        // main of WP-40's first rounds: pass A bound the new `ls_step::sname` to the lambda's live record (0x3020) and the
        // lambda's static was declared new.
        SynthWorld w = basic_world(false);
        w.data(w.tu(), "`anonymous namespace'::s_count", 0x10);
        w.proc_with_statics(w.tu(), "ls_step", 0x60, {{"sname", 0x20}});
        w.open();
        SynthObj o;
        object(o, true, true);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        BindPlan partial;
        REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
        CHECK(count_code(errors, diag::kAmbiguousStatic) >= 1);
        CHECK(contains(errors[0].message, "a lambda of 'ls_step' in the patch has a static named 'sname'"));
        CHECK(decision(partial, "?sname@?1??ls_step@@YAHXZ@4ULsName@@A").decision != DecisionKind::BindOldData);
        // (the same holds when the own static is the OLD one and the lambda is new: the records cannot tell the two apart)
    }
    SUBCASE("the function's own static and an inlined lambda's, both old, as many records as statics: refused (ADR-0043 ruling)") {
        // Until the second high review: placed by the layout. Which of the two records is the own static's is not provable
        // (the same records are left when an inlined lambda was deleted and an own static of its name declared), so the
        // own static is refused: an over-refusal the ruling accepts.
        SynthWorld w = basic_world(false);
        w.data(w.tu(), "`anonymous namespace'::s_count", 0x10);
        w.proc_with_statics(w.tu(), "ls_step", 0x60, {{"sname", 0x20}, {"sname", 0x30}});
        w.open();
        SynthObj o;
        object(o, true, true);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        BindPlan partial;
        REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
        CHECK(count_code(errors, diag::kAmbiguousStatic) >= 1);
        CHECK(decision(partial, "?sname@?1??ls_step@@YAHXZ@4ULsName@@A").decision != DecisionKind::BindOldData);
    }
    SUBCASE("L1 in its operator(), L2 inlined; the patch deletes L2 and declares the function's own static in its place: refused") {
        // The old image: <lambda_0>'s `sname` in its own operator() (0x3040), <lambda_1>'s inlined into ls_step (0x3020).
        // The patch: <lambda_0> and a new `ls_step::sname`. Until the review the record in ls_step was taken for the own
        // static's (every lambda static of the patch had a record in a call operator) and the new own static took
        // <lambda_1>'s live object.
        SynthWorld w = basic_world(false);
        w.data(w.tu(), "`anonymous namespace'::s_count", 0x10);
        w.proc_with_statics(w.tu(), "ls_step", 0x60, {{"sname", 0x20}});
        w.proc_with_statics(w.tu(), "ls_step::<lambda_0>::operator()", 0x90, {{"sname", 0x40}});
        w.open();
        SynthObj o;
        object(o, true, true);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        BindPlan partial;
        REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
        CHECK(count_code(errors, diag::kAmbiguousStatic) >= 1);
        const SymbolDecision own = decision(partial, "?sname@?1??ls_step@@YAHXZ@4ULsName@@A");
        CHECK(own.decision != DecisionKind::BindOldData);
        CHECK_FALSE(own.old_address.rva == 0x3020);
    }
    SUBCASE("the patch deletes the lambda and declares the function's own static of its static's name: refused, not bound to the lambda's") {
        // cl /O2 records the inlined lambda's `sname` in its operator() and in ls_step at one address (0x3040). The patch
        // has no lambda any more and a new `ls_step::sname`. Until the review pass A paired it by name with the record in
        // ls_step: the lambda's live object. (This test asserted that pairing, as "the own static pairs by name".)
        SynthWorld w = basic_world(false);
        w.proc_with_statics(w.tu(), "ls_step", 0x60, {{"sname", 0x40}});
        w.proc_with_statics(w.tu(), "ls_step::<lambda_0>::operator()", 0x90, {{"sname", 0x40}});
        w.open();
        SynthObj o;
        object(o, true, false);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        BindPlan partial;
        REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kAmbiguousStatic);
        CHECK(contains(errors[0].message, "a lambda of 'ls_step' in the old image has a static named 'sname'"));
        CHECK(decision(partial, "?sname@?1??ls_step@@YAHXZ@4ULsName@@A").decision != DecisionKind::BindOldData);
    }
    SUBCASE("no lambda of the function has a static of that name, in the patch or in the old image: the own static pairs by name") {
        SynthWorld w = basic_world(false);
        w.proc_with_statics(w.tu(), "ls_step", 0x60, {{"sname", 0x20}});
        w.proc_with_statics(w.tu(), "ls_step::<lambda_0>::operator()", 0x90, {{"other", 0x40}});
        w.open();
        SynthObj o;
        object(o, true, false);
        Built b = o.build();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, "?sname@?1??ls_step@@YAHXZ@4ULsName@@A").source == BindSource::CompilandStatic);
    }
}

// ---- WP-40 review: a bitmask guard `$S1` guards every block of its function -----------------------------------------

TEST_CASE("guards: a `$S1` bitmask (no thread-safe statics) guards the statics of every block of its function") {
    // cl /Zc:threadSafeInit- and clang-cl -fno-threadsafe-statics write ONE `?$S1@?1??f@@YAHXZ@4IA` per function, in the
    // scope of its first block (CI, cl 19.44; clang 18 locally). Until the review only the statics of that first block were
    // counted as its statics.
    const auto world = [](SynthWorld& w, const std::vector<std::pair<std::string, std::uint32_t>>& records) {
        w.data(w.tu(), "`anonymous namespace'::s_a", 0x10);
        w.proc_with_statics(w.tu(), "f", 0x50, records);
        w.open();
    };
    const char* const k1 = "?k@?1??f@@YAHXZ@4HA";  // block 1
    const char* const n2 = "?n@?2??f@@YAHXZ@4HA";  // block 2
    const char* const m3 = "?m@?3??f@@YAHXZ@4HA";  // block 3
    const char* const s1 = "?$S1@?1??f@@YAHXZ@4IA";
    SUBCASE("statics only in later blocks, all bound: the bitmask is placed by the layout (it was a fresh one, CML3211)") {
        SynthWorld w = basic_world(false);
        world(w, {{"n", 0x14}, {"m", 0x18}});
        SynthObj o;
        const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
        o.symbol("?s_a@?A0x1@@3HA", 0x10, bss, false, false);
        o.symbol(n2, 0x14, bss, false, false);
        o.symbol(m3, 0x18, bss, false, false);
        o.symbol(s1, 0x1C, bss, false, false);
        Built b = o.build();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, s1).decision == DecisionKind::BindOldData);
        CHECK(decision(*r, s1).source == BindSource::CompilandLayout);
        CHECK(decision(*r, s1).old_address.rva == 0x301C);
        CHECK(r->warnings.empty());
        // (ADR-0043 ruling, second high review: only if the code of f() is unchanged, which the pipeline proves.)
        REQUIRE(r->bitmask_guards.size() == 1);
        CHECK(r->bitmask_guards[0].guard == s1);
        CHECK(r->bitmask_guards[0].function == "?f@@YAHXZ");
    }
    SUBCASE("a new static in block 1, a bound one in block 2: CML3600 (the bitmask was made new: the bound one constructed again)") {
        SynthWorld w = basic_world(false);
        world(w, {{"n", 0x18}});
        SynthObj o;
        const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
        o.symbol("?s_a@?A0x1@@3HA", 0x10, bss, false, false);
        o.symbol(k1, 0x14, bss, false, false);  // new
        o.symbol(n2, 0x18, bss, false, false);  // bound
        o.symbol(s1, 0x1C, bss, false, false);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        BindPlan partial;
        REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kGuardMismatch);
        CHECK(decision(partial, n2).decision == DecisionKind::BindOldData);
        CHECK(decision(partial, s1).decision != DecisionKind::BindOldData);
    }
    SUBCASE("a bound static in block 1, a new one in block 2: CML3600 (the new one got the old bitmask, whose bit may be set)") {
        SynthWorld w = basic_world(false);
        world(w, {{"k", 0x14}});
        SynthObj o;
        const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
        o.symbol("?s_a@?A0x1@@3HA", 0x10, bss, false, false);
        o.symbol(k1, 0x14, bss, false, false);  // bound
        o.symbol(n2, 0x18, bss, false, false);  // new
        o.symbol(s1, 0x1C, bss, false, false);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        REQUIRE_FALSE(plan_bind(w.input(b), &errors).has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kGuardMismatch);
    }
}


// ---- WP-46 (R-54, ADR-0043): a lambda clang-cl inlined everywhere ----------------------------------------------------
//
// clang-cl at -O2 inlines a lambda into every caller and emits no procedure for its call operator; its static `sname` is a
// record of the caller (`o2_step`, fixture planner_pairing_o2: llvm-pdbutil shows three S_LDATA32 `sname` in o2_step), and
// nothing in the records names the lambda. When the edit deletes the lambda and declares a static `sname` of the caller
// itself, pass A paired the new static with that record by its name: the lambda's live object. The TPI keeps the closure
// class of the lambda (`.?AV<lambda_0>@?0??o2_step@@YAXXZ@`, a forward reference and the definition, even when inlined);
// cml_pdb's closure_classes() lists them, and an own static of a function whose old image has a closure without a call
// operator procedure is refused (CML3207).
namespace {
const char* const kO2Own = "?sname@?1??o2_step@@YAXXZ@4UO2Name@@A";
const char* const kO2OwnGuard = "?$TSS0@?1??o2_step@@YAXXZ@4HA";

// The old o2_step with one record `sname` (data 0x20) and the closure class of its lambda in the TPI. `compiler` is the
// compiland's S_COMPILE3 version; `with_operator`: the lambda's call operator is a procedure of the compiland (not inlined);
// `closure_function`: the function the closure belongs to.
SynthWorld o2_world(const std::string& compiler, bool with_operator, const std::string& closure_function = "?o2_step@@YAXXZ",
                    const std::string& closure_name = "o2_step::<lambda_0>") {
    SynthWorld w = basic_world(false);
    tp::Module& tu = w.tu();
    tu.symbols = tp::Records();
    tu.symbols.objname(tu.name);
    tu.symbols.compile3(1, 0, 0xD0, compiler);
    w.proc(w.tu(), "compute_value", 0x10);
    w.proc(w.tu(), "helper", 0x40, 0x10, true);
    tp::TypeSet types;
    const std::string unique = ".?AV<lambda_0>@?0?" + closure_function + "@";
    const std::uint32_t forward = types.add(tp::kLfClass, tp::lf_udt(closure_name, unique, true));
    types.add(tp::kLfClass, tp::lf_udt(closure_name, unique, false, 1));
    const std::uint32_t this_ptr = types.add(tp::kLfPointer, tp::lf_pointer(forward, tp::kPtr64 | 0x400));
    const std::uint32_t args = types.add(tp::kLfArgList, tp::lf_arglist({}));
    const std::uint32_t op_type = types.add(tp::kLfMFunction, tp::lf_mfunction(tp::kTInt, forward, this_ptr, args, 0));
    types.add_to(w.spec.tpi);
    w.proc_with_statics(w.tu(), "o2_step", 0x60, {{"sname", 0x20}});
    if (with_operator) {
        w.tu().symbols.proc(0x110F, "o2_step::<lambda_0>::operator()", SynthWorld::kTextSec, 0x90, 0x10, 0, op_type);
        w.tu().symbols.end();
    }
    w.pub_func("?o2_step@@YAXXZ", 0x60);
    w.open();
    return w;
}
Built o2_own_static_object() {
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol(kO2Own, 0x20, bss, false, false);
    o.symbol(kO2OwnGuard, 0x28, bss, false, false);
    return o.build();
}
}  // namespace

TEST_CASE("R-54 (WP-46): an own static named like the static of a lambda clang-cl inlined everywhere is refused, never bound to its record") {
    SUBCASE("clang-cl, the lambda has no call operator procedure: the record in o2_step may be the lambda's (CML3207)") {
        // main: `o2_step::sname` paired by name with the record at 0x3020, the deleted lambda's live object (compiland-static).
        SynthWorld w = o2_world("clang version 18.1.3 (https://github.com/llvm/llvm-project)", false);
        Built b = o2_own_static_object();
        std::vector<cml::Diag> errors;
        BindPlan partial;
        REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kAmbiguousStatic);
        CHECK_MESSAGE(contains(errors[0].message, "whose call operator the compiler inlined everywhere"), errors[0].message);
        CHECK(contains(errors[0].message, "o2_step::<lambda_0>"));
        CHECK(decision(partial, kO2Own).decision != DecisionKind::BindOldData);
        CHECK(decision(partial, kO2OwnGuard).decision != DecisionKind::BindOldData);
    }
    SUBCASE("clang-cl, the lambda's call operator is a procedure of the compiland: its statics would be recorded there; pairs by name") {
        SynthWorld w = o2_world("clang version 18.1.3 (https://github.com/llvm/llvm-project)", true);
        Built b = o2_own_static_object();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, kO2Own).source == BindSource::CompilandStatic);
        CHECK(decision(*r, kO2Own).old_address.rva == 0x3020);
    }
    SUBCASE("clang-cl, the closure belongs to another function: pairs by name") {
        SynthWorld w = o2_world("clang version 18.1.3 (https://github.com/llvm/llvm-project)", false, "?o2_other@@YAXXZ", "o2_other::<lambda_0>");
        Built b = o2_own_static_object();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, kO2Own).old_address.rva == 0x3020);
    }
    SUBCASE("cl (which emits the call operators it inlines, WP-40): not looked at; pairs by name") {
        SynthWorld w = o2_world("Microsoft (R) Optimizing Compiler", false);
        Built b = o2_own_static_object();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, kO2Own).old_address.rva == 0x3020);
    }
}

TEST_CASE("R-54 (WP-46 review): the static's function is read from its parsed name, never from a guessed \"@4\" in its type") {
    // The type of the static holds a back-reference "@4@": 6b0a7de cut the scope at the last "@4", got the wrong function,
    // found no closure of it and paired the static with the inlined lambda's record.
    SynthWorld w = o2_world("clang version 18.1.3 (https://github.com/llvm/llvm-project)", false);
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    const std::string own = "?sname@?1??o2_step@@YAXXZ@4PEAV?$vec@H@4@A";
    o.symbol(own, 0x20, bss, false, false);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kAmbiguousStatic);
    CHECK(decision(partial, own).decision != DecisionKind::BindOldData);
}

TEST_CASE("R-54 (WP-46 review): a static of a lambda's call operator, and an inner lambda inlined into that operator") {
    // clang inlines a lambda into the lambda that calls it as well: the old outer call operator records the inner lambda's
    // static `sname`, and the patch (inner lambda deleted) declares a static `sname` of the outer call operator itself.
    // 6b0a7de did not look at the statics of call operators and paired it with the inner lambda's record.
    SynthWorld w = basic_world(false);
    tp::Module& tu = w.tu();
    tu.symbols = tp::Records();
    tu.symbols.objname(tu.name);
    tu.symbols.compile3(1, 0, 0xD0, "clang version 18.1.3 (https://github.com/llvm/llvm-project)");
    tp::TypeSet types;
    const std::string outer = ".?AV<lambda_0>@?0??o2_step@@YAXXZ@";
    const std::uint32_t outer_fwd = types.add(tp::kLfClass, tp::lf_udt("o2_step::<lambda_0>", outer, true));
    types.add(tp::kLfClass, tp::lf_udt("o2_step::<lambda_0>", outer, false, 1));
    types.add(tp::kLfClass, tp::lf_udt("o2_step::<lambda_0>::operator()::<lambda_1>", ".?AV<lambda_1>@?0???R<lambda_0>@?0??o2_step@@YAXXZ@QEBA@XZ@", false, 1));
    const std::uint32_t this_ptr = types.add(tp::kLfPointer, tp::lf_pointer(outer_fwd, tp::kPtr64 | 0x400));
    const std::uint32_t args = types.add(tp::kLfArgList, tp::lf_arglist({}));
    const std::uint32_t op_type = types.add(tp::kLfMFunction, tp::lf_mfunction(tp::kTVoid, outer_fwd, this_ptr, args, 0));
    types.add_to(w.spec.tpi);
    w.tu().symbols.proc(0x110F, "o2_step::<lambda_0>::operator()", SynthWorld::kTextSec, 0x60, 0x10, 0, op_type);
    w.tu().symbols.data(0x110C, "sname", SynthWorld::kDataSec, 0x20);
    w.tu().symbols.end();
    w.open();
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    const std::string own = "?sname@?1???R<lambda_0>@?0??o2_step@@YAXXZ@QEBA@XZ@4UO2Name@@A";
    o.symbol(own, 0x20, bss, false, false);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kAmbiguousStatic);
    CHECK_MESSAGE(contains(errors[0].message, "o2_step::<lambda_0>::operator()::<lambda_1>"), errors[0].message);
    CHECK(decision(partial, own).decision != DecisionKind::BindOldData);
}
