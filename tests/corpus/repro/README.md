# Reduced reproductions of the corpus findings (WP-26)

Each file reproduces one finding of `docs/wp/WP-26.md` in a few lines. They are sources only (not built by `ctest`:
the fixture directories are registered by the orchestrator); build one with the flags of `docs/BUILD_REQUIREMENTS.md`,
start it under `cml_broker --headless`, edit the marked function and `reload`.

| File | Finding | Expected today |
|---|---|---|
| `f1_static_library/` | F-1, members of a static library are not reloaded | the reload patches nothing (no diagnostic) |
| `f2_global_array_index.c` | F-2, `CML3101` for a global array indexed by a variable (cl) | the reload is refused with CML3101 |
| `f3_c_statics_clang_cl.c` | F-3, clang-cl: statics of a C translation unit | after the reload `counter` restarts from 0 (or the process crashes, in raylib's case) |
| `f4_static_overloads.cpp` | F-4, `CML3214` for overloaded static functions | the reload is refused with CML3214 |

The corpus workflow asserts the same behaviour on the real projects (`expected_failures` in
`tests/corpus/<project>/project.json`), and `tests/unit/broker/test_audit.cpp` pins F-1 on a synthetic PDB.
