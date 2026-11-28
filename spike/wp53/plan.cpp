// WP-53 spike driver: plan_bind of a patch object against a linked PDB (Linux pre-check, not shipped).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cml/coff/coff.h"
#include "cml/core/bind_plan.h"
#include "cml/core/hook_narrowing.h"
#include "cml/pe/pe.h"
#include "cml/pdb/index.h"
#include "cml/util/log.h"

using namespace cml;

static void sink(void*, LogLevel, const char*, int, const char* m) {
    if (std::strstr(m, "WP") != nullptr || std::getenv("ALL") != nullptr) {
        std::printf("  log: %s\n", m);
    }
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: plan <pdb> <obj> <compiland>\n");
        return 2;
    }
    set_log_level(LogLevel::Trace);
    set_log_sink(sink, nullptr);
    auto pdb = pdb::PdbIndex::open_pdb(argv[1]);
    if (!pdb) {
        std::fprintf(stderr, "pdb: %s\n", pdb.error().message.c_str());
        return 1;
    }
    std::ifstream f(argv[2], std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto obj = coff::Object::parse(coff::ByteSpan(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
    if (!obj) {
        std::fprintf(stderr, "obj: %s\n", obj.error().message.c_str());
        return 1;
    }
    // What the planner reads: every record of the compiland (procedures, data with their parent) and each procedure's inlinees.
    std::vector<std::uint32_t> cis = (*pdb)->compilands_by_object_name(argv[3]);
    if (cis.empty()) {
        cis = (*pdb)->compilands_by_object_stem(argv[3]);
    }
    std::printf("compilands named '%s': %zu\n", argv[3], cis.size());
    for (const std::uint32_t ci : cis) {
        (void)(*pdb)->load_compiland(ci);
        auto syms = (*pdb)->compiland_symbols(ci);
        if (!syms) {
            continue;
        }
        for (const pdb::SymbolView& v : *syms) {
            if (!pdb::is_proc_kind(v.kind) && !pdb::is_data_kind(v.kind)) {
                continue;
            }
            std::printf("  rec ci=%u #%u %s '%.*s' rva=%x type=%x parent='%.*s'\n", ci, v.index, pdb::is_proc_kind(v.kind) ? "proc" : "data", static_cast<int>(v.name.size()),
                        v.name.data(), v.rva, v.type_index, static_cast<int>(v.parent_name.size()), v.parent_name.data());
            if (pdb::is_proc_kind(v.kind)) {
                auto sites = (*pdb)->proc_inlinees(v);
                for (std::size_t k = 0; sites && k < sites->inlinees.size(); ++k) {
                    const auto& e = sites->inlinees[k];
                    std::printf("      inlinee '%s' class='%s' unique='%s' sig='%s' sites=%u depth=%u\n", e.name.c_str(), e.class_name.c_str(), e.class_unique.c_str(), e.signature.c_str(),
                                e.sites, e.depth);
                }
            }
        }
    }
    core::bind::PlanInput in;
    in.object = &*obj;
    in.object_name = argv[2];
    in.module_pdb = pdb->get();
    in.module_name = "spike.exe";
    in.module_base = 0x140000000ull;
    in.compiland = argv[3];
    std::vector<Diag> errors;
    core::bind::BindPlan partial;
    auto r = core::bind::plan_bind(in, &errors, &partial);
    const core::bind::BindPlan& p = r ? *r : partial;
    for (const auto& d : p.decisions) {
        if (d.type != core::bind::SymbolType::Data || !d.defined) {
            continue;
        }
        std::printf("%-60s %-14s %-22s rva=%x  %s\n", d.name.c_str(), core::bind::to_string(d.decision), core::bind::to_string(d.source), d.old_address.rva, d.reason.c_str());
    }
    for (const Diag& e : errors) {
        std::printf("ERROR CML%u %s\n", e.code, e.message.c_str());
    }
    for (const Diag& e : p.warnings) {
        std::printf("WARN CML%u %s\n", e.code, e.message.c_str());
    }
    std::printf("plan: %s\n", r ? "accepted" : "refused");
    // The WP-25 comparator on every hook of the plan (argv[4]: the old image): is the function's code the old one's under
    // the plan's decisions?
    if (argc > 4) {
        auto image = pe::load_image_file(argv[4]);
        if (!image) {
            std::printf("image: %s\n", image.error().message.c_str());
            return 0;
        }
        core::NarrowInput ni;
        ni.object = &*obj;
        ni.plan = &p;
        ni.old_image = &image->image;
        ni.load_base = in.module_base;
        ni.is_hooked = [](std::uint64_t) { return false; };
        const core::NarrowResult n = core::narrow_hooks(ni);
        for (const core::NarrowDecision& d : n.decisions) {
            std::printf("code %-40s %s %s\n", d.display.c_str(), core::to_string(d.reason), d.detail.c_str());
        }
    }
    return 0;
}
