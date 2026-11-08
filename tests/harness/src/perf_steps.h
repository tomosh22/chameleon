#pragma once

// The performance steps (WP-27): `perf_idle` and `perf_report`. Kept apart from runner.cpp / scenario.cpp, which only
// register them. Used by tests/bench/scenarios/perf.toml (the `pdb-benchmark` CI job); not by the scenarios that run in
// every build cell.
//
//   [[step]]
//   kind = "perf_idle"
//   duration_s = 60            # 60..3600, default 60: how long the broker is observed (after a fixed 5 s settling time)
//   max_cpu_permille = 10      # required: the step fails when the broker used more than this share of ONE core (10 = 1 %)
//
//   [[step]]
//   kind = "perf_report"
//   max_median_ms = 3000       # required: median over the recorded reloads of (total - compile), in ms
//   min_reloads = 3            # 1..100, default 3: at least this many `reload` steps must have run
//
// perf_idle samples the broker process's kernel+user CPU time (GetProcessTimes) over the observation window and
// reports it as a percentage of one core's wall time. perf_report needs the reloads recorded by `reload` steps
// (their `timing_ms` objects), computes median and maximum per stage and for total-minus-compile, reads the broker's
// peak private bytes and writes the numbers as JSON to $CML_PERF_JSON (default <artifacts>/perf-reload.json).

#include <cstdint>
#include <string>
#include <vector>

#include "cml/ipc/json.h"
#include "scenario.h"
#include "toml.h"

namespace cmltest {

// Portable: validates and reads the keys of a perf step (CML5411 on a bad one). `kind` is "perf_idle" or "perf_report".
cml::Result<Step> parse_perf_step(const toml::Table& table, const std::string& kind);

// Portable: the per-stage statistics of the recorded reloads (each entry is a reload result's `timing_ms` object).
struct StageStats {
    std::string stage;
    double median = 0;
    double max = 0;
};
// Stages in pipeline order (sweep, compile, plan, anchor, link, load, fixups, init, hooks, total) followed by
// "total_excl_compile" (total - compile per reload). Empty input gives an empty result.
std::vector<StageStats> compute_stage_stats(const std::vector<cml::ipc::json::Value>& timings);
double median_of(std::vector<double> values);

}  // namespace cmltest
