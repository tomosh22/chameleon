// cml_broker_mem: the broker's memory on a large PDB (WP-27).
//
//   cml_broker_mem <file.pdb> [--factor F] [--json FILE] [--timeout-s N] [--smoke]
//
// Budget (plan WP-27): broker memory at most 1.5 x the PDB size. Measured here on the broker's own code path: the
// ModuleIndex the broker creates when it enables a module (PDB open with publics, file lists of every compiland, build
// environment of every compiland, source-hash baseline), then what a reload does with it: a sweep (which maps the PDB
// again after a release, ADR-0017), release_pdb(), and a second sweep. The measure is the PEAK PRIVATE BYTES of this
// process (peak commit charge, PeakPagefileUsage); the file mapping of the PDB is file-backed, not private, and is
// reported separately. This process hosts nothing but the index, so the figure is the index's cost plus the runtime
// baseline (a few MiB).
//
// What it does NOT include: a reload of a translation unit in that PDB. The benchmark project is a plain program with no
// agent, so there is no process to patch; the compile/link/load memory of a reload is small next to the index and is
// covered by the broker's private bytes after the five reloads of the `perf` scenario (perf-reload.json).
//
// Exit code: 0 within budget, 1 over budget or the index failed, 2 usage / cannot open. --smoke prints but does not enforce.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

#include "cml/core/module_index.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

namespace {

double mib(double bytes) {
    return bytes / (1024.0 * 1024.0);
}

struct Memory {
    double peak_private = 0;
    double private_now = 0;
    double peak_working_set = 0;
};

Memory read_memory() {
    Memory m;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) {
        m.peak_private = static_cast<double>(pmc.PeakPagefileUsage);
        m.private_now = static_cast<double>(pmc.PrivateUsage);
        m.peak_working_set = static_cast<double>(pmc.PeakWorkingSetSize);
    }
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    std::string json_path;
    double factor = 1.5;
    unsigned timeout_s = 600;
    bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--factor" && i + 1 < argc) {
            factor = std::atof(argv[++i]);
        } else if (a == "--json" && i + 1 < argc) {
            json_path = argv[++i];
        } else if (a == "--timeout-s" && i + 1 < argc) {
            timeout_s = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (a == "--smoke") {
            smoke = true;
        } else if (!a.empty() && a[0] != '-' && path.empty()) {
            path = a;
        } else {
            std::fprintf(stderr, "usage: cml_broker_mem <file.pdb> [--factor F] [--json FILE] [--timeout-s N] [--smoke]\n");
            return 2;
        }
    }
    if (path.empty()) {
        std::fprintf(stderr, "usage: cml_broker_mem <file.pdb> [--factor F] [--json FILE] [--timeout-s N] [--smoke]\n");
        return 2;
    }
    std::uint64_t file_bytes = 0;
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            std::fprintf(stderr, "cannot open %s\n", path.c_str());
            return 2;
        }
        file_bytes = static_cast<std::uint64_t>(in.tellg());
    }
    std::printf("pdb: %s\nsize: %llu bytes (%.1f MiB)\n", path.c_str(), static_cast<unsigned long long>(file_bytes),
                mib(static_cast<double>(file_bytes)));
    const Memory before = read_memory();

    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    cml::core::ModuleIndexSpec spec;
    spec.module_path = path;
    spec.pdb_path = path;
    spec.scan_includes = false;  // the project's sources may not be on this machine; the PDB's file lists are what is indexed
    std::shared_ptr<cml::core::ModuleIndex> index = cml::core::ModuleIndex::start(std::move(spec));
    const bool ready = index->wait(timeout_s * 1000u);
    const double index_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    const cml::core::IndexStatus status = index->status();
    if (!ready || status.state != cml::core::IndexState::Ready) {
        std::fprintf(stderr, "the index did not become ready (%s): %s\n", cml::core::to_string(status.state),
                     status.error ? status.error->format().c_str() : status.phase.c_str());
        return 1;
    }
    std::printf("index: ready in %.0f ms, %zu compilands, %zu tracked files (pdb %.0f ms, baseline %.0f ms)\n", index_ms, status.compilands,
                status.files, status.pdb_ms, status.baseline_ms);
    const Memory after_index = read_memory();

    // What a reload does with the index: sweep (maps the PDB again after a release), release, sweep again.
    bool sweeps_ok = true;
    for (int round = 0; round < 2; ++round) {
        auto sweep = index->sweep();
        if (!sweep) {
            std::fprintf(stderr, "sweep %d failed: %s\n", round, sweep.error().format().c_str());
            sweeps_ok = false;
        } else {
            std::printf("sweep %d: %zu files checked, %zu hashed, %zu changed\n", round, sweep->checked, sweep->hashed, sweep->changed.size());
        }
        index->release_pdb();
    }
    const Memory end = read_memory();

    const double size = static_cast<double>(file_bytes);
    const double budget = factor * size;
    std::printf("\n== broker memory on this PDB ==\n");
    std::printf("  private bytes before the index    %9.1f MiB\n", mib(before.private_now));
    std::printf("  private bytes after the index     %9.1f MiB\n", mib(after_index.private_now));
    std::printf("  private bytes at the end          %9.1f MiB\n", mib(end.private_now));
    std::printf("  PEAK PRIVATE BYTES                %9.1f MiB = %.3f x PDB size   (budget %.1f x = %.1f MiB)\n", mib(end.peak_private),
                size > 0 ? end.peak_private / size : 0.0, factor, mib(budget));
    std::printf("  peak working set (incl. mapped)   %9.1f MiB\n", mib(end.peak_working_set));
    // Information: the same accounting as cml_pdb_bench (private bytes plus the whole file mapping, resident or not).
    std::printf("  peak private + mapped file        %9.1f MiB = %.3f x PDB size   [information only]\n", mib(end.peak_private + size),
                size > 0 ? (end.peak_private + size) / size : 0.0);

    if (!json_path.empty()) {
        std::ofstream out(json_path, std::ios::binary | std::ios::trunc);
        char buf[640];
        std::snprintf(buf, sizeof buf,
                      "{\"pdb_bytes\":%llu,\"factor_budget\":%.3f,\"index_ms\":%.1f,\"compilands\":%zu,\"tracked_files\":%zu,"
                      "\"peak_private_bytes\":%.0f,\"private_bytes_after_index\":%.0f,\"private_bytes_end\":%.0f,"
                      "\"peak_working_set_bytes\":%.0f,\"peak_private_over_pdb\":%.4f}\n",
                      static_cast<unsigned long long>(file_bytes), factor, index_ms, status.compilands, status.files, end.peak_private,
                      after_index.private_now, end.private_now, end.peak_working_set, size > 0 ? end.peak_private / size : 0.0);
        out << buf;
    }

    bool pass = sweeps_ok;
    if (smoke) {
        std::printf("\n(smoke mode: the budget is not enforced)\n");
    } else {
        const bool within = end.peak_private <= budget;
        std::printf("[%s] broker memory within budget: %.1f MiB <= %.1f MiB (%.1f x PDB size)\n", within ? "PASS" : "FAIL", mib(end.peak_private),
                    mib(budget), factor);
        pass = pass && within;
    }
    std::printf("\nresult: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
