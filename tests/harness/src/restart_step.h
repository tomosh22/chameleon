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
//   # WP-43 (failure injection in the restart, scenario restart_inject):
//   hold_new = true                 # restart {hold_new}: the step resumes the held process on `restarting`, from a second connection
//   hold_ms = 10000                 # with hold_new (default 10000)
//   expect_restarting = false       # with hold_new: no `restarting` may arrive (the hold could not be kept); default true
//   expect_error = "CML5804"        # the restart must be refused with this id (then nothing else is checked)
//   expect_error_text = "CML4657"   # ... and its message must contain this
//   expect_warning = "CML5973"      # a warning diag event with this id must come with the restart
//   expect_reapply_error = "CML5977"  # the result's reapply_error must have this id (instead of failing the step)
//   expect_not_enabled = "fixture_hot_restart.exe"  # the result's not_enabled must list a module of this name (instead of failing)
//
// The step fails when the restart is refused, when the new pid is not another process, when a module of the old
// process is not enabled again (`not_enabled`), when re-applying the session's edits failed (`reapply_error`), or
// when no `restarted` event with the same pids arrives. Before it sends `restart` it waits until the process's agent is
// connected (a process left running by a failed restart registers by itself, a moment later).
//
// After an expected refusal the step follows the process that runs: the old one when the refusal came before it was
// terminated, else the new one, named by the `pid` of the refusal's diag event.

#include <cstdint>
#include <string>
#include <vector>

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
    std::uint32_t new_pid = 0;  // the process to follow from now on (the old pid after a refusal that came before the termination)
};

#if defined(_WIN32)
// Runs the step against the broker behind `control` for the process `old_pid`. `limit_ms` is what is left of the step's
// time (and of the scenario's). `control_pipe` is the broker's control pipe (a held restart is resumed from a second
// connection to it).
RestartStepOutcome run_restart_step(cml::ipc::ControlClient& control, const std::vector<std::string>& capabilities, std::uint32_t old_pid,
                                    const Step& step, std::uint32_t limit_ms, const std::string& control_pipe);
#endif

}  // namespace cmltest
