# Chameleon — Architecture Decision Records

ADR style. Each record: context, decision, consequences. Decisions marked DECIDED in
`IMPLEMENTATION_PLAN.md` are not repeated here unless something was learned about them.

---

## ADR-0001 Build host and verification path

**Date:** 2026-10-24 · **Status:** Accepted

**Context.** The plan (§0.8) requires MSVC or clang-cl on Windows for everything in M0–M6.
The orchestrator runs in a cloud container:

| Item | Value |
|---|---|
| Host OS | Linux x86_64, Ubuntu 24.04.5 LTS (kernel 6.18) |
| MSVC (`cl.exe`, `link.exe`) | not available |
| clang-cl | not available (no `clang-cl` driver installed, no Windows SDK / MSVC CRT headers) |
| clang | 18.1.3 (Ubuntu) |
| lld-link | available (LLVM 18), but no Windows SDK libs to link against |
| CMake / Ninja | 3.28.3 / available |
| Wine | not available |
| Cores | 4 |

**Decision.** No acceptance check is ever performed locally. Every WP is verified by pushing
its branch and reading the GitHub Actions Windows matrix (windows-2022, windows-2025 × MSVC,
clang-cl × Debug, RelWithDebInfo). A subagent's claim of success or a Linux build is never
accepted as evidence. Local Linux tooling may be used only for quick syntax sanity checks of
portable code and for inspecting artifacts downloaded from CI.

**Consequences.**
- Iteration latency is CI latency; subagents must batch fixes and read job logs carefully.
- WP-03 (spike gate) needs an interactive Windows step ("debugger shows the patched function
  with symbols"). Because the orchestrator cannot attach Visual Studio, the spike is verified in
  CI by (a) the fixture's printed value changing and (b) a scripted debugger check
  (`cdb.exe` from the Windows SDK Debugging Tools, present on GitHub windows images) that
  loads the patch module, resolves the patched function's symbol and source line, and prints
  them. The orchestrator reads that output from the CI log. The user may additionally run the
  VS check by hand; see ADR in WP-03 notes.
- CI artifacts (logs, PDBs, dumps) must be uploaded by workflows so they can be inspected.

## ADR-0002 Branch and integration workflow

**Date:** 2026-10-24 · **Status:** Accepted

- Each WP is developed on `wp/NN-short-title` and lands via a PR against `main`.
- Orchestrator housekeeping (this file, `PROGRESS.md`, `KNOWN_LIMITATIONS.md`) is committed on
  the orchestrator branch and merged to `main` through small PRs.
- A PR merges only when: CI is green on every matrix cell on its head, the code-review skill
  has been run (medium; high for `agent/`, hook installer, thread suspension, retargeter, bind
  planner) and its findings addressed, and `docs/wp/WP-NN.md` exists.
- Merge method: squash, so `main` history is one commit per WP.

## ADR-0003 Diagnostic id allocation

**Date:** 2026-10-24 · **Status:** Accepted

Ids for the by-design limitations in plan §1/§4.2 are pre-allocated in
`KNOWN_LIMITATIONS.md` so WPs implementing them use the same numbers. Within each family,
`x0xx` is reserved for generic/plumbing errors, `x1xx` for by-design limitations, and `x2xx+`
for everything else, allocated by the WP that introduces it (record new ids in the WP doc;
the orchestrator folds limitation ids into `KNOWN_LIMITATIONS.md`).

## ADR-0004 GUI technology for M3 (plan §13 Q4)

**Date:** 2026-10-24 · **Status:** Accepted

**Decision.** Ship the Dear ImGui + D3D11 front-end (WP-19) as planned; no interim Win32 tray UI.
Until WP-19 lands, the headless CLI and control pipe (WP-12) are the only UI.

**Reason.** The crash dialog (WP-20) and clickable compile output need a real UI; building a
throwaway tray app first would cost a WP for no lasting value. ImGui is already vendored (WP-01).

## ADR-0005 Default editor-open commands (plan §13 Q5)

**Date:** 2026-10-24 · **Status:** Accepted

Defaults used by WP-19 for "open file at line" (user-overridable, `{file}` `{line}` `{col}` placeholders):

| Editor | Command |
|---|---|
| VS Code (default if `code` is on PATH) | `code -g "{file}:{line}:{col}"` |
| Visual Studio | `devenv.exe /edit "{file}" /command "Edit.GoTo {line}"` |
| Rider | `rider64.exe --line {line} "{file}"` |
| Fallback | `ShellExecute` "open" on `{file}` |

Detection order: user setting → VS Code on PATH → `devenv.exe` via vswhere → fallback.

## ADR-0006 Build-system facts established by WP-01

**Date:** 2026-10-25 · **Status:** Accepted

- **Static CRT project-wide** (`/MT`, `/MTd`), not only for `agent/`, because core static
  libraries link into the agent and mixing runtimes fails with LNK2038.
- **`_HAS_EXCEPTIONS=0` for every first-party and third-party target** so inline STL code is
  compiled one way per binary (no ODR mix). Tests, GUI and third-party code still compile with
  `/EHsc /GR`, so doctest can throw; `core/` and `agent/` use `/EHs-c- /GR-`. **Fixtures are the
  exception**: they model user projects and keep the normal STL (`tests/fixtures/CMakeLists.txt`).
- **`fail_fast` test hook.** `cml::set_fail_fast_handler` lets unit tests throw out of
  `fail_fast` to test abort paths. That unwinds through one `/EHs-c-` frame in `cml_util`. This
  works on x64 (table-based unwinding; no destructors in that frame) and is used only by tests.
  Production code never installs a handler.
- **CMake 3.24 minimum** (3.22 is needed for `/external:I`; 3.24 leaves some margin).
- **Toolchains in CI:** windows-2022 has VS 2022 17.14 (MSVC 14.44). The windows-2025 image's
  default developer environment is **VS 18 (MSVC 14.51)**, not VS 2022, so the matrix covers
  two MSVC generations. clang-cl comes from the preinstalled `C:\Program Files\LLVM`. An extra
  job tests the "Visual Studio 17 2022" generator on windows-2022.
- **raw_pdb licence is BSD-2-Clause**, not MIT as plan §3 says. It is still permissive and
  compatible. Pinned commits are in `third_party/README.md`.
- **xxHash:** only `xxhash.h` and `xxh3.h` are vendored. Its GPL-licensed CLI is not.

## ADR-0007 Shared fixture `basic` is orchestrator-owned integration work

**Date:** 2026-10-25 · **Status:** Accepted

WP-02 (COFF/PE tests), WP-04 (PDB tests) and WP-11 (agent integration) all need fixture
`basic`, and wave 1 runs them in parallel. To avoid three agents creating it concurrently, the
orchestrator authors `tests/fixtures/basic` and the `cml_add_fixture` helper before wave 1.
Layout: `logic.cpp` is the patch target with exactly two externals (`g_tick` data,
`fixture_note` function), as the WP-03 spike requires. `extras.cpp` holds the WP-02 coverage
items (COMDAT, static data, string literals, `.pdata/.xdata` with C++ EH, `.CRT$XCU`,
`/alternatename` plus clang `weak`, and `__imp_`). WP-11 adds the agent link to fixtures.

## ADR-0008 Diagnostic sub-ranges per work package

**Date:** 2026-10-25 · **Status:** Accepted

So parallel WPs never collide (refines ADR-0003):

| Range | Owner |
|---|---|
| CML1200–1299 | WP-02 COFF reader |
| CML1300–1399 | WP-02 PE reader |
| CML1400–1499 | WP-04 PDB index |
| CML1500–1599 | WP-05 toolchain parsing |
| CML2200–2299 | WP-05 compile invocation |
| CML3200–3299 | WP-07 bind planner |
| CML3300–3399 | WP-06/WP-09 COFF writer, linker driver |
| CML4200–4299 | WP-08 reservation and patch load |
| CML4300–4399 | WP-10 hook installer |
| CML5200–5299 | WP-11 agent lifecycle and IPC |
| CML5300–5399 | WP-12 broker pipeline |

## ADR-0009 PDB index benchmark strategy (WP-04)

**Date:** 2026-10-25 · **Status:** Accepted

Plan §8 WP-04 allows LLVM or a synthetic PDB. **Decision: synthetic.** A Python generator in the
repo emits a large C++ project (many TUs, functions, UDTs, statics). A dedicated CI job
(windows-2022, MSVC, RelWithDebInfo) builds it with `/Zi /DEBUG:FULL` and caches the result with
`actions/cache`, keyed by the generator's hash. The benchmark asserts the §8 budgets: 2 s per
500 MB, scaled linearly to the actual PDB size, with 2× slack for shared runners, and memory
at most 1.5× the PDB size. It always prints the measured numbers. The job is a separate
required check; it does not run in the 8 build cells. An LLVM-scale run can be added to the
WP-27 perf suite.

## ADR-0010 Open issue: matching obj symbols of statics to PDB symbols

**Date:** 2026-10-25 · **Status:** Open (WP-03 spike to inform, WP-07 to decide)

Plan §4.5 keys statics by (mangled name, object file). Module symbol records (`S_LPROC32`,
`S_LDATA32`) carry display names, not the obj's decorated names, and statics have no
`S_PUB32`. WP-04 therefore exposes, per compiland symbol, the display name, kind, RVA and size,
plus an RVA→public map for externals. The bind planner must bridge decorated obj names to PDB
records. Candidate methods: undecorate the obj name and compare with the display name scoped by
compiland; or match by section contribution plus offset order. The spike records what works.

## ADR-0011 WP-03 debugger acceptance via cdb.exe in CI

**Date:** 2026-10-25 · **Status:** Accepted (approved by the project owner)

The plan's WP-03 acceptance item "debugger (VS) shows the patched function with symbols" is
satisfied by a scripted `cdb.exe` session in CI. The session attaches to the running fixture
after the patch is applied, lists the patch module, resolves the patched function to the
patch DLL with its source file and line from the patch PDB, and prints the result. The
orchestrator reads that output from the CI log. No manual Visual Studio check is required for
the gate.

## ADR-0012 Compile commands come from `S_BUILDINFO`, not `S_ENVBLOCK` (corrects plan §4.3)

**Date:** 2026-10-25 · **Status:** Accepted · **Source:** WP-04 CI evidence

Neither cl 19.51 (`/Zi`) nor clang-cl 20.1.8 emitted `S_ENVBLOCK` for the fixture's
compilands. Both emit `S_BUILDINFO`, which refers to an `LF_BUILDINFO` record in the IPI
stream; only CRT assembly objects carried `S_ENVBLOCK`. `cml_pdb::PdbIndex::build_env()`
returns `S_ENVBLOCK` when present, else `S_BUILDINFO`, under the same keys (`cwd`, `cl`, `src`,
`pdb`, `cmd`).

Consequences:
- **cl** records the real cl command line plus `pdb` (the compiler PDB, e.g. `vc140.pdb`).
- **clang-cl** records the **cc1** command line (`"-cc1" "-triple" ...`, `"-D" "X"` pairs,
  `-internal-isystem`) and an empty `pdb`. WP-05 must either translate cc1 back into a
  clang-cl invocation or replay it as `clang-cl -cc1 ...` (to be decided by spike in WP-05),
  with `compile_commands.json` as the fallback.
- Diagnostic CML1102 ("no recorded command line") triggers when neither record exists.

## ADR-0013 The PDB file list is not an include graph for clang-cl (affects WP-15)

**Date:** 2026-10-25 · **Status:** Accepted · **Source:** WP-04 CI evidence

cl's DBI file list contains every header it opened. clang-cl lists only files that
contributed line information: `logic.h` (declarations only) is absent from `logic.obj`. Header →
compiland mapping for clang-cl compilands therefore needs another source. WP-15 will use the
first available of: `/showIncludes` (or `-MD` depfile) captured on first recompile and cached
per compiland, `compile_commands.json` plus a dependency scan, or the PDB list (cl only).
Until then, header changes on clang-cl builds may miss compilands whose headers contain no
code.

## ADR-0014 `cml_pdb` parses MSF/DBI/CodeView itself; raw_pdb is a validation gate

**Date:** 2026-10-25 · **Status:** Accepted

Plan §3/§6 describe `cml_pdb` as a wrapper over raw_pdb. WP-04 found that raw_pdb's stream
classes trust file offsets and assert on bad input, which is incompatible with
"diagnose, never crash" and WP-28 fuzzing. `cml_pdb` therefore uses raw_pdb's `ValidateFile`
only as a gate and does all parsing with its own bounds-checked readers. The benchmark meets the
budgets with this design: a 544 MiB PDB loads in 0.37 s. raw_pdb stays vendored and unmodified.
This is an implementation choice within the DECIDED architecture, not a change to it.

## ADR-0015 Object-format differences between cl and clang-cl (from WP-02)

**Date:** 2026-10-25 · **Status:** Accepted (reference for WP-06/07/09)

| Aspect | cl | clang-cl |
|---|---|---|
| Section names | `.text$mn`, `.text$di`, `.xdata$x`, `.rdata$r` | plain `.text`, `.xdata`, `.rdata` |
| Function COMDAT selection | non-inline `NoDuplicates`; inline/templates `Any` | `Any` |
| `.pdata`/`.xdata` | associative COMDATs, including leaf functions | associative COMDATs |
| EH personality | `__CxxFrameHandler4` | `__CxxFrameHandler3` |
| `.CRT$XCU` target | `??__E...` directly | `_GLOBAL__sub_I_<file>` wrapper (inlined when optimised) |
| Weak externals | none (`/alternatename` is only a `.drectve` entry) | true `WEAK_EXTERNAL` for `__attribute__((weak))` |
| String literals | Debug: static `$SG<n>` in `.rdata`; with `/GF`: `??_C@` COMDAT | `??_C@` COMDAT |
| REL32 variants | `REL32`, `REL32_1..4` | `REL32` only |
| `/bigobj` | always bigobj | ignored; bigobj only above 65279 sections; regular objects use unsigned 16-bit section numbers up to 65279 |

Linked images (both toolchains) have no `IMAGE_DLLCHARACTERISTICS_GUARD_CF`, but the load
config has `GuardFlags = CF_INSTRUMENTED` (from the `/guard:cf` CRT libraries). WP-18 must
detect `/guard:cf` from the DllCharacteristics bit plus a function table, not from GuardFlags
alone.

## ADR-0016 Agent runtime choices (from WP-11)

**Date:** 2026-10-25 · **Status:** Accepted

- **Pipe security.** The broker pipe has a protected DACL with a single ACE for the current
  user, rejects remote clients, and uses `FILE_FLAG_FIRST_PIPE_INSTANCE`. The agent connects at
  `SECURITY_IDENTIFICATION` and verifies the server process runs as the same user (CML5209).
- **Module notifications** use polling: a toolhelp diff every 500 ms and before each
  enable/disable. `LdrRegisterDllNotification` was rejected because its callback runs under the
  loader lock, which conflicts with WP-10's suspension rules.
- **Agent heap.** `operator new/delete` inside the agent route to a private `HeapCreate`
  heap. `heap::set_alloc_forbidden(true)` aborts on any allocation, and WP-10 sets it while
  host threads are suspended. CRT `malloc` and Win32-internal allocations are not covered;
  WP-10 must avoid those APIs in the suspension window.
- **The agent DLL is never unloaded** once started, because hooks will point into it.
- **Message ids** for every §4.13 family are fixed now (Hello=0x0001 … Restart=0x0061;
  table in `docs/wp/WP-11.md`).

## ADR-0017 WP-04 benchmark result and PDB file locking

**Date:** 2026-10-25 · **Status:** Accepted

Final benchmark numbers: a 544 MiB synthetic PDB (1131 compilands, 908k publics, 2.06M
contributions) loads in 238 ms, against a budget of 2 s per 500 MB. Real memory, measured as
private-bytes growth plus the mapped file, is 1.40× the PDB size, against a budget of 1.5×.

`cml_pdb` keeps the PDB memory-mapped for the index lifetime. A linker that rewrites a mapped
PDB fails with `ERROR_USER_MAPPED_FILE`. **WP-12 must release the module's index (or switch it to
a copy-in-memory mode) before anything relinks that module**, e.g. before hot-restart rebuilds.
Patch PDBs are separate files and are unaffected.

## ADR-0018 Review convergence policy

**Date:** 2026-10-25 · **Status:** Accepted

WP-11 needed six review rounds. Each high-effort review of the previous round's delta found
new edge cases, several of them introduced by the fixes themselves. Policy from now on:
1. The first review covers the whole PR at the required effort. Every finding with a realistic
   path gets fixed.
2. Later reviews cover only the fix delta and report only high-severity bugs: crash, UAF,
   deadlock, hang, lost requests, security.
3. When the same area keeps producing findings, the orchestrator stops issuing open-ended fix
   lists. It makes the design decision itself (exact rules, invariants), sends that as the spec,
   and merges on green CI. Remaining minor findings become tracked follow-ups (`PROGRESS.md`).
4. Prefer reverting a fix that caused regressions over patching it again.

## ADR-0019 Agent API contract details settled in WP-11

**Date:** 2026-10-25 · **Status:** Accepted

- `cml_destroy_agent` / `cml::DestroyAgent` return `cml_status`. Destroy is refused, with
  `CML_STATUS_INVALID_ARGUMENT`, when it is called from inside another agent call on the same
  thread, e.g. from work run in `SyncPoint()`. The ABI version stays at 2: no external
  consumer existed before this merge.
- `DestroyAgent` must not race with other calls on the same agent, except threads already
  blocked in `WaitForRequest`. Those are released with CML5215 and destroy waits for them to
  leave.
- Only an explicit, successful `DisableModule` removes a module from the agent's enabled set.
  Refused internal re-enables are retried after the next accepted module list.
- Auto-started broker launches back off 1 s → 60 s; refused module-list resends back off
  1 s → 30 s. Both back-offs reset once a module list is accepted.

## ADR-0021 Control-pipe protocol for tests and IDEs (WP-13 defines, WP-12 implements)

**Date:** 2026-10-25 · **Status:** Accepted

- **Transport:** named pipe `\\.\pipe\cml_control_<logon-session SID>` with the same
  current-user-only DACL as the agent pipe. Overrides: `CML_CONTROL_PIPE` and
  `cml_broker --control-pipe NAME`.
- **Framing:** newline-delimited JSON in UTF-8.
  - Request: `{"id","cmd","args"}`.
  - Response: `{"id","ok","result","diag"}`.
  - Unsolicited events: `{"event",...}`.
  - JSON lines were chosen over the agent's binary framing because the consumers are IDE
    extensions (WP-30/31) and scripts.
- **Commands:** `status` (includes `capabilities`), `reload`, `wait`, `set-option`, `subscribe`.
- **Parsing:** a hand-rolled, bounds-checked JSON codec in `cml_ipc`. No new third-party
  dependency.

## ADR-0022 Harness capability gate until the pipeline exists

**Date:** 2026-10-25 · **Status:** Accepted (temporary)

WP-13's acceptance ("harness runs the `basic` scenario in CI") comes before WP-12 delivers
`reload`. cmltest therefore queries `status.capabilities` and reports any step whose capability
is missing, and all later steps, as **skipped**. It prints a loud `PARTIAL:` summary line.
Skipped steps never count as passes. WP-12's acceptance switches CI to
`--require-all-capabilities`, which turns skips into failures. That switch removes the gate.

## ADR-0023 Spike runs externally, not through the agent

**Date:** 2026-10-25 · **Status:** Accepted

WP-03 drives fixture `basic` from outside (`VirtualAllocEx`, remote `LoadLibraryW` thread,
`WriteProcessMemory`, external `SuspendThread`) as its "stub agent". WP-11's agent is not
involved. This isolates the gate from agent bugs. Patching from inside the agent is WP-08 and WP-10.
