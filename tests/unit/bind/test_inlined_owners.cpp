// WP-57 (R-59, ADR-0046): WP-51 refuses an own static only when the inlined closure cannot be proven unchanged.
//
// WP-51 (R-54) refuses an own static of a function `f` whose old procedure holds its record and inlined closure code: the
// record may be the static of a lambda the edit deleted, which clang-cl at /O2 records as a plain record of `f`, in the shape
// of `f`'s own statics. Nothing in the records tells the two apart (s10 against s11 below: the same records, the same
// closure, the same patch shape); the code of `f` does. With PlanInput::prove_inlined_owners the planner pairs such a static
// and lists it (BindPlan::inlined_owner_statics), and the pipeline keeps it only while the patch's `f` is the old code under
// the plan (prove_inlined_owner_statics, WP-53's proof), CML3207 otherwise. Without the flag the planner refuses as WP-51
// does (test_inlined_lambdas.cpp, unchanged).
//
// `std::sort`'s out-of-line part is a static template instantiation over the closure, which the planner never pairs, so `f`
// calls a new function: the proof's callee step compares that function with the old procedure the old `f` called.
//
// The inputs are real: tests/unit/bind/wp57/<case>.{exe,pdb} (the old image, clang 18 clang-cl -O2 + lld-link) and
// <case>.patch.obj (the edited source), built by tests/unit/bind/wp57/build.sh from src/.
#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "cml/coff/coff.h"
#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_plan.h"
#include "cml/core/hook_narrowing.h"
#include "cml/pdb/index.h"
#include "cml/pe/pe.h"
#include "doctest.h"

using namespace cml;
using namespace cml::core;
using namespace cml::core::bind;

namespace {

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string path_of(const std::string& name) { return std::string(CML_BIND_WP57_DIR) + "/" + name; }

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

// One case: the old image, the patch, the plan (or what a failed planning decided) and both proofs, as the pipeline runs them.
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
    std::vector<Diag> owner_proof;   // WP-57

    explicit Case(const std::string& n, bool prove_owners = true, std::size_t max_callee_pairs = 64) : name(n) {
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
        BindPlan partial;
        auto r = plan_bind(in, &plan_errors, &partial);
        planned = r.has_value();
        plan = planned ? std::move(*r) : std::move(partial);

        NarrowInput ni = narrow_input();
        ni.max_callee_pairs = max_callee_pairs;
        InlinedProcVerdicts verdicts;
        lambda_proof = prove_inlined_lambda_statics(ni, &verdicts);
        owner_proof = prove_inlined_owner_statics(ni, &verdicts);
    }

    NarrowInput narrow_input() const {
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
        return ni;
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

// The own static and its guard are paired with the old ones, listed, and proven.
void check_reloads(const Case& c) {
    CAPTURE(c.name);
    const std::string error = c.first_error();
    CAPTURE(error);
    REQUIRE(c.planned);
    const InlinedOwnerStatic* s = c.listed(kOwnX);
    REQUIRE(s != nullptr);
    CHECK(s->proc == "f");
    CHECK(s->display == "f::x");
    CHECK(c.decision(kOwnX).decision == DecisionKind::BindOldData);
    CHECK(c.decision(kOwnX).old_address.rva == s->record_rva);
    CHECK(c.decision(kOwnGuard).decision == DecisionKind::BindOldData);
    const std::string proof = c.owner_proof.empty() ? std::string() : c.owner_proof.front().message;
    CAPTURE(proof);
    CHECK(c.owner_proof.empty());
}

// The proof refuses the own static with CML3207: WP-51's message and why the code does not prove it.
const Diag& check_refused(const Case& c) {
    CAPTURE(c.name);
    REQUIRE(c.listed(kOwnX) != nullptr);
    REQUIRE(c.owner_proof.size() == 1);
    const Diag& d = c.owner_proof.front();
    CHECK(d.id() == "CML3207");
    CHECK(contains(d.message, "'f::x'"));
    CHECK(contains(d.message, "may be that code's static and not this one"));
    CHECK(contains(d.message, "only an unchanged 'f' proves it is this one"));
    CHECK(contains(d.what_to_do, "Leave 'f' unchanged while the program runs"));
    return d;
}

}  // namespace

TEST_CASE("R-59: an own static next to a std::sort with a lambda reloads when the edit leaves f's code as it was (s1)") {
    // f: `static Obj x`, std::sort(a, a + n, [](int l, int r) { return l > r; }); the edit is in g. f inlined `sort`, a function
    // over the closure (WP-51's risk), and calls the static instantiation std::__introsort_loop<..., <lambda_0>>, which the
    // planner never pairs: the callee step proves it is the old one (it calls itself: a pair being compared counts as equal).
    const Case c("s1_sort_other");
    check_reloads(c);
    CHECK(contains(c.listed(kOwnX)->risk, "inlined 'sort', a function over a closure type"));
    CHECK(c.listed(kOwnX)->step == "pass A");
}

TEST_CASE("R-59: without the flag the planner refuses as WP-51 does") {
    const Case c("s1_sort_other", false);
    CHECK_FALSE(c.planned);
    REQUIRE(!c.plan_errors.empty());
    CHECK(c.plan_errors.front().id() == "CML3207");
    CHECK(contains(c.plan_errors.front().message, "may be that code's static and not this one"));
    CHECK(c.plan.inlined_owner_statics.empty());
    CHECK(c.decision(kOwnX).decision != DecisionKind::BindOldData);
}

TEST_CASE("R-59: an own static next to a fully inlined call_once-like lambda reloads when f is unchanged (s3)") {
    const Case c("s3_once_other");
    check_reloads(c);
    CHECK(contains(c.listed(kOwnX)->risk, "inlined 'once'"));
}

TEST_CASE("R-59: one verdict on f proves its own static and WP-53's inlined lambda static (s8)") {
    // f: own x, a std::sort lambda and an inlined lambda with `static Obj y` (WP-53's in-caller pairing); g edited.
    const Case c("s8_both_other");
    check_reloads(c);
    REQUIRE(c.plan.inlined_lambda_statics.size() == 1);
    CHECK(c.plan.inlined_lambda_statics[0].display == "f::<lambda_1>::operator()::y");
    CHECK(c.lambda_proof.empty());
}

TEST_CASE("R-54 is still refused: the lambda's static deleted and declared in f, with the guard placed (s12)") {
    // The order of the globals is kept, so the plan succeeds: f::x by its record, $TSS0 by the layout. Before WP-51 that was
    // the wrong pairing; now the proof is what refuses it, because the code of f changed.
    const Case c("s12_r54_layout");
    const std::string error = c.first_error();
    CAPTURE(error);
    REQUIRE(c.planned);
    CHECK(c.decision(kOwnX).decision == DecisionKind::BindOldData);
    CHECK(c.decision(kOwnGuard).decision == DecisionKind::BindOldData);
    const Diag& d = check_refused(c);
    CHECK(contains(d.message, "inlined 'f::<lambda_0>::operator()', a lambda's code"));
    CHECK(contains(d.message, "the code of 'f' changed"));
}

TEST_CASE("R-54 with the lambda kept (same signature, its static moved into f) is refused like its benign twin (s10, s11)") {
    // s10's patch is s11's old source: the old images hold one record `x` in f and one inline site of the same closure's
    // call operator, the patches the same closure without statics. Only the code tells them apart, and in both f changed.
    {
        const Case c("s10_r54_keep_sig");
        CAPTURE(c.first_error());
        check_refused(c);
    }
    {
        const Case c("s11_benign_sig");
        REQUIRE(c.planned);
        const Diag& d = check_refused(c);
        CHECK(contains(d.message, "the code of 'f' changed"));
    }
}

TEST_CASE("R-54 compiling to the old machine code is accepted (s7, ADR-0046, as WP-53's c7)") {
    // `[] { static int x = 5; return ++x; }` inlined into f becomes f's own `static int x = 5`: the patch's f, with x at the
    // old record, is the old f byte for byte. The running program cannot tell the binding from an unchanged function, so f's
    // new x keeps the deleted lambda's value.
    const Case c("s7_r54_samecode");
    REQUIRE(c.planned);
    REQUIRE(c.listed("?x@?1??f@@YAXPEAHH@Z@4HA") != nullptr);
    CHECK(c.owner_proof.empty());
}

TEST_CASE("the callee step: a new static function over the closure equal to the old one proves f (s15)") {
    // f calls `apply<lambda_1>` (noinline, static) and inlines `once` with lambda_0; g edited.
    const Case c("s15_callee_same");
    check_reloads(c);
}

TEST_CASE("the callee step: a changed lambda in the called template instantiation refuses f (s13, s14)") {
    {
        // `v * 2` -> `v * 4` in the lambda apply<> inlines: f's bytes are the old ones, the callee's are not.
        const Case c("s13_callee_changed");
        REQUIRE(c.planned);
        const Diag& d = check_refused(c);
        CHECK(contains(d.message, "the new static function '??$apply@V<lambda_1>@"));
        CHECK(contains(d.message, "its code is not the code there"));
    }
    {
        // A longer lambda: the callee's size is not the old procedure's.
        const Case c("s14_callee_size");
        REQUIRE(c.planned);
        const Diag& d = check_refused(c);
        CHECK(contains(d.message, "the new static function '??$apply@V<lambda_1>@"));
        CHECK(contains(d.message, "the old procedure there has"));
    }
}

TEST_CASE("the callee step: a callee without an old procedure there, or over the pair cap, cannot prove f") {
    {
        const Case c("s1_sort_other", true, 0);
        REQUIRE(c.planned);
        const Diag& d = check_refused(c);
        CHECK(contains(d.message, "more than 0 callee pairs would have to be compared"));
    }
    {
        Case c("s15_callee_same");
        REQUIRE(c.planned);
        NarrowInput ni = c.narrow_input();
        ni.old_proc_size = [](std::uint32_t) -> std::optional<std::uint32_t> { return std::nullopt; };
        const std::vector<Diag> out = prove_inlined_owner_statics(ni);
        REQUIRE(out.size() == 1);
        CHECK(contains(out.front().message, "no old procedure starts there"));
    }
}

TEST_CASE("the callee step is not hook narrowing's: f still counts as changed there (s1)") {
    const Case c("s1_sort_other");
    REQUIRE(c.planned);
    const NarrowResult r = narrow_hooks(c.narrow_input());
    bool judged = false;
    for (const NarrowDecision& d : r.decisions) {
        if (d.symbol == "?f@@YAXPEAHH@Z") {
            judged = true;
            CHECK(d.reason == NarrowReason::TargetNew);
        }
    }
    CHECK(judged);
}

TEST_CASE("the callee step on a later reload: f and the callee are compared with the code the earlier patch gave them (s1)") {
    // After the first reload f is hooked (hook narrowing does not follow the callee) and its patch copy calls the patch's own
    // __introsort_loop. The next reload of the file compares f with that recorded code, whose call reaches the earlier
    // patch's copy of the callee: the callee step compares the new callee with that copy's record.
    const Case c("s1_sort_other");
    REQUIRE(c.planned);
    const NarrowInput base = c.narrow_input();
    const CodeRecorder recorder(base);
    std::string introsort;
    for (const SymbolDecision& d : c.plan.decisions) {
        if (d.type == SymbolType::Function && d.decision == DecisionKind::PatchNewFunction && contains(d.name, "__introsort_loop")) {
            introsort = d.name;
        }
    }
    REQUIRE(!introsort.empty());
    const std::uint64_t f_patch = 0x150000000ull;
    const std::uint64_t introsort_patch = 0x150001000ull;
    const auto address_of = [&](const std::string& name) -> std::optional<std::uint64_t> {
        if (name == introsort) {
            return introsort_patch;
        }
        return name == "?f@@YAXPEAHH@Z" ? std::optional<std::uint64_t>(f_patch) : std::nullopt;
    };
    std::optional<CodeImage> f_code = recorder.capture("?f@@YAXPEAHH@Z");
    std::optional<CodeImage> introsort_code = recorder.capture(introsort);
    REQUIRE(f_code.has_value());
    REQUIRE(introsort_code.has_value());
    REQUIRE(finalize_code(*f_code, address_of));
    REQUIRE(finalize_code(*introsort_code, address_of));
    const auto f_record = std::make_shared<const CodeImage>(std::move(*f_code));
    const auto introsort_record = std::make_shared<const CodeImage>(std::move(*introsort_code));
    const SymbolDecision& f = c.decision("?f@@YAXPEAHH@Z");

    NarrowInput ni = base;
    ni.is_hooked = [&](std::uint64_t va) { return va == f.old_address.va; };
    ni.prior_code = [&](std::uint64_t va) -> std::shared_ptr<const CodeImage> {
        if (va == f.old_address.va) {
            return f_record;
        }
        return va == introsort_patch ? introsort_record : nullptr;
    };
    const std::vector<Diag> proven = prove_inlined_owner_statics(ni);
    CHECK(proven.empty());

    // Without the earlier patch's copy of the callee there is nothing to compare it with (no old procedure at its address).
    ni.prior_code = [&](std::uint64_t va) -> std::shared_ptr<const CodeImage> { return va == f.old_address.va ? f_record : nullptr; };
    const std::vector<Diag> out = prove_inlined_owner_statics(ni);
    REQUIRE(out.size() == 1);
    CHECK(contains(out.front().message, "no old procedure starts there"));
}

TEST_CASE("the owner proof refuses a pairing whose procedure is not hooked or whose old code is not available") {
    Case c("s3_once_other");
    REQUIRE(c.planned);
    BindPlan unhooked = c.plan;
    unhooked.hooks.clear();
    NarrowInput ni = c.narrow_input();
    ni.plan = &unhooked;
    std::vector<Diag> out = prove_inlined_owner_statics(ni);
    REQUIRE(out.size() == 1);
    CHECK(contains(out.front().message, "is not paired with a function of the patch"));

    ni.plan = &c.plan;
    ni.old_image = nullptr;
    out = prove_inlined_owner_statics(ni);
    REQUIRE(out.size() == 1);
    CHECK(contains(out.front().message, "the old code of 'f' is not available"));
}
