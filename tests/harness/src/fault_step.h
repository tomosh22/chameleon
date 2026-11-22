#pragma once

// The `inject_failure` scenario step (WP-28, ADR-0035): arms one failure-injection stage through the broker's control
// capability `fault-injection`. Kept apart from runner.cpp / scenario.cpp, which only register the step.
//
//   [[step]]
//   kind = "inject_failure"
//   stage = "link"            # a name from the broker's `fault-stages`, or "none" (disarm everything)
//   count = 1                 # optional, default 1: how many passes of the stage fail
//   expect_error = "CML5960"  # optional: the command itself must be refused with this id (bad-request checks)
//
// The step first asks `fault-stages` and fails when the stage is not listed (the scenario and the broker disagree about the
// stages); then sends `inject-failure` and checks the answer: {stage, armed:true} (armed:false for "none"). The failing
// reload, the diagnostic and the clean reload afterwards are ordinary `reload`, `expect_diag` and `expect_stdout` steps.

#include <cstdint>
#include <string>

#include "scenario.h"
#include "toml.h"

#if defined(_WIN32)
#include "cml/ipc/control.h"
#endif

namespace cmltest {

// Portable: validates and reads the keys of an inject_failure step (CML5411 on a bad one).
cml::Result<Step> parse_inject_step(const toml::Table& table);

struct InjectStepOutcome {
    bool ok = true;
    std::uint32_t diag_code = 0;  // CML5985 when !ok
    std::string detail;
};

#if defined(_WIN32)
InjectStepOutcome run_inject_step(cml::ipc::ControlClient& control, const Step& step, std::uint32_t timeout_ms);
#endif

}  // namespace cmltest
