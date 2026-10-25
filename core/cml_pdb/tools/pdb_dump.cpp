// cml_pdb_dump: prints what cml_pdb indexes for a PDB (or for the PDB of a PE image).
//
//   cml_pdb_dump <file.pdb | image.exe|dll> [options]
//     --summary            identity, sizes, compiland count, timings (default)
//     --compilands         one line per compiland (object, library, compiler, env source)
//     --compiland <stem>   full dump of the compilands whose object stem matches: compile info,
//                          build environment, files, and every symbol with its RVA and the public
//                          symbol found at that RVA (evidence for static/template naming)
//     --publics <substr>   publics whose name contains <substr> (empty string: all)
//     --file <path>        compilands that include <path>
//     --rva <hex>          section:offset, contributing compiland, containing proc, public at RVA
//
// The input is treated as a PE image when it starts with "MZ". Output goes to stdout; diagnostics
// to stderr. Exit code 0 on success, 1 on a CML diagnostic, 2 on bad usage.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cml/pdb/pdb.h"

namespace {

using cml::pdb::PdbIndex;

int fail(const cml::Diag& d) {
    std::fprintf(stderr, "%s\n", d.format().c_str());
    return 1;
}

std::string sv(std::string_view v) {
    return std::string(v);
}

bool is_pe(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    char sig[2] = {0, 0};
    const std::size_t n = std::fread(sig, 1, 2, f);
    std::fclose(f);
    return n == 2 && sig[0] == 'M' && sig[1] == 'Z';
}

void print_compile(const cml::pdb::Compiland& c) {
    if (c.has_compile3) {
        const auto& ci = c.compile;
        std::printf("  compile3: language=%s machine=0x%X frontend=%u.%u.%u.%u backend=%u.%u.%u.%u\n", ci.language_name(),
                    static_cast<unsigned>(ci.machine), ci.frontend[0], ci.frontend[1], ci.frontend[2], ci.frontend[3],
                    ci.backend[0], ci.backend[1], ci.backend[2], ci.backend[3]);
        std::printf("            version=\"%s\"\n", sv(ci.version).c_str());
        std::printf("            flags=0x%08X ltcg(/GL)=%d hotpatch=%d edit_and_continue=%d security_checks(/GS)=%d sdl=%d pgo=%d no_debug_info=%d\n",
                    ci.raw_flags, ci.ltcg, ci.hotpatch, ci.edit_and_continue, ci.security_checks, ci.sdl, ci.pgo,
                    ci.no_debug_info);
    } else {
        std::printf("  compile3: (none)\n");
    }
}

void print_env(const PdbIndex& idx, const cml::pdb::Compiland& c) {
    std::printf("  S_ENVBLOCK: %s\n", c.has_env_block() ? "present" : "absent");
    if (c.has_env_block()) {
        for (const auto& kv : c.env) {
            std::printf("    %s = %s\n", sv(kv.first).c_str(), sv(kv.second).c_str());
        }
    }
    auto be = idx.build_env(c.index);
    if (!be) {
        std::printf("  build_env: error %s\n", be.error().format().c_str());
        return;
    }
    std::printf("  build_env source: %s optimisation-hint: %s\n", cml::pdb::to_string(be->source),
                cml::pdb::to_string(be->optimization()));
    if (be->source == cml::pdb::EnvSource::BuildInfo) {
        for (const auto& kv : be->pairs) {
            std::printf("    %s = %s\n", kv.first.c_str(), kv.second.c_str());
        }
    }
}

void dump_compiland(const PdbIndex& idx, std::uint32_t i) {
    const auto& c = idx.compiland(i);
    std::printf("== compiland %u: %s\n", i, sv(c.object_name).c_str());
    if (!c.library_name.empty()) {
        std::printf("  library: %s\n", sv(c.library_name).c_str());
    }
    std::printf("  S_OBJNAME: %s\n", sv(c.objname_record).c_str());
    print_compile(c);
    print_env(idx, c);
    std::printf("  files (%zu):\n", idx.compiland_files(i).size());
    const auto raw = idx.compiland_files(i);
    const auto norm = idx.compiland_files_normalized(i);
    for (std::size_t k = 0; k < raw.size(); ++k) {
        std::printf("    %s  ->  %s\n", sv(raw[k]).c_str(), sv(norm[k]).c_str());
    }
    auto syms = idx.compiland_symbols(i);
    if (!syms) {
        std::printf("  symbols: error %s\n", syms.error().format().c_str());
        return;
    }
    std::printf("  symbols (%zu): kind | name | section:offset | rva | size | parent | public-at-rva\n", syms->size());
    for (const auto& s : *syms) {
        std::string pub = "-";
        if (s.rva != cml::pdb::kNoRva) {
            auto pubs = idx.publics_at_rva(s.rva);
            if (pubs && !pubs->empty()) {
                pub.clear();
                for (const auto& p : *pubs) {
                    if (!pub.empty()) {
                        pub += " | ";
                    }
                    pub += sv(p.name);
                }
            }
        }
        std::printf("    %-9s | %s | %04X:%08X | %s | %u | %s | %s\n", cml::pdb::to_string(s.kind), sv(s.name).c_str(),
                    static_cast<unsigned>(s.section), s.offset,
                    s.rva == cml::pdb::kNoRva ? "-" : std::to_string(s.rva).c_str(), s.size,
                    s.parent_name.empty() ? "-" : sv(s.parent_name).c_str(), pub.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: cml_pdb_dump <file.pdb|image> [--summary|--compilands|--compiland <stem>|--publics <substr>|--file <path>|--rva <hex>]\n");
        return 2;
    }
    const std::string input = argv[1];
    cml::pdb::OpenOptions opts;
    cml::Result<std::shared_ptr<const PdbIndex>> opened = is_pe(input) ? PdbIndex::open_for_pe(input, opts) : PdbIndex::open_pdb(input, opts);
    if (!opened) {
        return fail(opened.error());
    }
    const PdbIndex& idx = **opened;
    std::string mode = argc >= 3 ? argv[2] : "--summary";
    const std::string arg = argc >= 4 ? argv[3] : "";

    if (mode == "--summary") {
        std::printf("pdb: %s\nid: %s\nmachine: 0x%X dbi_age: %u size: %llu bytes\n", idx.path().c_str(), idx.id().to_string().c_str(),
                    static_cast<unsigned>(idx.machine()), idx.dbi_age(), static_cast<unsigned long long>(idx.file_size()));
        std::printf("compilands: %u distinct_files: %zu sections: %zu contributions: %zu\n", idx.compiland_count(),
                    idx.distinct_file_count(), idx.sections().size(), idx.contribution_count());
        const auto t = idx.timings();
        std::printf("open: %.2f ms (map %.2f msf %.2f dbi %.2f modules %.2f files %.2f sections %.2f)\n", t.open_total_ms, t.map_ms,
                    t.msf_ms, t.dbi_ms, t.modules_ms, t.files_ms, t.sections_ms);
        return 0;
    }
    if (mode == "--compilands") {
        for (std::uint32_t i = 0; i < idx.compiland_count(); ++i) {
            const auto& c = idx.compiland(i);
            std::printf("%u | %s | lib=%s | compile3=%s %s | env=%s | files=%u\n", i, sv(c.object_name).c_str(),
                        c.library_name.empty() ? "-" : sv(c.library_name).c_str(), c.has_compile3 ? c.compile.language_name() : "-",
                        c.has_compile3 ? sv(c.compile.version).c_str() : "", c.has_env_block() ? "S_ENVBLOCK" : "no", c.file_count);
        }
        return 0;
    }
    if (mode == "--compiland") {
        const auto hits = idx.compilands_by_object_stem(arg);
        if (hits.empty()) {
            std::fprintf(stderr, "no compiland with object stem '%s'\n", arg.c_str());
            return 1;
        }
        for (const std::uint32_t i : hits) {
            dump_compiland(idx, i);
        }
        return 0;
    }
    if (mode == "--publics") {
        auto pubs = idx.all_publics();
        if (!pubs) {
            return fail(pubs.error());
        }
        for (const auto& p : *pubs) {
            if (arg.empty() || sv(p.name).find(arg) != std::string::npos) {
                std::printf("%04X:%08X rva=%s flags=0x%X %s\n", static_cast<unsigned>(p.section), p.offset,
                            p.rva == cml::pdb::kNoRva ? "-" : std::to_string(p.rva).c_str(), p.flags, sv(p.name).c_str());
            }
        }
        return 0;
    }
    if (mode == "--file") {
        for (const std::uint32_t i : idx.compilands_for_file(arg)) {
            std::printf("%u | %s\n", i, sv(idx.compiland(i).object_name).c_str());
        }
        return 0;
    }
    if (mode == "--rva") {
        const std::uint32_t rva = static_cast<std::uint32_t>(std::strtoul(arg.c_str(), nullptr, 16));
        if (const auto so = idx.section_offset_from_rva(rva)) {
            std::printf("section:offset %04X:%08X\n", static_cast<unsigned>(so->section), so->offset);
        }
        if (const auto c = idx.compiland_for_rva(rva)) {
            std::printf("contributing compiland %u: %s\n", *c, sv(idx.compiland(*c).object_name).c_str());
        }
        auto proc = idx.find_proc_by_rva(rva);
        if (!proc) {
            return fail(proc.error());
        }
        if (*proc) {
            std::printf("proc: %s (+0x%X of %u)\n", sv((*proc)->name).c_str(), rva - (*proc)->rva, (*proc)->size);
        }
        auto pubs = idx.publics_at_rva(rva);
        if (!pubs) {
            return fail(pubs.error());
        }
        for (const auto& p : *pubs) {
            std::printf("public: %s\n", sv(p.name).c_str());
        }
        return 0;
    }
    std::fprintf(stderr, "unknown option '%s'\n", mode.c_str());
    return 2;
}
