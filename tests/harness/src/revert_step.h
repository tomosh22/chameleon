#pragma once

// The `revert` scenario step (WP-22): control command `revert` with scope "last" or "all".
// Windows only (it talks to the broker's control pipe); compiled into cmltest.

#include <cstdint>
#include <optional>
#include <string>

#include "cml/ipc/control.h"
#include "cml/util/diag.h"
#include "scenario.h"

namespace cmltest {

struct RevertStepResult {
    std::string detail;
    std::optional<cml::Diag> diag;  // set: the step failed
};

// Sends `revert` {scope}. Passes when the broker reports at least one reverted module, or (step.expect_error) refuses
// with exactly that diagnostic id.
RevertStepResult run_revert_step(cml::ipc::ControlClient& control, const Step& step, std::uint32_t limit_ms);

}  // namespace cmltest
