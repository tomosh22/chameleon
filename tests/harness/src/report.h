#pragma once

// Scenario results, the console summary and the JUnit XML writer. Portable.
//
// Step statuses (a skipped step is never a pass, anywhere):
//   Passed            the step ran and its check held
//   Failed            the step ran and failed (or could not run)
//   SkippedCapability the broker lacks the capability the step needs (temporary gate until WP-12);
//                     this step and every later step of the scenario get this status
//   NotRun            an earlier step failed, so this one was not attempted
// Scenario outcome: Failed if any step failed; else Partial if any step was skipped for a missing
// capability (or Failed under --require-all-capabilities); else Passed.

#include <string>
#include <vector>

#include "cml/util/result.h"

namespace cmltest {

enum class StepStatus { Passed, Failed, SkippedCapability, NotRun };

const char* to_string(StepStatus status);

struct StepResult {
    std::string name;      // "build fixture", "expect_stdout /value=1/ within 10 s"
    StepStatus status = StepStatus::NotRun;
    std::string detail;    // failure text, skip reason, or a short note for a pass
    std::string diag_id;   // "CML5416" for failures that carry one
    double seconds = 0.0;
};

enum class Outcome { Passed, Partial, Failed };

const char* to_string(Outcome outcome);

struct ScenarioReport {
    std::string scenario;
    std::string generator;  // "ninja" / "msbuild"
    std::string config;     // "Debug" / "RelWithDebInfo"
    std::string compiler;   // "cl" / "clang-cl"
    std::vector<StepResult> steps;
    double seconds = 0.0;

    std::size_t count(StepStatus status) const;
    Outcome outcome(bool require_all_capabilities) const;
};

// Human-readable report: one line per step, then the summary. Contains, when steps were skipped for
// capabilities, the loud line `PARTIAL: N steps skipped for missing broker capabilities`.
std::string console_report(const ScenarioReport& report, bool require_all_capabilities);

// One machine-greppable line: CMLTEST_RESULT scenario=basic outcome=partial passed=5 failed=0 ...
std::string result_line(const ScenarioReport& report, bool require_all_capabilities);

// JUnit XML with one testcase for the scenario; the steps are listed in <system-out>. A partial
// scenario carries <skipped message="PARTIAL: ..."/>, a failed one <failure>.
std::string junit_xml(const ScenarioReport& report, bool require_all_capabilities);

// Escapes text for XML content/attributes and drops characters XML 1.0 cannot carry.
std::string xml_escape(const std::string& text);

cml::Result<void> write_text_file(const std::string& path, const std::string& content);

}  // namespace cmltest
