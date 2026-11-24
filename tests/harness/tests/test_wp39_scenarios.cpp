// WP-39: scenarios for an externally built program (`external = true`), the tagged perf_report and its evidence line,
// and the checked-in large-PDB reload scenario.
#include <filesystem>
#include <string>
#include <vector>

#include "cml/ipc/control_ids.h"
#include "cml/ipc/json.h"
#include "doctest.h"
#include "perf_steps.h"
#include "scenario.h"

using namespace cmltest;

TEST_CASE("scenario: external = true marks a program built outside cmltest; anything but a boolean is CML5411") {
    const std::string steps = "[[step]]\nkind = \"launch\"\n";
    cml::Result<Scenario> plain = parse_scenario("name = \"x\"\nfixture = \"f\"\n" + steps);
    REQUIRE_MESSAGE(plain.has_value(), plain.error().format());
    CHECK_FALSE(plain->external);  // the default: a fixture of tests/fixtures, copied and built by cmltest

    cml::Result<Scenario> external = parse_scenario("name = \"x\"\nfixture = \"bench_project\"\nexternal = true\n" + steps);
    REQUIRE_MESSAGE(external.has_value(), external.error().format());
    CHECK(external->external);
    CHECK(external->fixture == "bench_project");

    cml::Result<Scenario> off = parse_scenario("name = \"x\"\nfixture = \"f\"\nexternal = false\n" + steps);
    REQUIRE_MESSAGE(off.has_value(), off.error().format());
    CHECK_FALSE(off->external);

    for (const char* bad : {"external = \"true\"\n", "external = 1\n"}) {
        cml::Result<Scenario> r = parse_scenario(std::string("name = \"x\"\nfixture = \"f\"\n") + bad + steps);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == cml::ids::kScenarioInvalid);
    }
}

TEST_CASE("scenario: perf_report takes an optional tag for its evidence line") {
    const std::string head = "name = \"x\"\nfixture = \"f\"\n[[step]]\nkind = \"launch\"\n";
    cml::Result<Scenario> r = parse_scenario(head + "[[step]]\nkind = \"perf_report\"\nmax_median_ms = 3000\nmin_reloads = 5\ntag = \"WP39-RELOAD\"\n" +
                                             "[[step]]\nkind = \"perf_report\"\nmax_median_ms = 3000\n");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    REQUIRE(r->steps.size() == 3);
    CHECK(r->steps[1].perf_tag == "WP39-RELOAD");
    CHECK(r->steps[1].describe() == "perf_report median at most 3000 ms over at least 5 reloads (WP39-RELOAD)");
    CHECK(r->steps[2].perf_tag.empty());  // untagged: the WP-27 report, no evidence line

    for (const char* bad : {"tag = \"\"\n", "tag = \"wp39\"\n", "tag = \"WP39 RELOAD\"\n", "tag = 39\n", "tag = \"ABCDEFGHIJKLMNOPQRSTUVWXYZ-0123456\"\n"}) {
        cml::Result<Scenario> invalid = parse_scenario(head + "[[step]]\nkind = \"perf_report\"\nmax_median_ms = 3000\n" + bad);
        REQUIRE_FALSE(invalid.has_value());
        CHECK(invalid.error().code == cml::ids::kScenarioInvalid);
    }
    // a tag belongs to perf_report only
    cml::Result<Scenario> idle = parse_scenario(head + "[[step]]\nkind = \"perf_idle\"\nmax_cpu_permille = 10\ntag = \"X\"\n");
    REQUIRE_FALSE(idle.has_value());
    CHECK(idle.error().code == cml::ids::kScenarioInvalid);
}

TEST_CASE("reload_evidence_line: median and maximum of total minus compile, the budget, then every stage median") {
    using cml::ipc::json::Value;
    const char* const stages[] = {"sweep", "compile", "plan", "anchor", "link", "load", "fixups", "init", "hooks"};
    // Three reloads: every stage but compile takes 2, 4 and 9 ms; compile 1000, 3000, 2000; total = the sum.
    std::vector<Value> timings;
    const double others[] = {2, 4, 9};
    const double compile[] = {1000, 3000, 2000};
    for (int i = 0; i < 3; ++i) {
        Value t = Value::object();
        for (const char* s : stages) {
            t.set(s, Value::number(std::string(s) == "compile" ? compile[i] : others[i]));
        }
        t.set("total", Value::number(compile[i] + 8 * others[i]));
        timings.push_back(std::move(t));
    }
    REQUIRE(validate_timings(timings).empty());
    const std::string line = reload_evidence_line("WP39-RELOAD", 544.3, timings.size(), compute_stage_stats(timings), 3000);
    // total - compile per reload: 16, 32, 72 -> median 32, max 72. The median of total is 2072 (1016, 3032, 2072).
    CHECK(line == "WP39-RELOAD pdb_mib=544.3 reloads=3 total_minus_compile_median_ms=32.0 max_ms=72.0 budget_ms=3000 sweep_median_ms=4.0 "
                  "compile_median_ms=2000.0 plan_median_ms=4.0 anchor_median_ms=4.0 link_median_ms=4.0 load_median_ms=4.0 fixups_median_ms=4.0 "
                  "init_median_ms=4.0 hooks_median_ms=4.0 total_median_ms=2072.0");
}

#ifdef CML_BENCH_SCENARIOS_DIR
TEST_CASE("the checked-in large-PDB scenario: external, five edit + reload rounds on the generated project, a tagged report at 3000 ms") {
    namespace fs = std::filesystem;
    cml::Result<Scenario> r = load_scenario(fs::path(CML_BENCH_SCENARIOS_DIR) / "large_pdb_reload.toml");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    CHECK(r->external);
    CHECK(r->fixture == "bench_project");
    int edits = 0;
    int reloads = 0;
    bool report_seen = false;
    bool edit_pending = false;
    for (const Step& s : r->steps) {
        if (s.kind == StepKind::Edit) {
            ++edits;
            // the edits the generator writes (tests/bench/gen_pdb_project.py): edits/reload_2.edit .. reload_6.edit, in order
            CHECK(s.patch == "edits/reload_" + std::to_string(edits + 1) + ".edit");
            edit_pending = true;
        }
        if (s.kind == StepKind::Reload) {
            ++reloads;
            CHECK(edit_pending);  // every reload has its own edit: no reload of an unchanged tree is counted
            edit_pending = false;
            CHECK(s.expect_error.empty());
            CHECK(s.result_key == "patched");
            CHECK(s.result_value == 1);
        }
        if (s.kind == StepKind::PerfIdle) {
            FAIL("the idle measurement belongs to the perf scenario");
        }
        if (s.kind == StepKind::PerfReport) {
            report_seen = true;
            CHECK(reloads == 5);
            CHECK(s.result_value == 3000);  // ADR-0042: 2 x the plan target of 1500 ms; never raised to make a run pass
            CHECK(s.min_samples == 5);
            CHECK(s.perf_tag == "WP39-RELOAD");
        }
    }
    CHECK(edits == 5);
    CHECK(report_seen);
}
#endif
