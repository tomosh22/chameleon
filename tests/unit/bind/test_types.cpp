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
    void record(std::uint32_t symbol_index, const std::string& name, std::uint32_t type = tp::kTInt, std::uint16_t kind = 0x110C) {
        sites_.push_back({static_cast<std::uint32_t>(records_.bytes().size()) + 4 + 4, symbol_index});
        records_.data(kind, name, 0, 0, type);
    }
    void local(const std::string& decorated, const std::string& name, std::uint32_t type = tp::kTInt) { record(symbol(decorated), name, type); }
    // WP-46: an S_LOCAL (a local variable of the procedure, not a static).
    void variable(std::uint32_t type, const std::string& name) {
        tp::Bytes b;
        tp::put32(b, type);
        tp::put16(b, 0);
        tp::putstr(b, name);
        records_.add(0x113E, b);
    }

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

TEST_CASE("a function-local thread_local paired by its function keeps the SECREL of its old slot") {
    // int g(int) { static thread_local int calls; ... }  int g(float) { static thread_local int calls; ... }
    const char* const g_int = "?g@@YAHH@Z";
    const char* const g_float = "?g@@YAHM@Z";
    const char* const calls_int = "?calls@?1??g@@YAHH@Z@4HA";
    const char* const calls_float = "?calls@?1??g@@YAHM@Z@4HA";
    constexpr std::uint16_t tls_image_section = 4;
    SynthWorld w;
    w.spec.sections.push_back({".tls", 0x100, 0x4000, 0x100, 0xC0000040});
    for (int i = 0; i < 2; ++i) {
        w.tu().symbols.proc(0x1110, "g", SynthWorld::kTextSec, 0x40 + 0x10 * static_cast<std::uint32_t>(i), 0x10);
        w.tu().symbols.data(0x1112, "calls", tls_image_section, 0x10 + 8 * static_cast<std::uint32_t>(i));  // S_LTHREAD32
        w.tu().symbols.end();
    }
    w.pub_func(g_int, 0x40);
    w.pub_func(g_float, 0x50);
    w.open();

    StaticsObj o;
    const std::uint32_t tls = o.obj.section(".tls$", kData, 0x10);
    const std::uint32_t sym_float = o.obj.symbol(calls_float, 0, static_cast<std::int32_t>(tls), false, false);
    const std::uint32_t sym_int = o.obj.symbol(calls_int, 8, static_cast<std::int32_t>(tls), false, false);
    o.proc(g_float, "g");
    o.record(sym_float, "calls", tp::kTInt, 0x1112);
    o.end();
    o.proc(g_int, "g");
    o.record(sym_int, "calls", tp::kTInt, 0x1112);
    o.end();
    // Each function reads its variable the way compilers do: a SECREL displacement.
    std::uint32_t text_float = 0;
    std::uint32_t text_int = 0;
    for (std::uint32_t i = 0; i < o.obj.model().sections.size(); ++i) {
        if (o.obj.model().sections[i].name == ".text$mn") {
            (text_float == 0 ? text_float : text_int) = i + 1;
        }
    }
    REQUIRE(text_int != 0);
    o.obj.reloc(text_float, 4, sym_float, coff::RelocType::SecRel);
    o.obj.reloc(text_int, 4, sym_int, coff::RelocType::SecRel);
    Built b = o.build(some_debug_t());
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, calls_int).source == BindSource::CompilandProcedure);
    CHECK(decision(*r, calls_int).old_address.rva == 0x4010);
    CHECK(decision(*r, calls_float).old_address.rva == 0x4018);
    std::size_t secrels = 0;
    for (const Fixup& fx : r->fixups) {
        if (fx.kind == FixupKind::SecRel32) {
            ++secrels;
            CHECK(fx.target_section_offset == (fx.symbol == calls_int ? 0x10u : 0x18u));
        }
    }
    CHECK(secrels == 2);
    CHECK(r->warnings.empty());
}

// ---- a known section layout decides; the place in the function is the fallback (re-review of PR #64) -----------------

TEST_CASE("statics of one name in two blocks: where the section's layout is known it decides, and no order pairing touches it") {
    // clang-cl without /Gw: the function-local statics share one non-COMDAT section. A plain static of the unit teaches
    // its layout (old RVA = offset + 0x3000). get_argument_options() has two `names` whose RECORD order disagrees with
    // that layout: record 1 names the symbol at 0x20, record 2 the one at 0x10, while the old records are at 0x10, 0x20.
    SynthWorld w;
    w.data(w.tu(), "`anonymous namespace'::s_plain", 0x08);
    old_options(w);
    w.open();

    StaticsObj o;
    const std::uint32_t d = o.obj.section(".data", kData, 0x40);
    o.obj.symbol("?s_plain@?A0x1@@3HA", 0x08, static_cast<std::int32_t>(d), false, false);
    const std::uint32_t at_20 = o.obj.symbol(kNames4, 0x20, static_cast<std::int32_t>(d), false, false);
    const std::uint32_t at_10 = o.obj.symbol(kNames8, 0x10, static_cast<std::int32_t>(d), false, false);
    // A guard the old image has no record for (clang-cl records none): pass C places it from the section layout, which
    // it can only do while that layout is intact.
    const char* const guard = "?$TSS7@?4??get_argument_options@SceneTree@@UEBAXH@Z@4HA";
    o.obj.symbol(guard, 0x30, static_cast<std::int32_t>(d), false, false);
    o.proc(kOptions, "SceneTree::get_argument_options");
    o.block();
    o.record(at_20, "names");
    o.end();
    o.block();
    o.record(at_10, "names");
    o.end();
    o.end();
    Built b = o.build(some_debug_t());
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    // By the layout, not by the order of the records.
    CHECK(decision(*r, kNames4).source == BindSource::CompilandRva);
    CHECK(decision(*r, kNames4).old_address.rva == 0x3020);
    CHECK(decision(*r, kNames8).source == BindSource::CompilandRva);
    CHECK(decision(*r, kNames8).old_address.rva == 0x3010);
    CHECK(decision(*r, "?s_plain@?A0x1@@3HA").old_address.rva == 0x3008);
    // The layout is still there for the rest of the section.
    const SymbolDecision g = decision(*r, guard);
    CHECK(g.decision == DecisionKind::BindOldData);
    CHECK(g.source == BindSource::CompilandLayout);
    CHECK(g.old_address.rva == 0x3030);
    CHECK(r->warnings.empty());  // no CML3570 (nothing was paired by order), no CML3211 (the guard was placed)
}

TEST_CASE("statics of one name in two blocks: a known layout that predicts nothing for them leaves them to their place") {
    // The layout of the section is known (s_plain), but an unrelated static was added in front of the two `names`:
    // their offsets moved, and the layout points at no old `names` for either. That is no opinion, not a verdict: they
    // are paired by their place in the function, with the warning, and the layout of the section is left alone.
    SynthWorld w;
    w.data(w.tu(), "`anonymous namespace'::s_plain", 0x08);
    old_options(w);
    w.open();
    StaticsObj o;
    const std::uint32_t d = o.obj.section(".data", kData, 0x60);
    o.obj.symbol("?s_plain@?A0x1@@3HA", 0x08, static_cast<std::int32_t>(d), false, false);
    o.obj.symbol("?s_new@?A0x1@@3HA", 0x10, static_cast<std::int32_t>(d), false, false);
    const std::uint32_t first = o.obj.symbol(kNames4, 0x40, static_cast<std::int32_t>(d), false, false);
    const std::uint32_t second = o.obj.symbol(kNames8, 0x50, static_cast<std::int32_t>(d), false, false);
    o.proc(kOptions, "SceneTree::get_argument_options");
    o.block();
    o.record(first, "names");
    o.end();
    o.block();
    o.record(second, "names");
    o.end();
    o.end();
    Built b = o.build(some_debug_t());
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, kNames4).source == BindSource::CompilandProcedure);
    CHECK(decision(*r, kNames4).old_address.rva == 0x3010);
    CHECK(decision(*r, kNames8).source == BindSource::CompilandProcedure);
    CHECK(decision(*r, kNames8).old_address.rva == 0x3020);
    CHECK(decision(*r, "?s_plain@?A0x1@@3HA").old_address.rva == 0x3008);
    CHECK(decision(*r, "?s_new@?A0x1@@3HA").decision == DecisionKind::PatchNewData);
    REQUIRE(r->warnings.size() == 1);
    CHECK(r->warnings[0].id() == "CML3570");
}

TEST_CASE("statics of one name in two blocks: a layout that predicts an old record for one of them decides for both") {
    // The layout puts the first `names` of the patch on the old SECOND record (a hit; the order says the first). The
    // second `names` of the patch is at an offset the layout has nothing for. The layout wins: the hit stands, and the
    // other one is not paired by its place (which would contradict the hit) but refused.
    SynthWorld w;
    w.data(w.tu(), "`anonymous namespace'::s_plain", 0x08);
    old_options(w);
    w.open();
    StaticsObj o;
    const std::uint32_t d = o.obj.section(".data", kData, 0x60);
    o.obj.symbol("?s_plain@?A0x1@@3HA", 0x08, static_cast<std::int32_t>(d), false, false);
    const std::uint32_t first = o.obj.symbol(kNames4, 0x20, static_cast<std::int32_t>(d), false, false);   // old second record is at 0x20
    const std::uint32_t second = o.obj.symbol(kNames8, 0x50, static_cast<std::int32_t>(d), false, false);  // nothing there
    o.proc(kOptions, "SceneTree::get_argument_options");
    o.block();
    o.record(first, "names");
    o.end();
    o.block();
    o.record(second, "names");
    o.end();
    o.end();
    Built b = o.build(some_debug_t());
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].id() == "CML3207");
    // (WP-40, R-48 item 10: the reason names the static whose prediction decided, and where it put it.)
    CHECK_MESSAGE(contains(errors[0].message, "on the old record at rva 0x3020 and places no old static named 'names' of 'SceneTree::get_argument_options' at the offset of this one; the layout decides"),
                  errors[0].message);
    CHECK(decision(partial, kNames4).source == BindSource::CompilandRva);
    CHECK(decision(partial, kNames4).old_address.rva == 0x3020);
    CHECK(decision(partial, kNames8).decision != DecisionKind::BindOldData);
    for (const cml::Diag& warning : partial.warnings) {
        CHECK(warning.code != diag::kStaticsMatchedByOrder);
    }
}

TEST_CASE("statics paired by place need their guards bound by a record: a record-less guard refuses the set, it is never left a fresh guard") {
    // clang-cl: `if (i == 0) { static const T names = make(); } else { static const T names = make(); }` in a unit whose
    // only statics these are. The two `names` and their guards share .bss; clang-cl records no guard variable; nothing
    // teaches the section's layout. Paired by place, the guards could not be bound: the patch would start with fresh
    // guards and construct the two bound old objects again. Refused instead (CML3207), as before WP-37.
    const char* const options = "?options@LsTree@@QEBAHH@Z";
    const char* const names_1 = "?names@?1??options@LsTree@@QEBAHH@Z@4ULsList@@B";
    const char* const names_2 = "?names@?2??options@LsTree@@QEBAHH@Z@4ULsList@@B";
    const char* const guard_1 = "?$TSS0@?1??options@LsTree@@QEBAHH@Z@4HA";
    const char* const guard_2 = "?$TSS1@?2??options@LsTree@@QEBAHH@Z@4HA";
    const auto old_world = [&](SynthWorld& w, bool with_teacher) {
        if (with_teacher) {
            w.data(w.tu(), "`anonymous namespace'::s_plain", 0x08);
        }
        w.proc_with_statics(w.tu(), "LsTree::options", 0x40, {{"names", 0x10}, {"names", 0x20}}, 0x20);  // no guard records, no blocks
        w.pub_func(options, 0x40);
        w.open();
    };
    const auto object = [&](StaticsObj& o, std::uint32_t shift, bool with_teacher) {
        const std::uint32_t d = o.obj.section(".bss", kData, 0x80);
        if (with_teacher) {
            o.obj.symbol("?s_plain@?A0x1@@3HA", 0x08, static_cast<std::int32_t>(d), false, false);
        }
        const std::uint32_t first = o.obj.symbol(names_1, 0x10 + shift, static_cast<std::int32_t>(d), false, false);
        o.obj.symbol(guard_1, 0x18 + shift, static_cast<std::int32_t>(d), false, false);
        const std::uint32_t second = o.obj.symbol(names_2, 0x20 + shift, static_cast<std::int32_t>(d), false, false);
        o.obj.symbol(guard_2, 0x28 + shift, static_cast<std::int32_t>(d), false, false);
        o.proc(options, "LsTree::options");
        o.record(first, "names");
        o.record(second, "names");
        o.end();
    };
    const auto refused_for_the_guard = [&](SynthWorld& w, StaticsObj& o) {
        Built b = o.build(some_debug_t());
        std::vector<cml::Diag> errors;
        BindPlan partial;
        auto r = plan_bind(w.input(b), &errors, &partial);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(errors.size() == 2);
        for (const cml::Diag& e : errors) {
            CHECK(e.id() == "CML3207");
            CHECK_MESSAGE(contains(e.message, "the guard variable 'LsTree::options::$TSS0' (?$TSS0@?1??options@LsTree@@QEBAHH@Z@4HA) of static 1 of 2 has no record in the old image"), e.message);
        }
        CHECK(decision(partial, names_1).decision != DecisionKind::BindOldData);
        CHECK(decision(partial, names_2).decision != DecisionKind::BindOldData);
        CHECK(decision(partial, guard_1).decision != DecisionKind::BindOldData);
        CHECK(decision(partial, guard_2).decision != DecisionKind::BindOldData);
        for (const cml::Diag& warning : partial.warnings) {
            CHECK(warning.code != diag::kStaticsMatchedByOrder);
            CHECK_MESSAGE(warning.code != diag::kGuardUnmatched, warning.message);  // an error, never the "fresh guard" warning
        }
    };
    SUBCASE("nothing teaches the layout of the section") {
        SynthWorld w;
        old_world(w, false);
        StaticsObj o;
        object(o, 0, false);
        refused_for_the_guard(w, o);
    }
    SUBCASE("the layout is known but predicts nothing for them (they moved): the guards are not left to that layout either") {
        SynthWorld w;
        old_world(w, true);
        StaticsObj o;
        object(o, 0x40, true);
        refused_for_the_guard(w, o);
    }
    SUBCASE("the layout is known and places them: by layout, and pass C binds the guards from it") {
        SynthWorld w;
        old_world(w, true);
        StaticsObj o;
        object(o, 0, true);
        Built b = o.build(some_debug_t());
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, names_1).source == BindSource::CompilandRva);
        CHECK(decision(*r, names_1).old_address.rva == 0x3010);
        CHECK(decision(*r, names_2).old_address.rva == 0x3020);
        CHECK(decision(*r, guard_1).source == BindSource::CompilandLayout);
        CHECK(decision(*r, guard_1).old_address.rva == 0x3018);
        CHECK(decision(*r, guard_2).old_address.rva == 0x3028);
        CHECK(r->warnings.empty());
    }
}

TEST_CASE("one static of a name in each of two functions, in one shared section") {
    // int f(int) { static int n; }  int f(float) { static int n; }: the two functions whose statics an earlier version
    // of the by-place step made refuse each other (it learned a layout from the first and held it against the second).
    const char* const f_int = "?f@@YAHH@Z";
    const char* const f_float = "?f@@YAHM@Z";
    const char* const n_int = "?n@?1??f@@YAHH@Z@4HA";
    const char* const n_float = "?n@?1??f@@YAHM@Z@4HA";
    const auto old_world = [&](SynthWorld& w, bool with_plain) {
        if (with_plain) {
            w.data(w.tu(), "`anonymous namespace'::s_plain", 0x08);
        }
        w.proc_with_statics(w.tu(), "f", 0x40, {{"n", 0x10}}, 0x10);
        w.proc_with_statics(w.tu(), "f", 0x50, {{"n", 0x20}}, 0x10);
        w.pub_func(f_int, 0x40);
        w.pub_func(f_float, 0x50);
        w.open();
    };
    const auto object = [&](StaticsObj& o, std::uint32_t int_at, std::uint32_t float_at, bool with_plain) {
        const std::uint32_t d = o.obj.section(".data", kData, 0x40);
        if (with_plain) {
            o.obj.symbol("?s_plain@?A0x1@@3HA", 0x08, static_cast<std::int32_t>(d), false, false);
        }
        const std::uint32_t sym_float = o.obj.symbol(n_float, float_at, static_cast<std::int32_t>(d), false, false);
        const std::uint32_t sym_int = o.obj.symbol(n_int, int_at, static_cast<std::int32_t>(d), false, false);
        o.proc(f_float, "f");
        o.record(sym_float, "n");
        o.end();
        o.proc(f_int, "f");
        o.record(sym_int, "n");
        o.end();
    };
    SUBCASE("the section's layout is known: both are placed by it, without a warning, and the layout stays") {
        SynthWorld w;
        old_world(w, true);
        StaticsObj o;
        object(o, 0x10, 0x20, true);
        Built b = o.build(some_debug_t());
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, n_int).source == BindSource::CompilandRva);
        CHECK(decision(*r, n_int).old_address.rva == 0x3010);
        CHECK(decision(*r, n_float).source == BindSource::CompilandRva);
        CHECK(decision(*r, n_float).old_address.rva == 0x3020);
        CHECK(r->warnings.empty());
    }
    SUBCASE("no layout is known and the object orders them the other way round: each pairs by its function, no warning") {
        SynthWorld w;
        old_world(w, false);
        StaticsObj o;
        object(o, 0x20, 0x10, false);  // f(int)'s static now lies behind f(float)'s
        Built b = o.build(some_debug_t());
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, n_int).source == BindSource::CompilandProcedure);
        CHECK(decision(*r, n_int).old_address.rva == 0x3010);
        CHECK(decision(*r, n_float).source == BindSource::CompilandProcedure);
        CHECK(decision(*r, n_float).old_address.rva == 0x3020);
        CHECK(r->warnings.empty());
    }
}

TEST_CASE("the by-place step is not asked about statics without a function scope, and says when two procedures claim one") {
    SUBCASE("two file-scope statics of one name: the reason is not about procedure records") {
        // Two records named 's_calls' at file scope in the old compiland, two symbols in the patch, no layout.
        SynthWorld w;
        w.data(w.tu(), "`anonymous namespace'::s_calls", 0x10);
        w.data(w.tu(), "`anonymous namespace'::s_calls", 0x20);
        w.open();
        SynthObj o;
        comdat_static(o, "?s_calls@?A0x1@@3HA");
        comdat_static(o, "?s_calls@?A0x1@@3NA");
        Built b = o.build();
        std::vector<cml::Diag> errors;
        auto r = plan_bind(w.input(b), &errors);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(errors.size() == 2);
        for (const cml::Diag& e : errors) {
            CHECK(e.id() == "CML3207");
            CHECK_FALSE(contains(e.message, "procedure"));
        }
    }
    SUBCASE("the records of two procedures name one symbol") {
        SynthWorld w;
        old_options(w);
        w.open();
        StaticsObj o;
        const std::uint32_t first = o.symbol(kNames4);
        const std::uint32_t second = o.symbol(kNames8);
        o.proc(kOptions, "SceneTree::get_argument_options");
        o.record(first, "names");
        o.record(second, "names");
        o.end();
        o.proc("?other@@YAXXZ", "other");
        o.record(first, "names");
        o.end();
        Built b = o.build(some_debug_t());
        std::vector<cml::Diag> errors;
        auto r = plan_bind(w.input(b), &errors);
        REQUIRE_FALSE(r.has_value());
        bool said = false;
        for (const cml::Diag& e : errors) {
            CHECK(e.id() == "CML3207");
            said = said || contains(e.message, "the records of several procedures of logic.cpp.obj name it as their static");
        }
        CHECK(said);
    }
}

// ---- Dear ImGui, imgui_widgets.cpp (corpus runs 45410039045 and 45477350367) ----------------------------------------

TEST_CASE("a function-local static in block 11 or later is the old static, not a new one") {
    // ImGui::ColorEdit4() declares `static const char* fmt_table_float[3][4]` deep inside; cl names it
    // ?fmt_table_float@?BL@??ColorEdit4@ImGui@@YA_NPEBDQEAMH@Z@4PAY03PEBDA (block 27, "?BL@"). The name reader used to
    // give that scope up, the static had no enclosing function, found no record and was declared NEW: the patch got a
    // fresh copy and the reload "passed" (run 45410039045). It is the old static of ImGui::ColorEdit4.
    const char* const color_edit = "?ColorEdit4@ImGui@@YA_NPEBDQEAMH@Z";
    const char* const table_float = "?fmt_table_float@?BL@??ColorEdit4@ImGui@@YA_NPEBDQEAMH@Z@4PAY03PEBDA";
    const char* const table_int = "?fmt_table_int@?BL@??ColorEdit4@ImGui@@YA_NPEBDQEAMH@Z@4PAY03PEBDA";
    SynthWorld w;
    w.proc_with_statics(w.tu(), "ImGui::ColorEdit4", 0x40, {{"fmt_table_int", 0x10}, {"fmt_table_float", 0x40}}, 0x20);
    w.pub_func(color_edit, 0x40);
    w.open();
    SynthObj o;
    o.function(color_edit, true);
    const std::uint32_t d = o.section(".data", kData, 0x80);
    o.symbol(table_int, 0x00, static_cast<std::int32_t>(d), false, false);
    o.symbol(table_float, 0x30, static_cast<std::int32_t>(d), false, false);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision f = decision(*r, table_float);
    CHECK(f.display == "ImGui::ColorEdit4::fmt_table_float");
    CHECK(f.decision == DecisionKind::BindOldData);
    CHECK(f.source == BindSource::CompilandStatic);
    CHECK(f.old_address.rva == 0x3040);
    CHECK(decision(*r, table_int).decision == DecisionKind::BindOldData);
    CHECK(decision(*r, table_int).old_address.rva == 0x3010);
    CHECK(r->new_globals.empty());
}

// ---- WP-40 (ADR-0043, R-48 items 5..11): what the type reader and the by-place step still got wrong ----------------

namespace {

// A call operator of closure <lambda_n> of f() returning int with the parameter list `args`.
std::uint32_t call_operator_with(Types& t, int n, const std::vector<std::uint32_t>& args) {
    const std::uint32_t cls = t.set.add(tp::kLfClass, tp::lf_udt("f::<lambda_" + std::to_string(n) + ">", closure_unique(n), true));
    const std::uint32_t const_cls = t.set.add(tp::kLfModifier, tp::lf_modifier(cls, 1));
    const std::uint32_t this_ptr = t.set.add(tp::kLfPointer, tp::lf_pointer(const_cls, tp::kPtr64 | 0x400));
    const std::uint32_t list = t.args(args);
    return t.set.add(tp::kLfMFunction, tp::lf_mfunction(tp::kTInt, cls, this_ptr, list, static_cast<std::uint16_t>(args.size())));
}

}  // namespace

TEST_CASE("lambda statics (R-48 item 5): two lambdas that changed places are told apart by their call operators' signatures") {
    // f() holds `[]() {...}` (lambda 1) and `[](int) {...}` (lambda 2), each with `sname`; the edit swaps the two
    // expressions, so the compiler numbers them the other way round. main: both statics paired by their (unchanged)
    // numbers, i.e. exchanged.
    const auto run = [](bool swapped, std::vector<cml::Diag>& errors, BindPlan& partial) {
        SynthWorld w;
        Types old_types;
        const std::uint32_t op1 = call_operator_with(old_types, 1, {});
        const std::uint32_t op2 = call_operator_with(old_types, 2, {tp::kTInt});
        old_proc(w, "`f'::`2'::<lambda_1>::operator()", 0x60, op1, {{"sname", 0x20}, {"$TSS0", 0x28}});
        old_proc(w, "`f'::`2'::<lambda_2>::operator()", 0x70, op2, {{"sname", 0x30}, {"$TSS0", 0x38}});
        old_types.set.add_to(w.spec.tpi);
        w.open();
        Types t;
        // The closure names stay <lambda_1> / <lambda_2>: only the signatures can say which lambda is which.
        const std::uint32_t new_op1 = call_operator_with(t, 1, swapped ? std::vector<std::uint32_t>{tp::kTInt} : std::vector<std::uint32_t>{});
        const std::uint32_t new_op2 = call_operator_with(t, 2, swapped ? std::vector<std::uint32_t>{} : std::vector<std::uint32_t>{tp::kTInt});
        StaticsObj o;
        o.proc(call_operator_symbol(1), "`f'::`2'::<lambda_1>::operator()", false, new_op1);
        o.end();
        o.proc(call_operator_symbol(2), "`f'::`2'::<lambda_2>::operator()", false, new_op2);
        o.end();
        for (int n = 1; n <= 2; ++n) {
            o.symbol(static_in_lambda("sname", n));
            o.symbol(static_in_lambda("$TSS0", n));
        }
        Built b = o.build(t.set.debug_t());
        auto r = plan_bind(w.input(b), &errors, &partial);
        if (r) {
            partial = *r;
        }
        return r.has_value();
    };
    SUBCASE("swapped: refused (CML3207), nothing paired by number") {
        std::vector<cml::Diag> errors;
        BindPlan partial;
        REQUIRE_FALSE(run(true, errors, partial));
        REQUIRE_FALSE(errors.empty());
        bool named = false;
        for (const cml::Diag& e : errors) {
            // CML3207 for the statics; the call operators themselves no longer pair by signature either (CML3214).
            CHECK((e.id() == "CML3207" || e.id() == "CML3214"));
            named = named || (e.id() == "CML3207" && contains(e.message, "has another signature than the old one of that number"));
        }
        CHECK(named);
        for (int n = 1; n <= 2; ++n) {
            CHECK(decision(partial, static_in_lambda("sname", n)).decision != DecisionKind::BindOldData);
            CHECK(decision(partial, static_in_lambda("$TSS0", n)).decision != DecisionKind::BindOldData);
        }
    }
    SUBCASE("in place: paired by closure as before") {
        std::vector<cml::Diag> errors;
        BindPlan partial;
        REQUIRE(run(false, errors, partial));
        CHECK(decision(partial, static_in_lambda("sname", 1)).old_address.rva == 0x3020);
        CHECK(decision(partial, static_in_lambda("sname", 2)).old_address.rva == 0x3030);
        CHECK(decision(partial, static_in_lambda("sname", 2)).source == BindSource::CompilandClosure);
    }
}

TEST_CASE("lambda statics (R-48 item 6): paired by closure, but their section layout contradicts itself: refused, not given fresh guards") {
    // clang-cl: the two lambdas' statics lie the other way round in the patch's .bss. The closure step pairs both; the
    // two pairs disagree on the section layout, which is dropped, so no guard can be placed. main: two CML3211 (fresh
    // guards), plan accepted: both bound `sname` constructed a second time on their next use.
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
        const std::uint32_t at = 0x30 - 0x10 * static_cast<std::uint32_t>(n);  // lambda 0 at 0x30, lambda 1 at 0x20
        o.symbol(sym("sname", n), at, static_cast<std::int32_t>(d), false, false);
        o.symbol(sym("$TSS0", n), at + 8, static_cast<std::int32_t>(d), false, false);
    }
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(w.input(b), &errors, &partial);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(errors.size() == 2);
    for (const cml::Diag& e : errors) {
        CHECK(e.code == diag::kGuardMismatch);
        CHECK(contains(e.message, "no layout of its object section is known"));
    }
    CHECK(decision(partial, sym("sname", 0)).source == BindSource::CompilandClosure);
    CHECK(partial.warnings.empty());
}

TEST_CASE("by place (R-48 item 7): cl's `$S1` guards of /Zc:threadSafeInit- pair by place too, instead of refusing themselves") {
    // Without thread-safe statics cl guards each block with a bitmask `$S1` (one name per block, so two `$S1` in one
    // function). main: the `$S1` pair reached the by-place step, whose guard scan found each member as "a guard of its
    // block" without a binding and refused it (CML3207), and the two `names` with it.
    const char* const s1_4 = "?$S1@?4??get_argument_options@SceneTree@@UEBAXH@Z@4IA";
    const char* const s1_8 = "?$S1@?8??get_argument_options@SceneTree@@UEBAXH@Z@4IA";
    SynthWorld w;
    tp::Records& s = w.tu().symbols;
    s.proc(0x1110, "SceneTree::get_argument_options", SynthWorld::kTextSec, 0x40, 0x20);
    s.block();
    s.data(0x110C, "names", SynthWorld::kDataSec, 0x10, tp::kTInt);
    s.data(0x110C, "$S1", SynthWorld::kDataSec, 0x18, tp::kTInt);
    s.end();
    s.block();
    s.data(0x110C, "names", SynthWorld::kDataSec, 0x20, tp::kTInt);
    s.data(0x110C, "$S1", SynthWorld::kDataSec, 0x28, tp::kTInt);
    s.end();
    s.end();
    w.pub_func(kOptions, 0x40);
    w.open();
    StaticsObj o;
    o.proc(kOptions, "SceneTree::get_argument_options");
    o.block();
    o.local(kNames4, "names");
    o.local(s1_4, "$S1");
    o.end();
    o.block();
    o.local(kNames8, "names");
    o.local(s1_8, "$S1");
    o.end();
    o.end();
    Built b = o.build(some_debug_t());
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, kNames4).old_address.rva == 0x3010);
    CHECK(decision(*r, kNames8).old_address.rva == 0x3020);
    CHECK(decision(*r, s1_4).source == BindSource::CompilandProcedure);
    CHECK(decision(*r, s1_4).old_address.rva == 0x3018);
    CHECK(decision(*r, s1_8).old_address.rva == 0x3028);
    std::size_t order_warnings = 0;
    for (const cml::Diag& warning : r->warnings) {
        order_warnings += warning.code == diag::kStaticsMatchedByOrder ? 1u : 0u;
    }
    CHECK(order_warnings == 2);  // the `names` and the `$S1`, each a pairing by place
}

TEST_CASE("by place (R-48 item 8, refusal kept): an unbound guard of a NEW static in the block refuses the pairing") {
    // The edit adds a guarded `extra` to the first block of get_argument_options. Its guard `$TSS2` has no record. cl
    // numbers guards per function, so whether `$TSS2` is extra's (sound) or the guard of the old `names` while `extra`
    // took names's old number (a new static would then never be constructed) cannot be told from the names. Kept refused,
    // on purpose; this passes on main as well.
    const char* const extra = "?extra@?4??get_argument_options@SceneTree@@UEBAXH@Z@4HA";
    const char* const guard_extra = "?$TSS2@?4??get_argument_options@SceneTree@@UEBAXH@Z@4HA";
    SynthWorld w;
    old_options(w);
    w.open();
    StaticsObj o;
    o.proc(kOptions, "SceneTree::get_argument_options");
    o.block();
    o.local(kNames4, "names");
    o.local(kGuard0, "$TSS0");
    o.local(extra, "extra");
    o.local(guard_extra, "$TSS2");
    o.end();
    o.block();
    o.local(kNames8, "names");
    o.local(kGuard1, "$TSS1");
    o.end();
    o.end();
    Built b = o.build(some_debug_t());
    std::vector<cml::Diag> errors;
    BindPlan partial;
    REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
    bool guard_reason = false;
    for (const cml::Diag& e : errors) {
        CHECK(e.id() == "CML3207");
        guard_reason = guard_reason || contains(e.message, "$TSS2");
    }
    CHECK(guard_reason);
    CHECK(decision(partial, kNames4).decision != DecisionKind::BindOldData);
}

TEST_CASE("by place (R-48 item 9): the guards of statics refused for another reason get no CML3211 next to the CML3207") {
    // clang-cl shape (no guard records, every static in a section of its own: no layout). The edit adds a third `names`
    // block: the counts differ. main: the three CML3207 and, from pass C, three CML3211 for their guards.
    SynthWorld w;
    w.proc_with_statics(w.tu(), "SceneTree::get_argument_options", 0x40, {{"names", 0x10}, {"names", 0x20}}, 0x20);
    w.pub_func(kOptions, 0x40);
    w.open();
    StaticsObj o;
    o.proc(kOptions, "SceneTree::get_argument_options");
    for (const char* const block : {"?4?", "?8?", "?9?"}) {
        const std::string scope = std::string(block) + "?get_argument_options@SceneTree@@UEBAXH@Z@4";
        o.local("?names@" + scope + "HB", "names");
        o.symbol("?$TSS0@" + scope + "HA");
    }
    o.end();
    Built b = o.build(some_debug_t());
    std::vector<cml::Diag> errors;
    BindPlan partial;
    REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
    REQUIRE(errors.size() == 3);
    for (const cml::Diag& e : errors) {
        CHECK(e.id() == "CML3207");
        CHECK(contains(e.message, "declares 3 of them in the patch and 2 in the old image"));
    }
    for (const cml::Diag& warning : partial.warnings) {
        CHECK_MESSAGE(warning.code != diag::kGuardUnmatched, warning.message);
    }
}

TEST_CASE("by place (R-48 item 11): same-named block statics of an internal-linkage function pair under cl's qualified record names") {
    // cl records the statics of a function with internal linkage under a qualified name ("`opts'::`2'::names"). main:
    // the by-place step compared the whole name with "names", found none ("declares 0 of them") and refused.
    const char* const opts = "?opts@@YAHH@Z";
    const char* const names_a = "?names@?1??opts@@YAHH@Z@4HB";
    const char* const names_b = "?names@?2??opts@@YAHH@Z@4HB";
    const char* const guard_a = "?$TSS0@?1??opts@@YAHH@Z@4HA";
    const char* const guard_b = "?$TSS1@?2??opts@@YAHH@Z@4HA";
    SynthWorld w;
    tp::Records& s = w.tu().symbols;
    s.proc(0x110F, "opts", SynthWorld::kTextSec, 0x40, 0x20);
    s.block();
    s.data(0x110C, "`opts'::`2'::names", SynthWorld::kDataSec, 0x10, tp::kTInt);
    s.data(0x110C, "`opts'::`2'::$TSS0", SynthWorld::kDataSec, 0x18, tp::kTInt);
    s.end();
    s.block();
    s.data(0x110C, "`opts'::`3'::names", SynthWorld::kDataSec, 0x20, tp::kTInt);
    s.data(0x110C, "`opts'::`3'::$TSS1", SynthWorld::kDataSec, 0x28, tp::kTInt);
    s.end();
    s.end();
    w.open();
    StaticsObj o;
    o.proc(opts, "opts", false);
    o.block();
    o.local(names_a, "`opts'::`2'::names");
    o.local(guard_a, "`opts'::`2'::$TSS0");
    o.end();
    o.block();
    o.local(names_b, "`opts'::`3'::names");
    o.local(guard_b, "`opts'::`3'::$TSS1");
    o.end();
    o.end();
    Built b = o.build(some_debug_t());
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, names_a).source == BindSource::CompilandProcedure);
    CHECK(decision(*r, names_a).old_address.rva == 0x3010);
    CHECK(decision(*r, names_b).old_address.rva == 0x3020);
    CHECK(decision(*r, guard_a).old_address.rva == 0x3018);
    CHECK(decision(*r, guard_b).old_address.rva == 0x3028);
    REQUIRE(r->warnings.size() == 1);
    CHECK(r->warnings[0].code == diag::kStaticsMatchedByOrder);
}

// ---- WP-40 (R-48 item 7) and F-41.1: Jolt's GetRTTIOfType overloads under cl --------------------------------------
//
// tests/corpus/repro/f41_1_f41_2_rtti_overloads.cpp: overloads of one free function, each with ONE guarded static `rtti`
// (cl records it and its guard `$TSS0` in the overload's procedure). The groups "rtti" and "$TSS0" have one member per
// overload and one record per overload, all with the same display name: pass A, the exact names and the layout (every
// static a COMDAT of its own under cl) leave them to the by-place step, which identifies each overload's OLD procedure by
// the address of its decorated public (unique per overload) and pairs the one static of the name there. main refused:
// the by-place step of the "$TSS0" group scanned for "the guard of the member's block" and found the member itself, a
// guard without a binding ("the guard variable ... $TSS0 ... of static 1 of 1 has no record in the old image", R-48 item
// 7, F-41.1's message), and every `rtti` failed with it. clang-cl records no guard: there it stays refused (CML3207).
namespace {
const char* const kRttiFn[4] = {"?GetRTTIOfType@@YAPEAURtti@@PEAUShapeSettings@@@Z", "?GetRTTIOfType@@YAPEAURtti@@PEAUSphereSettings@@@Z",
                                "?GetRTTIOfType@@YAPEAURtti@@PEAUBoxSettings@@@Z", "?GetRTTIOfType@@YAPEAURtti@@PEAUCapsuleSettings@@@Z"};
std::string rtti_of(int n) { return std::string("?rtti@?1?") + kRttiFn[n] + "@4URtti@@A"; }
std::string rtti_guard_of(int n) { return std::string("?$TSS0@?1?") + kRttiFn[n] + "@4HA"; }
}  // namespace

TEST_CASE("by place (R-48 item 7, F-41.1 under cl): each overload's guarded static and its guard pair with that overload's own records") {
    // The old image: Shape at text 0x40 (rtti 0x3010, guard 0x3018), Sphere at 0x60 (0x3020, 0x3028), Box at 0x80 (0x3030,
    // 0x3038). The patch defines them in another order (Box, Shape, Sphere): the order of the overloads plays no part.
    SynthWorld w;
    tp::Records& s = w.tu().symbols;
    for (int n = 0; n < 3; ++n) {
        const std::uint32_t at = 0x10 + 0x10 * static_cast<std::uint32_t>(n);
        s.proc(0x1110, "GetRTTIOfType", SynthWorld::kTextSec, 0x40 + 0x20 * static_cast<std::uint32_t>(n), 0x20);
        s.data(0x110C, "rtti", SynthWorld::kDataSec, at, tp::kTInt);
        s.data(0x110C, "$TSS0", SynthWorld::kDataSec, at + 8, tp::kTInt);
        s.end();
        w.pub_func(kRttiFn[n], 0x40 + 0x20 * static_cast<std::uint32_t>(n));
    }
    w.open();
    const auto overload = [](StaticsObj& o, int n) {
        o.proc(kRttiFn[n], "GetRTTIOfType");
        o.local(rtti_of(n), "rtti");
        o.local(rtti_guard_of(n), "$TSS0");
        o.end();
    };
    SUBCASE("the edit changed one overload's code: every static and guard keeps its own old object") {
        StaticsObj o;
        overload(o, 2);
        overload(o, 0);
        overload(o, 1);
        Built b = o.build(some_debug_t());
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        for (int n = 0; n < 3; ++n) {
            const std::uint32_t at = 0x3010 + 0x10 * static_cast<std::uint32_t>(n);
            CHECK(decision(*r, rtti_of(n)).decision == DecisionKind::BindOldData);
            CHECK(decision(*r, rtti_of(n)).source == BindSource::CompilandProcedure);
            CHECK(decision(*r, rtti_of(n)).old_address.rva == at);
            // The guard is bound to its OLD RECORD (not by a layout): the live object is never constructed again.
            CHECK(decision(*r, rtti_guard_of(n)).decision == DecisionKind::BindOldData);
            CHECK(decision(*r, rtti_guard_of(n)).source == BindSource::CompilandProcedure);
            CHECK(decision(*r, rtti_guard_of(n)).old_address.rva == at + 8);
        }
        // One static of the name per function: its function is its identity, no order is involved (no CML3570).
        for (const cml::Diag& warning : r->warnings) {
            CHECK(warning.code != diag::kStaticsMatchedByOrder);
        }
        CHECK(r->bitmask_guards.empty());
    }
    SUBCASE("a new overload: its static and guard are refused (no old function to take them from), never another overload's") {
        // (As on main: the group has more members than records, and the new overload has no old procedure.)
        StaticsObj o;
        overload(o, 3);
        overload(o, 0);
        overload(o, 1);
        overload(o, 2);
        Built b = o.build(some_debug_t());
        std::vector<cml::Diag> errors;
        BindPlan partial;
        REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
        REQUIRE_FALSE(errors.empty());
        CHECK(contains(errors[0].message, "the function is not matched to a function of this translation unit in the old image"));
        CHECK(decision(partial, rtti_of(3)).decision != DecisionKind::BindOldData);
        CHECK(decision(partial, rtti_guard_of(3)).decision != DecisionKind::BindOldData);
        for (int n = 0; n < 3; ++n) {
            const SymbolDecision st = decision(partial, rtti_of(n));
            CHECK((st.decision != DecisionKind::BindOldData || st.old_address.rva == 0x3010 + 0x10 * static_cast<std::uint32_t>(n)));
            const SymbolDecision g = decision(partial, rtti_guard_of(n));
            CHECK((g.decision != DecisionKind::BindOldData || g.old_address.rva == 0x3018 + 0x10 * static_cast<std::uint32_t>(n)));
        }
    }
}

// ---- WP-46 (F-41.1 under clang-cl, ADR-0043): the guards without a record ---------------------------------------------
//
// clang-cl records no thread-safe guard (`$TSS0`) at all. The by-place step refused every overload's `rtti` because its guard
// had no record (F-41.1's message under clang-cl). Now, in a compiland that records no `$TSS` guard, a static that is the only
// one of its name in its function pairs by that function, and its guard is left to settle_guards, which places it from the
// layout those statics teach (only settle_guards reads it) or refuses with CML3600. The shape is clang 18's (llvm-objdump of
// tests/corpus/repro/f41_1_f41_2_rtti_overloads.cpp, -O2): `rtti` and its guard next to each other in one `.bss`, not a COMDAT.
namespace {
// The clang-cl old image: three overloads, `rtti` at data 0x10 / 0x20 / 0x30, no guard record. `cl_guard_record`: another
// function of the compiland has a `$TSS0` record (the compiler records guards: cl).
void clang_rtti_world(SynthWorld& w, bool cl_guard_record = false) {
    tp::Records& s = w.tu().symbols;
    for (int n = 0; n < 3; ++n) {
        s.proc(0x1110, "GetRTTIOfType", SynthWorld::kTextSec, 0x40 + 0x20 * static_cast<std::uint32_t>(n), 0x20);
        s.data(0x110C, "rtti", SynthWorld::kDataSec, 0x10 + 0x10 * static_cast<std::uint32_t>(n), tp::kTInt);
        s.end();
        w.pub_func(kRttiFn[n], 0x40 + 0x20 * static_cast<std::uint32_t>(n));
    }
    if (cl_guard_record) {
        s.proc(0x1110, "other", SynthWorld::kTextSec, 0xA0, 0x10);
        s.data(0x110C, "cache", SynthWorld::kDataSec, 0x50, tp::kTInt);
        s.data(0x110C, "$TSS0", SynthWorld::kDataSec, 0x58, tp::kTInt);
        s.end();
        w.pub_func("?other@@YAHXZ", 0xA0);
    }
    w.open();
}
// The patch: the overloads in another order (2, 0, 1) in .debug$S; `rtti` n at .bss 0x10 + 0x10 n + shift[n], its guard 8
// bytes after it. `comdat`: every static and guard in a COMDAT section of its own instead (nothing teaches a layout).
Built clang_rtti_object(const std::array<std::uint32_t, 3>& shift, bool comdat = false) {
    StaticsObj o;
    const std::uint32_t bss = comdat ? 0 : o.obj.section(".bss", kData, 0x80);
    std::array<std::uint32_t, 3> rtti{};
    for (int n = 0; n < 3; ++n) {
        const std::uint32_t at = 0x10 + 0x10 * static_cast<std::uint32_t>(n) + shift[static_cast<std::size_t>(n)];
        if (comdat) {
            rtti[static_cast<std::size_t>(n)] = o.symbol(rtti_of(n));
            o.symbol(rtti_guard_of(n));
        } else {
            rtti[static_cast<std::size_t>(n)] = o.obj.symbol(rtti_of(n), at, static_cast<std::int32_t>(bss), false, false);
            o.obj.symbol(rtti_guard_of(n), at + 8, static_cast<std::int32_t>(bss), false, false);
        }
    }
    for (const int n : {2, 0, 1}) {
        o.proc(kRttiFn[n], "GetRTTIOfType");
        o.record(rtti[static_cast<std::size_t>(n)], "rtti");
        o.end();
    }
    return o.build(some_debug_t());
}
}  // namespace

TEST_CASE("by place, F-41.1 under clang-cl (WP-46): each overload's `rtti` binds to its own overload's record, its guard to the old guard next to it") {
    SUBCASE("one .bss: paired by their functions, the guards placed from the layout the statics teach") {
        // main: CML3207 for every `rtti` ("the guard variable ... of static 1 of 1 has no record in the old image").
        SynthWorld w;
        clang_rtti_world(w);
        Built b = clang_rtti_object({0, 0, 0});
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        for (int n = 0; n < 3; ++n) {
            CAPTURE(n);
            const std::uint32_t at = 0x3010 + 0x10 * static_cast<std::uint32_t>(n);
            const SymbolDecision st = decision(*r, rtti_of(n));
            CHECK(st.decision == DecisionKind::BindOldData);
            CHECK(st.source == BindSource::CompilandProcedure);
            CHECK(st.old_address.rva == at);
            const SymbolDecision g = decision(*r, rtti_guard_of(n));
            CHECK(g.decision == DecisionKind::BindOldData);
            CHECK(g.source == BindSource::CompilandLayout);
            CHECK(g.old_address.rva == at + 8);
        }
        for (const cml::Diag& warning : r->warnings) {
            CHECK(warning.code != diag::kStaticsMatchedByOrder);
            CHECK(warning.code != diag::kGuardUnmatched);
        }
    }
    SUBCASE("every static a COMDAT of its own: no layout, the guards are refused (CML3600), never fresh, never bound") {
        SynthWorld w;
        clang_rtti_world(w);
        Built b = clang_rtti_object({0, 0, 0}, true);
        std::vector<cml::Diag> errors;
        BindPlan partial;
        REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
        REQUIRE(errors.size() == 3);
        for (const cml::Diag& e : errors) {
            CHECK(e.code == diag::kGuardMismatch);
            CHECK_MESSAGE(contains(e.message, "no layout of its object section is known"), e.message);
        }
        for (int n = 0; n < 3; ++n) {
            CHECK(decision(partial, rtti_guard_of(n)).decision != DecisionKind::BindOldData);
        }
        for (const cml::Diag& warning : partial.warnings) {
            CHECK(warning.code != diag::kGuardUnmatched);
        }
    }
    SUBCASE("the statics disagree on the layout (one moved in the section): no layout, CML3600, no guard bound") {
        SynthWorld w;
        clang_rtti_world(w);
        Built b = clang_rtti_object({0, 0, 0x40});
        std::vector<cml::Diag> errors;
        BindPlan partial;
        REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
        REQUIRE(errors.size() == 3);
        for (const cml::Diag& e : errors) {
            CHECK(e.code == diag::kGuardMismatch);
        }
        for (int n = 0; n < 3; ++n) {
            CHECK(decision(partial, rtti_guard_of(n)).decision != DecisionKind::BindOldData);
        }
    }
    SUBCASE("a compiland that records `$TSS` guards (cl): a guard without a record still refuses its static (CML3207), as before") {
        SynthWorld w;
        clang_rtti_world(w, true);
        Built b = clang_rtti_object({0, 0, 0});
        std::vector<cml::Diag> errors;
        BindPlan partial;
        REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
        REQUIRE_FALSE(errors.empty());
        for (const cml::Diag& e : errors) {
            CHECK(e.code == diag::kAmbiguousStatic);
        }
        CHECK(contains(errors[0].message, "has no record in the old image"));
        for (int n = 0; n < 3; ++n) {
            CHECK(decision(partial, rtti_of(n)).decision != DecisionKind::BindOldData);
            CHECK(decision(partial, rtti_guard_of(n)).decision != DecisionKind::BindOldData);
        }
    }
}

// ---- WP-40, rule (d): the planner's side (the sets it reports, and the pairing across numbers it is given) -----------

TEST_CASE("lambda statics (rule (d)): lambdas with the same call operator are reported for the comparison of their code") {
    SynthWorld w;
    Types old_types;
    old_proc(w, "`f'::`2'::<lambda_1>::operator()", 0x60, call_operator_with(old_types, 1, {}), {{"sname", 0x20}, {"$TSS0", 0x28}});
    old_proc(w, "`f'::`2'::<lambda_2>::operator()", 0x70, call_operator_with(old_types, 2, {}), {{"sname", 0x30}, {"$TSS0", 0x38}});
    old_types.set.add_to(w.spec.tpi);
    w.open();
    Types t;
    const std::uint32_t op1 = call_operator_with(t, 1, {});
    const std::uint32_t op2 = call_operator_with(t, 2, {});
    StaticsObj o;
    o.proc(call_operator_symbol(1), "`f'::`2'::<lambda_1>::operator()", false, op1);
    o.end();
    o.proc(call_operator_symbol(2), "`f'::`2'::<lambda_2>::operator()", false, op2);
    o.end();
    for (int n = 1; n <= 2; ++n) {
        o.symbol(static_in_lambda("sname", n));
        o.symbol(static_in_lambda("$TSS0", n));
    }
    Built b = o.build(t.set.debug_t());
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    REQUIRE(r->lambda_sets.size() == 1);
    const LambdaSet& set = r->lambda_sets.front();
    REQUIRE(set.lambdas.size() == 2);
    CHECK(set.lambdas[0].closure == "<lambda_1>@?1??f@@YAXXZ@");
    CHECK(set.lambdas[0].old_closure == "<lambda_1>@?1??f@@YAXXZ@");
    CHECK(set.lambdas[0].call_operator == call_operator_symbol(1));
    CHECK(set.lambdas[1].call_operator == call_operator_symbol(2));
    CHECK(set.lambdas[0].statics.size() == 2);  // sname and its guard (cl records both)
    // Lambdas with different call operators are not reported (their types tell them apart).
    CHECK(decision(*r, static_in_lambda("sname", 2)).old_address.rva == 0x3030);
}

TEST_CASE("lambda statics (rule (d)): given a pairing across the numbers, the statics, guards and call operators follow it") {
    SUBCASE("cl: guards with records") {
        SynthWorld w;
        Types old_types;
        old_proc(w, "`f'::`2'::<lambda_1>::operator()", 0x60, call_operator_with(old_types, 1, {}), {{"sname", 0x20}, {"$TSS0", 0x28}});
        old_proc(w, "`f'::`2'::<lambda_2>::operator()", 0x70, call_operator_with(old_types, 2, {}), {{"sname", 0x30}, {"$TSS0", 0x38}});
        old_types.set.add_to(w.spec.tpi);
        w.open();
        Types t;
        const std::uint32_t op1 = call_operator_with(t, 1, {});
        const std::uint32_t op2 = call_operator_with(t, 2, {});
        StaticsObj o;
        o.proc(call_operator_symbol(1), "`f'::`2'::<lambda_1>::operator()", false, op1);
        o.end();
        o.proc(call_operator_symbol(2), "`f'::`2'::<lambda_2>::operator()", false, op2);
        o.end();
        for (int n = 1; n <= 2; ++n) {
            o.symbol(static_in_lambda("sname", n));
            o.symbol(static_in_lambda("$TSS0", n));
        }
        Built b = o.build(t.set.debug_t());
        PlanInput in = w.input(b);
        in.closure_pairing = {{"<lambda_1>@?1??f@@YAXXZ@", "<lambda_2>@?1??f@@YAXXZ@"}, {"<lambda_2>@?1??f@@YAXXZ@", "<lambda_1>@?1??f@@YAXXZ@"}};
        auto r = plan_bind(in);
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, static_in_lambda("sname", 1)).old_address.rva == 0x3030);
        CHECK(decision(*r, static_in_lambda("$TSS0", 1)).old_address.rva == 0x3038);
        CHECK(decision(*r, static_in_lambda("sname", 2)).old_address.rva == 0x3020);
        CHECK(decision(*r, static_in_lambda("$TSS0", 2)).old_address.rva == 0x3028);
        CHECK(decision(*r, call_operator_symbol(1)).old_address.rva == 0x1070);
        CHECK(decision(*r, call_operator_symbol(2)).old_address.rva == 0x1060);
    }
    SUBCASE("clang-cl: no guard records; the guards go where the layout puts the guard of the other number") {
        // Old .bss: s_plain 0x08, lambda 0's sname 0x20 + guard 0x28, lambda 1's 0x30 + 0x38. In the patch the lambdas
        // changed places (the code said so): lambda 0's statics are the old lambda 1's.
        SynthWorld w;
        w.data(w.tu(), "`anonymous namespace'::s_plain", 0x08);
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
        o.symbol("?s_plain@?A0x1@@3HA", 0x08, static_cast<std::int32_t>(d), false, false);
        for (int n = 0; n < 2; ++n) {
            o.symbol(sym("sname", n), 0x20 + 0x10 * static_cast<std::uint32_t>(n), static_cast<std::int32_t>(d), false, false);
            o.symbol(sym("$TSS0", n), 0x28 + 0x10 * static_cast<std::uint32_t>(n), static_cast<std::int32_t>(d), false, false);
        }
        Built b = o.build();
        PlanInput in = w.input(b);
        in.closure_pairing = {{"<lambda_0>@?0??f@@YAXXZ@", "<lambda_1>@?0??f@@YAXXZ@"}, {"<lambda_1>@?0??f@@YAXXZ@", "<lambda_0>@?0??f@@YAXXZ@"}};
        auto r = plan_bind(in);
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, sym("sname", 0)).old_address.rva == 0x3030);
        CHECK(decision(*r, sym("$TSS0", 0)).old_address.rva == 0x3038);
        CHECK(decision(*r, sym("$TSS0", 0)).source == BindSource::CompilandLayout);
        CHECK(decision(*r, sym("sname", 1)).old_address.rva == 0x3020);
        CHECK(decision(*r, sym("$TSS0", 1)).old_address.rva == 0x3028);
        CHECK(r->warnings.empty());
    }
}

TEST_CASE("inlined lambdas under cl /O2 /Ob2: a static recorded both in its operator() and in the caller is one variable") {
    // CI (cl 19.44, fixture planner_pairing_o2, head 020866a): cl emits the call operators AND inlines them, and records each
    // lambda's statics in both procedures at the same address. main of WP-40's first push counted them twice ("6 there,
    // some of them inlined, 3 in the patch") and refused the reload; they pair by closure.
    LambdaWorld lw;
    lw.lambda(1, 0x60, 0x20);
    lw.lambda(2, 0x70, 0x30);
    old_proc(lw.w, "f", 0x40, lw.types.fn(tp::kTVoid, {}), {{"sname", 0x20}, {"$TSS0", 0x28}, {"sname", 0x30}, {"$TSS0", 0x38}});
    lw.open();
    SynthObj o;
    for (int n = 1; n <= 2; ++n) {
        for (const char* name : {"sname", "$TSS0"}) {
            const std::uint32_t sec = o.section(".bss", kDataComdat, 8, coff::ComdatSelection::Any);
            o.symbol(static_in_lambda(name, n), 0, static_cast<std::int32_t>(sec), false, false);
        }
    }
    Built b = o.build();
    auto r = plan_bind(lw.w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, static_in_lambda("sname", 1)).source == BindSource::CompilandClosure);
    CHECK(decision(*r, static_in_lambda("sname", 1)).old_address.rva == 0x3020);
    CHECK(decision(*r, static_in_lambda("$TSS0", 1)).old_address.rva == 0x3028);
    CHECK(decision(*r, static_in_lambda("sname", 2)).old_address.rva == 0x3030);
    CHECK(decision(*r, static_in_lambda("$TSS0", 2)).old_address.rva == 0x3038);
}

TEST_CASE("inlined lambdas under cl /O2 /Ob2: a caller's record at ANOTHER address than the operator's is not the same variable") {
    // Only an equal RVA makes the caller's record the operator's static; otherwise it counts as an inlined lambda's static,
    // the counts disagree (4 old, 2 in the patch) and the statics are refused, as before.
    LambdaWorld lw;
    lw.lambda(1, 0x60, 0x20);
    lw.lambda(2, 0x70, 0x30);
    old_proc(lw.w, "f", 0x40, lw.types.fn(tp::kTVoid, {}), {{"sname", 0x40}, {"$TSS0", 0x48}, {"sname", 0x50}, {"$TSS0", 0x58}});
    lw.open();
    SynthObj o;
    for (int n = 1; n <= 2; ++n) {
        for (const char* name : {"sname", "$TSS0"}) {
            const std::uint32_t sec = o.section(".bss", kDataComdat, 8, coff::ComdatSelection::Any);
            o.symbol(static_in_lambda(name, n), 0, static_cast<std::int32_t>(sec), false, false);
        }
    }
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    REQUIRE_FALSE(plan_bind(lw.w.input(b), &errors, &partial).has_value());
    REQUIRE_FALSE(errors.empty());
    for (const cml::Diag& e : errors) {
        CHECK(e.id() == "CML3207");
        CHECK(contains(e.message, "are not the same as in the old image (4 there, some of them inlined, 2 in the patch)"));
    }
}

TEST_CASE("lambda statics (rule (d), review): lambdas in an anonymous namespace are reported, and a pairing across numbers is followed") {
    // The decorated names carry "?A0x<hash>@" (another hash than the old build's), the closures are named without it.
    const std::string old_fn = "?0??f@?A0x520C9D4F@@YAHXZ@";
    const std::string new_fn = "?0??f@?A0x11111111@@YAHXZ@";
    const std::string plain_fn = "?0??f@?A0x@@YAHXZ@";
    const auto sym = [&new_fn](const std::string& name, int n) { return "?" + name + "@?1???R<lambda_" + std::to_string(n) + ">@" + new_fn + "QEBA@XZ@4HA"; };
    const auto op = [&new_fn](int n) { return "??R<lambda_" + std::to_string(n) + ">@" + new_fn + "QEBA@XZ"; };
    const auto old_world = [&](SynthWorld& w) {
        w.data(w.tu(), "`anonymous namespace'::s_plain", 0x08);
        Types types;
        for (int n = 0; n < 2; ++n) {
            const std::uint32_t type = types.call_operator("<lambda_" + std::to_string(n) + ">", ".?AV<lambda_" + std::to_string(n) + ">@" + old_fn);
            old_proc(w, "`anonymous namespace'::f::<lambda_" + std::to_string(n) + ">::operator()", 0x60 + 0x10 * static_cast<std::uint32_t>(n), type,
                     {{"sname", 0x20 + 0x10 * static_cast<std::uint32_t>(n)}});
        }
        types.set.add_to(w.spec.tpi);
        w.open();
    };
    const auto object = [&](StaticsObj& o, Types& t) {
        for (int n = 0; n < 2; ++n) {
            o.proc(op(n), "`anonymous namespace'::f::<lambda_" + std::to_string(n) + ">::operator()", false,
                   t.call_operator("<lambda_" + std::to_string(n) + ">", ".?AV<lambda_" + std::to_string(n) + ">@" + new_fn));
            o.end();
        }
        const std::uint32_t d = o.obj.section(".bss", kData, 0x40);
        o.obj.symbol("?s_plain@?A0x11111111@@3HA", 0x08, static_cast<std::int32_t>(d), false, false);
        for (int n = 0; n < 2; ++n) {
            o.obj.symbol(sym("sname", n), 0x20 + 0x10 * static_cast<std::uint32_t>(n), static_cast<std::int32_t>(d), false, false);
            o.obj.symbol(sym("$TSS0", n), 0x28 + 0x10 * static_cast<std::uint32_t>(n), static_cast<std::int32_t>(d), false, false);
        }
    };
    SUBCASE("reported for the comparison of their code, with their call operators") {
        SynthWorld w;
        old_world(w);
        Types t;
        StaticsObj o;
        object(o, t);
        Built b = o.build(t.set.debug_t());
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        REQUIRE(r->lambda_sets.size() == 1);
        REQUIRE(r->lambda_sets[0].lambdas.size() == 2);
        CHECK(r->lambda_sets[0].lambdas[0].closure == "<lambda_0>@" + plain_fn);
        CHECK(r->lambda_sets[0].lambdas[0].call_operator == op(0));  // (until the review: empty, so the set was never compared)
        CHECK(r->lambda_sets[0].lambdas[1].call_operator == op(1));
    }
    SUBCASE("given the swap, the statics, the call operators and the guards (by their twins' places) follow it") {
        SynthWorld w;
        old_world(w);
        Types t;
        StaticsObj o;
        object(o, t);
        Built b = o.build(t.set.debug_t());
        PlanInput in = w.input(b);
        in.closure_pairing = {{"<lambda_0>@" + plain_fn, "<lambda_1>@" + plain_fn}, {"<lambda_1>@" + plain_fn, "<lambda_0>@" + plain_fn}};
        auto r = plan_bind(in);
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, sym("sname", 0)).old_address.rva == 0x3030);
        CHECK(decision(*r, sym("$TSS0", 0)).old_address.rva == 0x3038);
        CHECK(decision(*r, sym("sname", 1)).old_address.rva == 0x3020);
        CHECK(decision(*r, sym("$TSS0", 1)).old_address.rva == 0x3028);
        CHECK(decision(*r, op(0)).old_address.rva == 0x1070);
        CHECK(decision(*r, op(1)).old_address.rva == 0x1060);
    }
}

// ---- WP-40 review: two statics of the patch on one live object ---------------------------------------------------------

TEST_CASE("cl /O2: a new `f::n` and the lambda's `n` recorded twice at one address are never both bound to it") {
    // cl records the lambda's static `n` in its operator() and in f() at one RVA. The patch adds f's own `n`. Until the
    // review the duplicate check keyed the old records by their record, not by the address, and both statics were bound to
    // the one live object.
    SynthWorld w;
    Types types;
    const std::uint32_t op = types.call_operator("f::<lambda_1>", ".?AV<lambda_1>@?1??f@@YAXXZ@");
    old_proc(w, "`f'::`2'::<lambda_1>::operator()", 0x60, op, {{"n", 0x20}, {"$TSS0", 0x28}});
    old_proc(w, "f", 0x40, types.fn(tp::kTVoid, {}), {{"n", 0x20}, {"$TSS0", 0x28}});
    w.pub_func("?f@@YAXXZ", 0x40);
    types.set.add_to(w.spec.tpi);
    w.open();
    SynthObj o;
    for (const char* const name : {"?n@?1???R<lambda_1>@?1??f@@YAXXZ@QEBAAEBUName@@XZ@4U2@A", "?$TSS0@?1???R<lambda_1>@?1??f@@YAXXZ@QEBAAEBUName@@XZ@4HA",
                                    "?n@?1??f@@YAXXZ@4HA"}) {
        const std::uint32_t sec = o.section(".bss", kDataComdat, 8, coff::ComdatSelection::Any);
        o.symbol(name, 0, static_cast<std::int32_t>(sec), false, false);
    }
    Built b = o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
    std::size_t at_0x3020 = 0;
    for (const SymbolDecision& d : partial.decisions) {
        at_0x3020 += d.decision == DecisionKind::BindOldData && d.old_address.rva == 0x3020 ? 1u : 0u;
    }
    CHECK(at_0x3020 <= 1);
    CHECK_FALSE(errors.empty());
}

// ---- WP-46 (F-41.3, ADR-0043): the destructor of a function-local class under clang-cl -------------------------------
//
// tests/corpus/repro/f41_3_local_class_destructor.cpp, built with clang 18 (-O2, llvm-pdbutil): the old compiland has two
// S_LPROC32 `System::Process::LocalCollector::~LocalCollector` with the same type, the destructor `??1` and its scalar
// deleting destructor `??_G` (the lld map gives their addresses); only the second has the implicit parameter
// `should_call_delete`. The patch's `??1` matched both (CML3214, "1 new and 2 old functions have the same signature").
namespace {
const char* const kLocalDtor = "??1LocalCollector@?1??Process@System@@QEAAXH@Z@UEAA@XZ";
const char* const kLocalDtorDisplay = "System::Process::LocalCollector::~LocalCollector";

void s_local(tp::Records& s, const std::string& name, std::uint32_t type) {
    tp::Bytes b;
    tp::put32(b, type);
    tp::put16(b, 1);  // CV_LVARFLAGS: fIsParam
    tp::putstr(b, name);
    s.add(0x113E, b);  // S_LOCAL
}
// The old destructor at text 0x60 and its deleting destructor at `deleting_at`, both under the destructor's name and type.
// `record_flag`: the deleting destructor's `should_call_delete` is recorded.
void old_destructors(SynthWorld& w, std::uint32_t deleting_at, bool record_flag = true) {
    Types old_types;
    const std::uint32_t type = old_types.fn(tp::kTVoid, {});
    tp::Records& s = w.tu().symbols;
    const auto destructor = [&](std::uint32_t at, bool deleting) {
        s.proc(0x110F, kLocalDtorDisplay, SynthWorld::kTextSec, at, 0x10, 0, type);
        s_local(s, "this", 0x0603);
        if (deleting && record_flag) {
            s_local(s, "should_call_delete", tp::kTInt);
        }
        s.end();
    };
    if (deleting_at < 0x60) {
        destructor(deleting_at, true);
        destructor(0x60, false);
    } else {
        destructor(0x60, false);
        destructor(deleting_at, true);
    }
    old_types.set.add_to(w.spec.tpi);
    w.open();
}
Built local_destructor_object() {
    Types t;
    const std::uint32_t id = t.func_id(t.fn(tp::kTVoid, {}), "~LocalCollector");
    TypedObj o;
    o.function(kLocalDtor, id);
    o.obj.function("??_GLocalCollector@?1??Process@System@@QEAAXH@Z@UEAAPEAXI@Z", false);  // the patch's deleting destructor
    return o.build(t.set.debug_t());
}
}  // namespace

TEST_CASE("destructor of a local class (F-41.3, WP-46): the deleting destructor's record is not a candidate, the destructor is hooked to its own") {
    for (const std::uint32_t deleting_at : {0x40u, 0x80u}) {
        CAPTURE(deleting_at);
        // main: CML3214 "1 new and 2 old functions have the same signature", in either order.
        SynthWorld w;
        old_destructors(w, deleting_at);
        Built b = local_destructor_object();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        const SymbolDecision d = decision(*r, kLocalDtor);
        CHECK(d.decision == DecisionKind::PatchHooked);
        CHECK((d.source == BindSource::CompilandStatic || d.source == BindSource::CompilandSignature));  // (since the review: by name in pass A, the one record left)
        CHECK(d.old_address.rva == 0x1060);
        for (const HookEntry& h : r->hooks) {
            CHECK(h.old_va != SynthWorld::kBase + 0x1000 + deleting_at);  // nothing is hooked onto the old deleting destructor
        }
    }
}

TEST_CASE("destructor of a local class (F-41.3, WP-46): without the recorded parameter the two records stay ambiguous (CML3214)") {
    SynthWorld w;
    old_destructors(w, 0x80, false);
    Built b = local_destructor_object();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kAmbiguousOverloads);
    CHECK(contains(errors[0].message, "1 new and 2 old functions have the same signature"));
    CHECK(partial.hooks.empty());
}

// ---- WP-46 (F-41.2, ADR-0043 ruling D1): lambdas cl names by a hash ------------------------------------------------
//
// cl's legacy lambda processor (Jolt: C++17 without /permissive-) names every closure `<lambda_<32 hex>>`, in no scope
// (`??R<lambda_0b7c...>@@QEBA@XZ`): the closures of the whole translation unit are one set. The corpus (cl, unity batch 12)
// had 39 static call operators in the patch and 90 records named `operator()` in the old compiland: the other 51 had external
// linkage (lambdas of inline functions whose COMDAT the linker took from this unit) and were paired by their public name, but
// were counted in the set, so the sets never agreed (CML3214).
namespace {
const std::string kHash1 = "<lambda_0b7c44cbca6a0337bec371af34c9af94>";
const std::string kHash2 = "<lambda_056dc9763d275277fba2e4fa806073e2>";
const std::string kHash3 = "<lambda_24d82191404f736f33e380197c42a713>";
std::string hashed_op(const std::string& closure) { return "??R" + closure + "@@QEBAHXZ"; }
std::string hashed_unique(const std::string& closure) { return ".?AV" + closure + "@@"; }

// The old compiland: static call operators of kHash1 (text 0x60) and kHash3 (0x80), and the call operator of kHash2 (0x70),
// which has external linkage (a public at its address).
void hashed_world(SynthWorld& w) {
    Types old_types;
    old_proc(w, kHash1 + "::operator()", 0x60, old_types.call_operator(kHash1, hashed_unique(kHash1)));
    old_proc(w, kHash2 + "::operator()", 0x70, old_types.call_operator(kHash2, hashed_unique(kHash2)));
    old_proc(w, kHash3 + "::operator()", 0x80, old_types.call_operator(kHash3, hashed_unique(kHash3)));
    w.pub_func(hashed_op(kHash2), 0x70);
    old_types.set.add_to(w.spec.tpi);
    w.open();
}
}  // namespace

TEST_CASE("hash-named lambdas (F-41.2, WP-46): an old call operator an external definition of the patch replaces is not in the set") {
    SUBCASE("the patch defines kHash2 with external linkage: the static ones pair by their names, and are listed for the proof by code") {
        // main: CML3214 for both static call operators ("the lambdas of the enclosing function are not the same as in the old
        // image (3 call operator(s) there, 2 in the patch)").
        SynthWorld w;
        hashed_world(w);
        Types t;
        TypedObj o;
        o.function(hashed_op(kHash3), t.call_operator(kHash3, hashed_unique(kHash3)), false);
        o.function(hashed_op(kHash1), t.call_operator(kHash1, hashed_unique(kHash1)), false);
        o.obj.function(hashed_op(kHash2), true);
        Built b = o.build(t.set.debug_t());
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        const SymbolDecision d1 = decision(*r, hashed_op(kHash1));
        CHECK(d1.decision == DecisionKind::PatchHooked);
        CHECK(d1.source == BindSource::CompilandSignature);
        CHECK(d1.old_address.rva == 0x1060);
        const SymbolDecision d3 = decision(*r, hashed_op(kHash3));
        CHECK(d3.decision == DecisionKind::PatchHooked);
        CHECK(d3.old_address.rva == 0x1080);
        const SymbolDecision d2 = decision(*r, hashed_op(kHash2));
        CHECK(d2.decision == DecisionKind::PatchHooked);
        CHECK(d2.source == BindSource::PdbPublic);
        CHECK(d2.old_address.rva == 0x1070);
        // Only the pairings made by the hash name are left to the proof (the external one pairs by its public name).
        REQUIRE(r->hashed_lambdas.size() == 2);
        for (const HashedLambda& h : r->hashed_lambdas) {
            CHECK((h.closure == kHash1 || h.closure == kHash3));
            REQUIRE(h.functions.size() == 1);
            CHECK(h.functions[0] == hashed_op(h.closure));
        }
    }
    SUBCASE("the patch no longer defines the external kHash2 (edited and renamed, or gone; it has no statics): the static ones still pair") {
        // (Round 2: an external lambda of a hash-named set never takes part in the set of static call operators; an edited one
        // is renamed, so its old function is not defined by the patch at all. It has no statics here, so nothing can be lost.)
        SynthWorld w;
        hashed_world(w);
        Types t;
        TypedObj o;
        o.function(hashed_op(kHash1), t.call_operator(kHash1, hashed_unique(kHash1)), false);
        o.function(hashed_op(kHash3), t.call_operator(kHash3, hashed_unique(kHash3)), false);
        Built b = o.build(t.set.debug_t());
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, hashed_op(kHash1)).old_address.rva == 0x1060);
        CHECK(decision(*r, hashed_op(kHash3)).old_address.rva == 0x1080);
    }
    SUBCASE("a lambda added: the set changed (CML3214), also where every old name is still there") {
        SynthWorld w;
        hashed_world(w);
        const std::string added = "<lambda_ffffffffffffffffffffffffffffffff>";
        Types t;
        TypedObj o;
        o.function(hashed_op(kHash1), t.call_operator(kHash1, hashed_unique(kHash1)), false);
        o.function(hashed_op(kHash3), t.call_operator(kHash3, hashed_unique(kHash3)), false);
        o.function(hashed_op(added), t.call_operator(added, hashed_unique(added)), false);
        o.obj.function(hashed_op(kHash2), true);
        Built b = o.build(t.set.debug_t());
        std::vector<cml::Diag> errors;
        BindPlan partial;
        REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
        REQUIRE(errors.size() == 4);
        // (Round 2: first the leftover of the name matching, a lambda on one side only, then the set rule for each.)
        CHECK(errors[0].code == diag::kHashedLambdasUnproven);
        CHECK(contains(errors[0].message, "1 lambda(s) of the patch (" + added + ") and 0 of the old image (none) are left"));
        for (std::size_t k = 1; k < errors.size(); ++k) {
            CHECK(errors[k].code == diag::kAmbiguousOverloads);
        }
        CHECK(decision(partial, hashed_op(kHash1)).decision != DecisionKind::PatchHooked);
    }
}

TEST_CASE("hash-named lambdas (F-41.2, WP-46): a static paired by the hash name with no hooked function of its lambda is refused (CML3630)") {
    // The patch's lambda has no call operator of its own (the compiler inlined it), so the static pairs with the old static of
    // the same closure by the hash name alone, and no function of the lambda is hooked to an old one whose code could prove
    // that it is the same lambda. main: bound to the old static without any proof.
    SynthWorld w;
    Types old_types;
    old_proc(w, kHash1 + "::operator()", 0x60, old_types.call_operator(kHash1, hashed_unique(kHash1)), {{"sname", 0x20}});
    old_types.set.add_to(w.spec.tpi);
    w.open();
    StaticsObj o;
    const std::string sname = "?sname@?1???R" + kHash1 + "@@QEBAHXZ@4HA";
    o.symbol(sname);
    Built b = o.build(some_debug_t());
    std::vector<cml::Diag> errors;
    BindPlan partial;
    REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kHashedLambdasUnproven);
    CHECK(contains(errors[0].message, "belongs to lambda '" + kHash1 + "', which the compiler names by a hash"));
    CHECK(decision(partial, sname).decision != DecisionKind::BindOldData);
    CHECK(partial.hashed_lambdas.empty());
}

// ---- WP-46 round 2: cl's hash depends on the lambda's own code (CI, fixture legacy_lambdas: editing a lambda renamed its
// closure, `WP46-HASH-SET ... old=3 new=3 common=2`). The one lambda left on each side after the names are matched is paired
// by elimination (the pipeline then requires every name-matched lambda to be unchanged); anything else is refused (CML3630).
namespace {
const std::string kEdited1 = "<lambda_e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1>";
const std::string kEdited3 = "<lambda_e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3e3>";
std::string hashed_static(const std::string& closure) { return "?c@?1???R" + closure + "@@QEBAHXZ@4HA"; }
std::string hashed_guard(const std::string& closure) { return "?$TSS0@?1???R" + closure + "@@QEBAHXZ@4HA"; }

// The old compiland (cl): static call operators of kHash1 (text 0x60, `c` at data 0x20, its guard 0x28) and kHash3 (0x80,
// 0x30, 0x38); with `external`, also kHash2's call operator (0x70, external: a public) with `c` at 0x40 and its guard 0x48.
void hashed_statics_world(SynthWorld& w, bool external = false) {
    Types old_types;
    old_proc(w, kHash1 + "::operator()", 0x60, old_types.call_operator(kHash1, hashed_unique(kHash1)), {{"c", 0x20}, {"$TSS0", 0x28}});
    old_proc(w, kHash3 + "::operator()", 0x80, old_types.call_operator(kHash3, hashed_unique(kHash3)), {{"c", 0x30}, {"$TSS0", 0x38}});
    if (external) {
        old_proc(w, kHash2 + "::operator()", 0x70, old_types.call_operator(kHash2, hashed_unique(kHash2)), {{"c", 0x40}, {"$TSS0", 0x48}});
        w.pub_func(hashed_op(kHash2), 0x70);
    }
    old_types.set.add_to(w.spec.tpi);
    w.open();
}
// The patch: these static lambdas (call operator, `c` and its guard, each a COMDAT as cl emits them); `external_closure`:
// one more, with external linkage.
Built hashed_statics_object(const std::vector<std::string>& closures, const std::string& external_closure = {}) {
    Types t;
    TypedObj o;
    const auto lambda = [&](const std::string& closure, bool external) {
        if (external) {
            o.obj.function(hashed_op(closure), true);
        } else {
            o.function(hashed_op(closure), t.call_operator(closure, hashed_unique(closure)), false);
        }
        for (const std::string& name : {hashed_static(closure), hashed_guard(closure)}) {
            const std::uint32_t sec = o.obj.section(".bss", kDataComdat, 8, coff::ComdatSelection::Any);
            o.obj.symbol(name, 0, static_cast<std::int32_t>(sec), false, false);
        }
    };
    for (const std::string& closure : closures) {
        lambda(closure, false);
    }
    if (!external_closure.empty()) {
        lambda(external_closure, true);
    }
    return o.build(t.set.debug_t());
}
}  // namespace

// WP-46 review (high, item 1): being the only lambda left on each side is no evidence that two lambdas are one. The
// elimination needs the same enclosing function (the one procedure with a local of the closure's type, on both sides), the
// same closure layout (its captures), the same statics (names and types) and the same call operator signature.
namespace {
struct ClosureTypes {
    std::uint32_t forward = 0;
    std::uint32_t op = 0;
};
// A closure class capturing one member of `member` (forward reference, field list, definition) and its `int () const` call
// operator.
ClosureTypes closure_types(Types& t, const std::string& closure, std::uint32_t member, std::uint16_t size = 4) {
    ClosureTypes c;
    const std::string unique = hashed_unique(closure);
    c.forward = t.set.add(tp::kLfClass, tp::lf_udt(closure, unique, true));
    tp::Bytes fields;
    tp::put16(fields, 0x150D);  // LF_MEMBER
    tp::put16(fields, 3);       // public
    tp::put32(fields, member);
    tp::put16(fields, 0);       // offset 0
    tp::putstr(fields, "base");
    while (fields.size() % 4 != 0) {
        fields.push_back(static_cast<std::uint8_t>(0xF0 + (4 - fields.size() % 4)));
    }
    const std::uint32_t list = t.set.add(0x1203, fields);  // LF_FIELDLIST
    tp::Bytes def;
    tp::put16(def, 1);
    tp::put16(def, 0x200);
    tp::put32(def, list);
    tp::put32(def, 0);
    tp::put32(def, 0);
    tp::put16(def, size);
    tp::putstr(def, closure);
    tp::putstr(def, unique);
    t.set.add(tp::kLfClass, def);
    const std::uint32_t const_cls = t.set.add(tp::kLfModifier, tp::lf_modifier(c.forward, 1));
    const std::uint32_t this_ptr = t.set.add(tp::kLfPointer, tp::lf_pointer(const_cls, tp::kPtr64 | 0x400));
    const std::uint32_t args = t.args({});
    c.op = t.set.add(tp::kLfMFunction, tp::lf_mfunction(tp::kTInt, c.forward, this_ptr, args, 0));
    return c;
}

// The old compiland (cl): kHash1's call operator (text 0x60; `c` 0x20, `$TSS0` 0x28) held by ll_a (0xA0), kHash3's (0x80;
// 0x30, 0x38) held by ll_b (0xC0); ll_g (0xE0) holds none. Each closure captures one int.
void evidence_world(SynthWorld& w) {
    Types t;
    const std::uint32_t holder = t.fn(tp::kTInt, {});
    const ClosureTypes c1 = closure_types(t, kHash1, tp::kTInt);
    const ClosureTypes c3 = closure_types(t, kHash3, tp::kTInt);
    old_proc(w, kHash1 + "::operator()", 0x60, c1.op, {{"c", 0x20}, {"$TSS0", 0x28}});
    old_proc(w, kHash3 + "::operator()", 0x80, c3.op, {{"c", 0x30}, {"$TSS0", 0x38}});
    tp::Records& s = w.tu().symbols;
    const auto held = [&](const std::string& display, std::uint32_t at, std::uint32_t closure) {
        s.proc(0x1110, display, SynthWorld::kTextSec, at, 0x10, 0, holder);
        if (closure != 0) {
            tp::Bytes b;
            tp::put32(b, closure);
            tp::put16(b, 0);
            tp::putstr(b, "lambda");
            s.add(0x113E, b);  // S_LOCAL
        }
        s.end();
    };
    held("ll_a", 0xA0, c1.forward);
    held("ll_b", 0xC0, c3.forward);
    held("ll_g", 0xE0, 0);
    w.pub_func("?ll_a@@YAHXZ", 0xA0);
    w.pub_func("?ll_b@@YAHXZ", 0xC0);
    w.pub_func("?ll_g@@YAHXZ", 0xE0);
    t.set.add_to(w.spec.tpi);
    w.open();
}

// The patch: kHash1 unchanged in ll_a, and the edited lambda `edited` (member type `member`, static `name`) held by `holder`.
struct EditedLambda {
    std::string holder = "ll_b";
    std::uint32_t member = tp::kTInt;
    std::string name = "c";
    bool local = true;  // the holder has a local of the closure's type
};
Built evidence_object(const EditedLambda& e) {
    Types t;
    const std::uint32_t holder = t.fn(tp::kTInt, {});
    const ClosureTypes c1 = closure_types(t, kHash1, tp::kTInt);
    const ClosureTypes c3 = closure_types(t, kEdited3, e.member);
    StaticsObj o;
    const auto lambda = [&](const std::string& closure, const ClosureTypes& c, const std::string& name) {
        o.proc(hashed_op(closure), closure + "::operator()", false, c.op);
        o.local("?" + name + "@?1???R" + closure + "@@QEBAHXZ@4HA", name);
        o.local(hashed_guard(closure), "$TSS0");
        o.end();
    };
    lambda(kHash1, c1, "c");
    lambda(kEdited3, c3, e.name);
    const auto held = [&](const std::string& display, std::uint32_t closure) {
        o.proc("?" + display + "@@YAHXZ", display, true, holder);
        if (closure != 0) {
            o.variable(closure, "lambda");
        }
        o.end();
    };
    held("ll_a", c1.forward);
    held(e.holder, e.local ? c3.forward : 0);
    if (e.holder != "ll_b") {
        held("ll_b", 0);
    }
    return o.build(t.set.debug_t());
}

void refused_without_evidence(const EditedLambda& e, const std::string& reason) {
    SynthWorld w;
    evidence_world(w);
    Built b = evidence_object(e);
    std::vector<cml::Diag> errors;
    BindPlan partial;
    REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
    REQUIRE_FALSE(errors.empty());
    CHECK(errors[0].code == diag::kHashedLambdasUnproven);
    CHECK_MESSAGE(contains(errors[0].message, "cannot be shown to be the same lambda: " + reason), errors[0].message);
    CHECK(decision(partial, hashed_op(kEdited3)).decision != DecisionKind::PatchHooked);
    for (const SymbolDecision& d : partial.decisions) {
        if (d.type == SymbolType::Data && d.defined && d.name.find(kEdited3) != std::string::npos) {
            CHECK_MESSAGE(d.decision != DecisionKind::BindOldData, d.name);
        }
    }
}
}  // namespace

TEST_CASE("hash-named lambdas (WP-46 round 2): the one edited lambda is paired by elimination, its static and guard with the old ones") {
    // kHash3's body was edited, so cl named it kEdited3: held by ll_b on both sides, the same capture, the same static, the
    // same call operator. main: CML3214 for both call operators (the set changed) and CML3207 for the statics.
    SynthWorld w;
    evidence_world(w);
    Built b = evidence_object(EditedLambda());
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision op = decision(*r, hashed_op(kEdited3));
    CHECK(op.decision == DecisionKind::PatchHooked);
    CHECK(op.source == BindSource::CompilandSignature);
    CHECK(op.old_address.rva == 0x1080);
    CHECK(decision(*r, hashed_static(kEdited3)).decision == DecisionKind::BindOldData);
    CHECK(decision(*r, hashed_static(kEdited3)).old_address.rva == 0x3030);
    CHECK(decision(*r, hashed_guard(kEdited3)).decision == DecisionKind::BindOldData);
    CHECK(decision(*r, hashed_guard(kEdited3)).old_address.rva == 0x3038);
    // The name-matched lambda keeps its own old objects.
    CHECK(decision(*r, hashed_op(kHash1)).old_address.rva == 0x1060);
    CHECK(decision(*r, hashed_static(kHash1)).old_address.rva == 0x3020);
    CHECK(decision(*r, hashed_guard(kHash1)).old_address.rva == 0x3028);
    // Both are left to the proof by code; the eliminated one says which old lambda it was paired with.
    REQUIRE(r->hashed_lambdas.size() == 2);
    for (const HashedLambda& h : r->hashed_lambdas) {
        CHECK(h.old_closure == (h.closure == kEdited3 ? kHash3 : std::string()));
    }
}

TEST_CASE("hash-named lambdas (WP-46 review): the two lambdas left are paired only with the evidence that they are one (CML3630 otherwise)") {
    SUBCASE("removed from ll_b, added to ll_g: another enclosing function") {
        // 6b0a7de: paired by elimination; the lambda of ll_g took the live static of ll_b's lambda.
        EditedLambda e;
        e.holder = "ll_g";
        refused_without_evidence(e, "the lambda is held by 'll_g' in the patch and by 'll_b' in the old image");
    }
    SUBCASE("another capture (the closure's layout)") {
        EditedLambda e;
        e.member = tp::kTDouble;
        refused_without_evidence(e, "the closures have different layouts");
    }
    SUBCASE("another static") {
        EditedLambda e;
        e.name = "d";
        refused_without_evidence(e, "their call operators have different statics");
    }
    SUBCASE("the enclosing function is not recorded (no local of the closure's type)") {
        EditedLambda e;
        e.local = false;
        refused_without_evidence(e, "the function that holds the lambda is not known on both sides (0 procedure(s) of the patch and 1 of the old image");
    }
}

TEST_CASE("hash-named lambdas (WP-46 round 2): two edited lambdas, or a lambda added or removed, are refused (CML3630), nothing bound") {
    const auto refused = [](SynthWorld& w, Built& b, const std::string& left) {
        std::vector<cml::Diag> errors;
        BindPlan partial;
        REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
        REQUIRE_FALSE(errors.empty());
        CHECK(errors[0].code == diag::kHashedLambdasUnproven);
        CHECK_MESSAGE(contains(errors[0].message, left), errors[0].message);
        for (const SymbolDecision& d : partial.decisions) {
            if (d.type == SymbolType::Data && d.defined && d.name.find("<lambda_") != std::string::npos) {
                CHECK_MESSAGE(d.decision != DecisionKind::BindOldData, d.name);
            }
        }
    };
    SUBCASE("both lambdas edited") {
        SynthWorld w;
        hashed_statics_world(w);
        Built b = hashed_statics_object({kEdited1, kEdited3});
        refused(w, b, "2 lambda(s) of the patch (" + kEdited1 + ", " + kEdited3 + ") and 2 of the old image (" + kHash1 + ", " + kHash3 + ") are left");
    }
    SUBCASE("one lambda removed") {
        SynthWorld w;
        hashed_statics_world(w);
        Built b = hashed_statics_object({kHash1});
        refused(w, b, "0 lambda(s) of the patch (none) and 1 of the old image (" + kHash3 + ") are left");
    }
}

TEST_CASE("hash-named lambdas (WP-46 round 2): an edited lambda with external linkage and statics is refused, its statics never fresh") {
    // kHash2 (an inline function's lambda, external, with `c`) was edited: the patch defines it as kEdited1, a public name the
    // old image has nowhere. main: kEdited1 is a new function whose `c` and guard are fresh (the running program's kHash2
    // statics are left behind: a silent reset). Round 2 ruling, points 2 and 3: refused.
    SynthWorld w;
    hashed_statics_world(w, true);
    Built b = hashed_statics_object({kHash1, kHash3}, kEdited1);
    std::vector<cml::Diag> errors;
    BindPlan partial;
    REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
    REQUIRE_FALSE(errors.empty());
    CHECK(errors[0].code == diag::kHashedLambdasUnproven);
    CHECK_MESSAGE(contains(errors[0].message, "1 lambda(s) of the patch (" + kEdited1 + ") and 1 of the old image (" + kHash2 + ") are left"), errors[0].message);
}

TEST_CASE("hash-named lambdas (WP-46 round 2): an external lambda without statics that is new or gone pairs as before (by its public name)") {
    SynthWorld w;
    Types old_types;
    old_proc(w, kHash1 + "::operator()", 0x60, old_types.call_operator(kHash1, hashed_unique(kHash1)), {{"c", 0x20}, {"$TSS0", 0x28}});
    old_proc(w, kHash2 + "::operator()", 0x70, old_types.call_operator(kHash2, hashed_unique(kHash2)));
    w.pub_func(hashed_op(kHash2), 0x70);
    old_types.set.add_to(w.spec.tpi);
    w.open();
    Types t;
    TypedObj o;
    o.function(hashed_op(kHash1), t.call_operator(kHash1, hashed_unique(kHash1)), false);
    for (const std::string& name : {hashed_static(kHash1), hashed_guard(kHash1)}) {
        const std::uint32_t sec = o.obj.section(".bss", kDataComdat, 8, coff::ComdatSelection::Any);
        o.obj.symbol(name, 0, static_cast<std::int32_t>(sec), false, false);
    }
    o.obj.function(hashed_op(kEdited1), true);  // an inline function's lambda, new or renamed, without statics
    Built b = o.build(t.set.debug_t());
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, hashed_static(kHash1)).old_address.rva == 0x3020);
    CHECK(decision(*r, hashed_op(kEdited1)).decision == DecisionKind::PatchNewFunction);
}

TEST_CASE("hash-named lambdas (WP-46 review): a static call operator that identical-code folding shares with an external function stays in the set") {
    // The linker folded kHash1's call operator with the external `inline_fn` (one address, the public of inline_fn there), and
    // the patch defines inline_fn. 6b0a7de dropped kHash1's old record from the set because a public sits at its address: the
    // sets then differed ({kHash3} against {kHash1, kHash3}) while the record stayed a candidate. Only the call operator's own
    // public takes a record out of the set.
    SynthWorld w;
    Types old_types;
    old_proc(w, kHash1 + "::operator()", 0x60, old_types.call_operator(kHash1, hashed_unique(kHash1)));
    old_proc(w, kHash3 + "::operator()", 0x80, old_types.call_operator(kHash3, hashed_unique(kHash3)));
    w.pub_func("?inline_fn@@YAXXZ", 0x60);
    old_types.set.add_to(w.spec.tpi);
    w.open();
    Types t;
    TypedObj o;
    o.function(hashed_op(kHash1), t.call_operator(kHash1, hashed_unique(kHash1)), false);
    o.function(hashed_op(kHash3), t.call_operator(kHash3, hashed_unique(kHash3)), false);
    o.obj.function("?inline_fn@@YAXXZ", true);
    Built b = o.build(t.set.debug_t());
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(decision(*r, hashed_op(kHash1)).source == BindSource::CompilandSignature);
    CHECK(decision(*r, hashed_op(kHash1)).old_address.rva == 0x1060);
    CHECK(decision(*r, hashed_op(kHash3)).old_address.rva == 0x1080);
}

// ---- WP-46 review (item 3): the layout a guard without a record is placed from ------------------------------------------
namespace {
const char* const kGf = "?gf@@YAHXZ";
const char* const kGfStatic = "?s@?1??gf@@YAHXZ@4HA";
const char* const kGfGuard = "?$TSS0@?1??gf@@YAHXZ@4HA";
}  // namespace

TEST_CASE("guards without a record (WP-46 review): one static paired by its function does not teach the layout of its section") {
    // clang-cl. The old image: two overloads gf() and gf(int) with a static `s` each; gf()'s `s` at data 0x10, its guard (no
    // record) at 0x14. The patch removed gf(int)'s static and inserted an unguarded file-scope static `x` between gf()'s `s`
    // and its guard: `s` 0x10, `x` 0x14, the guard 0x18. `s` (one member, two records of the name) pairs by its function;
    // 6b0a7de learned the section's layout from it alone and placed the guard at 0x3018, which is not the old guard.
    SynthWorld w;
    tp::Records& s = w.tu().symbols;
    s.proc(0x1110, "gf", SynthWorld::kTextSec, 0x40, 0x20);
    s.data(0x110C, "s", SynthWorld::kDataSec, 0x10, tp::kTInt);
    s.end();
    s.proc(0x1110, "gf", SynthWorld::kTextSec, 0x60, 0x20);
    s.data(0x110C, "s", SynthWorld::kDataSec, 0x30, tp::kTInt);
    s.end();
    w.pub_func(kGf, 0x40);
    w.pub_func("?gf@@YAHH@Z", 0x60);
    w.open();
    StaticsObj o;
    const std::uint32_t bss = o.obj.section(".bss", kData, 0x40);
    const std::uint32_t st = o.obj.symbol(kGfStatic, 0x10, static_cast<std::int32_t>(bss), false, false);
    o.obj.symbol("?x@?A0x1@@3HA", 0x14, static_cast<std::int32_t>(bss), false, false);
    o.obj.symbol(kGfGuard, 0x18, static_cast<std::int32_t>(bss), false, false);
    o.proc(kGf, "gf");
    o.record(st, "s");
    o.end();
    Built b = o.build(some_debug_t());
    std::vector<cml::Diag> errors;
    BindPlan partial;
    REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kGuardMismatch);
    CHECK(contains(errors[0].message, "no layout of its object section is known"));
    CHECK(decision(partial, kGfGuard).decision != DecisionKind::BindOldData);
}

TEST_CASE("guards without a record (WP-46 review): a guard is never placed inside a variable or onto a guard the old image records") {
    SUBCASE("inside another variable's extent (not at its start)") {
        // Two statics paired by their functions agree on the layout (delta 0x3000); the guard of the first lands at 0x3022,
        // inside the old 8-byte `wide` at 0x3020. 6b0a7de compared start addresses only and placed it there.
        SynthWorld w;
        Types old_types;
        const std::uint32_t wide_type = old_types.set.add(tp::kLfArray, tp::lf_array(tp::kTInt, 8));
        tp::Records& s = w.tu().symbols;
        s.proc(0x1110, "ga", SynthWorld::kTextSec, 0x40, 0x10);
        s.data(0x110C, "a", SynthWorld::kDataSec, 0x10, tp::kTInt);
        s.end();
        s.proc(0x1110, "gb", SynthWorld::kTextSec, 0x60, 0x10);
        s.data(0x110C, "b", SynthWorld::kDataSec, 0x30, tp::kTInt);
        s.end();
        w.tu().symbols.data(0x110C, "`anonymous namespace'::wide2", SynthWorld::kDataSec, 0x20, wide_type);
        w.pub_func("?ga@@YAHXZ", 0x40);
        w.pub_func("?gb@@YAHXZ", 0x60);
        old_types.set.add_to(w.spec.tpi);
        w.open();
        StaticsObj o;
        const std::uint32_t bss = o.obj.section(".bss", kData, 0x40);
        const std::uint32_t sa = o.obj.symbol("?a@?1??ga@@YAHXZ@4HA", 0x10, static_cast<std::int32_t>(bss), false, false);
        o.obj.symbol("?$TSS0@?1??ga@@YAHXZ@4HA", 0x22, static_cast<std::int32_t>(bss), false, false);
        const std::uint32_t sb = o.obj.symbol("?b@?1??gb@@YAHXZ@4HA", 0x30, static_cast<std::int32_t>(bss), false, false);
        o.obj.symbol("?$TSS0@?1??gb@@YAHXZ@4HA", 0x34, static_cast<std::int32_t>(bss), false, false);
        o.proc("?ga@@YAHXZ", "ga");
        o.record(sa, "a");
        o.end();
        o.proc("?gb@@YAHXZ", "gb");
        o.record(sb, "b");
        o.end();
        Built b = o.build(some_debug_t());
        std::vector<cml::Diag> errors;
        BindPlan partial;
        REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kGuardMismatch);
        CHECK_MESSAGE(contains(errors[0].message, "where the old image records '`anonymous namespace'::wide2' (0x3020..0x3028)"), errors[0].message);
        CHECK(decision(partial, "?$TSS0@?1??ga@@YAHXZ@4HA").decision != DecisionKind::BindOldData);
        // The guard of the second static is placed: 0x3034 overlaps nothing.
        CHECK(decision(partial, "?$TSS0@?1??gb@@YAHXZ@4HA").old_address.rva == 0x3034);
    }
    SUBCASE("onto another function's recorded guard (guards were skipped by their name)") {
        // The layout learned from `s_count` (pass A) puts the guard of gf's `s` at 0x3018, where the old image records the guard
        // `$TSS0` of another function. 6b0a7de skipped records named `$...` and bound it there.
        SynthWorld w;
        w.data(w.tu(), "`anonymous namespace'::s_count", 0x08);
        tp::Records& s = w.tu().symbols;
        s.proc(0x1110, "gf", SynthWorld::kTextSec, 0x40, 0x20);
        s.data(0x110C, "s", SynthWorld::kDataSec, 0x10, tp::kTInt);
        s.end();
        s.proc(0x1110, "other", SynthWorld::kTextSec, 0x60, 0x20);
        s.data(0x110C, "$TSS0", SynthWorld::kDataSec, 0x18, tp::kTInt);
        s.end();
        w.pub_func(kGf, 0x40);
        w.open();
        StaticsObj o;
        const std::uint32_t bss = o.obj.section(".bss", kData, 0x40);
        o.obj.symbol("?s_count@?A0x1@@3HA", 0x08, static_cast<std::int32_t>(bss), false, false);
        const std::uint32_t st = o.obj.symbol(kGfStatic, 0x10, static_cast<std::int32_t>(bss), false, false);
        o.obj.symbol(kGfGuard, 0x18, static_cast<std::int32_t>(bss), false, false);
        o.proc(kGf, "gf");
        o.record(st, "s");
        o.end();
        Built b = o.build(some_debug_t());
        std::vector<cml::Diag> errors;
        BindPlan partial;
        REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kGuardMismatch);
        CHECK_MESSAGE(contains(errors[0].message, "where the old image records '$TSS0'"), errors[0].message);
        CHECK(decision(partial, kGfGuard).decision != DecisionKind::BindOldData);
    }
}

TEST_CASE("hash-named lambdas (WP-46 review): only the closure's own functions and statics are attributed to it") {
    SUBCASE("a function that only mentions the closure (apply<lambda>) does not vouch for the lambda's static") {
        // The patch's lambda kHash1 has no call operator of its own (inlined); its static pairs by the hash name. A static
        // function `apply(<lambda>)` is hooked by its name. 6b0a7de counted apply as a function of the closure: the static was
        // left to a "proof" by apply's code, which says nothing about the lambda.
        SynthWorld w;
        Types old_types;
        old_proc(w, kHash1 + "::operator()", 0x60, old_types.call_operator(kHash1, hashed_unique(kHash1)), {{"sname", 0x20}});
        w.proc(w.tu(), "apply", 0x90, 0x10, true);
        old_types.set.add_to(w.spec.tpi);
        w.open();
        StaticsObj o;
        const std::string sname = "?sname@?1???R" + kHash1 + "@@QEBAHXZ@4HA";
        o.symbol(sname);
        o.obj.function("?apply@@YAHV" + kHash1 + "@@@Z", false);
        Built b = o.build(some_debug_t());
        std::vector<cml::Diag> errors;
        BindPlan partial;
        REQUIRE_FALSE(plan_bind(w.input(b), &errors, &partial).has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kHashedLambdasUnproven);
        CHECK(decision(partial, sname).decision != DecisionKind::BindOldData);
    }
    SUBCASE("a static whose type is the closure is not one of the closure's statics") {
        // `static auto g_closure = []{...};` at file scope: its type names the closure. 6b0a7de took it for a static of the
        // closure, found no hooked function of it and refused the reload (CML3630).
        SynthWorld w = basic_world(false);
        w.data(w.tu(), "`anonymous namespace'::g_closure", 0x10);
        w.open();
        SynthObj o;
        const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x20));
        const std::string closure_static = "?g_closure@?A0x1@@3V" + kHash1 + "@@A";
        o.symbol(closure_static, 0x10, bss, false, false);
        Built b = o.build();
        auto r = plan_bind(w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(decision(*r, closure_static).old_address.rva == 0x3010);
        CHECK(r->hashed_lambdas.empty());
    }
}

TEST_CASE("destructor of a local class (WP-46 review): a lone old record that is the deleting destructor is not paired by name in pass A") {
    // The old compiland records only the scalar deleting destructor under the destructor's name (the destructor itself was
    // inlined into it). 6b0a7de: one member, one candidate, paired by the name: the new `??1` hooked onto the old `??_G`.
    SynthWorld w;
    Types old_types;
    const std::uint32_t type = old_types.fn(tp::kTVoid, {});
    tp::Records& s = w.tu().symbols;
    s.proc(0x110F, kLocalDtorDisplay, SynthWorld::kTextSec, 0x80, 0x10, 0, type);
    s_local(s, "this", 0x0603);
    s_local(s, "should_call_delete", tp::kTInt);
    s.end();
    old_types.set.add_to(w.spec.tpi);
    w.open();
    Built b = local_destructor_object();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const SymbolDecision d = decision(*r, kLocalDtor);
    CHECK(d.decision == DecisionKind::PatchNewFunction);
    CHECK(r->hooks.empty());
}
