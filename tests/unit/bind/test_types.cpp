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

// ---- what cl really writes (CI evidence, cl 19.44): a unit hash after the closure's unique name ---------------------

TEST_CASE("lambda statics: cl's closure names carry the unit's hash, the decorated scope does not; they still pair") {
    // Old PDB: ".?AV<lambda_N>@?1??f@@YAXXZ@`9bfd0ef1". New object (compiled from another path): "`a8f45668".
    SynthWorld w;
    Types old_types;
    for (int n = 1; n <= 2; ++n) {
        const std::uint32_t type = old_types.call_operator("f::<lambda_" + std::to_string(n) + ">", closure_unique(n) + "`9bfd0ef1");
        const std::uint32_t at = 0x20 + 0x10 * static_cast<std::uint32_t>(n - 1);
        old_proc(w, "`f'::`2'::<lambda_" + std::to_string(n) + ">::operator()", 0x60 + 0x10 * static_cast<std::uint32_t>(n - 1), type, {{"sname", at}, {"$TSS0", at + 8}});
    }
    old_types.set.add_to(w.spec.tpi);
    w.open();

    Types t;
    const std::uint32_t op2 = t.call_operator("f::<lambda_2>", closure_unique(2) + "`a8f45668");
    const std::uint32_t op1 = t.call_operator("f::<lambda_1>", closure_unique(1) + "`a8f45668");
    TypedObj o;
    o.function(call_operator_symbol(2), op2, false);
    o.function(call_operator_symbol(1), op1, false);
    const std::uint32_t d = o.obj.section(".data", kData, 0x40);
    o.obj.symbol(static_in_lambda("sname", 2), 0x00, static_cast<std::int32_t>(d), false, false);
    o.obj.symbol(static_in_lambda("$TSS0", 2), 0x08, static_cast<std::int32_t>(d), false, false);
    o.obj.symbol(static_in_lambda("sname", 1), 0x10, static_cast<std::int32_t>(d), false, false);
    o.obj.symbol(static_in_lambda("$TSS0", 1), 0x18, static_cast<std::int32_t>(d), false, false);
    Built b = o.build(t.set.debug_t());
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, static_in_lambda("sname", 1)).old_address.rva == 0x3020);
    CHECK(decision(*r, static_in_lambda("$TSS0", 1)).old_address.rva == 0x3028);
    CHECK(decision(*r, static_in_lambda("sname", 2)).old_address.rva == 0x3030);
    CHECK(decision(*r, static_in_lambda("$TSS0", 2)).old_address.rva == 0x3038);
    CHECK(decision(*r, static_in_lambda("sname", 1)).source == BindSource::CompilandClosure);
    CHECK(decision(*r, call_operator_symbol(1)).old_address.rva == 0x1060);
    CHECK(decision(*r, call_operator_symbol(1)).source == BindSource::CompilandSignature);
    CHECK(decision(*r, call_operator_symbol(2)).old_address.rva == 0x1070);
}

// ---- doubt refuses -----------------------------------------------------------------------------------------------

TEST_CASE("overloads: when no new signature occurs among the old ones nothing is called new (CML3214)") {
    // Two old overloads, two new ones, all four signatures different: "both were replaced" and "the two sides spell a
    // type differently" look the same, and the second would leave both old functions silently unhooked.
    SynthWorld w;
    Types old_types;
    old_proc(w, "helper", 0x60, old_types.fn(tp::kTInt, {tp::kTInt}));
    old_proc(w, "helper", 0x70, old_types.fn(tp::kTDouble, {tp::kTDouble}));
    old_types.set.add_to(w.spec.tpi);
    w.open();
    Types t;
    TypedObj o;
    o.function(kHelperFloat, t.func_id(t.fn(tp::kTFloat, {tp::kTFloat}), "helper"));
    o.function("?helper@@YAHD@Z", t.func_id(t.fn(tp::kTInt, {tp::kTChar}), "helper"));
    Built b = o.build(t.set.debug_t());
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(errors.size() == 2);
    for (const cml::Diag& e : errors) {
        CHECK(e.id() == "CML3214");
        CHECK(contains(e.message, "none of the 2 signature(s) in the patch occurs among the 2 of the old image"));
    }
    CHECK(partial.hooks.empty());
    CHECK(partial.new_functions.empty() == false);  // (the partial plan lists them; the plan as a whole failed)
}

TEST_CASE("lambda statics: closure classes that contradict the function names are not trusted (CML3207)") {
    // The old records sit in procs named after f's lambdas, but their classes have unique names of a shape the planner
    // does not know (as if a compiler named closures by a content hash). By name the function has two such statics, by
    // closure class none: declaring the new ones "new" would silently start them from scratch.
    SynthWorld w;
    Types old_types;
    for (int n = 1; n <= 2; ++n) {
        const std::uint32_t type = old_types.call_operator("f::<lambda_" + std::to_string(n) + ">", ".?AV<lambda_0123456789abcdef0123456789abcde" + std::to_string(n) + ">@@");
        old_proc(w, "`f'::`2'::<lambda_" + std::to_string(n) + ">::operator()", 0x60 + 0x10 * static_cast<std::uint32_t>(n - 1), type,
                 {{"sname", 0x20 + 0x10 * static_cast<std::uint32_t>(n - 1)}});
    }
    old_types.set.add_to(w.spec.tpi);
    w.open();
    SynthObj o;
    const std::uint32_t d = o.section(".data", kData, 0x40);
    o.symbol(static_in_lambda("sname", 1), 0x20, static_cast<std::int32_t>(d), false, false);
    o.symbol(static_in_lambda("sname", 2), 0x30, static_cast<std::int32_t>(d), false, false);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(errors.size() == 2);
    for (const cml::Diag& e : errors) {
        CHECK(e.id() == "CML3207");
        CHECK(contains(e.message, "cannot be identified consistently (2 by the name of their function, 0 by their closure class)"));
    }
    CHECK(partial.new_globals.empty() == false);
    for (int n = 1; n <= 2; ++n) {
        CHECK(decision(partial, static_in_lambda("sname", n)).decision != DecisionKind::BindOldData);
    }
}

// ---- Godot's scene_tree.cpp (corpus run 45438434773) and the review of PR #64 ---------------------------------------

namespace {

// cl puts every function-local static into a COMDAT section of its own (CI evidence, fixture `statics`): the layout
// step can never place one.
void comdat_static(SynthObj& o, const std::string& decorated) {
    const std::uint32_t sec = o.section(".bss", kDataComdat, 8, coff::ComdatSelection::Any);
    o.symbol(decorated, 0, static_cast<std::int32_t>(sec), false, false);
}

}  // namespace

TEST_CASE("lambda statics: a lambda in block 11 or later of a member function (cl writes the block as letters) pairs like any other") {
    // SceneTree::_flush_scene_change() holds SNAME in its block 11, SceneTree::process_timers() in block 13, and the
    // TU has other SNAME lambdas and unrelated operator() functions (Node::ComparatorWithPhysicsPriority).
    const std::string in_flush = "?L@??_flush_scene_change@SceneTree@@AEAAXXZ@";
    const std::string in_timers = "?N@??process_timers@SceneTree@@AEAAXN_N@Z@";
    const std::string in_process = "?1??process@SceneTree@@UEAA_NN@Z@";
    LambdaWorld lw;
    lw.lambda(1, 0x40, 0x10, in_process, "`SceneTree::process'::`2'::");
    lw.lambda(1, 0x50, 0x20, in_flush, "`SceneTree::_flush_scene_change'::`11'::");
    lw.lambda(1, 0x60, 0x30, in_timers, "`SceneTree::process_timers'::`13'::");
    old_proc(lw.w, "Node::ComparatorWithPhysicsPriority::operator()", 0x70, lw.types.call_operator("Node::ComparatorWithPhysicsPriority", ".?AUComparatorWithPhysicsPriority@Node@@"));
    old_proc(lw.w, "Node::ComparatorWithPriority::operator()", 0x80, lw.types.call_operator("Node::ComparatorWithPriority", ".?AUComparatorWithPriority@Node@@"));
    lw.open();

    Types t;
    TypedObj o;
    for (const std::string* fn : {&in_timers, &in_flush, &in_process}) {
        o.function(call_operator_symbol(1, *fn), t.call_operator("<lambda_1>", closure_unique(1, *fn) + "`a8f45668"), false);
        comdat_static(o.obj, static_in_lambda("sname", 1, *fn));
        comdat_static(o.obj, static_in_lambda("$TSS0", 1, *fn));
    }
    Built b = o.build(t.set.debug_t());
    auto r = plan_bind(lw.w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const std::pair<const std::string*, std::uint32_t> expected[] = {{&in_process, 0x10}, {&in_flush, 0x20}, {&in_timers, 0x30}};
    for (const auto& e : expected) {
        CAPTURE(*e.first);
        const SymbolDecision s = decision(*r, static_in_lambda("sname", 1, *e.first));
        CHECK(s.decision == DecisionKind::BindOldData);
        CHECK(s.source == BindSource::CompilandClosure);
        CHECK(s.old_address.rva == 0x3000 + e.second);
        CHECK(decision(*r, static_in_lambda("$TSS0", 1, *e.first)).old_address.rva == 0x3008 + e.second);
        const SymbolDecision op = decision(*r, call_operator_symbol(1, *e.first));
        CHECK(op.decision == DecisionKind::PatchHooked);
        CHECK(op.old_address.rva == 0x1030 + e.second);
    }
    CHECK(display_name(static_in_lambda("sname", 1, in_flush)) == "SceneTree::_flush_scene_change::<lambda_1>::operator()::sname");
}

TEST_CASE("lambda statics: a closure that is exactly the symbol's scope identifies its record whatever the procedure is called") {
    // The display name of the old call operator is spelled in a way the planner cannot read (here: the block number in
    // hexadecimal). The closure's unique name is the decorated scope of the new symbol: that is the identity.
    const std::string in_flush = "?L@??_flush_scene_change@SceneTree@@AEAAXXZ@";
    const std::string in_process = "?1??process@SceneTree@@UEAA_NN@Z@";
    LambdaWorld lw;
    lw.lambda(1, 0x40, 0x10, in_process, "`SceneTree::process'::`2'::");
    lw.lambda(1, 0x50, 0x20, in_flush, "`SceneTree::_flush_scene_change'::`0xb'::");
    lw.open();
    SynthObj o;
    comdat_static(o, static_in_lambda("sname", 1, in_flush));
    comdat_static(o, static_in_lambda("$TSS0", 1, in_flush));
    comdat_static(o, static_in_lambda("sname", 1, in_process));
    comdat_static(o, static_in_lambda("$TSS0", 1, in_process));
    Built b = o.build();
    auto r = plan_bind(lw.w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, static_in_lambda("sname", 1, in_flush)).source == BindSource::CompilandClosure);
    CHECK(decision(*r, static_in_lambda("sname", 1, in_flush)).old_address.rva == 0x3020);
    CHECK(decision(*r, static_in_lambda("$TSS0", 1, in_flush)).old_address.rva == 0x3028);
    CHECK(decision(*r, static_in_lambda("sname", 1, in_process)).old_address.rva == 0x3010);
}

TEST_CASE("lambda statics: a function of an anonymous namespace (the unit's hash is in the decorated scope too) pairs by closure") {
    // namespace { int f() { return ([]{ static int sname = 1; return sname; })() + ([]{ static int sname = 2; return sname; })(); } }
    SUBCASE("cl: block numbers, a hash after the closure's unique name, old PDB linked from another path") {
        const std::string old_fn = "?1??f@?A0x9bfd0ef1@@YAHXZ@";
        const std::string new_fn = "?1??f@?A0xa8f45668@@YAHXZ@";
        LambdaWorld lw;
        for (int n = 1; n <= 2; ++n) {
            const std::uint32_t type = lw.types.call_operator("<lambda_" + std::to_string(n) + ">", closure_unique(n, old_fn) + "`9bfd0ef1");
            old_proc(lw.w, "``anonymous namespace'::f'::`2'::<lambda_" + std::to_string(n) + ">::operator()", 0x50 + 0x10 * static_cast<std::uint32_t>(n), type,
                     {{"sname", 0x10 * static_cast<std::uint32_t>(n)}});
        }
        lw.open();
        SynthObj o;
        comdat_static(o, static_in_lambda("sname", 2, new_fn));
        comdat_static(o, static_in_lambda("sname", 1, new_fn));
        Built b = o.build();
        auto r = plan_bind(lw.w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        for (int n = 1; n <= 2; ++n) {
            const SymbolDecision s = decision(*r, static_in_lambda("sname", n, new_fn));
            CHECK(s.source == BindSource::CompilandClosure);
            CHECK(s.old_address.rva == 0x3000 + 0x10 * static_cast<std::uint32_t>(n));
        }
    }
    SUBCASE("clang-cl: lambdas numbered from 0, upper-case hash, no suffix") {
        const std::string fn = "?0??f@?A0x520C9D4F@@YAHXZ@";
        const auto sym = [&fn](int n) { return "?sname@?1???R<lambda_" + std::to_string(n) + ">@" + fn + "QEBA?A?<auto>@@XZ@4HA"; };
        LambdaWorld lw;
        for (int n = 0; n < 2; ++n) {
            const std::uint32_t type = lw.types.call_operator("<lambda_" + std::to_string(n) + ">", closure_unique(n, fn));
            old_proc(lw.w, "`anonymous namespace'::f::<lambda_" + std::to_string(n) + ">::operator()", 0x60 + 0x10 * static_cast<std::uint32_t>(n), type,
                     {{"sname", 0x10 + 0x10 * static_cast<std::uint32_t>(n)}});
        }
        lw.open();
        SynthObj o;
        const std::uint32_t d = o.section(".data", kData, 0x40);
        o.symbol(sym(1), 0x00, static_cast<std::int32_t>(d), false, false);
        o.symbol(sym(0), 0x08, static_cast<std::int32_t>(d), false, false);
        Built b = o.build();
        auto r = plan_bind(lw.w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        for (int n = 0; n < 2; ++n) {
            const SymbolDecision s = decision(*r, sym(n));
            CHECK(s.source == BindSource::CompilandClosure);
            CHECK(s.old_address.rva == 0x3010 + 0x10 * static_cast<std::uint32_t>(n));
        }
    }
}

TEST_CASE("lambda statics: the lambdas of another overload do not disturb the pairing") {
    const std::string in_int = "?1??f@@YAXH@Z@";
    const std::string in_float = "?1??f@@YAXM@Z@";
    SUBCASE("an overload (or its use of the macro) was deleted: the remaining overload's statics still pair") {
        LambdaWorld lw;
        lw.lambda(1, 0x60, 0x20, in_int);
        lw.lambda(1, 0x70, 0x30, in_float);
        lw.open();
        Types t;
        TypedObj o;
        o.function(call_operator_symbol(1, in_int), t.call_operator("<lambda_1>", closure_unique(1, in_int)), false);
        comdat_static(o.obj, static_in_lambda("sname", 1, in_int));
        comdat_static(o.obj, static_in_lambda("$TSS0", 1, in_int));
        Built b = o.build(t.set.debug_t());
        auto r = plan_bind(lw.w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, static_in_lambda("sname", 1, in_int)).source == BindSource::CompilandClosure);
        CHECK(decision(*r, static_in_lambda("sname", 1, in_int)).old_address.rva == 0x3020);
        CHECK(decision(*r, static_in_lambda("$TSS0", 1, in_int)).old_address.rva == 0x3028);
        CHECK(decision(*r, call_operator_symbol(1, in_int)).old_address.rva == 0x1060);
        CHECK(r->hooks.size() == 1);  // the deleted overload's call operator is left alone
    }
    SUBCASE("the macro is new in an overload that had none: not told from a changed parameter list, refused") {
        LambdaWorld lw;
        lw.lambda(1, 0x60, 0x20, in_int);
        lw.lambda(2, 0x70, 0x30, in_int);
        lw.open();
        SynthObj o;
        comdat_static(o, static_in_lambda("sname", 1, in_float));
        Built b = o.build();
        std::vector<cml::Diag> errors;
        BindPlan partial;
        auto r = plan_bind(lw.w.input(b), &errors, &partial);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].id() == "CML3207");
        CHECK(contains(errors[0].message, "none in a function with the parameter list of the patch's"));
        CHECK(decision(partial, static_in_lambda("sname", 1, in_float)).decision != DecisionKind::BindOldData);
    }
}

TEST_CASE("lambda call operators: a changed set of lambdas is not hooked by lambda number (CML3214)") {
    // ls_over(int) and ls_over(double) each hold one int() lambda. cl numbers the lambdas of each function from 1.
    const std::string in_int = "?1??ls_over@@YAHH@Z@";
    const std::string in_double = "?1??ls_over@@YAHN@Z@";
    const auto old_world = [&](LambdaWorld& lw, bool with_types) {
        const std::uint32_t op_int = lw.types.call_operator("<lambda_1>", closure_unique(1, in_int));
        const std::uint32_t op_double = lw.types.call_operator("<lambda_1>", closure_unique(1, in_double));
        old_proc(lw.w, "`ls_over'::`2'::<lambda_1>::operator()", 0x60, op_int);
        old_proc(lw.w, "`ls_over'::`2'::<lambda_1>::operator()", 0x70, op_double);
        lw.open(with_types);
    };
    SUBCASE("a lambda inserted in front: the old <lambda_1> is now <lambda_2>; neither is hooked, the other overload is") {
        LambdaWorld lw;
        old_world(lw, true);
        Types t;
        TypedObj o;
        o.function(call_operator_symbol(1, in_int), t.call_operator("<lambda_1>", closure_unique(1, in_int)), false);
        o.function(call_operator_symbol(2, in_int), t.call_operator("<lambda_2>", closure_unique(2, in_int)), false);
        o.function(call_operator_symbol(1, in_double), t.call_operator("<lambda_1>", closure_unique(1, in_double)), false);
        Built b = o.build(t.set.debug_t());
        std::vector<cml::Diag> errors;
        BindPlan partial;
        auto r = plan_bind(lw.w.input(b), &errors, &partial);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(errors.size() == 2);
        for (const cml::Diag& e : errors) {
            CHECK(e.id() == "CML3214");
            CHECK(contains(e.message, "the lambdas of the enclosing function are not the same as in the old image (1 call operator(s) there, 2 in the patch)"));
        }
        CHECK(decision(partial, call_operator_symbol(1, in_int)).decision != DecisionKind::PatchHooked);
        CHECK(decision(partial, call_operator_symbol(2, in_int)).decision != DecisionKind::PatchHooked);
        const SymbolDecision other = decision(partial, call_operator_symbol(1, in_double));
        CHECK(other.decision == DecisionKind::PatchHooked);
        CHECK(other.old_address.rva == 0x1070);
    }
    SUBCASE("the same lambdas on both sides are hooked (the guard does not fire)") {
        LambdaWorld lw;
        old_world(lw, true);
        Types t;
        TypedObj o;
        o.function(call_operator_symbol(1, in_double), t.call_operator("<lambda_1>", closure_unique(1, in_double)), false);
        o.function(call_operator_symbol(1, in_int), t.call_operator("<lambda_1>", closure_unique(1, in_int)), false);
        Built b = o.build(t.set.debug_t());
        auto r = plan_bind(lw.w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, call_operator_symbol(1, in_int)).old_address.rva == 0x1060);
        CHECK(decision(*r, call_operator_symbol(1, in_double)).old_address.rva == 0x1070);
    }
    SUBCASE("without types (clang-cl names): a removed lambda shifts the numbers; not paired by the exact name") {
        // Old: f::<lambda_0> and f::<lambda_1>. The patch has only <lambda_0>, which is the former <lambda_1>.
        SynthWorld w;
        w.proc(w.tu(), "f::<lambda_0>::operator()", 0x60, 0x10, true);
        w.proc(w.tu(), "f::<lambda_1>::operator()", 0x70, 0x10, true);
        w.open();
        SynthObj o;
        o.function("??R<lambda_0>@?0??f@@YAXXZ@QEBA?A?<auto>@@XZ", false);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        BindPlan partial;
        auto r = plan_bind(w.input(b), &errors, &partial);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].id() == "CML3214");
        CHECK(contains(errors[0].message, "the lambdas of this name are not the same as in the old image (2 there, 1 in the patch)"));
        CHECK(partial.hooks.empty());
    }
}

TEST_CASE("lambda statics: the reason says which side has several statics of the name in one lambda") {
    // The old lambda has one `sname`; the patch has two in that lambda (a second block).
    LambdaWorld lw;
    lw.lambda(1, 0x60, 0x20);
    lw.lambda(2, 0x70, 0x30);
    lw.open();
    const std::string second = "?sname@?2???R<lambda_1>@?1??f@@YAXXZ@QEBAAEBUName@@XZ@4U2@A";
    SynthObj o;
    comdat_static(o, static_in_lambda("sname", 1));
    comdat_static(o, second);
    comdat_static(o, static_in_lambda("sname", 2));
    Built b = o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(lw.w.input(b), &errors);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(errors.size() == 2);
    for (const cml::Diag& e : errors) {
        CHECK(e.id() == "CML3207");
        CHECK(contains(e.message, "2 statics named 'sname' of the patch are in the lambda that has one in the old image"));
        CHECK_FALSE(contains(e.message, "the lambda has 1 statics"));
    }
}

// ---- same-named statics in different blocks of one function (ADR-0042, amended) --------------------------------------

namespace {

// A patch object whose functions have function-local statics, with the `.debug$S` records that say so: every static is
// a COMDAT section of its own (as cl emits it) and an S_LDATA32 record inside its procedure, tied to its symbol by a
// SECREL relocation.
class StaticsObj {
public:
    void proc(const std::string& decorated, const std::string& display, bool external = true, std::uint32_t type = 0x1000) {
        const SynthObj::Func f = obj.function(decorated, external);
        sites_.push_back({static_cast<std::uint32_t>(records_.bytes().size()) + 4 + 28, f.symbol});
        records_.proc(external ? 0x1110 : 0x110F, display, 0, 0, 0x10, 0, type);
    }
    void block() { records_.block(); }
    void end() { records_.end(); }
    void inline_site() { records_.add(0x114D, tp::Bytes(12, 0)); }
    void inline_site_end() { records_.add(0x114E, tp::Bytes()); }
    // The symbol of a static (its own section); record() names it later, so the two orders can differ.
    std::uint32_t symbol(const std::string& decorated) {
        const std::uint32_t sec = obj.section(".bss", kDataComdat, 8, coff::ComdatSelection::Any);
        return obj.symbol(decorated, 0, static_cast<std::int32_t>(sec), false, false);
    }
    void record(std::uint32_t symbol_index, const std::string& name, std::uint32_t type = tp::kTInt) {
        sites_.push_back({static_cast<std::uint32_t>(records_.bytes().size()) + 4 + 4, symbol_index});
        records_.data(0x110C, name, 0, 0, type);
    }
    void local(const std::string& decorated, const std::string& name, std::uint32_t type = tp::kTInt) { record(symbol(decorated), name, type); }

    Built build(const tp::Bytes& debug_t) {
        add_debug(".debug$T", debug_t);
        std::uint32_t base = 0;
        const tp::Bytes s = tp::debug_s_section(records_.bytes(), &base);
        const std::uint32_t sec = add_debug(".debug$S", s);
        for (const auto& site : sites_) {
            obj.reloc(sec, base + site.first, site.second, coff::RelocType::SecRel);
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
    tp::Records records_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sites_;  // offset of the field within the records, symbol
};

// Godot, scene/main/scene_tree.cpp:
//   void SceneTree::get_argument_options(...) const {
//       if (p_idx == 0) { static const Vector<StringName> names = {...}; ... }
//       else if (p_idx == 1) { static const Vector<StringName> names = {...}; ... }
const char* const kOptions = "?get_argument_options@SceneTree@@UEBAXH@Z";
const char* const kNames4 = "?names@?4??get_argument_options@SceneTree@@UEBAXH@Z@4HB";
const char* const kNames8 = "?names@?8??get_argument_options@SceneTree@@UEBAXH@Z@4HB";
const char* const kGuard0 = "?$TSS0@?4??get_argument_options@SceneTree@@UEBAXH@Z@4HA";
const char* const kGuard1 = "?$TSS1@?8??get_argument_options@SceneTree@@UEBAXH@Z@4HA";

// The old function: `names` at data 0x10 (first block) and 0x20 (second block), each with its guard.
void old_options(SynthWorld& w, std::uint32_t second_type = tp::kTInt, bool second_in_block = true) {
    tp::Records& s = w.tu().symbols;
    s.proc(0x1110, "SceneTree::get_argument_options", SynthWorld::kTextSec, 0x40, 0x20);
    s.block();
    s.data(0x110C, "names", SynthWorld::kDataSec, 0x10, tp::kTInt);
    s.data(0x110C, "$TSS0", SynthWorld::kDataSec, 0x18, tp::kTInt);
    s.end();
    if (second_in_block) {
        s.block();
    }
    s.data(0x110C, "names", SynthWorld::kDataSec, 0x20, second_type);
    s.data(0x110C, "$TSS1", SynthWorld::kDataSec, 0x28, tp::kTInt);
    if (second_in_block) {
        s.end();
    }
    s.end();
    w.pub_func(kOptions, 0x40);
}

tp::Bytes some_debug_t() {
    Types t;
    t.fn(tp::kTVoid, {tp::kTInt});
    return t.set.debug_t();
}

}  // namespace

TEST_CASE("statics of one name in two blocks of a function: paired by their place in it, with a warning (CML3570)") {
    SynthWorld w;
    old_options(w);
    w.open();

    StaticsObj o;
    // The symbols in the opposite order of the records: the records decide.
    const std::uint32_t second = o.symbol(kNames8);
    const std::uint32_t first = o.symbol(kNames4);
    o.proc(kOptions, "SceneTree::get_argument_options");
    o.block();
    o.record(first, "names");
    o.local(kGuard0, "$TSS0");
    o.end();
    o.block();
    o.record(second, "names");
    o.local(kGuard1, "$TSS1");
    o.end();
    o.end();
    Built b = o.build(some_debug_t());
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision a = decision(*r, kNames4);
    const SymbolDecision c = decision(*r, kNames8);
    CHECK(a.decision == DecisionKind::BindOldData);
    CHECK(a.source == BindSource::CompilandProcedure);
    CHECK(a.old_address.rva == 0x3010);
    CHECK(c.decision == DecisionKind::BindOldData);
    CHECK(c.source == BindSource::CompilandProcedure);
    CHECK(c.old_address.rva == 0x3020);
    CHECK(decision(*r, kGuard0).old_address.rva == 0x3018);
    CHECK(decision(*r, kGuard1).old_address.rva == 0x3028);
    CHECK(std::string(to_string(BindSource::CompilandProcedure)) == "compiland-static-procedure");
    // Never silent.
    REQUIRE(r->warnings.size() == 1);
    CHECK(r->warnings[0].code == diag::kStaticsMatchedByOrder);
    CHECK(r->warnings[0].id() == "CML3570");
    CHECK(contains(r->warnings[0].message, "Function-local statics 'names' of 'SceneTree::get_argument_options' (2) are matched by their order in the function"));
    CHECK(contains(r->warnings[0].message, "their values may be exchanged"));
}

TEST_CASE("statics of one name in two blocks of a function: anything that differs is CML3207, never a pairing") {
    const auto refused = [](SynthWorld& w, StaticsObj& o, std::size_t errors_expected, const std::string& reason) {
        Built b = o.build(some_debug_t());
        std::vector<cml::Diag> errors;
        BindPlan partial;
        auto r = plan_bind(w.input(b), &errors, &partial);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(errors.size() == errors_expected);
        for (const cml::Diag& e : errors) {
            CHECK(e.id() == "CML3207");
            CHECK_MESSAGE(contains(e.message, reason), e.message);
            CHECK(contains(e.message, "cannot be matched by their place in the function"));
        }
        for (const SymbolDecision& d : partial.decisions) {
            if (d.display == "SceneTree::get_argument_options::names") {
                CHECK(d.decision != DecisionKind::BindOldData);
            }
        }
        for (const cml::Diag& warning : partial.warnings) {
            CHECK(warning.code != diag::kStaticsMatchedByOrder);
        }
    };
    SUBCASE("another count: a third one in the patch") {
        SynthWorld w;
        old_options(w);
        w.open();
        StaticsObj o;
        o.proc(kOptions, "SceneTree::get_argument_options");
        for (const char* name : {kNames4, kNames8, "?names@?M@??get_argument_options@SceneTree@@UEBAXH@Z@4HB"}) {
            o.block();
            o.local(name, "names");
            o.end();
        }
        o.end();
        refused(w, o, 3, "the function declares 3 of them in the patch and 2 in the old image");
    }
    SUBCASE("another count: one was removed") {
        SynthWorld w;
        old_options(w);
        w.open();
        StaticsObj o;
        o.proc(kOptions, "SceneTree::get_argument_options");
        o.block();
        o.local(kNames4, "names");
        o.end();
        o.end();
        refused(w, o, 1, "the function declares 1 of them in the patch and 2 in the old image");
    }
    SUBCASE("another type") {
        SynthWorld w;
        old_options(w);
        w.open();
        StaticsObj o;
        o.proc(kOptions, "SceneTree::get_argument_options");
        o.block();
        o.local(kNames4, "names");
        o.end();
        o.block();
        o.local(kNames8, "names", tp::kTDouble);
        o.end();
        o.end();
        refused(w, o, 2, "static 2 of 2 has another type than in the old image (t74 there, t41 in the patch)");
    }
    SUBCASE("another block: the second one moved into a nested block") {
        SynthWorld w;
        old_options(w);
        w.open();
        StaticsObj o;
        o.proc(kOptions, "SceneTree::get_argument_options");
        o.block();
        o.local(kNames4, "names");
        o.block();
        o.local(kNames8, "names");
        o.end();
        o.end();
        o.end();
        refused(w, o, 2, "static 2 of 2 is declared in another block than in the old image (block path 1 there, 0.0 in the patch)");
    }
    SUBCASE("block records on one side only") {
        SynthWorld w;
        old_options(w, tp::kTInt, false);  // the old second `names` sits directly in the function
        w.open();
        StaticsObj o;
        o.proc(kOptions, "SceneTree::get_argument_options");
        o.block();
        o.local(kNames4, "names");
        o.end();
        o.block();
        o.local(kNames8, "names");
        o.end();
        o.end();
        refused(w, o, 2, "static 2 of 2 is declared in another block than in the old image (block path - there, 1 in the patch)");
    }
    SUBCASE("inside an inlined call") {
        SynthWorld w;
        old_options(w);
        w.open();
        StaticsObj o;
        o.proc(kOptions, "SceneTree::get_argument_options");
        o.block();
        o.local(kNames4, "names");
        o.end();
        o.block();
        o.inline_site();
        o.local(kNames8, "names");
        o.inline_site_end();
        o.end();
        o.end();
        refused(w, o, 2, "static 2 of 2 is declared inside an inlined call");
    }
    SUBCASE("no record in the object's .debug$S names them") {
        SynthWorld w;
        old_options(w);
        w.open();
        SynthObj o;
        o.function(kOptions, true);
        comdat_static(o, kNames4);
        comdat_static(o, kNames8);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        auto r = plan_bind(w.input(b), &errors);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(errors.size() == 2);
        CHECK(errors[0].id() == "CML3207");
        CHECK(contains(errors[0].message, "no procedure of logic.cpp.obj records it as its static"));
    }
}

TEST_CASE("one static of a name per function: the statics of two overloads pair by their function, without a warning") {
    // int f(int) { static int n; ... }  int f(float) { static int n; ... }: one display name "f::n", and under cl no
    // layout (COMDAT sections). The function each is declared in is its identity.
    const char* const f_int = "?f@@YAHH@Z";
    const char* const f_float = "?f@@YAHM@Z";
    const char* const n_int = "?n@?1??f@@YAHH@Z@4HA";
    const char* const n_float = "?n@?1??f@@YAHM@Z@4HA";
    SynthWorld w;
    w.proc_with_statics(w.tu(), "f", 0x40, {{"n", 0x10}}, 0x10);
    w.proc_with_statics(w.tu(), "f", 0x50, {{"n", 0x20}}, 0x10);
    w.pub_func(f_int, 0x40);
    w.pub_func(f_float, 0x50);
    w.open();
    StaticsObj o;
    o.proc(f_float, "f");
    o.local(n_float, "n");
    o.end();
    o.proc(f_int, "f");
    o.local(n_int, "n");
    o.end();
    Built b = o.build(some_debug_t());
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, n_int).old_address.rva == 0x3010);
    CHECK(decision(*r, n_float).old_address.rva == 0x3020);
    CHECK(decision(*r, n_int).source == BindSource::CompilandProcedure);
    CHECK(r->warnings.empty());
}

TEST_CASE("two statics of one name in ONE lambda (two blocks of its call operator) pair by their place, with the warning") {
    // The closure tells which lambda; inside it the two `sname` are told apart like in any other function.
    SynthWorld w;
    Types types;
    const std::uint32_t op = types.call_operator("<lambda_1>", closure_unique(1));
    {
        tp::Records& s = w.tu().symbols;
        s.proc(0x110F, "`f'::`2'::<lambda_1>::operator()", SynthWorld::kTextSec, 0x60, 0x10, 0, op);
        s.block();
        s.data(0x110C, "sname", SynthWorld::kDataSec, 0x20);
        s.end();
        s.block();
        s.data(0x110C, "sname", SynthWorld::kDataSec, 0x30);
        s.end();
        s.end();
    }
    types.set.add_to(w.spec.tpi);
    w.open();
    const std::string first = static_in_lambda("sname", 1);
    const std::string second = "?sname@?2???R<lambda_1>@?1??f@@YAXXZ@QEBAAEBUName@@XZ@4U2@A";
    Types t;
    const std::uint32_t new_op = t.call_operator("<lambda_1>", closure_unique(1));
    StaticsObj o;
    o.proc(call_operator_symbol(1), "`f'::`2'::<lambda_1>::operator()", false, new_op);
    o.block();
    o.local(first, "sname");
    o.end();
    o.block();
    o.local(second, "sname");
    o.end();
    o.end();
    Built b = o.build(t.set.debug_t());
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, first).old_address.rva == 0x3020);
    CHECK(decision(*r, second).old_address.rva == 0x3030);
    CHECK(decision(*r, second).source == BindSource::CompilandProcedure);
    REQUIRE(r->warnings.size() == 1);
    CHECK(r->warnings[0].id() == "CML3570");
}
