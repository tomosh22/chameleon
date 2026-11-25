# Reduced reproductions of the corpus findings (WP-26)

Each file reproduces one finding of `docs/wp/WP-26.md` in a few lines. They are sources only (not built by `ctest`:
the fixture directories are registered by the orchestrator); build one with the flags of `docs/BUILD_REQUIREMENTS.md`,
start it under `cml_broker --headless`, edit the marked function and `reload`.

| File | Finding | Expected today |
|---|---|---|
| `f1_static_library/` | F-1, members of a static library are not reloaded | the reload patches nothing (no diagnostic) |
| `f2_image_relative_data.c` | F-2, `CML3101` for a cl function that addresses globals image-relatively (a switch with a jump table that touches global arrays, or two global arrays indexed). Replaces `f2_global_array_index.c`, which did not reproduce it | fixed by WP-36 (ADR-0042): the reload succeeds and the patched code uses the running image's data; the broker logs `WP36-IMGREL sites=N verified=N`. Fixture `tests/fixtures/imgrel`, scenario `imgrel` |
| `f3_c_statics_clang_cl.c` | F-3, clang-cl: statics of a C translation unit | after the reload `counter` restarts from 0 (or the process crashes, in raylib's case) |
| `f4_static_overloads.cpp` | F-4, `CML3214` for overloaded static functions | fixed by WP-37: the edit reloads (fixture `static_overloads`) |
| `f42_plugin_reload/` | F-42.1, F-42.2, F-42.3 (`docs/wp/WP-42.md`): a plugin DLL loaded with `LoadLibrary` after start-up, freed and loaded again, and a hot-restart that loads it late | F-42.1: after the DLL is loaded again it runs its original code, silently; F-42.2: the first reload of it after that fails with CML3350 (LNK1104 on the freed instance's `g1\patch_1_1.dll`); F-42.3: after the restart its edits are not re-applied when it is loaded, silently (only CML5809 at the restart) |

The corpus workflow asserts the same behaviour on the real projects (`expected_failures` in
`tests/corpus/<project>/project.json`), and `tests/unit/broker/test_audit.cpp` pins F-1 on a synthetic PDB.
