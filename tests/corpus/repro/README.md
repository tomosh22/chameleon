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
| `f41_1_f41_2_rtti_overloads.cpp` | WP-41 F-41.1 (`CML3207`, cl and clang-cl) and F-41.2 (`CML3214` for lambdas, cl): Jolt's RTTI shape, overloads of one function each with a guarded local static built from two lambdas; a unity batch holds several | reduction not yet run on Windows; the Jolt cells assert both |
| `f41_3_local_class_destructor.cpp` | WP-41 F-41.3 (`CML3214`, clang-cl): the implicit virtual destructor of a local class in a member function, two old records with one signature | reduction not yet run on Windows; the Jolt clang-cl cell asserts it |

The corpus workflow asserts the same behaviour on the real projects (`expected_failures` in
`tests/corpus/<project>/project.json`), and `tests/unit/broker/test_audit.cpp` pins F-1 on a synthetic PDB.
