# Chameleon — Known Limitations (v1, Windows x64)

Every by-design limitation is diagnosed, never silently wrong. Ids are stable (ADR-0003).
"Implemented by" names the WP that must emit the diagnostic; until that WP merges, the
diagnostic is planned, not live.

| Id | Limitation | User-facing message | What to do | Implemented by | Status |
|---|---|---|---|---|---|
| CML1101 | PDB built with `/DEBUG:FASTLINK` | Module `<module>` uses a /DEBUG:FASTLINK PDB; Chameleon needs /DEBUG:FULL. | Relink the module with /DEBUG:FULL. | WP-18 | **live** (`cml_broker check`, enable time) |
| CML1102 | Compile command unknown (no `S_ENVBLOCK`, no `compile_commands.json` entry) | Compiland `<obj>` has no recorded command line; it cannot be reloaded. | Build with MSVC /Zi or /Z7 (clang-cl: -gcodeview-command-line) or provide compile_commands.json. | WP-05 (compile), WP-18 (audit) | **live** |
| CML1103 | `/GL` (LTCG) compilands | Compiland `<obj>` in `<module>` was built with /GL (LTCG); its objects are IL, not COFF. | Disable /GL for modules you want to hot-reload. | WP-18 | **live** |
| CML1104 | `/guard:cf` modules | Module `<module>` is CFG-enabled (/guard:cf compile and /GUARD:CF link); patched code would not be a valid CFG call target. | Build with /guard:cf- for development configurations. | WP-18 | **live** (FAIL when the image is CFG-enabled; a compiland compiled with /guard:cf in a module linked without /GUARD:CF is a WARN) |
| CML1105 | Function-level linking (`/Gy`) absent | Compiland `<obj>` was not built with /Gy; functions cannot be patched individually. | Add /Gy to the compile flags. | WP-05/WP-18 | **live** (detection: ADR-0026; FAIL when absent, WARN "unknown, recommended" when it cannot be told) |
| CML1106 | Non-x64 module (x86, ARM64) | Module `<module>` is `<machine>`; only x64 is supported in v1. | Use an x64 build. | WP-18 | **live** (also for PE32 images, where the PE reader says CML1303) |
| CML2101 | Header included by the PCH changed | (superseded: reported as CML5550 by the broker, before anything is compiled) | - | WP-15 | superseded |
| CML5550 | Header included by the PCH changed | The changed file `<file>` belongs to the precompiled header of `<module>`; a precompiled header cannot be hot-reloaded. | Revert the change, or rebuild and restart; edit the TUs that use the PCH instead. | WP-15 | **live** |
| CML5552 | A compiland to recompile uses a precompiled header (`/Yu`) whose files cannot be determined | Cannot tell which files make up the precompiled header used by `<obj>`; a recompile could use a stale .pch. | Rebuild and hot-restart, or name the header with `/Yu<header>`. | WP-15 | **live** |
| CML5551 | A reload would recompile more compilands than `max_affected_compilands` (default 64) | The change affects `<n>` compilands of `<module>`, more than the limit of `<N>`. | Repeat `reload` with `"confirm": true`, or raise the option. | WP-15 | **live** |
| CML2102 | PCH-creating TU (`/Yc`, or clang `-emit-pch`) changed | `<file>` creates the precompiled header (/Yc); changes to it cannot be hot-reloaded. | Rebuild the project and hot-restart. | WP-05 | **live** |
| CML3101 | Image-relative (`ADDR32NB`) reference from patch to old-module data | `<function>` references `<symbol>` in `<module>` image-relatively (ADDR32NB); this cannot be retargeted. | Move the reference behind a function or pointer, or hot-restart. | WP-07 | **live** |
| CML3102 | Thread-local variable defined in a patched TU (v1; lifted in WP-24 for existing TLS) | `<file>` defines thread_local `<symbol>`; TLS in patched code is not supported in v1. | Move the thread_local to an unchanged TU, or hot-restart. | WP-07 (WP-24 narrows) | **live** |
| CML3103 | Layout of a type with live instances changed | Layout of `<T>` changed; existing objects not migrated. Use a post-patch hook or hot-restart. | Migrate state in a CML_POSTPATCH_HOOK or hot-restart. | WP-07/WP-18 | **not done in WP-18**: needs the UDT layouts (TPI) of the old PDB and of the patch, which cml_pdb does not read yet (WP-04 left type records out) |
| CML3207 | A static data symbol whose name matches several records of the old compiland (same scope) and cannot be told apart by position or by RVA in the unchanged section layout (ADR-0010) | Static `<name>` matches `<n>` records named `<id>` in compiland `<i>` but the patch object defines `<m>` of them. | Rename one of the statics, keep their number unchanged, or hot-restart. | WP-07 | **live** |
| CML3210 | Clang's TU-wide initialiser (`_GLOBAL__sub_I_<file>`) in a TU that has new globals AND globals bound to the old image (warning) | Initialiser `<fn>` initialises every global of `<obj>`, including ones bound to the old image; running it may re-initialise them. | Put new globals with dynamic initialisation into a separate source file, or hot-restart. | WP-07 | **live** |
| CML3211 | Guard variable of a function-local static cannot be located in the old image because the TU's static layout changed (warning) | Guard variable `<name>` of a function-local static was not found in the old image; the patch gets a fresh guard. | Keep the layout of the statics in this source file unchanged, or hot-restart. | WP-07 | **live** |
| CML3212 | Reference through a section symbol (private/section-relative) into a section that holds data bound to the old image (warning) | Object section `<n>` holds data bound to the old image and is also referenced through its section symbol. | Give the data external linkage or avoid private references to it. | WP-07 | **live** |
| CML3214 | Overloads of a static function (same scope, same name) cannot be told apart: the planner never pairs them by position, and cml_pdb has no type access to compare signatures yet | Ambiguous overloads of static `<name>` in `<obj>`; cannot tell which old function each new one replaces. | Rename one overload, or hot-restart. | WP-07 | **live** |
| CML3215 | A dynamic initialiser (`??__E...`) of the patched TU belongs to no global the planner can see (unexpected mangling) | Dynamic initialiser `<sym>` could not be matched to a new global. | Report the symbol names; move the global to an unchanged source file, or hot-restart. | WP-07 | **live** |
| CML1504 | C++/CLI (`/clr`) or C++/CX (`/ZW`) compilands | Compiland `<obj>` is built with /clr or /ZW; managed and WinRT code cannot be hot-reloaded. | Hot-reload only native compilands. | WP-05 (compile), WP-18 (audit) | **live** |
| CML1601 | Functions without 6 bytes of padding before them (warning; /FUNCTIONPADMIN, /hotpatch) | Functions of `<module>` have no hot-patch padding (`<n>` of `<m>` sampled); short functions cannot be hooked atomically. | Compile with /hotpatch and link with /FUNCTIONPADMIN. | WP-18 | **live** |
| CML1602 | /OPT:REF or /OPT:ICF in effect, or the linker command line is not recorded (warning) | Module `<module>` is linked with /OPT:REF and /OPT:ICF; functions may be removed or folded. | Link with /OPT:NOREF /OPT:NOICF. | WP-18 | **live** |
| CML1603 | Optimised compilands with inlining on (warning) | `<n>` of `<m>` compilands of `<module>` are optimised with inlining; small changed functions may be inlined into callers that keep the old code (CML3104). | Build iterated modules with /Ob0 or /Od. | WP-18 | **live** |
| CML2210 | `/GL` or LTO removed for the patch compile (warning) | `<obj>` was built with /GL (or -flto); the patch is compiled without it. | Disable /GL for iterated modules to avoid behaviour differences. | WP-05 | **live** |
| CML3104 | Small changed function of a compiland optimised with inlining (warning, one per compiland and reload) | Compiland `<obj>` of `<module>` is optimised with inlining and `<n>` small changed function(s) (`<names>`) may be inlined into their callers: a caller that is still executing, or whose own code did not change, keeps the old code. | Build iterated modules with /Ob0 or /Od, or hot-restart. | WP-18 | **live** (advisory: see docs/wp/WP-18.md, "CML3104: what is and is not true") |
| CML4101 | Function too short to hook (<5 bytes, no padding) | `<function>` is shorter than 5 bytes and has no hot-patch padding; it cannot be hooked. | Build with /hotpatch and link with /FUNCTIONPADMIN. | WP-10 | superseded by CML4301 (live, see docs/wp/WP-10.md) |
| CML4300 | Unknown instruction in the bytes a hook overwrites (VEX/EVEX prologue, ...) | The instruction at `<function>+N` (bytes ..) is not understood; the hook installer fails safe. | Report the bytes, or exclude the function. | WP-10 | **live** |
| CML4301 | Function shorter than 5 bytes with no padding before or after | `<function>` is too short for the 5-byte jump and no `CC`/`90` padding can be used. | Link with /FUNCTIONPADMIN or give the function a longer body. | WP-10 | **live** |
| CML4304 | A call ends inside the overwritten bytes | A call at `<function>+N` ends inside the hook's overwritten range. | Exclude the function or add padding before it. | WP-10 | **live** |
| CML4305 | Thread stayed inside the function's first bytes for 50 stops | Thread N kept executing inside the first bytes of `<function>`. | Wait for the application to leave the function and reload again. | WP-10 | **live** |

## Documented behaviour (no diagnostic)

- Compile on save (WP-14): a save whose change affects more compilands than `max_affected_compilands` is reported once as
  CML5551 and nothing is compiled; the files stay changed until an explicit `reload` with `"confirm": true` or a smaller save.
- Patch hooks (WP-17): a patch that hooks no function (only new functions or data) sends no `InstallHooks`, so no pre/post hooks
  run for it and hook records it adds stay invisible. Hooks run once per patch, so a reload of several files calls them several
  times. A hook deleted from the source keeps running (the old patch's records stay). `CML_PRERESTART_HOOK` is accepted but
  nothing calls it until hot-restart exists. Only a processor exception in a hook is contained, not state corruption; a hook must
  not call `DestroyAgent` (refused).
- Crash handling (WP-20) holds a faulting thread only while a control client subscribes to `exception` and `crash_handler` is on,
  only for access violations, integer divide-by-zero and illegal instructions, and only when the faulting address is in an enabled
  module or a patch (a fault inside a system DLL such as `memcpy(nullptr)` in ucrtbase is passed on). Breakpoints are not handled.
  Continue re-executes the faulting instruction with the registers it had, so it only helps when the patch changed what that
  instruction touches (memory state), not a bad value already in a register. Leave function skips `__finally` blocks and
  destructors of the frame it leaves and sets RAX/XMM0 to 0. No function name is reported for a fault inside a patch. A handler the
  application registers later as first runs before ours. At most 8 threads are held at once; a ninth fault is passed on. A held
  thread that owns the loader lock blocks patch loading until it is released. With a debugger attached nothing is held unless
  `break_with_debugger` is set. `subscribe` with an empty `events` array means the default set.
- Patch revert (WP-22): the unit of revert is one reload (`last` undoes every patch of the newest reload; modules that reload
  did not touch keep their patches). Patch DLLs stay loaded and the globals a reverted patch created stay alive, unused. No
  pre/post-patch hooks run around a revert; a hook already running from a reverted patch finishes. The reverted files stay
  changed on disk, so the next reload recompiles them. A module freed and reloaded at the same base and path is not detected as a
  different instance. A partial multi-module revert answers ok with `partial`/`failed` and a CML5854 diag event. Session reset
  (unloading patch DLLs) is not implemented.
- Hot-restart (WP-21) restores the command line, working directory, environment and the three standard handles; other inherited
  handles, the parent process, the console title and window state are not restored. With no console of its own, the broker
  borrows the old process's console during the relaunch. Reloads are refused (CML5304) while a restart runs. `timeout_ms` bounds
  the preparation (the pre-restart hooks); after the old process is terminated, `registration_timeout_ms` (default 30 s) bounds
  registration, re-enabling and re-applying the edits. Modules the application loads lazily after start-up are reported (CML5809)
  and not enabled again. `CML_RESTART_TOKEN` stays in the new process's environment (ADR-0034). `Agent::ScheduleRestart` still
  returns `CML_STATUS_NOT_IMPLEMENTED`. A broker started with a short `--exit-when-idle` can exit during the restart gap.
- Broker UI (WP-19): enable/disable of modules is shown but not switchable (the control protocol has no such command); a relative
  path in compiler output is resolved best-effort against the modules' directories.
- `unload_patch` (WP-17) waits up to 10 s for a hook that is still running and then refuses (CML4212, patch stays hidden); in
  synchronised mode that stalls the agent thread meanwhile.
- A thread that runs an application hook (the agent thread, or an application thread at `cml_sync_point`) gets a 64 KB stack
  guarantee (`SetThreadStackGuarantee`) the first time, so a stack overflow in a hook can be handled; that thread's usable stack
  is 64 KB smaller from then on (WP-17).
- Several enabled modules and several processes (WP-16): every enabled module of every process is its own unit (own registry,
  index, patches, hooks, source baseline). One `reload` compiles each distinct recorded command line once and then links and loads
  a patch per process. A process that is started after an edit was made enables its modules against the edited sources: its
  baseline is taken from the files on disk, while the process runs the binary it was built from, so that edit is not applied to
  it until the next change of the file (rebuild and restart such a process).
- `CML_TEST_FORCE_LOAD_COLLISIONS=<n>` (WP-16) is a test-only environment variable of the agent: it makes the first n patch loads of the process fail (CML4203) so the broker's relink can be tested. Never set it in a real application; the agent logs a warning at start when it is set.
- A reload that changes several source files makes one patch per file. If a later one fails,
  the earlier patches stay applied (and are counted in `generation`); fix the error and reload
  again (WP-12).
- A patch initialiser that fails part-way leaves the objects it already constructed behind;
  there is no undo (WP-12).
- Control-pipe events (`patch_applied`, `diag`, `log`) are delivered asynchronously and may
  arrive after the `reload` response they belong to. A subscriber that does not read for 5 s is
  disconnected (WP-12).
- Touching a file is not a change: only content hashes are compared (WP-12). A changed file that no
  compiland lists and the include scan cannot reach is warned as CML5303.
- Read-only data (`.rdata`, const tables, constants) is not bound to the old image: the patch uses a fresh copy, so edits to it take effect, but its ADDRESS changes across a patch (code of the old image that took and keeps the old address, or compares addresses, sees the old table).
- Statics only match old records of exactly the same scope (same enclosing function, or file scope): a static that moved to another function is a new, fresh variable, and a lambda moved to another function is new code that is not hooked.
- Functions currently executing on a thread's stack keep running old code until they return;
  the next call runs the new code.
- Hook installation (WP-10) cannot see a thread whose real resume point is inside the bytes being
  overwritten when its RIP is in ntdll's exception / APC / callback dispatcher at the moment it is
  stopped (the interrupted RIP is in a context record on its stack, which is not examined). In
  practice no compiler makes the first bytes of a function such a resume point.
- x86 (32-bit), ARM64, Linux and macOS targets are out of scope for v1 (CML1106 for modules;
  CML1303 when the PE reader sees a PE32 image).
- Modules loaded or unloaded by the host are noticed by the agent within about 500 ms
  (polling; ADR-0016). EnableModule always refreshes the list first.
- The agent DLL stays loaded until the process exits, even after `DestroyAgent`.
- Header changes reach every compiland that includes the header, on cl and clang-cl (WP-15,
  ADR-0013): the broker scans `#include` directives itself. The scan is textual: an include chosen
  by a macro (`#include MACRO`) is not followed, an include found only through the `INCLUDE`
  environment variable or compiler-built-in directories is not followed (those are system
  headers, which are not tracked anyway), and a changed `-I` list needs a rebuild and a re-enable.
  A header the scan cannot reach and the PDB does not list is reported as CML5303 (warning).
  A quoted include is searched in the including file's directory, the directories of the files above
  it on the include stack, then the `-I` list.
- A dynamic initialiser of a new global in a patch that faults (access violation, C++ exception)
  is caught and reported as CML4209, and the patch stays loaded. Recovery from a *stack overflow*
  inside such an initialiser is best effort (`_resetstkoflw`): it is not covered by a test, because a real
  overflow in the test host crashed one CI cell (WP-08). If the stack cannot be restored, the
  agent refuses to run further initialisers on that thread; hot-restart.
- Patch DLLs never appear in the module list the agent sends to the broker (WP-08).
- A *static* (TU-private) function whose parameter types change, keeping its name, is matched to
  its old namesake (the PDB carries no signatures for the planner yet; ADR-0029). Callers inside
  the patched TU are recompiled and are fine; an old function pointer to it held elsewhere would
  be called with the old signature. Rename the function when changing its signature, or hot-restart.
