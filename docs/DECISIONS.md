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
