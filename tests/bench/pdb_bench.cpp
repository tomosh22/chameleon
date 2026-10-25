// cml_pdb_bench: load-time and memory benchmark for cml_pdb on a (large) PDB.
//
//   cml_pdb_bench <file.pdb> [--min-size-mb N] [--slack S] [--threads N] [--smoke]
//
// Budgets (docs/IMPLEMENTATION_PLAN.md WP-04, ADR-0009):
//   load time   2 s for a 500 MB PDB, scaled linearly with the file size, times the CI slack S
//               (default 2.0, shared runners), with a floor of 100 ms for tiny files.
//               "Load" = open() + publics + every compiland's symbols (parallel load_all).
//   memory      at most 1.5 x the PDB size: bytes allocated by cml_pdb's own containers (peak) plus
//               the mapped file size (the whole mapping counts, resident or not).
// Exit code: 0 all assertions hold, 1 a budget or sanity check failed, 2 usage / cannot open.
// --smoke only checks that everything works (used by ctest on the tiny fixture PDB).
//
// The OS cache is warmed first by reading the file in 1 MiB chunks (reported separately): the
// budget is about parsing, not about a cold disk.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "cml/pdb/pdb.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

double mib(double bytes) {
    return bytes / (1024.0 * 1024.0);
}

struct Check {
    int failures = 0;
    void expect(bool ok, const char* what, const std::string& detail) {
        std::printf("[%s] %s: %s\n", ok ? "PASS" : "FAIL", what, detail.c_str());
        if (!ok) {
            ++failures;
        }
    }
};

std::string fmt(const char* f, double a, double b = 0, double c = 0) {
    char buf[256];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

#if defined(_WIN32)
void print_process_memory() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) {
        std::printf("process: peak working set %.1f MiB (includes every mapped page touched), peak pagefile %.1f MiB\n",
                    mib(static_cast<double>(pmc.PeakWorkingSetSize)), mib(static_cast<double>(pmc.PeakPagefileUsage)));
    }
}
#else
void print_process_memory() {}
#endif

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    double min_size_mb = 0;
    double slack = 2.0;
    unsigned threads = 0;
    bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--min-size-mb" && i + 1 < argc) {
            min_size_mb = std::atof(argv[++i]);
        } else if (a == "--slack" && i + 1 < argc) {
            slack = std::atof(argv[++i]);
        } else if (a == "--threads" && i + 1 < argc) {
            threads = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (a == "--smoke") {
            smoke = true;
        } else if (!a.empty() && a[0] != '-' && path.empty()) {
            path = a;
        } else {
            std::fprintf(stderr, "usage: cml_pdb_bench <file.pdb> [--min-size-mb N] [--slack S] [--threads N] [--smoke]\n");
            return 2;
        }
    }
    if (path.empty()) {
        std::fprintf(stderr, "usage: cml_pdb_bench <file.pdb> [--min-size-mb N] [--slack S] [--threads N] [--smoke]\n");
        return 2;
    }

    // ---- warm the OS cache -----------------------------------------------------------------
    std::uint64_t file_bytes = 0;
    double prefetch_ms = 0;
    {
        const auto t0 = Clock::now();
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "cannot open %s\n", path.c_str());
            return 2;
        }
        std::vector<char> chunk(1u << 20);
        while (in) {
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            file_bytes += static_cast<std::uint64_t>(in.gcount());
        }
        prefetch_ms = ms_since(t0);
    }
    const double size_mb = mib(static_cast<double>(file_bytes));
    std::printf("pdb: %s\nsize: %llu bytes (%.1f MiB)\nprefetch (not counted): %.0f ms\n", path.c_str(),
                static_cast<unsigned long long>(file_bytes), size_mb, prefetch_ms);

    // ---- load -------------------------------------------------------------------------------
    cml::pdb::reset_memory_peaks();
    cml::pdb::OpenOptions opts;
    opts.use_cache = false;
    opts.threads = threads;

    const auto t_open = Clock::now();
    auto opened = cml::pdb::PdbIndex::open_pdb(path, opts);
    const double open_ms = ms_since(t_open);
    if (!opened) {
        std::fprintf(stderr, "%s\n", opened.error().format().c_str());
        return 2;
    }
    const cml::pdb::PdbIndex& idx = **opened;

    const cml::pdb::MemoryStats after_open = cml::pdb::memory_stats();
    const auto t_pub = Clock::now();
    auto pub_ok = idx.load_publics();
    const double pub_ms = ms_since(t_pub);

    const cml::pdb::MemoryStats after_pub = cml::pdb::memory_stats();
    const auto t_all = Clock::now();
    auto all_ok = idx.load_all();
    const double all_ms = ms_since(t_all);

    const cml::pdb::MemoryStats mem = cml::pdb::memory_stats();
    const cml::pdb::Timings tm = idx.timings();
    const double load_ms = open_ms + pub_ms + all_ms;

    std::printf("\n== load-time breakdown (ms, wall) ==\n");
    std::printf("  open() total          %9.1f\n", open_ms);
    std::printf("    map                 %9.1f\n", tm.map_ms);
    std::printf("    msf directory       %9.1f\n", tm.msf_ms);
    std::printf("    dbi header          %9.1f\n", tm.dbi_ms);
    std::printf("    modules + heads     %9.1f\n", tm.modules_ms);
    std::printf("    files + inverse map %9.1f\n", tm.files_ms);
    std::printf("    sections + contribs %9.1f\n", tm.sections_ms);
    std::printf("  publics + globals     %9.1f\n", pub_ms);
    std::printf("  all compiland symbols %9.1f   (cpu sum across threads: %.1f)\n", all_ms, tm.module_symbols_ms);
    std::printf("  TOTAL                 %9.1f\n", load_ms);

    std::printf("\n== index contents ==\n");
    std::printf("  compilands %u, distinct files %zu, sections %zu, contributions %zu, publics %zu\n", idx.compiland_count(),
                idx.distinct_file_count(), idx.sections().size(), idx.contribution_count(), idx.public_count());

    std::printf("\n== memory ==\n");
    const double tracked_peak = static_cast<double>(mem.tracked_peak_bytes);
    const double mapped = static_cast<double>(mem.mapped_peak_bytes);
    const double total_mem = tracked_peak + mapped;
    std::printf("  index allocations (peak, counted) %9.1f MiB\n", mib(tracked_peak));
    std::printf("  index allocations (now)           %9.1f MiB   [after open %.1f, + publics %.1f, + all symbols %.1f]\n",
                mib(static_cast<double>(mem.tracked_bytes)), mib(static_cast<double>(after_open.tracked_bytes)),
                mib(static_cast<double>(after_pub.tracked_bytes)), mib(static_cast<double>(mem.tracked_bytes)));
    std::printf("  mapped file                       %9.1f MiB\n", mib(mapped));
    std::printf("  budget accounting (peak + mapped) %9.1f MiB = %.2f x PDB size\n", mib(total_mem),
                file_bytes > 0 ? total_mem / static_cast<double>(file_bytes) : 0.0);
    print_process_memory();

    // ---- lookups (sanity and a rough per-query cost) ---------------------------------------------
    Check check;
    check.expect(static_cast<bool>(pub_ok), "publics load", pub_ok ? "ok" : pub_ok.error().format());
    check.expect(static_cast<bool>(all_ok), "all compiland symbols load", all_ok ? "ok" : all_ok.error().format());
    check.expect(idx.compiland_count() > 0, "compilands present", std::to_string(idx.compiland_count()));
    check.expect(idx.public_count() > 0, "publics present", std::to_string(idx.public_count()));

    std::size_t looked_up = 0;
    std::size_t resolved = 0;
    std::size_t misses = 0;
    const auto t_q = Clock::now();
    const std::uint32_t step = idx.compiland_count() / 200 + 1;
    for (std::uint32_t c = 0; c < idx.compiland_count(); c += step) {
        auto syms = idx.compiland_symbols(c);
        if (!syms) {
            continue;
        }
        std::size_t taken = 0;
        const auto files = idx.compiland_files_normalized(c);
        const bool file_ok = files.empty() || !idx.compilands_for_file(files[0]).empty();
        for (const auto& s : *syms) {
            if (!cml::pdb::is_proc_kind(s.kind) || s.rva == cml::pdb::kNoRva || s.size == 0) {
                continue;
            }
            ++looked_up;
            const std::uint32_t target = s.rva + s.size / 2;
            auto p = idx.find_proc_by_rva(target);
            auto n = idx.find_in_compiland(c, s.name);
            // The proc found must contain the address (aliases and odd CRT routines may differ from `s`).
            const bool contains = p && p->has_value() && (*p)->rva <= target && target - (*p)->rva < (*p)->size;
            if (contains && n && !n->empty() && file_ok) {
                ++resolved;
            } else if (misses++ < 5) {
                std::printf("  miss: compiland %u proc '%.*s' rva 0x%X size %u\n", c, static_cast<int>(s.name.size()), s.name.data(),
                            s.rva, s.size);
            }
            if (++taken >= 25) {
                break;
            }
        }
    }
    const double q_ms = ms_since(t_q);
    // 99%: CRT assembly routines can nest or alias procs; everything the compiler produced must resolve.
    check.expect(looked_up > 0 && resolved * 100 >= looked_up * 99, "sampled lookups (proc by rva, by name, file map)",
                 std::to_string(resolved) + "/" + std::to_string(looked_up) + " resolved in " + fmt("%.1f ms", q_ms));

    // ---- budgets ------------------------------------------------------------------------------
    if (smoke) {
        std::printf("\n(smoke mode: budgets are not enforced)\n");
    } else {
        const double budget_ms = std::max(100.0, 2000.0 * (size_mb / 500.0) * slack);
        check.expect(load_ms <= budget_ms, "load time within budget",
                     fmt("%.0f ms <= %.0f ms (2 s per 500 MB x slack %.1f)", load_ms, budget_ms, slack));
        const double mem_budget = 1.5 * static_cast<double>(file_bytes);
        check.expect(total_mem <= mem_budget, "memory within budget",
                     fmt("%.1f MiB <= %.1f MiB (1.5 x PDB size)", mib(total_mem), mib(mem_budget)));
        if (min_size_mb > 0) {
            check.expect(size_mb >= min_size_mb, "PDB is big enough to be a meaningful benchmark",
                         fmt("%.1f MiB >= %.1f MiB", size_mb, min_size_mb));
        }
    }
    std::printf("\nresult: %s\n", check.failures == 0 ? "PASS" : "FAIL");
    return check.failures == 0 ? 0 : 1;
}
