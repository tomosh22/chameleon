// WP-26c: statics of a translation unit compiled as C. The object's names are not Microsoft-decorated:
//   clang-cl  "platform", "platform.0" (LLVM's uniquifier), "next.counter" (function-local), "next.counter.1"
//   cl        "platform", "counter$1" (function-local, the enclosing function is not in the name)
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
    const CStaticName cl = c_static_name("counter$1");
    CHECK(cl.base == "counter");
    CHECK(cl.any_function);
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

TEST_CASE("c statics (cl): 'counter$1' is a function-local static of unknown function: unique record binds") {
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "platform", 0x20);
    w.proc_with_statics(w.tu(), "next", 0x50, {{"counter", 0x30}});
    w.open();
    SynthObj o;
    const std::int32_t sec = static_cast<std::int32_t>(o.section(".data", kData, 0x40));
    o.symbol("platform", 0x20, sec, false, false);
    o.symbol("counter$1", 0x30, sec, false, false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(find(*r, "platform")->decision == DecisionKind::BindOldData);
    const SymbolDecision* c = find(*r, "counter$1");
    CHECK(c->decision == DecisionKind::BindOldData);
    CHECK(c->old_address.rva == 0x3030);
    CHECK(r->new_globals.empty());
}

TEST_CASE("c statics (cl): two locals called counter in different functions are told apart by the unchanged layout") {
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "anchor", 0x10);
    w.proc_with_statics(w.tu(), "first", 0x50, {{"counter", 0x30}});
    w.proc_with_statics(w.tu(), "second", 0x90, {{"counter", 0x34}});
    w.open();
    SynthObj o;
    const std::int32_t sec = static_cast<std::int32_t>(o.section(".data", kData, 0x40));
    o.symbol("anchor", 0x10, sec, false, false);
    o.symbol("counter$1", 0x30, sec, false, false);
    o.symbol("counter$2", 0x34, sec, false, false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(find(*r, "counter$1")->old_address.rva == 0x3030);
    CHECK(find(*r, "counter$1")->source == BindSource::CompilandRva);
    CHECK(find(*r, "counter$2")->old_address.rva == 0x3034);
}

TEST_CASE("c statics (cl): identity that cannot be established is refused (CML3207), never bound as new") {
    SynthWorld w = basic_world(false);
    w.proc_with_statics(w.tu(), "first", 0x50, {{"counter", 0x30}});
    w.proc_with_statics(w.tu(), "second", 0x90, {{"counter", 0x34}});
    w.open();
    SynthObj o;  // no unique static to fix the layout
    const std::int32_t sec = static_cast<std::int32_t>(o.section(".data", kData, 0x40));
    o.symbol("counter$1", 0x30, sec, false, false);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(w.input(b), &errors);
    REQUIRE(!r.has_value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kAmbiguousStatic);
}

TEST_CASE("c statics: writable static data without a source-level name is refused (CML3500)") {
    SynthWorld w = basic_world(true);
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
