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
