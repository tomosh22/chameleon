# Chameleon — Known Limitations (v1, Windows x64)

Every by-design limitation is diagnosed, never silently wrong. Ids are stable (ADR-0003).
"Implemented by" names the WP that must emit the diagnostic; until that WP merges, the
diagnostic is planned, not live.

Follow-on work for these limitations (and the product gaps that are not diagnostics) is prioritised in
`docs/ROADMAP.md` under stable ids `R-nn`.

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
| CML3101 | Image-relative (`ADDR32NB`) reference from patch to old-module data that is not a proved sign-extended disp32 of a memory operand with a base register (since WP-36 the common `cl` form `[base+index*scale+sym@IMGREL]` is retargeted) | `<function>` references `<symbol>` in `<module>` image-relatively (ADDR32NB); this cannot be retargeted (with the instruction bytes and the reason). | Move the reference behind a function or pointer, or hot-restart. | WP-07, WP-36 | **live** |
| CML3102 | Thread-local reference shape that cannot be retargeted (SECTION/SECREL7 relocation, SECREL against a symbol that is not a thread-local of the old image, address-of-TLS through REL32/ADDR64, ordinary data that was `thread_local` in the old image) | `<function>` references thread_local `<symbol>` in a way that cannot be retargeted. | Move the thread_local to an unchanged source file, or hot-restart. | WP-07 (narrowed by WP-24) | **live** |
| CML3400 | New `thread_local` variable the old image does not have (also a static that gained `thread_local`) | `<symbol>` is a new thread_local; patch-local thread-local storage is not supported. | Hot-restart, or move the new thread_local to an unchanged source file. | WP-24 | **live** |
| CML3401 | TLS dynamic initialiser/destructor in a patched TU (warning) | The old image's initialiser and destructor stay in effect; a changed initialiser is not run. | Hot-restart if the changed initialiser must take effect. | WP-24 (unit-tested only; no CI fixture has a dynamic `thread_local`) | **live** |
| CML3402 | Several `S_GTHREAD32` records for one thread_local name | Ambiguous thread_local `<symbol>`. | Rename one of them, or hot-restart. | WP-24 | **live** |
| CML3103 | Layout of a type with live instances changed | Layout of `<T>` changed; existing objects not migrated. Use a post-patch hook or hot-restart. | Migrate state in a CML_POSTPATCH_HOOK or hot-restart. | WP-07/WP-18 | **not done in WP-18**: needs the UDT layouts (TPI) of the old PDB and of the patch, which cml_pdb does not read yet (WP-04 left type records out) |
| CML3207 | A static data symbol whose name matches several records of the old compiland (same scope) and cannot be told apart by name, closure (lambda statics, WP-37), RVA in a known section layout, or order within one procedure with equal count, types, block nesting and bound guards (WP-37). **Unity builds (WP-41, F-41.1):** a translation unit or unity batch with several overloads of one function that each hold a guarded local static (Jolt's `GetRTTIOfType`, one per RTTI macro) pair by procedure (cl since WP-40; clang-cl since WP-46, whose record-less guards are then placed by the layout their statics teach, or refused with CML3600). **WP-40 over-refusals:** a function's own static `x` next to a static `x` of one of its lambdas (in the patch or the old image); two functions that `/OPT:ICF` folded to one RVA. **Not detected (R-54, WP-46):** under clang-cl -O2 a lambda inlined everywhere that an edit deletes, with a static of its static's name added to the caller, binds that static to the lambda's live object | Static `<name>` matches `<n>` records named `<id>` in compiland `<i>` ... (with the reason). | Rename one of the statics, keep their number unchanged, or hot-restart. | WP-07, WP-37 | **live**; under cl two same-named block statics of a `static` or anonymous-namespace function are still refused (qualified record names) |
| CML3210 | Clang's TU-wide initialiser (`_GLOBAL__sub_I_<file>`) in a TU that has new globals AND globals bound to the old image (warning) | Initialiser `<fn>` initialises every global of `<obj>`, including ones bound to the old image; running it may re-initialise them. | Put new globals with dynamic initialisation into a separate source file, or hot-restart. | WP-07 | **live** |
| CML3211 | Guard variable of a function-local static cannot be located in the old image because the TU's static layout changed (warning) | Guard variable `<name>` of a function-local static was not found in the old image; the patch gets a fresh guard. | Keep the layout of the statics in this source file unchanged, or hot-restart. | WP-07 | **live** |
| CML3212 | Reference through a section symbol (private/section-relative) into a section that holds data bound to the old image (warning) | Object section `<n>` holds data bound to the old image and is also referenced through its section symbol. | Give the data external linkage or avoid private references to it. | WP-07 | **live** |
| CML3214 | Overloads of a static function (same scope, same name) that cannot be paired by signature: equal signatures, or types that cannot be read (no type records, `LF_PRECOMP`, malformed), or the set of lambdas of the enclosing function changed. **Unity builds (WP-41):** both fixed by WP-46: the closure-set count no longer includes call operators that pair by their public name (F-41.2), and the scalar deleting destructor `??_G` is no longer a destructor candidate (F-41.3) | Ambiguous overloads of static `<name>` in `<obj>`; cannot tell which old function each new one replaces (with the reason). | Rename one overload, or hot-restart. | WP-07, WP-37 | **live** (signatures from the PDB type stream since WP-37) |
| CML3570 | Same-named function-local statics in different blocks of one function were paired by their order in the function (warning; only where the section layout is unknown, i.e. cl) | Function-local statics `<name>` of `<function>` (`<n>`) are matched by their order in the function; if you reordered or replaced blocks that declare them, their values may be exchanged. | Rename one of them, or hot-restart after reordering. | WP-37 | **live** |
| CML3600 | A function-local static's guard cannot be bound consistently with the statics it guards (WP-40): some statics of a block bound and some new with no old guard record; clang-cl's record-less guard that the section layout cannot place; a bitmask guard (`/Zc:threadSafeInit-`, `-fno-threadsafe-statics`) of a function whose code changed, so an edit of a function with function-local statics is refused in those modes while other functions of the file reload. Also clang-cl: a plain static added to a block next to a bound guarded one. | Hot-restart, or move the static to file scope. See TROUBLESHOOTING. |
| CML3601 | A static the compiler split into pieces (`name.0`, `name.1`, ...) whose pieces the PDB does not record, so the live pieces cannot be found and a reload would reset them (WP-40, R-48 item 4). Seen in raylib under clang-cl (`rprand_state` in rcore.c, `edit:header-inline-raymath`); cl keeps the variable whole. | Give the variable external linkage or take its address, or hot-restart. |
| CML3602 | Warning: lambdas of one function whose code changed and that code comparison cannot tell apart are paired by their compiler-assigned number (WP-40 rule (d)); if their order changed, their statics may be exchanged. | Check the named lambdas' state after the reload, or hot-restart. |
| CML3603 | A lambda whose number's old lambda was taken by a code-equivalent new lambda: the remaining lambdas cannot be paired (WP-40). | Hot-restart. |
| CML3630 | cl's legacy lambda processor (C++17 without `/permissive-` or `/Zc:lambda`) names lambdas `<lambda_HASH>` from their body, so an edited lambda is renamed (WP-46). One edited lambda per reload pairs by elimination when its signature, statics, closure layout and enclosing function (the one non-template procedure holding a closure-typed local) match and every other lambda of the unit is proven unchanged by code. Refused: two or more edited lambdas in one reload, a lambda added or removed, a lambda whose code cannot be compared, an edited lambda used only as a temporary (no closure-typed local), and an edited lambda passed to a template that has its own function-local statics. A lambda renamed by an earlier reload counts as edited until a restart. | Edit one lambda per reload, give lambdas a named local, build with `/Zc:lambda` (implied by `/permissive-` and C++20), or hot-restart. |
| CML3215 | A dynamic initialiser (`??__E...`) of the patched TU belongs to no global the planner can see (unexpected mangling) | Dynamic initialiser `<sym>` could not be matched to a new global. | Report the symbol names; move the global to an unchanged source file, or hot-restart. | WP-07 | **live** |
| CML1504 | C++/CLI (`/clr`) or C++/CX (`/ZW`) compilands | Compiland `<obj>` is built with /clr or /ZW; managed and WinRT code cannot be hot-reloaded. | Hot-reload only native compilands. | WP-05 (compile), WP-18 (audit) | **live** |
| CML1601 | Functions without 6 bytes of padding before them (warning; /FUNCTIONPADMIN, /hotpatch) | Functions of `<module>` have no hot-patch padding (`<n>` of `<m>` sampled); short functions cannot be hooked atomically. | Compile with /hotpatch and link with /FUNCTIONPADMIN. | WP-18 | **live** |
| CML1602 | /OPT:REF or /OPT:ICF in effect, or the linker command line is not recorded (warning) | Module `<module>` is linked with /OPT:REF and /OPT:ICF; functions may be removed or folded. | Link with /OPT:NOREF /OPT:NOICF. | WP-18 | **live** |
| CML1603 | Optimised compilands with inlining on (warning) | `<n>` of `<m>` compilands of `<module>` are optimised with inlining; small changed functions may be inlined into callers that keep the old code (CML3104). | Build iterated modules with /Ob0 or /Od. | WP-18 | **live** |
| CML2210 | `/GL` or LTO removed for the patch compile (warning) | `<obj>` was built with /GL (or -flto); the patch is compiled without it. | Disable /GL for iterated modules to avoid behaviour differences. | WP-05 | **live** |
| CML3104 | Small changed function of a compiland optimised with inlining (warning, one per compiland and reload) | Compiland `<obj>` of `<module>` is optimised with inlining and `<n>` small changed function(s) (`<names>`) may be inlined into their callers: a caller that is still executing, or whose own code did not change, keeps the old code. | Build iterated modules with /Ob0 or /Od, or hot-restart. | WP-18 | **live** (advisory: see docs/wp/WP-18.md, "CML3104: what is and is not true") |
| CML4101 | Function too short to hook (<5 bytes, no padding) | `<function>` is shorter than 5 bytes and has no hot-patch padding; it cannot be hooked. | Build with /hotpatch and link with /FUNCTIONPADMIN. | WP-10 | superseded by CML4301 (live, see docs/wp/WP-10.md) |
| CML4300 | Unknown instruction in the bytes a hook overwrites: an instruction set the decoder does not cover (XOP, 3DNow!, FMA4, AMX, AVX512-FP16, APX) or bytes that are no instruction | The instruction at `<function>+N` (bytes ..) is not understood; the hook installer fails safe. | Report the bytes, or exclude the function. | WP-10, WP-38 | **live** (VEX and EVEX are decoded since WP-38) |
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
  and not enabled again by the restart; their edits are re-applied when they are enabled later (WP-45).
  A restart that leaves its new process running without finishing (CML5808, CML5809) remembers what it could not do for that
  process; the next `restart` of it does it. A late registration alone does not (WP-43).
- **A reload of several compilands is atomic per module (WP-47, R-52).** Every compiland of a module is planned, proven and
  linked before any is applied; a refusal applies nothing (CML3650). A failure while applying rolls back what that reload applied
  (CML3651); a roll-back that fails or is not confirmed is CML3652/CML3654, and if hooks were not confirmed (CML5309/CML4656) or the
  agent was lost mid-apply the reply says which compilands may be live (CML3653/CML3655). A reload across several modules or
  processes is still applied module by module (R-56).
- **Modules freed and loaded again, or enabled late after a hot-restart (WP-45).** A module instance enabled after the session has
  accepted edits for its sources (FreeLibrary + LoadLibrary, or a module the restarted process loads later, CML5809) gets those
  edits re-applied by a re-apply reload once its index is ready; each instance has its own patch work directory. A re-apply that
  fails is CML5600, an index that fails CML5601; both name the module and the files. Edits are carried only to the same process
  (restart token, command line and creation time). Limits: a FreeLibrary + LoadLibrary at the same base inside one agent module
  poll (500 ms) is invisible to the agent, and the new image runs its original code (R-53); when an agent reconnects to a live
  process, the module instances enabled at the disconnect leave no carry; a module whose index is not ready during a restart
  (CML5810) gets its edits re-applied when the index is ready, and CML5809/CML5810 name the files that could not be carried. `CML_RESTART_TOKEN` stays in the new process's environment (ADR-0034). `Agent::ScheduleRestart` still
  returns `CML_STATUS_NOT_IMPLEMENTED`. A broker started with a short `--exit-when-idle` can exit during the restart gap.
- Broker UI (WP-19): enable/disable of modules is shown but not switchable (the control protocol has no such command); a relative
  path in compiler output is resolved best-effort against the modules' directories.
- Unchanged-function elision (WP-25): a reload whose recompiled code is identical to the running code loads nothing and warns
  CML3450; one whose only change is the initial value of a writable global that lives in the running image warns CML3451 (the
  initialiser of an existing global does not run again). Read-only constants are fresh copies in the patch and do take effect.
  Data that is unreadable in the module file or holds pointers is not compared, so such an edit may get plain CML3450. The old
  bytes are read from the module file, not process memory. `hooks_kept` in the reload response lists at most 50 entries.
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
- **Re-execute after a crash (WP-23)** is offered only for an unoptimised frame of an enabled module with a loaded PDB. The recorded
  build command must end in `/Od`/`-O0`. Frames in patch code are never offered (patch PDBs are not indexed).
  - It reloads the parameters' values *at the fault*. They equal the call arguments only if the function did not assign to a
    parameter first. Nothing detects this; every offer carries `notes.reexecute`.
  - Side effects before the fault happen again.
  - Destructors and `__finally`/`__except` of the discarded frames do not run, so locks and handles held there stay held.
  - Never offered for: by-value aggregates among the first four parameters, member pointers, 80/128-bit floats, variadic
    functions, and functions returning a class by value (CML5900-5907).
  - A patch committed between the fault and the broker receiving the report counts as present at the fault. Re-execute is then not
    offered for that function until a later patch.
- **Failure injection (WP-28)** is compiled in by default (`CML_FAULT_INJECTION=ON`). Release packaging must configure with
  `-DCML_FAULT_INJECTION=OFF`; a fuzz.yml job proves that configuration builds.
  - Covered: the reload pipeline's 11 stages, the hot-restart's 7 and the revert's 3 (WP-43). An armed restart or revert stage
    applies to the next restart or revert only, whether it fired or not; unused stages are listed as `injection_disarmed`.
  - Covered too (WP-44): the crash handler (exception report, hold, the continue/leave/re-execute actions) and the compile-on-save
    watcher (debounce, change detection, the reload it starts). The watch group, restart, revert and exception-action stages refuse
    to arm while their operation runs (CML5978).
  - Arming persists across a broker restart only with `inject-failure {persist:true}` (state next to `--log`; CML5984 without one).
  - Test-harness limitations (WP-44): records of a process whose exit the broker did not see stay until a global
    `{"stage":"none"}` (or `none` for that pid once its exit is confirmed); broker-side armings made for a pid carry over to a new
    process that reuses it; `none` during a running cleanup operation can clear its records before the operation reports them.
  - The reservation stage fires only on a module's first patch; arming it later is refused (CML5970).
  - If an injected post-patch-hooks rollback is incomplete (CML4656), the patch image stays mapped with some hooks possibly live;
    restart the application. A later revert of that batch may report hooks it cannot find.
- **Fuzzing (WP-28)** covers the COFF, PE, PDB, IPC and control-JSON readers.
  - Not fuzzed: the user-hook scanner, the bind planner and the toolchain parser. The hook decoder is fuzzed since WP-38.
  - Only RelWithDebInfo is fuzzed (ASan needs the release CRT).
- **Real-project findings (WP-26 corpus, ADR-0036).**
  - **Image-relative references to old data (F-2) are fixed (WP-36, #67).** The common `cl` form is retargeted; what is
    still refused is listed under CML3101 above.
  - **Overloaded statics (F-4) and same-named lambda statics (Godot `SNAME`) are fixed (WP-37, #64).** Overloads pair
    by signature from the PDB type stream (also through a `/Zi` precompiled header's compiler PDB), lambda statics by
    their closure class, same-named block statics by order with CML3570. What still refuses is listed under CML3207 and
    CML3214. Five pre-existing pairing defects found on the way are R-48 (wave 11).
  - **Unreadable C static names (WP-26c):** a writable static whose name is neither a plain identifier, an MS-decorated
    name, a clang-cl C spelling (`a`, `a.N`, `f.x`, `f.x.N`) nor on the compiler-generated list is refused with CML3500
    rather than reset. No fixture or corpus case hits it.
  - **Static-library members (F-1)** are fixed (WP-26b, #49), and the clang-cl C-statics crash (F-3) is fixed (WP-26c, #50). Members
    without a recorded build environment (a prebuilt library not built with /Z7) stay unreloadable: CML5920 at enable,
    CML5921 when one of their files changes. Toolchain archives (CRT, STL, SDK, compiler runtime) are never reloadable.
  - **Compiler warnings from `chameleon_enable` (fixed in WP-33, #57).** `/hotpatch` is passed to clang-cl only, and
    `/Ob1`..`/Ob3` are stripped from the target directory's non-Debug flags by a deferred call at the end of that
    directory. Every other compiled target in the directory gets its original level back as a per-target, C/C++-only
    option. What is left:
    - Calling `chameleon_enable` from a parent directory on a target defined in a subdirectory that has already been
      processed cannot strip that directory's flags. CML1712 warns and cl prints D9025 for that target. Call it from the
      target's own directory.
    - With a Visual Studio generator, if a directory's C and C++ configuration flags carry different `/Ob` levels, the
      restored level on mixed C/C++ targets is the C++ one.
    - The VS-generator CI cell builds Debug only, so only D9002 is checked there. The Ninja cells check D9025.
  - **IDE integrations (wave 8).** The VS Code and Visual Studio extensions are tested against a real broker in CI. The editor
    UI itself (menus, status bar, Output pane, Error List, the debugger `Attach2` call) is covered only by the manual
    checklists in `docs/wp/WP-30.md` and `docs/wp/WP-31.md`.
    - *Structured diag locations* are done (WP-35, #58). `diag` events carry an absolute `file`, `line` and `column`,
      resolved against the directory the compiler actually ran in. When no absolute path can be formed, the fields are
      left out and the extensions fall back to their regexes.
    - *Early re-attach after hot-restart* is done (WP-34, #59): `restart {hold_new, hold_ms, token}`, the `restarting`
      event, `restart-resume` and a watchdog that resumes a held process if the broker dies. CI drives the extension
      with a fake debugger. **A real Visual Studio attach to a `CREATE_SUSPENDED` process is not verified in CI**;
      the extension resumes once the debugger lists the pid or after `HoldResumeGraceMs` (3 s), whichever comes first,
      and falls back to the late attach if the early one fails. The manual checklist in `docs/wp/WP-34.md` is the check.
    - *WP-34 review follow-ups* are fixed (WP-43, #71, R-40).
