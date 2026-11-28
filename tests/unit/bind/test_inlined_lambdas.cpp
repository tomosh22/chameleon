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
#include "cml/core/hook_narrowing.h"
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
    Built object;  // WP-56: the patch, for the pipeline's proof (prove_inlined_lambda_statics)
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
    c.object = std::move(b);
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

namespace {
// The index of the procedure `name` in the TU's symbols (what InlineVerdictCache is keyed by), and its RVA.
std::pair<std::uint32_t, std::uint32_t> proc_of(const SynthWorld& w, const std::string& name) {
    const auto syms = w.index->compiland_symbols(0);
    REQUIRE(syms.has_value());
    for (const cml::pdb::SymbolView& v : *syms) {
        if (cml::pdb::is_proc_kind(v.kind) && v.name == name) {
            return {v.index, v.rva};
        }
    }
    FAIL("no procedure " << name);
    return {0, 0};
}
}  // namespace

TEST_CASE("WP-55 (R-62): the verdicts on inline sites are kept across the plans of one image and decide as a fresh read does") {
    SUBCASE("a refusal: the second plan takes the verdict from the cache and refuses with the same message") {
        SynthWorld w = basic_world(false);
        OldTypes t;
        const std::uint32_t call = t.member(w.spec.ipi, "o2_step::<lambda_0>", kClosure, "operator()");
        const std::uint32_t make = t.function(w.spec.ipi, "o2_make");
        proc_inlining(w, "o2_step", 0x60, t.fn_void, {{"sname", 0x20}}, {{call, make}});
        open_with(w, t);
        Built b = own_static_patch();
        std::vector<cml::Diag> fresh_errors;
        REQUIRE_FALSE(plan_bind(w.input(b), &fresh_errors).has_value());  // without a cache
        REQUIRE_FALSE(fresh_errors.empty());

        InlineVerdictCache cache;
        for (int round = 0; round < 3; ++round) {
            CAPTURE(round);
            PlanInput in = w.input(b);
            in.inline_verdicts = &cache;
            std::vector<cml::Diag> errors;
            BindPlan partial;
            REQUIRE_FALSE(plan_bind(in, &errors, &partial).has_value());
            REQUIRE(count_code(errors, diag::kAmbiguousStatic) == 1);
            CHECK(errors[0].message == fresh_errors[0].message);
            CHECK(decision(partial, kOwn).decision != DecisionKind::BindOldData);
            CHECK(cache.stores() == 1);  // read once, by the first plan
            CHECK(cache.hits() == static_cast<std::uint64_t>(round));
            CHECK(cache.kept() == static_cast<std::uint64_t>(round));  // WP55-INLINE-CACHE kept=
            CHECK(cache.reads() == 1);                                 // ... read=
        }
        CHECK(cache.size() == 1);
    }
    SUBCASE("a pairing: the second plan pairs the same way from the cache") {
        SynthWorld w = basic_world(false);
        OldTypes t;
        const std::uint32_t ctor = t.member(w.spec.ipi, "JPH::RTTI", ".?AVRTTI@JPH@@", "RTTI");
        proc_inlining(w, "JPH::GetRTTIOfType", 0x60, t.fn_void, {{"rtti", 0x20}}, {{ctor}});
        open_with(w, t);
        SynthObj o;
        const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
        const std::string rtti = "?rtti@?1??GetRTTIOfType@JPH@@YAPEAVRTTI@2@PEAVSphereShape@2@@Z@4V32@A";
        o.symbol(rtti, 0x20, bss, false, false);
        Built b = o.build();
        InlineVerdictCache cache;
        for (int round = 0; round < 2; ++round) {
            PlanInput in = w.input(b);
            in.inline_verdicts = &cache;
            auto r = plan_bind(in);
            REQUIRE_MESSAGE(r.has_value(), err(r));
            CHECK(decision(*r, rtti).decision == DecisionKind::BindOldData);
            CHECK(decision(*r, rtti).source == BindSource::CompilandStatic);
            CHECK(decision(*r, rtti).old_address.rva == 0x3020);
        }
        CHECK(cache.stores() == 1);
        CHECK(cache.hits() == 1);
        CHECK(cache.kept() == 1);
        CHECK(cache.reads() == 1);
    }
    SUBCASE("a changed input: a verdict kept for another image, or for another procedure at that place, is not used") {
        // A function that inlined nothing, and a poisoned cache that says it inlined a lambda: used, it would refuse.
        SynthWorld w = basic_world(false);
        OldTypes t;
        proc_inlining(w, "o2_step", 0x60, t.fn_void, {{"sname", 0x20}}, {});
        open_with(w, t);
        const auto [proc, rva] = proc_of(w, "o2_step");
        CachedInlineVerdict poison;
        poison.proc_rva = rva;
        poison.proc_name = "o2_step";
        poison.risk = "POISON inlined a lambda's code";
        poison.closures = 1;
        Built b = own_static_patch();
        const auto plan_with = [&](InlineVerdictCache& cache) {
            PlanInput in = w.input(b);
            in.inline_verdicts = &cache;
            std::vector<cml::Diag> errors;
            auto r = plan_bind(in, &errors);
            return r.has_value() ? std::string() : (errors.empty() ? r.error().message : errors[0].message);
        };
        // The same identity, compiland, procedure, RVA and name: taken (this shows the planner reads the cache).
        {
            InlineVerdictCache cache;
            cache.store(w.index->id(), 0, proc, poison);
            CHECK(contains(plan_with(cache), "POISON"));
            CHECK(cache.hits() == 1);
        }
        // Another PDB identity (the module was relinked and its PDB mapped again): not used, read afresh and kept for this one.
        {
            InlineVerdictCache cache;
            cml::pdb::PdbId other = w.index->id();
            other.age += 1;
            cache.store(other, 0, proc, poison);
            CHECK(plan_with(cache).empty());  // paired, as without a cache
            CHECK(cache.hits() == 0);
            CHECK(cache.kept() == 0);
            CHECK(cache.reads() == 1);  // read afresh
            CHECK(cache.size() == 1);  // the stale entry went with its image
            CHECK_FALSE(cache.find(other, 0, proc, rva, "o2_step").has_value());
            CHECK(cache.find(w.index->id(), 0, proc, rva, "o2_step").has_value());
        }
        // Another procedure at that index (another RVA or name): not used.
        {
            InlineVerdictCache cache;
            CachedInlineVerdict moved = poison;
            moved.proc_rva = rva + 0x10;
            cache.store(w.index->id(), 0, proc, moved);
            CHECK(plan_with(cache).empty());
            CachedInlineVerdict renamed = poison;
            renamed.proc_name = "o2_other";
            InlineVerdictCache by_name;
            by_name.store(w.index->id(), 0, proc, renamed);
            CHECK(plan_with(by_name).empty());
        }
        // An image of unknown identity is never kept.
        {
            InlineVerdictCache cache;
            cache.store(cml::pdb::PdbId{}, 0, proc, poison);
            CHECK(cache.size() == 0);
            CHECK_FALSE(cache.find(cml::pdb::PdbId{}, 0, proc, rva, "o2_step").has_value());
        }
    }
    SUBCASE("kept= counts the verdicts asked for, not the batch's probes; read= every verdict judged from a read") {
        // Two procedures hold a record named like the patch's static; only o2_step's verdict is asked for. o2_other's verdict is
        // in the cache already: the batch probes it (a hit) but nobody asked for it, so it is not `kept`.
        SynthWorld w = basic_world(false);
        OldTypes t;
        proc_inlining(w, "o2_step", 0x60, t.fn_void, {{"sname", 0x20}}, {});
        proc_inlining(w, "o2_other", 0x90, t.fn_void, {{"sname", 0x30}}, {});
        open_with(w, t);
        const auto [other, other_rva] = proc_of(w, "o2_other");
        CachedInlineVerdict clean;
        clean.proc_rva = other_rva;
        clean.proc_name = "o2_other";
        InlineVerdictCache cache;
        cache.store(w.index->id(), 0, other, clean);
        Built b = own_static_patch();
        PlanInput in = w.input(b);
        in.inline_verdicts = &cache;
        auto r = plan_bind(in);
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, kOwn).old_address.rva == 0x3020);
        CHECK(cache.hits() == 1);   // the probe of o2_other
        CHECK(cache.kept() == 0);   // ... not asked for
        CHECK(cache.reads() == 1);  // o2_step, read
        // The next plan asks for o2_step again: from the cache, and counted.
        auto again = plan_bind(in);
        REQUIRE_MESSAGE(again.has_value(), err(again));
        CHECK(cache.kept() == 1);
        CHECK(cache.reads() == 1);
    }
    SUBCASE("an inlinee that cannot be read is part of the image: the verdict is kept and refuses again from the cache") {
        SynthWorld w = basic_world(false);
        OldTypes t;
        proc_inlining(w, "o2_step", 0x60, t.fn_void, {{"sname", 0x20}}, {{0x1777}});  // an inlinee not in the IPI
        open_with(w, t);
        Built b = own_static_patch();
        InlineVerdictCache cache;
        for (int round = 0; round < 2; ++round) {
            PlanInput in = w.input(b);
            in.inline_verdicts = &cache;
            std::vector<cml::Diag> errors;
            REQUIRE_FALSE(plan_bind(in, &errors).has_value());
            REQUIRE(count_code(errors, diag::kAmbiguousStatic) == 1);
        }
        CHECK(cache.stores() == 1);
        CHECK(cache.hits() == 1);
        CHECK(cache.kept() == 1);
        CHECK(cache.reads() == 1);
    }
}

// ---- WP-52 (ADR-0045 ruling): a new static never silently replaces a live one of its function (cl nested, R-66) --------

namespace {

// cl's names for the fixture's nested lambda (planner_pairing_o2/nested.cpp, CI run 46093165335): cl counts `$TSS0`, and the
// outer closure is written as a back-reference (`??R1?1??`), which the closure comparisons of WP-37/40 do not resolve (R-68).
const char* const kClNested = "?nested@?1???R<lambda_1>@?1???R1?1??o2_nest_step@@YAXXZ@QEBA@XZ@QEBA@XZ@4UO2Name@@A";
const char* const kClNestedGuard = "?$TSS0@?1???R<lambda_1>@?1???R1?1??o2_nest_step@@YAXXZ@QEBA@XZ@QEBA@XZ@4HA";
// clang 18's names for the same fixture, both lambdas inlined into o2_nest_step (R-66: the record two levels up).
const char* const kClangNested = "?nested@?1???R<lambda_1>@?0???R<lambda_0>@?0??o2_nest_step@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?4@XZ@4UO2Name@@A";
const char* const kClangNestedGuard = "?$TSS0@?1???R<lambda_1>@?0???R<lambda_0>@?0??o2_nest_step@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?3@XZ@4HA";

// WP-56: paired with the record of o2_nest_step at 0x3020 (by the layout), its guard placed with it, and listed for the
// pipeline's proof by the code of o2_nest_step; without that code (the synthetic world has none: o2_nest_step is not a
// function of the patch) the proof refuses it, CML3207.
void check_bound_pending_proof(const GuardCase& c, const std::string& name, const std::string& display) {
    REQUIRE_MESSAGE(c.r.has_value(), err(c.r));
    CHECK(c.errors.empty());
    const SymbolDecision d = decision(*c.r, name);
    CAPTURE(d.reason);
    CHECK(d.decision == DecisionKind::BindOldData);
    CHECK(d.old_address.rva == 0x3020);
    CHECK(d.source == BindSource::CompilandRva);
    const SymbolDecision g = decision(*c.r, name == kClNested ? kClNestedGuard : kClangNestedGuard);
    CHECK(g.decision == DecisionKind::BindOldData);
    CHECK(g.old_address.rva == 0x3028);
    REQUIRE(c.r->inlined_lambda_statics.size() == 1);
    CHECK(c.r->inlined_lambda_statics[0].symbol == name);
    CHECK(c.r->inlined_lambda_statics[0].display == display);
    CHECK(c.r->inlined_lambda_statics[0].proc == "o2_nest_step");
    CHECK(c.r->inlined_lambda_statics[0].record_rva == 0x3020);
    NarrowInput ni;
    ni.object = &c.object.obj();
    ni.plan = &*c.r;
    ni.old_image = nullptr;
    ni.is_hooked = [](std::uint64_t) { return false; };
    const std::vector<cml::Diag> proof = prove_inlined_lambda_statics(ni);
    REQUIRE(proof.size() == 1);
    CAPTURE(proof[0].message);
    CHECK(proof[0].id() == "CML3207");
    CHECK(contains(proof[0].message, "only an unchanged 'o2_nest_step' proves the record is this lambda's"));
}

void check_reset_refused(const GuardCase& c, const std::string& name) {
    REQUIRE_FALSE(c.r.has_value());
    REQUIRE(count_code(c.errors, diag::kAmbiguousStatic) == 1);
    const cml::Diag* e = nullptr;
    for (const cml::Diag& d : c.errors) {
        e = d.code == diag::kAmbiguousStatic ? &d : e;
    }
    CAPTURE(e->message);
    CHECK(e->id() == "CML3207");
    CHECK(contains(e->message, "would be new in the patch"));
    CHECK(contains(e->message, "records 'nested' at rva 0x3020"));
    CHECK(contains(e->message, "a procedure of its function 'o2_nest_step'"));
    CHECK(contains(e->message, "a new static would reset it"));
    CHECK(decision(c.partial, name).decision == DecisionKind::PatchNewData);
    CHECK(count_code(c.errors, diag::kGuardUnmatched) == 0);
}

}  // namespace

TEST_CASE("WP-52 ruling: cl's nested lambda static, decided new, is refused while its function records a static of its name") {
    // CI (857fa31, cl /O2): the static and its guard were decided new (new_globals=2) and the reload reset `nest` to 700001.
    SUBCASE("the record in the function's own procedure (both lambdas inlined): WP-56, paired and proven by code, or refused") {
        // WP-52: refused (the planner looked one level up only). WP-56 (R-66): the chain of enclosing procedures is searched,
        // the record of o2_nest_step pairs by the layout, and the pairing waits for the code of o2_nest_step (WP-53).
        GuardCase c = plan_guards(kCl, kCl, "o2_nest_step", {{"nested", 0x20}}, {{kClNested, 0x20}, {kClNestedGuard, 0x28}});
        check_bound_pending_proof(c, kClNested, "o2_nest_step::<lambda_1>::operator()::<lambda_1>::operator()::nested");
    }
    SUBCASE("the record in the outer lambda's call operator, the only one of its name and no types to check: paired, not new") {
        // (The rule applies only to a static decided new; this one is found, as before. In CI the closure types of the old
        // image are read and do not match the back-referenced scope, R-68, and the static was new.)
        GuardCase c = plan_guards(kCl, kCl, "`o2_nest_step'::`2'::<lambda_1>::operator()", {{"nested", 0x20}}, {{kClNested, 0x20}, {kClNestedGuard, 0x28}});
        REQUIRE_MESSAGE(c.r.has_value(), err(c.r));
        CHECK(decision(*c.r, kClNested).decision == DecisionKind::BindOldData);
        CHECK(decision(*c.r, kClNestedGuard).decision == DecisionKind::BindOldData);
    }
}

TEST_CASE("WP-56 (R-66): clang 18's fully inlined nested lambda, its static recorded two levels up, pairs and waits for the proof") {
    // clang-cl 18 /O2 without the fixture's noinline: `nested` is a record of o2_nest_step itself. WP-52: the planner looked
    // one level up (the outer call operator, which has no procedure), decided the static new, and its ruling refused that
    // (CML3207). WP-56: every procedure of the chain is searched; the record pairs by the layout and is listed for the code
    // proof of o2_nest_step, which refuses it where the code cannot be judged (real images: test_nested_inlined.cpp).
    GuardCase c = plan_guards(kClang18, kClang18, "o2_nest_step", {{"nested", 0x20}}, {{kClangNested, 0x20}, {kClangNestedGuard, 0x28}});
    check_bound_pending_proof(c, kClangNested, "o2_nest_step::<lambda_0>::operator()::<lambda_1>::operator()::nested");
}


TEST_CASE("WP-52 ruling: a static whose name no procedure of its function records is new, as before") {
    const std::string fresh = "?fresh_one@?1???R<lambda_1>@?0???R<lambda_0>@?0??o2_nest_step@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?4@XZ@4UO2Name@@A";
    GuardCase c = plan_guards(kClang18, kClang18, "o2_nest_step", {{"nested", 0x20}}, {{fresh, 0x30}});
    REQUIRE_MESSAGE(c.r.has_value(), err(c.r));
    CHECK(decision(*c.r, fresh).decision == DecisionKind::PatchNewData);
    // Nor a static of that name in another function's procedures.
    GuardCase d = plan_guards(kClang18, kClang18, "other_function", {{"nested", 0x20}}, {{kClangNested, 0x30}});
    REQUIRE_MESSAGE(d.r.has_value(), err(d.r));
    CHECK(decision(*d.r, kClangNested).decision == DecisionKind::PatchNewData);
}

// ---- WP-56 (R-68, ADR-0046): cl's nested lambda, its closure compared with the back-references resolved ------------------
//
// cl /O2 for planner_pairing_o2/nested.cpp (link without /OPT:REF, as the fixtures; the module PDB of CI run 46115540265,
// windows-2025 msvc RelWithDebInfo, llvm-pdbutil): both call operators keep their procedures, and the inner lambda's
// `nested` and its `$TSS0` are records of the inner call operator, and again (inside the inline sites, at the same addresses)
// of the outer one and of o2_nest_step. The names, the procedures' display names and the unique names below are that PDB's. The static's decorated name spells the outer closure as a
// back-reference to its table, whose slot 0 is the static's own name (`??R1?1??`: `<lambda_1>`), the inner closure's unique
// name to its own table (`??R0?1??`). WP-37/40 compared the two as written: no closure matched, `nested` and its guard were
// new (CI 857fa31: the reload reset `nest`), then WP-52 refused that (CML3207).

namespace {

const char* const kClOuterUnique = ".?AV<lambda_1>@?1??o2_nest_step@@YAXXZ@";
const char* const kClInnerUnique = ".?AV<lambda_1>@?1???R0?1??o2_nest_step@@YAXXZ@QEBA@XZ@";
const char* const kClOuterProc = "`o2_nest_step'::`2'::<lambda_1>::operator()";
const char* const kClInnerProc = "``o2_nest_step'::`2'::<lambda_1>::operator()'::`2'::<lambda_1>::operator()";

// A call operator procedure `name` at `text_offset` of the closure (`cls`, `unique`) with the static records `statics`.
void typed_operator(SynthWorld& w, OldTypes& t, const std::string& name, std::uint32_t text_offset, const std::string& cls, const std::string& unique,
                    const std::vector<std::pair<std::string, std::uint32_t>>& statics) {
    const std::uint32_t c = t.tpi.add(tp::kLfClass, tp::lf_udt(cls, unique, true));
    const std::uint32_t this_ptr = t.tpi.add(tp::kLfPointer, tp::lf_pointer(c, tp::kPtr64 | 0x400));
    const std::uint32_t fn = t.tpi.add(tp::kLfMFunction, tp::lf_mfunction(tp::kTInt, c, this_ptr, t.args, 0));
    proc_inlining(w, name, text_offset, fn, statics, {});
}

struct ClNested {
    std::string inner_unique = kClInnerUnique;
    bool outer_copy = true;   // the outer call operator records the inlined static too
    bool caller_copy = true;  // and o2_nest_step
};

// The old image of the cl build, and the patch (nest_4: another function of nested.cpp edited) with `symbols`.
GuardCase plan_cl_nested(const ClNested& shape, const std::vector<std::pair<std::string, std::uint32_t>>& symbols) {
    SynthWorld w = basic_world(false, kCl);
    w.data(w.tu(), "`anonymous namespace'::s_a", 0x10);
    OldTypes t;
    const std::vector<std::pair<std::string, std::uint32_t>> records = {{"nested", 0x20}, {"$TSS0", 0x28}};
    typed_operator(w, t, kClInnerProc, 0x60, "``o2_nest_step'::`2'::<lambda_1>::operator()'::`2'::<lambda_1>", shape.inner_unique, records);
    typed_operator(w, t, kClOuterProc, 0x90, "`o2_nest_step'::`2'::<lambda_1>", kClOuterUnique, shape.outer_copy ? records : std::vector<std::pair<std::string, std::uint32_t>>{});
    if (shape.caller_copy) {
        proc_inlining(w, "o2_nest_step", 0xC0, t.fn_void, records, {});
    }
    open_with(w, t);
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("?s_a@?A0x1@@3HA", 0x10, bss, false, false);
    for (const auto& [name, offset] : symbols) {
        o.symbol(name, offset, bss, false, false);
    }
    o.compile3(kCl);
    Built b = o.build();
    GuardCase c;
    c.r = plan_bind(w.input(b), &c.errors, &c.partial);
    c.object = std::move(b);
    return c;
}

}  // namespace

TEST_CASE("WP-56 (R-68): cl's nested lambda static pairs with its own call operator's record, its guard with it") {
    // main (910f203): no closure matched the back-referenced scope, so `nested` was new and the WP-52 rule refused it (CML3207,
    // "would be new in the patch"); before WP-52 the reload reset it.
    SUBCASE("records in both call operators and in o2_nest_step (cl without /OPT:REF)") {
        GuardCase c = plan_cl_nested({}, {{kClNested, 0x20}, {kClNestedGuard, 0x28}});
        REQUIRE_MESSAGE(c.r.has_value(), err(c.r));
        const SymbolDecision d = decision(*c.r, kClNested);
        CAPTURE(d.reason);
        CHECK(d.decision == DecisionKind::BindOldData);
        CHECK(d.source == BindSource::CompilandClosure);
        CHECK(d.old_address.rva == 0x3020);
        const SymbolDecision g = decision(*c.r, kClNestedGuard);
        CAPTURE(g.reason);
        CHECK(g.decision == DecisionKind::BindOldData);
        CHECK(g.source == BindSource::CompilandClosure);
        CHECK(g.old_address.rva == 0x3028);
        // The record of its own closure's call operator identifies it: nothing waits for a proof.
        CHECK(c.r->inlined_lambda_statics.empty());
        CHECK(c.errors.empty());
        CHECK(count_code(c.r->warnings, diag::kGuardUnmatched) == 0);
    }
    SUBCASE("only the inner call operator records it") {
        ClNested shape;
        shape.outer_copy = false;
        shape.caller_copy = false;
        GuardCase c = plan_cl_nested(shape, {{kClNested, 0x20}, {kClNestedGuard, 0x28}});
        REQUIRE_MESSAGE(c.r.has_value(), err(c.r));
        CHECK(decision(*c.r, kClNested).decision == DecisionKind::BindOldData);
        CHECK(decision(*c.r, kClNested).old_address.rva == 0x3020);
        CHECK(decision(*c.r, kClNestedGuard).old_address.rva == 0x3028);
    }
}

TEST_CASE("WP-56 (R-68): a closure whose resolved name is another lambda's does not take the static") {
    // The inner call operator's closure resolves to `<lambda_2>` of a `<lambda_2>` (its `0` is its own first name), the static's
    // to `<lambda_1>` of `<lambda_1>`: the record is not its closure's, and the static is refused (CML3207), never paired.
    ClNested shape;
    shape.inner_unique = ".?AV<lambda_2>@?1???R0?1??o2_nest_step@@YAXXZ@QEBA@XZ@";
    // (`0` there is `<lambda_2>`, so the outer closure reads `<lambda_2>` too: another function altogether.)
    GuardCase c = plan_cl_nested(shape, {{kClNested, 0x20}, {kClNestedGuard, 0x28}});
    REQUIRE_FALSE(c.r.has_value());
    CHECK(decision(c.partial, kClNested).decision != DecisionKind::BindOldData);
    CHECK(decision(c.partial, kClNestedGuard).decision != DecisionKind::BindOldData);
    REQUIRE(count_code(c.errors, diag::kAmbiguousStatic) == 2);
    CHECK(contains(c.errors[0].message + c.errors[1].message, kClNested));
}

TEST_CASE("WP-56 (R-68): two lambdas of one function tied only by the resolved names are not paired by number") {
    // A second inner lambda `<lambda_2>` with a static `nested` of its own: their numbers would be confirmed by the code of their
    // call operators (rule (d)), which finds closures as written; the resolved pairing is not made (refused).
    SynthWorld w = basic_world(false, kCl);
    w.data(w.tu(), "`anonymous namespace'::s_a", 0x10);
    OldTypes t;
    typed_operator(w, t, kClInnerProc, 0x60, "``o2_nest_step'::`2'::<lambda_1>::operator()'::`2'::<lambda_1>", kClInnerUnique, {{"nested", 0x20}});
    typed_operator(w, t, "``o2_nest_step'::`2'::<lambda_1>::operator()'::`2'::<lambda_2>::operator()", 0x90,
                   "``o2_nest_step'::`2'::<lambda_1>::operator()'::`2'::<lambda_2>", ".?AV<lambda_2>@?1???R<lambda_1>@?1??o2_nest_step@@YAXXZ@QEBA@XZ@", {{"nested", 0x30}});
    open_with(w, t);
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("?s_a@?A0x1@@3HA", 0x10, bss, false, false);
    // (The outer closure `<lambda_1>` is not in this name's table, slot 1 is `<lambda_2>`: cl writes it out.)
    const std::string second = "?nested@?1???R<lambda_2>@?1???R<lambda_1>@?1??o2_nest_step@@YAXXZ@QEBA@XZ@QEBA@XZ@4UO2Name@@A";
    o.symbol(kClNested, 0x20, bss, false, false);
    o.symbol(second, 0x30, bss, false, false);
    o.compile3(kCl);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE_FALSE(r.has_value());
    CHECK(decision(partial, kClNested).source != BindSource::CompilandClosure);
    CHECK(decision(partial, second).source != BindSource::CompilandClosure);
    bool named = false;
    for (const cml::Diag& d : errors) {
        named = named || contains(d.message, "cannot be confirmed by the code of their call operators");
    }
    CHECK(named);
}

TEST_CASE("WP-52 ruling, kept by WP-56: cl's nested static recorded only in o2_nest_step, without a layout, is still refused") {
    // cl puts every function-local static in a COMDAT section of its own, so no section layout places it, and a record in the
    // procedure that inlined the lambdas does not say whose it is (WP-40, WP-53 (1b) deferred). The chain search finds the
    // record; nothing pairs it, and the static is refused.
    SynthWorld w = basic_world(false, kCl);
    w.data(w.tu(), "`anonymous namespace'::s_a", 0x10);
    OldTypes t;
    proc_inlining(w, "o2_nest_step", 0x60, t.fn_void, {{"nested", 0x20}}, {});
    open_with(w, t);
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("?s_a@?A0x1@@3HA", 0x10, bss, false, false);
    const std::int32_t own = static_cast<std::int32_t>(o.section(".bss", kData | coff::scn::LnkComdat, 0x10, coff::ComdatSelection::Any));
    o.symbol(kClNested, 0, own, false, false);
    o.compile3(kCl);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(count_code(errors, diag::kAmbiguousStatic) == 1);
    CAPTURE(errors[0].message);
    CHECK(contains(errors[0].message, kClNested));
    CHECK(contains(errors[0].message, "does not say which lambda each belongs to"));
    CHECK(decision(partial, kClNested).decision != DecisionKind::BindOldData);
}

TEST_CASE("WP-52 ruling, kept by WP-56: a record of the static's name in its function outside the chain still refuses the reset") {
    // A member of a class local to the outer lambda records `nested`: not a procedure the chain search takes (not a call
    // operator, not an enclosing function), so the static is new, and the WP-52 rule refuses it.
    GuardCase c = plan_guards(kClang18, kClang18, "o2_nest_step::<lambda_0>::operator()::Local::run", {{"nested", 0x20}},
                              {{kClangNested, 0x20}, {kClangNestedGuard, 0x28}});
    check_reset_refused(c, kClangNested);
}
