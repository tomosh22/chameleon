# integrations/cmake

CMake support for projects that use Chameleon: `find_package(Chameleon)` and `chameleon_enable(target)`.

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyGame LANGUAGES CXX)

find_package(Chameleon CONFIG REQUIRED)      # -DCMAKE_PREFIX_PATH=<chameleon install prefix>, or -DChameleon_DIR=<prefix>/lib/cmake/Chameleon

add_executable(game main.cpp)
chameleon_enable(game)                       # flags + Chameleon::agent
```

Configure with Ninja from an x64 developer prompt, or with `-G "Visual Studio 17 2022" -A x64`; build `Debug` or `RelWithDebInfo`.
Then check the result: `<prefix>/bin/cml_broker check game.exe` reports no FAIL and no WARN.

## What `chameleon_enable(<target> [NO_AGENT] [ALLOW_INLINING] [NO_COPY_AGENT] [NO_BUILD_TYPE_CHECK])` does

It applies the flags of `docs/BUILD_REQUIREMENTS.md` for `cl` and `clang-cl`, with Ninja and the Visual Studio generators.

| Flags | Applied to | Why (id of the check) |
|---|---|---|
| `/Gy` `/hotpatch` | every target type | function-level linking, padding before functions (CML1105, CML1601) |
| `/Ob0` outside Debug | every target type | no inlining, so a changed function stays patchable (CML1603); Debug is `/Od` |
| `/DEBUG:FULL` `/INCREMENTAL:NO` `/OPT:NOREF` `/OPT:NOICF` `/FUNCTIONPADMIN` | executables, shared and module libraries | full PDB, no folding or stripping, padding (CML1101, CML1602, CML1601) |
| links `Chameleon::agent` | all but `NO_AGENT` | `Chameleon_API.h` include directory, the agent's import library; for an executable `cml_agent.dll` is copied next to it after the build |

Options: `NO_AGENT` (flags only; the fixtures use it, they load the DLL at run time), `ALLOW_INLINING` (keep `/Ob0` off for a
target you deliberately build optimised), `NO_COPY_AGENT`, `NO_BUILD_TYPE_CHECK` (skips CML1704; for this repository's fixtures). Call it in the directory that creates the target.
Applications do not need the import library: `Chameleon_API.h` loads `cml_agent.dll` with `LoadLibrary`, so linking `Chameleon::agent` costs
nothing at run time. Optimised builds work but are not what Chameleon is for (the broker warns CML1603 / CML3104).

The same file works in the source tree (`include(integrations/cmake/ChameleonEnable.cmake)`, which the top-level `CMakeLists.txt` and
`tests/fixtures/CmlFixture.cmake` do; `Chameleon::agent` is an alias of `cml_agent` there) and from an install tree
(`ChameleonConfig.cmake` includes the installed copy).

## Configure-time messages (CML1700-CML1749)

Each is `message(WARNING|FATAL_ERROR "CML17xx: <what> What to do: <what to do>")`, so `grep CML17` finds it in a configure log.

| Id | Severity | Meaning |
|---|---|---|
| CML1700 | error | target is not Windows x64 (architecture from the VS platform or the compiler, not the host; also raised by `find_package` when the project is not 64-bit) |
| CML1701 | error | compiler is not `cl` or `clang-cl` (a GNU-style `clang`, gcc) |
| CML1702 | warning | the target has no compiled code (INTERFACE or utility target); no flags applied |
| CML1703 | error | `chameleon_enable(x)`: no such target |
| CML1704 | warning | single-configuration build type is not Debug or RelWithDebInfo |
| CML1705 | warning | interprocedural optimisation (`/GL`) is on; switched off for the target (`check` fails `/GL`, CML1103) |
| CML1706 | warning | flags in effect (`CMAKE_*_FLAGS[_<CONFIG>]`, linker flags, directory and target options) that `check` rejects: `/guard:cf` (CML1104), `/GL`, `/LTCG` (CML1103), `/clr`, `/ZW` (CML1504), `/DEBUG:FASTLINK` (CML1101) |
| CML1707 | warning | `cl` older than 19.30 (VS 2022) or clang-cl older than LLVM 15 |
| CML1708 | error | the target is IMPORTED; its flags were fixed elsewhere |
| CML1709 | error | unknown argument to `chameleon_enable` |
| CML1710 | error | `Chameleon::agent` does not exist: call `find_package(Chameleon CONFIG REQUIRED)` first, or pass `NO_AGENT` |
| CML1711 | warning | `chameleon_enable` was called in another directory than the target's; `cml_agent.dll` is not copied |
| CML1712 | warning | the target belongs to a directory that is already processed (`chameleon_enable` called from a parent after `add_subdirectory`): CMake's default `/Ob1` `/Ob2` cannot be removed there and cl prints D9025 |

CML1713-CML1749 are unused. The ids are exercised by the ctest tests `cmake.enable.*` (`integrations/cmake/tests`).

## The install tree

`cmake --install <build> --prefix <dir>` (the tree WP-32 packages):

```
bin/                          cml_broker.exe  cml_agent.dll  [cml_ui.exe]  and PDBs (Debug, RelWithDebInfo)
include/Chameleon_API.h
lib/cml_agent.lib
lib/cmake/Chameleon/          ChameleonConfig.cmake  ChameleonConfigVersion.cmake  ChameleonTargets*.cmake  ChameleonEnable.cmake
```

Imported targets: `Chameleon::agent`, `Chameleon::broker` (cml_broker.exe, usable in `add_custom_command`), `Chameleon::ui` when present.
Variables: `Chameleon_ROOT_DIR`, `Chameleon_BIN_DIR`, `Chameleon_INCLUDE_DIR`. The tree is relocatable (CI moves it before use and greps the
exported files for absolute build paths). The version is 0.1.0 (`SameMinorVersion`).

## Examples

`examples/minimal` (one executable) and `examples/exe_plus_dll` (an executable and a DLL; both reloadable) are standalone projects:

```
cmake -S integrations/cmake/examples/minimal -B build-minimal -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=<prefix>
cmake --build build-minimal
<prefix>/bin/cml_broker check build-minimal/minimal.exe
```

CI (`.github/workflows/ci.yml`, step "WP-29 install tree and CMake examples", driver `tests/run_examples.ps1`) installs the build, builds both
examples against the moved install tree in every matrix cell (cl and clang-cl, Debug and RelWithDebInfo, plus the Visual Studio generator),
runs `cml_broker check` on each binary and prints `WP29-EXAMPLE <name> <compiler> <config> check=PASS warn=0 ...`.
