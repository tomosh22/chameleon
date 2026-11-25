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
| WP-18 | Diagnostics pass and build-requirement checks | 5 | done | wp/18-diagnostics | #31 | — |
| WP-19 | ImGui broker UI (`cml_ui`, ADR-0033) | 6 | done (window/tray/hotkey not seen on a display: manual checklist in WP-19.md) | wp/19-imgui-broker-ui | #33 | — |
| WP-20 | Crash → patch → continue | 6 | done (breakpoints deferred) | wp/20-crash-patch-continue | #34 | — |
| WP-21 | Hot-restart (`ScheduleRestart` API still not implemented) | 6 | done | wp/21-hot-restart | #36 | — |
| WP-22 | Patch revert (session reset deferred) | 6 | done | wp/22-patch-revert | #35 | — |
| WP-24 | Thread-local variables | 6 | done | wp/24-thread-local | #39 | — |
| WP-25 | Unchanged-function elision (scope narrowed, ADR-0034) | 6 | done | wp/25-unchanged-elision | #37 | — |
| WP-23 | Re-execute function after crash (ADR-0035) | 7 | done | wp/23-crash-reexecute | #43 (3230b6f) | CI 14/14 x2 at bff09cb verified (crash_reexecute 25/25 incl. windows-2025 msvc RelWithDebInfo, clang-cl Debug; agent_hook 25/25); 10 high findings fixed, delta checked |
| WP-26 | Real-project corpus (raylib, Dear ImGui, Godot; ADR-0035) | 7 | done | wp/26-corpus | #46 (a4a6c87) | corpus 5/5 cells x2 (push 45084902079, PR 45084906331) at 8c2ed0f verified (raylib clang-cl F-3 EXPECTED-FAIL as ruled); ci 12/14 x2. Both windows-2025 clang-cl cells fail STL1000 from the runner image change, on every branch (ADR-0037, fixed via WP-26c); accepted on that basis |
| WP-26b | Static-library members reloadable (F-1; ADR-0036) | 7 | done | wp/26b-static-lib | #49 (3f4f8cb) | ci 14/14 x2 (push 45109972292, PR 45109977074) at 19d761e verified; corpus 5/5 (45109972343), Godot static-lib-main and static-lib-core-message-queue PASS (F-1 flipped); medium review, 3 important findings fixed and re-checked |
| WP-26c | clang-cl C-statics crash after reload (F-3; ADR-0036) | 7 | done | wp/26c-c-statics | #50 (40a4218) | ci 14/14 x2 (push 45102108170, PR 45102112593) at f83fff9 verified (windows-2025 clang-cl on VS clang 22.1.3: c_statics 12/12, virtuals 9/9); corpus 5/5 (45102111033), raylib clang-cl header-inline-raymath and still-running-final PASS (F-3 flipped); high review, fix re-checked. Carries #48 (closed, superseded): ADR-0036, ADR-0037 |
| WP-29 | CMake integration module (ADR-0038) | 8 | done | wp/29-cmake | #55 (7e2c87c) | ci 14/14 x2 (push 45128886735, PR 45128892777) at d39d71b verified (cmake.enable.* 10/10, WP29-INSTALL layout=OK relocatable=OK, WP29-EXAMPLE check=PASS warn=0); head 61a25e5 is docs-only; medium review, 1 important + 6 nits fixed, delta checked |
| WP-30 | VS Code extension (ADR-0038) | 8 | done | wp/30-vscode | #54 (b8587a6) | ci 14/14 x2 (push 45129610852, PR 45129616087) and vscode x2 (45129610814, 45129616057) at 2db2ce6 verified (48/48 unit, WP30-IT reload=ok revert=ok against real broker, .vsix artefact); medium review, 5 important + 3 nits fixed, delta checked. Editor UI covered by the manual checklist only |
| WP-31 | Visual Studio extension (ADR-0038) | 8 | done | wp/31-vs | #53 (8cda582) | ci 14/14 x2 (push 45130410359, PR 45130417261) and vs (45130417306) at d89919b verified (62/62 unit, WP31-IT result=ok checks=31 failures=0 incl. restart and re-attach decision); medium review, 8 important + 2 nits fixed, delta checked. Editor UI and real Attach2 covered by the manual checklist only; early attach deferred (KNOWN_LIMITATIONS) |
| WP-32 | Packaging, docs, licensing hook (ADR-0038; merges last) | 8 | done | wp/32-package | #52 (8eb1696); acceptance #56 | ci 14/14 x2 (push 45144074992, PR 45144083886) and package (45144083921) at 0a93ac6 verified; medium review, 10 findings fixed, delta checked. Acceptance: quickstart-acceptance run 45187018820 at 02edf48, a script written by a no-context agent from docs/QUICKSTART.md alone, on clean windows-2022 with only the zip of package run 45187036163: QS-STEP 1-13 PASS (check 11 PASS 0 FAIL, find_package from the zip, reload ok:true patched=1, value=2 in the same process), QS-RESULT PASS. Found and fixed: cml-reload.ps1 pipe-name lookup (runner token has no logon group in whoami), QUICKSTART check-output wording. LICENSE proprietary (ADR-0039); package -Strict, 0 WP32-PENDING |
| WP-33 | chameleon_enable without cl warnings (ADR-0040) | 9 | done | wp/33-enable-warnings | #57 (5e7a1de) | ci 14/14 x2 at 74a1daa verified (WP33-WARN d9002=0 d9025=0, WP33-WARN-VS d9002=0, cmake.enable.* incl. helper_function, parent_for_subdir, child_for_parent); corpus and package green. Also fixed a pre-existing race in agent_hook test_hardening (offender not yet in the window). Four medium reviews: 3+2+2 important findings fixed (DEFER strip, per-target restore, language gate, ancestor walk), last delta checked |
| WP-34 | Early debugger re-attach after hot-restart (ADR-0040) | 9 | done | wp/34-restart-hold | #59 (9ff4508) | ci 14/14 x2 and vs at fe597f0 verified (integration.restart_hold_{resume,timeout,disconnect,not_held,broker_killed,two_holds}, WP34-VS hold/attach-failure/attach-blocks=ok, WP35 checks still green); three high reviews, 4+2 important findings fixed, third clean (nits in KNOWN_LIMITATIONS). Real VS attach to a suspended process is manual-checklist only |
| WP-35 | File and line on diag events (ADR-0040) | 9 | done | wp/35-diag-locations | #58 (38676cd) | ci 14/14 x2, vscode and vs at 25c4b84 verified (WP35-LOC line=12 col=0 cl / col=15 clang-cl, WP35-VSCODE result=ok C2059 line 12, WP35-VS result=ok line 12); medium review, 2 important + 4 nits fixed. Merged before WP-33 (no shared files) |
| WP-27 | Performance budgets | 7 | done | wp/27-perf-budgets | #42 (f39dc19) | CI 14/14 push 42460887634 + PR 42460890741 at 00e0ac6; pdb index 328 ms/544 MiB (budget 4352), reload excl. compile median 40.9 ms, idle CPU 0.000 %, large-PDB broker 219 MiB = 0.40x PDB, session broker peak 11.0 MiB (limit 1.5x PDB + 32 MiB baseline). Test-coverage gap: no reload measured inside the large PDB (benchmark has no agent). |
| WP-28 | Fuzzing and failure injection | 7 | done | wp/28-fuzz-injection | #44 (5b6dffb) | CI 14/14 x2 + fuzz 6/6 (incl. OFF build) at e41c1f5 verified; inject_stages 89/89, crash_reexecute 25/25, agent_hook 25/25 (windows-2025 clang-cl RelWithDebInfo); 8 high findings fixed, post-patch-hooks fires before commit; delta checked |
| WP-36 | Image-relative references to old data (R-01, F-2; ADR-0042) | 10 | done | wp/36-imgrel | #67 (4878597) | ci 14/14 x2 (push 45478082260, PR 45478094808), corpus 5/5 (45478094802, Godot) at 0698a3e verified: `CMLTEST_RESULT scenario=imgrel ... passed=14 failed=0` (8 cells), `WP36-IMGREL sites=10 verified=10` (cl RelWithDebInfo), raylib/msvc `edit:header-inline-raymath: PASS ... patched=3` with 97/97 sites proved, F-2 xfail removed. High review: 2 important + 1 nit fixed (32-bit lea, immediate placement, base-less SIB); high re-review clean, 4 nits deferred (R-50) |
| WP-37 | PDB type stream; static overloads and lambda-local statics (R-02, R-03; ADR-0042) | 10 | done | wp/37-pdb-types | #64 (2456392) | ci 14/14 x2 (push 45508103161, PR 45508112785 attempt 2), corpus 7/7 (45508112935, Godot), fuzz 10/10 at c627515 verified: Godot `edit:static-lib-scene-tree: PASS ... hooks=126` (R-03), ImGui cl three `static-overloads-*` PASS (R-02), `OPTIONS-NAMES ... guards=2 guards_bound=2` both compilers. One failed acceptance (Godot, block-number parser) and four high reviews; last review's over-refusals deferred to WP-40 (ADR-0018). Attempt 1 of the PR ci run failed only in the pre-existing unit.watch timing test (fixed in the close-out PR) |
| WP-38 | VEX/EVEX in the hook length decoder; decoder fuzzing (R-04, R-41; ADR-0042) | 10 | done | wp/38-vex-decoder | #65 (8de8b47) | ci 14/14 x2 (push 45456991699, PR 45457008741), corpus 7/7 (45457009071), fuzz 10/10 (45457008506), package at code head 3cd27ad verified (final head 24e4212 docs only): `WP10-STRESS ... crashes=0` + `WP38-STRESS-VEX cpu_avx2=1 targets=8` in all 8 cells, Godot `refused_vex=0` (main 220), `fuzz / lde` 10.5 M runs `disagreements=0` against Zydis, `WP38-DIFF instructions=3424419 mismatches=0`. High review: 1 important (CMake 3.24) + 4 nits fixed. Legacy maps tightened beyond the ADR, accepted with the exhaustive comparison against main's decoder (`delta_real=0`) |
| WP-39 | Reload timing inside a large PDB, with budgets (R-13; ADR-0042) | 10 | done | wp/39-large-pdb-reload | #63 (2453615) | ci 14/14 x2 (push 45422099800, PR 45422114245) and corpus 5/5 (45422114246) at b22d155 verified: `WP39-RELOAD pdb_mib=546.1 reloads=5 total_minus_compile_median_ms=109.3 max_ms=1340.4 budget_ms=3000`, `PERF broker_peak_private_mib=836.9 limit_mib=851.2`, Godot `reload-budget: PASS 6 reloads`, slowest per TU 972.8 ms. Medium review: nits only. Memory headroom of 14 MiB and the review nits go to WP-39b |
| WP-39b | The broker reads the module file once (follow-up to WP-39) | 10 | done | wp/39b-module-read | #66 (46bf967) | ci 14/14 (PR 45440733449), corpus, fuzz, package green at 7b94c1f (42/42 checks) verified; pdb-benchmark job 138610804960: `PERF broker_peak_private_mib=587.2 limit_mib=851.2` (main 836.9), `WP39-RELOAD ... median 109.3 max_ms=813.2` (main max 1340.4). Medium review: nits only (pe::read_file_headers on corrupt files, a pre-existing harness leftover) |
| WP-40 | Bind-planner pairing defects (R-48, R-50; ADR-0043) | 11 | in progress | wp/40-planner-pairing | — | — |
| WP-41 | Corpus: an /arch:AVX2, unity-built, static-library project (R-06; ADR-0043) | 11 | done | wp/41-corpus-avx2-unity | #69 (1fe83bb) | corpus 7/7 at 83e97f1 (PR 45581214040) verified: `CORPUS-RESULT project=jolt compiler=msvc pass=13 expected_fail=5 fail=0`, `jolt compiler=clang-cl pass=14 expected_fail=4 fail=0`, `godot-gdextension compiler=msvc pass=18 expected_fail=3 fail=0` (after merging main); ci 14/14. Jolt Physics v5.7.0 (MIT), `/arch:AVX2`, unity batches, static library: `decode ... refused_vex=0` (530 VEX entries under cl). Findings F-41.1..3 asserted with `match` (WP-46), F-41.4 recorded as R-52. Medium review; its fixes (match on events, refusal order shown fixed) checked |
| WP-42 | Corpus: a DLL-plugin architecture (R-06; ADR-0043) | 11 | done | wp/42-corpus-plugins | #70 (2e33ca6) | corpus 6/6 at ced5a04 (PR 45547023275) verified: `CORPUS-RESULT project=godot-gdextension compiler=msvc pass=18 expected_fail=3 fail=0 skipped=0`, `reload-budget: PASS 3 reloads, each within 3000 ms`. Found F-42.1..3 (edits lost on plugin reload / late load, patch path collision), asserted as expected failures with `match`; fixed by WP-45. Medium review, fixes re-checked |
| WP-43 | Restart-hold follow-ups; failure injection in restart and revert (R-40, R-42; ADR-0043) | 11 | done | wp/43-restart-injection | #71 (4bf051f) | ci 14/14 at 6dec4de (push 45547613555, PR 45547619893), vs, fuzz, package green; verified in every ninja cell: `CMLTEST_RESULT scenario=restart_inject ... passed=54 failed=0`, `scenario=revert_inject ... passed=44 failed=0`, `WP10-REPEAT ... unit.agent_hook runs=25 passed=25 failed=0`. Two high reviews; the second (fix delta) found five fault-injection-build-only races, deferred to WP-44 as R-51 (ADR-0018). Found and fixed a use-after-free in test_dispatch's `ScopedVar` (the intermittent SIGSEGV) |
| WP-44 | Failure injection in the crash handler and the watcher; persistent arming (R-42, R-49, R-51; ADR-0043) | 11 | in progress | wp/44-injection-rest | — | — |
| WP-45 | Re-apply session edits to re-loaded and late-enabled modules (F-42.1..3; ADR-0043) | 11 | in progress | wp/45-module-reenable | — | — |
| WP-46 | Planner coverage on unity builds (F-41.1..3; ADR-0043) | 11 | todo | wp/46-unity-planner | — | starts after WP-40 merges |

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
| FU-22 | WP-21 (#36) | When the broker has no console, it borrows the old process's console (`AttachConsole`) during the relaunch, so for that moment its own standard handles may refer to the user's console. Untested; restore the std handles around attach/free | next broker WP |
| FU-23 | WP-19 (#33) | `cml_ui` window, tray icon, hotkey, single instance and editor launch have never been exercised on a display; the manual checklist in WP-19.md is unchecked | needs a person on Windows |
| FU-24 | WP-19 (#33) | `patch_applied` carries no changed file names or timings, so the UI history is thin for broker-triggered (compile-on-save) reloads; add `changed_files` and `timing_ms` to the event | next broker WP |
| FU-25 | WP-24 (#39) | SECREL value comes from `section_offset_from_rva` of the record's RVA, not the record's own section offset; correct on CI (both compilers, threads keep values) but would break if a linker merged `.tls` into another section; also no check that a patch DLL has no TLS directory, and WP-24.md still says "inside the old `.tls` section" (the code decides by thread records) | next bind-planner WP |

## Handoff (2026-11-08, wave 7 paused)

Wave 6 complete. Wave 7 is paused at the user's request: WP-23, WP-27 and WP-28 are done, and WP-26 is open.

**Merged on main**
- WP-27 (#42, f39dc19).
- WP-23 (#43, 3230b6f).
- Orchestrator PR #45 (55c0bdc): LLVM install cache in the clang-cl probe jobs.
- WP-28 (#44, 5b6dffb).
- Main at f39dc19 is verified green (one re-run, for LLVM 504s only), and so is 3230b6f (run 42478478405). The runs for 55c0bdc and 5b6dffb are to be confirmed on resume.

**WP-26 (#46, head 53e4d43): open, not accepted.**
- The corpus workflow was green at fd81a45, all five cells: raylib and ImGui under cl and clang-cl, and Godot 4.5.1 under cl (cold build 1444 s).
- The `ci` pull_request run on 53e4d43 went green; its push run and the corpus run on the final head are not verified.
- Before accepting:
  1. Merge main (it conflicts with WP-28's lde/tests changes, if any).
  2. Verify all runs myself and do a medium review.
  3. Rule on F-1 (static-library members silently not reloaded): it needs a diagnostic, not silence.
  4. Fix F-3, or refuse explicitly: raylib rcore.c under clang-cl crashes after reload, probably static matching for C TUs in bind_plan.cpp. Planner trace output is in the corpus logs.
  5. Fold F-2 and F-4 into KNOWN_LIMITATIONS.
- VEX opcodes are counted as refused, not decoded; whether to decode them is a separate decision (it reverses a WP-10 limitation).

**Resumed 2026-11-22.** Main is green through 5b6dffb. #47 merged as 12d198a. ADR-0036 is in #48. WP-26 fixes, WP-26b and WP-26c are running.

**Wave 7 complete (2026-11-23).** Main 40a4218 went red on a wall-clock bound in test_json.cpp (108 ms vs 100 ms, windows-2025 msvc Debug); fixed in #51 with a ratio check.

**Wave 8 launched (2026-11-23):** WP-29..32 in parallel under ADR-0038. WP-32 merges last; its acceptance is a quickstart run by a no-context agent on a clean runner.

**Wave 8 complete (2026-11-23):** WP-29..32 done. ci 14/14 on #56 head 02edf48 (with vs, vscode, package -Strict and quickstart-acceptance green). Licence: proprietary (ADR-0039). Follow-ups in KNOWN_LIMITATIONS: chameleon_enable cl warnings, early re-attach, diag file/line.

**Wave 9 launched (2026-11-23):** WP-33..35, the wave-8 follow-ups, in parallel under ADR-0040, as the user asked.

**Wave 9 complete (2026-11-23):** WP-33..35 done. Merge order changed from ADR-0040's 33→35→34 to 35→34→33: the PRs shared no files and WP-33 needed further review rounds. Remaining follow-ups (WP-34 review nits, the real-VS suspended attach, CML1712 subdirectory case) are in KNOWN_LIMITATIONS.

**v1 plan complete (2026-11-23).** All WPs of the implementation plan are done. What remains for a competitive product is in `docs/ROADMAP.md` (ADR-0041); the recommended wave 10 is R-01, R-02/R-03, R-04, R-13.

**Wave 10 launched (2026-11-24):** WP-36..39 in parallel under ADR-0042 (R-01, R-02 + R-03, R-04 with decoder fuzzing, R-13).
Before the launch the orchestrator ran a compiler-output spike for R-01 (run 45402685481): the checked-in F-2 repro does not
reproduce the finding, and the roadmap's proposed direction would not have fixed raylib; ADR-0042 records the evidence and
the design chosen instead. The same PR adds `DecodeResult::disp_offset`/`disp_size` to the length decoder (used by WP-36,
kept by WP-38).

**Wave 10 complete (2026-11-25):** WP-36 (#67), WP-37 (#64), WP-38 (#65), WP-39 (#63) and the follow-up WP-39b (#66), under
ADR-0042. R-01, R-02, R-03, R-04, R-13 and the decoder part of R-41 are done. Corpus: F-2 and F-4 fixed, Godot `refused_vex`
220 → 0, Godot `scene_tree.cpp` reloads. New findings went to R-46..R-50; R-48 (pre-existing planner pairing defects) leads
wave 11.

**Wave 11 launched (2026-11-25):** WP-40..WP-43 under ADR-0043; WP-44 starts when the first of them merges.
**Wave 11 progress (2026-11-25):** WP-43 (#71) and WP-42 (#70) merged; WP-45 and WP-44 launched into the freed slots; WP-46 waits for WP-40.
