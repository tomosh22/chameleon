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
    return 0;
}
