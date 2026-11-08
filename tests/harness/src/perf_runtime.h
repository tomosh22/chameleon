#pragma once

// Windows half of the performance steps (WP-27, see perf_steps.h): measures the broker process and writes the results.
// Compiled into cmltest only.

#if defined(_WIN32)

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "cml/ipc/json.h"
#include "scenario.h"

namespace cmltest {

struct PerfStepResult {
    bool ok = true;
    std::string detail;
};

class PerfRecorder {
public:
    // The `timing_ms` object of one successful reload result (ignored when absent).
    void add_reload(const cml::ipc::json::Value& reload_result);
    std::size_t reload_count() const { return timings_.size(); }

    // perf_idle: observes the broker `broker_pid` for step.within_s seconds. `fixture_alive` is polled once a second;
    // a fixture that exits makes the measurement meaningless and fails the step.
    template <class Alive>
    PerfStepResult idle(std::uint32_t broker_pid, const Step& step, Alive fixture_alive) {
        return idle_impl(broker_pid, step, std::function<bool()>(fixture_alive));
    }

    // perf_report: statistics of the recorded reloads, the broker's peak private bytes, the JSON file.
    PerfStepResult report(std::uint32_t broker_pid, const Step& step, const std::string& scenario, const std::filesystem::path& artifacts_dir);

private:
    PerfStepResult idle_impl(std::uint32_t broker_pid, const Step& step, const std::function<bool()>& fixture_alive);

    std::vector<cml::ipc::json::Value> timings_;
    bool have_idle_ = false;
    double idle_cpu_percent_ = 0;
    double idle_cpu_ms_ = 0;
    double idle_wall_s_ = 0;
};

}  // namespace cmltest

#endif  // _WIN32
