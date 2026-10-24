# Chameleon — Native Hot-Reload for C/C++ (Live++ competitor): Implementation Plan, Windows First

**Audience:** an orchestrating agent (Opus) that delegates work packages to implementation subagents (Sonnet).
**Status:** v1.0 of the plan. Windows x64 is the only target for Phases M0–M6. Linux, macOS, consoles, ARM64 and Rust/Zig are Phase 2+ and are sketched in §12 only.
**Product name:** Chameleon. API prefix `cml_`, C++ namespace `cml`, macro prefix `CML_`, diagnostic ids `CMLnnnn`.

---

## 0. How to use this document (orchestrator instructions)

1. Read §1–§6 in full before delegating anything. They contain the architectural decisions and the reasons for them. Do not let subagents re-litigate decisions marked **DECIDED**; questions marked **OPEN** are for you to resolve, ideally by a small spike.
2. Execute work packages (§8) in the wave order of §9. The M0 spike (WP-03) is a hard gate: nothing in M1+ is delegated until WP-03 demonstrates end-to-end function replacement.
3. Each delegated task must be given, verbatim: the WP text, §7 (conventions), the repo layout (§6), and the relevant fixture list (§10). Subagents do not get the whole plan; it is too long and they will drift.
4. A WP is done only when its acceptance tests pass in CI and the subagent has written `docs/wp/WP-NN.md` (what was built, what was learned, what was deferred). You verify by running the tests yourself, not by trusting the report.
5. Keep a running `docs/DECISIONS.md` (ADR style) and `docs/KNOWN_LIMITATIONS.md`. Every "unsupported in v1" item below must appear in the latter with the exact user-facing diagnostic text.
6. Default subagent scope: one WP, one PR, ideally under one working day of agent effort. Split a WP if a subagent cannot describe its plan in ten bullets.
7. Review every PR with the code-review skill at medium effort before merge. Loader, hook and thread-suspension code gets high effort.
8. **Build host.** Everything in M0–M6 compiles only with MSVC or clang-cl on Windows. If the orchestrator or a subagent is running on Linux or macOS, it can still write code, but it must verify through the GitHub Actions Windows matrix (push a branch, wait for the run, read the logs) rather than claim local success. Prefer running the orchestrator on a Windows machine with Visual Studio 2022 and LLVM installed.

---

## 1. Product definition (v1, Windows)

**What it does:** while a native Windows x64 process is running, the developer edits a `.cpp` file, presses a hotkey (or saves, if enabled). The tool recompiles only the changed translation units with the project's own compiler and flags, links a patch DLL, loads it into the running process, rebinds the patch to the process's existing code and data, and redirects the old functions to the new ones. State is preserved. The debugger stays attached and can step into the patched code with full symbols.

**Supported in v1**

| Area | Support |
|---|---|
| OS / arch | Windows 10 21H2+ and Windows 11, x64 only |
| Compilers | MSVC 19.3x+ (VS 2022 17.x) and clang-cl 17+ |
| Linkers (original build) | link.exe, lld-link |
| Linker used for patches | the same linker family that built the module, auto-detected; lld-link fallback |
| Build systems | any; compile commands are read from the PDB, with `compile_commands.json` as fallback |
| Debug info | PDB produced with `/DEBUG:FULL` (not `/DEBUG:FASTLINK`), `/Zi`, `/ZI` or `/Z7` originals |
| Code changes | function bodies, new functions, new global/static variables, new classes, template instantiations, lambdas, virtual functions (existing slots) |
| Modules | EXE and DLLs, multiple modules per process, multiple processes per session |
| Debugger | Visual Studio, WinDbg, RAD Debugger, Rider: patches are real DLLs with PDBs so this is free |
| Developer UX | hotkey compile, optional compile-on-save, pre/post patch hooks, sync points, hot-restart, crash → patch → continue |

**Explicitly unsupported in v1 (must be diagnosed, never silently wrong)**

- Changing the layout of a struct/class that has live instances (adding/removing/reordering members, changing vtable slot count). Diagnostic: "Layout of `T` changed; existing objects not migrated. Use a post-patch hook or hot-restart."
- Functions currently executing on any thread's stack keep running the old code until they return; the next call gets the new code. Not a diagnostic, documented behaviour.
- Inlined copies of a changed function in other translation units are not updated. Warn when the compiland was compiled with optimisation (`S_COMPILE3` flags) and the changed function is small.
- Changing a header included by the precompiled header. Diagnostic: "Header `X` is part of the PCH; rebuild and hot-restart."
- `/GL` (LTCG) compilands: object files are IL, not COFF. Diagnostic at module enable time.
- `/guard:cf` modules: patched code is not a valid CFG target. Diagnostic at enable time; Phase 1.5 may add `SetProcessValidCallTargets`.
- Thread-local variables defined in a patched TU: Phase 1.5 (WP-24). v1 diagnostic.
- Image-relative (`ADDR32NB`) references from patch code to symbols in the old module, other than those routed through thunks (see §4.5). Diagnostic names the symbol.
- x86 (32-bit), ARM64, Linux, macOS.

---

## 2. Competitive context (one paragraph for calibration)

Live++ 2.11 (Molecular Matters) is the incumbent: same mechanism, closed binaries, Windows plus consoles, Linux more recently. Visual Studio Hot Reload / Edit & Continue is MSVC-only and far more limited. Open-source references worth reading for mechanism but not for code: `blink` (Patrick Mours, in-process COFF loading on x64), `jet-live` (Linux ELF), `RuntimeCompiledCPlusPlus` (DLL-level, not function-level). Our v1 differentiators are source-available licensing, a cleaner multi-compiler story, and a roadmap to Linux/macOS/Rust. v1 itself must simply be as robust as Live++ on Windows for the supported subset. Robustness beats features.

---

## 3. System architecture

Three runtime pieces and a set of libraries.

```
┌────────────────────────────┐   named pipe    ┌──────────────────────────────────┐
│ Target process             │◄───────────────►│ Broker (cml_broker.exe)           │
│  ├ app code                │                 │  ├ session / process registry     │
│  ├ cml_agent.dll            │                 │  ├ PDB index (per module)         │
│  │   ├ module registry     │                 │  ├ file watcher + change detector │
│  │   ├ patch loader side   │                 │  ├ compile driver (msvc/clang-cl) │
│  │   ├ hook installer      │                 │  ├ patch linker driver            │
│  │   ├ sync point / hooks  │                 │  ├ bind planner (symbol → addr)   │
│  │   └ exception handler   │                 │  ├ relocation retargeter          │
│  └ Chameleon_API.h shim       │                 │  ├ CLI + GUI (ImGui)              │
└────────────────────────────┘                 │  └ control API (for tests/IDEs)   │
                                               └──────────────────────────────────┘
```

**Agent** (`cml_agent.dll`, loaded by a header-only shim via `LoadLibrary` so the agent's CRT is isolated from the host's): minimal code that must run inside the target. Owns anything requiring process-local execution: address-space reservation, `LoadLibraryEx` of patch DLLs, memory writes, thread suspension, hook installation, running dynamic initialisers for new globals, invoking user pre/post patch hooks, the vectored exception handler, and the sync-point mechanism. No parsing of PDB/COFF in the agent. No heap allocation while threads are suspended.

**Broker** (`cml_broker.exe`): everything else. One broker per user session, auto-started by the first agent if not running. Holds all heavy state (PDB indices can be hundreds of MB). Stateless with respect to the target apart from a per-process symbol registry.

**Libraries** (`core/`): `cml_coff` (COFF object reader + light writer), `cml_pe` (PE image reader, exports/imports/debug directory), `cml_pdb` (wrapper over vendored `raw_pdb`, MIT), `cml_ipc` (framed messages over named pipes), `cml_proc` (process/thread/module utilities), `cml_toolchain` (compiler/linker command parsing, rewriting, invocation), `cml_watch` (directory watching, hashing), `cml_util` (logging, arenas, strings).

**Data flow for one patch**

1. Watcher reports `foo.cpp` changed (or hotkey pressed → scan hashes).
2. PDB index maps `foo.cpp` → compiland(s) in module(s) in process(es); header changes map to all including compilands.
3. Compile driver replays the compiland's recorded command line with `/Z7`, new `/Fo`, into a per-session temp dir. Parallel across compilands.
4. Bind planner parses `foo.obj`: for every symbol it decides *patch-local* or *bind-to-old*, and resolves bind-to-old symbols to addresses in the target.
5. Linker driver generates a *binding object* (thunks + placeholders, §4.5) and links `foo.obj + binding.obj` into `patch_N.dll` with a fixed base inside an address range the agent reserved near the target module.
6. Agent loads the DLL (`LoadLibraryExW`), reports actual base.
7. Retargeter rewrites every relocation site in the patch that referenced a bind-to-old symbol (sites known from `foo.obj` relocations + anchor symbols in the patch PDB, §4.6), and fills thunk slots. Performed by the agent from a compact fix-up list sent by the broker.
8. Agent runs dynamic initialisers for *new* globals only, then at the next sync point suspends threads and installs entry hooks old → new. Pre-patch hooks run before, post-patch hooks after.
9. Broker updates the process symbol registry (new functions now have canonical addresses) and reports success, timing and warnings to the UI.

---

## 4. Core technical design (DECIDED unless marked OPEN)

### 4.1 Why "real linker + post-load retargeting" and not a custom in-process loader — DECIDED

A custom COFF loader (blink style) is attractive but makes patches invisible to debuggers (no module load event, no PDB) and forces us to reimplement COMDAT selection, `.pdata`/`.xdata` registration, TLS, CRT section ordering and `/GS` metadata. Linking a genuine DLL with the project's own linker gives all of that plus debugger support for free. The cost is two problems, both solved below: (a) the linker cannot bind references to absolute 64-bit addresses in another image, so we retarget those relocations ourselves after load; (b) `REL32` retargeting requires the patch within ±2 GB of the target module, so the agent reserves address space and the patch is linked `/FIXED` at a reserved base. A custom loader remains an option for Linux/macOS (§12), where the same trick is harder.

### 4.2 Build requirements imposed on the user's project

Required (detected from PDB and linker flags; enabling a module without them fails with a precise message):

- `/Zi`, `/ZI` or `/Z7`, and `/DEBUG:FULL` at link.
- `/Gy` (function-level linking; default with `/Zi`? it is not, check `S_COMPILE3`? Not recorded; detect by checking that functions are in COMDAT sections in a test compile — **OPEN**, WP-05 decides a detection method or we simply recommend it).
- No `/GL`, no `/guard:cf` in v1.

Strongly recommended, with a warning if absent:

- `/FUNCTIONPADMIN` (6 bytes of padding before each function on x64) and `/hotpatch` (first instruction at least 2 bytes). Enables atomic 2-byte hook writes.
- `/OPT:NOREF /OPT:NOICF` so every function in the PDB corresponds to a real, unfolded body.
- `/Ob0` or `/Od` for the modules being iterated on, to avoid stale inlined copies.
- `/INCREMENTAL` is fine (ILT thunks jump to the real entry, which we hook).

### 4.3 Reading build information from the PDB

Source of truth is the module's PDB, located via the PE debug directory (CodeView record path), with override paths and a symbol-server path configurable.

Per compiland (DBI module), we need:

- Object file name and library name (DBI module info).
- `S_COMPILE3`: compiler version, language, optimisation flags (for warnings).
- `S_ENVBLOCK`: key/value strings `cwd`, `cl` (compiler path), `cmd` (full command line), `src`, `pdb`. MSVC emits these with `/Zi` and `/Z7`. clang-cl emits them when `-gcodeview-command-line` is on (default in clang ≥ 14; verify in WP-05 and document the flag).
- File list for the compiland (DBI source-info substream): every header the TU included. This is the dependency graph. No `/showIncludes` pass is needed.
- Symbols: `S_GPROC32`, `S_LPROC32` (static functions), `S_GDATA32`, `S_LDATA32` (static data incl. function-local statics), `S_GTHREAD32`/`S_LTHREAD32`, `S_UDT`, with section:offset.
- Section contributions (DBI) and the section header stream, to turn section:offset into RVA.

Globally: publics stream (`S_PUB32`) for every linker-visible symbol including `__imp_*`, `__guard_*`, `_tls_index`, `__security_cookie`.

Fallback when `S_ENVBLOCK` is missing: `compile_commands.json` lookup by absolute source path. If neither exists, the compiland is marked non-reloadable with a diagnostic.

### 4.4 Compile command rewriting

Given `cmd` and `cwd`, produce a new command:

- Replace `/Zi`, `/ZI` with `/Z7`; drop `/Fd…`, `/FS`, `/MP`, `/showIncludes`, `/Yc…` (if the PCH-creating TU itself is the change, report unsupported), keep `/Yu…` and `/Fp…` so the existing PCH is reused.
- Set `/Fo<session_tmp>/<module>/<compiland_hash>/<name>.obj`, add `/c`, add `/nologo`.
- Preserve `/I`, `/D`, `/std`, `/EHsc`, `/MD`/`/MT`, `/Zc`, `/d1`/`/d2` undocumented flags verbatim. Preserve response-file semantics (expand `@file` before rewriting, write our own response file for long lines).
- Run the exact compiler binary recorded in `cl`, with the recorded `cwd`. If the binary no longer exists, fall back to the toolchain detected from the running broker's environment and warn.
- For clang-cl: same rules; also ensure `-gcodeview-command-line` is passed so re-reading works for chained patches.

Parallelism: `min(changed TUs, hardware_concurrency)`. Output captured and shown with the usual `file(line): error` format so IDEs can click through.

### 4.5 Bind planning: which symbols come from the patch, which from the old image

Parse the new `.obj` (COFF). For each symbol:

| Symbol kind in new .obj | Decision |
|---|---|
| Defined function (external or static), present in the old compiland | **patch-local definition; old entry is hooked to it** |
| Defined function, not in the old image | patch-local; registered as a new canonical symbol (no hook) |
| Defined data (external or static, incl. function-local statics and their guard variables `?$TSS…`), present in the old image | **bind-to-old**: all references in the patch are retargeted to the existing storage; the patch's own copy is left dead |
| Defined data, not in the old image | patch-local; its dynamic initialiser (`.CRT$XCU` entry, `??__E…` symbol) is run once after load |
| vtables `??_7…`, RTTI `??_R…`, string/FP constants, `.pdata`/`.xdata` | patch-local (fresh copies are correct; MSVC RTTI compares by name when pointers differ) |
| Undefined symbol | **bind-to-old**, resolved in this order: (1) process symbol registry (functions/data introduced by earlier patches), (2) same compiland's `S_L*` symbols in the module PDB (statics), (3) module PDB globals/publics, (4) `__imp_X`: the module's IAT slot address for `X` (public `__imp_X` in PDB; else parse import directory in memory), (5) exports of other loaded modules in the process. Unresolved → compile-style error naming the symbol and the referencing function |

Lookup key for statics is (mangled name, original object file name). For externals it is the mangled name.

**Binding object.** So the linker is satisfied and so image-relative references work, the broker generates `binding.obj` (COFF written by `cml_coff`):

- For each bind-to-old *function* symbol: a 14-byte thunk `jmp qword ptr [rip+slot]` in `.text`, and an 8-byte `slot` in `.data` named `__cml_slot_<name>`. Thunks make image-relative references legal (needed for `__CxxFrameHandler4`, `__GSHandlerCheck`, `__C_specific_handler`, `__security_check_cookie` in `.xdata`, and function pointers stored in `.rdata` with `ADDR32NB`).
- For each bind-to-old *data* symbol: an 8-byte-aligned placeholder in `.data` named exactly the symbol. Size does not matter, every reference is retargeted.
- For each defined-data symbol that is bind-to-old, nothing extra is needed; the obj's own definition satisfies the linker.
- We do **not** use `/FORCE:UNRESOLVED`, so genuine resolution failures surface as link errors.

### 4.6 Linking the patch and locating relocation sites

Link command (link.exe or lld-link, mirroring the family that built the module; flags to be finalised in WP-09):

```
<linker> /nologo /DLL /NOENTRY /NODEFAULTLIB /DEBUG:FULL /INCREMENTAL:NO
         /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN /FIXED /DYNAMICBASE:NO
         /BASE:0x<reserved> /PDB:<patch>.pdb /OUT:<patch>.dll
         /IGNORE:4001,4254,4281 foo.obj binding.obj
```

`/NOENTRY /NODEFAULTLIB`: the patch has no CRT of its own; every CRT reference binds to the host's. `/FIXED /BASE`: load at the reserved address or fail (then retry with another reservation).

**Anchor symbols.** Before linking, the broker rewrites `foo.obj` to add one static symbol `__cml_anchor_<n>` at offset 0 of every section (COMDAT sections keep their selection; associative sections are anchored too). The patch PDB then tells us where each object section landed. For each relocation in `foo.obj` whose target symbol is bind-to-old, the fix-up is:

```
site_va  = patch_base + rva(anchor of section containing site) + reloc.offset
target   = resolved address of symbol (+ symbol's section offset if the symbol is section-relative in the obj)
REL32..REL32_5 : write (int32)(target - (site_va + 4 + N))
ADDR64         : write (uint64)target
ADDR32NB       : only legal if target is inside the patch (thunk) → otherwise diagnostic
SECREL/SECTION : TLS and debug-only; TLS deferred to WP-24, debug sections never retargeted
```

The fix-up list is serialised to the agent as `(rva, kind, value)` triples. Range check every `REL32` and fail the patch (unload DLL) if out of range rather than writing garbage.

Thunk slots: `__cml_slot_<name>` addresses come from the patch PDB publics; the agent writes the resolved 64-bit address.

### 4.7 Address-space reservation

On module enable, the agent scans the address space with `VirtualQuery` and reserves (`MEM_RESERVE`) N blocks (default 4 × 64 MB, configurable) inside `[module_base − 1.5 GB, module_end + 1.5 GB]`, excluding ranges that would be out of `REL32` range from any part of the module. For each patch the broker picks a block, the agent releases exactly it, and immediately calls `LoadLibraryExW` with the `/FIXED` base inside it. On failure (another thread grabbed the range) re-reserve and retry with the next block up to 5 times. Patches are small; a 64 MB block is sub-allocated by linking consecutive patches at increasing bases (the broker tracks a bump pointer per block, aligned to 64 KB).

### 4.8 Hook installation

Mechanism, chosen per function at install time:

1. **Padding present** (6 × `0xCC` or `0x90` immediately before entry, and first instruction ≥ 2 bytes as reported by a tiny length decoder): write `jmp rel32` into the padding, then atomically write the 2-byte `jmp -7` (`EB F9`) at entry with a 16-bit interlocked exchange. No thread suspension strictly required, but we still do the suspend-and-check for uniformity.
2. **No padding**: 5-byte `jmp rel32` at entry. Requires the first ≥5 bytes to be fully owned (use the length decoder to find instruction boundaries; if the function is shorter than 5 bytes and has no trailing alignment padding, fail with a diagnostic).
3. Target is always within ±2 GB by construction (§4.7), so no 14-byte absolute jumps are needed on x64 in v1.

Procedure (agent, on the applying thread):

- Enumerate threads via `NtGetNextThread` or toolhelp snapshot; suspend all except the current one; loop until a snapshot shows no new threads.
- For each suspended thread, `GetThreadContext`; if `RIP` falls inside any `[entry, entry + bytes_written)` window, resume all, `SwitchToThread`, retry (max 50, then fail the patch with a diagnostic naming the thread and function).
- `VirtualProtect` → write → restore protection → `FlushInstructionCache`. Resume all.
- Record the original bytes so a patch can be reverted and so hot-restart can report state.
- Never allocate, log or take locks between suspend and resume. Pre-size all buffers.

Re-patching the same function (patch 2 of the same TU): re-point the *original* entry hook to the newest implementation. Functions introduced by patch N and changed in patch M get a hook at their patch-N entry (they are canonical in the registry).

### 4.9 Sync points, hooks, modes

- `cml::SyncPoint()` is called by the app, typically once per frame on the main thread. In *synchronised* mode, the agent prepares everything on its own thread and performs the hook write at the next `SyncPoint()`. In *immediate* mode, the agent thread applies as soon as ready.
- `CML_PREPATCH_HOOK(fn)` / `CML_POSTPATCH_HOOK(fn)`: function pointers placed in section `.cmlhk$a`/`.cmlhk$b` via `__declspec(allocate)`; the agent enumerates them in the enabled modules and in each patch DLL (the latter via the patch PDB). Post-patch hooks in the patch let the user reconstruct state after a layout change by hand.
- `CML_COMPILE_START_HOOK` / `CML_COMPILE_SUCCESS_HOOK` / `CML_COMPILE_ERROR_HOOK` for in-game notifications.
- Modes: compile on hotkey (default: Ctrl+Alt+F11), compile on save (opt-in), with a per-session "apply automatically" toggle.

### 4.10 Dynamic initialisers and destructors

Only `.CRT$XCU` entries of *new* globals run, in object order, after retargeting and before hooks, on the applying thread. `atexit` registrations from patch code call the host CRT's `atexit` (bound via publics), which is correct. Destructors of patched classes: old objects have old vtables whose entries lead through hooks to new code. Nothing special.

### 4.11 Crash → patch → continue

The agent installs a vectored exception handler (first) for `EXCEPTION_ACCESS_VIOLATION`, `EXCEPTION_INT_DIVIDE_BY_ZERO`, `EXCEPTION_ILLEGAL_INSTRUCTION`, breakpoints when configured. If the faulting address is inside an enabled module and no debugger is attached (or the user opted in even with a debugger), the handler blocks the faulting thread on an event and notifies the broker with thread id, context, and the function name (resolved by the broker). The UI offers: **Continue** (resume at faulting instruction, used after a fix that makes the instruction safe), **Leave function** (unwind one frame with `RtlVirtualUnwind`, set `RAX=0`, resume at the return address), **Ignore** (pass to the next handler). After a patch is applied, "Continue" or "Re-execute function" (`RIP` = entry of the new function with the four register arguments reloaded from the frame's home space; only offered for compilands built without optimisation) are enabled. v1 ships Continue, Leave, Ignore; Re-execute is WP-23.

### 4.12 Hot-restart

The agent records the process's command line, cwd and environment at start. On request the broker asks the agent to run user `CML_PRERESTART` hooks, terminates the process, relaunches it with the same parameters and a `CML_RESTART_TOKEN` env var, waits for re-registration, and re-enables the same modules. Debugger re-attach is the IDE plugin's job (M6).

### 4.13 IPC protocol

Named pipe `\\.\pipe\cml_broker_<session_sid>`, one connection per agent. Messages are little-endian `uint32 len, uint16 type, uint16 version, payload`. Payloads are hand-rolled structs with explicit serialisation in `cml_ipc` (no third-party schema compiler; keep it debuggable). Message families: `Hello/Welcome` (versions, pid, agent path), `ModuleList/EnableModule/ModuleLoaded/ModuleUnloaded`, `ReserveRange/ReleaseRange`, `LoadPatch/PatchLoaded`, `ApplyFixups`, `RunInitializers`, `InstallHooks/HooksInstalled`, `SyncPointReady`, `Exception/ExceptionAction`, `Log`, `Restart`. Every request has an id and a reply with a status code and a UTF-8 message. Agent never blocks the app's threads waiting on the broker except at `SyncPoint()` when a patch is ready, bounded by a timeout.

### 4.14 Change detection

`ReadDirectoryChangesW` on the union of directories containing any source or header known from the PDB indices (recursive where directory count would explode). Events are debounced (150 ms) and confirmed by content hash (xxh3) against a per-file baseline taken lazily at first sight. Hotkey mode does a hash sweep of files with changed mtime since last sweep. Saves that do not change content (formatter no-ops) produce no compile.

---

## 5. Public API (header `Chameleon_API.h`, C API plus thin C++ wrapper)

```cpp
// Loading: the header locates and LoadLibrary()s cml_agent.dll; no link dependency.
cml::Agent agent = cml::CreateDefaultAgent(L"C:/tools/chameleon");         // immediate mode
cml::Agent agent = cml::CreateSynchronizedAgent(L"C:/tools/chameleon");    // requires SyncPoint()

agent.EnableModule(cml::GetCurrentModulePath(), cml::ModuleOptions::AllImports);
agent.EnableModule(L"Game.dll", cml::ModuleOptions::None);
agent.DisableModule(...);

agent.SyncPoint();                 // once per frame on the thread that should apply patches
agent.ScheduleReload();            // same as hotkey
agent.WaitForReload(timeoutMs);    // tests and tools
agent.ScheduleRestart();
agent.SetGlobalHotkeys(...);

CML_PREPATCH_HOOK(OnBeforePatch);   CML_POSTPATCH_HOOK(OnAfterPatch);
CML_COMPILE_START_HOOK(...);  CML_COMPILE_SUCCESS_HOOK(...);  CML_COMPILE_ERROR_HOOK(...);
CML_PRERESTART_HOOK(...);

cml::DestroyAgent(agent);
```

C API mirrors every call (`cml_create_default_agent`, …) for C projects and FFI. Header compiles clean at `/W4 /WX` and `-Wall -Wextra -Werror` (clang-cl), C++14 and C99.

---

## 6. Repository layout, toolchain, conventions

```
/CMakeLists.txt                  C++20, MSVC + clang-cl, Ninja and VS generators, /W4 /WX
/cmake/                          toolchain helpers, warnings, install rules
/core/cml_util      logging (ring buffer + file), arena, string utils, result<T>, hashing (xxh3)
/core/cml_coff      COFF reader (sections, symbols, relocs, COMDAT), writer (binding/anchors)
/core/cml_pe        PE headers, exports, imports, debug dir, exception dir
/core/cml_pdb       wrapper over third_party/raw_pdb: compiland index, file index, symbol lookup
/core/cml_ipc       pipe transport + message definitions + (de)serialisers
/core/cml_proc      threads, modules, memory, address space scanning (shared agent/broker)
/core/cml_toolchain command-line parsing/rewriting for cl/clang-cl, link/lld-link; invocation
/core/cml_watch     directory watcher, debounce, hash baseline
/agent/            cml_agent.dll + public include/Chameleon_API.h
/broker/cml_core    session, process registry, bind planner, retargeter, patch pipeline
/broker/cml_cli     cml_broker.exe --headless, --control-pipe, scripting for tests
/broker/cml_gui     Dear ImGui + D3D11 front-end (process list, modules, log, compile output, crash dialog)
/integrations/vscode  extension: hotkey, output channel, status item (M6)
/integrations/vs      VSIX: commands + debugger re-attach after hot-restart (M6)
/integrations/cmake   ChameleonConfig.cmake and `chameleon_enable(target)` adding recommended flags
/tests/unit            doctest-based unit tests per core lib
/tests/fixtures/       small apps, one per feature (see §10), each buildable via CMake with MSBuild and Ninja
/tests/harness/        cmltest.exe: builds fixture, launches with agent, drives broker through control pipe, applies scripted edits, asserts on fixture stdout
/third_party/          raw_pdb, imgui, doctest, xxhash (vendored, pinned, licences recorded)
/docs/                 IMPLEMENTATION_PLAN.md (this file), DECISIONS.md, KNOWN_LIMITATIONS.md, BUILD_REQUIREMENTS.md, wp/WP-NN.md
/.github/workflows/    windows-2022 + windows-2025, MSVC and clang-cl, Debug + RelWithDebInfo
```

---

## 7. Conventions for every subagent (paste into each task)

- C++20, MSVC and clang-cl clean at `/W4 /WX`. No exceptions in `agent/` and `core/` (use `cml::Result<T>`); exceptions allowed in GUI and tests. No RTTI dependency in `agent/`.
- `agent/` has no third-party dependencies, links the static CRT (`/MT`), and never allocates while threads are suspended. Every public function in the agent must be callable from any thread.
- Every new code path gets a unit test or a fixture scenario in the same PR. Tests run via `ctest`. No PR without green CI.
- Every user-visible failure is a `cml::Diag` with a stable id (`CML1xxx` parse/PDB, `CML2xxx` compile, `CML3xxx` link/bind, `CML4xxx` load/hook, `CML5xxx` runtime), a one-line message naming the symbol/file/function, and a "what to do" sentence. Add it to `docs/KNOWN_LIMITATIONS.md` if it is a by-design limitation.
- Do not touch files outside your WP's listed directories without saying so in the PR description. Do not refactor neighbours.
- Write `docs/wp/WP-NN.md` on completion: what was built, how to run it, what was learned, what was deferred with reasons.
- Never read, disassemble or copy Live++ binaries or headers. Public blog posts, talks and open-source projects under permissive licences are fine as references; do not copy GPL code.
- Logging: `CML_LOG(level, "...")` with structured fields; verbose logs are the main debugging tool for users, so log every decision in the bind planner at `trace`.
- Commit messages: imperative, reference the WP id.

---

## 8. Work packages

Format: **WP-NN Title** (size S/M/L; depends on). Goal. Deliverables. Acceptance.

### Milestone M0 — Foundations and end-to-end spike

**WP-01 Repository skeleton and CI** (S; none)
Goal: buildable monorepo. Deliverables: CMake tree per §6, warning flags, doctest wiring, GitHub Actions matrix (windows-2022, windows-2025 × MSVC, clang-cl × Debug, RelWithDebInfo), `third_party/` vendoring with licence notes, `docs/` stubs. Acceptance: empty libs and a hello-world `cml_broker --version` build and test in CI on all matrix cells.

**WP-02 cml_coff and cml_pe readers** (M; WP-01)
Goal: parse COFF objects (big object `/bigobj` too) and PE64 images. Deliverables: section table, symbol table with aux records (section definitions, COMDAT selection, weak externals), relocations with AMD64 type enum, string table; PE headers, section headers, export/import/debug/exception directories, in-memory and on-disk variants. Acceptance: unit tests on objects produced by cl and clang-cl from fixture `basic` covering COMDAT functions, static data, string literals, `.pdata/.xdata`, `.CRT$XCU`, weak externals (`/alternatename`), `__imp_` references; round-trip identity test for the writer added in WP-06.

**WP-03 End-to-end spike: replace one function by hand** (L; WP-02)
Goal: prove §4.1–§4.8 before any further investment. A throwaway `spike/` program that, given fixture `basic` running with a stub agent, (1) recompiles `logic.cpp` with a hard-coded command, (2) builds a binding object by hand for the two externals it uses, (3) links a `/FIXED` DLL at an address reserved in the target, (4) loads it via a remote `LoadLibraryW` thread, (5) retargets the `REL32` references using anchor symbols read with raw_pdb, (6) installs a 5-byte jump with thread suspension, and the fixture's per-second printed value changes. Acceptance: demo recorded as a script in `spike/README.md`, runs on MSVC and clang-cl builds of the fixture, debugger (VS) shows the patched function with symbols when attached. Document every surprise in `docs/DECISIONS.md`. **Gate for M1.**

### Milestone M1 — Broker pipeline for a single module, single process, function bodies only

**WP-04 cml_pdb index** (M; WP-01)
Goal: fast indexed access to the PDB data in §4.3. Deliverables: open PDB by path or from PE debug directory, GUID/age validation, compiland table (object name, `S_COMPILE3`, `S_ENVBLOCK`), per-compiland file list, inverted file → compilands map, symbol lookup by (mangled name, compiland) and by mangled name globally (publics + globals), section:offset → RVA, section contributions. In-memory cache keyed by PDB GUID/age; load time budget: 2 s for a 500 MB PDB, memory budget: 1.5× PDB size max. Acceptance: unit tests against fixture PDBs from cl and clang-cl; benchmark test on a large PDB (build LLVM or similar in CI once and cache it, or use a synthetic large PDB).

**WP-05 Toolchain command parsing and rewriting** (M; WP-04)
Goal: §4.4. Deliverables: cl/clang-cl argument tokeniser (Windows rules, response files, quoting), rewrite rules as data, detection of toolchain family and version, invocation with captured output and cancellation, parallel job pool. Also decide and document detection of `/Gy`, `/GL`, `/guard:cf`, `/hotpatch` (compile a probe or inspect PDB/PE: `/GL` via object format of the recorded `.obj` if present, else the `S_COMPILE3` flags; CFG via load config directory). Verify clang-cl `S_ENVBLOCK` emission across clang 17–20 and record the minimum version. Acceptance: table-driven tests for 40+ real-world command lines (collect from VS, CMake/Ninja, FASTBuild, Unreal UBT builds); fixture TUs recompile and produce objects whose symbol set matches the original compiland's symbols.

**WP-06 cml_coff writer: anchors and binding object** (M; WP-02)
Goal: §4.5 binding object and §4.6 anchor insertion. Deliverables: COFF writer; anchor insertion that preserves COMDAT associativity and symbol indices used by relocations (append symbols, never reorder); binding object generator (thunks, slots, placeholders) given a list of symbols with kinds. Acceptance: link.exe and lld-link accept the output; unit test disassembles thunks; anchored objects link to byte-identical code sections compared with unanchored.

**WP-07 Bind planner** (L; WP-04, WP-06)
Goal: §4.5 decisions and resolution. Deliverables: classification of every symbol in a new object, resolution chain with the process symbol registry, diagnostics `CML3xxx` for unresolved and for unsupported reloc kinds, fix-up list generation (`rva, kind, value`), trace logging of every decision. Acceptance: unit tests using fixture objects with known expected plans (golden JSON); negative tests for unresolved symbols and `ADDR32NB` to old data.

**WP-08 Address-space reservation and patch loading (agent side)** (M; WP-03)
Goal: §4.7 and the agent half of §4.6. Deliverables: in `cml_agent.dll`: range scanning/reservation per module, release-and-load with retry, apply fix-ups (with `REL32` range checks), write thunk slots, run selected initialisers, unload on failure. Acceptance: unit tests inside a test host exe; stress test loading 500 small patches; failure injection for base collisions.

**WP-09 Linker driver** (M; WP-05, WP-06)
Goal: §4.6 link command for link.exe and lld-link with family auto-detection; PDB of the patch opened with WP-04 to recover anchor RVAs and slot addresses. Acceptance: patches for all M1 fixtures link under both linkers; link time budget under 300 ms for a typical TU.

**WP-10 Hook installer (agent side)** (L; WP-03)
Goal: §4.8. Deliverables: thread enumeration/suspension with convergence loop, RIP-in-window check, 2-byte-with-padding and 5-byte strategies, small x64 instruction-length decoder (table-driven, covers prologue instructions; fail safe on unknown opcodes), original-byte recording, revert. Acceptance: unit tests hooking functions in the test host under heavy multithreaded load (16 threads spinning through the hooked functions) for 60 s with zero crashes; coverage for functions shorter than 5 bytes with and without padding.

**WP-11 Agent skeleton, public header and IPC** (M; WP-01)
Goal: §4.13 and §5 minus hooks/exceptions. Deliverables: `cml_ipc` transport and messages, `cml_agent.dll` lifecycle (connect, auto-start broker, hello, module enumeration, enable/disable), `Chameleon_API.h` with C and C++ API, `cml::SyncPoint` plumbing, environment variable overrides for broker path and pipe name. Acceptance: header compiles in C99/C++14/C++20 under both compilers at max warnings; integration test: fixture registers with a headless broker and enables its module.

**WP-12 Broker core: session, pipeline, headless CLI and control pipe** (L; WP-04, WP-05, WP-07, WP-08, WP-09, WP-10, WP-11)
Goal: wire §3 data flow end to end. Deliverables: process/module registry, PDB index loading on enable (async, with progress), patch pipeline orchestration with per-stage timing, control pipe (`cml_broker --control`) exposing `reload`, `status`, `wait`, `set-option` for tests and IDEs, structured log sink. Acceptance: `cmltest` (WP-13) scenarios `basic`, `statics`, `virtuals`, `templates`, `lambdas`, `new_function`, `new_global` pass on MSVC and clang-cl × Debug and RelWithDebInfo(/Ob0).

**WP-13 Test harness `cmltest`** (M; WP-11)
Goal: §10 harness. Deliverables: scenario files (`scenario.toml`: fixture, build config, steps: `edit file with patch`, `reload`, `expect stdout regex within N s`, `expect diag CMLnnnn`), fixture builder for MSBuild and Ninja, process supervision with timeouts and crash dumps collection, JUnit output for CI. Acceptance: harness runs the `basic` scenario in CI on all matrix cells.

### Milestone M2 — Change detection, headers, multiple modules and processes

**WP-14 File watcher and change detection** (M; WP-04)
Goal: §4.14. Acceptance: unit tests for debounce, rename/atomic-save patterns (VS, VS Code, Rider, vim), content-unchanged saves; scenario `save_triggers_reload` with compile-on-save enabled.

**WP-15 Header dependency handling** (M; WP-04, WP-12)
Goal: header change → recompile all including compilands across modules; PCH header detection; batching and progress reporting; cap with a confirmation prompt when more than N compilands (default 64) are affected. Acceptance: scenarios `header_change_two_tus`, `pch_header_change_rejected`.

**WP-16 Multiple modules and multiple processes** (M; WP-12)
Goal: one process with EXE plus DLLs; two processes sharing a module (server/client). One compile per distinct command line, one link/load per process. Registry per process. Acceptance: scenarios `exe_plus_dll`, `two_processes`.

**WP-17 Pre/post patch hooks, compile hooks, synchronised mode** (S; WP-11, WP-12)
Goal: §4.9. Acceptance: scenario `sync_point_apply` proves hooks are installed only inside `SyncPoint()` on the main thread; `hooks_order` checks pre → install → post.

**WP-18 Diagnostics pass and build-requirement checks** (M; WP-05, WP-12)
Goal: implement every diagnostic in §1 and §4.2 with ids; `cml_broker check <exe>` command that audits a module before enabling. Acceptance: negative scenarios for `/GL`, `/guard:cf`, `/DEBUG:FASTLINK`, missing PDB, missing `S_ENVBLOCK`.

### Milestone M3 — GUI

**WP-19 ImGui broker UI** (L; WP-12)
Goal: usable daily driver. Deliverables: process and module list with enable toggles, change list, compile output panel with clickable errors (opens in configured editor via `code -g`, `devenv /edit`, or custom command), patch history with timings, log view with filtering, settings (hotkeys, compile-on-save, jobs, editor command), tray icon, single-instance. Acceptance: manual checklist in `docs/wp/WP-19.md` plus screenshot tests not required; all control-pipe operations reachable from UI.

### Milestone M4 — Runtime robustness features

**WP-20 Crash → patch → continue** (L; WP-10, WP-12, WP-19)
Goal: §4.11 with Continue, Leave function, Ignore. Acceptance: scenario `crash_null_deref_fixed` (fixture dereferences null each frame; patch fixes; Continue resumes), `crash_leave_function`.

**WP-21 Hot-restart** (M; WP-11, WP-12)
Goal: §4.12. Acceptance: scenario `hot_restart` shows the relaunched process re-registering and re-enabling modules automatically.

**WP-22 Patch revert and session reset** (S; WP-10, WP-12)
Goal: revert the last patch or all patches (restore original bytes, keep DLLs loaded), useful after a bad change. Acceptance: scenario `revert`.

**WP-23 Re-execute function after crash** (M; WP-20)
Goal: §4.11 re-execute for unoptimised compilands using home-space argument reload. Acceptance: scenario `crash_reexecute`.

**WP-24 Thread-local variables** (M; WP-07, WP-08)
Goal: `SECREL`/`SECTION` relocations for TLS variables bound to old storage via the module's `.tls` section and `_tls_index`; new TLS variables rejected with a diagnostic in this WP (patch-local TLS would need `.tls` registration). Acceptance: scenario `tls_read_write`.

**WP-25 Unchanged-function elision** (M; WP-07)
Goal: skip hooking functions whose new code is byte-identical to the old (compare with relocation sites masked, using the old code bytes from the target and the relocation list of the new object). Reduces risk and improves "no functional change" feedback. Acceptance: scenario `only_one_function_changed` reports one hook.

### Milestone M5 — Hardening and performance

**WP-26 Real-project corpus** (L; WP-12…WP-25)
Goal: run the tool against three open-source C++ codebases of increasing size (suggested: a raylib game, Dear ImGui demo with a custom main loop, and a large one such as Godot's editor or a UE sample if licensing permits in CI, otherwise LLVM's `clang` executable built with `/Zi`). Record every failure as an issue with a reduced fixture. Acceptance: documented pass/fail matrix; all failures either fixed or added to `KNOWN_LIMITATIONS.md` with diagnostics.

**WP-27 Performance budgets** (M; WP-12)
Goal: measure and meet budgets: PDB index 2 s/500 MB, compile → applied under 1.5 s for a 2 k-line TU on a typical workstation excluding compiler time, broker idle CPU under 0.5 %, memory 1.5× PDB. Acceptance: perf tests in CI with thresholds and trend output.

**WP-28 Fuzzing and failure injection** (M; WP-02, WP-04)
Goal: libFuzzer (clang-cl) targets for COFF/PE/PDB readers and the IPC deserialiser; injected failures for every pipeline stage produce a diagnostic and leave the target running. Acceptance: fuzzers run 10 minutes per CI nightly with no crashes; injection scenarios pass.

### Milestone M6 — Integrations and release

**WP-29 CMake integration module** (S; WP-18)
Goal: `chameleon_enable(target)` sets recommended flags; `find_package(Chameleon)`; example projects. Acceptance: fixtures use it.

**WP-30 VS Code extension** (M; WP-12)
Goal: commands (reload, restart, revert), output channel with problem matcher, status bar item, talks to the control pipe. Acceptance: manual checklist; packaged `.vsix`.

**WP-31 Visual Studio extension** (M; WP-21)
Goal: commands, output pane, automatic debugger re-attach after hot-restart (the main reason this exists). Acceptance: manual checklist; `.vsix`.

**WP-32 Packaging, docs, licensing hook** (M; all)
Goal: installer-free zip layout (`cml_broker.exe`, `cml_agent.dll`, `include/Chameleon_API.h`, integrations), quickstart, build-requirements doc, troubleshooting by diagnostic id, a `cml_license` interface with a no-op implementation (commercial enforcement is out of scope for the engineering plan). Acceptance: fresh-machine quickstart followed by a subagent with no prior context succeeds.

---

## 9. Dependency graph and delegation waves

```
Wave 0 : WP-01
Wave 1 : WP-02, WP-04, WP-11                         (parallel)
Wave 2 : WP-03 (spike; gate), WP-05, WP-06, WP-13    (WP-05/06/13 may start, but no M1 merges before WP-03 passes)
Wave 3 : WP-07, WP-08, WP-09, WP-10                  (parallel after gate)
Wave 4 : WP-12                                       (integration; single agent, you review closely)
Wave 5 : WP-14, WP-15, WP-16, WP-17, WP-18           (parallel)
Wave 6 : WP-19, WP-20, WP-21, WP-22, WP-24, WP-25    (parallel; WP-20 after WP-19 has a crash dialog or uses CLI first)
Wave 7 : WP-23, WP-26, WP-27, WP-28
Wave 8 : WP-29, WP-30, WP-31, WP-32
```

Rules: at most four implementation subagents concurrently; each wave merges into `main` before the next starts; you run the full `ctest` suite after every merge. Any WP that fails acceptance twice gets split or spiked by you personally before re-delegation.

---

## 10. Test fixtures (`tests/fixtures/`)

Each fixture is a console app with a 100 ms main loop printing `TICK k=v …` markers, linked with the agent, buildable with `cmake -G "Visual Studio 17 2022"` and `-G Ninja`, in Debug and RelWithDebInfo with `/Ob0`, under cl and clang-cl. Each has one or more `.edit` files (unified diffs) applied by the harness.

| Fixture | Exercises |
|---|---|
| `basic` | free function returning a constant; the canonical smoke test |
| `statics` | file-static data, function-local statics with guards, static member data, `const` data in `.rdata` |
| `virtuals` | class hierarchy, existing objects keep working, virtual override body change |
| `templates` | template instantiations in multiple TUs (COMDAT), changed body |
| `lambdas` | captured lambdas, `std::function` stored across frames |
| `new_function` | add a function and call it from an existing one |
| `new_global` | add a global with a dynamic initialiser and a destructor |
| `exceptions` | C++ throw/catch inside patched function; SEH `__try/__except` |
| `strings_and_floats` | string literal and FP constant pools |
| `exe_plus_dll` | patched DLL function called from EXE, and vice versa, imports via `__imp_` |
| `two_processes` | same DLL in two processes |
| `header_change_two_tus` | inline function in a header used by two TUs |
| `pch` | project with PCH; TU change OK; PCH header change rejected |
| `threads` | 8 worker threads constantly calling the patched function |
| `tls` | `thread_local` read/write (WP-24) |
| `crash_*` | null deref, divide by zero; fixed by patch |
| `sync_point` | asserts hooks are installed on the main thread inside `SyncPoint()` |
| `optimised` | `/O2` build; demonstrates the inlining warning |
| `bigobj` | `/bigobj` object |
| `c_only` | a C99 TU using the C API |

---

## 11. Risks and mitigations specific to this design

| Risk | Mitigation |
|---|---|
| `/FIXED` load at reserved base fails under heavy allocation churn | Retry across blocks; fall back to manual mapping with debugger-invisible patch and a loud warning (WP-08 stretch) |
| Instruction-length decoder meets an unexpected prologue | Decoder is conservative: unknown opcode → prefer padding strategy, else fail patch with diagnostic; corpus test over all function entries in large PDBs (WP-26) |
| Thread suspension deadlocks (heap lock, loader lock) | No allocation or Win32 calls that take locks in the window; `LoadLibrary` happens outside the window; watchdog resumes threads after 2 s and fails the patch |
| Anchor insertion perturbs linker output | Verified by byte comparison in WP-06; alternative is section contributions from the PDB |
| clang-cl drops `S_ENVBLOCK` in some version | `compile_commands.json` fallback; documented minimum version |
| Compiler updates change CodeView details | PDB reader tests pinned per toolchain version in CI matrix; nightly run on preview toolchains |
| Patch chains grow address space | Patches are kilobytes; per-block bump allocation; session reset unloads everything after revert |
| Accidental dependence on Live++ internals | Clean-room rule in §7; reviewers check for it |

---

## 12. Later phases (not for delegation now; keep the Windows code shaped for them)

- **ARM64 Windows:** hooks need a 16-byte sequence (`ldr x16, +8; br x16; .quad target`) or `b` within ±128 MB; keep the hook installer behind an arch interface from day one.
- **Linux x64:** ELF object reader, DWARF/`.symtab` index, compile commands from `compile_commands.json` or `-grecord-command-line`, patch as a shared object linked with `-shared -nostdlib -Wl,-Ttext-segment` or a custom in-process ELF loader plus the GDB JIT interface for debugger visibility. Thread suspension via signals. Expect to revisit §4.1 here.
- **macOS arm64:** Mach-O, hardened runtime and JIT entitlements, pointer authentication; development builds only.
- **Consoles:** PS5, Xbox Series, Switch via platform middleware programs; register early because approvals take months; the agent/broker split already supports remote targets over TCP (add a transport to `cml_ipc`).
- **Rust and Zig:** same object formats and relocations; symbol classification rules differ (monomorphisation, no static-local guards); needs `-C debuginfo=2` and codeview on Windows.
- **Struct layout migration and on-stack replacement:** research items after GA.

---

## 13. Open questions for the orchestrator to settle early

1. `/Gy` detection method (WP-05) or simply require it and detect by absence of COMDAT function sections in a probe compile.
2. Whether `lld-link` is the default patch linker even for link.exe-built modules (faster, fewer surprises) once WP-09 measures both.
3. Minimum clang-cl version given `S_ENVBLOCK` behaviour.
4. Whether to ship the ImGui GUI or a lighter Win32 tray + log window first; recommendation is ImGui because the crash dialog and compile output need real UI.
5. Editor-open command defaults per IDE.

End of plan.
