// WP-60 (R-57, ADR-0047): an own static never takes the static of an inlined helper the edit deleted.
//
// clang-cl at /O2 records the function-local statics of a helper it inlined (a `static` function, a member of a class in an
// anonymous namespace) as plain records of the calling procedure, in the shape of the caller's own statics, as it does for an
// inlined lambda (R-54, WP-51). An edit that deletes the helper and declares a static of that name in the caller left one record
// of the name in the caller, and the planner paired the new static with the helper's live object (h1 below, without the flag:
// the plan succeeds, its guard placed by the layout). WP-60 applies WP-51/WP-57's rule to helpers: where the caller's inline sites
// name a helper whose statics may hold the record's name, the static is paired and listed (BindPlan::inlined_owner_statics), and
// the pipeline keeps it only when WP-25's comparator proves the caller unchanged under the plan (prove_inlined_owner_statics);
// otherwise CML3207. A helper clears the record when its statics can be read and hold no static of that name: its own
// procedure in the compiland (an out-of-line copy, found by name and signature) holds none (h5), or it is a member of a class
// with external linkage and the record is a local one (an external function's static is a global record, h8).
//
// The inputs are real: tests/unit/bind/wp60/<case>.{exe,pdb} (the old image, clang 18 clang-cl -O2 + lld-link) and
// <case>.patch.obj (the edited source), built by tests/unit/bind/wp60/build.sh from src/. Synthetic PDBs (as WP-51's tests) cover
// the shapes clang does not write for these sources: an unreadable scope, an out-of-line copy found by name and signature.
#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cml/coff/coff.h"
#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_plan.h"
#include "cml/core/hook_narrowing.h"
#include "cml/pdb/index.h"
#include "cml/pe/pe.h"
#include "doctest.h"
#include "synth.h"
#include "worlds.h"
#include "../pdb/type_builder.h"

using namespace cml;
using namespace cml::core;
using namespace cml::core::bind;
using namespace cml_bind_test;

namespace {

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string path_of(const std::string& name) { return std::string(CML_BIND_WP60_DIR) + "/" + name; }

std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), path);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::transform(raw.begin(), raw.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

const char* const kOwnX = "?x@?1??f@@YAXPEAHH@Z@4UObj@@A";
const char* const kOwnGuard = "?$TSS0@?1??f@@YAXPEAHH@Z@4HA";

// One case: the old image, the patch, the plan and both proofs, as the pipeline runs them (WP-57's harness).
struct Case {
    std::string name;
    std::shared_ptr<const pdb::PdbIndex> pdb;
    std::vector<std::byte> object_bytes;
    std::unique_ptr<coff::Object> object;
    std::unique_ptr<pe::OwnedImage> image;
    bool planned = false;
    BindPlan plan;
    std::vector<Diag> plan_errors;
    std::vector<Diag> lambda_proof;  // WP-53
    std::vector<Diag> owner_proof;   // WP-57 / WP-60

    explicit Case(const std::string& n, bool prove_owners = true, InlineVerdictCache* cache = nullptr) : name(n) {
        auto p = pdb::PdbIndex::open_pdb(path_of(n + ".pdb"));
        REQUIRE_MESSAGE(p.has_value(), p.error().message);
        pdb = *p;
        object_bytes = read_file(path_of(n + ".patch.obj"));
        auto o = coff::Object::parse(coff::ByteSpan(object_bytes.data(), object_bytes.size()));
        REQUIRE_MESSAGE(o.has_value(), o.error().message);
        object = std::make_unique<coff::Object>(std::move(*o));
        auto img = pe::load_image_file(path_of(n + ".exe"));
        REQUIRE_MESSAGE(img.has_value(), img.error().message);
        image = std::make_unique<pe::OwnedImage>(std::move(*img));

        PlanInput in;
        in.object = object.get();
        in.object_name = n + ".patch.obj";
        in.module_pdb = pdb.get();
        in.module_name = n + ".exe";
        in.module_base = 0x140000000ull;
        in.compiland = n + ".old.obj";
        in.prove_inlined_owners = prove_owners;
        in.inline_verdicts = cache;
        BindPlan partial;
        auto r = plan_bind(in, &plan_errors, &partial);
        planned = r.has_value();
        plan = planned ? std::move(*r) : std::move(partial);

        NarrowInput ni;
        ni.object = object.get();
        ni.plan = &plan;
        ni.old_image = &image->image;
        ni.load_base = 0x140000000ull;
        ni.is_hooked = [](std::uint64_t) { return false; };
        const pdb::PdbIndex* index = pdb.get();
        ni.old_proc_size = [index](std::uint32_t rva) -> std::optional<std::uint32_t> {
            const Result<std::optional<pdb::SymbolView>> v = index->find_proc_by_rva(rva);
            if (!v || !v->has_value() || (*v)->rva != rva || (*v)->size == 0) {
                return std::nullopt;
            }
            return (*v)->size;
        };
        InlinedProcVerdicts verdicts;
        lambda_proof = prove_inlined_lambda_statics(ni, &verdicts);
        owner_proof = prove_inlined_owner_statics(ni, &verdicts);
    }

    const InlinedOwnerStatic* listed(const std::string& symbol) const {
        for (const InlinedOwnerStatic& s : plan.inlined_owner_statics) {
            if (s.symbol == symbol) {
                return &s;
            }
        }
        return nullptr;
    }
    const SymbolDecision& decision(const std::string& symbol) const {
        const SymbolDecision* d = plan.find_decision(symbol);
        REQUIRE_MESSAGE(d != nullptr, symbol);
        return *d;
    }
    std::string first_error() const { return plan_errors.empty() ? std::string() : plan_errors.front().message; }
};

// The plan pairs `f::x` and its guard with the old records (and nothing refuses them).
void check_paired(const Case& c) {
    CAPTURE(c.name);
    const std::string error = c.first_error();
    CAPTURE(error);
    REQUIRE(c.planned);
    CHECK(c.decision(kOwnX).decision == DecisionKind::BindOldData);
    CHECK(c.decision(kOwnGuard).decision == DecisionKind::BindOldData);
    const std::string proof = c.owner_proof.empty() ? std::string() : c.owner_proof.front().message;
    CAPTURE(proof);
    CHECK(c.owner_proof.empty());
    CHECK(c.lambda_proof.empty());
}

// Listed for the helper `helper` and kept: the proof found `f` unchanged.
void check_proven(const Case& c, const std::string& helper) {
    check_paired(c);
    const InlinedOwnerStatic* s = c.listed(kOwnX);
    REQUIRE(s != nullptr);
    CHECK(s->helper == helper);
    CHECK(s->proc == "f");
    CHECK(c.decision(kOwnX).old_address.rva == s->record_rva);
}

// Listed for the helper `helper` and refused by the proof with CML3207: WP-51's message, the helper and why the code does not
// prove the pairing.
Diag check_refused(const Case& c, const std::string& helper) {
    CAPTURE(c.name);
    const InlinedOwnerStatic* s = c.listed(kOwnX);
    REQUIRE(s != nullptr);
    CHECK(s->helper == helper);
    CHECK(s->step == "pass A");
    REQUIRE(c.owner_proof.size() == 1);
    const Diag& d = c.owner_proof.front();
    CAPTURE(d.message);
    CHECK(d.id() == "CML3207");
    CHECK(contains(d.message, "'f::x'"));
    CHECK(contains(d.message, "inlined '" + helper + "', a helper function"));
    CHECK(contains(d.message, "may be that code's static and not this one"));
    CHECK(contains(d.message, "only an unchanged 'f' proves it is this one"));
    CHECK(contains(d.what_to_do, "Leave 'f' unchanged while the program runs"));
    return d;
}

}  // namespace

TEST_CASE("R-57: an own static is not paired with the static of an inlined helper the edit deleted (h1)") {
    // Old: `static Obj& h() { static Obj x = make(1); return x; }` inlined into f; new: h deleted, `static Obj x = make(2)` in f.
    // The old image has one record `x` in f (the helper's) and an inline site of `h`; the order of the globals is kept, so the
    // guard is placed by the layout.
    SUBCASE("without the flag the planner pairs, as before WP-60: the new static takes the helper's live object") {
        const Case c("h1_r57", false);
        check_paired(c);
        CHECK(c.plan.inlined_owner_statics.empty());
    }
    SUBCASE("with the flag the static is listed and the proof refuses it: the code of f changed") {
        const Case c("h1_r57");
        const Diag d = check_refused(c, "h");
        CHECK(contains(d.message, "the compiland has no procedure of 'h' (every call to it was inlined), so its statics cannot be read"));
        CHECK(contains(d.message, "the code of 'f' changed"));
        CHECK(contains(c.listed(kOwnX)->risk, "a helper function (free-function)"));
    }
}

TEST_CASE("R-57: a helper in an anonymous-namespace class, and one whose own procedure holds a static of the name (h7, h6)") {
    SUBCASE("`namespace { struct H { static Obj& get() { static Obj x; ... } }; }` inlined, deleted, f declares x") {
        const Case c("h7_internal_class");
        const Diag d = check_refused(c, "`anonymous namespace'::H::get");
        CHECK(contains(d.message, "a helper function (internal-class)"));
    }
    SUBCASE("the helper also has an out-of-line copy (its address is taken), which holds the record x") {
        const Case c("h6_outofline_x");
        const Diag d = check_refused(c, "h");
        CHECK(contains(d.message, "its own procedure (rva "));
        CHECK(contains(d.message, "holds a static 'x'"));
    }
}

TEST_CASE("R-57 compiling to the old machine code is accepted through the proof, as WP-57's s7 (h2)") {
    // `static Obj x = make(1)` moved from the inlined helper into f: the patch's f is the old f byte for byte, so the running
    // program cannot tell the binding from "nothing changed" (ADR-0046 ruling 3, applied to helpers).
    const Case c("h2_r57_samecode");
    check_proven(c, "h");
}

TEST_CASE("R-57: an own static next to an inlined helper reloads while its function is unchanged (h3), and not when it is edited (h4)") {
    SUBCASE("g edited: f is the old code, its own static keeps the running object") {
        const Case c("h3_helper_other");
        check_proven(c, "hs");
    }
    SUBCASE("f edited: refused (the accepted limitation: nothing in the records tells this from h1)") {
        const Case c("h4_helper_edit");
        const Diag d = check_refused(c, "hs");
        CHECK(contains(d.message, "the code of 'f' changed"));
    }
}

TEST_CASE("R-57: a helper whose statics can be read and hold no static of the name clears the record (h5, h8)") {
    SUBCASE("the helper's out-of-line copy (found by name and signature) holds no `x`: f edited, x pairs") {
        const Case c("h5_outofline");
        check_paired(c);
        CHECK(c.plan.inlined_owner_statics.empty());
        CHECK(c.decision("?calls@?1??hs@@YAHH@Z@4HA").decision == DecisionKind::BindOldData);
    }
    SUBCASE("a member of a class with external linkage (`Ext::bump`): its statics are global records, x is a local one") {
        const Case c("h8_external_member");
        check_paired(c);
        CHECK(c.plan.inlined_owner_statics.empty());
    }
}

TEST_CASE("R-57: an inlined external free function is a helper too, its linkage cannot be read (h10, accepted limitation)") {
    // `inline int ext_count() { static int ec; ... }` inlined into f, f edited: LF_FUNC_ID does not say ext_count is external,
    // and no procedure of it is in the compiland, so the record `x` may be its static as far as the image shows.
    const Case c("h10_external_free");
    const Diag d = check_refused(c, "ext_count");
    CHECK(contains(d.message, "the code of 'f' changed"));
}

TEST_CASE("R-57: the helpers of a verdict kept by WP-55's cache are the ones read (h1, then h3)") {
    InlineVerdictCache cache;
    const Case first("h1_r57", true, &cache);
    CHECK(cache.stores() >= 1);
    const Case again("h1_r57", true, &cache);
    CHECK(cache.kept() >= 1);
    check_refused(again, "h");
    const Case other("h3_helper_other", true, &cache);  // another image: its own verdicts
    check_proven(other, "hs");
}

// ---- synthetic PDBs (WP-51's shapes) ----------------------------------------------------------------------------------

namespace {

tp::Bytes site(std::uint32_t inlinee) {
    tp::Bytes b;
    tp::put32(b, 0);  // parent
    tp::put32(b, 0);  // end
    tp::put32(b, inlinee);
    b.push_back(0x0B);  // annotations
    b.push_back(0x04);
    return b;
}

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
    std::uint32_t member(tp::Ipi& ipi, const std::string& cls, const std::string& unique, const std::string& name) {
        const std::uint32_t c = tpi.add(tp::kLfClass, tp::lf_udt(cls, unique, true));
        const std::uint32_t this_ptr = tpi.add(tp::kLfPointer, tp::lf_pointer(c, tp::kPtr64 | 0x400));
        const std::uint32_t fn = tpi.add(tp::kLfMFunction, tp::lf_mfunction(tp::kTInt, c, this_ptr, args, 0));
        return ipi.record(tp::kLfMFuncId, tp::lf_mfunc_id(c, fn, name));
    }
};

// A procedure of the TU with function-local static records and one inline site per inlinee.
void proc_inlining(SynthWorld& w, const std::string& name, std::uint32_t text_offset, std::uint32_t type, std::uint16_t kind,
                   const std::vector<std::pair<std::string, std::uint32_t>>& statics, const std::vector<std::uint32_t>& inlinees) {
    tp::Records& r = w.tu().symbols;
    r.proc(kind, name, SynthWorld::kTextSec, text_offset, 0x30, 0, type);
    for (const auto& s : statics) {
        r.data(0x110C, s.first, SynthWorld::kDataSec, s.second);
    }
    for (const std::uint32_t id : inlinees) {
        r.add(0x114D, site(id));
        r.add(0x114E, tp::Bytes());
    }
    r.end();
}

const char* const kCounter = "?counter@?1??main@@9@4HA";

// ImGui's C-linkage main with `static int counter` (record at .data 0x20) that inlined what `setup` adds; the patch defines
// counter in .bss. Planned with the flag, as the pipeline does.
BindPlan plan_main(const std::function<void(SynthWorld&, OldTypes&)>& setup, std::vector<Diag>* errors = nullptr) {
    SynthWorld w = basic_world(false);
    OldTypes t;
    setup(w, t);
    t.tpi.add_to(w.spec.tpi);
    w.open();
    SynthObj o;
    const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
    o.symbol(kCounter, 0x20, bss, false, false);
    Built b = o.build();
    PlanInput in = w.input(b);
    in.prove_inlined_owners = true;
    BindPlan partial;
    auto r = plan_bind(in, errors, &partial);
    return r.has_value() ? std::move(*r) : std::move(partial);
}

const InlinedOwnerStatic* listed_counter(const BindPlan& p) {
    for (const InlinedOwnerStatic& s : p.inlined_owner_statics) {
        if (s.symbol == kCounter) {
            return &s;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("R-57 (synthetic): which inlinees put an own static to the proof") {
    SUBCASE("a free function without a procedure in the compiland (WP-51's ImGui main and ImMax): listed") {
        const BindPlan p = plan_main([](SynthWorld& w, OldTypes& t) {
            const std::uint32_t im_max = w.spec.ipi.record(tp::kLfFuncId, tp::lf_func_id(t.fn_int, "ImMax"));
            proc_inlining(w, "main", 0x60, t.fn_int, 0x1110, {{"counter", 0x20}}, {im_max});
        });
        const InlinedOwnerStatic* s = listed_counter(p);
        REQUIRE(s != nullptr);
        CHECK(s->helper == "ImMax");
        CHECK(s->record_rva == 0x3020);
        CHECK(p.find_decision(kCounter)->decision == DecisionKind::BindOldData);
    }
    SUBCASE("its out-of-line copy, of the same name and signature, holds no `counter`: cleared") {
        const BindPlan p = plan_main([](SynthWorld& w, OldTypes& t) {
            const std::uint32_t im_max = w.spec.ipi.record(tp::kLfFuncId, tp::lf_func_id(t.fn_int, "ImMax"));
            proc_inlining(w, "ImMax", 0x90, t.fn_int, 0x110F, {{"other", 0x30}}, {});
            proc_inlining(w, "main", 0x60, t.fn_int, 0x1110, {{"counter", 0x20}}, {im_max});
        });
        CHECK(listed_counter(p) == nullptr);
        CHECK(p.find_decision(kCounter)->decision == DecisionKind::BindOldData);
    }
    SUBCASE("a procedure of that name but another signature is not the helper: listed") {
        const BindPlan p = plan_main([](SynthWorld& w, OldTypes& t) {
            const std::uint32_t im_max = w.spec.ipi.record(tp::kLfFuncId, tp::lf_func_id(t.fn_int, "ImMax"));
            proc_inlining(w, "ImMax", 0x90, t.fn_void, 0x110F, {}, {});
            proc_inlining(w, "main", 0x60, t.fn_int, 0x1110, {{"counter", 0x20}}, {im_max});
        });
        REQUIRE(listed_counter(p) != nullptr);
        CHECK(contains(listed_counter(p)->risk, "no procedure of 'ImMax'"));
    }
    SUBCASE("a scoped helper's procedure is found by its qualified name (`ns::Clamp`), and holds `counter`: listed") {
        const BindPlan p = plan_main([](SynthWorld& w, OldTypes& t) {
            const std::uint32_t ns = w.spec.ipi.string_id("ns");
            const std::uint32_t clamp = w.spec.ipi.record(tp::kLfFuncId, tp::lf_func_id(t.fn_int, "Clamp", ns));
            proc_inlining(w, "ns::Clamp", 0x90, t.fn_int, 0x110F, {{"counter", 0x20}}, {});
            proc_inlining(w, "main", 0x60, t.fn_int, 0x1110, {{"counter", 0x20}}, {clamp});
        });
        REQUIRE(listed_counter(p) != nullptr);
        CHECK(listed_counter(p)->helper == "ns::Clamp");
        CHECK(contains(listed_counter(p)->risk, "holds a static 'counter'"));
    }
    SUBCASE("a helper whose scope cannot be read: listed") {
        const BindPlan p = plan_main([](SynthWorld& w, OldTypes& t) {
            const std::uint32_t im_max = w.spec.ipi.record(tp::kLfFuncId, tp::lf_func_id(t.fn_int, "ImMax", 0x1FFF));
            proc_inlining(w, "ImMax", 0x90, t.fn_int, 0x110F, {}, {});
            proc_inlining(w, "main", 0x60, t.fn_int, 0x1110, {{"counter", 0x20}}, {im_max});
        });
        REQUIRE(listed_counter(p) != nullptr);
        CHECK(contains(listed_counter(p)->risk, "the scope of 'ImMax' cannot be read"));
    }
    SUBCASE("Jolt's GetRTTIOfType inlined only the constructor of JPH::RTTI (a class with external linkage): cleared") {
        SynthWorld w = basic_world(false);
        OldTypes t;
        const std::uint32_t ctor = t.member(w.spec.ipi, "JPH::RTTI", ".?AVRTTI@JPH@@", "RTTI");
        proc_inlining(w, "JPH::GetRTTIOfType", 0x60, t.fn_void, 0x1110, {{"rtti", 0x20}}, {ctor});
        t.tpi.add_to(w.spec.tpi);
        w.open();
        SynthObj o;
        const std::int32_t bss = static_cast<std::int32_t>(o.section(".bss", kData, 0x40));
        const std::string rtti = "?rtti@?1??GetRTTIOfType@JPH@@YAPEAVRTTI@2@PEAVSphereShape@2@@Z@4V32@A";
        o.symbol(rtti, 0x20, bss, false, false);
        Built b = o.build();
        PlanInput in = w.input(b);
        in.prove_inlined_owners = true;
        auto r = plan_bind(in);
        REQUIRE(r.has_value());
        CHECK(r->inlined_owner_statics.empty());
        CHECK(r->find_decision(rtti)->decision == DecisionKind::BindOldData);
    }
    SUBCASE("a member of a class in an anonymous namespace: listed") {
        const BindPlan p = plan_main([](SynthWorld& w, OldTypes& t) {
            const std::uint32_t get = t.member(w.spec.ipi, "`anonymous namespace'::H", ".?AUH@?A0x1234@@", "get");
            proc_inlining(w, "main", 0x60, t.fn_int, 0x1110, {{"counter", 0x20}}, {get});
        });
        REQUIRE(listed_counter(p) != nullptr);
        CHECK(listed_counter(p)->helper == "`anonymous namespace'::H::get");
        CHECK(contains(listed_counter(p)->risk, "(internal-class)"));
    }
}
