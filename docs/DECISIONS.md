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

**Date:** 2026-10-25 · **Status:** Settled by WP-07 — see ADR-0029

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

**Resolution (WP-15).** The broker scans the include graph itself when a module is indexed
(`broker/cml_core/src/include_scan.cpp`): for every compiland, from the include directories,
forced includes and source file of its recorded command line, it follows `#include` through the
files on disk. This is the "dependency scan" option; it needs no compile, works for cl and
clang-cl alike, and tracks code-less headers from the first `reload` (a `/showIncludes` capture
only exists after a recompile, so a first header edit would have been missed). The scan is an
over-approximation (conditional includes are followed either way), which costs at most an extra
recompile. The PDB list stays in use and is united with the scan. `/showIncludes` capture is not
used. See `docs/wp/WP-15.md`.

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

## ADR-0020 WP-03 spike findings

**Date:** 2026-10-25 · **Status:** Accepted (evidence: CI run 40162572353 of branch `wp/03-e2e-spike`, details in `docs/wp/WP-03.md`)

The end-to-end mechanism of plan §4.1–§4.8 works in all 8 matrix cells (windows-2022 and
windows-2025, cl and clang-cl, Debug and RelWithDebInfo). Each finding is tagged **[asserted]**
(`run_spike.py` / `cml_spike` fails the job if it does not hold) or **[log only]** (observed in the
CI log, not enforced; re-check before relying on it).

1. **Static anchor symbols are invisible; anchors must be EXTERNAL.** [asserted] Neither link.exe nor
   lld-link writes obj-local (`IMAGE_SYM_CLASS_STATIC`) symbols to the PDB, neither as publics nor
   as module symbols: the spike adds a static probe `__cml_sanchor_<n>` next to every external
   anchor and fails if any probe is found anywhere (0 found in all cells). All anchorable sections
   (4 to 7 per cell) have their `__cml_anchor_<n>` as an S_PUB32 public, which `cml_pdb::find_public`
   returns (asserted n/n). The external anchors sit in COMDAT sections too (`.text$mn`/`.text`,
   `.xdata`, `.pdata`, `.rdata`, `.rtc$*`), appended after all original symbols.
   A function COMDAT's leader and its section anchor have equal RVAs (asserted, at least one
   comparison). Sections that get no anchor [log only]: `.debug*`, `.drectve`, `.sxdata`, `.gfids`,
   `.giats`, `.gljmp`, `.gehcont`, `.voltbl`, anything `LNK_INFO`/`LNK_REMOVE` (`.chks64`,
   `.llvm_addrsig`).
2. **The REL32 retarget is correct and proven.** [asserted] After the fix-up the displacement is
   decoded from the target's memory; `site + 4 + N + disp - in-place addend` must equal the OLD
   module's `g_tick` (module base + RVA from the old PDB), must differ from the placeholder in the
   patch, and the placeholder must still be zero. Behaviourally [asserted]: between the hook and the
   exercise the fixture prints no `NOTE` (37 to 97 ticks per cell); after resetting `g_tick` to 0
   exactly one `NOTE logic-start` appears (it comes from the OLD `fixture_note()` reached through
   thunk and slot). The in-place addend differs by compiler [log only]: cl emits `REL32_1` (addend 0)
   for `cmp dword ptr [g_tick], 0`; clang-cl emits plain `REL32` with addend -1. The fix-up must read
   the addend: `target = sym + addend; value = target - (site + 4 + N)`.
3. **Function-vs-data classification must come from the PDB, not the object.** [log only] cl marks
   undefined function externals DTYPE_FUNCTION, clang-cl does not (the spike logs a NOTE for each
   mismatch). Classification uses PDB publics (flag / executable section); an external found nowhere
   fails the bind step [asserted].
4. **`/hotpatch` is ignored by cl on x64** [log only] (`cl : Command line warning D9002: ignoring
   unknown option '/hotpatch'` in every msvc cell, also for the fixture build). Entry bytes seen in
   the logs [log only]: `40 57 48 83 ec 20` (cl Debug), `48 83 ec 28` (others). The 5-byte jump
   therefore splits an instruction (`sub rsp, 20h`); the stray bytes are never executed. WP-10 needs
   the length decoder.
5. **Padding.** [asserted: at least 6 bytes of 0xCC precede the old entry in every cell] The logs show
   7 (cl Debug), 8 (cl Release) and 16 (clang-cl) `CC` bytes. This cannot be told apart from 16-byte
   function alignment, so it is not evidence that `/FUNCTIONPADMIN` did anything.
6. **/RTC1 (cl Debug) adds externals `_RTC_InitBase` and `_RTC_Shutdown`** [log only] (code, from the
   CRT, via ADDR64 pointers in `.rtc$IMZ`/`.rtc$TMZ`). They are bound through thunks like any
   function and never run for the patch. clang-cl ignores `/RTC1`. No `/GS` cookie,
   `__security_cookie`, `__CxxFrameHandler*`, `__imp_*` or weak externals appeared (logic.cpp has no
   arrays or EH); other code is unproven.
7. **Linkers.** clang-cl cells use lld-link (`CMAKE_LINKER=C:/Program Files/LLVM/bin/lld-link.exe`),
   msvc cells link.exe 14.44 / 14.51 [log only: `LINKER-USED`]. The plan §4.6 command is accepted by
   both with exit code 0 [asserted]. Linking once WITHOUT `/IGNORE:4001,4254,4281` printed **0
   lines** (no warnings at all) in all 8 cells [log only: `SPIKE-LINK-NOIGNORE`], so `/IGNORE` was not
   needed for this input and no LNK4001/LNK4254/LNK4281 had to be dealt with. An earlier draft of
   this ADR implied warnings were involved; they were not.
8. **`/FIXED` load works**, including with bases above 4 GB. [asserted] The DLL is confirmed at exactly
   its `/BASE` after a remote `LoadLibraryW` (else the spike fails). The 64 MB block is reserved within
   +-1.5 GB and rel32 reach of the whole module [asserted]; it was directly adjacent below the image in
   every cell (gap 0 bytes; the distance log of the first version was wrong for blocks on the other
   side of the module and is fixed). The remote thread's stack allocation is not controlled; it did
   not collide [log only].
9. **Debugger (cdb -pv).** [asserted] `patch_1` is listed with `(private pdb symbols)` (cl) or
   `(pdb symbols)` (lld-link); `x patch_1!compute_value*` resolves to exactly the hook target; `ln`
   reports the patched copy `...\spike-work\src\logic.cpp(8)` and `lsa` shows `return 2;`; `u` at the
   old entry shows `jmp patch_1!compute_value (<addr>)` with the parsed address equal to the hook's
   new entry. cdb needs `.reload /f patch_1.dll` first (symbols are deferred). cdb 10.0.26100.4202 is
   at `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe` on both images [log only].
10. **cml_pdb is sufficient**: publics of the old module (functions, data, CRT `_RTC_*`), publics of
    the patch (anchors, `__cml_slot_*`, the patched function). The patch's module symbols contain
    only `compute_value` [log only]. ADR-0010 was not needed for externals.
11. **Tooling** [log only]: `git apply --ignore-whitespace` applies `edits/*.edit` to a copy outside
    any repository; Python 3 and git are on PATH on both images.

Anchor-rewriter pitfalls for WP-06: new symbols must go after the existing ones (relocation and
`.sxdata`/`.llvm_addrsig` symbol indices stay valid); the string table (4-byte size included) must be
at the end of the file and its first dword and the header's symbol count rewritten; long names need
string-table offsets that include the size field; regular COFF section numbers are 16-bit (bigobj
needs the 20-byte symbol form, not implemented).

Not exercised: the "RIP inside the window, retry" path of the hook (one thread, the first attempt
always succeeded), bigobj, non-trivial externals, unload on failure.

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

## ADR-0024 Section locators replace "anchor every section" (corrects plan §4.6)

**Date:** 2026-10-25 · **Status:** Accepted · **Source:** WP-03 and WP-06 CI evidence

Plan §4.6 adds a static `__cml_anchor_<n>` to every section. The evidence contradicts that:
- **Static anchors are invisible.** Neither linker writes them to the PDB (WP-03, WP-06).
- **External anchors break in discarded COMDATs.** They work in single-object links, but
  lld-link fails with "undefined symbol" when it discards a COMDAT as a duplicate of another
  object's copy. link.exe tolerates it.

**Decision.** Each section gets a *locator*:
- **Non-COMDAT section:** an external anchor `__cml_anchor_<tag>_<n>`. The tag is unique per
  object within one link: a content hash plus the compiland index.
- **COMDAT section:** its leader symbol, which is external and appears in the patch PDB as
  `S_PUB32`. RVA of the section = leader RVA − leader value. A discarded duplicate resolves to
  the kept copy, which is the correct retarget base.
- **Associative section** (`.pdata`/`.xdata`): no locator. These never need retargeting to old
  data; their relocations target patch-local functions or binding thunks.

The retargeter (WP-07/09/12) computes `site_va` from the locator. A relocation needing
retargeting inside a section without a locator is a CML33xx diagnostic. Section contributions
from the patch PDB are a cross-check: lld-link matches all sections, and link.exe all except
`.pdata`.

## ADR-0025 Fix-up formula reads the in-place addend (corrects plan §4.6)

**Date:** 2026-10-25 · **Status:** Accepted · **Source:** WP-03

The plan's REL32 formula omits the addend stored at the relocation site. cl emits `REL32_1`
with an addend of 0, while clang-cl emits plain `REL32` with an in-place addend of −1 for the
same access. The retargeter therefore computes `target = S + A` (A read from the original
object's section bytes at the site) and writes `target − (site + 4 + N)`. ADDR64 likewise adds
the in-place 64-bit addend.

## ADR-0026 Toolchain facts from WP-05

**Date:** 2026-10-25 · **Status:** Accepted

- **clang-cl replay:** the recorded cc1 command is replayed as `<clang> -cc1 <rewritten args>`.
  This is option (a). Translating cc1 back to driver flags was lossy, so it was rejected.
  Symbol sets match the original in clang 17.0.6, 18.1.8, 19.1.7 and 20.1.8. **The minimum
  supported clang-cl is 17** (plan §13 Q3). Older versions are untested.
- **cl PCH + `/Zi`:** forcing `/Z7` fails with C2855 against a `/Zi` PCH. Default mode
  `KeepZiPrivatePdb` keeps `/Zi /Yu /Fp` and points `/Fd` at a private copy of the recorded
  compiler PDB. This amends plan §4.4's "replace `/Zi` with `/Z7`" for PCH users.
- **`/Gy` detection (plan §13 Q1):** in priority order, an explicit command-line flag, then
  functions in COMDAT sections in the recorded object, then implied by other flags
  (`/ZI`, `/O1`, `/O2`), then absent.
- **`/hotpatch`:** the `S_COMPILE3` hotpatch bit is set for every x64 object, and cl ignores
  `/hotpatch` on x64 (D9002). The flag fact therefore means "x64 entry is patchable", not
  "option passed".
- **MSVC STL vs older clang:** STL 14.44+ rejects clang < 19 (STL1000) unless
  `_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` is defined. This affects users on clang 17/18 with
  current MSVC.

## ADR-0027 Patch linker mirrors the module's linker; lld-link is not the default (plan §13 Q2)

**Date:** 2026-10-25 · **Status:** Accepted · **Source:** WP-09 CI measurements (PR #17)

WP-09 timed the `basic` patch link with both linkers on the same objects in all 8 matrix cells (median of 5 links after a warm-up; full table in `docs/wp/WP-09.md`). link.exe was faster in every cell, by 4–16 ms. Every median was 26–46 ms, far inside the 300 ms budget.

**Decision.** The patch is linked with the family that linked the module. The family is detected from the module PDB's `* Linker *` compiland, cross-checked with the PE. A user override exists. lld-link has no speed advantage that would justify the cost of mixing toolchains.

**Related facts:**
- `/IGNORE:4001,4254,4281` is not passed by default. Neither linker printed anything without it. It remains available.
- link.exe warns LNK4210 (".CRT section exists") for patches with dynamic initialisers. The warning is surfaced as CML3357; the initialisers are run by the agent (WP-08, plan §4.10).
- **lld-link 20.1.8 crash.** lld-link 20.1.8 crashes (0xC0000005) linking a single anchored clang-cl object with `/DEBUG:FULL /NODEFAULTLIB` and nothing else. This happens when nothing is bound, e.g. the `new_function` fixture.
  - Workaround: the driver always links a binding object. If it has no content, it is a pad object holding `__cml_binding_pad`.
  - Re-check when the minimum LLVM moves.
- **Correction to WP-04's notes.** link.exe also writes an `S_ENVBLOCK` into the `* Linker *` compiland, so an env block alone identifies no linker family.
- **Optimisation flags the driver refuses (CML3360).** These would break section locators (ADR-0024) or the `/FIXED` base:
  - `/OPT:REF`, `/OPT:ICF[=n]`, `/OPT:SAFEICF`;
  - `/PROFILE`, `/INCREMENTAL`, `/DYNAMICBASE`, `/FIXED:NO`, `/FORCE*`.

  The driver also refuses `/DEBUG:NONE` and `/DEBUG:FASTLINK`, because the patch PDB must be full.

## ADR-0028 Associative sections get a locator on demand (amends ADR-0024)

**Date:** 2026-10-25 · **Status:** Accepted · **Source:** WP-07 CI evidence (PR #19)

ADR-0024 said associative sections never need retargeting. That is wrong for cl. EH funclets
live in `.text$x` associative COMDATs, and the funclet of a function with a thread-safe local
static references the bound guard `$TSS0`.

**Decision.**
- The bind plan lists `associative_locator_sections`: the associative sections that contain a
  site needing retargeting.
- The patch pipeline (WP-12) passes them to `cml_coff::add_anchors` with
  `AnchorOptions::anchor_associative`. Only those sections get an external anchor.
- Cost: when the parent COMDAT is discarded as a duplicate, the child's anchor is undefined and
  lld-link fails. Requesting anchors only for listed sections keeps that to the cases that need
  it. If it happens, it surfaces as a link diagnostic, not a silent error.

## ADR-0029 Matching statics, fix-up kinds and binding rules from WP-07 (settles ADR-0010)

**Date:** 2026-10-25 · **Status:** Accepted · **Source:** WP-07 (PR #19), `docs/wp/WP-07.md`

**Matching obj statics to PDB records (ADR-0010).**
- Match by display name within the same compiland, **only with an identical scope**: the same
  enclosing function, or file scope.
- If no record has the same scope, the symbol is new. There is no fallback to other scopes.
- Same-scope, same-name groups (overloads) are resolved only by exact qualified name or, for
  data, by the RVA the unchanged section layout predicts. Anything else is CML3214. Groups are
  never paired by position.
- cl records the guard `$TSS0` in the PDB. clang-cl does not, so the planner infers it from the
  unchanged layout, or uses a fresh guard plus warning CML3211.
- Name spellings differ:
  - cl spells local scopes with quotes and block numbers, and lambdas with a hash;
  - clang-cl names internal data with plain identifiers.
- **Gap:** cml_pdb has no type access, so a static function whose parameter types changed still
  matches its single same-named predecessor (KNOWN_LIMITATIONS).

**Binding rules.**
- Read-only data (no `MEM_WRITE`) is always patch-fresh. Its address changes across a patch.
- Every REL32 reference to a bind-to-old function (call or `lea`) is retargeted directly to the
  old function's address, so pointer identity matches old code.
- Thunks remain for ADDR32NB and for targets out of REL32 range (`thunk_fallback`).
- Data reachable only by REL32 from another module more than 2 GiB away is rejected at plan time
  with CML3205.

**New fix-up kind `SecRel32`.**
- Every thread-safe function-local static reads the CRT's `_Init_thread_epoch` through a SECREL
  relocation. The value is the target's offset inside its old image section, computed by the
  broker.
- WP-08's wire kinds are Rel32, Addr64 and SlotWrite. **WP-12 must add a plain 32-bit write kind
  to ApplyFixups**, or the broker must pre-apply the value. Without it, no patch with a
  function-local static can load.

**CRT functions the old image never linked** (for example `strlen` in `new_global`) resolve
from other modules' exports, step 5 of the resolution chain. WP-12's real `ProcessModules` must
include ntdll and ucrtbase.

**Hook narrowing.** The planner hooks every defined function present in the old image: up to 83
for STL-heavy TUs. WP-12 narrows the hook list to changed functions by comparing function bytes,
or the COMDAT checksum, against the old image.

## ADR-0030 Broker patch pipeline decisions from WP-12

**Date:** 2026-11-03 · **Status:** Accepted · **Source:** WP-12 (PR #23), `docs/wp/WP-12.md`

**SECREL is pre-applied (answers the ADR-0029 open point).** The broker writes each SecRel32
value into the patch DLL file between link and `LoadPatch` (`preapply_write32`). Every site is
range-checked against its section's raw data before anything is written. The value depends only
on the old image's section layout, never on the load address, so no `Write32` wire kind was
added and the agent and wire format are unchanged.

**Hook narrowing (ADR-0029 hand-off).** A planned hook is dropped only when all three hold:
- the old bytes are known;
- the new code has the same length and equal bytes with relocation sites masked;
- every relocation resolves to the same target as in the old code.

Read-only constants that carry pointers (vtables) never count as equal. Masking alone would miss
`call a` → `call b` and edited string literals. `set-option narrow_hooks false` restores the
planner's full list.

**One patch DLL per recompiled compiland.** This avoids duplicate thunks and definitions between
objects in one link. The registry is updated between patches, so a later compiland binds to an
earlier one's new symbols. A reload that fails partway keeps the patches already applied, and
`generation` counts them.

**Staged agent requests.** The broker sends LoadPatch, ApplyFixups, RunInitializers and
InstallHooks as separate requests, each with its own timing (`RELOAD-TIMING`). Failure handling:
- Only a real timeout after InstallHooks (CML5320, synchronised mode) leaves the patch loaded,
  and records the hooks as installed.
- A send failure or a lost agent unloads the patch.
- A LoadPatch timeout queues an unload behind the late load.

**Control events are asynchronous.** Each subscribed connection has a bounded queue and a writer
thread that shares the response write lock. A stalled subscriber never blocks the pipeline. An
event such as `patch_applied` may arrive after the `reload` response it belongs to.

**`status` readiness.** A module appears enabled only once its PDB index exists; until then the
index is reported as `pending`. cmltest's `expect_module_enabled` waits for `ready`, so an edit
can never be baked into the source baseline.

## ADR-0031 Diagnostic sub-ranges for wave 5 (extends ADR-0008)

**Date:** 2026-11-03 · **Status:** Accepted

CML5400–5499 is already used by the control pipe and cmltest (WP-12/WP-13). Wave 5 runs in
parallel, so each work package gets its own range:

| Range | Owner |
|---|---|
| CML5500–5549 | WP-14 file watcher and change detection |
| CML5550–5599 | WP-15 header dependency handling |
| CML5600–5649 | WP-16 multiple modules and processes |
| CML5650–5699 | WP-17 patch/compile hooks and synchronised mode |
| CML1600–1699, CML2300–2399 | WP-18 build-requirement checks (`cml_broker check`) |

## ADR-0032 Wave-6 diagnostic ranges, IPC message blocks and control-pipe shapes (extends ADR-0008/0031)

**Date:** 2026-11-03 · **Status:** Accepted

Wave 6 (WP-19, WP-20, WP-21, WP-22, WP-24, WP-25) runs up to four packages in parallel, and three of them
add agent messages and control commands. To keep them from colliding, and to let the UI (WP-19) be built
against commands that land in parallel, the ids and wire shapes are fixed here.

**Diagnostic ranges**

| Range | Owner |
|---|---|
| CML5700–5749 | WP-19 broker UI (`cml_ui`) |
| CML4400–4449, CML5750–5799 | WP-20 crash → patch → continue (agent, broker) |
| CML4450–4499, CML5800–5849 | WP-21 hot-restart (agent, broker) |
| CML4500–4549, CML5850–5899 | WP-22 patch revert and session reset (agent, broker) |
| CML3400–3449, CML4550–4599 | WP-24 thread-local variables (planner, agent) |
| CML3450–3499 | WP-25 unchanged-function elision |

**Agent message types** (`core/cml_ipc/include/cml/ipc/messages.h`): WP-20 owns `Exception` 0x0050,
`ExceptionAction` 0x0051 and may add 0x0052–0x005F; WP-21 owns `Restart` 0x0061 and may add 0x0062–0x006F
(`Log` 0x0060 is unchanged); WP-22 owns 0x0080–0x008F; WP-24 owns 0x0090–0x009F; WP-25 owns 0x00A0–0x00AF.
Additions are additive; no package changes `agent_abi_version` (the orchestrator bumps it once at the end of the
wave if the merged set needs it).

**Control-pipe commands and events** (request/response framing per WP-13; every package advertises its
capability name in `hello`):

| Owner | Capability | Shape |
|---|---|---|
| WP-20 | `exceptions` | event `{"event":"exception","pid","tid","exception_id","code":"0xC0000005","address","module","function","offset","actions":["continue","leave","ignore"]}`; command `exception-action` `{pid, exception_id, action}` → `{pid, exception_id, action}`; command `exceptions` → `{pending:[<exception event bodies>]}` |
| WP-21 | `restart` | command `restart` `{pid, timeout_ms?}` → `{old_pid, new_pid, modules:[...]}`; event `{"event":"restarted","old_pid","new_pid"}` |
| WP-22 | `revert` | command `revert` `{pid?, module?, scope:"last"\|"all"}` → `{reverted:[{pid, module, generation}], generation}`; event `{"event":"patch_reverted","pid","module","generation"}` |

A package may add fields but not rename these. Clients (WP-19, cmltest) treat CML5402 (unknown command) or a
missing capability as "feature not available in this broker" and disable the feature instead of failing.

## ADR-0033 Broker UI is a separate control-pipe client (`cml_ui.exe`) built on vendored Dear ImGui (WP-19)

**Date:** 2026-11-03 · **Status:** Accepted

Plan §8 WP-19 asks for an ImGui UI and says every control-pipe operation must be reachable from it.
**Decision:** the UI is its own executable, `cml_ui.exe` (`broker/cml_ui/`), and talks to the broker only over
the control pipe (the same protocol cmltest uses). If no broker is running it starts `cml_broker.exe` from its own
directory, as the agent does. Dear ImGui (MIT) is vendored under `third_party/imgui` at a pinned release tag
with its licence file; backends are Win32 + Direct3D 11 (both ship with Windows and the SDK; no new runtime
dependency). Single instance via a named mutex; the tray icon uses `Shell_NotifyIconW`.

Reasons: the broker stays headless and fully tested by the harness; a UI defect cannot take a reload down;
and the "every operation reachable" requirement becomes testable. The UI keeps a table of the control commands
it drives, and a unit test checks that table against the commands the broker advertises. UI logic lives in a
window-free view-model layer that is unit-tested; drawing code is thin. CI builds `cml_ui` in every cell and
runs `cml_ui --self-test` (connects to a broker started by the test, runs `status`, exercises the view-model,
creates no window). Visual acceptance is the manual checklist in `docs/wp/WP-19.md`.

## ADR-0034 Wave-6 close-out rulings (agent ABI, restart token, WP-25 scope)

**Date:** 2026-11-08 · **Status:** Accepted

**`CML_AGENT_ABI_VERSION` stays 2.** It versions the C interface between `Chameleon_API.h` and `cml_agent.dll` (exports,
`CreateAgent` parameters, status codes). No wave-6 package changed that interface: `ScheduleRestart` still returns
`CML_STATUS_NOT_IMPLEMENTED`, and only comments in the header changed. A bump would force every application to rebuild
for nothing. The broker–agent wire is versioned per message (`version` in the frame header) and by capabilities. The new
messages are additive: WP-20 `Exception`/`ExceptionAction`/`ExceptionConfig`, WP-21 `Restart`/`RestartInfo`/`ReenableModules`/
`RestartCancel` and the optional trailing `Hello.restart_token`, and WP-22 `ReleaseMode::Uncommit`. Agent and broker ship
from one build. An older agent that does not know `Uncommit` refuses it, and the broker turns that into warning CML5855.

**The restart token stays in the relaunched process's environment (WP-21).** Plan §4.12 makes `CML_RESTART_TOKEN` the way
an application learns it was restarted, and the `hot_restart` fixture relies on it. A child process that inherits it cannot be
taken for the restarted process, because the broker accepts a registration only when both the token and the pid of the process
it created match.

**WP-25 was narrowed.** WP-12 had already implemented the elision rule ("hook narrowing", ADR-0030). WP-25 closes the gaps:
- functions already hooked by an earlier patch are compared against that patch's code;
- a reload that compiles to identical code loads nothing and reports CML3450;
- hook counts are reported.

The old bytes still come from the module file, not process memory: after the first gap is closed, reading memory adds
nothing, and it breaks when something else writes the code.

## ADR-0035 Wave-7 ranges, message blocks, CI ownership and design rulings (extends ADR-0008/0031/0032)

**Date:** 2026-11-08 · **Status:** Accepted

Wave 7 (WP-23, WP-26, WP-27, WP-28) runs all four packages in parallel.

**Diagnostic ranges**

| Range | Owner |
|---|---|
| CML4600–4649, CML5900–5919 | WP-23 re-execute function after crash (agent, broker) |
| CML3500–3549, CML1470–1479, CML5920–5939 | WP-26 real-project corpus (fixes in planner/linker/toolchain, PDB reader, broker) |
| CML5940–5959 | WP-27 performance budgets |
| CML1280–1289, CML1380–1389, CML1480–1489, CML4650–4699, CML5960–5989 | WP-28 fuzzing and failure injection (COFF/PE/PDB reader hardening, agent, broker) |

**Agent message types:** WP-23 owns 0x00B0–0x00BF, WP-28 owns 0x00C0–0x00CF. `agent_abi_version` is unchanged (ADR-0034).

**Control pipe**

| Owner | Capability | Shape |
|---|---|---|
| WP-23 | `reexecute` | the `exception` event's `actions` contains `"reexecute"` only when it is offered; event `{"event":"exception_updated","pid","exception_id","actions":[...],"reasons":{...}}` when a patch changes what is offered; `exception-action` accepts `"action":"reexecute"` |
| WP-28 | `fault-injection` | command `inject-failure` `{stage, pid?, count?}` → `{stage, armed}`; `{"stage":"none"}` disarms; command `fault-stages` → `{stages:[...]}` |

**CI ownership.** WP-27 is the only package that edits `.github/workflows/ci.yml`, and only the steps of the
`pdb-benchmark` job (job names and the job count, 14, stay). WP-26 adds `.github/workflows/corpus.yml`, WP-28 adds
`.github/workflows/fuzz.yml`; both run on `schedule`, `workflow_dispatch`, and on `pull_request` filtered to their own
paths, so the PR that adds them carries its own acceptance run. Fixture directories `crash_reexecute`, `perf`,
`inject` and test directories `tests/fuzz`, `tests/corpus` are pre-registered.

**WP-23 (re-execute).** Plan §4.11 says "four register arguments reloaded from the frame's home space". clang-cl at
`-O0` spills parameters to its own locals, not to the home space, so the rule is generalised: parameter values are
read from the locations the PDB records for the faulting function's parameters (`S_REGREL32`, `S_LOCAL` +
`S_DEFRANGE_*`), which covers MSVC's home-space spill and clang-cl alike. Re-execute is offered only when the function
was compiled without optimisation, a patch applied after the fault replaces it, every register parameter's location
is known, and the frame unwinds; otherwise the event says why not. The frame re-executed is the innermost frame whose
function has a newer patched version; frames above it are discarded without running destructors (as Leave function).
The stack pointer is restored to its value at function entry; stack-passed arguments stay in the caller's frame.
Repeated side effects and parameters the function assigned to before the fault are documented behaviour.

**WP-26 (corpus).** Projects: a raylib example game (zlib), Dear ImGui `example_win32_directx11` (MIT), and the Godot 4
editor (MIT) as the large one; pinned tags, fetched at CI time, never vendored; the few lines that create the agent and
the required compiler/linker flags are patch files in `tests/corpus/<project>/`. No GPL code anywhere in the chain.
Every corpus PDB also feeds the hook decoder over all function entries (plan §11), reporting counts per outcome.

**WP-27 (performance).** CI thresholds keep ADR-0009's 2× slack for shared runners on time and CPU budgets; memory has
none. Trend output is a JSON artefact plus the step summary, compared against the latest successful `main` run when
one is available; only absolute budgets fail the job.

**WP-28 (fuzzing, injection).** CMake option `CML_FUZZ` (clang-cl, libFuzzer + ASan) builds fuzzers; without it each
target builds as a replay executable registered with ctest over its checked-in seed corpus, so every cell exercises
it. Failure injection is compiled in under CMake option `CML_FAULT_INJECTION` (default ON; the release packaging WP
turns it off) and is reachable only through the `fault-injection` control capability.

## ADR-0036 WP-26 findings: static-library members are reloadable; the C-statics crash is its own package (extends ADR-0035)

**Context.** The real-project corpus (WP-26, PR #46) found the following.

- **F-1:** `ModuleIndex::is_source_compiland` rejects every compiland whose PDB module names an archive. Code linked from a
  static library is therefore never tracked, so a reload after editing it answers `ok`, `patched: 0`, with no diagnostic. Most of
  Godot is built this way.
- **F-3:** under clang-cl, an accepted reload of raylib `rcore.c` crashes the process 3 ms later. The suspected cause is that a
  `static` variable of a C translation unit is bound as a new global, so the patch reads a zero-filled copy.
- **F-2:** `__ImageBase`-relative array indexing under cl gives CML3101 for the whole TU.
- **F-4:** an overloaded `static` function set in a header gives CML3214 for every TU that includes it.
- **VEX:** the hook decoder refuses VEX-encoded (C4/C5) prologues.

**Decision.**

1. **Library members (F-1) are reloadable.** A compiland is a source compiland when its object name is a real object (not
   `Import:` or `*`) and the PDB records its build environment (S_ENVBLOCK: cwd, compiler, command, source), whether or not
   it came from an archive. Its sources are tracked and swept like any other. A compiland that is skipped (no build
   environment, missing source, import stub) is counted.
   - At enable time the broker warns once with the number of skipped compiland objects that come from archives, naming up to
     five.
   - When a reload finds a changed file that no reloadable compiland owns, it warns instead of answering a silent `patched: 0`.
   - Package **WP-26b**: ranges CML5920–5939 (broker) and CML1470–1479 (PDB reader).
2. **The F-3 crash is package WP-26c** (range CML3500–3549, planner).
   - Root-cause it from the corpus trace. Then either fix the static matching for C translation units in `bind_plan.cpp`, or
     refuse the reload with a planner diagnostic before anything is applied.
   - An accepted reload that crashes the target is never an acceptable end state.
   - WP-26c adds a fixture/scenario reproduction (a C TU with a `static` struct read through a pointer after the reload) in
     every matrix cell. The corpus alone is not enough, because it runs nightly and only on its own path filter.
3. **F-2, F-4 and VEX stay documented limitations** (KNOWN_LIMITATIONS) and become follow-ups FU-26..28. Decoding VEX
   reverses a WP-10 limitation and is not wave-7 work.
4. **The corpus lands first** (PR #46), with F-1 and F-3 asserted as expected failures. WP-26b and WP-26c each flip their entries.
   The corpus runner records an expected failure that passes as a failure, so the flip is required.
5. WP-26b and WP-26c are wave-7 packages and run in parallel; they touch different files (module index, sweep and audit vs. the
   bind planner). Review: high for WP-26c (bind planner), medium for WP-26b.
## ADR-0037 CI selects the clang-cl that the active MSVC STL accepts

**Context.** The `windows-2025` runner image became `windows-2025-vs2026`: Visual Studio 18 with MSVC STL 14.52, which
requires Clang 22 (`error STL1000`). Its standalone LLVM in `C:\Program Files\LLVM` is still 20.1.8. Both clang-cl cells on
windows-2025 failed to compile anything (PR #46 at d5d937c, 2026-11-22).

**Decision.** The matrix cells pick clang-cl in this order:
1. The one bundled with the active Visual Studio toolset (`$env:llvmX64` from msvc-dev-cmd), which is the pairing Microsoft
   supports.
2. The standalone install.
3. Whatever is on PATH.

The first candidate that compiles a small STL translation unit wins. Each rejected candidate is logged, and `toolchain.txt`
records the chosen compiler and its version. The cells therefore test the clang-cl a user of that Visual Studio gets.
- **Older clang versions:** the explicit clang-cl 17–20 probe jobs still cover them.
- **`_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH`:** not used; it builds a combination Microsoft does not support.
- **Other workflows:** `corpus.yml` (WP-26) and `fuzz.yml` (WP-28) select clang-cl the same old way. Each owner applies this rule
  when its workflow runs on an image where the standalone LLVM is too old.

## ADR-0038 Wave-8 ranges, directories, CI ownership and integration contracts (extends ADR-0008/0035)

**Date:** 2026-11-23 · **Status:** Accepted

Wave 8 (WP-29 CMake integration, WP-30 VS Code extension, WP-31 Visual Studio extension, WP-32 packaging) runs all four
packages in parallel. WP-32 depends on the other three and merges last.

**Diagnostic ranges**

| Range | Owner |
|---|---|
| CML1700–1749 | WP-29 CMake integration (configure-time messages of `chameleon_enable`) |
| CML5990–5993 | WP-30 VS Code extension (client-side failures: no broker, pipe lost, unsupported broker) |
| CML5994–5997 | WP-31 Visual Studio extension (same, plus re-attach failures) |
| CML5998–5999 | WP-32 licensing hook |

**Directories.** WP-29: `integrations/cmake/`, `cmake/` install and export rules, the top-level `CMakeLists.txt`
(`project(... VERSION)`, install and export), and `tests/fixtures/CmlFixture.cmake`. WP-30: `integrations/vscode/`. WP-31:
`integrations/vs/`. WP-32: `tools/package/`, `core/cml_license/` (plus one call at broker start-up), `docs/QUICKSTART.md`,
`docs/TROUBLESHOOTING.md`, `docs/BUILD_REQUIREMENTS.md`, `README.md`.

**CI ownership.** WP-29 is the only package that edits `.github/workflows/ci.yml`, and only steps inside existing jobs (job
names and the job count, 14, stay). WP-30 adds `vscode.yml`, WP-31 adds `vs.yml`, WP-32 adds `package.yml`. Each runs on
`workflow_dispatch`, on `push` to `main`, and on `pull_request` filtered to its own paths, so the PR that adds it carries
its own acceptance run.

**Integration contracts**

- **Clients only.** The extensions use only the existing control pipe (ADR-0011 transport and framing; commands `status`,
  `reload`, `restart`, `revert`, `subscribe`; events `diag`, `patch_applied`, `patch_reverted`, `restarted`, `exception`).
  They may ask for added fields in their PR, but must not rename anything. A missing capability disables that feature
  (ADR-0032).
- **Build entry points for WP-32.** WP-30: `npm ci && npm run package` in `integrations/vscode` writes exactly one
  `integrations/vscode/*.vsix`. WP-31: `integrations/vs/build.ps1 -Configuration Release` writes
  `integrations/vs/out/*.vsix`. WP-29: `cmake --install <build> --prefix <dir>` lays out `bin/`, `include/`, `lib/` and
  `lib/cmake/Chameleon/` (`ChameleonConfig.cmake`, `ChameleonConfigVersion.cmake`, imported target `Chameleon::agent`,
  function `chameleon_enable(target)`).
- **`chameleon_enable(target)`** applies the flags in `docs/BUILD_REQUIREMENTS.md`, links the agent, and makes
  `cml_broker check` report 0 WARN on the target under cl and clang-cl. The fixtures go through it (in-tree mode), and CI
  builds an example project against an install tree with `find_package(Chameleon)`.
- **Release package (WP-32):** RelWithDebInfo with `CML_FAULT_INJECTION=OFF` and `CML_BUILD_TESTS=OFF`, built by
  `package.yml`. `docs/TROUBLESHOOTING.md` has an entry for every diagnostic id defined in code, kept in sync by a check
  that runs in CI.
- **Dependencies:** permissive licences only (MIT, BSD, Apache-2.0, Microsoft SDK packages for building the VSIX). No
  GPL/LGPL in any dependency tree; each extension records its dependency licences in its README.
- **Acceptance.** The plan's "manual checklist" stays: each extension's `docs/wp/WP-NN.md` contains one, and the user runs
  it. Automated proxies are required on top:
  - each extension's control client is tested in its workflow against a real `cml_broker` and a running fixture;
  - WP-31's re-attach logic is unit-tested behind a debugger interface;
  - the `.vsix` is built and uploaded as an artefact.
  - WP-32's quickstart is followed by a fresh agent that has no prior context, as plan WP-32 requires. The orchestrator
    runs that check as a workflow on a clean windows-2022 runner.

## ADR-0039 Chameleon is proprietary, commercial software

**Date:** 2026-11-23 · **Status:** Accepted (user decision)

The user decided that Chameleon will be sold commercially and that its licence must reflect that.

- `LICENSE` at the repository root is a proprietary, all-rights-reserved notice. Use, copying and redistribution
  need a separate written licence agreement with the copyright holder. The holder is currently named by the repository
  owner's handle, and the text is a placeholder for counsel. Both are to be replaced by the legal entity and reviewed
  licence terms (EULA) before the first sale.
- The release zip carries `LICENSE` (WP-32). The VS Code extension declares `"license": "SEE LICENSE IN LICENSE"`
  and packages a copy of it, so `vsce --skip-license` is gone. The Visual Studio extension shows it via `<License>`
  in its manifest.
- With `LICENSE` present nothing is `WP32-PENDING`, so the package workflow now runs `package.ps1 -Strict`. A
  pending part is a CI failure from now on.
- Third-party code stays permissive-only (MIT, BSD-2-Clause today; ADR-0038), with its licence texts shipped in
  `licenses/`. Copyleft code (GPL, LGPL, AGPL, MPL) must not be vendored or bundled, and a dependency is checked for
  commercial-use compatibility before it is added. The VS Code extension's build-only dev dependency
  `@vscode/vsce-sign` (proprietary Microsoft licence) is not shipped in any package.
- Licence enforcement itself (keys, activation) is out of scope for now. It plugs in behind the `core/cml_license`
  hook (no-op provider, CML5998/5999) without changing the callers.


## ADR-0040 Wave 9: the wave-8 follow-ups (WP-33, WP-34, WP-35)

**Date:** 2026-11-23 · **Status:** Accepted

The user asked for every follow-up logged at the end of wave 8 (KNOWN_LIMITATIONS) to be addressed. Wave 9 runs three
packages in parallel. The order of merging is WP-33, WP-35, WP-34 (WP-34 merges main last and resolves the one-line
overlaps in the capability and event lists).

**WP-33: `chameleon_enable` without compiler warnings** (medium review).
- Problem: with MSVC on x64 every TU of an enabled target prints `D9002 ignoring unknown option '/hotpatch'`, and in
  RelWithDebInfo `D9025 overriding '/Ob1' with '/Ob0'`.
- Change: pass `/hotpatch` only where it means something (x86 cl; clang-cl wherever it accepts it). On x64 the padding
  comes from `/FUNCTIONPADMIN`. Remove `/Ob1` (and `/Ob2`) from the target's effective flags for the configurations where
  `/Ob0` is applied, rather than appending an override. Apply the same to the test-fixture style in
  `cmake/CmlTargets.cmake`, so fixtures and users build alike.
- `cml_broker check` must still report 0 FAIL / 0 WARN on enabled targets under cl and clang-cl. If a check rule
  demanded `/hotpatch` on x64, fix the rule to match the real requirement (padding before functions) and say why in
  `docs/BUILD_REQUIREMENTS.md`.
- Acceptance: in every ci matrix cell, the build logs of the WP-29 examples and the fixtures show no D9002 and no D9025
  for enabled targets, as a greppable `WP33-WARN d9002=0 d9025=0` line per cell. `cml_broker check` stays at 0 WARN.
- Directories: `integrations/cmake/`, `cmake/CmlTargets.cmake`, `docs/BUILD_REQUIREMENTS.md`, broker check rules only if
  needed. The only package that may edit `.github/workflows/ci.yml` (steps only; job count stays 14). No new ids are
  expected; if one is needed, it is in CML1712–1719.

**WP-34: early debugger re-attach after a hot-restart** (high review: restart and process start-up).
- Broker:
  - `restart` takes an optional `hold_new: true`. The new process is then created suspended.
  - The broker publishes `restarting {old_pid, new_pid}` and keeps the process suspended until either a
    `restart-resume {pid}` arrives or `hold_ms` passes (default 10000, maximum 60000). Then it resumes the process and
    the restart continues as today. The time held does not count against the registration budget.
  - The capability `restart-hold` gates all of this. Without `hold_new`, behaviour is byte-for-byte as now.
  - A broker or client that disappears while holding must never leave the process suspended forever.
  - Restart-hold diagnostics use CML5813–5819 and are documented in `docs/TROUBLESHOOTING.md`.
- Visual Studio extension: when re-attach is on and the broker has `restart-hold`, it sends `restart {hold_new:true}`. On
  `restarting` it attaches the debugger to `new_pid`, then sends `restart-resume`. Its diagnostic ids stay in
  CML5994–5997 (ADR-0038). Otherwise it keeps today's late re-attach on `restarted`.
- Acceptance:
  - ci: a broker test with the fixture `hot_restart` shows that with `hold_new` the new process does not run until
    `restart-resume` (no TICK before it), that it runs after resume, and that it is resumed after `hold_ms` with nobody
    resuming it.
  - ci also shows that a client disconnecting while holding does not strand the process.
  - vs workflow: the extension's integration test drives `restart {hold_new}`, sees `restarting`, resumes, and gets
    `restarted`. It also unit-tests the decision against the debugger interface.
- Directories: `broker/cml_core` (restart, control server), `core/cml_ipc` (command and event names), the restart host in
  `core/cml_proc` or wherever `relaunch` lives, `integrations/vs/`, `docs/TROUBLESHOOTING.md` (its ids only).

**WP-35: structured file and line on `diag` events** (medium review).
- Broker:
  - A compile failure's `diag` event (and the `reload` result's diagnostic) carries `file` (absolute path), `line` and,
    when the compiler gives one, `column`, taken from the first compiler error the pipeline already parses
    (`toolchain::ToolDiagnostic`).
  - Fields are added only when known. No existing field changes.
  - The capability `diag-locations` advertises them.
- VS Code and Visual Studio extensions: when a `diag` has `file` and `line`, the Problems or Error List entry uses them
  exactly. The current regex parse stays as the fallback for older brokers.
- Acceptance:
  - ci: a broker test with the fixture `bad_builds` (or `basic` with a syntax-error edit) shows the event fields for cl
    and clang-cl.
  - vscode and vs workflows: the integration tests check that a compile error yields an entry at the right file and line.
- Directories: `core/cml_util` (Diag, only if a location is added there), `core/cml_toolchain`, `broker/cml_core`
  (pipeline, event bus), `integrations/vscode/`, `integrations/vs/` (Error List sink only), `docs/wp/WP-35.md`.
- No new ids.

**Shared files.** `broker/cml_core/src/control_server.cpp` capability and event lists: WP-34 adds `restart-hold` and
`restarting`; WP-35 adds `diag-locations`. In `integrations/vs/`, WP-34 owns `ChameleonController.cs`, the re-attach code
and `VsDebuggerAttach.cs`, and WP-35 owns `ErrorListSink.cs` and the diag parsing in `Chameleon.Control`. Both may add
tests in their own files.

## ADR-0041 v1 plan complete; follow-on work is tracked in docs/ROADMAP.md

**Date:** 2026-11-23 · **Status:** Accepted

Every work package of `docs/IMPLEMENTATION_PLAN.md` (WP-01..WP-35, waves 1–9) is merged and verified on the Windows CI
matrix. The plan's bar for v1 (§2, "as robust as Live++ on Windows for the supported subset") is not yet met on real
code: the WP-26 corpus still has refusals of ordinary code (F-2, F-4, VEX prologues), and the product has had no real use.

Decisions:
- `docs/ROADMAP.md` is the single index of follow-on work, with stable ids `R-nn`. New waves cite the R-ids they close;
  a closed item is marked done there with its PR, as PROGRESS does for WPs.
- Per-diagnostic detail stays in `docs/KNOWN_LIMITATIONS.md`; ROADMAP links to it rather than repeating it.
- Recommended next engineering wave (wave 10): R-01 (image-relative array indexing), R-02 + R-03 (PDB type stream for
  static overloads and same-named local statics), R-04 (VEX/EVEX in the hook decoder, with decoder fuzzing), R-13
  (reload timing in a large PDB). Its acceptance is the corpus: the F-2 xfail flips to pass, the ImGui CML3214
  refusals become reloads, and `cml_lde_probe` reports `refused_vex=0`.
- Plan §2's differentiator "source-available licensing" no longer applies after ADR-0039; positioning is an open user
  decision (R-30).

## ADR-0042 Wave 10: real-code coverage (WP-36..WP-39; closes R-01..R-04, R-13, the decoder part of R-41)

**Date:** 2026-11-24 · **Status:** Accepted

Wave 10 runs four packages in parallel under the plan's §0 rules. Each closes roadmap items (ADR-0041); the acceptance
of the wave is the corpus.

| WP | Roadmap | Title | Branch | Review |
|---|---|---|---|---|
| WP-36 | R-01 | Image-relative references to old data (F-2) | `wp/36-imgrel` | high (bind planner, agent fix-ups) |
| WP-37 | R-02, R-03 | PDB type stream; static overloads and lambda-local statics | `wp/37-pdb-types` | high (bind planner) |
| WP-38 | R-04, R-41 (decoder) | VEX/EVEX in the hook length decoder; decoder fuzzing | `wp/38-vex-decoder` | high (length decoder) |
| WP-39 | R-13 | Reload timing inside a large PDB, with budgets | `wp/39-large-pdb-reload` | medium |

**Ranges.** WP-36: CML3550–3569 (planner), CML4700–4719 (agent fix-ups). WP-37: CML1430–1459 (PDB reader),
CML3570–3599 (planner). WP-38: CML4317–4329 (hook installer). WP-39: CML5940–5959 (the WP-27 range, unused so far).
No new agent message types. WP-36 adds one fix-up kind to `ApplyFixups` (below); `agent_abi_version` rises only if an
older agent would misread the message rather than reject the unknown kind.

### WP-36: image-relative references to old data (R-01, F-2)

**Evidence (orchestrator spike, run 45402685481, windows-2022, cl 19.44, `/O2 /Ob0 /Gy` and `/O1`).**
- The checked-in repro `tests/corpus/repro/f2_global_array_index.c` does **not** reproduce F-2. For one global array
  indexed by a variable cl emits `lea rax,[table]` (REL32), which already works.
- cl emits `sym@IMGREL` (ADDR32NB) to data only when the function has `__ImageBase` in a register: a function with a
  switch jump table that also touches globals (`mov byte ptr CORE[rbx+rdi],0`, bytes `C6 84 3B disp32 00`), or one that
  indexes two or more global arrays (`/O1` and `/O2`). raylib's `PlayAutomationEvent` is the first kind.
- Every such site is the 32-bit **displacement** of a ModRM/SIB memory operand whose base register holds
  `__ImageBase` (`mov`, `cmp`, `lea`, `cvttss2si`, `mov rax,[rdi+rax*8+fptab]`). The processor sign-extends it.
- The jump table lives in the function's own code section (in `.rdata` with `/jumptablerdata`). Its entries are
  ADDR32NB to labels of the patch, loaded **zero-extended** (`mov eax,[rdi+rax*4+$LN16]`, `add rax,rdi`, `jmp rax`).
- `/Od` has no ADDR32NB to data. clang-cl has none at all (jump tables are table-relative).

**Decision.** The roadmap's direction (bind the patch's `__ImageBase` to the old image base and refuse mixed TUs) is
rejected: the references occur precisely in functions that mix a jump table with old data, and the zero-extended
table entries break when the patch lies below the old image. Instead:

1. `__ImageBase` stays the patch image's base. Jump tables, `.pdata`, `.xdata` and RTTI are untouched.
2. An ADDR32NB site whose target is bound to the old image (`BindOldData`) gets a new fix-up kind, `Addr32NbDisp`:
   the agent writes `int32(S + A − patch_image_base)`, where `S + A` is the absolute target as for the other kinds
   (ADR-0025), and refuses with a CML47xx diagnostic when the value does not fit a signed 32-bit field. Patches load
   within 1.5 GiB of the module (WP-08), so it fits in practice.
3. The planner accepts such a site only when it is **proved** to be a sign-extended displacement of a plain 64-bit
   address:
   - the site lies in a code section;
   - decoding linearly (the shared length decoder, compiled into the broker unchanged) from the nearest function
     symbol at or before the site reaches an instruction whose `disp_size == 4` and whose `disp_offset` is exactly the
     site. The sweep starts at the function, not at the section, because a section can hold several functions
     (`/Gy-`, `#pragma code_seg`) and each one's jump table follows its code;
   - that instruction has none of `kInsnRipRel`, `kInsnAddr32` (a `67` prefix) and `kInsnSegFsGs`;
   - every other relocation site the sweep passes lies wholly inside the displacement or the immediate of one decoded
     instruction. A relocation that straddles an instruction boundary means the sweep ran into data.

   Anything else (a site in a data section, an immediate, an instruction the decoder does not know before the site)
   keeps CML3101, whose message now names the instruction bytes. "Unknown" always refuses; nothing is guessed.
4. The decoder reports the displacement (`DecodeResult::disp_offset`, `disp_size`) and the two prefix flags above.
   The orchestrator adds them in the PR that carries this ADR, so that WP-36 and WP-38 both start from them. WP-38 owns
   `agent/src/hook/length_decoder.*` and keeps the fields correct for the encodings it adds; WP-36 does not edit the
   decoder.
5. Unchanged-function elision (WP-25) and hook narrowing must treat an `Addr32NbDisp` site like the other bound-data
   sites. If comparing them is not possible, the function counts as changed (more hooks, never fewer).

**Acceptance.**
- The repro is replaced by one that reproduces F-2 (a switch with a jump table that reads and writes global arrays,
  plus a function indexing two global arrays), and becomes fixture `imgrel` with a cmltest scenario in
  `tests/harness/scenarios/`: the edit reloads and the process reads and writes the **old** globals afterwards (values
  written before the reload are visible after it, and the switch still dispatches). Green in all 8 matrix cells.
- In the cl RelWithDebInfo cells the log shows that the path was taken: a line `WP36-IMGREL sites=N verified=N` with
  N > 0 for the fixture's object. In Debug and clang-cl cells N may be 0.
- Unit tests: planner (verified site, immediate site refused, unknown instruction before the site refused, site in a
  data section refused), fix-up triples, the agent's write and its range check, wire round trip.
- Corpus: raylib/cl `edit:header-inline-raymath` passes and is removed from `expected_failures`; the raylib cells stay
  green under both compilers.
- Directories: `broker/cml_core` (planner, fix-ups, pipeline, narrowing), `core/cml_ipc` (fix-up kind),
  `agent/src/loader` (apply), `tests/fixtures/imgrel`, `tests/harness/scenarios`, `tests/unit/{bind,agent_loader,lib}`,
  `tests/corpus/repro`, `tests/corpus/raylib/project.json`.

### WP-37: PDB type stream; static overloads and lambda-local statics (R-02, R-03)

**Decision.**
1. `cml_pdb` reads the TPI and IPI streams itself (ADR-0014: no third-party parser). Access is lazy: opening an index
   reads no type records, and the first lookup builds whatever offset table it needs. The WP-27 budgets (index time,
   broker memory) must hold with the reader in place; the `pdb-benchmark` job shows it.
2. The new side's types come from the patch object. With `/Z7` (the usual rewrite, ADR-0026) they are in its
   `.debug$T`, next to the procedure records in `.debug$S`. A cl TU that uses a `/Zi` precompiled header is compiled
   with `/Zi` and a private compiler PDB (`PchDebugMode::KeepZiPrivatePdb`, the default): its `.debug$T` holds only a
   type-server reference (`LF_TYPESERVER2`), and the types are read from that PDB with the same TPI reader. A
   `/Z7 /Yu` object refers to the PCH object's types (`LF_PRECOMP`); if those cannot be resolved the types count as
   unreadable (point 3). `/Zi` with a PCH is Visual Studio's default project shape, so the type-server path is in
   scope.

   One canonical signature (calling convention, return type, parameter list, member-function class and `this`
   adjustment, with every referenced type rendered structurally and by its unique name where it has one) is computed
   by the **same** code for a TPI record and for a `.debug$T` record. The planner compares signatures; it never
   compares type indices across the two sides and never parses decorated names to learn a signature.
3. Static functions that share scope and name (pass B of the static matching) pair one-to-one by signature. A
   function with no partner is a new function (not hooked); an old one with no partner stays as it is. CML3214 stays
   for what is still ambiguous: equal signatures, or types that cannot be read (no type records, a type server PDB
   that is absent, a malformed record).
4. Same-named function-local statics of several lambdas in one function (Godot's `SNAME`: `sname`, `$TSS0`) are told
   apart by the identity of the enclosing lambda: the closure class's unique name from the type stream on the old
   side, the decorated scope on the new side. CML3207 stays for true ambiguity.
5. A non-overloaded static function whose signature changed keeps today's documented behaviour (matched to its
   namesake). Changing that is not this package.
6. The type reader is a parser of untrusted input: malformed records give a CML14xx diagnostic or "unknown", never a
   crash or an out-of-bounds read, and `fuzz_pdb` (WP-28) covers it with seeds that carry type streams.

**Acceptance.**
- Fixture `static_overloads` (a header with a `static inline` overload set, included by two TUs; the edit changes one
  overload) and fixture `lambda_statics` (two or more lambdas in one function, each with a local static of the same
  name, in the shape of `SNAME`), each with a cmltest scenario, green in all 8 matrix cells. The scenarios show that
  the statics keep their values across the reload (no re-initialisation) and that the edited overload, not its
  sibling, changed.
- Unit tests on synthetic type streams and on the real fixture PDBs, including true ambiguity (CML3214/CML3207 still
  raised) and unreadable types.
- Fixture `pch` gains a static overload set in a TU that uses the precompiled header, with a scenario that edits one
  overload. Under cl the log shows which path supplied the new side's types (`WP37-TYPES source=debugT|typeserver|
  unreadable`). `typeserver` or `debugT` must end in a correct reload; `unreadable` must end in CML3214, and the WP
  document says why it could not be read. A wrong pairing is the only unacceptable result.
- Corpus, Dear ImGui under cl and clang-cl: the three `unsupported-static-overloads-*` edits become successful
  reloads with an observed marker (the win32 backend, `imgui.cpp`, and the `ImFloor` header edit).
- Corpus, Godot: a new edit of `scene/main/scene_tree.cpp` reloads and its marker is observed.
- `pdb-benchmark` stays inside its budgets.
- Directories: `core/cml_pdb`, `core/cml_coff` (reading `.debug$T`/`.debug$S` if not there yet), `broker/cml_core`
  (planner), `tests/fixtures/{static_overloads,lambda_statics}`, `tests/harness/scenarios`, `tests/unit/{pdb,bind}`,
  `tests/fuzz` (PDB target and seeds), `tests/corpus/{imgui,godot}/project.json`, `tests/corpus/repro`.

**Amendment (2026-11-24, from the Godot corpus run of WP-37).** `scene/main/scene_tree.cpp` has, besides the `SNAME`
lambdas, an ordinary function with two same-named, same-typed function-local statics in different blocks
(`if (...) { static const Vector<StringName> names = ...; } else { static const ... names = ...; }`). Under cl every
function-local static is in its own COMDAT section, so the layout step cannot place them, and the PDB record carries no
block number. Ruling: such statics (data only, one procedure, one name) may be paired by their order of emission in the
procedure, old PDB against the patch object's `.debug$S`, only when the counts are equal, the canonical types are
pairwise equal, and the block-nesting paths agree where both sides record them. Each reload that pairs this way emits a
warning (CML3570–3599) naming the statics, because reordering the blocks would exchange their values. This is the same
class of residual risk as the lambda number, accepted only with the user told. Anything else stays CML3207.
Order pairing is only the fallback for sections whose layout is unknown (cl's COMDAT section per static). Where the
section layout of a member is known, the layout decides, with no warning; where order and a known layout disagree, the
layout wins, and an order pairing never teaches, contradicts or erases a learned layout (high re-review of PR #64).
Consequently CML3570 is raised only where the layout is unknown (cl); the acceptance's "warning in all 8 cells" becomes a
per-compiler assertion in `unit.bind_fixtures` on each cell's PDB plus the `WP37-ORDER` line in the cl cells.

### WP-38: VEX and EVEX in the hook length decoder; decoder fuzzing (R-04, R-41)

**This reverses a WP-10 decision** ("no VEX, no EVEX": anything VEX-encoded is Unknown, CML4300). The reason then was
caution about a decoder nobody had checked against a reference. The corpus shows the cost: the MSVC STL's vectorised
helpers and 220 Godot functions cannot be hooked, and any `/arch:AVX2` build has VEX at the entry of many leaf
functions. The decision is reversed **together with** a reference check, which is what makes it safe.

**Decision.**
1. In 64-bit mode `C4` and `C5` are always VEX and `62` is always EVEX. The decoder reads the prefix payload (1, 2 and
   3 bytes), the opcode map (VEX and EVEX maps 1–3), the opcode, then ModRM/SIB/displacement as for legacy
   instructions, then an 8-bit immediate where the opcode has one. An EVEX compressed displacement is still one byte.
   A legacy prefix (`66`, `F2`, `F3`, `F0`) or a REX before a VEX/EVEX prefix is Unknown. Other maps (EVEX 4–7, XOP,
   3DNow!) stay Unknown. `disp_offset`/`disp_size` and `kInsnRipRel` are filled as for legacy instructions.
2. "Unknown rather than probably" still holds: an opcode the tables do not name is Unknown. The four assertions in
   `test_length_decoder.cpp` that pin "VEX is unknown" are replaced by vectors worked out by hand from the SDM.
3. **Reference check.** A fuzz target (`fuzz_lde`, libFuzzer + ASan in `fuzz.yml`, replay over checked-in seeds in
   every `ci` cell) asserts the decoder's own invariants (never reads past `available`, length 1..15, a displacement
   inside the instruction, the same verdict whatever follows the instruction). In `fuzz.yml` it also compares with
   Zydis (MIT), fetched at a pinned commit at run time, linked only into the fuzz/differential executables, never
   vendored and never shipped: whenever our decoder says `Ok`, Zydis must decode the same bytes in 64-bit mode to the
   same length. A disagreement is a failure. Where Zydis rejects bytes that we accept, the WP either makes the decoder
   refuse them or lists the exception with its reason in `docs/wp/WP-38.md`.
4. The same comparison runs once over real code: a linear sweep from every function entry of the fixture modules and
   of an `/arch:AVX2` module.

**Acceptance.**
- `cml_lde_probe` reports `refused_vex=0` for every fixture module (`tool.lde_corpus_*`, which now fail on a non-zero
  count) and for the five corpus cells (raylib, ImGui, Godot); the corpus driver fails on `refused_vex` > 0.
- A test module built with `/arch:AVX2` (clang-cl: `-mavx2`) whose hooked functions begin with VEX instructions, with
  and without padding before them, runs under the existing 16-thread stress test for the full 60 s:
  `WP10-STRESS ... crashes=0` in every matrix cell, with a line naming the VEX-entry targets and their first bytes.
- `fuzz.yml`: `fuzz_lde` runs with the Zydis comparison, zero disagreements; the differential sweep line
  `WP38-DIFF instructions=N mismatches=0`.
- KNOWN_LIMITATIONS text for CML4300 is proposed in the PR (the orchestrator applies it).
- Directories: `agent/src/hook/length_decoder.*`, `tests/unit/agent_hook`, `tests/fuzz`, `.github/workflows/fuzz.yml`,
  the decoder step of `tests/corpus/run_corpus.ps1`.

### WP-39: reload timing inside a large PDB (R-13)

**Decision.**
1. The measure is the WP-27 one: per reload the broker's `timing_ms` (sweep, compile, plan, anchor, link, load,
   fixups, init, hooks, total). "Detect to applied" is `total`; the budgeted figure is `total − compile`, because the
   compiler's time belongs to the user's code.
2. Two places measure it.
   - **Every pull request:** the generated benchmark project of the `pdb-benchmark` job (544 MiB PDB) becomes a
     program that runs with the agent, and a cmltest scenario reloads a translation unit inside it five times.
   - **The real thing:** the Godot corpus cell (523 MB PDB) records the same stages for each of its edits.
3. Budgets. Plan target: 1.5 s for `total − compile` (the WP-27 figure, unchanged by module size). CI thresholds carry
   ADR-0009's 2× slack: median ≤ 3.0 s on the generated project, and each Godot edit ≤ 3.0 s per recompiled TU. If
   the first measurement is over the plan target, the WP finds the stage responsible and fixes it when the fix is
   local (for example a PDB re-map or a full symbol scan per reload); otherwise it reports the numbers and the cause,
   and the orchestrator rules. A threshold is never raised to make a run pass.
4. Broker memory during those reloads is asserted against the WP-27 limit (1.5 × PDB + 32 MiB).
5. Guards as in WP-27: a missing or non-numeric stage fails; nothing is read as 0.

**Acceptance.**
- `pdb-benchmark`: a line `WP39-RELOAD pdb_mib=… reloads=5 total_minus_compile_median_ms=… max_ms=… budget_ms=3000`
  with the stage medians, in `perf-results.json` and the step summary; the job fails over budget.
- Godot cell: `WP39-GODOT edit=… total_ms=… compile_ms=… rest_ms=… budget_ms=…` per edit, in the step summary; the
  cell fails over budget.
- Directories: `tests/bench`, `tests/harness` (steps), `tests/ci/perf_results.ps1`, the `pdb-benchmark` job of
  `.github/workflows/ci.yml` (the only package that edits `ci.yml`, apart from registering new ctest entries, which
  needs no workflow change), the timing part of `tests/corpus/run_corpus.ps1`, `.github/workflows/corpus.yml`.
  Broker code only for a fix under point 3.

### Shared files and merge order

- `tests/corpus/run_corpus.ps1`: WP-38 owns the decoder step, WP-39 the timing output; WP-36 and WP-37 change only
  their `project.json` files.
- `broker/cml_core/src/bind_plan.cpp`: WP-36 works in the relocation walk (fix-ups), WP-37 in the static matching.
  Whichever merges second merges `main` and re-runs its acceptance.
- `tests/fixtures/CMakeLists.txt` and the scenario lists: one line per fixture; conflicts are resolved by keeping both.
- `corpus.yml` runs on pull requests that touch `tests/corpus/**`. Every package of this wave touches that tree, so
  every PR carries a corpus run. Its `push` trigger is extended to the branches of wave 10 and later (`wp/36-*` and
  up), without Godot, as before.
- No fixed merge order. Each PR is reviewed at the level in the table, re-reviewed after a substantial fix round, and
  merged as a squash with the expected head sha once the orchestrator has read its CI logs.

## ADR-0043 Wave 11: planner correctness, a wider corpus, restart and injection hardening (WP-40..WP-44)

**Date:** 2026-11-24 · **Status:** Proposed (accepted when wave 10 closes with WP-37)

Wave 10 (ADR-0042) closes R-01, R-02, R-03, R-04, R-13 and the decoder part of R-41 (R-02/R-03 when WP-37 merges). It also found pre-existing
bind-planner defects that can pair the wrong state (R-48), one of which can make an accepted reload use an object that was
never constructed. A wrong pairing is the worst outcome the product has, so R-48 leads this wave, ahead of the user's list
(R-06, R-40, R-42). No R-10 (dogfooding) findings exist yet.

| WP | Roadmap | Title | Branch | Review |
|---|---|---|---|---|
| WP-40 | R-48, R-50 | Bind-planner pairing defects | `wp/40-planner-pairing` | high (bind planner) |
| WP-41 | R-06 | Corpus: an `/arch:AVX2`, unity-built, static-library project | `wp/41-corpus-avx2-unity` | medium |
| WP-42 | R-06 | Corpus: a DLL-plugin architecture | `wp/42-corpus-plugins` | medium |
| WP-43 | R-40, R-42 (restart, revert) | Restart-hold follow-ups; failure injection in restart and revert | `wp/43-restart-injection` | high (restart, process start-up) |
| WP-44 | R-42 (rest), R-49 | Failure injection in the crash handler and the watcher; persistent arming | `wp/44-injection-rest` | high (crash handler) |

WP-40..WP-43 start together; WP-44 starts when the first of them merges (at most four at once).

**Ranges.** WP-40: CML3600–3629 (planner). WP-41: CML5000–5049. WP-42: CML5050–5099 (both for fixes their corpus finds, in
any component, after asking the orchestrator if the fix is outside the corpus driver). WP-43: CML5820–5839 (restart),
CML5972–5978 and CML4657–4679 (failure injection, broker and agent). WP-44: CML5979–5984 and CML4680–4699. No new agent
message types without asking; `agent_abi_version` unchanged.

### WP-40: bind-planner pairing defects (R-48, R-50)

**Decision.** Each of the five defects WP-37 recorded (`docs/wp/WP-37.md`, "Found, not fixed") ends in a correct pairing or
a refusal with a planner diagnostic, never in a silent wrong pairing:
1. clang-cl `/Ob2`: a static of an inlined lambda is decided new while its guard binds by layout. A guard is never placed by
   layout when the static of the same decorated scope was decided new (nor when it has no static at all in that scope); find
   where clang records the inlined static (in the calling procedure) and pair it there, or refuse.
2. A guard placed by layout on an unrelated old address when the layout happens to agree: the same rule as 1, plus the
   guard's old address must lie inside the old compiland's contribution to that section.
3. A new file-scope static `f` hooked onto an existing `X::f` in pass A: names must match in scope, not only in base name.
4. clang-cl `/O2` aggregates split into `name.N`: pair the pieces with the old pieces (same `.N` suffixes and sizes) or
   refuse; never reset silently.
5. Two lambdas that swap places in one function exchange their statics: use the closure identity from WP-37's type reader;
   where it cannot tell, refuse.
6. R-50: the four WP-36 re-review nits.

**Acceptance.** A unit test per defect that fails on main (quote the failing assertion in the PR) and passes after; a
fixture or fixture function per defect built in every matrix cell, plus one fixture target compiled with clang-cl
`/O2 /Ob2` in the clang-cl cells (cl cells build it with `/O2 /Ob2` too, where it is harmless), whose scenario shows that the
inlined lambda's static keeps its value or that the reload is refused, and that the process never faults. `unit.bind`,
the goldens and the corpus stay green. Directories: `broker/cml_core` (planner, names), `tests/unit/bind`, `tests/fixtures`,
`tests/harness/scenarios`.

### WP-41: corpus, an `/arch:AVX2`, unity-built, static-library project (R-06)

**Decision.** Add a fourth corpus project that is built with `/arch:AVX2`, as a set of static libraries, with CMake's unity
build (`CMAKE_UNITY_BUILD=ON`) on at least the library targets. Recommended: Jolt Physics (MIT), a sample or its
HelloWorld, pinned by tag and commit; the agent confirms the licence and that it builds on `windows-2022` under both
compilers within the corpus job's time limit, or proposes another permissive project with the same three properties before
starting. Edits: a library TU that is part of a unity batch, a header included across a batch (fan-out, CML5551 and the
64-compiland cap), a function that starts with a VEX instruction, and a template. Every failure becomes `F-n` with a reduced
repro and an asserted expected failure, as WP-26 did; fixes are separate decisions unless trivial.
**Acceptance.** Green corpus cells for the project under cl and clang-cl with `refused_vex=0`, every edit passing or
asserted as `xfail` with its `F-n`, and `WP39-<PROJECT>` timing lines. LLVM (named in R-06) is deferred: a cold build of
clang with `/Zi` does not fit the runners' time limit; say so in the WP doc with the measured estimate.
**Directories:** `tests/corpus/<project>/`, `tests/corpus/repro/`, one matrix entry in `.github/workflows/corpus.yml`;
`tests/corpus/run_corpus.ps1` only for generic needs (say which).

### WP-42: corpus, a DLL-plugin architecture (R-06)

**Decision.** Add a project in which a host loads plugin DLLs at run time with `LoadLibrary`, after the module list was
first sent to the broker. Recommended: a Godot GDExtension from `godot-cpp` (MIT) loaded by the Godot editor that the corpus
already builds (reuse its cache; do not change the Godot pin, patch or `build.ps1`). Edits: a plugin function, a function in
the host that calls into the plugin, a plugin unloaded and loaded again (if the host supports it), and a hot-restart with
the plugin loaded lazily (CML5809). Findings as in WP-41.
**Acceptance.** A green corpus cell under cl (clang-cl if the host builds with it), every edit passing or asserted `xfail`
with its `F-n`. **Directories:** `tests/corpus/<project>/`, `tests/corpus/repro/`, one matrix entry in `corpus.yml`.

### WP-43: restart-hold follow-ups and failure injection in restart and revert (R-40, R-42)

**Decision.** Fix the eight R-40 items (ROADMAP; KNOWN_LIMITATIONS "IDE integrations"). Add failure-injection stages to the
restart path (pre-restart hooks, terminate, relaunch, hold, registration, re-enable, re-apply) and the revert path, through
the existing `inject-failure` command and `fault-stages` list; each stage's failure must leave the application running
(or, for restart, a new process running or a clear diagnostic and the old one intact before terminate), never a suspended
or orphaned process.
**Acceptance.** A cmltest scenario per new stage group (`hot_restart`, `revert` fixtures) in all 8 cells; a test per R-40
item; the vs workflow green. **Directories:** `broker/cml_core` (restart, revert, fault injection, control server),
`core/cml_ipc` (stage table), `agent/src` (restart, fault injection), `integrations/vs/`, `tests/harness`, `tests/fixtures`.

### WP-44: failure injection in the crash handler and the watcher; persistent arming (R-42, R-49)

**Decision.** Stages for the crash handler (exception report, hold, the actions continue/leave/re-execute) and the
compile-on-save watcher (debounce, change detection, the reload it starts). Arming survives a broker restart only when the
user asks (`inject-failure {persist:true}`), stored next to the broker's log and cleared by `{"stage":"none"}`. R-49's three
nits.
**Acceptance.** Scenarios for the new stages in all 8 cells; a test for persistent arming across a broker restart.
**Directories:** `agent/src/crash`, `broker/cml_core` (exception service, save watcher, fault injection), `core/cml_pe`
(R-49), `tests/harness`.

### Shared files and merge order

- `broker/cml_core/src/fault_injection.cpp` and `core/cml_ipc/.../fault_stages.h`: WP-43 and WP-44 each append their stages;
  whoever merges second merges main and re-runs.
- `.github/workflows/corpus.yml` and `tests/corpus/run_corpus.ps1`: WP-41 and WP-42 each add one matrix entry; the second
  merges main and re-runs the corpus.
- CI capacity is the bottleneck (a `ci` run took over an hour in wave 10 with four branches pushing). Agents batch commits,
  push once per fix round, and hand back while their runs are queued; the orchestrator resumes them.
