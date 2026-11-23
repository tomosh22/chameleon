// WP-26c: statics of a translation unit compiled as C. The object's names are not Microsoft-decorated:
//   clang-cl  "platform", "platform.0" (LLVM's uniquifier), "next.counter" (function-local), "next.counter.1"
//   cl        "platform"; function-local "?counter@?1??next@@9@9" (decorated even in C: seen in CI, not "counter$1")
// The PDB records carry the source names ("platform", "counter"); a function-local one is inside its proc.
// F-3 (raylib, clang-cl): `static PlatformData platform` of an #included .c file is "platform.0"; it was bound as a new
// global and the patched code read a zero-filled copy.
#include <string>

#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_names.h"
#include "cml/core/bind_plan.h"
#include "doctest.h"
#include "synth.h"
#include "worlds.h"

using namespace cml_bind_test;
using namespace cml::core;
using namespace cml::core::bind;

TEST_CASE("c statics: names of both compilers are read to (base, enclosing function)") {
    CHECK(c_static_name("platform").base == "platform");
    CHECK(c_static_name("platform").encl.empty());
    CHECK(c_static_name("platform.0").base == "platform");
    CHECK(c_static_name("platform.0").encl.empty());
    CHECK(c_static_name("next.counter").base == "counter");
    CHECK(c_static_name("next.counter").encl == "next");
    CHECK(c_static_name("next.counter.12").base == "counter");
    CHECK(c_static_name("next.counter.12").encl == "next");
    CHECK(c_static_name("counter$1").base.empty());  // not a shape any compiler emits: unreadable, so refused when writable
    CHECK(c_static_name("__const.main.table").base.empty());  // private constant copy of a local initialiser
    CHECK(c_static_name(".str.1").base.empty());
    CHECK(c_static_name("a.b.c").base.empty());
    CHECK(c_static_name("$SG1234").base.empty());
    CHECK(c_static_name("?x@@3HA").base.empty());
}

TEST_CASE("c statics (clang-cl): 'platform.0', 'next.counter' and a plain file static are bound to the old records") {
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "counter_total", 0x10);
    w.data(w.tu(), "platform", 0x20);  // PDB: the source name, not 'platform.0'
    w.proc_with_statics(w.tu(), "next", 0x50, {{"counter", 0x30}});
    w.open();
    SynthObj o;
    const std::int32_t sec = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("counter_total", 0x10, sec, false, false);
    o.symbol("platform.0", 0x20, sec, false, false);
    o.symbol("next.counter", 0x30, sec, false, false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* total = find(*r, "counter_total");
    const SymbolDecision* platform = find(*r, "platform.0");
    const SymbolDecision* counter = find(*r, "next.counter");
    REQUIRE(total != nullptr);
    REQUIRE(platform != nullptr);
    REQUIRE(counter != nullptr);
    CHECK(total->decision == DecisionKind::BindOldData);
    CHECK(platform->decision == DecisionKind::BindOldData);
    CHECK(platform->old_address.rva == 0x3020);
    CHECK(counter->decision == DecisionKind::BindOldData);
    CHECK(counter->old_address.rva == 0x3030);
    CHECK(r->new_globals.empty());
}

TEST_CASE("c statics (clang-cl): a uniquified local 'next.counter.1' binds to the local, not to a file-scope record") {
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "counter", 0x10);  // a file-scope 'counter' too
    w.proc_with_statics(w.tu(), "next", 0x50, {{"counter", 0x30}});
    w.open();
    SynthObj o;
    const std::int32_t sec = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("counter", 0x10, sec, false, false);
    o.symbol("next.counter.1", 0x30, sec, false, false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(find(*r, "counter")->old_address.rva == 0x3010);
    CHECK(find(*r, "next.counter.1")->old_address.rva == 0x3030);
    CHECK(r->new_globals.empty());
}

TEST_CASE("c statics (cl): a plain file static and a decorated function-local static bind to the old records") {
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "platform", 0x20);
    w.proc_with_statics(w.tu(), "next", 0x50, {{"counter", 0x30}});
    w.open();
    SynthObj o;
    const std::int32_t sec = static_cast<std::int32_t>(o.section(".data", kData, 0x40));
    o.symbol("platform", 0x20, sec, false, false);
    o.symbol("?counter@?1??next@@9@9", 0x30, sec, false, false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(find(*r, "platform")->decision == DecisionKind::BindOldData);
    const SymbolDecision* c = find(*r, "?counter@?1??next@@9@9");
    CHECK(c->decision == DecisionKind::BindOldData);
    CHECK(c->old_address.rva == 0x3030);
    CHECK(r->new_globals.empty());
}

TEST_CASE("c statics: a file-scope name never takes over a function-local record of the old compiland") {
    SynthWorld w = basic_world(false);
    w.proc_with_statics(w.tu(), "first", 0x50, {{"counter", 0x30}});
    w.proc_with_statics(w.tu(), "second", 0x90, {{"counter", 0x34}});
    w.open();
    SynthObj o;  // "counter" is file scope in the object, local twice in the old compiland: no record of that scope
    const std::int32_t sec = static_cast<std::int32_t>(o.section(".data", kData, 0x40));
    o.symbol("counter", 0x30, sec, false, false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    // A file-scope name never takes over a function-local record: it is a new static.
    CHECK(find(*r, "counter")->decision == DecisionKind::PatchNewData);
}

TEST_CASE("c statics: compiler-generated writable data stays new; a user-looking unreadable writable static is refused (CML3500)") {
    SynthWorld w = basic_world(true);
    {
        // clang's file-scope compound literal (`static int* t = (int[]){1, 2, 3};`), ASan and PGO data: never the user's names.
        SynthObj o;
        const std::int32_t sec = static_cast<std::int32_t>(o.section(".data", kData, 0x80));
        o.symbol(".compoundliteral", 0x00, sec, false, false);
        o.symbol(".compoundliteral.1", 0x10, sec, false, false);
        o.symbol("___asan_gen_.1", 0x20, sec, false, false);
        o.symbol("__profc_main", 0x30, sec, false, false);
        o.symbol("?$S1@?1??f@@9@9", 0x40, sec, false, false);
        Built b = o.build();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        for (const char* n : {".compoundliteral", ".compoundliteral.1", "___asan_gen_.1", "__profc_main"}) {
            const SymbolDecision* d = find(*r, n);
            REQUIRE_MESSAGE(d != nullptr, n);
            CHECK_MESSAGE(d->decision == DecisionKind::PatchNewData, n);
        }
    }
    {
        SynthObj o;
        const std::int32_t sec = static_cast<std::int32_t>(o.section(".data", kData, 0x40));
        o.symbol("a.b.c", 0x10, sec, false, false);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        auto r = plan_bind(w.input(b), &errors);
        REQUIRE(!r.has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kUnnamedStatic);
        CHECK(errors[0].id() == "CML3500");
        CHECK(errors[0].message.find("a.b.c") != std::string::npos);
        CHECK(errors[0].message.find("logic.cpp.obj") != std::string::npos);
    }
    {
        SynthObj o;  // read-only compiler constants (clang's private copy of a local initialiser) are fine as new
        const std::int32_t sec = static_cast<std::int32_t>(o.section(".rdata", kRdata, 0x40));
        o.symbol("__const.main.table", 0x10, sec, false, false);
        Built b = o.build();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(find(*r, "__const.main.table")->decision != DecisionKind::BindOldData);
        CHECK(r->warnings.empty());
    }
}

TEST_CASE("c statics: a genuinely new C static is new; a new local does not take over another function's") {
    SynthWorld w = basic_world(false);
    w.proc_with_statics(w.tu(), "next", 0x50, {{"counter", 0x30}});
    w.open();
    SynthObj o;
    const std::int32_t sec = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol("fresh_one", 0x10, sec, false, false);
    o.symbol("other.counter", 0x30, sec, false, false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(find(*r, "fresh_one")->decision == DecisionKind::PatchNewData);
    CHECK(find(*r, "other.counter")->decision == DecisionKind::PatchNewData);
}
