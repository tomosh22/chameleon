// The parameter records of fixture `crash_reexecute`'s PDB, built by whichever compiler this CI cell uses (cl or clang-cl), in Debug
// and RelWithDebInfo: step() (built with /Od in every configuration) must give a complete recipe, opt_step() (built with /O2) must be
// refused as optimised. These are the real records the broker's decision rests on; the printed WP23-RECORDS lines are the evidence.
#include <cstdio>
#include <string>

#include "cml/core/reexecute.h"
#include "cml/core/reexecute_diag_ids.h"
#include "cml/pdb/pdb.h"
#include <ostream>  // doctest streams string_view operands; MSVC needs the full definition

#include "doctest.h"

#if defined(CML_FIXTURE_CRASH_REEXECUTE_PDB) && defined(CML_FIXTURE_CRASH_REEXECUTE_EXE)

using namespace cml;
using namespace cml::core::reexecute;

namespace {

std::shared_ptr<const pdb::PdbIndex> open_fixture() {
    auto r = pdb::PdbIndex::open_pdb(CML_FIXTURE_CRASH_REEXECUTE_PDB);
    if (!r) {
        FAIL(r.error().format());
    }
    return *r;
}

struct Found {
    pdb::SymbolView proc;
    pdb::ProcInfo info;
    pdb::OptimizationHint hint = pdb::OptimizationHint::Unknown;
    std::string command;
};

Found find_function(const pdb::PdbIndex& index, const char* stem, const char* name) {
    const std::vector<std::uint32_t> compilands = index.compilands_by_object_stem(stem);
    REQUIRE_MESSAGE(compilands.size() == 1, "compilands for " << stem);
    auto symbols = index.find_in_compiland(compilands[0], name);
    REQUIRE_MESSAGE(symbols.has_value(), symbols.error().format());
    const pdb::SymbolView* proc = nullptr;
    for (const pdb::SymbolView& s : *symbols) {
        if (pdb::is_proc_kind(s.kind) && s.name == name) {
            proc = &s;
        }
    }
    REQUIRE_MESSAGE(proc != nullptr, "the proc " << name);
    Found f;
    f.proc = *proc;
    auto info = index.proc_info(*proc);
    REQUIRE_MESSAGE(info.has_value(), info.error().format());
    f.info = *info;
    auto env = index.build_env(compilands[0]);
    REQUIRE_MESSAGE(env.has_value(), env.error().format());
    f.hint = env->optimization();
    f.command = env->command() ? std::string(*env->command()) : std::string();
    return f;
}

void print_records(const char* what, const Found& f) {
    std::printf("WP23-RECORDS %s: rva=0x%X size=%u optimisation=%s frame_bytes=%u frame_flags=0x%X local_base=%u param_base=%u params_known=%d%s%s\n", what, f.info.rva,
                f.info.size, pdb::to_string(f.hint), f.info.frame_bytes, f.info.frame_flags, f.info.local_base_register, f.info.param_base_register,
                f.info.params_known ? 1 : 0, f.info.params_known ? "" : " error=", f.info.params_error.c_str());
    for (std::size_t i = 0; i < f.info.params.size(); ++i) {
        const pdb::ParamType& p = f.info.params[i];
        std::printf("WP23-RECORDS   param %zu: class=%d size=%u type=0x%X %s\n", i, static_cast<int>(p.cls), p.size, p.type_index, p.why.c_str());
    }
    for (const pdb::ProcVariable& v : f.info.variables) {
        for (const pdb::VarLocation& l : v.locations) {
            std::printf("WP23-RECORDS   var %s: param=%d regrel=%d type=0x%X kind=%d reg=%u offset=%d range=[0x%X,+0x%X]\n", v.name.c_str(), v.is_param ? 1 : 0,
                        v.from_regrel ? 1 : 0, v.type_index, static_cast<int>(l.kind), l.reg, l.offset, l.start_rva, l.length);
        }
    }
    std::printf("WP23-RECORDS   command: %.300s\n", f.command.c_str());
}

const pdb::ProcVariable* variable(const pdb::ProcInfo& info, const char* name) {
    for (const pdb::ProcVariable& v : info.variables) {
        if (v.name == name) {
            return &v;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("fixture crash_reexecute: step() records int*, int, double and a complete recipe") {
    const auto index = open_fixture();
    const Found f = find_function(*index, "step", "step");
    print_records("step", f);

    CHECK(f.hint == pdb::OptimizationHint::Disabled);
    REQUIRE_MESSAGE(f.info.params_known, f.info.params_error);
    REQUIRE(f.info.params.size() == 3);
    CHECK(f.info.params[0].cls == pdb::ParamClass::Int);
    CHECK(f.info.params[0].size == 8);
    CHECK(f.info.params[1].cls == pdb::ParamClass::Int);
    CHECK(f.info.params[1].size == 4);
    CHECK(f.info.params[2].cls == pdb::ParamClass::Float);
    CHECK(f.info.params[2].size == 8);
    REQUIRE(variable(f.info, "p") != nullptr);
    REQUIRE(variable(f.info, "k") != nullptr);
    REQUIRE(variable(f.info, "scale") != nullptr);

    // Emulate the frame: whichever register the records are relative to, put the home slot of position 0 where parameter p says it
    // is. (A real frame is checked by the scenario; this checks that the records describe consistent places.)
    ipc::ExceptionFrame frame;
    frame.entry_rsp = 0x100000;
    const pdb::ProcVariable* p = variable(f.info, "p");
    const pdb::VarLocation& where = p->locations.front();
    const std::uint16_t reg = where.kind == pdb::VarLocationKind::FramePointerRelative ? f.info.param_base_register : where.reg;
    const std::uint64_t base = frame.entry_rsp + 8 - static_cast<std::int64_t>(where.offset);  // the register value that puts p at the home slot of position 0
    if (p->from_regrel) {
        // MSVC /Od: the parameters are spilled to the home space.
        (reg == pdb::kCvRegRbp ? frame.rbp : reg == pdb::kCvRegR13 ? frame.r13 : frame.rsp) = base;
    }
    const std::uint32_t rva = f.info.rva + f.info.size / 2;
    const RecipeResult r = build_recipe(f.info, f.hint, rva, frame);
    REQUIRE_MESSAGE(r.ok, r.reason);
    REQUIRE(r.params.size() == 3);
    CHECK(r.params[0].is_float == 0);
    CHECK(r.params[0].size == 8);
    CHECK(r.params[1].is_float == 0);
    CHECK(r.params[1].size == 4);
    CHECK(r.params[2].is_float == 1);
    CHECK(r.params[2].size == 8);
    for (const ipc::ReexecuteParam& param : r.params) {
        CHECK((param.base_register == pdb::kCvRegRsp || param.base_register == pdb::kCvRegRbp || param.base_register == pdb::kCvRegR13));
    }
    if (p->from_regrel) {
        // Home space: the three parameters are 8 bytes apart.
        CHECK(r.params[1].offset == r.params[0].offset + 8);
        CHECK(r.params[2].offset == r.params[0].offset + 16);
    } else {
        // clang-cl: its own locals - three distinct places, none of them overlapping.
        CHECK(r.params[0].offset != r.params[1].offset);
        CHECK(r.params[1].offset != r.params[2].offset);
        CHECK(r.params[0].offset != r.params[2].offset);
    }
}

TEST_CASE("fixture crash_reexecute: opt_step() is refused as optimised") {
    const auto index = open_fixture();
    const Found f = find_function(*index, "opt", "opt_step");
    print_records("opt_step", f);
    CHECK(f.hint != pdb::OptimizationHint::Disabled);
    CHECK(f.hint != pdb::OptimizationHint::Unknown);
    ipc::ExceptionFrame frame;
    frame.entry_rsp = 0x100000;
    const RecipeResult r = build_recipe(f.info, f.hint, f.info.rva + 1, frame);
    CHECK_FALSE(r.ok);
    CHECK(r.diag == diag::kOptimised);
    CHECK(r.reason.find("optimi") != std::string::npos);
}

#endif
