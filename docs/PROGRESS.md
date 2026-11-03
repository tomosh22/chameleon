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
| WP-05 | Toolchain command parsing and rewriting | 2 | done (post-merge fixes #15, #21) | wp/05-toolchain | #13, #15, #21 | — |
| WP-06 | cml_coff writer: anchors and binding object | 2 | done | wp/06-coff-writer | #11 | — |
| WP-13 | Test harness cmltest | 2 | done (partial scenario per ADR-0022 until WP-12) | wp/13-cmltest | #12 | — |
| WP-07 | Bind planner (settles ADR-0010; ADR-0028/0029) | 3 | done | wp/07-bind-planner | #19 | — |
| WP-08 | Address-space reservation and patch loading (agent) | 3 | done | wp/08-agent-loading | #18 | — |
| WP-09 | Linker driver (Q2 settled: ADR-0027) | 3 | done | wp/09-linker-driver | #17 | — |
| WP-10 | Hook installer (agent) | 3 | done | wp/10-hook-installer | #20 | — |
| WP-12 | Broker core (orchestrator integration; ADR-0030) | 4 | done — all 7 M1 scenarios green in all 8 cells under `--require-all-capabilities` (verified by orchestrator from CI logs) | wp/12-broker-core | #23 | — |
| WP-14 | File watcher and change detection | 5 | done (FU-13 closed: `reload_twice` scenario) | wp/14-file-watcher | #26 | — |
| WP-15 | Header dependency handling | 5 | done | wp/15-header-deps | #28 | — |
| WP-16 | Multiple modules and multiple processes | 5 | done (FU-12, FU-15 closed) | wp/16-multi-module-process | #25 | — |
| WP-17 | Pre/post patch hooks, compile hooks, synchronised mode | 5 | done (FU-06, FU-14 closed) | wp/17-hooks-sync | #29 | — |
| WP-18 | Diagnostics pass and build-requirement checks | 5 | done | wp/18-diagnostics | #31 | 0d97b48 |

## Orchestrator integration work

| Item | Status | PR |
|---|---|---|
| Housekeeping docs (DECISIONS, KNOWN_LIMITATIONS, PROGRESS) | done | #1 |
| Shared fixture `basic` + `cml_add_fixture`; flaky log test fix | done | #3 |
| M1 fixtures statics/virtuals/templates/lambdas/new_function/new_global | done | #14 |
| Make `extras.cpp` opaque so constructs survive clang-cl /O2 (WP-02 note) | done | #8 |
| `unit.agent_hook` hang fix (in-window allocation in the negative hardening test); cdb stacks on hang; CI repeats `unit.agent_hook` x25 per cell | done | #30 |
| CI: run every checked-in scenario; concurrency groups (cancel superseded runs, never on main) | done | #24, #27 |

## Tracked follow-ups (minor review items deferred per ADR-0018)

| Id | Origin | Item | Owner |
|---|---|---|---|
| FU-01 | WP-11 | `stop()` status codes: distinguish "not running" from "timed out" | done (WP-18, #31) |
| FU-02 | WP-11 | Idle trimming of completed (`Done`) answers in the agent's answer table | done (WP-18, #31) |
| FU-03 | WP-11 | Console `CTRL_CLOSE_EVENT` handler in the broker | **done** (#23) |
| FU-04 | WP-05 | `""` quoting parity with the MSVC argv rules in the response-file writer | done (WP-18, #31) |
| FU-05 | WP-04 | Replace the private RSDS reader in `core/cml_pdb/src/detail/pe_codeview.cpp` with cml_pe (tried in WP-12 and reverted: cml_pe treats NB10 records and unterminated paths differently, so three `unit.pdb` assertions would change; decide the intended behaviour first) | done (WP-18, #31) |
| FU-06 | WP-11 | `DestroyAgent` waits with `Sleep(1)`; use an event | done (WP-17, #29) |
| FU-07 | WP-05 fix (#15) | No-job fallback (AssignProcessToJobObject failed) kills only the driver on cancel/timeout | done (WP-18, #31) |
| FU-08 | WP-05 fix (#15) | `is_pdb_server` treats an image-name query failure (e.g. >520-char path) as mspdbsrv; use a growing buffer | done (WP-18, #31) |
| FU-09 | WP-11 fix (#16) | Test watchdog only fires inside a test case; hangs in static destructors or doctest teardown stay silent until ctest TIMEOUT | WP-13 owner / orchestrator |
| FU-10 | WP-08 (#18) | Dedup `hex()` (3 copies) into cml_util; `restore_reservation` should reuse `proc::query_free_regions`; single rollback helper in `try_load_candidate`; also `hex()` in pipeline.cpp/hook_narrowing.cpp and the stat helper in module_symbols.cpp/source_baseline.cpp (#23) | done (WP-18, #31) |
| FU-11 | WP-09 (#17) | Linker output parsing: match only real linker diagnostics (not any ": warning: " line); dedup trim/upper/read_u32 with invoke_common and cml_pe | done (WP-18, #31) |
| FU-12 | WP-07 (#19) | Bind planner normalises compiland names repeatedly (O(G*N)); cache normalised names once per compiland | done (WP-16, #25) |
| FU-13 | WP-12 (#23) | No real-process test of a second reload of the same TU (patch 2 binding to patch 1's new symbols); covered only with fakes in `test_reload.cpp` | done (WP-14 `reload_twice`, #26) |
| FU-14 | WP-12 (#23) | No real-process test of synchronised-mode hooks (`SyncPoint()`, CML5320 path) | done (WP-17 `sync_point_apply`/`sync_point_late`, #29) |
| FU-15 | WP-12 (#23) | No real-process test of relink at another base after CML4203/4205 | done (WP-16 `relink_other_base`, #25) |
| FU-16 | WP-15 (#28) | No unit test combines compile progress events with compile-cache hits (WP-16 cache) | next broker WP |
| FU-17 | WP-16 (#25) | Forced-collision accounting (`CML_TEST_FORCE_LOAD_COLLISIONS` counter decremented only on a successful reservation) has no unit test; covered only by `relink_other_base` | next agent-loader WP |
| FU-18 | WP-17 (#29) | `unit.agent_hook` died with an empty SEGFAULT once (push run 41590303876, windows-2022 msvc Debug, d06f83c). The fix (`SetThreadStackGuarantee`) rests on an inference (stack overflow in the new hook test), not proof; no recurrence in 16 cells x 26 runs on b95bfcb. If it recurs: drop the stack-overflow hook test and capture a dump | orchestrator (watch WP10-REPEAT) |
| FU-19 | WP-18 (#31) | CML3103 (layout change of a type used by unchanged code) not implemented: needs TPI/UDT access in cml_pdb | a later pdb WP |
| FU-20 | WP-18 (#31) | CML3104 is advisory only: without /GL it cannot flag a genuinely stale copy (callers in the same TU are recompiled and re-hooked); kept as a reminder about still-executing callers. Revisit if /GL support lands | orchestrator |
| FU-21 | WP-18 (#31) | CML1101 for link.exe relies on the PDB's recorded /DEBUG option, because a real /DEBUG:FASTLINK PDB still lists function symbols (seen in CI, link 14.44). A PDB with no recorded linker command line and FASTLINK cannot be told apart from full | next pdb WP (look for a type-server/`/DEBUG:FASTLINK` marker in the PDB) |
