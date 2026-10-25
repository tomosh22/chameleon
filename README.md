# Chameleon

Chameleon is a native C/C++ hot-reload tool for Windows x64: edit a function in a running
program, rebuild just that translation unit, and have the running process pick up the new code.

Status: repository skeleton (WP-01). The libraries are empty shells and `cml_broker` only prints
its version. See `docs/IMPLEMENTATION_PLAN.md` for the plan and `docs/PROGRESS.md` for status.

## Build (Windows x64)

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
