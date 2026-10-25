# Chameleon — Known Limitations (v1, Windows x64)

Every by-design limitation is diagnosed, never silently wrong. Ids are stable (ADR-0003).
"Implemented by" names the WP that must emit the diagnostic; until that WP merges, the
diagnostic is planned, not live.

| Id | Limitation | User-facing message | What to do | Implemented by | Status |
|---|---|---|---|---|---|
| CML1101 | PDB built with `/DEBUG:FASTLINK` | Module `<module>` uses a /DEBUG:FASTLINK PDB; Chameleon needs /DEBUG:FULL. | Relink the module with /DEBUG:FULL. | WP-18 | planned |
| CML1102 | Compile command unknown (no `S_ENVBLOCK`, no `compile_commands.json` entry) | Compiland `<obj>` has no recorded command line; it cannot be reloaded. | Build with MSVC /Zi or /Z7 (clang-cl: -gcodeview-command-line) or provide compile_commands.json. | WP-18 | planned |
| CML1103 | `/GL` (LTCG) compilands | Compiland `<obj>` in `<module>` was built with /GL (LTCG); its objects are IL, not COFF. | Disable /GL for modules you want to hot-reload. | WP-18 | planned |
| CML1104 | `/guard:cf` modules | Module `<module>` is built with /guard:cf; patched code would not be a valid CFG call target. | Build with /guard:cf- for development configurations. | WP-18 | planned |
| CML1105 | Function-level linking (`/Gy`) absent | Compiland `<obj>` was not built with /Gy; functions cannot be patched individually. | Add /Gy to the compile flags. | WP-05/WP-18 | planned (detection method OPEN, WP-05) |
| CML1106 | Non-x64 module (x86, ARM64) | Module `<module>` is `<machine>`; only x64 is supported in v1. | Use an x64 build. | WP-18 | planned |
| CML2101 | Header included by the PCH changed | Header `<file>` is part of the PCH; rebuild and hot-restart. | Rebuild the project and hot-restart. | WP-15 | planned |
| CML2102 | PCH-creating TU (`/Yc`, or clang `-emit-pch`) changed | `<file>` creates the precompiled header (/Yc); changes to it cannot be hot-reloaded. | Rebuild the project and hot-restart. | WP-05 | **live** |
| CML3101 | Image-relative (`ADDR32NB`) reference from patch to old-module data | `<function>` references `<symbol>` in `<module>` image-relatively (ADDR32NB); this cannot be retargeted. | Move the reference behind a function or pointer, or hot-restart. | WP-07 | **live** |
| CML3102 | Thread-local variable defined in a patched TU (v1; lifted in WP-24 for existing TLS) | `<file>` defines thread_local `<symbol>`; TLS in patched code is not supported in v1. | Move the thread_local to an unchanged TU, or hot-restart. | WP-07 (WP-24 narrows) | **live** |
| CML3103 | Layout of a type with live instances changed | Layout of `<T>` changed; existing objects not migrated. Use a post-patch hook or hot-restart. | Migrate state in a CML_POSTPATCH_HOOK or hot-restart. | WP-07/WP-18 | planned |
| CML3207 | A static (file-static, function-local static, static function) whose name matches several records of the old compiland and cannot be told apart by position or by RVA in the unchanged section layout (ADR-0010) | Static `<name>` matches `<n>` records named `<id>` in compiland `<i>` but the patch object defines `<m>` of them. | Rename one of the statics, keep their number unchanged, or hot-restart. | WP-07 | **live** |
| CML3210 | Clang's TU-wide initialiser (`_GLOBAL__sub_I_<file>`) in a TU that has new globals AND globals bound to the old image (warning) | Initialiser `<fn>` initialises every global of `<obj>`, including ones bound to the old image; running it may re-initialise them. | Put new globals with dynamic initialisation into a separate source file, or hot-restart. | WP-07 | **live** |
| CML3211 | Guard variable of a function-local static cannot be located in the old image because the TU's static layout changed (warning) | Guard variable `<name>` of a function-local static was not found in the old image; the patch gets a fresh guard. | Keep the layout of the statics in this source file unchanged, or hot-restart. | WP-07 | **live** |
| CML3212 | Reference through a section symbol (private/section-relative) into a section that holds data bound to the old image (warning) | Object section `<n>` holds data bound to the old image and is also referenced through its section symbol. | Give the data external linkage or avoid private references to it. | WP-07 | **live** |
| CML3213 | A static function matches several records of the old compiland and cannot be told apart (warning); static function template instantiations are never matched (silently not hooked) | Static function `<name>` matches `<n>` records named `<id>` in compiland `<i>` but the patch object defines `<m>`; it is not hooked. | Rename the overloads or keep their number unchanged; callers outside the patch keep the old code of this function. | WP-07 | **live** |
| CML1504 | C++/CLI (`/clr`) or C++/CX (`/ZW`) compilands | Compiland `<obj>` is built with /clr or /ZW; managed and WinRT code cannot be hot-reloaded. | Hot-reload only native compilands. | WP-05 | **live** |
| CML2210 | `/GL` or LTO removed for the patch compile (warning) | `<obj>` was built with /GL (or -flto); the patch is compiled without it. | Disable /GL for iterated modules to avoid behaviour differences. | WP-05 | **live** |
| CML3104 | Stale inlined copies (warning) | `<function>` changed but compiland `<obj>` is optimised; inlined copies elsewhere keep the old code. | Build iterated modules with /Ob0 or /Od. | WP-18 | planned |
| CML4101 | Function too short to hook (<5 bytes, no padding) | `<function>` is shorter than 5 bytes and has no hot-patch padding; it cannot be hooked. | Build with /hotpatch and link with /FUNCTIONPADMIN. | WP-10 | superseded by CML4301 (live, see docs/wp/WP-10.md) |
| CML4300 | Unknown instruction in the bytes a hook overwrites (VEX/EVEX prologue, ...) | The instruction at `<function>+N` (bytes ..) is not understood; the hook installer fails safe. | Report the bytes, or exclude the function. | WP-10 | **live** |
| CML4301 | Function shorter than 5 bytes with no padding before or after | `<function>` is too short for the 5-byte jump and no `CC`/`90` padding can be used. | Link with /FUNCTIONPADMIN or give the function a longer body. | WP-10 | **live** |
| CML4304 | A call ends inside the overwritten bytes | A call at `<function>+N` ends inside the hook's overwritten range. | Exclude the function or add padding before it. | WP-10 | **live** |
| CML4305 | Thread stayed inside the function's first bytes for 50 stops | Thread N kept executing inside the first bytes of `<function>`. | Wait for the application to leave the function and reload again. | WP-10 | **live** |

## Documented behaviour (no diagnostic)

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
- Header changes on clang-cl builds may miss compilands whose included headers contain no code,
  until WP-15 adds include tracking for clang-cl (ADR-0013).
