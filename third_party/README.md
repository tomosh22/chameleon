# Third-party code

Vendored by copying source at a pinned revision (no submodules, no upstream `.git`). Tests,
examples and docs were removed; every LICENSE file is kept next to the code. Nothing here is
GPL. None of it is compiled with `/WX`, and the include directories are `SYSTEM`.

| Name | Upstream | Pinned version | Licence | CMake target | Used by |
|---|---|---|---|---|---|
| raw_pdb | https://github.com/MolecularMatters/raw_pdb | commit `43cc59b7037238ad84e1d5abe10711a06d998fa0` (upstream `main`, 2026-06-24; the project publishes no tags) | BSD-2-Clause (Molecular Matters GmbH) | `cml_third_party_raw_pdb` (static lib, relaxed warnings) | `core/cml_pdb` |
| Dear ImGui | https://github.com/ocornut/imgui | tag `v1.93.0` = commit `c1aaac65e896a45f81f47a4d68f9959e0f86c859` | MIT | `cml_third_party_imgui` (only with `CML_BUILD_GUI=ON`) | `broker/cml_gui` |
| doctest | https://github.com/doctest/doctest | tag `v2.5.3` = commit `2d0a9359a60c51affe2a9bebb1be1dca47868151` | MIT | `cml_third_party_doctest` (interface, single header) | `tests/` |
| xxHash | https://github.com/Cyan4973/xxHash | tag `v0.8.4` = commit `c87183a77d67f7d37e3d2d1b7eaac5e7c695e4f0` | BSD-2-Clause (library; the CLI is GPL-2.0 upstream and is not vendored) | `cml_third_party_xxhash` (interface, used with `XXH_INLINE_ALL`) | `core/cml_util` |

Note: the plan lists raw_pdb as MIT; upstream's LICENSE is the 2-clause BSD licence. It is
permissive and compatible; the text is in `raw_pdb/LICENSE`.

## What was kept

- `raw_pdb/`: `LICENSE`, `README.md`, `raw_pdb.natvis`, `src/` without `src/Examples/` and
  without upstream's `src/CMakeLists.txt` (our own `third_party/CMakeLists.txt` globs `src/*.cpp`;
  the upstream precompiled header is not used, each source includes `PDB_PCH.h` itself).
- `imgui/`: `LICENSE.txt`, the core `imgui*.cpp/.h`, `imstb_*.h`, `imconfig.h`, and
  `backends/imgui_impl_win32.*` and `backends/imgui_impl_dx11.*`. Removed: `imgui_demo.cpp`
  (nothing calls `ShowDemoWindow`), `docs/`, `examples/`, `misc/`, other backends.
- `doctest/`: `LICENSE.txt` and `doctest.h`.
- `xxhash/`: `LICENSE`, `xxhash.h`, `xxh3.h`. Removed: `xxhash.c`, `cli/`, `tests/`, dispatch code.

## Local modifications

None. All files are byte-identical to upstream at the pinned revision.

## Updating

Clone the new tag, replace the directory contents with the same subset as above, update this
table, and run the full CI matrix.
