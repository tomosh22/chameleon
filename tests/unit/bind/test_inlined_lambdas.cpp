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
void nested_world(SynthWorld& w) {
    w.data(w.tu(), "`anonymous namespace'::s_count", 0x10);
    OldTypes t;
    const std::uint32_t outer_cls = t.tpi.add(tp::kLfClass, tp::lf_udt("g::<lambda_0>", kOuterClosure, true));
    const std::uint32_t this_ptr = t.tpi.add(tp::kLfPointer, tp::lf_pointer(outer_cls, tp::kPtr64 | 0x400));
    const std::uint32_t outer_fn = t.tpi.add(tp::kLfMFunction, tp::lf_mfunction(tp::kTInt, outer_cls, this_ptr, t.args, 0));
    const std::uint32_t inner = t.member(w.spec.ipi, "g::<lambda_0>::operator()::<lambda_1>", kInnerClosure, "operator()");
    proc_inlining(w, "g::<lambda_0>::operator()", 0x60, outer_fn, {{"gx", 0x20}}, {{inner}});
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
}

// ---- WP-52 (R-60, ADR-0045): the nested lambda's guard is tied to its bound static -------------------------------------

namespace {

// clang 18's names for the nested case (x86_64-pc-windows-msvc -O2, docs/wp/WP-52.md): the guard does not count its own
// name in the back-reference table, the static counts `gx`, so the same scope reads `?A?3@` in one and `?A?4@` in the other.
const char* const kNestedGx = "?gx@?1???R<lambda_1>@?0???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?4@XZ@4UObj@@A";
const char* const kNestedGuard = "?$TSS0@?1???R<lambda_1>@?0???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?3@XZ@4HA";

// The patch of nested_world: s_count, the inner lambda's `gx` (0x20) and the guard `guard` (0x28).
Built nested_patch(const std::string& guard) {
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("?s_count@?A0x1@@3HA", 0x10, bss, false, false);
    o.symbol(kNestedGx, 0x20, bss, false, false);
    o.symbol(guard, 0x28, bss, false, false);
    return o.build();
}

}  // namespace

TEST_CASE("R-60: the guard of a nested lambda's static is tied to the static and placed with it (clang 18 names)") {
    // main (d110b5d): the guard was tied to no static (its scope is spelled with other back-references), so it got a fresh
    // guard with the warning CML3211 while `gx` stayed bound: the patch ran gx's initialiser again over the live object.
    SynthWorld w = basic_world(false);
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

TEST_CASE("R-60: a guard that cannot be tied next to a bound static without a guard is refused (CML3211, an error)") {
    // main: the warning CML3211, a fresh guard, and the plan accepted: the bound `gx` would be constructed again.
    struct Case {
        const char* what;
        std::string guard;
    };
    const Case cases[] = {
        {"a back-reference outside the table (the scope cannot be read)",
         "?$TSS0@?1???R<lambda_1>@?0???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?7@XZ@4HA"},
        {"another block of the same function", "?$TSS0@?2???R<lambda_1>@?0???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?3@XZ@4HA"},
        {"the outer lambda's block (no static of it in the patch)", "?$TSS0@?1???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@4HA"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.what);
        SynthWorld w = basic_world(false);
        nested_world(w);
        Built b = nested_patch(c.guard);
        std::vector<cml::Diag> errors;
        BindPlan partial;
        auto r = plan_bind(w.input(b), &errors, &partial);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(count_code(errors, diag::kGuardUnmatched) == 1);
        const cml::Diag& e = errors[0];
        CAPTURE(e.message);
        CHECK(e.id() == "CML3211");
        CHECK(contains(e.message, "cannot be tied to a static of its block"));
        CHECK(contains(e.message, "is bound to the running process without a guard of its own"));
        CHECK(contains(e.message, "initialiser would run again"));
        CHECK(decision(partial, c.guard).decision == DecisionKind::PatchNewData);
        CHECK(count_code(partial.warnings, diag::kGuardUnmatched) == 0);
    }
}

TEST_CASE("R-60: a guard tied to no static, of another function than every bound static, is still the warning CML3211") {
    // No bound static of the patch can be the guard's: a fresh guard runs no bound initialiser again.
    SynthWorld w = basic_world(false);
    nested_world(w);
    Built b = nested_patch("?$TSS0@?1??other@@YAHXZ@4HA");
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, kNestedGx).decision == DecisionKind::BindOldData);
    CHECK(decision(*r, "?$TSS0@?1??other@@YAHXZ@4HA").decision == DecisionKind::PatchNewData);
    REQUIRE(count_code(r->warnings, diag::kGuardUnmatched) == 1);
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
