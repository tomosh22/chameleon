// WP-24: thread_local variables. Existing TLS binds to the old module's storage (SECREL pre-applied with the
// offset inside the old .tls section); new TLS is rejected with CML3400; dynamic TLS initialisers warn (CML3401).
#include <string>
#include <vector>

#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_plan.h"
#include "doctest.h"
#include "synth.h"
#include "worlds.h"

using namespace cml_bind_test;
using namespace cml::core;
using namespace cml::core::bind;

namespace {

constexpr std::uint16_t kTlsSec = 4;

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// basic_world plus a .tls image section (rva 0x4000) with `g_tls_counter` (public, offset 0x8) and, in the
// TU's compiland, the function-local static thread_local f::calls (S_LTHREAD32, offset 0x10).
SynthWorld tls_world() {
    SynthWorld w = basic_world(false);
    w.spec.sections.push_back({".tls", 0x100, 0x4000, 0x100, 0xC0000040});
    // The public of a thread-local carries a different offset than its S_GTHREAD32 (as in real MSVC PDBs): the
    // planner must use the S_GTHREAD32 record.
    w.pub_data("?g_tls_counter@@3HA", 0x60, kTlsSec);
    w.spec.symrec.data(0x1113, "g_tls_counter", kTlsSec, 0x8);
    w.pub_data("_Init_thread_epoch", 0x70, kTlsSec);
    w.spec.symrec.data(0x1113, "_Init_thread_epoch", kTlsSec, 0x4);
    w.pub_data("_tls_index", 0x120);
    w.tu().symbols.proc(0x1110, "f", SynthWorld::kTextSec, 0x50, 0x30);
    w.tu().symbols.data(0x1112, "calls", kTlsSec, 0x10);
    w.tu().symbols.end();
    w.open();
    return w;
}

}  // namespace

TEST_CASE("tls: an existing external thread_local binds to the old storage; SECREL gets its offset in the old .tls") {
    SynthWorld w = tls_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x20);
    const std::uint32_t tls = o.section(".tls$", kData, 8);
    const std::uint32_t var = o.symbol("?g_tls_counter@@3HA", 0, static_cast<std::int32_t>(tls), false);
    const std::uint32_t idx = o.undefined("_tls_index", false);
    o.reloc(f.section, 4, idx, kRel32);
    o.reloc(f.section, 0x10, var, coff::RelocType::SecRel);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* d = find(*r, "?g_tls_counter@@3HA");
    REQUIRE(d != nullptr);
    CHECK(d->decision == DecisionKind::BindOldData);
    CHECK(find(*r, "_tls_index")->decision == DecisionKind::BindOldData);
    bool saw = false;
    for (const Fixup& fx : r->fixups) {
        if (fx.kind == FixupKind::SecRel32) {
            saw = true;
            CHECK(fx.target_section_offset == 0x8);
            CHECK(fx.offset == 0x10);
        }
    }
    CHECK(saw);
    CHECK(r->warnings.empty());
}

TEST_CASE("tls: an undefined thread_local of the module (the CRT's epoch) is placed by its S_GTHREAD32, not its public") {
    SynthWorld w = tls_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x20);
    const std::uint32_t ep = o.undefined("_Init_thread_epoch", false);
    o.reloc(f.section, 4, ep, coff::RelocType::SecRel);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    REQUIRE(r->fixups.size() == 1);
    CHECK(r->fixups[0].kind == FixupKind::SecRel32);
    CHECK(r->fixups[0].target_section_offset == 0x4);
    CHECK(r->fixups[0].target_rva == 0x4004);  // .tls is the 4th section, rva 0x4000: not the first
}

TEST_CASE("tls: a function-local static thread_local matches the old S_LTHREAD32 record") {
    SynthWorld w = tls_world();
    SynthObj o;
    auto fn = o.function("?f@@YAXXZ", true, 0x30);
    const std::uint32_t tls = o.section(".tls$", kData, 0x20);
    const std::uint32_t var = o.symbol("?calls@?1??f@@YAXXZ@4HA", 0x10, static_cast<std::int32_t>(tls), false, false);
    o.reloc(fn.section, 8, var, coff::RelocType::SecRel, 0);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision* d = find(*r, "?calls@?1??f@@YAXXZ@4HA");
    REQUIRE(d != nullptr);
    CHECK(d->decision == DecisionKind::BindOldData);
    REQUIRE(r->fixups.size() == 1);
    CHECK(r->fixups[0].kind == FixupKind::SecRel32);
    CHECK(r->fixups[0].target_section_offset == 0x10);
}

TEST_CASE("tls: a new external thread_local is CML3400") {
    SynthWorld w = tls_world();
    SynthObj o;
    o.function("?compute_value@@YAHXZ");
    const std::uint32_t tls = o.section(".tls$", kData, 8);
    o.symbol("?g_new_tls@@3HA", 0, static_cast<std::int32_t>(tls), false);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(w.input(b), &errors);
    REQUIRE(!r.has_value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kNewThreadLocal);
    CHECK(errors[0].id() == "CML3400");
    CHECK(contains(errors[0].message, "g_new_tls"));
    CHECK(contains(errors[0].message, "new thread_local"));
    CHECK(contains(errors[0].format(), "Hot-restart"));
}

TEST_CASE("tls: a new static thread_local (no old record) is CML3400") {
    SynthWorld w = tls_world();
    SynthObj o;
    auto fn = o.function("?f@@YAXXZ", true, 0x30);
    const std::uint32_t tls = o.section(".tls$", kData, 0x20);
    const std::uint32_t var = o.symbol("?extra@?1??f@@YAXXZ@4HA", 0x14, static_cast<std::int32_t>(tls), false, false);
    o.reloc(fn.section, 8, var, coff::RelocType::SecRel);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(w.input(b), &errors);
    REQUIRE(!r.has_value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kNewThreadLocal);
    CHECK(contains(errors[0].message, "extra"));
}

TEST_CASE("tls: a REL32 to a defined thread_local is CML3102 (only the SECREL access sequence is understood)") {
    SynthWorld w = tls_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x20);
    const std::uint32_t tls = o.section(".tls$", kData, 8);
    const std::uint32_t var = o.symbol("?g_tls_counter@@3HA", 0, static_cast<std::int32_t>(tls), false);
    o.reloc(f.section, 4, var, kRel32);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(w.input(b), &errors);
    REQUIRE(!r.has_value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kThreadLocal);
}

TEST_CASE("tls: a dynamic TLS initialiser table in the patched TU warns CML3401 and does not fail the plan") {
    SynthWorld w = tls_world();
    SynthObj o;
    o.function("?compute_value@@YAHXZ", true, 0x20);
    const std::uint32_t tls = o.section(".tls$", kData, 8);
    o.symbol("?g_tls_counter@@3HA", 0, static_cast<std::int32_t>(tls), false);
    auto init = o.function("??__E?g_tls_counter@@YAXXZ", false, 0x10);
    const std::uint32_t xdu = o.section(".CRT$XDU", kRdata, 8);
    o.reloc(xdu, 0, init.symbol, kAddr64);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    REQUIRE(r->warnings.size() == 1);
    CHECK(r->warnings[0].code == diag::kThreadLocalInit);
    CHECK(r->warnings[0].id() == "CML3401");
    CHECK(contains(r->warnings[0].message, "OLD initialiser"));
}
