#include "perf_steps.h"

#include <algorithm>
#include <utility>

#include "cml/ipc/control_ids.h"

namespace cmltest {

namespace {

cml::Diag invalid(int line, const std::string& what) {
    return cml::Diag::make(cml::ids::kScenarioInvalid, "scenario line " + std::to_string(line) + ": " + what,
                           "Fix the scenario file (tests/harness/README.md lists the keys of every step kind).");
}

// An integer key in [lo, hi]; `required` without a default.
cml::Result<long long> int_key(const toml::Table& table, const char* key, long long fallback, long long lo, long long hi, bool required) {
    const toml::Value* v = table.find(key);
    if (v == nullptr) {
        if (required) {
            return invalid(table.line, std::string("the step needs the key '") + key + "'");
        }
        return fallback;
    }
    if (v->kind != toml::Value::Kind::Integer || v->integer < lo || v->integer > hi) {
        return invalid(v->line, std::string("'") + key + "' must be an integer between " + std::to_string(lo) + " and " + std::to_string(hi));
    }
    return v->integer;
}

}  // namespace

cml::Result<Step> parse_perf_step(const toml::Table& table, const std::string& kind) {
    Step step;
    step.line = table.line;
    const bool idle = kind == "perf_idle";
    step.kind = idle ? StepKind::PerfIdle : StepKind::PerfReport;
    for (const auto& entry : table.entries) {
        const std::string& key = entry.first;
        const bool known = key == "kind" || (idle ? (key == "duration_s" || key == "max_cpu_permille") : (key == "max_median_ms" || key == "min_reloads"));
        if (!known) {
            return invalid(entry.second.line, "unknown key '" + key + "' in a " + kind + " step");
        }
    }
    if (idle) {
        cml::Result<long long> duration = int_key(table, "duration_s", 60, 60, 3600, false);
        if (!duration) return duration.error();
        cml::Result<long long> permille = int_key(table, "max_cpu_permille", 0, 1, 1000, true);
        if (!permille) return permille.error();
        step.within_s = static_cast<int>(*duration);
        step.result_value = *permille;
    } else {
        cml::Result<long long> median = int_key(table, "max_median_ms", 0, 1, 600000, true);
        if (!median) return median.error();
        cml::Result<long long> reloads = int_key(table, "min_reloads", 3, 1, 100, false);
        if (!reloads) return reloads.error();
        step.result_value = *median;
        step.min_samples = static_cast<int>(*reloads);
    }
    step.has_result_value = true;
    return step;
}

double median_of(std::vector<double> values) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    return n % 2 == 1 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

std::vector<StageStats> compute_stage_stats(const std::vector<cml::ipc::json::Value>& timings) {
    static const char* const kStages[] = {"sweep", "compile", "plan", "anchor", "link", "load", "fixups", "init", "hooks", "total"};
    std::vector<StageStats> out;
    if (timings.empty()) {
        return out;
    }
    const auto number = [](const cml::ipc::json::Value& object, const char* key) {
        const cml::ipc::json::Value* v = object.find(key);
        return v != nullptr && v->is_number() ? v->as_double() : 0.0;
    };
    for (const char* stage : kStages) {
        std::vector<double> values;
        for (const cml::ipc::json::Value& t : timings) {
            values.push_back(number(t, stage));
        }
        StageStats s;
        s.stage = stage;
        s.median = median_of(values);
        s.max = *std::max_element(values.begin(), values.end());
        out.push_back(std::move(s));
    }
    std::vector<double> excl;
    for (const cml::ipc::json::Value& t : timings) {
        excl.push_back(number(t, "total") - number(t, "compile"));
    }
    StageStats s;
    s.stage = "total_excl_compile";
    s.median = median_of(excl);
    s.max = *std::max_element(excl.begin(), excl.end());
    out.push_back(std::move(s));
    return out;
}

}  // namespace cmltest
