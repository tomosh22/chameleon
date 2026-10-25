# Chameleon — Progress

Status values: `todo`, `in progress`, `in review`, `blocked`, `done`. "Done" = acceptance green
on every CI matrix cell + reviewed + `docs/wp/WP-NN.md` present + merged.

| WP | Title | Wave | Status | Branch | PR | Blockers |
|---|---|---|---|---|---|---|
| WP-01 | Repository skeleton and CI | 0 | done | wp/01-skeleton-ci | #2 | — |
| WP-02 | cml_coff and cml_pe readers | 1 | done | wp/02-coff-pe-readers | #4 | — |
| WP-04 | cml_pdb index | 1 | done | wp/04-pdb-index | #5 | — |
| WP-11 | Agent skeleton, public header and IPC | 1 | done (post-merge fix #16: ScriptedBroker shutdown race, test watchdog) | wp/11-agent-ipc | #6, #16 | — |
| WP-03 | End-to-end spike (M1 gate) | 2 | done — **gate passed** (verified by orchestrator from CI logs, all 8 cells) | wp/03-e2e-spike | #10 | — |
| WP-05 | Toolchain command parsing and rewriting | 2 | done (post-merge fix #15: never kill mspdbsrv on cancel) | wp/05-toolchain | #13, #15 | — |
| WP-06 | cml_coff writer: anchors and binding object | 2 | done | wp/06-coff-writer | #11 | — |
| WP-13 | Test harness cmltest | 2 | done (partial scenario per ADR-0022 until WP-12) | wp/13-cmltest | #12 | — |
| WP-07 | Bind planner | 3 | in progress | wp/07-bind-planner | — | — |
| WP-08 | Address-space reservation and patch loading (agent) | 3 | in review (7 high-effort findings) | wp/08-agent-loading | #18 | — |
| WP-09 | Linker driver (settles §13 Q2: lld-link default?) | 3 | in review (6 findings) | wp/09-linker-driver | #17 | — |
| WP-10 | Hook installer (agent) | 3 | in progress | wp/10-hook-installer | — | — |
| WP-12 | Broker core (orchestrator integration) | 4 | todo | — | — | wave 3 |

## Orchestrator integration work

| Item | Status | PR |
|---|---|---|
| Housekeeping docs (DECISIONS, KNOWN_LIMITATIONS, PROGRESS) | done | #1 |
| Shared fixture `basic` + `cml_add_fixture`; flaky log test fix | done | #3 |
| M1 fixtures statics/virtuals/templates/lambdas/new_function/new_global | done | #14 |
| Make `extras.cpp` opaque so constructs survive clang-cl /O2 (WP-02 note) | done | #8 |

## Tracked follow-ups (minor review items deferred per ADR-0018)

| Id | Origin | Item | Owner |
|---|---|---|---|
| FU-01 | WP-11 | `stop()` status codes: distinguish "not running" from "timed out" | WP-12 |
| FU-02 | WP-11 | Idle trimming of completed (`Done`) answers in the agent's answer table | WP-12 |
| FU-03 | WP-11 | Console `CTRL_CLOSE_EVENT` handler in the broker | WP-12 |
| FU-04 | WP-05 | `""` quoting parity with the MSVC argv rules in the response-file writer | WP-18 |
| FU-05 | WP-04 | Replace the private RSDS reader in `core/cml_pdb/src/detail/pe_codeview.cpp` with cml_pe | WP-12 |
| FU-06 | WP-11 | `DestroyAgent` waits with `Sleep(1)`; use an event | WP-12 |
| FU-07 | WP-05 fix (#15) | No-job fallback (AssignProcessToJobObject failed) kills only the driver on cancel/timeout | WP-18 |
| FU-08 | WP-05 fix (#15) | `is_pdb_server` treats an image-name query failure (e.g. >520-char path) as mspdbsrv; use a growing buffer | WP-18 |
| FU-09 | WP-11 fix (#16) | Test watchdog only fires inside a test case; hangs in static destructors or doctest teardown stay silent until ctest TIMEOUT | WP-13 owner / orchestrator |
| FU-10 | WP-08 (#18) | Dedup `hex()` (3 copies) into cml_util; `restore_reservation` should reuse `proc::query_free_regions`; single rollback helper in `try_load_candidate` | WP-12 |
| FU-11 | WP-09 (#17) | Linker output parsing: match only real linker diagnostics (not any ": warning: " line); dedup trim/upper/read_u32 with invoke_common and cml_pe | WP-18 |
