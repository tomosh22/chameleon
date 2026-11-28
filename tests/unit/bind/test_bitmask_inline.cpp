// WP-59 (R-67, ADR-0046): clang's bitmask `??_B<scope>@5<n>` of an inline function's statics is a guard.
//
// Without thread-safe statics (`-fno-threadsafe-statics`, `/Zc:threadSafeInit-`) clang-cl guards the statics of an
// internal-linkage function with `?$S1@<scope>@4IA` and those of an inline (externally visible) function with ONE external
// bitmask per function, `??_B<scope>@5<n>`, in a COMDAT of its own. Its bits go to the function's statics in the order
// they are declared (clang's static-local number, constant-initialised statics and thread_locals counted too: see
// docs/wp/WP-59.md). Until WP-59 `is_guard_variable` did not know the name, so the bitmask was planned as ordinary data:
// bound to the old one by its public symbol whatever the edit did to the function's statics (a static added in front of
// the others took a bit the running program had set, and the next one a clear bit: constructed again over the live object).
// Now the guard rules treat it as cl's `$S1`: tied to the statics of its function, never bound next to new ones, refused
// when its statics are bound and the old image has no bitmask of that name, and kept only while the function's code is
// unchanged (prove_bitmask_guards).
//
// The inputs are real: tests/unit/bind/wp59/<case>.{exe,pdb} (the old image, clang 18 clang-cl -O2 /Zc:threadSafeInit- +
// lld-link) and <case>.patch.obj (the edited source), built by tests/unit/bind/wp59/build.sh from src/.
#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "cml/coff/coff.h"
#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_names.h"
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

std::string path_of(const std::string& name) { return std::string(CML_BIND_WP59_DIR) + "/" + name; }

std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), path);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::transform(raw.begin(), raw.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

const char* const kBits = "??_B?1??counter@@YAHXZ@51";
const char* const kCounter = "?counter@@YAHXZ";
const char* const kA = "?a@?1??counter@@YAHXZ@4UObj@@A";
const char* const kB = "?b@?1??counter@@YAHXZ@4UObj@@A";
const char* const kZ = "?z@?1??counter@@YAHXZ@4UObj@@A";

// One case: the old image, the patch, the plan (or what a failed planning decided) and the bitmask proof.
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
        proof = prove_bitmask_guards(ni);
    }

    const SymbolDecision& decision(const std::string& symbol) const {
        const SymbolDecision* d = plan.find_decision(symbol);
        REQUIRE_MESSAGE(d != nullptr, symbol);
        return *d;
    }
    const Diag* plan_error(const char* code_id) const {
        for (const Diag& d : plan_errors) {
            if (d.id() == code_id) {
                return &d;
            }
        }
        return nullptr;
    }
};

}  // namespace

TEST_CASE("R-67: clang's bitmask of an inline function's statics is a guard variable (real clang 18 names)") {
    // clang-cl 18 -fno-threadsafe-statics (docs/wp/WP-59.md): the scope is that of the first guarded static, then `@5` and a
    // number (the depth of that static's block: `?1?` -> `1`, `?4?` -> `4`).
    CHECK(is_guard_variable(kBits));
    CHECK(is_guard_variable("??_B?1??nf@ns@@YAHXZ@51"));
    CHECK(is_guard_variable("??_B?4??nested_first@@YAHH@Z@54"));
    CHECK(is_guard_variable("??_B?1???$tf@H@@YAHXZ@51"));
    CHECK(is_guard_variable("??_B?1??m@S@@QEAAHXZ@51"));
    CHECK(is_guard_variable("??_B?1???R<lambda_1>@?0??lam@@YA?A?<auto>@@XZ@QEBA?A?2@XZ@51"));
    CHECK(is_inline_bitmask_guard(kBits));
    CHECK_FALSE(is_inline_bitmask_guard("?$S1@?1??f@@YAHXZ@4IA"));
    // Not a bitmask: no function scope, another storage class, nothing after `@5`, other `??_` names.
    CHECK_FALSE(is_guard_variable("??_B@51"));
    CHECK_FALSE(is_guard_variable("??_Bcounter@@51"));
    CHECK_FALSE(is_guard_variable("??_B?1??counter@@YAHXZ@4HA"));
    CHECK_FALSE(is_guard_variable("??_B?1??counter@@YAHXZ@5"));
    CHECK_FALSE(is_guard_variable("??_B?1??counter@@YAHXZ@51x"));
    CHECK_FALSE(is_guard_variable("??_7Foo@@6B@"));
    CHECK_FALSE(is_guard_variable("??_R0?AUFoo@@@8"));
    // clang's thread-local counterpart `??__J` is not this rule's (thread_locals are WP-24's, CML3400).
    CHECK_FALSE(is_guard_variable("??__J?1??with_tl@@YAHXZ@51"));
}

TEST_CASE("R-67: the bitmask's scope is its function's, read like the statics' (back-references resolved)") {
    // The special name `?_B` is never in the back-reference table: both readings of the guard are the same.
    for (const bool counted : {false, true}) {
        CAPTURE(counted);
        const auto g = local_scope_key(kBits, counted);
        const auto a = local_scope_key(kA, false);
        REQUIRE(g.has_value());
        REQUIRE(a.has_value());
        CHECK(g->block == a->block);
        CHECK(g->rest == a->rest);
        // clang 18: the lambda's `<auto>` is `?2` in the bitmask (no guard name counted) and `?3` in the static (`ls` is 0).
        const auto lg = local_scope_key("??_B?1???R<lambda_1>@?0??lam@@YA?A?<auto>@@XZ@QEBA?A?2@XZ@51", counted);
        const auto ls = local_scope_key("?ls@?1???R<lambda_1>@?0??lam@@YA?A?<auto>@@XZ@QEBA?A?3@XZ@4UObj@@A", false);
        REQUIRE(lg.has_value());
        REQUIRE(ls.has_value());
        CHECK(lg->rest == ls->rest);
    }
    // The bitmask of nested_first is named in its first static's block (`?4?`), and guards the function's other blocks.
    const auto ng = local_scope_key("??_B?4??nested_first@@YAHH@Z@54", false);
    const auto out = local_scope_key("?out@?1??nested_first@@YAHH@Z@4UObj@@A", false);
    REQUIRE(ng.has_value());
    REQUIRE(out.has_value());
    CHECK(ng->block != out->block);
    CHECK(ng->rest == out->rest);
    CHECK_FALSE(local_scope_key("??_B?1??counter@@YAHXZ@4HA", false).has_value());
}

TEST_CASE("R-67: another function of the file edited: the inline function's bitmask and statics are kept (b1)") {
    const Case c("b1_other");
    REQUIRE(c.planned);
    CHECK(c.decision(kA).decision == DecisionKind::BindOldData);
    CHECK(c.decision(kB).decision == DecisionKind::BindOldData);
    const SymbolDecision& bits = c.decision(kBits);
    CHECK(bits.decision == DecisionKind::BindOldData);
    CHECK(bits.source == BindSource::PdbPublic);
    // The bitmask is bound only while counter() hands out its bits as before: the pipeline proves it from counter's code.
    REQUIRE(c.plan.bitmask_guards.size() == 1);
    CHECK(c.plan.bitmask_guards[0].guard == kBits);
    CHECK(c.plan.bitmask_guards[0].function == kCounter);
    CHECK(c.proof.empty());
    for (const Diag& w : c.plan.warnings) {
        CHECK_MESSAGE(w.code != diag::kGuardUnmatched, w.message);
    }
}

TEST_CASE("R-67: a static declared in front of the inline function's statics: the bitmask is refused (b2, CML3600)") {
    // z takes bit 0, which the running program set for a; a would take bit 1, which is clear: a constructed again, z never.
    const Case c("b2_front");
    CHECK_FALSE(c.planned);
    const Diag* d = c.plan_error("CML3600");
    REQUIRE(d != nullptr);
    CHECK(contains(d->message, kBits));
    CHECK(contains(d->message, "counter::z"));
    CHECK(c.decision(kZ).decision == DecisionKind::PatchNewData);
    CHECK(c.decision(kBits).decision == DecisionKind::PatchNewData);
}

TEST_CASE("R-67: the inline function itself edited: its bitmask is refused by the proof (b3, CML3600)") {
    // Its statics are the same, but nothing proves it but the code, as for cl's `$S1` (accepted over-refusal).
    const Case c("b3_edit");
    REQUIRE(c.planned);
    CHECK(c.decision(kBits).decision == DecisionKind::BindOldData);
    REQUIRE(c.plan.bitmask_guards.size() == 1);
    REQUIRE(c.proof.size() == 1);
    CHECK(c.proof[0].id() == "CML3600");
    CHECK(contains(c.proof[0].message, kBits));
    CHECK(contains(c.proof[0].message, "the code of 'counter' changed"));
}

TEST_CASE("R-67: a bound static now guarded by a bitmask the old image does not have is refused (b4, CML3600)") {
    // The old a was constant-initialised (no bitmask in the old image); a fresh bitmask would construct the live a again.
    const Case c("b4_late");
    CHECK_FALSE(c.planned);
    const Diag* d = c.plan_error("CML3600");
    REQUIRE(d != nullptr);
    CHECK(contains(d->message, kBits));
    CHECK(contains(d->message, "counter::a"));
    CHECK(contains(d->message, "the old image has no bitmask of that name"));
}

TEST_CASE("R-67: an inline function that gains its statics gets a new bitmask, without a warning (b5)") {
    const Case c("b5_new");
    REQUIRE(c.planned);
    CHECK(c.decision(kA).decision == DecisionKind::PatchNewData);
    CHECK(c.decision(kB).decision == DecisionKind::PatchNewData);
    CHECK(c.decision(kBits).decision == DecisionKind::PatchNewData);
    CHECK(c.plan.bitmask_guards.empty());
    CHECK(c.proof.empty());
    for (const Diag& w : c.plan.warnings) {
        CHECK_MESSAGE(w.code != diag::kGuardUnmatched, w.message);
    }
}
