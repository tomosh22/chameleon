// WP-53 (R-61, ADR-0045): a new static of an inlined lambda never takes its caller's deleted static.
//
// clang-cl at -O2 records the static of a lambda it inlined into `f` as a plain record of `f`, and `f`'s own statics the
// same way (docs/wp/WP-53.md, round 1). An edit that deletes `f`'s own `static x` and declares `static x` in the lambda
// therefore leaves a record `x` in `f` that WP-40's in-caller pairing gives to the lambda's new static: the lambda takes
// `f`'s live object and its initialiser never runs. Nothing in the records tells the two apart; the code of `f` does. The
// planner lists every such pairing (BindPlan::inlined_lambda_statics) and the pipeline keeps it only when the patch's `f`
// is the old code under the plan (prove_inlined_lambda_statics), CML3207 otherwise.
//
// The inputs are real: tests/unit/bind/wp53/<case>.{exe,pdb} (the old image, clang 18 clang-cl -O2 + lld-link) and
// <case>.patch.obj (the edited source), built by tests/unit/bind/wp53/build.sh from src/. On main (d110b5d) c10 and c7
// were accepted with the wrong pairing (c10 by the layout, c7 by the place in the function), and c3, c4, c5, c12 were
// refused only because their guard found no layout (CML3600): the edit had changed the order the globals were created in.
// In c4 and c12 that refused guard is also the only difference the code proof sees; with the layout kept such an edit has
// the old code and is accepted (c13), as ruled for c7.
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

std::string path_of(const std::string& name) { return std::string(CML_BIND_WP53_DIR) + "/" + name; }

std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), path);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::transform(raw.begin(), raw.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

// One case: the old image, the patch, the plan (or what a failed planning decided) and the proof.
struct Case {
    std::string name;
    std::shared_ptr<const pdb::PdbIndex> pdb;
    std::vector<std::byte> object_bytes;
    std::unique_ptr<coff::Object> object;
    std::unique_ptr<pe::OwnedImage> image;
    bool planned = false;
    BindPlan plan;
    std::vector<Diag> plan_errors;
    std::vector<Diag> proof;

    explicit Case(const std::string& n) : name(n) {
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
        BindPlan partial;
        auto r = plan_bind(in, &plan_errors, &partial);
        planned = r.has_value();
        plan = planned ? std::move(*r) : std::move(partial);

        NarrowInput ni;
        ni.object = object.get();
        ni.plan = &plan;
        ni.old_image = &image->image;
        ni.load_base = in.module_base;
        ni.is_hooked = [](std::uint64_t) { return false; };
        proof = prove_inlined_lambda_statics(ni);
    }

    const InlinedLambdaStatic* listed(const std::string& display) const {
        for (const InlinedLambdaStatic& s : plan.inlined_lambda_statics) {
            if (s.display == display) {
                return &s;
            }
        }
        return nullptr;
    }
    // The proof's CML3207 for a static, by its display name.
    const Diag* refusal(const std::string& display) const {
        for (const Diag& d : proof) {
            if (d.code == diag::kAmbiguousStatic && contains(d.message, "'" + display + "'")) {
                return &d;
            }
        }
        return nullptr;
    }
};

const char* const kLambdaX = "f::<lambda_0>::operator()::x";

void check_refused(const Case& c, const std::string& display) {
    CAPTURE(c.name);
    CAPTURE(display);
    const InlinedLambdaStatic* s = c.listed(display);
    REQUIRE(s != nullptr);
    CHECK(s->proc == "f");
    const Diag* d = c.refusal(display);
    REQUIRE(d != nullptr);
    CHECK(contains(d->message, "only an unchanged 'f' proves the record is this lambda's"));
    CHECK(contains(d->message, "the code of 'f' changed"));
}

}  // namespace

TEST_CASE("R-61: a lambda's new static is not given its caller's deleted static (clang-cl -O2, real images)") {
    SUBCASE("c3: f's own x deleted, x declared in f's inlined lambda") {
        const Case c("c3_r61");
        check_refused(c, kLambdaX);
    }
    SUBCASE("c5: the lambda already had a static y and gains x") {
        const Case c("c5_r61_other");
        check_refused(c, kLambdaX);
        // y is the old lambda's (its record is that of y): refused as well, since f changed and nothing else proves it.
        check_refused(c, "f::<lambda_0>::operator()::y");
    }
    SUBCASE("c10: the own x was in a block of f; the layout survives the edit, so main accepted the wrong pairing") {
        const Case c("c10_r61_block");
        CHECK(c.planned);  // the planner still pairs it; the proof refuses
        REQUIRE(c.listed(kLambdaX) != nullptr);
        CHECK(c.listed(kLambdaX)->how == BindSource::CompilandRva);
        check_refused(c, kLambdaX);
    }
}

TEST_CASE("R-61 is refused whatever the order the globals were created in, where the code of f changed") {
    // c3 and c5 fail the plan with CML3600 as well (their guard found no layout: the edit changed the order in which the
    // globals were created); c10 keeps the order and plans. In all three the code of f itself changed, and the proof
    // refuses them alike, whatever the guard's fate.
    for (const char* name : {"c3_r61", "c5_r61_other", "c10_r61_block"}) {
        const Case c(name);
        CAPTURE(c.name);
        const Diag* d = c.refusal(kLambdaX);
        REQUIRE(d != nullptr);
        CHECK_FALSE(contains(d->message, "target-new"));  // a change of f's own code, not the refused guard
    }
}

TEST_CASE("c4 and c12: their refusal comes from the guard that found no layout, not from f's code") {
    // In c4 (the lambda new in the patch) and c12 (x moved into a lambda that only returns it) the only difference the
    // comparator finds is the reference to the lambda's guard, which the plan could not place (CML3600: the edit changed the
    // order the globals were created in) and made new ("target-new"). With the layout kept, a c12-shaped edit has the old
    // code and is accepted: see c13.
    for (const char* name : {"c4_r61_newlambda", "c12_r61_samecode"}) {
        const Case c(name);
        CAPTURE(c.name);
        CHECK_FALSE(c.planned);
        bool cml3600 = false;
        for (const Diag& e : c.plan_errors) {
            cml3600 = cml3600 || e.code == diag::kGuardMismatch;
        }
        CHECK(cml3600);
        const Diag* d = c.refusal(kLambdaX);
        REQUIRE(d != nullptr);
        CHECK(contains(d->message, "target-new"));
        CHECK(contains(d->message, "$TSS0"));
    }
}

TEST_CASE("c13: an R-61 edit with the guard placed and f's code unchanged is accepted (ADR-0045, as c7)") {
    // c12 rewritten so that s_count is created first on both sides: the layout is kept, the lambda's guard is placed on f's
    // old guard, and the patch's f is the old f byte for byte. The running program cannot tell the binding from an unchanged
    // function, so the pairing stands and the lambda's x keeps f's deleted x's value.
    const Case c("c13_r61_samecode_layout");
    const std::string first_error = c.plan_errors.empty() ? std::string() : c.plan_errors.front().message;
    CAPTURE(first_error);
    REQUIRE(c.planned);
    REQUIRE(c.listed(kLambdaX) != nullptr);
    const SymbolDecision* x = c.plan.find_decision(c.listed(kLambdaX)->symbol);
    REQUIRE(x != nullptr);
    CHECK(x->decision == DecisionKind::BindOldData);
    const SymbolDecision* guard = c.plan.find_decision("?$TSS0@?1???R<lambda_0>@?0??f@@YAXXZ@QEBA@XZ@4HA");
    REQUIRE(guard != nullptr);
    CHECK(guard->decision == DecisionKind::BindOldData);
    CHECK(c.proof.empty());
}

TEST_CASE("WP-40's lambda statics in their caller still bind while the caller is unchanged (c11, the fixture's mark_3)") {
    const Case c("c11_wp40_other");
    REQUIRE(c.planned);
    REQUIRE(c.plan.inlined_lambda_statics.size() == 3);
    for (int n = 0; n < 3; ++n) {
        const std::string display = "f::<lambda_" + std::to_string(n) + ">::operator()::x";
        CAPTURE(display);
        REQUIRE(c.listed(display) != nullptr);
        const SymbolDecision* d = c.plan.find_decision(c.listed(display)->symbol);
        REQUIRE(d != nullptr);
        CHECK(d->decision == DecisionKind::BindOldData);
    }
    CHECK(c.proof.empty());
}

TEST_CASE("R-61 with the old machine code (c7: a constant-initialised static moved into the lambda) is accepted (ADR-0045)") {
    // The patch's f, with the lambda's x at the old record, is the old f byte for byte: the running program cannot tell the
    // binding from an unchanged function, so the pairing stands (docs/wp/WP-53.md; KNOWN_LIMITATIONS).
    const Case c("c7_r61_const");
    REQUIRE(c.planned);
    REQUIRE(c.listed(kLambdaX) != nullptr);
    CHECK(c.proof.empty());
}

TEST_CASE("WP-40's lambda statics in an EDITED caller are refused: nothing proves the record is the lambda's (c1, c8)") {
    {
        const Case c("c1_wp40_once");
        REQUIRE(c.planned);  // the planner pairs it, as on main
        check_refused(c, kLambdaX);
    }
    {
        const Case c("c8_wp40_three");
        REQUIRE(c.planned);
        for (int n = 0; n < 3; ++n) {
            check_refused(c, "f::<lambda_" + std::to_string(n) + ">::operator()::x");
        }
        CHECK(c.proof.size() == 3);
    }
}

TEST_CASE("the proof refuses a pairing whose procedure is not hooked or whose old code is not available") {
    Case c("c11_wp40_other");
    REQUIRE(c.planned);
    BindPlan unhooked = c.plan;
    unhooked.hooks.clear();
    NarrowInput ni;
    ni.object = c.object.get();
    ni.plan = &unhooked;
    ni.old_image = &c.image->image;
    ni.is_hooked = [](std::uint64_t) { return false; };
    std::vector<Diag> out = prove_inlined_lambda_statics(ni);
    REQUIRE(out.size() == 3);
    CHECK(contains(out.front().message, "is not paired with a function of the patch"));

    ni.plan = &c.plan;
    ni.old_image = nullptr;
    out = prove_inlined_lambda_statics(ni);
    REQUIRE(out.size() == 3);
    CHECK(contains(out.front().message, "the old code of 'f' is not available"));
}
