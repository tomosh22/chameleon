# Chameleon

Chameleon is a native C/C++ hot-reload tool for Windows x64: edit a function in a running
program, rebuild just that translation unit, and have the running process pick up the new code.

## What it is

Chameleon patches a running program in place. The pieces:

* **`cml_broker.exe`**: watches your sources, recompiles only the translation units that changed with the compiler
  command recorded in your PDB, links a patch DLL and tells the agent to hook the changed functions.
* **`cml_agent.dll`** (`include/Chameleon_API.h`): loaded by your program; applies patches, no third-party dependencies.
* **Clients** of the broker's control pipe: `cmltest` (test harness), `cml_ui` (tray front-end), the VS Code and Visual
  Studio extensions in `integrations/`, and the CMake helper `chameleon_enable(target)`.

## Quickstart

To use Chameleon you only need the release zip `chameleon-<version>-win64.zip` (built by `tools/package/package.ps1`, uploaded
by the `package` workflow): follow **`docs/QUICKSTART.md`**, from a fresh Windows machine to a first reload.
If something fails, every diagnostic id (`CMLnnnn`) is explained in `docs/TROUBLESHOOTING.md`; the build settings your
program needs are in `docs/BUILD_REQUIREMENTS.md`; what cannot be patched is in `docs/KNOWN_LIMITATIONS.md`.

Status and plan: `docs/IMPLEMENTATION_PLAN.md`, `docs/PROGRESS.md`.

## Building Chameleon from source

Requirements are listed in `docs/BUILD_REQUIREMENTS.md`: CMake 3.24+, Visual Studio 2022 (MSVC
v143) and optionally LLVM clang-cl. Use an x64 developer prompt for the Ninja generator.

```bat
:: Ninja + MSVC
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
build\bin\cml_broker.exe --version

:: Ninja + clang-cl
cmake -S . -B build-clang -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang-cl

:: Visual Studio generator
cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64
cmake --build build-vs --config Debug
ctest --test-dir build-vs -C Debug --output-on-failure
```

Options: `-DCML_BUILD_GUI=ON` builds the Dear ImGui front-end placeholder, `-DCML_BUILD_TESTS=OFF`
skips the unit tests, `-DCML_ALLOW_NON_WINDOWS=ON` lets the portable parts configure and compile on
other hosts for syntax checking only (not a supported configuration).

## Layout

See section 6 of `docs/IMPLEMENTATION_PLAN.md`. Vendored dependencies and their licences are
listed in `third_party/README.md`. Per-work-package notes are in `docs/wp/`.

## Licence

Chameleon is proprietary, commercial software: all rights reserved (see `LICENSE`, ADR-0039). Use
requires a separate written licence agreement. Bundled third-party code is under permissive licences
only (`third_party/README.md`); copyleft code must not be added.

## Release package

```bat
powershell -File tools\package\package.ps1 -OutDir build\package
```

builds RelWithDebInfo with `CML_FAULT_INJECTION=OFF` and `CML_BUILD_TESTS=OFF`, installs, adds the two `.vsix` files, the
docs and the third-party licences, writes `MANIFEST.txt` (SHA-256 of every file), checks the content (required files, no
debug-only or fault-injection strings) and zips it. `tools\package\smoke.ps1 -Root <unzipped>` then builds the example against
the unzipped tree and does one reload. The licensing hook is `core/cml_license` (a no-op provider; ids CML5998/5999).
