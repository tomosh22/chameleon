// WP-37 (ADR-0042): the planner with type records. Static overloads pair one-to-one by canonical signature; the
// same-named function-local statics of several lambdas pair by the identity of the closure class. CML3214 / CML3207
// stay for true ambiguity and for types that cannot be read. Synthetic objects (WP-06 writer) with `.debug$S` /
// `.debug$T` sections against synthetic PDBs (WP-04 builder) with a TPI stream: no compiler, every expectation exact.
#include <array>
#include <string>
#include <utility>
#include <vector>

#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_names.h"
#include "cml/core/bind_plan.h"
#include "cml/core/bind_types.h"
#include "cml/pdb/diag_ids.h"
#include "doctest.h"
#include "synth.h"
#include "worlds.h"
#include "../pdb/type_builder.h"

using namespace cml_bind_test;
using namespace cml::core;
using namespace cml::core::bind;

namespace {

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

inline constexpr std::uint32_t kDebugChars = 0x42100040u;  // initialised data, discardable, readable, align 1

// Function types used by the tests, added to a TypeSet in a caller-chosen order.
struct Types {
    tp::TypeSet set;
    std::uint32_t args(const std::vector<std::uint32_t>& a) { return set.add(tp::kLfArgList, tp::lf_arglist(a)); }
    std::uint32_t fn(std::uint32_t ret, const std::vector<std::uint32_t>& a) {
        const std::uint32_t list = args(a);
        return set.add(tp::kLfProcedure, tp::lf_procedure(ret, list, static_cast<std::uint16_t>(a.size())));
    }
    // The call operator of a closure class with this unique name, returning int.
    std::uint32_t call_operator(const std::string& name, const std::string& unique) {
        const std::uint32_t cls = set.add(tp::kLfClass, tp::lf_udt(name, unique, true));
        const std::uint32_t const_cls = set.add(tp::kLfModifier, tp::lf_modifier(cls, 1));
        const std::uint32_t this_ptr = set.add(tp::kLfPointer, tp::lf_pointer(const_cls, tp::kPtr64 | 0x400));
        const std::uint32_t list = args({});
        return set.add(tp::kLfMFunction, tp::lf_mfunction(tp::kTInt, cls, this_ptr, list, 0));
    }
    std::uint32_t func_id(std::uint32_t type, const std::string& name) { return set.add(tp::kLfFuncId, tp::lf_func_id(type, name)); }
};

// A patch object under construction: static functions, each with a procedure record, plus the two debug sections.
class TypedObj {
public:
    // A static function (its own COMDAT section) with a procedure record naming `type` (an id when `is_id`).
    // `with_record` false: the function gets no record in .debug$S.
    SynthObj::Func function(const std::string& decorated, std::uint32_t type, bool is_id = true, bool with_record = true) {
        const SynthObj::Func f = obj.function(decorated, false);
        if (with_record) {
            procs_.push_back({static_cast<std::uint16_t>(is_id ? 0x1146 : 0x110F), decorated, type});
            symbols_.push_back(f.symbol);
        }
        return f;
    }
    // Adds `.debug$T` (unless empty) and `.debug$S` with the SECREL relocations that tie each record to its function.
    Built build(const tp::Bytes& debug_t) {
        if (!debug_t.empty()) {
            add_debug(".debug$T", debug_t);
        }
        std::vector<std::uint32_t> fields;
        const tp::Bytes s = tp::debug_s(procs_, &fields);
        const std::uint32_t sec = add_debug(".debug$S", s);
        for (std::size_t i = 0; i < fields.size(); ++i) {
            obj.reloc(sec, fields[i], symbols_[i], coff::RelocType::SecRel);
        }
        return obj.build();
    }

    SynthObj obj;

private:
    std::uint32_t add_debug(const std::string& name, const tp::Bytes& bytes) {
        const std::uint32_t sec = obj.section(name, kDebugChars, static_cast<std::uint32_t>(bytes.size()));
        coff::ByteVector& data = obj.model().sections[sec - 1].data;
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            data[i] = static_cast<std::byte>(bytes[i]);
        }
        return sec;
    }
    std::vector<tp::DebugSProc> procs_;
    std::vector<std::uint32_t> symbols_;
};

// The old module: a TU with local procs that carry type indices.
void old_proc(SynthWorld& w, const std::string& name, std::uint32_t text_offset, std::uint32_t type,
              const std::vector<std::pair<std::string, std::uint32_t>>& statics = {}) {
    w.tu().symbols.proc(0x110F, name, SynthWorld::kTextSec, text_offset, 0x10, 0, type);
    for (const auto& s : statics) {
        w.tu().symbols.data(0x110C, s.first, SynthWorld::kDataSec, s.second);
    }
    w.tu().symbols.end();
}

// (By value: the callers pass temporaries as `name`.)
SymbolDecision decision(const BindPlan& p, const std::string& name) {
    const SymbolDecision* d = p.find_decision(name);
    REQUIRE_MESSAGE(d != nullptr, name);
    return *d;
}

const char* const kHelperInt = "?helper@@YAHH@Z";
const char* const kHelperDouble = "?helper@@YANN@Z";
const char* const kHelperFloat = "?helper@@YAMM@Z";

}  // namespace

// ---- names -------------------------------------------------------------------------------------------------------

TEST_CASE("names: the scope of a static in a call operator starts with the closure's qualified name") {
    CHECK(call_operator_scope("?sname@?1???R<lambda_1>@?1??f@@YAXXZ@QEBAAEBUName@@XZ@4U2@A") == "<lambda_1>@?1??f@@YAXXZ@QEBAAEBUName@@XZ@4U2@A");
    CHECK(call_operator_scope("?$TSS0@?1???R<lambda_0>@?0??walk@Tree@@QEAAHH@Z@QEBA@XZ@4HA") == "<lambda_0>@?0??walk@Tree@@QEAAHH@Z@QEBA@XZ@4HA");
    // A scope number above 9 is written in letters.
    CHECK(call_operator_scope("?x@?BA@???R<lambda_1>@?1??f@@YAXXZ@QEBA@XZ@4HA") == "<lambda_1>@?1??f@@YAXXZ@QEBA@XZ@4HA");
    // Not in an operator(): an ordinary function, a file-scope static, a C name, a function, garbage.
    for (const char* other : {"?g@?1??statics_value@@YAHXZ@4UGuarded@@A", "?s_calls@?A0x1@@3HA", "counter", "?helper@@YAHH@Z", "??R<lambda_1>@?1??f@@YAXXZ@QEBA@XZ",
                              "?x@?1", "?x@?", "?", "", "?x@?1??", "?x@?BA???R<lambda_1>@@QEBA@XZ@4HA", "?sname@?1???"}) {
        CAPTURE(other);
        CHECK(call_operator_scope(other).empty());
    }
}

// ---- overloads ---------------------------------------------------------------------------------------------------

TEST_CASE("overloads: helper(int) / helper(double) defined in swapped order pair by signature") {
    SynthWorld w;
    Types old_types;
    const std::uint32_t old_int = old_types.fn(tp::kTInt, {tp::kTInt});
    const std::uint32_t old_double = old_types.fn(tp::kTDouble, {tp::kTDouble});
    old_proc(w, "helper", 0x60, old_int);
    old_proc(w, "helper", 0x70, old_double);
    old_types.set.add_to(w.spec.tpi);
    w.open();

    // The patch object: other type indices (a different order and 20 records in front), the functions swapped.
    Types t;
    for (int i = 0; i < 20; ++i) {
        t.set.add(tp::kLfPointer, tp::lf_pointer(tp::kTChar));
    }
    const std::uint32_t id_double = t.func_id(t.fn(tp::kTDouble, {tp::kTDouble}), "helper");
    const std::uint32_t id_int = t.func_id(t.fn(tp::kTInt, {tp::kTInt}), "helper");
    TypedObj o;
    o.function(kHelperDouble, id_double);
    o.function(kHelperInt, id_int);
    Built b = o.build(t.set.debug_t());

    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision di = decision(*r, kHelperInt);
    const SymbolDecision dd = decision(*r, kHelperDouble);
    CHECK(di.decision == DecisionKind::PatchHooked);
    CHECK(di.source == BindSource::CompilandSignature);
    CHECK(di.old_address.rva == 0x1060);
    CHECK(dd.decision == DecisionKind::PatchHooked);
    CHECK(dd.source == BindSource::CompilandSignature);
    CHECK(dd.old_address.rva == 0x1070);
    CHECK(r->hooks.size() == 2);
    CHECK(std::string(to_string(BindSource::CompilandSignature)) == "compiland-static-signature");
}

TEST_CASE("overloads: a new overload is a new function, a removed one leaves its old function alone") {
    SynthWorld w;
    Types old_types;
    old_proc(w, "helper", 0x60, old_types.fn(tp::kTInt, {tp::kTInt}));
    old_proc(w, "helper", 0x70, old_types.fn(tp::kTDouble, {tp::kTDouble}));
    old_types.set.add_to(w.spec.tpi);
    w.open();

    SUBCASE("one more overload in the patch") {
        Types t;
        TypedObj o;
        o.function(kHelperFloat, t.func_id(t.fn(tp::kTFloat, {tp::kTFloat}), "helper"));
        o.function(kHelperInt, t.func_id(t.fn(tp::kTInt, {tp::kTInt}), "helper"));
        o.function(kHelperDouble, t.func_id(t.fn(tp::kTDouble, {tp::kTDouble}), "helper"));
        Built b = o.build(t.set.debug_t());
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, kHelperInt).old_address.rva == 0x1060);
        CHECK(decision(*r, kHelperDouble).old_address.rva == 0x1070);
        const SymbolDecision f = decision(*r, kHelperFloat);
        CHECK(f.decision == DecisionKind::PatchNewFunction);
        CHECK(f.source == BindSource::None);
        CHECK(contains(f.reason, "no old function"));
        CHECK(r->hooks.size() == 2);
        REQUIRE(r->new_functions.size() == 1);
        CHECK(r->new_functions[0].name == kHelperFloat);
    }
    SUBCASE("helper(double) became helper(const char*): the old helper(double) is not hooked by anything") {
        Types t;
        const std::uint32_t const_char = t.set.add(tp::kLfModifier, tp::lf_modifier(tp::kTChar, 1));
        const std::uint32_t ptr = t.set.add(tp::kLfPointer, tp::lf_pointer(const_char));
        TypedObj o;
        o.function("?helper@@YAHPEBD@Z", t.func_id(t.fn(tp::kTInt, {ptr}), "helper"));
        o.function(kHelperInt, t.func_id(t.fn(tp::kTInt, {tp::kTInt}), "helper"));
        Built b = o.build(t.set.debug_t());
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, kHelperInt).old_address.rva == 0x1060);
        CHECK(decision(*r, "?helper@@YAHPEBD@Z").decision == DecisionKind::PatchNewFunction);
        REQUIRE(r->hooks.size() == 1);
        CHECK(r->hooks[0].old_rva == 0x1060);
    }
    SUBCASE("only one overload left in the patch: it still finds its own partner among two records") {
        Types t;
        TypedObj o;
        o.function(kHelperDouble, t.func_id(t.fn(tp::kTDouble, {tp::kTDouble}), "helper"));
        Built b = o.build(t.set.debug_t());
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, kHelperDouble).decision == DecisionKind::PatchHooked);
        CHECK(decision(*r, kHelperDouble).old_address.rva == 0x1070);
    }
    SUBCASE("procedure records that carry type indices instead of ids (S_LPROC32)") {
        Types t;
        TypedObj o;
        o.function(kHelperDouble, t.fn(tp::kTDouble, {tp::kTDouble}), false);
        o.function(kHelperInt, t.fn(tp::kTInt, {tp::kTInt}), false);
        Built b = o.build(t.set.debug_t());
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, kHelperInt).old_address.rva == 0x1060);
        CHECK(decision(*r, kHelperDouble).old_address.rva == 0x1070);
    }
}

TEST_CASE("overloads: true ambiguity is still CML3214") {
    SUBCASE("two old functions and two new ones with the same signature") {
        SynthWorld w;
        Types old_types;
        const std::uint32_t same = old_types.fn(tp::kTInt, {tp::kTInt});
        old_proc(w, "helper", 0x60, same);
        old_proc(w, "helper", 0x70, same);
        old_types.set.add_to(w.spec.tpi);
        w.open();
        Types t;
        const std::uint32_t id = t.func_id(t.fn(tp::kTInt, {tp::kTInt}), "helper");
        TypedObj o;
        o.function(kHelperInt, id);
        o.function("?helper@?A0x2@@YAHH@Z", id);  // same display name, same signature
        Built b = o.build(t.set.debug_t());
        std::vector<cml::Diag> errors;
        BindPlan partial;
        auto r = plan_bind(w.input(b), &errors, &partial);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(!errors.empty());
        for (const cml::Diag& e : errors) {
            CHECK(e.code == diag::kAmbiguousOverloads);
            CHECK(contains(e.message, "ambiguous overloads of static"));
        }
        CHECK(partial.hooks.empty());
    }
    SUBCASE("one new function, two old ones with its signature") {
        SynthWorld w;
        Types old_types;
        const std::uint32_t same = old_types.fn(tp::kTInt, {tp::kTInt});
        old_proc(w, "helper", 0x60, same);
        old_proc(w, "helper", 0x70, same);
        old_types.set.add_to(w.spec.tpi);
        w.open();
        Types t;
        TypedObj o;
        o.function(kHelperInt, t.func_id(t.fn(tp::kTInt, {tp::kTInt}), "helper"));
        Built b = o.build(t.set.debug_t());
        std::vector<cml::Diag> errors;
        auto r = plan_bind(w.input(b), &errors);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].id() == "CML3214");
        CHECK(contains(errors[0].message, "1 new and 2 old functions have the same signature"));
    }
}

TEST_CASE("overloads: unreadable types degrade to CML3214, never to a pairing") {
    // The old side is fine in every case below unless the case says otherwise.
    const auto world = [](bool with_tpi) {
        auto w = std::make_unique<SynthWorld>();
        Types old_types;
        old_proc(*w, "helper", 0x60, old_types.fn(tp::kTInt, {tp::kTInt}));
        old_proc(*w, "helper", 0x70, old_types.fn(tp::kTDouble, {tp::kTDouble}));
        if (with_tpi) {
            old_types.set.add_to(w->spec.tpi);
        }
        w->open();
        return w;
    };
    const auto expect_3214 = [](SynthWorld& w, const Built& b, const std::string& reason) {
        std::vector<cml::Diag> errors;
        BindPlan partial;
        auto r = plan_bind(w.input(b), &errors, &partial);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(errors.size() == 2);
        for (const cml::Diag& e : errors) {
            CHECK(e.id() == "CML3214");
            CHECK(contains(e.message, "cannot be read"));
            CHECK_MESSAGE(contains(e.message, reason), e.message);
            CHECK(e.what_to_do == "Rename one overload, or hot-restart.");
        }
        CHECK(partial.hooks.empty());
    };
    Types t;
    const std::uint32_t id_double = t.func_id(t.fn(tp::kTDouble, {tp::kTDouble}), "helper");
    const std::uint32_t id_int = t.func_id(t.fn(tp::kTInt, {tp::kTInt}), "helper");

    SUBCASE("the patch object has no .debug$T (compiled without debug information)") {
        auto w = world(true);
        TypedObj o;
        o.function(kHelperDouble, id_double);
        o.function(kHelperInt, id_int);
        Built b = o.build(tp::Bytes());
        expect_3214(*w, b, "no .debug$T section");
    }
    SUBCASE("the .debug$T section is not CodeView C13") {
        auto w = world(true);
        TypedObj o;
        o.function(kHelperDouble, id_double);
        o.function(kHelperInt, id_int);
        Built b = o.build(tp::Bytes{2, 0, 0, 0, 9, 9, 9, 9});
        expect_3214(*w, b, "not a CodeView C13 type section");
    }
    SUBCASE("a function without a procedure record") {
        auto w = world(true);
        TypedObj o;
        o.function(kHelperDouble, id_double);
        o.function(kHelperInt, id_int, true, false);
        Built b = o.build(t.set.debug_t());
        expect_3214(*w, b, "no procedure record");
    }
    SUBCASE("a malformed type record in the patch object") {
        auto w = world(true);
        Types bad;
        const std::uint32_t list = bad.set.add(tp::kLfPointer, tp::lf_pointer(tp::kTInt));  // not an argument list
        const std::uint32_t bad_fn = bad.set.add(tp::kLfProcedure, tp::lf_procedure(tp::kTInt, list, 1));
        TypedObj o;
        o.function(kHelperDouble, bad.func_id(bad.fn(tp::kTDouble, {tp::kTDouble}), "helper"));
        o.function(kHelperInt, bad.func_id(bad_fn, "helper"));
        Built b = o.build(bad.set.debug_t());
        expect_3214(*w, b, "LF_ARGLIST");
    }
    SUBCASE("the old PDB has no type stream") {
        auto w = world(false);
        TypedObj o;
        o.function(kHelperDouble, id_double);
        o.function(kHelperInt, id_int);
        Built b = o.build(t.set.debug_t());
        expect_3214(*w, b, "TPI");
    }
    SUBCASE("a /Zi object whose compiler PDB is missing") {
        auto w = world(true);
        tp::TypeSet server;
        server.add(tp::kLfTypeServer2, tp::lf_typeserver2({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}, 1, w->dir->file("no_such_vc143.pdb")));
        TypedObj o;
        o.function(kHelperDouble, id_double);
        o.function(kHelperInt, id_int);
        Built b = o.build(server.debug_t());
        expect_3214(*w, b, "no_such_vc143.pdb");
    }
}

TEST_CASE("overloads: a /Zi patch object reads its types from the compiler PDB it names (type server)") {
    SynthWorld w;
    Types old_types;
    old_proc(w, "helper", 0x60, old_types.fn(tp::kTInt, {tp::kTInt}));
    old_proc(w, "helper", 0x70, old_types.fn(tp::kTDouble, {tp::kTDouble}));
    old_types.set.add_to(w.spec.tpi);
    w.open();

    // The compiler PDB: types in TPI, function ids in IPI (two index spaces, both starting at 0x1000).
    tp::Spec compiler_pdb;
    compiler_pdb.guid = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18, 0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x90};
    Types types;
    for (int i = 0; i < 9; ++i) {
        types.set.add(tp::kLfPointer, tp::lf_pointer(tp::kTChar));
    }
    const std::uint32_t fn_double = types.fn(tp::kTDouble, {tp::kTDouble});
    const std::uint32_t fn_int = types.fn(tp::kTInt, {tp::kTInt});
    types.set.add_to(compiler_pdb.tpi);
    const std::uint32_t id_double = compiler_pdb.ipi.record(tp::kLfFuncId, tp::lf_func_id(fn_double, "helper"));
    const std::uint32_t id_int = compiler_pdb.ipi.record(tp::kLfFuncId, tp::lf_func_id(fn_int, "helper"));
    const std::string pdb_path = w.dir->file("vc143.pdb");
    tp::write_file(pdb_path, tp::build(compiler_pdb));

    const auto object_for = [&](const std::array<std::uint8_t, 16>& guid, std::uint32_t age) {
        tp::TypeSet server;
        server.add(tp::kLfTypeServer2, tp::lf_typeserver2(guid, age, pdb_path));
        TypedObj o;
        o.function(kHelperDouble, id_double);
        o.function(kHelperInt, id_int);
        return o.build(server.debug_t());
    };
    SUBCASE("the PDB the object was compiled against") {
        Built b = object_for(compiler_pdb.guid, 1);
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, kHelperInt).old_address.rva == 0x1060);
        CHECK(decision(*r, kHelperInt).source == BindSource::CompilandSignature);
        CHECK(decision(*r, kHelperDouble).old_address.rva == 0x1070);

        ObjectSignatures sigs(b.obj(), "logic.cpp.obj");
        CHECK(sigs.origin() == ObjectTypeOrigin::NotRead);  // nothing is read before the first question
        const SymbolDecision d = decision(*r, kHelperInt);
        CHECK(sigs.at(d.section, d.value).known);
        CHECK(sigs.origin() == ObjectTypeOrigin::TypeServer);
        CHECK(std::string(to_string(sigs.origin())) == "typeserver");
        CHECK(contains(sigs.detail(), "vc143.pdb"));
        CHECK(sigs.proc_records() == 2);
    }
    SUBCASE("a later age of the same PDB is accepted (the compiler wrote it again)") {
        Built b = object_for(compiler_pdb.guid, 7);
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, kHelperInt).old_address.rva == 0x1060);
    }
    SUBCASE("another build's PDB at that path (GUID differs): unreadable, CML3214 naming CML1434's reason") {
        std::array<std::uint8_t, 16> other = compiler_pdb.guid;
        other[0] ^= 0xFF;
        Built b = object_for(other, 1);
        std::vector<cml::Diag> errors;
        auto r = plan_bind(w.input(b), &errors);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(errors.size() == 2);
        CHECK(errors[0].id() == "CML3214");
        CHECK(contains(errors[0].message, "was compiled against"));
        ObjectSignatures sigs(b.obj(), "logic.cpp.obj");
        const cml::pdb::FunctionSignature& s = sigs.at(1, 0);
        CHECK_FALSE(s.known);
        CHECK(s.diag_id == cml::pdb::diag::kTypeServerMismatch);
        CHECK(std::string(to_string(sigs.origin())) == "unreadable");
    }
}

TEST_CASE("object signatures: where the types of a patch object come from") {
    Types t;
    const std::uint32_t id_int = t.func_id(t.fn(tp::kTInt, {tp::kTInt}), "helper");
    SUBCASE("own records") {
        TypedObj o;
        const SynthObj::Func f = o.function(kHelperInt, id_int);
        Built b = o.build(t.set.debug_t());
        ObjectSignatures sigs(b.obj(), "x.obj");
        const cml::pdb::FunctionSignature& s = sigs.at(f.section, 0);
        REQUIRE_MESSAGE(s.known, s.why);
        CHECK(s.text == "F(cc=0,fa=0,r=t74,a=[t74])");
        CHECK(std::string(to_string(sigs.origin())) == "debugT");
        // Another address has no record.
        const cml::pdb::FunctionSignature& none = sigs.at(f.section, 4);
        CHECK_FALSE(none.known);
        CHECK(none.diag_id == cml::pdb::diag::kBadObjectSymbols);
    }
    SUBCASE("precompiled types: a signature that needs none of them is known, one that does is not") {
        tp::TypeSet own;
        own.set_first(0x1200);
        const std::uint32_t list_own = own.add(tp::kLfArgList, tp::lf_arglist({tp::kTInt}));
        const std::uint32_t fn_own = own.add(tp::kLfProcedure, tp::lf_procedure(tp::kTInt, list_own, 1));
        const std::uint32_t list_pch = own.add(tp::kLfArgList, tp::lf_arglist({0x1080}));  // a type of the PCH object
        const std::uint32_t fn_pch = own.add(tp::kLfProcedure, tp::lf_procedure(tp::kTInt, list_pch, 1));
        const std::uint32_t id_own = own.add(tp::kLfFuncId, tp::lf_func_id(fn_own, "helper"));
        const std::uint32_t id_pch = own.add(tp::kLfFuncId, tp::lf_func_id(fn_pch, "helper"));
        const std::pair<std::uint16_t, tp::Bytes> head(tp::kLfPrecomp, tp::lf_precomp(0x1000, 0x200, "pch.obj"));
        TypedObj o;
        const SynthObj::Func a = o.function(kHelperInt, id_own);
        const SynthObj::Func c = o.function("?helper@@YAHAEBUPchPoint@@@Z", id_pch);
        Built b = o.build(own.debug_t(&head));
        ObjectSignatures sigs(b.obj(), "x.obj");
        CHECK(sigs.at(a.section, 0).known);
        CHECK_FALSE(sigs.at(c.section, 0).known);
        CHECK(std::string(to_string(sigs.origin())) == "debugT");
        CHECK(contains(sigs.detail(), "precompiled header object 'pch.obj'"));
    }
}

// ---- lambda-local statics --------------------------------------------------------------------------------------------

namespace {

// f() with two lambdas, each with `sname` and its guard, as cl records them: the statics sit in the lambda's
// operator(), whose class has the unique name ".?AV<lambda_N>@?1??f@@YAXXZ@".
std::string closure_unique(int n, const std::string& function = "?1??f@@YAXXZ@") {
    return ".?AV<lambda_" + std::to_string(n) + ">@" + function;
}
std::string static_in_lambda(const std::string& name, int n, const std::string& function = "?1??f@@YAXXZ@") {
    return "?" + name + "@?1???R<lambda_" + std::to_string(n) + ">@" + function + "QEBAAEBUName@@XZ@4U2@A";
}
std::string call_operator_symbol(int n, const std::string& function = "?1??f@@YAXXZ@") {
    return "??R<lambda_" + std::to_string(n) + ">@" + function + "QEBAAEBUName@@XZ";
}

struct LambdaWorld {
    SynthWorld w;
    Types types;
    // An old lambda of `function` whose operator() holds `sname` at data offset `at` and its guard right after.
    void lambda(int n, std::uint32_t text_offset, std::uint32_t at, const std::string& function = "?1??f@@YAXXZ@", const std::string& display = "`f'::`2'::") {
        const std::uint32_t type = types.call_operator("f::<lambda_" + std::to_string(n) + ">", closure_unique(n, function));
        old_proc(w, display + "<lambda_" + std::to_string(n) + ">::operator()", text_offset, type, {{"sname", at}, {"$TSS0", at + 8}});
    }
    void open(bool with_types = true) {
        if (with_types) {
            types.set.add_to(w.spec.tpi);
        }
        w.open();
    }
};

}  // namespace

TEST_CASE("lambda statics: `sname` and its guard pair by the closure class, whatever the order in the object") {
    LambdaWorld lw;
    lw.lambda(1, 0x60, 0x20);
    lw.lambda(2, 0x70, 0x30);
    lw.open();

    // The patch object lays the statics out in the opposite order (and has no type records at all: the new side of a
    // lambda static is its decorated scope).
    SynthObj o;
    const std::uint32_t d = o.section(".data", kData, 0x40);
    o.symbol(static_in_lambda("sname", 2), 0x00, static_cast<std::int32_t>(d), false, false);
    o.symbol(static_in_lambda("$TSS0", 2), 0x08, static_cast<std::int32_t>(d), false, false);
    o.symbol(static_in_lambda("sname", 1), 0x10, static_cast<std::int32_t>(d), false, false);
    o.symbol(static_in_lambda("$TSS0", 1), 0x18, static_cast<std::int32_t>(d), false, false);
    Built b = o.build();
    auto r = plan_bind(lw.w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const std::pair<std::string, std::uint32_t> expected[] = {{static_in_lambda("sname", 1), 0x3020}, {static_in_lambda("$TSS0", 1), 0x3028},
                                                              {static_in_lambda("sname", 2), 0x3030}, {static_in_lambda("$TSS0", 2), 0x3038}};
    for (const auto& e : expected) {
        const SymbolDecision s = decision(*r, e.first);
        CHECK(s.decision == DecisionKind::BindOldData);
        CHECK(s.source == BindSource::CompilandClosure);
        CHECK(s.old_address.rva == e.second);
    }
    CHECK(r->warnings.empty());
    CHECK(std::string(to_string(BindSource::CompilandClosure)) == "compiland-static-closure");
}

TEST_CASE("lambda statics: the lambdas of two overloads have the same display name and still pair") {
    // f(int) and f(float): cl numbers the lambdas of each function from 1, so both are "<lambda_1>".
    const std::string in_int = "?1??f@@YAXH@Z@";
    const std::string in_float = "?1??f@@YAXM@Z@";
    LambdaWorld lw;
    lw.lambda(1, 0x60, 0x20, in_int);
    lw.lambda(1, 0x70, 0x30, in_float);
    lw.open();

    // New side: the two call operators (static functions with procedure records) and their statics.
    Types t;
    const std::uint32_t op_float = t.call_operator("f::<lambda_1>", closure_unique(1, in_float));
    const std::uint32_t op_int = t.call_operator("f::<lambda_1>", closure_unique(1, in_int));
    TypedObj o;
    o.function(call_operator_symbol(1, in_float), op_float, false);
    o.function(call_operator_symbol(1, in_int), op_int, false);
    const std::uint32_t d = o.obj.section(".data", kData, 0x40);
    o.obj.symbol(static_in_lambda("sname", 1, in_float), 0x00, static_cast<std::int32_t>(d), false, false);
    o.obj.symbol(static_in_lambda("sname", 1, in_int), 0x10, static_cast<std::int32_t>(d), false, false);
    o.obj.symbol(static_in_lambda("$TSS0", 1, in_float), 0x08, static_cast<std::int32_t>(d), false, false);
    o.obj.symbol(static_in_lambda("$TSS0", 1, in_int), 0x18, static_cast<std::int32_t>(d), false, false);
    Built b = o.build(t.set.debug_t());
    auto r = plan_bind(lw.w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, static_in_lambda("sname", 1, in_int)).old_address.rva == 0x3020);
    CHECK(decision(*r, static_in_lambda("$TSS0", 1, in_int)).old_address.rva == 0x3028);
    CHECK(decision(*r, static_in_lambda("sname", 1, in_float)).old_address.rva == 0x3030);
    CHECK(decision(*r, static_in_lambda("$TSS0", 1, in_float)).old_address.rva == 0x3038);
    // The call operators: same qualified name, told apart by the class in their signature.
    const SymbolDecision oi = decision(*r, call_operator_symbol(1, in_int));
    const SymbolDecision of = decision(*r, call_operator_symbol(1, in_float));
    CHECK(oi.decision == DecisionKind::PatchHooked);
    CHECK(oi.source == BindSource::CompilandSignature);
    CHECK(oi.old_address.rva == 0x1060);
    CHECK(of.old_address.rva == 0x1070);
}

TEST_CASE("lambda statics: a changed set of lambdas is ambiguous (CML3207), not paired by number and not by layout") {
    LambdaWorld lw;
    lw.lambda(1, 0x60, 0x20);
    lw.lambda(2, 0x70, 0x30);
    // A plain static of the TU in the same section: it teaches the planner the section layout (old RVA - offset),
    // which before WP-37 would have placed the lambdas' statics by position.
    lw.w.data(lw.w.tu(), "`anonymous namespace'::s_plain", 0x10);
    lw.open();

    SUBCASE("one lambda more in the patch (inserted in front: every number shifted)") {
        SynthObj o;
        const std::uint32_t d = o.section(".data", kData, 0x60);
        o.symbol("?s_plain@?A0x1@@3HA", 0x10, static_cast<std::int32_t>(d), false, false);
        for (int n = 1; n <= 3; ++n) {
            o.symbol(static_in_lambda("sname", n), 0x10 + 0x10 * static_cast<std::uint32_t>(n), static_cast<std::int32_t>(d), false, false);
        }
        Built b = o.build();
        std::vector<cml::Diag> errors;
        BindPlan partial;
        auto r = plan_bind(lw.w.input(b), &errors, &partial);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(errors.size() == 3);
        for (const cml::Diag& e : errors) {
            CHECK(e.code == diag::kAmbiguousStatic);
            CHECK(e.id() == "CML3207");
            CHECK(contains(e.message, "are not the same as in the old image (2 there, 3 in the patch)"));
        }
        // The plain static is bound; none of the lambda statics is.
        CHECK(decision(partial, "?s_plain@?A0x1@@3HA").decision == DecisionKind::BindOldData);
        for (int n = 1; n <= 3; ++n) {
            CHECK(decision(partial, static_in_lambda("sname", n)).decision != DecisionKind::BindOldData);
        }
    }
    SUBCASE("one lambda fewer in the patch") {
        SynthObj o;
        const std::uint32_t d = o.section(".data", kData, 0x60);
        o.symbol(static_in_lambda("sname", 1), 0x20, static_cast<std::int32_t>(d), false, false);
        o.symbol("?s_plain@?A0x1@@3HA", 0x10, static_cast<std::int32_t>(d), false, false);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        auto r = plan_bind(lw.w.input(b), &errors);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].id() == "CML3207");
        CHECK(contains(errors[0].message, "(2 there, 1 in the patch)"));
    }
}

TEST_CASE("lambda statics: lambdas of a function that had no such static are new; other functions' statics are not touched") {
    LambdaWorld lw;
    lw.lambda(1, 0x60, 0x20);
    lw.lambda(2, 0x70, 0x30);
    lw.open();
    // g() is new in the patch and uses the macro twice.
    const std::string in_g = "?1??g@@YAXXZ@";
    SynthObj o;
    const std::uint32_t d = o.section(".data", kData, 0x60);
    o.symbol(static_in_lambda("sname", 1), 0x20, static_cast<std::int32_t>(d), false, false);
    o.symbol(static_in_lambda("sname", 2), 0x30, static_cast<std::int32_t>(d), false, false);
    o.symbol(static_in_lambda("sname", 1, in_g), 0x40, static_cast<std::int32_t>(d), false, false);
    o.symbol(static_in_lambda("sname", 2, in_g), 0x50, static_cast<std::int32_t>(d), false, false);
    Built b = o.build();
    auto r = plan_bind(lw.w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, static_in_lambda("sname", 1)).old_address.rva == 0x3020);
    CHECK(decision(*r, static_in_lambda("sname", 2)).old_address.rva == 0x3030);
    for (int n = 1; n <= 2; ++n) {
        const SymbolDecision s = decision(*r, static_in_lambda("sname", n, in_g));
        CHECK(s.decision == DecisionKind::PatchNewData);
        CHECK(contains(s.reason, "no lambda of that function"));
    }
    CHECK(r->new_globals.size() == 2);
}

TEST_CASE("lambda statics: without type records the planner does what it did before WP-37") {
    SUBCASE("no layout known: CML3207, with the reason") {
        LambdaWorld lw;
        lw.lambda(1, 0x60, 0x20);
        lw.lambda(2, 0x70, 0x30);
        lw.open(false);  // the old PDB has no TPI stream
        SynthObj o;
        const std::uint32_t d = o.section(".data", kData, 0x40);
        o.symbol(static_in_lambda("sname", 1), 0x00, static_cast<std::int32_t>(d), false, false);
        o.symbol(static_in_lambda("sname", 2), 0x10, static_cast<std::int32_t>(d), false, false);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        auto r = plan_bind(lw.w.input(b), &errors);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(errors.size() == 2);
        CHECK(errors[0].id() == "CML3207");
        CHECK(contains(errors[0].message, "cannot be told apart"));
        CHECK(contains(errors[0].message, "TPI"));
    }
    SUBCASE("an unchanged layout still places them (compiland-static-rva)") {
        LambdaWorld lw;
        lw.lambda(1, 0x60, 0x20);
        lw.lambda(2, 0x70, 0x30);
        lw.w.data(lw.w.tu(), "`anonymous namespace'::s_plain", 0x10);
        lw.open(false);
        SynthObj o;
        const std::uint32_t d = o.section(".data", kData, 0x40);
        o.symbol("?s_plain@?A0x1@@3HA", 0x10, static_cast<std::int32_t>(d), false, false);
        o.symbol(static_in_lambda("sname", 1), 0x20, static_cast<std::int32_t>(d), false, false);
        o.symbol(static_in_lambda("sname", 2), 0x30, static_cast<std::int32_t>(d), false, false);
        Built b = o.build();
        auto r = plan_bind(lw.w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, static_in_lambda("sname", 1)).source == BindSource::CompilandRva);
        CHECK(decision(*r, static_in_lambda("sname", 1)).old_address.rva == 0x3020);
        CHECK(decision(*r, static_in_lambda("sname", 2)).old_address.rva == 0x3030);
    }
}

TEST_CASE("lambda statics: the closure matches teach the section layout, so a guard without a record is placed (clang-cl)") {
    // clang-cl records `sname` but no guard variable: the guard is inferred from the unchanged layout of the section,
    // which the planner now learns from the statics it paired by closure.
    SynthWorld w;
    Types types;
    for (int n = 0; n < 2; ++n) {
        const std::uint32_t type = types.call_operator("f::<lambda_" + std::to_string(n) + ">", ".?AV<lambda_" + std::to_string(n) + ">@?0??f@@YAXXZ@");
        old_proc(w, "f::<lambda_" + std::to_string(n) + ">::operator()", 0x60 + 0x10 * static_cast<std::uint32_t>(n), type,
                 {{"sname", 0x20 + 0x10 * static_cast<std::uint32_t>(n)}});
    }
    types.set.add_to(w.spec.tpi);
    w.open();
    const auto sym = [](const std::string& name, int n) { return "?" + name + "@?1???R<lambda_" + std::to_string(n) + ">@?0??f@@YAXXZ@QEBA@XZ@4HA"; };
    SynthObj o;
    const std::uint32_t d = o.section(".bss", kData, 0x40);
    for (int n = 0; n < 2; ++n) {
        o.symbol(sym("sname", n), 0x20 + 0x10 * static_cast<std::uint32_t>(n), static_cast<std::int32_t>(d), false, false);
        o.symbol(sym("$TSS0", n), 0x28 + 0x10 * static_cast<std::uint32_t>(n), static_cast<std::int32_t>(d), false, false);
    }
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    for (int n = 0; n < 2; ++n) {
        CHECK(decision(*r, sym("sname", n)).source == BindSource::CompilandClosure);
        CHECK(decision(*r, sym("sname", n)).old_address.rva == 0x3020 + 0x10 * static_cast<std::uint32_t>(n));
        const SymbolDecision guard = decision(*r, sym("$TSS0", n));
        CHECK(guard.decision == DecisionKind::BindOldData);
        CHECK(guard.source == BindSource::CompilandLayout);
        CHECK(guard.old_address.rva == 0x3028 + 0x10 * static_cast<std::uint32_t>(n));
    }
    CHECK(r->warnings.empty());
}
