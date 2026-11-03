#pragma once

// Harness steps for crash -> patch -> continue (WP-20): watch_exceptions, expect_exception, exception_action,
// expect_pending_exceptions. Their bodies live here; runner.cpp only dispatches to them (docs/wp/WP-20.md, "Harness steps").
//
//   watch_exceptions           control command `subscribe` for diag, patch_applied, log AND exception. The broker answers after the
//                              agents hold faulting threads (ExceptionConfig was acknowledged), so a fixture that faults right
//                              afterwards is held, not crashed. Needs capability "exceptions".
//   expect_exception           the next `exception` event (others are consumed on the way); optional `code` ("0xC0000005") and
//                              `function` (substring of the resolved function name) must match; the thread becomes "the current
//                              exception" of the following steps.
//   exception_action           control command `exception-action` for the current exception: continue | leave | ignore. With
//                              `expect_error = "CMLnnnn"` the action must be refused with that diagnostic.
//   expect_pending_exceptions  control command `exceptions`: exactly `count` threads wait for a decision.

#if defined(_WIN32)

#include <cstdint>
#include <string>

#include "cml/ipc/control.h"
#include "scenario.h"

namespace cmltest {

struct ExceptionStepResult {
    bool ok = true;
    std::string detail;
    std::uint32_t diag_code = 0;  // the CML5xxx id of a failure
};

class ExceptionSteps {
public:
    ExceptionStepResult watch(cml::ipc::ControlClient& control, std::uint32_t timeout_ms);
    // `limit_ms` is `step.within_s` or less when the scenario's own time runs out first (`limited_by_scenario`).
    ExceptionStepResult expect(cml::ipc::ControlClient& control, const Step& step, std::uint32_t limit_ms, bool limited_by_scenario);
    ExceptionStepResult act(cml::ipc::ControlClient& control, const Step& step, std::uint32_t timeout_ms);
    ExceptionStepResult pending(cml::ipc::ControlClient& control, const Step& step, std::uint32_t timeout_ms);

private:
    bool have_current_ = false;
    std::int64_t current_pid_ = 0;
    std::int64_t current_id_ = 0;
};

}  // namespace cmltest

#endif  // _WIN32
