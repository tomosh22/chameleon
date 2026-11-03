# Build requirements

Stub, extended by later work packages.

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
| `/FUNCTIONPADMIN` and `/hotpatch` (6 bytes of padding before functions) | CML1601 | WARN |
| `/OPT:NOREF /OPT:NOICF` | CML1602 | WARN (also when the PDB records no linker command line) |
| `/Ob0` or `/Od` for the modules being iterated | CML1603 (per function at reload: CML3104) | WARN |

`/INCREMENTAL` is fine (the thunks jump to the real entry, which is hooked). `tests/fixtures/CmlFixture.cmake` shows
the recommended flags in CMake form.
