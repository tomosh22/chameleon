// The recipe builder (broker/cml_core/src/reexecute.cpp) on ProcInfo records shaped like what cl and clang-cl write. Portable.
#include <string>
#include <vector>

#include "cml/core/reexecute.h"
#include "cml/core/reexecute_diag_ids.h"
#include "doctest.h"

using namespace cml;
using namespace cml::core::reexecute;

namespace {

pdb::ParamType int_param(std::uint8_t size, std::uint32_t type = 0x74) {
    pdb::ParamType p;
    p.cls = pdb::ParamClass::Int;
    p.size = size;
    p.type_index = type;
    return p;
}

pdb::ParamType float_param(std::uint8_t size, std::uint32_t type = 0x41) {
    pdb::ParamType p;
    p.cls = pdb::ParamClass::Float;
    p.size = size;
    p.type_index = type;
    return p;
}

pdb::VarLocation rsp_rel(std::int32_t offset) {
    pdb::VarLocation l;
    l.kind = pdb::VarLocationKind::RegisterRelative;
    l.reg = pdb::kCvRegRsp;
    l.offset = offset;
    return l;
}

// step(int* p, int k, double scale) the way MSVC /Od records it: S_REGREL32 [rsp+N] for parameters and locals alike.
pdb::ProcInfo msvc_step() {
    pdb::ProcInfo info;
    info.name = "step";
    info.rva = 0x1000;
    info.size = 0x60;
    info.has_frameproc = true;
    info.param_base_register = pdb::kCvRegRsp;
    info.params_known = true;
    info.params = {int_param(8, 0x674), int_param(4, 0x74), float_param(8, 0x41)};
    // The frame: entry RSP 0x1000, the body RSP is 0x1000 - 0x28 = 0xFD8 (a 0x28 byte frame). Home slots: 0x1008, 0x1010, 0x1018
    // -> offsets 0x30, 0x38, 0x40 from the body RSP. A local at [rsp+0x20] is not a parameter.
    const struct { const char* name; std::int32_t off; std::uint32_t type; } vars[] = {
        {"v", 0x20, 0x74}, {"scale", 0x40, 0x41}, {"k", 0x38, 0x74}, {"p", 0x30, 0x674}};
    for (const auto& v : vars) {
        pdb::ProcVariable var;
        var.name = v.name;
        var.type_index = v.type;
        var.from_regrel = true;
        var.locations.push_back(rsp_rel(v.off));
        info.variables.push_back(var);
    }
    return info;
}

ipc::ExceptionFrame msvc_frame() {
    ipc::ExceptionFrame f;
    f.rip = 0x7FF600001020ull;
    f.entry_rsp = 0x1000;
    f.rsp = 0x1000 - 0x28;
    return f;
}

// The same function the way clang-cl records it: S_LOCAL (parameter flag) + S_DEFRANGE_FRAMEPOINTER_REL_FULL_SCOPE relative to RBP.
pdb::ProcInfo clang_step() {
    pdb::ProcInfo info;
    info.name = "step";
    info.rva = 0x1000;
    info.size = 0x60;
    info.has_frameproc = true;
    info.param_base_register = pdb::kCvRegRbp;
    info.params_known = true;
    info.params = {int_param(8, 0x674), int_param(4, 0x74), float_param(8, 0x41)};
    const struct { const char* name; std::int32_t off; std::uint32_t type; bool param; } vars[] = {
        {"scale", -24, 0x41, true}, {"k", -12, 0x74, true}, {"p", -8, 0x674, true}, {"v", -36, 0x74, false}};
    // clang writes the S_LOCAL records of the parameters in declaration order, then the locals.
    const char* order[] = {"p", "k", "scale", "v"};
    for (const char* wanted : order) {
        for (const auto& v : vars) {
            if (std::string(v.name) != wanted) {
                continue;
            }
            pdb::ProcVariable var;
            var.name = v.name;
            var.type_index = v.type;
            var.is_param = v.param;
            pdb::VarLocation l;
            l.kind = pdb::VarLocationKind::FramePointerRelative;
            l.offset = v.off;
            var.locations.push_back(l);
            info.variables.push_back(var);
        }
    }
    return info;
}

}  // namespace

TEST_CASE("recipe: MSVC /Od shape - parameters are found at their home slots") {
    const RecipeResult r = build_recipe(msvc_step(), pdb::OptimizationHint::Disabled, 0x20, msvc_frame());
    REQUIRE_MESSAGE(r.ok, r.reason);
    REQUIRE(r.params.size() == 3);
    CHECK(r.params[0].position == 0);
    CHECK(r.params[0].is_float == 0);
    CHECK(r.params[0].size == 8);
    CHECK(r.params[0].base_register == pdb::kCvRegRsp);
    CHECK(r.params[0].offset == 0x30);
    CHECK(r.params[1].position == 1);
    CHECK(r.params[1].size == 4);
    CHECK(r.params[1].offset == 0x38);
    CHECK(r.params[2].position == 2);
    CHECK(r.params[2].is_float == 1);
    CHECK(r.params[2].size == 8);
    CHECK(r.params[2].offset == 0x40);
}

TEST_CASE("recipe: clang-cl shape - S_LOCAL parameters with frame-pointer-relative locations") {
    ipc::ExceptionFrame frame = msvc_frame();
    const RecipeResult r = build_recipe(clang_step(), pdb::OptimizationHint::Disabled, 0x20, frame);
    REQUIRE_MESSAGE(r.ok, r.reason);
    REQUIRE(r.params.size() == 3);
    CHECK(r.params[0].base_register == pdb::kCvRegRbp);
    CHECK(r.params[0].offset == -8);
    CHECK(r.params[0].size == 8);
    CHECK(r.params[1].offset == -12);
    CHECK(r.params[1].size == 4);
    CHECK(r.params[2].offset == -24);
    CHECK(r.params[2].is_float == 1);
}

TEST_CASE("recipe: only code built without optimisation is accepted, and an unknown level is not guessed") {
    for (const pdb::OptimizationHint hint : {pdb::OptimizationHint::Speed, pdb::OptimizationHint::Size, pdb::OptimizationHint::Full}) {
        const RecipeResult r = build_recipe(msvc_step(), hint, 0x20, msvc_frame());
        CHECK_FALSE(r.ok);
        CHECK(r.diag == diag::kOptimised);
        CHECK(r.reason.find("CML5901") == 0);
        CHECK(r.reason.find("optimi") != std::string::npos);
    }
    const RecipeResult unknown = build_recipe(msvc_step(), pdb::OptimizationHint::Unknown, 0x20, msvc_frame());
    CHECK_FALSE(unknown.ok);
    CHECK(unknown.diag == diag::kOptimisationUnknown);
    // (S_FRAMEPROC's "optimised for speed" bit is set by cl even at /Od - CI evidence - so it is not consulted.)
}

TEST_CASE("recipe: a function whose parameters cannot be read, or that shifts the arguments, is refused with the reason") {
    pdb::ProcInfo unknown = msvc_step();
    unknown.params_known = false;
    unknown.params_error = "the function returns a class, struct or union by value";
    RecipeResult r = build_recipe(unknown, pdb::OptimizationHint::Disabled, 0x20, msvc_frame());
    CHECK_FALSE(r.ok);
    CHECK(r.diag == diag::kSignatureUnsupported);
    CHECK(r.reason.find("returns a class") != std::string::npos);

    pdb::ProcInfo aggregate = msvc_step();
    aggregate.params[1] = pdb::ParamType();
    aggregate.params[1].why = "passed by value as a class, struct or union (type 0x1234)";
    r = build_recipe(aggregate, pdb::OptimizationHint::Disabled, 0x20, msvc_frame());
    CHECK_FALSE(r.ok);
    CHECK(r.diag == diag::kParamUnsupported);
    CHECK(r.reason.find("parameter 2") != std::string::npos);
    CHECK(r.reason.find("by value") != std::string::npos);
}

TEST_CASE("recipe: parameters beyond the fourth stay in the caller's frame and need no record") {
    pdb::ProcInfo info = msvc_step();
    info.params.push_back(int_param(4));       // position 4: the fourth register argument (home slot 0x48 from the body RSP)
    pdb::ProcVariable fourth;
    fourth.name = "j";
    fourth.type_index = 0x74;
    fourth.from_regrel = true;
    fourth.locations.push_back(rsp_rel(0x48));
    info.variables.push_back(fourth);
    info.params.push_back(int_param(4));       // position 5: on the stack, not reloaded
    info.params.push_back(pdb::ParamType());   // position 6: an aggregate, not our business
    const RecipeResult r = build_recipe(info, pdb::OptimizationHint::Disabled, 0x20, msvc_frame());
    REQUIRE_MESSAGE(r.ok, r.reason);
    CHECK(r.params.size() == 4);
    CHECK(r.params[3].position == 3);
    CHECK(r.params[3].offset == 0x48);
}

TEST_CASE("recipe: a location that is unknown, ambiguous or in a register is refused") {
    // MSVC shape without a record at the second home slot.
    pdb::ProcInfo missing = msvc_step();
    missing.variables.erase(missing.variables.begin() + 2);  // "k"
    RecipeResult r = build_recipe(missing, pdb::OptimizationHint::Disabled, 0x20, msvc_frame());
    CHECK_FALSE(r.ok);
    CHECK(r.diag == diag::kLocationUnknown);
    CHECK(r.reason.find("home slot") != std::string::npos);

    // A frame whose RSP does not match any record (the broker saw a different frame than the PDB describes).
    ipc::ExceptionFrame elsewhere = msvc_frame();
    elsewhere.rsp -= 0x100;
    CHECK(build_recipe(msvc_step(), pdb::OptimizationHint::Disabled, 0x20, elsewhere).diag == diag::kLocationUnknown);

    // clang shape: the value lives in a register at the fault.
    pdb::ProcInfo in_register = clang_step();
    in_register.variables[1].locations[0].kind = pdb::VarLocationKind::Register;
    in_register.variables[1].locations[0].reg = 18;
    r = build_recipe(in_register, pdb::OptimizationHint::Disabled, 0x20, msvc_frame());
    CHECK_FALSE(r.ok);
    CHECK(r.diag == diag::kLocationUnknown);
    CHECK(r.reason.find("register") != std::string::npos);

    // clang shape: a range that does not cover the faulting instruction.
    pdb::ProcInfo late = clang_step();
    late.variables[0].locations[0].start_rva = 0x1040;
    late.variables[0].locations[0].length = 0x10;
    CHECK(build_recipe(late, pdb::OptimizationHint::Disabled, 0x1020, msvc_frame()).diag == diag::kLocationUnknown);
    late.variables[0].locations[0].start_rva = 0x1010;
    CHECK(build_recipe(late, pdb::OptimizationHint::Disabled, 0x1020, msvc_frame()).diag == diag::kLocationUnknown);  // 0x1010 + 0x10 = 0x1020: end is exclusive
    late.variables[0].locations[0].length = 0x11;
    CHECK(build_recipe(late, pdb::OptimizationHint::Disabled, 0x1020, msvc_frame()).ok);

    // clang shape: an unnamed parameter has no S_LOCAL, so the records cannot be matched to positions.
    pdb::ProcInfo unnamed = clang_step();
    unnamed.variables.erase(unnamed.variables.begin() + 1);
    r = build_recipe(unnamed, pdb::OptimizationHint::Disabled, 0x20, msvc_frame());
    CHECK_FALSE(r.ok);
    CHECK(r.diag == diag::kLocationUnknown);
    CHECK(r.reason.find("unnamed") != std::string::npos);

    // clang shape without a frame-pointer register in S_FRAMEPROC.
    pdb::ProcInfo no_base = clang_step();
    no_base.param_base_register = 0;
    CHECK(build_recipe(no_base, pdb::OptimizationHint::Disabled, 0x20, msvc_frame()).diag == diag::kLocationUnknown);
}

TEST_CASE("recipe: a function without parameters is re-executable with an empty recipe") {
    pdb::ProcInfo none;
    none.name = "tick";
    none.params_known = true;
    const RecipeResult r = build_recipe(none, pdb::OptimizationHint::Disabled, 0, msvc_frame());
    CHECK(r.ok);
    CHECK(r.params.empty());
}
