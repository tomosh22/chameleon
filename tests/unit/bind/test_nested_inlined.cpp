// WP-56 (R-66, ADR-0046): the static of a nested lambda inlined all the way is looked for in the chain of its enclosing
// procedures, and binds where the code proves the record its own.
//
// clang-cl at -O2 inlines both lambdas of planner_pairing_o2/nested.cpp (without the fixture's noinline) into the function
// and records the inner lambda's `nested` as a plain record of that function, two lambdas up from its call operator. Until
// WP-56 the planner looked one level up only (the outer call operator, which has no procedure): the static was new, and
// WP-52's ruling refused that (CML3207, "would be new in the patch"). Now every procedure of the chain is searched; the
// record pairs (by the section layout, as WP-40's inlined records do) and is listed for WP-53's proof: the pairing stands
// only if the patch's version of that procedure is the old code under the plan AND refers to the static (an unchanged
// procedure that does not use the static says nothing about whose the record is).
//
// The inputs are real: tests/unit/bind/wp56/<case>.{exe,pdb} (the old image, clang 18 clang-cl -O2 + lld-link) and
// <case>.patch.obj (the edited source), built by tests/unit/bind/wp56/build.sh from src/.
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "../pe/pe_builder.h"
#include "cml/coff/coff.h"
#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_plan.h"
#include "cml/core/hook_narrowing.h"
#include "cml/pdb/index.h"
#include "cml/pe/pe.h"
#include "doctest.h"
#include "synth.h"

using namespace cml;
using namespace cml::core;
using namespace cml::core::bind;

namespace {

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string path_of(const std::string& name) { return std::string(CML_BIND_WP56_DIR) + "/" + name; }

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
};

// clang 18's names of the inner lambda's static and its guard (f's lambdas return `auto`: the guard spells the second
// `<auto>` as `3`, the static as `4`, WP-52).
const char* const kNested = "?nested@?1???R<lambda_1>@?0???R<lambda_0>@?0??f@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?4@XZ@4UObj@@A";
const char* const kNestedGuard = "?$TSS0@?1???R<lambda_1>@?0???R<lambda_0>@?0??f@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?3@XZ@4HA";
const char* const kNestedDisplay = "f::<lambda_0>::operator()::<lambda_1>::operator()::nested";

SymbolDecision decision(const BindPlan& p, const std::string& name) {
    const SymbolDecision* d = p.find_decision(name);
    REQUIRE_MESSAGE(d != nullptr, name);
    return *d;
}

}  // namespace

TEST_CASE("WP-56 (R-66): a fully inlined nested lambda's static binds to its record two levels up when the code proves it") {
    // n1: planner_pairing_o2's nest_4 (another function of the file edited, f unchanged). main (910f203): `nested` had no
    // record where the planner looked, was new, and the WP-52 rule refused the reload (CML3207, "would be new in the patch").
    const Case c("n1_r66_other");
    CAPTURE(c.plan_errors.empty() ? std::string() : c.plan_errors[0].message);
    REQUIRE(c.planned);
    const SymbolDecision d = decision(c.plan, kNested);
    CAPTURE(d.reason);
    CHECK(d.decision == DecisionKind::BindOldData);
    CHECK(d.source == BindSource::CompilandRva);
    CHECK(d.old_address.rva == 0x3018);
    // Its guard (no record under clang-cl) is placed with it by the layout.
    CHECK(decision(c.plan, kNestedGuard).decision == DecisionKind::BindOldData);
    CHECK(decision(c.plan, kNestedGuard).source == BindSource::CompilandLayout);
    REQUIRE(c.plan.inlined_lambda_statics.size() == 1);
    CHECK(c.plan.inlined_lambda_statics[0].symbol == kNested);
    CHECK(c.plan.inlined_lambda_statics[0].display == kNestedDisplay);
    CHECK(c.plan.inlined_lambda_statics[0].proc == "f");
    CHECK(c.plan.inlined_lambda_statics[0].record_rva == 0x3018);
    // f is the old code under the plan and refers to `nested`: proven, no refusal.
    CHECK(c.proof.empty());
}

TEST_CASE("WP-56 (R-66): the record two levels up is refused where the function that holds it changed") {
    SUBCASE("n2: f itself edited") {
        const Case c("n2_r66_edited");
        REQUIRE(c.planned);
        CHECK(decision(c.plan, kNested).decision == DecisionKind::BindOldData);
        REQUIRE(c.plan.inlined_lambda_statics.size() == 1);
        CHECK(c.plan.inlined_lambda_statics[0].proc == "f");
        REQUIRE(c.proof.size() == 1);
        CAPTURE(c.proof[0].message);
        CHECK(c.proof[0].id() == "CML3207");
        CHECK(contains(c.proof[0].message, kNestedDisplay));
        CHECK(contains(c.proof[0].message, "the code of 'f' changed"));
    }
    SUBCASE("n3: R-61 two lambdas down (f's own `nested` deleted, `nested` declared in the inner lambda)") {
        // The record is f's own static's, which nothing in the records tells from an inlined lambda's: the code of f
        // changed (it no longer initialises its own), so the pairing is refused.
        const Case c("n3_r61_deep");
        REQUIRE(c.plan.inlined_lambda_statics.size() == 1);
        CHECK(c.plan.inlined_lambda_statics[0].symbol == kNested);
        REQUIRE(c.proof.size() == 1);
        CHECK(c.proof[0].id() == "CML3207");
        CHECK(contains(c.proof[0].message, "the code of 'f' changed"));
    }
}

// ---- the proof needs the procedure to refer to the static ------------------------------------------------------------
//
// A synthetic old image (tests/unit/pe) with a 16-byte procedure P at kOldP, and a patch object whose version of P has the same
// bytes. With `refers`, P is `mov ecx, 7; lea rax,[rip+S]; ret` and S is the listed static, bound to the record at kOldS;
// without, P is `mov ecx, 7; ret` and nothing in P names S. Both are unchanged under the plan; only the first proves that the
// record is S's.

namespace {

constexpr std::uint64_t kImageBase = 0x140000000ull;
constexpr std::uint32_t kOldP = 0x1100;
constexpr std::uint32_t kOldS = 0x3100;
const char* const kP = "?f@@YAXXZ";
const char* const kS = "?x@?1???R<lambda_1>@?0???R<lambda_0>@?0??f@@YAXXZ@QEBA@XZ@QEBA@XZ@4HA";

TargetAddress address(std::uint32_t rva) {
    TargetAddress t;
    t.valid = true;
    t.rva = rva;
    t.va = kImageBase + rva;
    t.module = "old.exe";
    return t;
}

std::vector<Diag> prove_synthetic(bool refers, bool hooked = true) {
    std::vector<std::uint8_t> code = {0xB9, 7, 0, 0, 0};  // mov ecx, 7
    if (refers) {
        const std::vector<std::uint8_t> lea = {0x48, 0x8D, 0x05, 0, 0, 0, 0};  // lea rax,[rip+S]
        code.insert(code.end(), lea.begin(), lea.end());
    }
    code.push_back(0xC3);
    while (code.size() < 16) {
        code.push_back(0xCC);
    }
    // The old image: P's bytes with the displacement to the old S.
    cml_test::PeBytes disk = cml_test::build_pe_on_disk();
    std::vector<std::uint8_t> old_code = code;
    if (refers) {
        const std::int32_t disp = static_cast<std::int32_t>(kOldS) - static_cast<std::int32_t>(kOldP + 5 + 3 + 4);
        std::memcpy(old_code.data() + 8, &disp, 4);
    }
    std::memcpy(disk.data() + cml_test::pe_layout::kTextRaw + (kOldP - cml_test::pe_layout::kTextRva), old_code.data(), old_code.size());
    // The patch: P (COMDAT) and S (.bss).
    cml_bind_test::SynthObj obj;
    const cml_bind_test::SynthObj::Func f = obj.function(kP, true, 16);
    coff::ModelSection& s = obj.model().sections[f.section - 1];
    for (std::size_t i = 0; i < code.size(); ++i) {
        s.data[i] = static_cast<std::byte>(code[i]);
    }
    const std::uint32_t bss = obj.section(".bss", cml_bind_test::kData, 0x10);
    const std::uint32_t sym = obj.symbol(kS, 0, static_cast<std::int32_t>(bss), false, false);
    if (refers) {
        obj.reloc(f.section, 8, sym, coff::RelocType::Rel32);
    }
    const cml_bind_test::Built built = obj.build();
    BindPlan plan;
    plan.object_name = "logic.cpp.obj";
    SymbolDecision fd;
    fd.symbol_index = f.symbol;
    fd.name = kP;
    fd.display = "f";
    fd.type = SymbolType::Function;
    fd.defined = true;
    fd.section = f.section;
    fd.decision = DecisionKind::PatchHooked;
    fd.old_address = address(kOldP);
    plan.decisions.push_back(fd);
    SymbolDecision sd;
    sd.symbol_index = sym;
    sd.name = kS;
    sd.display = "f::<lambda_0>::operator()::<lambda_1>::operator()::x";
    sd.type = SymbolType::Data;
    sd.defined = true;
    sd.is_static = true;
    sd.section = bss;
    sd.decision = DecisionKind::BindOldData;
    sd.old_address = address(kOldS);
    plan.decisions.push_back(sd);
    HookEntry h;
    h.new_symbol = kP;
    h.display = "f";
    h.old_va = kImageBase + kOldP;
    h.old_rva = kOldP;
    h.old_size = 16;
    if (hooked) {
        plan.hooks.push_back(h);
    }
    InlinedLambdaStatic l;
    l.symbol = kS;
    l.display = sd.display;
    l.record_rva = kOldS;
    l.proc = "f";
    l.proc_rva = kOldP;
    l.how = BindSource::CompilandRva;
    plan.inlined_lambda_statics.push_back(l);
    auto image = pe::Image::parse(cml_test::pe_span(disk), pe::ImageKind::OnDisk);
    REQUIRE_MESSAGE(image.has_value(), image.error().format());
    NarrowInput in;
    in.object = &built.obj();
    in.plan = &plan;
    in.old_image = &*image;
    in.load_base = kImageBase;
    in.is_hooked = [](std::uint64_t) { return false; };
    return prove_inlined_lambda_statics(in);
}

}  // namespace

TEST_CASE("WP-56: an unchanged procedure proves an inlined lambda's record only where it refers to the static") {
    // main (910f203): both were proven (the proof asked only whether the procedure was unchanged).
    CHECK(prove_synthetic(true).empty());
    const std::vector<Diag> out = prove_synthetic(false);
    REQUIRE(out.size() == 1);
    CAPTURE(out[0].message);
    CHECK(out[0].id() == "CML3207");
    CHECK(contains(out[0].message, "the patch's 'f' does not refer to the static"));
}

TEST_CASE("WP-56 (review of 6dc968a, finding 1): a procedure without a hook proves nothing, whatever its code") {
    // No hook of the plan has P's old entry: nothing compares P or shows that it refers to the static. CML3207. (WP-53's
    // verdict refuses it first, "is not paired with a function of the patch"; the refers-to check does not rely on that and
    // refuses it as well, "has no hook to show it refers to the static".)
    const std::vector<Diag> out = prove_synthetic(true, false);
    REQUIRE(out.size() == 1);
    CAPTURE(out[0].message);
    CHECK(out[0].id() == "CML3207");
    CHECK(contains(out[0].message, "'f' is not paired with a function of the patch"));
}
