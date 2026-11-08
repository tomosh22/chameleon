// WP-27: the perf_idle / perf_report steps, their statistics, and the checked-in perf scenario.
#include <filesystem>
#include <string>
#include <vector>

#include "cml/ipc/control_ids.h"
#include "cml/ipc/json.h"
#include "doctest.h"
#include "perf_steps.h"
#include "scenario.h"

using namespace cmltest;

TEST_CASE("scenario: perf_idle and perf_report take their budgets and validate them") {
    const char* text = R"(
name = "x"
fixture = "perf"
[[step]]
kind = "launch"
[[step]]
kind = "perf_idle"
max_cpu_permille = 10
[[step]]
kind = "perf_idle"
duration_s = 90
max_cpu_permille = 5
[[step]]
kind = "perf_report"
max_median_ms = 3000
[[step]]
kind = "perf_report"
max_median_ms = 1500
min_reloads = 7
)";
    cml::Result<Scenario> r = parse_scenario(text);
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    REQUIRE(r->steps.size() == 5);
    CHECK(r->steps[1].kind == StepKind::PerfIdle);
    CHECK(r->steps[1].within_s == 60);  // the default duration
    CHECK(r->steps[1].result_value == 10);
    CHECK(r->steps[2].within_s == 90);
    CHECK(r->steps[2].result_value == 5);
    CHECK(r->steps[3].kind == StepKind::PerfReport);
    CHECK(r->steps[3].result_value == 3000);
    CHECK(r->steps[3].min_samples == 3);  // the default
    CHECK(r->steps[4].min_samples == 7);
    CHECK(r->steps[1].required_capability() == nullptr);
    CHECK(r->steps[2].describe() == "perf_idle 90 s, at most 5 permille CPU");
    CHECK(r->steps[4].describe() == "perf_report median at most 1500 ms over at least 7 reloads");
}

TEST_CASE("scenario: bad perf step keys are CML5411") {
    const std::string head = "name = \"x\"\nfixture = \"f\"\n[[step]]\nkind = \"launch\"\n";
    const auto invalid = [&](const std::string& steps) {
        cml::Result<Scenario> r = parse_scenario(head + steps);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == cml::ids::kScenarioInvalid);
    };
    invalid("[[step]]\nkind = \"perf_idle\"\n");                                         // no budget
    invalid("[[step]]\nkind = \"perf_idle\"\nmax_cpu_permille = 0\n");                   // out of range
    invalid("[[step]]\nkind = \"perf_idle\"\nmax_cpu_permille = 10\nduration_s = 5\n");  // too short to mean anything
    invalid("[[step]]\nkind = \"perf_idle\"\nmax_cpu_permille = 10\nmax_median_ms = 5\n");
    invalid("[[step]]\nkind = \"perf_report\"\n");
    invalid("[[step]]\nkind = \"perf_report\"\nmax_median_ms = \"3000\"\n");
    invalid("[[step]]\nkind = \"perf_report\"\nmax_median_ms = 3000\nmin_reloads = 0\n");
    invalid("[[step]]\nkind = \"perf_report\"\nmax_median_ms = 3000\nwhat = 1\n");
}

TEST_CASE("stage statistics: median and maximum per stage, total minus compile") {
    using cml::ipc::json::Value;
    const auto reload = [](double compile, double link, double total) {
        Value v = Value::object();
        v.set("compile", Value::number(compile));
        v.set("link", Value::integer(static_cast<std::int64_t>(link)));
        v.set("total", Value::number(total));
        return v;
    };
    CHECK(compute_stage_stats({}).empty());
    // total - compile: 100, 50, 300, 80 -> median (80 + 100) / 2 = 90, max 300
    const std::vector<Value> timings = {reload(400, 30, 500), reload(450, 20, 500), reload(100, 90, 400), reload(420, 40, 500)};
    const std::vector<StageStats> stats = compute_stage_stats(timings);
    REQUIRE(stats.size() == 11);
    CHECK(stats.back().stage == "total_excl_compile");
    CHECK(stats.back().median == doctest::Approx(90.0));
    CHECK(stats.back().max == doctest::Approx(300.0));
    bool saw_link = false;
    for (const StageStats& s : stats) {
        if (s.stage == "link") {
            saw_link = true;
            CHECK(s.median == doctest::Approx(35.0));  // 20 30 40 90
            CHECK(s.max == doctest::Approx(90.0));
        }
        if (s.stage == "sweep") {
            CHECK(s.max == doctest::Approx(0.0));  // absent members count as 0
        }
    }
    CHECK(saw_link);
    CHECK(median_of({3, 1, 2}) == doctest::Approx(2.0));
    CHECK(median_of({}) == doctest::Approx(0.0));
}

#ifdef CML_BENCH_SCENARIOS_DIR
TEST_CASE("the checked-in perf scenario parses: the watcher idles first, then five reloads, then the report") {
    namespace fs = std::filesystem;
    cml::Result<Scenario> r = load_scenario(fs::path(CML_BENCH_SCENARIOS_DIR) / "perf.toml");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    CHECK(r->fixture == "perf");
    int reloads = 0;
    bool idle_seen = false;
    bool report_seen = false;
    for (const Step& s : r->steps) {
        if (s.kind == StepKind::PerfIdle) {
            idle_seen = true;
            CHECK(reloads == 0);               // the idle measurement comes before any reload
            CHECK(s.within_s >= 60);           // the brief: at least 60 s
            CHECK(s.result_value <= 10);       // CI threshold: 1 % of one core
        }
        if (s.kind == StepKind::Reload) ++reloads;
        if (s.kind == StepKind::PerfReport) {
            report_seen = true;
            CHECK(reloads == 5);
            CHECK(s.result_value <= 3000);     // CI threshold: 2 x 1.5 s
            CHECK(s.min_samples == 5);
        }
    }
    CHECK(idle_seen);
    CHECK(report_seen);
    // every edit the scenario applies exists in the fixture
    for (const Step& s : r->steps) {
        if (s.kind == StepKind::Edit) {
            CHECK_MESSAGE(fs::exists(fs::path(CML_FIXTURE_PERF_DIR) / s.patch), s.patch);
        }
    }
}
#endif
