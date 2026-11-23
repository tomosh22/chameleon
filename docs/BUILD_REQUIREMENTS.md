# Build requirements

Two audiences: building Chameleon itself from source (next table), and building *your* program so that it can be hot
reloaded (the section "What your own project must be built with"). Users of the release zip only need the second part;
`docs/QUICKSTART.md` walks through it.

## Using the release zip (`chameleon-<version>-win64.zip`)

| Requirement | Notes |
|---|---|
| Windows 10/11 x64 or Server 2022 or later | x64 only. |
| Visual Studio 2022 or Build Tools 2022 (workload "Desktop development with C++": MSVC v143, Windows SDK) | Chameleon recompiles your code with the same compiler and flags your build recorded in the PDB, so the compiler must be installed on the machine where you reload. The same install provides the Microsoft Visual C++ runtime (`VCRUNTIME140.dll`) that `cml_broker.exe` needs; on a machine without Visual Studio install the x64 "Microsoft Visual C++ Redistributable". |
| LLVM clang-cl 15 or newer | Optional alternative compiler for your program. |
| CMake 3.24+ and Ninja | Only to use `find_package(Chameleon)` / `chameleon_enable()` and the example; any build system works if you apply the flags below by hand. |
| An x64 Native Tools prompt | Start `cml_broker` from a prompt where `cl` works (environment variables `INCLUDE`, `LIB`, `PATH`). |

Nothing else is needed: no installer, no registry entries, no admin rights. Unpack the zip anywhere.

## Building Chameleon itself

| Requirement | Version | Notes |
|---|---|---|
| Windows | 10/11 x64, Server 2022/2025 | Only x64 is supported; configuration fails elsewhere unless `CML_ALLOW_NON_WINDOWS=ON`. |
| CMake | 3.24 or newer | 3.22+ is needed for `/external:I` SYSTEM includes with MSVC; 3.24 is the chosen floor (CI runs newer). |
| Visual Studio / Build Tools | 2022 (MSVC v143), Windows 10/11 SDK | Needed for `cl`, `link`, the SDK and the VS generator. |
| LLVM clang-cl | 15 or newer (CI uses what is preinstalled on the GitHub images) | Optional second compiler. Uses MSVC's `link.exe` and CRT. |
| Ninja | any recent | Optional; CI uses it. |

CI matrix: windows-2022 and windows-2025, each with MSVC `cl` and `clang-cl`, in Debug and
RelWithDebInfo (`.github/workflows/ci.yml`).

## What your own project must be built with (WP-18)

Check a module before enabling it: `cml_broker check [--json] [--verbose] [--pdb-dir DIR] <module.exe|module.dll>`. It
reads the module and its PDB without a running process, prints every check as PASS, WARN or FAIL with its diagnostic
id and exits with 1 on any FAIL. The broker runs the same checks when a module is enabled: a FAIL fails the module's
index (the reload then names the diagnostic), a WARN is published as a warning diagnostic (stage `enable`).

| Requirement | Id | Verdict if not met |
|---|---|---|
| x64 image | CML1106 | FAIL |
| PDB found next to the module or at its recorded path, matching it | CML1410 / CML1411 | FAIL |
| Full PDB: `/DEBUG:FULL`, not `/DEBUG:FASTLINK` | CML1101 | FAIL |
| Compile command recorded in the PDB (`/Zi`, `/ZI`, `/Z7`; clang-cl records it by default) | CML1102 | FAIL when no compiland has one, WARN when some lack it |
| No `/GL` (LTCG) | CML1103 | FAIL |
| No `/guard:cf` | CML1104 | FAIL |
| `/Gy` (function-level linking; detection method: ADR-0026) | CML1105 | FAIL when absent, WARN when it cannot be told |
| No `/clr`, `/ZW` | CML1504 | FAIL |
| 6 bytes of padding before functions: `/FUNCTIONPADMIN` at link time (the check samples the image, it does not require a compile flag); `/hotpatch` adds the 2-byte-entry guarantee on x86 cl and on clang-cl, and cl on x64 rejects it (D9002), so `chameleon_enable` passes it to clang-cl only | CML1601 | WARN |
| `/OPT:NOREF /OPT:NOICF` | CML1602 | WARN (also when the PDB records no linker command line) |
| `/Ob0` or `/Od` for the modules being iterated | CML1603 (per function at reload: CML3104) | WARN |

`chameleon_enable(<target>)` (CMake package `Chameleon`, shipped in the zip under `lib\cmake\Chameleon`) applies these
flags to a target. The `examples\hello` project in the zip shows it, and its `SMOKE_USE_PACKAGE=OFF` branch shows the same
flags written out by hand.

`chameleon_enable` prints no compiler driver warnings (WP-33): `/hotpatch` goes to clang-cl only (cl on x64 ignores it with
D9002; the padding comes from `/FUNCTIONPADMIN`), and CMake's default `/Ob1` or `/Ob2` is removed from the non-Debug
`CMAKE_<LANG>_FLAGS_<CONFIG>` before `/Ob0` is added, so cl does not print D9025. CMake cannot drop a configuration flag
for one target, so the removal is made in the calling directory's scope (the variables, not the cache); other targets of
that directory lose their default `/Ob1` or `/Ob2` too. `ALLOW_INLINING` skips the removal. If you write the flags by
hand, drop `/hotpatch` for cl x64 and `/Ob1` from your RelWithDebInfo flags the same way.

`/INCREMENTAL` is fine (the thunks jump to the real entry, which is hooked). `tests/fixtures/CmlFixture.cmake` shows
the recommended flags in CMake form.
