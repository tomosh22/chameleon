#include "perf_runtime.h"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>

#include "perf_steps.h"

namespace cmltest {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr double kBrokerBaselineMiB = 32.0;  // fixed cost of the broker process, whatever the PDB
constexpr DWORD kSettleMs = 5000;  // the broker has just finished its index and hashing work; let it go quiet

struct ProcessHandle {
    HANDLE h = nullptr;
    explicit ProcessHandle(std::uint32_t pid) : h(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid)) {}
    ~ProcessHandle() {
        if (h != nullptr) {
            CloseHandle(h);
        }
    }
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    bool alive() const {
        DWORD code = 0;
        return h != nullptr && GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    }
};

double filetime_ms(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<double>(u.QuadPart) / 10000.0;  // 100 ns units
}

// Kernel + user CPU time of the process, in ms.
bool cpu_ms(HANDLE process, double* out) {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        return false;
    }
    *out = filetime_ms(kernel) + filetime_ms(user);
    return true;
}

std::string fmt(const char* format, double a, double b = 0, double c = 0) {
    char buf[256];
    std::snprintf(buf, sizeof buf, format, a, b, c);
    return buf;
}

}  // namespace

void PerfRecorder::add_reload(const cml::ipc::json::Value& reload_result) {
    const cml::ipc::json::Value* timing = reload_result.find("timing_ms");
    if (timing != nullptr && timing->is_object()) {
        timings_.push_back(*timing);
    }
}

PerfStepResult PerfRecorder::idle_impl(std::uint32_t broker_pid, const Step& step, std::uint32_t limit_ms, const std::function<bool()>& fixture_alive) {
    PerfStepResult result;
    const std::uint64_t needed_ms = static_cast<std::uint64_t>(kSettleMs) + static_cast<std::uint64_t>(step.within_s) * 1000u + 2000u;
    if (needed_ms > limit_ms) {
        result.ok = false;
        result.detail = "the scenario's remaining time (" + std::to_string(limit_ms / 1000) + " s) is shorter than the settling time plus the " +
                        std::to_string(step.within_s) + " s observation; raise timeout_s";
        return result;
    }
    ProcessHandle broker(broker_pid);
    if (broker.h == nullptr) {
        result.ok = false;
        result.detail = "cannot open the broker process " + std::to_string(broker_pid) + " (error " + std::to_string(GetLastError()) + ")";
        return result;
    }
    Sleep(kSettleMs);
    double cpu0 = 0;
    if (!cpu_ms(broker.h, &cpu0)) {
        result.ok = false;
        result.detail = "GetProcessTimes failed for the broker (error " + std::to_string(GetLastError()) + ")";
        return result;
    }
    const Clock::time_point t0 = Clock::now();
    const Clock::time_point until = t0 + std::chrono::seconds(step.within_s);
    while (Clock::now() < until) {
        Sleep(1000);
        if (!broker.alive()) {
            result.ok = false;
            result.detail = "the broker exited during the idle measurement";
            return result;
        }
        if (fixture_alive && !fixture_alive()) {
            result.ok = false;
            result.detail = "the fixture exited during the idle measurement (no connected process to be idle with)";
            return result;
        }
    }
    double cpu1 = 0;
    if (!cpu_ms(broker.h, &cpu1)) {
        result.ok = false;
        result.detail = "GetProcessTimes failed for the broker at the end of the observation (error " + std::to_string(GetLastError()) + ")";
        return result;
    }
    const double wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (cpu1 < cpu0 || wall_ms <= 0) {
        result.ok = false;
        result.detail = fmt("the broker's CPU time went backwards (%.1f ms -> %.1f ms) or no time passed", cpu0, cpu1);
        return result;
    }
    idle_cpu_ms_ = cpu1 - cpu0;
    idle_wall_s_ = wall_ms / 1000.0;
    idle_cpu_percent_ = wall_ms > 0 ? 100.0 * idle_cpu_ms_ / wall_ms : 0.0;
    have_idle_ = true;
    const double budget_percent = static_cast<double>(step.result_value) / 10.0;
    std::printf("PERF broker_idle_cpu_percent=%.3f cpu_ms=%.1f wall_s=%.1f budget_percent=%.2f\n", idle_cpu_percent_, idle_cpu_ms_, idle_wall_s_,
                budget_percent);
    std::fflush(stdout);
    result.detail = fmt("broker CPU %.1f ms over %.1f s = %.3f %% of one core", idle_cpu_ms_, idle_wall_s_, idle_cpu_percent_);
    if (idle_cpu_percent_ > budget_percent) {
        result.ok = false;
        result.detail += fmt(" > the limit of %.2f %%", budget_percent);
    } else {
        result.detail += fmt(" (limit %.2f %%)", budget_percent);
    }
    return result;
}

PerfStepResult PerfRecorder::report(std::uint32_t broker_pid, const Step& step, const std::string& scenario, const fs::path& artifacts_dir,
                                    const fs::path& module_pdb) {
    using cml::ipc::json::Value;
    PerfStepResult result;
    if (timings_.size() < static_cast<std::size_t>(step.min_samples)) {
        result.ok = false;
        result.detail = "only " + std::to_string(timings_.size()) + " reloads were recorded, the step needs at least " + std::to_string(step.min_samples);
        return result;
    }
    const std::string invalid = validate_timings(timings_);
    if (!invalid.empty()) {
        result.ok = false;
        result.detail = "the recorded reload timings cannot be trusted: " + invalid;
        return result;
    }
    const std::vector<StageStats> stats = compute_stage_stats(timings_);
    Value stages = Value::object();
    double median_excl = 0;
    double max_excl = 0;
    std::printf("PERF reloads=%zu\n", timings_.size());
    for (const StageStats& s : stats) {
        std::printf("PERF stage=%s median_ms=%.1f max_ms=%.1f\n", s.stage.c_str(), s.median, s.max);
        Value one = Value::object();
        one.set("median_ms", Value::number(s.median));
        one.set("max_ms", Value::number(s.max));
        stages.set(s.stage, std::move(one));
        if (s.stage == "total_excl_compile") {
            median_excl = s.median;
            max_excl = s.max;
        }
    }
    Value samples = Value::array();
    for (const Value& t : timings_) {
        samples.push(t);
    }

    Value broker = Value::object();
    ProcessHandle process(broker_pid);
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    std::string memory_problem;
    if (process.h == nullptr || !GetProcessMemoryInfo(process.h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) {
        memory_problem = "cannot read the broker's memory counters (error " + std::to_string(GetLastError()) + ")";
    } else {
        std::error_code size_ec;
        const std::uintmax_t pdb_bytes = fs::file_size(module_pdb, size_ec);
        if (size_ec || pdb_bytes == 0) {
            memory_problem = "cannot read the size of the module PDB " + module_pdb.string();
        } else {
            const double limit = 1.5 * static_cast<double>(pdb_bytes) + kBrokerBaselineMiB * 1024.0 * 1024.0;
            broker.set("module_pdb_bytes", Value::integer(static_cast<std::int64_t>(pdb_bytes)));
            broker.set("peak_private_limit_bytes", Value::number(limit));
            if (static_cast<double>(pmc.PeakPagefileUsage) > limit) {
                memory_problem = fmt("the broker's peak private bytes (%.1f MiB) exceed 1.5 x the module PDB plus the process baseline (%.1f MiB)",
                                     static_cast<double>(pmc.PeakPagefileUsage) / 1048576.0, limit / 1048576.0);
            }
        }
        // PeakPagefileUsage is the peak commit charge of the process: its private bytes at their highest.
        broker.set("peak_private_bytes", Value::integer(static_cast<std::int64_t>(pmc.PeakPagefileUsage)));
        broker.set("private_bytes", Value::integer(static_cast<std::int64_t>(pmc.PrivateUsage)));
        broker.set("peak_working_set_bytes", Value::integer(static_cast<std::int64_t>(pmc.PeakWorkingSetSize)));
        std::printf("PERF broker_peak_private_bytes=%llu private_bytes=%llu peak_working_set_bytes=%llu\n",
                    static_cast<unsigned long long>(pmc.PeakPagefileUsage), static_cast<unsigned long long>(pmc.PrivateUsage),
                    static_cast<unsigned long long>(pmc.PeakWorkingSetSize));
    }
    if (have_idle_) {
        broker.set("idle_cpu_percent", Value::number(idle_cpu_percent_));
        broker.set("idle_cpu_ms", Value::number(idle_cpu_ms_));
        broker.set("idle_wall_s", Value::number(idle_wall_s_));
    }

    Value doc = Value::object();
    doc.set("scenario", Value::string(scenario));
    doc.set("reloads", Value::integer(static_cast<std::int64_t>(timings_.size())));
    doc.set("stages", std::move(stages));
    doc.set("reload_median_excl_compile_ms", Value::number(median_excl));
    doc.set("reload_max_excl_compile_ms", Value::number(max_excl));
    doc.set("samples", std::move(samples));
    doc.set("broker", std::move(broker));

    fs::path out = artifacts_dir / "perf-reload.json";
    char env[MAX_PATH * 2] = {};
    const DWORD env_len = GetEnvironmentVariableA("CML_PERF_JSON", env, static_cast<DWORD>(sizeof env));
    if (env_len > 0 && env_len < sizeof env) {
        out = env;
    }
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    file << cml::ipc::json::serialize(doc) << "\n";
    file.close();
    if (!file) {
        result.ok = false;
        result.detail = "cannot write " + out.string();
        return result;
    }

    result.detail = fmt("median %.1f ms, max %.1f ms (total minus compile) over ", median_excl, max_excl) + std::to_string(timings_.size()) +
                    " reloads, limit " + std::to_string(step.result_value) + " ms on the median; written to " + out.string();
    if (median_excl > static_cast<double>(step.result_value)) {
        result.ok = false;
        result.detail = "over budget: " + result.detail;
    }
    if (!memory_problem.empty()) {
        result.ok = false;
        result.detail = memory_problem + "; " + result.detail;
    }
    return result;
}

}  // namespace cmltest

#endif  // _WIN32
