# Chameleon — Roadmap after v1 (road to a Live++ competitor)

**Status (2026-11-23).** Every work package of `docs/IMPLEMENTATION_PLAN.md` (WP-01..WP-35, waves 1–9) is merged and
verified on the GitHub Actions Windows matrix. That finishes the *engineering plan*. It does **not** finish the product:
the plan's own bar (§2) is "as robust as Live++ on Windows for the supported subset", and real codebases still fall outside
that subset too often. This document lists everything known to be missing, in priority order, with the evidence, a proposed
direction, an acceptance test, and who has to act.

This file is the index of follow-on work. The per-diagnostic detail stays in `docs/KNOWN_LIMITATIONS.md`, and the reasons in
`docs/DECISIONS.md` (ADR-0041 adopts this roadmap). Item ids (`R-nn`) are stable; reference them from ADRs, WP docs and PRs.

Owner key: **Eng** = can be delegated as work packages under the plan's §0 rules. **User** = needs a decision, money, an
account, a legal step or hands-on use by the product owner. Competitor statements marked *(verify)* are from general
knowledge and must be checked against Live++'s current documentation before they are used in positioning.

---

## 1. Real-code coverage (highest priority, Eng)

These are the findings of the WP-26 corpus (raylib, Dear ImGui, Godot) and the WP-10 decoder probe. Each one makes a user
edit of ordinary code end in a refusal where Live++ would reload. Fixing them is what moves Chameleon from "works on our
fixtures" to "works on your project".

| Id | Gap | Evidence | Direction | Acceptance |
|---|---|---|---|---|
| R-01 | **Image-relative array indexing (F-2, cl).** A TU that indexes a global array by a variable through `__ImageBase` (`lea rcx, __ImageBase` + `[rcx+rax*4+sym@IMGREL]`, ADDR32NB) is refused whole with CML3101, whatever function was edited, and every header it includes is refused with it. | `docs/wp/WP-26.md` F-2; repro `tests/corpus/repro/f2_global_array_index.c`; raylib/cl cell `edit:header-inline-raymath` is an asserted xfail. | Bind the patch's `__ImageBase` to the **old** image base for TUs whose ADDR32NB sites target old-module data, so `sym@IMGREL` stays valid; refuse only if one TU mixes ADDR32NB to old data and to patch-local data. Planner range CML31xx; new ADR. | Repro passes as a cmltest scenario on cl × Debug/RelWithDebInfo; raylib/cl `edit:header-inline-raymath` flips from xfail to pass (remove it from `expected_failures`). |
| R-02 | **Overloaded statics (F-4, CML3214).** Any `static`/`static inline` overload set in a header (Dear ImGui's `ImFloor`) refuses every TU that includes it, because `cml_pdb` has no type information to tell the overloads apart. | KNOWN_LIMITATIONS "Real-project findings"; ImGui cells assert the CML3214 refusal (3 TUs msvc, 2 TUs clang-cl). | Add TPI/IPI type-stream reading to `cml_pdb` (procedure type records → argument list), pair overloads by signature; keep CML3214 only for true ambiguity. | ImGui cells: the refusal rows become reload-pass rows; a fixture with a header overload set reloads on both compilers. |
| R-03 | **Same-named function-local statics in one function (F-4 family).** Several lambdas in one function each defining a local static of the same name (Godot `SNAME` in `scene/main/scene_tree.cpp`) cannot be told apart (CML3207/CML3214). | KNOWN_LIMITATIONS; found by WP-26b. | Use the R-02 type stream plus the enclosing lambda's mangled scope (`<lambda_N>`) to disambiguate. Likely the same WP as R-02. | A fixture reproducing the Godot pattern reloads; add a Godot corpus edit of `scene_tree.cpp`. |
| R-04 | **VEX/EVEX prologues (CML4300).** The hook decoder refuses functions that start with a VEX/EVEX instruction: the MSVC STL's vectorised helpers and 220 of Godot's 512 467 functions. Any code built with `/arch:AVX2` has VEX at the entry of many leaf functions. | `docs/wp/WP-26.md` decoder section; `tests/unit/agent_hook/test_length_decoder.cpp` pins "VEX is unknown". | Teach the length decoder VEX (C4/C5) and EVEX (62): payload 1/2/3 bytes, maps 1–3, ModRM always except `0F 77`, imm8 for map 3 and a few map-1 opcodes. Reverses a WP-10 decision; needs an ADR and the four pinning assertions changed. Fuzz the decoder at the same time (R-41). | `cml_lde_probe` over the corpus reports `refused_vex=0`; a fixture built with `/arch:AVX2` hooks VEX-entry functions under the 16-thread stress test. |
| R-05 | **Unreadable C static names (WP-26c).** A writable static whose name is not a plain identifier, an MS-decorated name, a clang-cl C spelling or on the compiler-generated list is refused with CML3500. No fixture or corpus case hits it yet. | KNOWN_LIMITATIONS. | Wait for a real case (R-10 dogfooding); then extend the name rules. | Driven by a reported case. |
| R-06 | **Widen the corpus.** Three projects is a small sample. | `tests/corpus/`. | Add at least: a project built with FASTBuild or another unity/jumbo build, one built with `/arch:AVX2`, one large CMake codebase that uses static libraries heavily (e.g. LLVM `clang` with `/Zi`), and a DLL-plugin architecture. Record every failure as `F-n` with a repro, as WP-26 did. | Each new project has a green corpus cell with its failures asserted as xfail and listed here. |

## 2. Validation by real use (User + Eng)

| Id | Gap | Direction | Owner |
|---|---|---|---|
| R-10 | **No one has used Chameleon for real work.** Everything has passed in CI only. | The product owner (or design partners) uses it daily on a real project for at least two weeks and files every surprise as an issue with the broker log (`cml_broker` structured log) attached. Triage findings into R-items. | User (use), Eng (fixes) |
| R-11 | **Real Visual Studio attach to a held (suspended) process is unverified (WP-34).** CI drives the extension with a fake debugger. The extension resumes once the debugger lists the pid or after `HoldResumeGraceMs` (3 s) and falls back to a late attach. | Run the manual checklist in `docs/wp/WP-34.md` §"Manual checklist" in Visual Studio 2022. If the attach blocks until resume, consider attaching through the debugger's native engine with `DebugActiveProcess` semantics, or document the 3 s grace as the behaviour. | User (checklist), Eng (fix if it fails) |
| R-12 | **Editor UIs are covered by manual checklists only.** VS Code (menus, status bar, Output, Problems) and Visual Studio (menus, Output pane, Error List, `Attach2`). | Run the checklists in `docs/wp/WP-30.md` and `docs/wp/WP-31.md`; consider UI automation later (VS Code: `@vscode/test-electron` UI tests; VS: no cheap option). | User |
| R-13 | **Reload performance inside a large PDB is not measured.** The WP-27 large-PDB benchmark has no agent; only index loading (328 ms) and broker memory are measured on it. | Measure detect→applied on a Godot-sized module (523 MB PDB) in the corpus job, and add a budget. | Eng |

## 3. Market reach (User decisions first, then Eng)

| Id | Gap | Notes | Owner |
|---|---|---|---|
| R-20 | **Unreal Engine.** Unreal ships Live++ as its built-in "Live Coding" *(verify current terms)*, so Unreal is Live++'s largest user base. Chameleon has no Unreal integration. | Needs: a decision whether to target Unreal at all (licensing and competing with a bundled tool); then an Unreal plugin, Unreal Build Tool support (response files, unity files, adaptive non-unity), very large PDBs (R-13), and module DLL hot-reload interplay. | User (decide), Eng |
| R-21 | **Platforms beyond Windows x64.** Live++ supports consoles and more *(verify)*; Chameleon is Windows x64 only. | Plan §12 sketches ARM64 Windows, Linux x64 (ELF/DWARF), macOS arm64, consoles (PS5, Xbox, Switch via platform programmes: register early, approval takes months), Rust/Zig. Pick the next platform from R-30 positioning. | User (console programmes), Eng |
| R-22 | **Build-system breadth.** Compile commands come from the PDB (`S_ENVBLOCK`) or `compile_commands.json`; only CMake has an integration (WP-29). | Validate and document FASTBuild, Incredibuild/distributed builds, MSBuild without CMake, unity/jumbo builds (header-change fan-out and the 64-compiland cap, CML5551). | Eng |
| R-23 | **Struct/class layout changes** are refused (CML3103), as Live++ also does *(verify)*. Plan §12 lists layout migration and on-stack replacement as research after GA. | Research item; a differentiator if solved. | Eng (research) |

## 4. Commercial readiness (User, with Eng support)

| Id | Gap | Notes |
|---|---|---|
| R-30 | **Positioning.** The plan's v1 differentiator was "source-available licensing" (plan §2). The product is now proprietary (ADR-0039), so that argument is gone. | Choose the reasons to switch: price, multi-compiler (cl and clang-cl both first-class), Linux-first, better diagnostics (`cml_broker check`, stable `CMLnnnn` ids), VS Code support. Update plan §2. |
| R-31 | **Licence text.** `LICENSE` is a placeholder ("Copyright (c) 2026 tomosh22"). | Set the legal entity; have a lawyer review it and an EULA. |
| R-32 | **Licence enforcement.** `core/cml_license` is a no-op provider (ids CML5998/5999 reserved). | Choose a scheme (online activation, offline keys, floating seats), implement behind the existing hook; never block a reload mid-session on a licence check. |
| R-33 | **Code signing.** The zip, `cml_broker.exe`, `cml_agent.dll` and both extensions are unsigned. The agent DLL is injected into user processes, so unsigned binaries will trip antivirus and corporate policy. | Buy an EV code-signing certificate; sign in `package.yml`; publish the VS Code extension to the Marketplace and the VSIX to the VS Marketplace under a publisher account. |
| R-34 | **The desktop UI is not shipped.** `cml_ui.exe` (WP-19) is built with `CML_BUILD_GUI=OFF` in `tools/package/package.ps1`, so the zip has no GUI. | Decide whether it ships; if so, add it to the package, the smoke test and the quickstart. |
| R-35 | **Distribution.** Installer-free zip only; no installer, no update mechanism, no telemetry or crash reporting. | Installer (MSI/MSIX) or keep zip; auto-update channel; opt-in crash reports from the broker and agent (minidumps) with a privacy policy. |
| R-36 | **Go-to-market.** No website, documentation site, pricing, support channel or issue tracker for customers. | User. |

## 5. Hardening follow-ups (Eng)

| Id | Item | Where documented |
|---|---|---|
| R-40 | WP-34 review follow-ups: a failed early attach can report CML5997 two or three times; a `restart-resume` timeout is logged instead of reported (the process stays frozen until `hold_ms`); `restart-resume` racing with hold expiry can answer `resumed:true` alongside CML5813; a confirmation timeout reads as CML5817 (failed resume); the extension asks for `hold_new` even with no debugger on the old process; replacing the process host during a held restart strands the hold; the watchdog start logs nothing when `GetModuleFileNameW` fails; shutdown can wait for a held restart's registration step (pre-existing). | KNOWN_LIMITATIONS "IDE integrations" |
| R-41 | Fuzzing gaps: the hook decoder, the user-hook scanner, the bind planner and the toolchain parser are not fuzzed; only RelWithDebInfo is fuzzed. | KNOWN_LIMITATIONS "Fuzzing (WP-28)" |
| R-42 | Failure-injection gaps: `revert`, `restart`, the crash handler and the compile-on-save watcher are not covered; arming is not persistent. | KNOWN_LIMITATIONS "Failure injection (WP-28)" |
| R-43 | `chameleon_enable` residuals (WP-33): CML1712 when enabling from a parent directory a target whose own directory is already processed; mixed C/C++ targets under the VS generator get the C++ `/Ob` level; the VS-generator CI cell checks D9002 only (Debug build). | KNOWN_LIMITATIONS "Compiler warnings from `chameleon_enable`" |
| R-44 | Crash-handler scope (WP-20/23): only access violations, divide-by-zero and illegal instructions in enabled modules; at most 8 held threads; re-execute not offered for several signatures (CML5900–5907). | KNOWN_LIMITATIONS "Documented behaviour" |
| R-45 | v1 non-goals that are diagnosed today and could be lifted: `/guard:cf` (CML1104; `SetProcessValidCallTargets`, plan "Phase 1.5"), new `thread_local` variables (CML3400; patch-local TLS needs `.tls` registration), PCH header changes (CML5550), `/GL` (CML1103), `/DEBUG:FASTLINK` (CML1101). | KNOWN_LIMITATIONS table; plan §1 |

---

## Suggested order

1. **Wave 10 (Eng):** R-01, R-02 + R-03 (one WP: PDB type stream), R-04 + the decoder part of R-41, R-13. Acceptance is the
   corpus: the F-2 xfail flips, the ImGui CML3214 refusals become reloads, `refused_vex=0`.
2. **In parallel (User):** R-10 dogfooding, R-11 and R-12 checklists, R-30/R-31 decisions.
3. **Wave 11 (Eng):** fixes from R-10, R-06 corpus widening, R-40 and R-42.
4. **Before first sale (User + Eng):** R-32, R-33, R-34, R-35.
5. **After GA:** R-20 / R-21 per the positioning decision, R-23 research.
