// Scenario outcome rules, console summary and JUnit output. The key property: a skipped step is
// never a pass.
#include <string>

#include "doctest.h"
#include "report.h"

using namespace cmltest;

namespace {

StepResult step(const std::string& name, StepStatus status, const std::string& detail = std::string(), const std::string& id = std::string()) {
    StepResult s;
    s.name = name;
    s.status = status;
    s.detail = detail;
    s.diag_id = id;
    s.seconds = 1.5;
    return s;
}

ScenarioReport partial_report() {
    ScenarioReport r;
    r.scenario = "basic";
    r.generator = "ninja";
    r.config = "Debug";
    r.compiler = "cl";
    r.seconds = 12.5;
    r.steps.push_back(step("build fixture", StepStatus::Passed));
    r.steps.push_back(step("launch", StepStatus::Passed));
    r.steps.push_back(step("expect_stdout /value=1/ within 10 s", StepStatus::Passed, "matched: TICK n=0 value=1"));
    r.steps.push_back(step("edit edits/value_2.edit", StepStatus::Passed));
    r.steps.push_back(step("reload within 60 s", StepStatus::SkippedCapability, "capability 'reload' not provided by broker (lands in WP-12)"));
    r.steps.push_back(step("expect_stdout /value=2/ within 10 s", StepStatus::SkippedCapability, "capability 'reload' not provided by broker (lands in WP-12)"));
    return r;
}

}  // namespace

TEST_CASE("report: outcome rules") {
    ScenarioReport r = partial_report();
    CHECK(r.outcome(false) == Outcome::Partial);
    CHECK(r.outcome(true) == Outcome::Failed);  // --require-all-capabilities
    CHECK(r.count(StepStatus::Passed) == 4);
    CHECK(r.count(StepStatus::SkippedCapability) == 2);

    ScenarioReport pass;
    pass.steps.push_back(step("a", StepStatus::Passed));
    CHECK(pass.outcome(false) == Outcome::Passed);
    CHECK(pass.outcome(true) == Outcome::Passed);

    ScenarioReport fail = partial_report();
    fail.steps[3].status = StepStatus::Failed;
    CHECK(fail.outcome(false) == Outcome::Failed);

    ScenarioReport not_run;
    not_run.steps.push_back(step("a", StepStatus::NotRun));
    CHECK(not_run.outcome(false) == Outcome::Failed);  // can only happen after a failure; never "passed"
}

TEST_CASE("report: the console report shows skips loudly and does not count them as passes") {
    const std::string text = console_report(partial_report(), false);
    CHECK(text.find("[PASS] 1. build fixture") != std::string::npos);
    CHECK(text.find("[SKIP] 5. reload within 60 s") != std::string::npos);
    CHECK(text.find("capability 'reload' not provided by broker (lands in WP-12)") != std::string::npos);
    CHECK(text.find("scenario basic: partial (4 passed, 0 failed, 2 skipped, 0 not run)") != std::string::npos);
    CHECK(text.find("PARTIAL: 2 steps skipped for missing broker capabilities") != std::string::npos);
    CHECK(text.find("CMLTEST_RESULT scenario=basic generator=ninja config=Debug compiler=cl outcome=partial passed=4 failed=0 skipped=2 not_run=0") !=
          std::string::npos);

    const std::string strict = console_report(partial_report(), true);
    CHECK(strict.find("scenario basic: failed") != std::string::npos);
    CHECK(strict.find("FAILED (--require-all-capabilities): 2 steps skipped for missing broker capabilities") != std::string::npos);
    CHECK(strict.find("PARTIAL:") == std::string::npos);

    ScenarioReport pass;
    pass.scenario = "p";
    pass.steps.push_back(step("a", StepStatus::Passed));
    const std::string clean = console_report(pass, false);
    CHECK(clean.find("PARTIAL") == std::string::npos);
    CHECK(clean.find("outcome=passed") != std::string::npos);
}

TEST_CASE("report: JUnit for a partial scenario has <skipped> and lists the steps in system-out") {
    const std::string xml = junit_xml(partial_report(), false);
    CHECK(xml.find("<?xml version=\"1.0\" encoding=\"UTF-8\"?>") == 0);
    CHECK(xml.find("<testsuites name=\"cmltest\" tests=\"1\" failures=\"0\" errors=\"0\" skipped=\"1\"") != std::string::npos);
    CHECK(xml.find("<testcase classname=\"cmltest.ninja.Debug.cl\" name=\"basic\" time=\"12.5\"") != std::string::npos);
    CHECK(xml.find("<skipped message=\"PARTIAL: 2 steps skipped for missing broker capabilities: reload within 60 s (capability &apos;reload&apos; not provided by broker (lands in WP-12))") !=
          std::string::npos);
    CHECK(xml.find("<failure") == std::string::npos);
    CHECK(xml.find("<system-out>") != std::string::npos);
    CHECK(xml.find("[PASS] 1. build fixture") != std::string::npos);
    CHECK(xml.find("[SKIP] 5. reload within 60 s") != std::string::npos);
    CHECK(xml.find("<property name=\"steps_skipped\" value=\"2\"/>") != std::string::npos);
    CHECK(xml.find("<property name=\"steps_passed\" value=\"4\"/>") != std::string::npos);
}

TEST_CASE("report: JUnit for failures and for --require-all-capabilities") {
    ScenarioReport fail = partial_report();
    fail.steps[2] = step("expect_stdout /value=1/ within 10 s", StepStatus::Failed, "did not appear <in> \"time\" & more", "CML5416");
    fail.steps[3].status = StepStatus::NotRun;
    const std::string xml = junit_xml(fail, false);
    CHECK(xml.find("failures=\"1\"") != std::string::npos);
    CHECK(xml.find("<failure message=\"expect_stdout /value=1/ within 10 s: did not appear &lt;in&gt; &quot;time&quot; &amp; more\" type=\"CML5416\">") !=
          std::string::npos);
    CHECK(xml.find("<skipped") == std::string::npos);
    CHECK(xml.find("[NOT RUN] 4.") != std::string::npos);

    const std::string strict = junit_xml(partial_report(), true);
    CHECK(strict.find("failures=\"1\"") != std::string::npos);
    CHECK(strict.find("--require-all-capabilities: 2 steps skipped") != std::string::npos);
    CHECK(strict.find("<skipped") == std::string::npos);
}

TEST_CASE("report: a fully passing scenario has neither failure nor skipped") {
    ScenarioReport pass;
    pass.scenario = "ok";
    pass.generator = "msbuild";
    pass.config = "RelWithDebInfo";
    pass.compiler = "clang-cl";
    pass.steps.push_back(step("a", StepStatus::Passed));
    const std::string xml = junit_xml(pass, false);
    CHECK(xml.find("failures=\"0\"") != std::string::npos);
    CHECK(xml.find("skipped=\"0\"") != std::string::npos);
    CHECK(xml.find("<failure") == std::string::npos);
    CHECK(xml.find("<skipped") == std::string::npos);
    CHECK(xml.find("classname=\"cmltest.msbuild.RelWithDebInfo.clang-cl\"") != std::string::npos);
}

TEST_CASE("report: xml escaping drops what XML 1.0 cannot carry") {
    CHECK(xml_escape("a<b>&\"'") == "a&lt;b&gt;&amp;&quot;&apos;");
    CHECK(xml_escape(std::string("x\x01y\x1F" "z", 5)) == "xyz");
    CHECK(xml_escape("tab\tnl\ncr\r") == "tab\tnl\ncr\r");
    CHECK(xml_escape("caf\xC3\xA9") == "caf\xC3\xA9");
}
