// WP-57 spike driver (removed with spike/wp57/): copy into tests/unit/bind/ and apply spike.patch. Plans each case of $WP57_DIR
// against its old image (WP57_CASES), with WP-25's comparator under the plan on every hook, and prints the decisions.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "cml/coff/coff.h"
#include "cml/core/bind_plan.h"
#include "cml/core/hook_narrowing.h"
#include "cml/pdb/index.h"
#include "cml/pe/pe.h"
#include "doctest.h"

using namespace cml;
using namespace cml::core;
using namespace cml::core::bind;

namespace {
std::vector<std::byte> read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::transform(raw.begin(), raw.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}
}  // namespace

TEST_CASE("SPIKE WP-57") {
    const char* dir = std::getenv("WP57_DIR");
    const char* cases = std::getenv("WP57_CASES");
    if (dir == nullptr || cases == nullptr) {
        return;
    }
    std::istringstream list(cases);
    std::string n;
    while (list >> n) {
        const std::string base = std::string(dir) + "/" + n;
        std::printf("==== %s\n", n.c_str());
        auto p = pdb::PdbIndex::open_pdb(base + ".pdb");
        REQUIRE(p.has_value());
        std::vector<std::byte> bytes = read_all(base + ".patch.obj");
        auto o = coff::Object::parse(coff::ByteSpan(bytes.data(), bytes.size()));
        REQUIRE(o.has_value());
        auto img = pe::load_image_file(base + ".exe");
        REQUIRE(img.has_value());
        PlanInput in;
        in.object = &*o;
        in.object_name = n + ".patch.obj";
        in.module_pdb = p->get();
        in.module_name = n + ".exe";
        in.module_base = 0x140000000ull;
        in.compiland = n + ".old.obj";
        std::vector<Diag> errors;
        BindPlan partial;
        auto r = plan_bind(in, &errors, &partial);
        const BindPlan& plan = r ? *r : partial;
        std::printf("planned=%d\n", r.has_value() ? 1 : 0);
        for (const Diag& e : errors) {
            std::printf("  ERROR %s %s\n", e.id().c_str(), e.message.substr(0, 400).c_str());
        }
        for (const SymbolDecision& d : plan.decisions) {
            if (d.type != SymbolType::Function && d.defined && (d.name.find("@4") != std::string::npos || d.name.find("TSS") != std::string::npos || d.name.find("s_count") != std::string::npos)) {
                std::printf("  data %-60s %s %s rva=0x%X  (%s)\n", d.display.c_str(), to_string(d.decision), to_string(d.source), d.old_address.rva, d.reason.substr(0, 120).c_str());
            }
        }
        for (const SymbolDecision& d : plan.decisions) {
            if (d.type == SymbolType::Function && std::getenv("WP57_FUNCS") != nullptr) {
                std::printf("  func %-70s def=%d st=%d %s %s rva=0x%X  (%s)\n", d.display.substr(0, 70).c_str(), d.defined ? 1 : 0, d.is_static ? 1 : 0, to_string(d.decision), to_string(d.source),
                            d.old_address.rva, d.reason.substr(0, 160).c_str());
            }
        }
        for (const InlinedLambdaStatic& s : plan.inlined_lambda_statics) {
            std::printf("  wp53-listed %s rec=0x%X proc=%s\n", s.display.c_str(), s.record_rva, s.proc.c_str());
        }
        NarrowInput ni;
        ni.object = &*o;
        ni.plan = &plan;
        ni.old_image = &img->image;
        ni.load_base = in.module_base;
        ni.is_hooked = [](std::uint64_t) { return false; };
        const NarrowResult nr = narrow_hooks(ni);
        for (const NarrowDecision& d : nr.decisions) {
            std::printf("  hook %-50s %s %s\n", d.display.substr(0, 50).c_str(), to_string(d.reason), d.detail.substr(0, 200).c_str());
        }
        for (const Diag& d : prove_inlined_lambda_statics(ni)) {
            std::printf("  WP53-PROOF %s %s\n", d.id().c_str(), d.message.substr(0, 300).c_str());
        }
    }
}
