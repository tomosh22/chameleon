// expect_monotonic: reading TICK keys and judging the series (WP-12).
#include <string>

#include "cml/ipc/control_ids.h"
#include "doctest.h"
#include "scenario.h"
#include "ticks.h"

using namespace cmltest;

TEST_CASE("tick_values reads whole-word keys of TICK lines only") {
    const std::string out =
        "AGENT enabled=1 calls=99\n"
        "TICK n=0 value=3 calls=1 total=1\n"
        "TICK n=1 value=3 recalls=7 calls=2 total=11\r\n"
        "NOTE calls=500\n"
        "TICK n=2 value=3 total=111\n"      // no calls
        "TICK n=3 value=3 calls=x total=1\n"  // not an integer
        "TICK n=4 value=3 calls=-5\n";
    const std::vector<long long> v = tick_values(out, "calls");
    CHECK(v == std::vector<long long>{1, 2, -5});
    CHECK(tick_values(out, "n") == std::vector<long long>{0, 1, 2, 3, 4});
    CHECK(tick_values("", "calls").empty());
    CHECK(tick_values("TICK calls=4", "calls") == std::vector<long long>{4});
}

TEST_CASE("check_monotonic accepts a counter that keeps counting across a patch") {
    std::string out;
    for (int i = 0; i < 30; ++i) {
        out += "TICK n=" + std::to_string(i) + " value=" + (i < 15 ? "3" : "30") + " calls=" + std::to_string(i + 1) + "\n";
    }
    cml::Result<std::string> r = check_monotonic(out, "calls", 10);
    REQUIRE(r.has_value());
    CHECK(*r == "calls: 30 samples, 1 -> 30");
}

TEST_CASE("check_monotonic names a reset, a constant series and too few samples") {
    std::string reset;
    for (int i = 0; i < 12; ++i) {
        reset += "TICK n=" + std::to_string(i) + " calls=" + std::to_string(i < 6 ? i + 20 : i - 5) + "\n";
    }
    cml::Result<std::string> r = check_monotonic(reset, "calls", 10);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == cml::ids::kCounterNotMonotonic);
    CHECK(r.error().message.find("went from 25 to 1") != std::string::npos);

    std::string flat;
    for (int i = 0; i < 12; ++i) {
        flat += "TICK calls=7\n";
    }
    CHECK_FALSE(check_monotonic(flat, "calls", 10).has_value());
    CHECK_FALSE(check_monotonic("TICK calls=1\nTICK calls=2\n", "calls", 10).has_value());
}

TEST_CASE("check_continues: the first sample after the last reload is not below the last one before it (WP-52)") {
    const std::string before = "TICK nest=700810\nTICK nest=700811\nnoise\n";
    std::string after;
    for (int i = 0; i < 5; ++i) {
        after += "TICK nest=" + std::to_string(700812 + i) + "\n";
    }
    cml::Result<std::string> ok = check_continues(before, after, "nest", 5);
    REQUIRE_MESSAGE(ok.has_value(), ok.error().format());
    CHECK(*ok == "nest: 700811 before the reload, 700812 after, 5 samples since");
    // A restarted static: 700001 after 700811.
    cml::Result<std::string> reset = check_continues(before, "TICK nest=700001\nTICK nest=700002\n", "nest", 2);
    REQUIRE_FALSE(reset.has_value());
    CHECK(reset.error().code == cml::ids::kCounterNotMonotonic);
    CHECK(reset.error().message.find("was 700811 before the last reload and 700001 after it") != std::string::npos);
    CHECK_FALSE(check_continues(before, after, "nest", 6).has_value());  // too few samples since
    CHECK_FALSE(check_continues("", after, "nest", 1).has_value());      // nothing before
}

TEST_CASE("scenario: expect_monotonic takes since_reload (WP-52)") {
    const std::string head = "name = \"x\"\nfixture = \"statics\"\n[[step]]\nkind = \"launch\"\n[[step]]\nkind = \"expect_monotonic\"\nkey = \"nest\"\n";
    cml::Result<Scenario> r = parse_scenario(head + "since_reload = 20\n");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    CHECK(r->steps[1].since_reload == 20);
    CHECK(r->steps[1].describe() == "expect_monotonic nest (at least 10 samples, 20 since the last reload, continuing from before it)");
    cml::Result<Scenario> plain = parse_scenario(head);
    REQUIRE(plain.has_value());
    CHECK(plain->steps[1].since_reload == 0);
    CHECK_FALSE(parse_scenario(head + "since_reload = 0\n").has_value());
    CHECK_FALSE(parse_scenario(head + "since_reload = \"x\"\n").has_value());
    cml::Result<Scenario> waits = parse_scenario(head + "since_reload = 5\nwithin_s = 20\n");
    REQUIRE(waits.has_value());
    CHECK(waits->steps[1].within_s == 20);
    CHECK_FALSE(parse_scenario(head + "within_s = 20\n").has_value());  // only with since_reload
}
