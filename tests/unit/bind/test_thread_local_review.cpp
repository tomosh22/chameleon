// WP-24 review: the TLS invariant (a SECREL target lies inside the old .tls section and comes from a thread-local
// record; nothing else points into .tls), kind matching of statics, one diagnostic per variable.
#include <string>
#include <vector>

#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_plan.h"
#include "doctest.h"
#include "synth.h"
#include "tls_world.h"
#include "worlds.h"

using namespace cml_bind_test;
using namespace cml::core;
using namespace cml::core::bind;

namespace {

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::vector<cml::Diag> plan_errors(const SynthWorld& w, const Built& b) {
    std::vector<cml::Diag> errors;
    auto r = plan_bind(w.input(b), &errors);
    CHECK_FALSE(r.has_value());
    return errors;
}

}  // namespace

TEST_CASE("tls invariant: SECREL against ordinary module data is CML3102") {
    SynthWorld w = tls_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x20);
    const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
    o.reloc(f.section, 4, tick, coff::RelocType::SecRel);
    Built b = o.build();
    auto errors = plan_errors(w, b);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kThreadLocal);
}

TEST_CASE("tls invariant: a SECREL target found only as a public (no S_GTHREAD32) is refused, never placed by the public") {
    SynthWorld w = tls_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x20);
    const std::uint32_t pub = o.undefined("?only_pub@@3HA", false);
    o.reloc(f.section, 4, pub, coff::RelocType::SecRel);
    Built b = o.build();
    auto errors = plan_errors(w, b);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kThreadLocal);
    CHECK(contains(errors[0].message, "only_pub"));
}

TEST_CASE("tls invariant: an undefined thread_local resolved by S_GTHREAD32 is never retargeted by REL32 / ADDR64") {
    SynthWorld w = tls_world();
    for (const coff::RelocType type : {kRel32, kAddr64}) {
        SynthObj o;
        auto f = o.function("?compute_value@@YAHXZ", true, 0x20);
        const std::uint32_t t = o.undefined("?g_tls_counter@@3HA", false);
        o.reloc(f.section, 4, t, type);
        Built b = o.build();
        auto errors = plan_errors(w, b);
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kThreadLocal);
        CHECK(contains(errors[0].message, "g_tls_counter"));
    }
}

TEST_CASE("tls invariant: a global that was thread_local in the old image and is ordinary data now is CML3102") {
    SynthWorld w = tls_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x20);
    const std::uint32_t data = o.section(".data", kData, 8);
    const std::uint32_t var = o.symbol("?g_tls_counter@@3HA", 0, static_cast<std::int32_t>(data), false);
    o.reloc(f.section, 4, var, kRel32);
    Built b = o.build();
    auto errors = plan_errors(w, b);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kThreadLocal);
    CHECK(contains(errors[0].message, "g_tls_counter"));
}

TEST_CASE("tls kinds: a static that gained thread_local is CML3400, one that lost it is an ordinary new static") {
    SynthWorld w = tls_world();
    {
        // old: `static int plain` (S_LDATA32); new: `static thread_local int plain`
        SynthObj o;
        auto fn = o.function("?f@@YAXXZ", true, 0x30);
        const std::uint32_t tls = o.section(".tls$", kData, 0x20);
        const std::uint32_t var = o.symbol("?plain@?1??f@@YAXXZ@4HA", 0x10, static_cast<std::int32_t>(tls), false, false);
        o.reloc(fn.section, 8, var, coff::RelocType::SecRel);
        Built b = o.build();
        auto errors = plan_errors(w, b);
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kNewThreadLocal);
        CHECK(contains(errors[0].message, "plain"));
    }
    {
        // old: `static thread_local int calls` (S_LTHREAD32); new: `static int calls`
        SynthObj o;
        auto fn = o.function("?f@@YAXXZ", true, 0x30);
        const std::uint32_t d = o.section(".data", kData, 0x40);
        const std::uint32_t var = o.symbol("?calls@?1??f@@YAXXZ@4HA", 0x20, static_cast<std::int32_t>(d), false, false);
        o.reloc(fn.section, 8, var, kRel32);
        Built b = o.build();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(find(*r, "?calls@?1??f@@YAXXZ@4HA")->decision == DecisionKind::PatchNewData);
    }
}

TEST_CASE("tls review: compiler guards and ambiguous statics get exactly one diagnostic") {
    SynthWorld w = tls_world();
    {
        // an inline function's `static thread_local` with dynamic init: the variable and its guard, both new
        SynthObj o;
        auto fn = o.function("?g@@YAHXZ", true, 0x30);
        const std::uint32_t tls = o.section(".tls$", kDataComdat, 0x20, coff::ComdatSelection::Any);
        const std::uint32_t var = o.symbol("?x@?1??g@@YAHXZ@4HA", 0, static_cast<std::int32_t>(tls), false, false);
        const std::uint32_t guard_sec = o.section(".tls$", kDataComdat, 0x8, coff::ComdatSelection::Any);
        const std::uint32_t guard = o.symbol("?$TSS0@?1??g@@YAHXZ@4HA", 0, static_cast<std::int32_t>(guard_sec), false, false);
        o.reloc(fn.section, 4, var, coff::RelocType::SecRel);
        o.reloc(fn.section, 12, guard, coff::RelocType::SecRel);
        Built b = o.build();
        auto errors = plan_errors(w, b);
        REQUIRE(errors.size() == 2);
        for (const cml::Diag& e : errors) {
            CHECK(e.code == diag::kNewThreadLocal);
        }
        bool saw_guard = false;
        for (const cml::Diag& e : errors) {
            saw_guard = saw_guard || contains(e.message, "guard");
        }
        CHECK(saw_guard);
    }
    {
        // two thread_local statics for one old record: the ambiguity errors, and no extra CML3400 for them
        SynthObj o;
        auto fn = o.function("?f@@YAXXZ", true, 0x30);
        const std::uint32_t tls = o.section(".tls$", kData, 0x20);
        const std::uint32_t a = o.symbol("?calls@?1??f@@YAXXZ@4HA", 0x10, static_cast<std::int32_t>(tls), false, false);
        const std::uint32_t c = o.symbol("?calls@?2??f@@YAXXZ@4HA", 0x14, static_cast<std::int32_t>(tls), false, false);
        o.reloc(fn.section, 4, a, coff::RelocType::SecRel);
        o.reloc(fn.section, 12, c, coff::RelocType::SecRel);
        Built b = o.build();
        auto errors = plan_errors(w, b);
        CHECK_FALSE(errors.empty());
        for (const cml::Diag& e : errors) {
            CHECK(e.code != diag::kNewThreadLocal);
        }
    }
}

TEST_CASE("tls review: several S_GTHREAD32 records for one name are an ambiguity (CML3402), not a new thread_local") {
    SynthWorld w = basic_world(false);
    w.spec.sections.push_back({".tls", 0x100, 0x4000, 0x100, 0xC0000040});
    w.spec.symrec.data(0x1113, "g_dup", kTlsSec, 0x8);
    w.spec.symrec.data(0x1113, "g_dup", kTlsSec, 0xC);
    w.open();
    SynthObj o;
    o.function("?compute_value@@YAHXZ", true, 0x20);
    const std::uint32_t tls = o.section(".tls$", kData, 8);
    o.symbol("?g_dup@@3HA", 0, static_cast<std::int32_t>(tls), false);
    Built b = o.build();
    auto errors = plan_errors(w, b);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kAmbiguousThreadLocal);
}
