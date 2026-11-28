// WP-51 (R-54, ADR-0044): an own static never takes the static of a lambda the compiler inlined and the edit deleted.
//
// clang-cl at -O2 records the static of an inlined call operator as a plain record of the calling procedure, under its own
// name, next to the procedure's S_INLINESITE records, and emits no procedure for the call operator (docs/wp/WP-51.md, round
// 1: o2_step of the fixture planner_pairing_o2). An edit that deletes the lambda and declares a static of that name in the
// function leaves nothing in the patch that names the lambda; the record in the function is then the only candidate of the
// new static, and main paired them: the new static took the deleted lambda's live object. The planner now reads what the
// old procedure inlined and refuses such a pairing (CML3207) when it inlined a lambda's code.
//
// Every refusal case below failed on main (23bbe32): main paired the static with the record. The pairing cases (a function
// that inlined only other code: Jolt's GetRTTIOfType, ImGui's C-linkage main) pair on main and still do.
#include <string>
#include <utility>
#include <vector>

#include "cml/core/bind_diag_ids.h"
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

tp::Bytes site(std::uint32_t inlinee) {
    tp::Bytes b;
    tp::put32(b, 0);  // parent
    tp::put32(b, 0);  // end
    tp::put32(b, inlinee);
    b.push_back(0x0B);  // annotations
    b.push_back(0x04);
    return b;
}

// The types and ids of the old image (what clang 18 -O2 + lld wrote for the repros of docs/wp/WP-51.md).
struct OldTypes {
    tp::TypeSet tpi;
    std::uint32_t args = 0;
    std::uint32_t fn_void = 0;
    std::uint32_t fn_int = 0;

    OldTypes() {
        args = tpi.add(tp::kLfArgList, tp::lf_arglist({}));
        fn_void = tpi.add(tp::kLfProcedure, tp::lf_procedure(tp::kTVoid, args, 0));
        fn_int = tpi.add(tp::kLfProcedure, tp::lf_procedure(tp::kTInt, args, 0));
    }
    // A member function `name` of class `cls` (name, unique name): its LF_MFUNC_ID in `ipi`.
    std::uint32_t member(tp::Ipi& ipi, const std::string& cls, const std::string& unique, const std::string& name) {
        const std::uint32_t c = tpi.add(tp::kLfClass, tp::lf_udt(cls, unique, true));
        const std::uint32_t this_ptr = tpi.add(tp::kLfPointer, tp::lf_pointer(c, tp::kPtr64 | 0x400));
        const std::uint32_t fn = tpi.add(tp::kLfMFunction, tp::lf_mfunction(tp::kTInt, c, this_ptr, args, 0));
        return ipi.record(tp::kLfMFuncId, tp::lf_mfunc_id(c, fn, name));
    }
    // A free function `name` taking `params` (type indices).
    std::uint32_t function(tp::Ipi& ipi, const std::string& name, const std::vector<std::uint32_t>& params = {}) {
        const std::uint32_t list = params.empty() ? args : tpi.add(tp::kLfArgList, tp::lf_arglist(params));
        const std::uint32_t fn = params.empty() ? fn_int : tpi.add(tp::kLfProcedure, tp::lf_procedure(tp::kTInt, list, static_cast<std::uint16_t>(params.size())));
        return ipi.record(tp::kLfFuncId, tp::lf_func_id(fn, name));
    }
};

// A procedure of the TU with function-local static records and inline sites: each chain is a nest of sites, outermost first.
void proc_inlining(SynthWorld& w, const std::string& name, std::uint32_t text_offset, std::uint32_t type,
                   const std::vector<std::pair<std::string, std::uint32_t>>& statics, const std::vector<std::vector<std::uint32_t>>& chains) {
    tp::Records& r = w.tu().symbols;
    r.proc(0x1110, name, SynthWorld::kTextSec, text_offset, 0x30, 0, type);
    for (const auto& s : statics) {
        r.data(0x110C, s.first, SynthWorld::kDataSec, s.second);
    }
    for (const std::vector<std::uint32_t>& chain : chains) {
        for (const std::uint32_t id : chain) {
            r.add(0x114D, site(id));
        }
        for (std::size_t k = 0; k < chain.size(); ++k) {
            r.add(0x114E, tp::Bytes());
        }
    }
    r.end();
}

void open_with(SynthWorld& w, const OldTypes& t) {
    t.tpi.add_to(w.spec.tpi);
    w.open();
}

// planner_pairing_o2's o2_step reduced to one lambda, and the R-54 edit (docs/wp/WP-51.md: r54_old.cpp -> r54_new.cpp).
const char* const kOwn = "?sname@?1??o2_step@@YAXXZ@4UO2Name@@A";
const char* const kOwnGuard = "?$TSS0@?1??o2_step@@YAXXZ@4HA";
const char* const kClosure = ".?AV<lambda_0>@?0??o2_step@@YAXXZ@";

// The patch: `static O2Name sname` declared in o2_step itself (at .bss 0x20, its guard at 0x28), the lambda gone.
Built own_static_patch() {
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol(kOwn, 0x20, bss, false, false);
    o.symbol(kOwnGuard, 0x28, bss, false, false);
    return o.build();
}

void check_refused(SynthWorld& w, const std::string& reason) {
    Built b = own_static_patch();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(count_code(errors, diag::kAmbiguousStatic) == 1);
    CAPTURE(errors[0].message);
    CHECK(errors[0].id() == "CML3207");
    CHECK(contains(errors[0].message, "o2_step::sname"));
    CHECK(contains(errors[0].message, reason));
    CHECK(contains(errors[0].message, "may be that code's static and not this one"));
    CHECK(decision(partial, kOwn).decision != DecisionKind::BindOldData);
    CHECK(decision(partial, kOwnGuard).decision != DecisionKind::BindOldData);
}

}  // namespace

TEST_CASE("R-54: an own static is not paired with the record of a lambda its function inlined (o2_step, clang-cl -O2)") {
    // main: 'sname' paired with the lambda's record at 0x3020 (compiland-static) and its guard placed by the layout on the
    // lambda's guard: the new static used the deleted lambda's live object, never constructed again.
    SynthWorld w = basic_world(false);
    OldTypes t;
    const std::uint32_t call = t.member(w.spec.ipi, "o2_step::<lambda_0>", kClosure, "operator()");
    const std::uint32_t make = t.function(w.spec.ipi, "o2_make");
    proc_inlining(w, "o2_step", 0x60, t.fn_void, {{"sname", 0x20}}, {{call, make}});
    open_with(w, t);
    check_refused(w, "inlined 'o2_step::<lambda_0>::operator()', a lambda's code");
}

TEST_CASE("R-54: the lambda may be another function's, inlined through it, at any depth") {
    // `static int d_g() { return []{ static Obj dx; ... }(); }` inlined into the caller: clang records dx in the caller,
    // under the inline site of d_g (docs/wp/WP-51.md, case d_caller). main: paired.
    SynthWorld w = basic_world(false);
    OldTypes t;
    const std::uint32_t helper = t.function(w.spec.ipi, "d_g");
    const std::uint32_t call = t.member(w.spec.ipi, "d_g::<lambda_8>", ".?AV<lambda_8>@?0??d_g@@YAHXZ@", "operator()");
    proc_inlining(w, "o2_step", 0x60, t.fn_void, {{"sname", 0x20}}, {{helper, call}});
    open_with(w, t);
    check_refused(w, "inlined 'd_g::<lambda_8>::operator()'");
}

TEST_CASE("R-54: a lambda's invoker, a template over a closure and an unknown closure member count as the lambda's code") {
    SUBCASE("__invoke") {
        SynthWorld w = basic_world(false);
        OldTypes t;
        const std::uint32_t invoke = t.member(w.spec.ipi, "o2_step::<lambda_0>", kClosure, "__invoke");
        proc_inlining(w, "o2_step", 0x60, t.fn_void, {{"sname", 0x20}}, {{invoke}});
        open_with(w, t);
        check_refused(w, "inlined 'o2_step::<lambda_0>::__invoke'");
    }
    SUBCASE("cl's invoker") {
        SynthWorld w = basic_world(false);
        OldTypes t;
        const std::uint32_t invoke = t.member(w.spec.ipi, "o2_step::<lambda_0>", kClosure, "<lambda_invoker_cdecl>");
        proc_inlining(w, "o2_step", 0x60, t.fn_void, {{"sname", 0x20}}, {{invoke}});
        open_with(w, t);
        check_refused(w, "a lambda's code");
    }
    SUBCASE("a member of the closure the planner does not know") {
        SynthWorld w = basic_world(false);
        OldTypes t;
        const std::uint32_t odd = t.member(w.spec.ipi, "o2_step::<lambda_0>", kClosure, "operator()<int>");
        proc_inlining(w, "o2_step", 0x60, t.fn_void, {{"sname", 0x20}}, {{odd}});
        open_with(w, t);
        check_refused(w, "inlined 'o2_step::<lambda_0>::operator()<int>'");
    }
    SUBCASE("a template over the closure: `template <class F> int run(F)` (the LF_FUNC_ID is named 'run' only)") {
        SynthWorld w = basic_world(false);
        OldTypes t;
        const std::uint32_t closure = t.tpi.add(tp::kLfClass, tp::lf_udt("o2_step::<lambda_0>", kClosure, true));
        const std::uint32_t run = t.function(w.spec.ipi, "run", {closure});
        proc_inlining(w, "o2_step", 0x60, t.fn_void, {{"sname", 0x20}}, {{run}});
        open_with(w, t);
        check_refused(w, "inlined 'run', a function over a closure type");
    }
}

TEST_CASE("R-54: inline sites that cannot be read are a risk") {
    SUBCASE("the inlinee is not in the IPI") {
        SynthWorld w = basic_world(false);
        OldTypes t;
        proc_inlining(w, "o2_step", 0x60, t.fn_void, {{"sname", 0x20}}, {{0x1FFF}});
        open_with(w, t);
        check_refused(w, "names an inlinee that cannot be read");
    }
    SUBCASE("the inlinee's type cannot be read") {
        SynthWorld w = basic_world(false);
        OldTypes t;
        const std::uint32_t broken = w.spec.ipi.record(tp::kLfFuncId, tp::lf_func_id(0x7FFF, "helper"));
        proc_inlining(w, "o2_step", 0x60, t.fn_void, {{"sname", 0x20}}, {{broken}});
        open_with(w, t);
        check_refused(w, "cannot be shown not to be a template over a closure");
    }
}

TEST_CASE("R-54: the statics of a call operator are refused when it inlined another lambda") {
    // An outer lambda with its own procedure and a static `sname`, which inlined an inner lambda: a record `sname` in the
    // outer call operator may be the inner one's. main: paired by pass A.
    SynthWorld w = basic_world(false);
    OldTypes t;
    const std::uint32_t outer_cls = t.tpi.add(tp::kLfClass, tp::lf_udt("ls_step::<lambda_0>", ".?AV<lambda_0>@?0??ls_step@@YAHXZ@", true));
    const std::uint32_t this_ptr = t.tpi.add(tp::kLfPointer, tp::lf_pointer(outer_cls, tp::kPtr64 | 0x400));
    const std::uint32_t outer_fn = t.tpi.add(tp::kLfMFunction, tp::lf_mfunction(tp::kTInt, outer_cls, this_ptr, t.args, 0));
    const std::uint32_t inner = t.member(w.spec.ipi, "ls_step::<lambda_0>::operator()::<lambda_1>",
                                         ".?AV<lambda_1>@?0???R<lambda_0>@?0??ls_step@@YAHXZ@QEBA?A?<auto>@@XZ@", "operator()");
    proc_inlining(w, "ls_step::<lambda_0>::operator()", 0x60, outer_fn, {{"sname", 0x20}}, {{inner}});
    open_with(w, t);
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    const std::string own = "?sname@?1???R<lambda_0>@?0??ls_step@@YAHXZ@QEBA@XZ@4ULsName@@A";
    o.symbol(own, 0x20, bss, false, false);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(count_code(errors, diag::kAmbiguousStatic) == 1);
    CAPTURE(errors[0].message);
    CHECK(contains(errors[0].message, "a lambda's code"));
    CHECK(decision(partial, own).decision != DecisionKind::BindOldData);
}

TEST_CASE("R-54: a pairing made by the section layout in pass B is checked as well") {
    // Two records `x` in f (its own static and an inlined lambda's), one own static in the patch, and a file-scope static
    // that teaches the layout: main paired `x` with the record at the address the layout predicts (compiland-static-rva).
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "`anonymous namespace'::s_count", 0x10);
    OldTypes t;
    const std::uint32_t call = t.member(w.spec.ipi, "f::<lambda_0>", ".?AV<lambda_0>@?0??f@@YAHXZ@", "operator()");
    proc_inlining(w, "f", 0x60, t.fn_int, {{"x", 0x20}, {"x", 0x30}}, {{call}});
    open_with(w, t);
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("?s_count@?A0x1@@3HA", 0x10, bss, false, false);
    o.symbol("?x@?1??f@@YAHXZ@4HA", 0x20, bss, false, false);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(count_code(errors, diag::kAmbiguousStatic) == 1);
    CAPTURE(errors[0].message);
    CHECK(contains(errors[0].message, "inlined 'f::<lambda_0>::operator()'"));
    CHECK(decision(partial, "?x@?1??f@@YAHXZ@4HA").decision != DecisionKind::BindOldData);
    CHECK(decision(partial, "?s_count@?A0x1@@3HA").decision == DecisionKind::BindOldData);
}

TEST_CASE("R-54: an own static of a function that inlined no lambda still pairs (Jolt's GetRTTIOfType, ImGui's main)") {
    SUBCASE("GetRTTIOfType inlined the RTTI constructor only (its lambdas are inlined into their own __invoke)") {
        SynthWorld w = basic_world(false);
        OldTypes t;
        const std::uint32_t ctor = t.member(w.spec.ipi, "JPH::RTTI", ".?AVRTTI@JPH@@", "RTTI");
        proc_inlining(w, "JPH::GetRTTIOfType", 0x60, t.fn_void, {{"rtti", 0x20}}, {{ctor}});
        // The closures' invokers, each with its call operator inlined, are procedures of their own.
        const std::uint32_t call = t.member(w.spec.ipi, "JPH::GetRTTIOfType::<lambda_1>", ".?AV<lambda_1>@?0??GetRTTIOfType@JPH@@YAPEAVRTTI@2@PEAVSphereShape@2@@Z@", "operator()");
        proc_inlining(w, "JPH::GetRTTIOfType::<lambda_1>::__invoke", 0x90, t.fn_void, {}, {{call}});
        open_with(w, t);
        SynthObj o;
        const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
        const std::string rtti = "?rtti@?1??GetRTTIOfType@JPH@@YAPEAVRTTI@2@PEAVSphereShape@2@@Z@4V32@A";
        o.symbol(rtti, 0x20, bss, false, false);
        Built b = o.build();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, rtti).decision == DecisionKind::BindOldData);
        CHECK(decision(*r, rtti).source == BindSource::CompilandStatic);
        CHECK(decision(*r, rtti).old_address.rva == 0x3020);
    }
    SUBCASE("a C-linkage main that inlined a free function and a closure's conversion operator") {
        SynthWorld w = basic_world(false);
        OldTypes t;
        const std::uint32_t helper = t.function(w.spec.ipi, "ImMax");
        const std::uint32_t conversion = t.member(w.spec.ipi, "main::<lambda_0>", ".?AV<lambda_0>@?0??main@@9@", "operator int (__cdecl *)(void)");
        proc_inlining(w, "main", 0x60, t.fn_int, {{"counter", 0x20}}, {{helper}, {conversion}});
        open_with(w, t);
        SynthObj o;
        const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
        o.symbol("?counter@?1??main@@9@4HA", 0x20, bss, false, false);
        Built b = o.build();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, "?counter@?1??main@@9@4HA").decision == DecisionKind::BindOldData);
        CHECK(decision(*r, "?counter@?1??main@@9@4HA").old_address.rva == 0x3020);
    }
    SUBCASE("a function without inline sites") {
        SynthWorld w = basic_world(false);
        OldTypes t;
        proc_inlining(w, "o2_step", 0x60, t.fn_void, {{"sname", 0x20}}, {});
        open_with(w, t);
        Built b = own_static_patch();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, kOwn).decision == DecisionKind::BindOldData);
        CHECK(decision(*r, kOwn).old_address.rva == 0x3020);
        CHECK(decision(*r, kOwnGuard).old_address.rva == 0x3028);
    }
}

TEST_CASE("R-54: WP-40's case is unchanged: the statics of inlined lambdas are still placed by the layout in their caller") {
    // The R-48 item 1 world of test_pairing.cpp with the inline sites clang writes: the lambdas' statics are not own statics
    // of ls_step, so the inline sites do not refuse them.
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "`anonymous namespace'::s_count", 0x10);
    OldTypes t;
    const std::uint32_t l0 = t.member(w.spec.ipi, "ls_step::<lambda_0>", ".?AV<lambda_0>@?0??ls_step@@YAHXZ@", "operator()");
    const std::uint32_t l1 = t.member(w.spec.ipi, "ls_step::<lambda_1>", ".?AV<lambda_1>@?0??ls_step@@YAHXZ@", "operator()");
    proc_inlining(w, "ls_step", 0x60, t.fn_int, {{"sname", 0x20}, {"sname", 0x30}}, {{l0}, {l1}});
    open_with(w, t);
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("?s_count@?A0x1@@3HA", 0x10, bss, false, false);
    for (int n = 0; n < 2; ++n) {
        const std::string scope = "@?1???R<lambda_" + std::to_string(n) + ">@?0??ls_step@@YAHXZ@QEBA@XZ@";
        o.symbol("?sname" + scope + "4ULsName@@A", 0x20 + 0x10 * static_cast<std::uint32_t>(n), bss, false, false);
        o.symbol("?$TSS0" + scope + "4HA", 0x28 + 0x10 * static_cast<std::uint32_t>(n), bss, false, false);
    }
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    for (int n = 0; n < 2; ++n) {
        CAPTURE(n);
        const std::string name = "?sname@?1???R<lambda_" + std::to_string(n) + ">@?0??ls_step@@YAHXZ@QEBA@XZ@4ULsName@@A";
        CHECK(decision(*r, name).decision == DecisionKind::BindOldData);
        CHECK(decision(*r, name).source == BindSource::CompilandRva);
        CHECK(decision(*r, name).old_address.rva == 0x3020 + 0x10 * static_cast<std::uint32_t>(n));
    }
}

// ---- review of 2a3580d, finding 1: nested lambdas ----------------------------------------------------------------------
//
// clang 18 -O2 for `void g() { auto o = []() __attribute__((noinline)) { auto i = [] { static Obj gx = make(1); return gx.v; };
// return i() + 1; }; sink(o()); }` (llvm-pdbutil, docs/wp/WP-51.md): the outer call operator keeps its procedure, the inner one
// is inlined into it, and the inner lambda's `gx` is a record of the OUTER call operator, next to the inner closure's inline site.

namespace {

const char* const kOuterClosure = ".?AV<lambda_0>@?0??g@@YAXXZ@";
const char* const kInnerClosure = ".?AV<lambda_1>@?0???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@";

// The old image: `s_count` (teaches the layout), the outer call operator with the record `gx` and the inner one inlined.
// With `with_k` (WP-53 review): a second lambda k's call operator holds o's `gx` as well (o inlined into k), and k's type
// cannot be read.
void nested_world(SynthWorld& w, bool with_k = false) {
    w.data(w.tu(), "`anonymous namespace'::s_count", 0x10);
    OldTypes t;
    const std::uint32_t outer_cls = t.tpi.add(tp::kLfClass, tp::lf_udt("g::<lambda_0>", kOuterClosure, true));
    const std::uint32_t this_ptr = t.tpi.add(tp::kLfPointer, tp::lf_pointer(outer_cls, tp::kPtr64 | 0x400));
    const std::uint32_t outer_fn = t.tpi.add(tp::kLfMFunction, tp::lf_mfunction(tp::kTInt, outer_cls, this_ptr, t.args, 0));
    const std::uint32_t inner = t.member(w.spec.ipi, "g::<lambda_0>::operator()::<lambda_1>", kInnerClosure, "operator()");
    proc_inlining(w, "g::<lambda_0>::operator()", 0x60, outer_fn, {{"gx", 0x20}}, {{inner}});
    if (with_k) {
        proc_inlining(w, "g::<lambda_2>::operator()", 0x90, 0x7777, {{"gx", 0x20}}, {});
    }
    open_with(w, t);
}

}  // namespace

TEST_CASE("R-54 review: an inner lambda's static recorded in the outer call operator is still bound (WP-40), not refused") {
    // 2a3580d: CML3207 (every call operator was taken for the static's own function). main: bound by the layout.
    SynthWorld w = basic_world(false);
    nested_world(w);
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("?s_count@?A0x1@@3HA", 0x10, bss, false, false);
    const std::string gx = "?gx@?1???R<lambda_1>@?0???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?4@XZ@4UObj@@A";
    // (Its guard: the R-60 tests below.)
    o.symbol(gx, 0x20, bss, false, false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, gx).decision == DecisionKind::BindOldData);
    CHECK(decision(*r, gx).old_address.rva == 0x3020);
    // WP-53: the record is in the outer call operator, which its function's own statics would have too: proven by code.
    REQUIRE(r->inlined_lambda_statics.size() == 1);
    CHECK(r->inlined_lambda_statics[0].symbol == gx);
    CHECK(r->inlined_lambda_statics[0].proc == "g::<lambda_0>::operator()");
}

TEST_CASE("R-61 review (WP-53): a record of another closure's call operator at the same address is not a twin") {
    // clang-cl /O2: the outer lambda o of g keeps its call operator and is also inlined into another lambda k, so a static of
    // o has a record in o's operator() and in k's operator(), at one address. A static of an inner lambda declared in o (after
    // an edit that deleted o's static of that name) pairs with one of them. bb63263 took the other for cl's twin record (any
    // record in a procedure named `operator()` at that address) and listed nothing: the pairing was never proven. A twin is a
    // record in a call operator of the static's OWN closure; k's closure is not that (here its type cannot even be read).
    SynthWorld w = basic_world(false);
    nested_world(w, true);  // o = g::<lambda_0>::operator() with `gx` at 0x20, the inner closure inlined; k with gx too
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("?s_count@?A0x1@@3HA", 0x10, bss, false, false);
    const std::string gx = "?gx@?1???R<lambda_1>@?0???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?4@XZ@4UObj@@A";
    o.symbol(gx, 0x20, bss, false, false);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    const BindPlan& p = r ? *r : partial;
    const SymbolDecision d = decision(p, gx);
    if (d.decision == DecisionKind::BindOldData) {
        // Bound to one of the two records: the pairing must wait for the proof.
        REQUIRE(p.inlined_lambda_statics.size() == 1);
        CHECK(p.inlined_lambda_statics[0].symbol == gx);
        CHECK(p.inlined_lambda_statics[0].record_rva == 0x3020);
    }
    CHECK(d.decision == DecisionKind::BindOldData);  // (the shape this test is about: the layout pairs it)
}

// ---- WP-52 (R-60, ADR-0045): the nested lambda's guard is tied to its bound static -------------------------------------

namespace {

// clang 18's names for the nested case (x86_64-pc-windows-msvc -O2, docs/wp/WP-52.md): the guard does not count its own
// name in the back-reference table, the static counts `gx`, so the same scope reads `?A?3@` in one and `?A?4@` in the other.
const char* const kNestedGx = "?gx@?1???R<lambda_1>@?0???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?4@XZ@4UObj@@A";
const char* const kNestedGuard = "?$TSS0@?1???R<lambda_1>@?0???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?3@XZ@4HA";
// S_COMPILE3 version strings: the planner reads the spelling from the PATCH object's (convergence ruling B).
const char* const kClang18 = "clang version 18.1.3";
const char* const kClang19 = "clang version 19.1.5";
const char* const kClang19Dev = "clang version 19.0.0git (https://github.com/llvm/llvm-project 1234abcd)";
const char* const kCl = "Microsoft (R) Optimizing Compiler";

// The patch of nested_world: s_count, the inner lambda's `gx` (0x20) and the guard `guard` (0x28), written by `compiler`
// (none: no S_COMPILE3).
Built nested_patch(const std::string& guard, const char* compiler = kClang18) {
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("?s_count@?A0x1@@3HA", 0x10, bss, false, false);
    o.symbol(kNestedGx, 0x20, bss, false, false);
    o.symbol(guard, 0x28, bss, false, false);
    if (compiler != nullptr) {
        o.compile3(compiler);
    }
    return o.build();
}

struct GuardCase {
    std::vector<cml::Diag> errors;
    BindPlan partial;
    cml::Result<BindPlan> r = cml::Result<BindPlan>(BindPlan{});
};

// A world (module compiled by `module_compiler`) whose TU has the file-scope `s_a` (0x10, teaches the layout) and the
// procedure `function` with the static records `statics`; the patch (written by `patch_compiler`, none: no S_COMPILE3)
// defines `s_a` and `symbols` (name, .bss offset).
GuardCase plan_guards(const char* module_compiler, const char* patch_compiler, const std::string& function,
                      const std::vector<std::pair<std::string, std::uint32_t>>& statics, const std::vector<std::pair<std::string, std::uint32_t>>& symbols) {
    SynthWorld w = basic_world(false, module_compiler);
    w.data(w.tu(), "`anonymous namespace'::s_a", 0x10);
    w.proc_with_statics(w.tu(), function, 0x50, statics);
    w.open();
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("?s_a@?A0x1@@3HA", 0x10, bss, false, false);
    for (const auto& [name, offset] : symbols) {
        o.symbol(name, offset, bss, false, false);
    }
    if (patch_compiler != nullptr) {
        o.compile3(patch_compiler);
    }
    Built b = o.build();
    GuardCase c;
    c.r = plan_bind(w.input(b), &c.errors, &c.partial);
    return c;
}

// Refused with exactly one CML3211, which names `static_name` and says `reason`.
void check_static_refused(const GuardCase& c, const std::string& static_name, const std::string& reason) {
    REQUIRE_FALSE(c.r.has_value());
    REQUIRE(count_code(c.errors, diag::kGuardUnmatched) == 1);
    const cml::Diag* e = nullptr;
    for (const cml::Diag& d : c.errors) {
        e = d.code == diag::kGuardUnmatched ? &d : e;
    }
    CAPTURE(e->message);
    CHECK(e->id() == "CML3211");
    CHECK(contains(e->message, "the function-local static"));
    CHECK(contains(e->message, static_name));
    CHECK(contains(e->message, "no guard of the patch that is bound to the old one is tied to it"));
    CHECK(contains(e->message, reason));
    CHECK(contains(e->message, "initialiser would run again"));
    CHECK_FALSE(contains(e->message, "same function"));
    CHECK(count_code(c.partial.warnings, diag::kGuardUnmatched) == 0);
}

// JPH::GetRTTIOfType(JPH::SphereShape *)'s `rtti` and its guard as cl and clang >= 19.1 write them ($TSS0 counted: `2` is
// JPH in both), and the same guard as clang 18 writes it (`1` is JPH).
const char* const kRtti = "?rtti@?1??GetRTTIOfType@JPH@@YAPEAVRTTI@2@PEAVSphereShape@2@@Z@4V32@A";
const char* const kRttiGuardCounted = "?$TSS0@?1??GetRTTIOfType@JPH@@YAPEAVRTTI@2@PEAVSphereShape@2@@Z@4HA";
const char* const kRttiGuardNotCounted = "?$TSS0@?1??GetRTTIOfType@JPH@@YAPEAVRTTI@1@PEAVSphereShape@1@@Z@4HA";

}  // namespace

TEST_CASE("R-60: the guard of a nested lambda's static is tied to the static and placed with it (clang 18 names)") {
    // main (d110b5d): the guard was tied to no static (its scope is spelled with other back-references), so it got a fresh
    // guard with the warning CML3211 while `gx` stayed bound: the patch ran gx's initialiser again over the live object.
    SynthWorld w = basic_world(false, kClang18);
    nested_world(w);
    Built b = nested_patch(kNestedGuard);
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, kNestedGx).decision == DecisionKind::BindOldData);
    CHECK(decision(*r, kNestedGx).old_address.rva == 0x3020);
    const SymbolDecision guard = decision(*r, kNestedGuard);
    CAPTURE(guard.reason);
    CHECK(guard.decision == DecisionKind::BindOldData);
    CHECK(guard.source == BindSource::CompilandLayout);
    CHECK(guard.old_address.rva == 0x3028);
    CHECK(count_code(r->warnings, diag::kGuardUnmatched) == 0);
}

TEST_CASE("R-60 review: the patch object's compiler decides the spelling, not the module's") {
    SUBCASE("patch by clang 19.1, module by clang 18: the patch's spelling ties the guard") {
        // In clang 18's reading `2` would be RTTI: another scope, and the bound rtti would be refused.
        GuardCase c = plan_guards(kClang18, kClang19, "JPH::GetRTTIOfType", {{"rtti", 0x14}}, {{kRtti, 0x14}, {kRttiGuardCounted, 0x18}});
        REQUIRE_MESSAGE(c.r.has_value(), err(c.r));
        CHECK(decision(*c.r, kRtti).decision == DecisionKind::BindOldData);
        CHECK(decision(*c.r, kRttiGuardCounted).decision == DecisionKind::BindOldData);
        CHECK(decision(*c.r, kRttiGuardCounted).old_address.rva == 0x3018);
        CHECK(count_code(c.r->warnings, diag::kGuardUnmatched) == 0);
    }
    SUBCASE("patch by clang 18, module by clang 19.1: clang 18's spelling") {
        GuardCase c = plan_guards(kClang19, kClang18, "JPH::GetRTTIOfType", {{"rtti", 0x14}}, {{kRtti, 0x14}, {kRttiGuardNotCounted, 0x18}});
        REQUIRE_MESSAGE(c.r.has_value(), err(c.r));
        CHECK(decision(*c.r, kRttiGuardNotCounted).decision == DecisionKind::BindOldData);
    }
    SUBCASE("clang 19.0.0git is not known: a match in one reading ties nothing, and the bound static is refused") {
        GuardCase c = plan_guards(kClang18, kClang19Dev, "JPH::GetRTTIOfType", {{"rtti", 0x14}}, {{kRtti, 0x14}, {kRttiGuardCounted, 0x18}});
        check_static_refused(c, "rtti", "which reading is right is not known (the object's compiler is 'clang version 19.0.0git");
        CHECK(decision(c.partial, kRttiGuardCounted).decision == DecisionKind::PatchNewData);
    }
    SUBCASE("no S_COMPILE3 in the patch (the module's clang 18 is not used): refused") {
        GuardCase c = plan_guards(kClang18, nullptr, "JPH::GetRTTIOfType", {{"rtti", 0x14}}, {{kRtti, 0x14}, {kRttiGuardNotCounted, 0x18}});
        check_static_refused(c, "rtti", "the object records no compiler");
    }
}

TEST_CASE("R-60 review: where the compiler is not known, a match in one spelling only ties the guard to nothing (CML3211)") {
    // The nested names in a patch whose S_COMPILE3 names no compiler the planner knows: clang 18's reading ties the guard to
    // `gx`, cl's to nothing, and which is right cannot be told. 835ee2c tied it (the one-sided match).
    SynthWorld w = basic_world(false);
    nested_world(w);
    Built b = nested_patch(kNestedGuard, "Test Compiler 19.44");
    GuardCase c;
    c.r = plan_bind(w.input(b), &c.errors, &c.partial);
    check_static_refused(c, "gx", "which reading is right is not known");
    CHECK(decision(c.partial, kNestedGuard).decision == DecisionKind::PatchNewData);
}

TEST_CASE("R-60 review: a cl guard is read in cl's spelling only, never tied to another function's static by clang 18's") {
    // A cl object (cl counts `$TSS0`). `other`'s own static is not in the patch; in clang 18's reading its scope would be
    // JPH::GetRTTIOfType(JPH::SphereShape *), the scope of `rtti`. 835ee2c took that one-sided match: `other` was tied to
    // `rtti` and placed by the layout as if it were rtti's guard (next to rtti's real guard). In cl's `1` is GetRTTIOfType.
    GuardCase c = plan_guards(kCl, kCl, "JPH::GetRTTIOfType", {{"rtti", 0x14}}, {{kRtti, 0x14}, {kRttiGuardCounted, 0x18}, {kRttiGuardNotCounted, 0x1C}});
    REQUIRE_MESSAGE(c.r.has_value(), err(c.r));
    CHECK(decision(*c.r, kRtti).decision == DecisionKind::BindOldData);
    CHECK(decision(*c.r, kRttiGuardCounted).decision == DecisionKind::BindOldData);
    CHECK(decision(*c.r, kRttiGuardCounted).old_address.rva == 0x3018);
    const SymbolDecision g = decision(*c.r, kRttiGuardNotCounted);
    CAPTURE(g.reason);
    CHECK(g.decision == DecisionKind::PatchNewData);
    CHECK(count_code(c.r->warnings, diag::kGuardUnmatched) == 1);  // tied to no static, and rtti has its own bound guard
}

TEST_CASE("R-60: a bound static whose guard cannot be tied is refused (CML3211, an error, naming the static)") {
    // main: the warning CML3211, a fresh guard, and the plan accepted: the bound `gx` would be constructed again. The guard's
    // back-reference is outside the table: its scope cannot be read, so it may be gx's.
    const std::string guard = "?$TSS0@?1???R<lambda_1>@?0???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?7@XZ@4HA";
    SynthWorld w = basic_world(false, kClang18);
    nested_world(w);
    Built b = nested_patch(guard);
    GuardCase c;
    c.r = plan_bind(w.input(b), &c.errors, &c.partial);
    check_static_refused(c, "gx", "cannot be read, so it may be this static's");
    CHECK(decision(c.partial, guard).decision == DecisionKind::PatchNewData);
}

TEST_CASE("R-60: a guard tied to no static whose scope differs from every bound static's is still the warning CML3211") {
    // Both scopes are read and differ, and nothing says `gx` has a guard: a fresh guard runs no bound initialiser again.
    const char* const guards[] = {
        "?$TSS0@?1??other@@YAHXZ@4HA",                                                                    // another function
        "?$TSS0@?2???R<lambda_1>@?0???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?3@XZ@4HA",       // another block
        "?$TSS0@?1???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@4HA",                                    // the outer lambda's
    };
    for (const char* guard : guards) {
        CAPTURE(guard);
        SynthWorld w = basic_world(false, kClang18);
        nested_world(w);
        Built b = nested_patch(guard);
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, kNestedGx).decision == DecisionKind::BindOldData);
        CHECK(decision(*r, guard).decision == DecisionKind::PatchNewData);
        CHECK(count_code(r->warnings, diag::kGuardUnmatched) == 1);
    }
}

TEST_CASE("R-60 review: a bound static whose scope cannot be read and has no bound guard is refused next to an untied guard") {
    // clang 18 names: `pm(void (K::*)())` holds `q`; a member pointer parameter (`P8K@@...`) is a type local_scope_key does
    // not read. With its own guard both are tied as written and the guard is placed. With only other()'s untied guard, q's
    // scope cannot be compared with it, so q is refused (convergence ruling: no function is matched).
    const std::string q = "?q@?1??pm@@YAHP8K@@EAAXXZ@Z@4URegistry@@A";
    const std::string own = "?$TSS0@?1??pm@@YAHP8K@@EAAXXZ@Z@4HA";
    GuardCase d = plan_guards(kClang18, kClang18, "pm", {{"q", 0x14}}, {{q, 0x14}, {own, 0x18}});
    REQUIRE_MESSAGE(d.r.has_value(), err(d.r));
    CHECK(decision(*d.r, own).decision == DecisionKind::BindOldData);
    CHECK(count_code(d.r->warnings, diag::kGuardUnmatched) == 0);
    GuardCase c = plan_guards(kClang18, kClang18, "pm", {{"q", 0x14}}, {{q, 0x14}, {"?$TSS0@?1??other@@YAHXZ@4HA", 0x18}});
    check_static_refused(c, "pm::q", "its own scope cannot be read");
}

TEST_CASE("R-60 review: a constant-initialised static needs no guard and does not make an untied guard an error") {
    // clang 18: `int cinit() { static int n = 0; ... }`, `n` has no guard. An untied guard of another block of cinit (its
    // static not in the patch): both scopes are read and differ, nothing says n has a guard: the warning. 835ee2c: CML3211.
    const std::string n = "?n@?1??cinit@@YAHXZ@4HA";
    GuardCase c = plan_guards(kClang18, kClang18, "cinit", {{"n", 0x14}}, {{n, 0x14}, {"?$TSS0@?2??cinit@@YAHXZ@4HA", 0x18}});
    REQUIRE_MESSAGE(c.r.has_value(), err(c.r));
    CHECK(decision(*c.r, n).decision == DecisionKind::BindOldData);
    CHECK(count_code(c.r->warnings, diag::kGuardUnmatched) == 1);
    // A guard whose scope cannot be read may be n's, whatever its block: refused (the safe side).
    GuardCase d = plan_guards(kClang18, kClang18, "cinit", {{"n", 0x14}}, {{n, 0x14}, {"?$TSS0@?2??cinit@@YAHU7@@Z@4HA", 0x18}});
    check_static_refused(d, "cinit::n", "cannot be read, so it may be this static's");
}

TEST_CASE("R-54 review: the R-54 edit one level down (the inner lambda deleted, its static's name declared in the outer lambda) is refused") {
    // main: the outer lambda's new static paired with the inner lambda's record (pass A).
    SynthWorld w = basic_world(false);
    nested_world(w);
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("?s_count@?A0x1@@3HA", 0x10, bss, false, false);
    const std::string own = "?gx@?1???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@4UObj@@A";
    o.symbol(own, 0x20, bss, false, false);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(count_code(errors, diag::kAmbiguousStatic) == 1);
    CAPTURE(errors[0].message);
    CHECK(contains(errors[0].message, "inlined 'g::<lambda_0>::operator()::<lambda_1>::operator()', a lambda's code"));
    CHECK(decision(partial, own).decision != DecisionKind::BindOldData);
}

TEST_CASE("R-54 review: a call operator whose closure cannot be read counts as the static's own and is checked") {
    // The outer call operator's type is not in the TPI: which closure it belongs to cannot be told, so the record is not
    // proven to be another lambda's, and the inner closure's inline site refuses the static.
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "`anonymous namespace'::s_count", 0x10);
    OldTypes t;
    const std::uint32_t inner = t.member(w.spec.ipi, "g::<lambda_0>::operator()::<lambda_1>", kInnerClosure, "operator()");
    proc_inlining(w, "g::<lambda_0>::operator()", 0x60, 0x7FFF, {{"gx", 0x20}}, {{inner}});
    open_with(w, t);
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("?s_count@?A0x1@@3HA", 0x10, bss, false, false);
    const std::string gx = "?gx@?1???R<lambda_1>@?0???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?4@XZ@4UObj@@A";
    o.symbol(gx, 0x20, bss, false, false);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE_FALSE(r.has_value());
    CHECK(decision(partial, gx).decision != DecisionKind::BindOldData);
}

// ---- second review (3b2fef3): another closure's call operator is skipped only when it inlined the static's own closure ------

namespace {

// A call operator procedure `name` of closure (`cls`, `unique`) with the records `statics`, which inlined the call operators of
// the closures `inlined` (name, unique name).
void call_operator(SynthWorld& w, OldTypes& t, const std::string& name, const std::string& cls, const std::string& unique,
                   const std::vector<std::pair<std::string, std::uint32_t>>& statics, const std::vector<std::pair<std::string, std::string>>& inlined) {
    const std::uint32_t c = t.tpi.add(tp::kLfClass, tp::lf_udt(cls, unique, true));
    const std::uint32_t this_ptr = t.tpi.add(tp::kLfPointer, tp::lf_pointer(c, tp::kPtr64 | 0x400));
    const std::uint32_t fn = t.tpi.add(tp::kLfMFunction, tp::lf_mfunction(tp::kTInt, c, this_ptr, t.args, 0));
    std::vector<std::vector<std::uint32_t>> chains;
    for (const auto& [inner_cls, inner_unique] : inlined) {
        chains.push_back({t.member(w.spec.ipi, inner_cls, inner_unique, "operator()")});
    }
    proc_inlining(w, name, 0x60, fn, statics, chains);
}

}  // namespace

TEST_CASE("R-54 second review: a lambda renumbered without rule (d) is checked against the call operator its record is in") {
    // The static is `f::<lambda_1>`'s; its old records are in `f::<lambda_0>::operator()`, which inlined `f::<lambda_2>` (not
    // `<lambda_1>`): nothing proves the records are another lambda's, so they count as the static's own, and the inlined
    // closure refuses it. 3b2fef3: skipped as "another closure's call operator" and paired (the one record of the name), no
    // CML3207.
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "`anonymous namespace'::s_count", 0x10);
    OldTypes t;
    call_operator(w, t, "f::<lambda_0>::operator()", "f::<lambda_0>", ".?AV<lambda_0>@?0??f@@YAHXZ@", {{"x", 0x20}},
                  {{"f::<lambda_2>", ".?AV<lambda_2>@?0??f@@YAHXZ@"}});
    open_with(w, t);
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("?s_count@?A0x1@@3HA", 0x10, bss, false, false);
    const std::string x = "?x@?1???R<lambda_1>@?0??f@@YAHXZ@QEBA@XZ@4HA";
    o.symbol(x, 0x20, bss, false, false);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(count_code(errors, diag::kAmbiguousStatic) == 1);
    CAPTURE(errors[0].message);
    CHECK(contains(errors[0].message, "inlined 'f::<lambda_2>::operator()', a lambda's code"));
    CHECK(decision(partial, x).decision != DecisionKind::BindOldData);
}

TEST_CASE("R-54 second review: a call operator whose closure name is spelled with other back-references is checked") {
    // clang spells the inner closure's unique name with a back-reference (`???R0?0??g@@...`, docs/wp/WP-51.md, g_caller) and
    // the static's decorated scope without (`???R<lambda_0>@?0??g@@...`): the record in the inner lambda's own call operator
    // looks like another closure's. That call operator inlined an innermost lambda, not the static's closure, so the record is
    // checked and refused. 3b2fef3: skipped, paired by name, no CML3207.
    SynthWorld w = basic_world(false);
    OldTypes t;
    call_operator(w, t, "g::<lambda_0>::operator()::<lambda_1>::operator()", "g::<lambda_0>::operator()::<lambda_1>",
                  ".?AV<lambda_1>@?0???R0?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@", {{"gx", 0x20}},
                  {{"g::<lambda_0>::operator()::<lambda_1>::operator()::<lambda_2>", ".?AV<lambda_2>@?0???R1?0???R0?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?<auto>@@XZ@"}});
    open_with(w, t);
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    const std::string gx = "?gx@?1???R<lambda_1>@?0???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?4@XZ@4UObj@@A";
    o.symbol(gx, 0x20, bss, false, false);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(count_code(errors, diag::kAmbiguousStatic) == 1);
    CAPTURE(errors[0].message);
    CHECK(contains(errors[0].message, "a lambda's code"));
    CHECK(decision(partial, gx).decision != DecisionKind::BindOldData);
}
