#pragma once

// The `restart` step (WP-21): control command `restart`, then the checks that the relaunched process really replaced
// the old one. Kept apart from runner.cpp / scenario.cpp, which only register the step.
//
//   [[step]]
//   kind = "restart"
//   within_s = 90                   # default 90
//   process = "fixture"             # the process to restart (default: the first launched)
//   expect_result_key = "patched"   # optional: an integer member of the restart result (e.g. the edits re-applied)
//   expect_result_value = 1
//
// The step fails when the restart is refused, when the new pid is not another process, when a module of the old
// process is not enabled again (`not_enabled`), when re-applying the session's edits failed (`reapply_error`), or
// when no `restarted` event with the same pids arrives.

#include <cstdint>
#include <string>

#include "scenario.h"
#include "toml.h"

#if defined(_WIN32)
#include "cml/ipc/control.h"
#endif

namespace cmltest {

// Portable: validates and reads the keys of a restart step (CML5411 on a bad one).
cml::Result<Step> parse_restart_step(const toml::Table& table);

struct RestartStepOutcome {
    bool ok = true;
    std::uint32_t diag_code = 0;  // CML5840 / CML5841 when !ok
    std::string detail;
    std::uint32_t old_pid = 0;
    std::uint32_t new_pid = 0;
};

#if defined(_WIN32)
// Runs the step against the broker behind `control` for the process `old_pid`. `limit_ms` is what is left of the step's
// time (and of the scenario's).
RestartStepOutcome run_restart_step(cml::ipc::ControlClient& control, std::uint32_t old_pid, const Step& step, std::uint32_t limit_ms);
#endif

}  // namespace cmltest
