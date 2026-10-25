# tests/fixtures

Small console apps, one per feature (plan §10). Each prints `TICK k=v ...` every 100 ms and is
declared with `cml_add_fixture(<name> SOURCES ...)` from `CmlFixture.cmake`, which creates:

- `fixture_<name>_objs`: OBJECT library (consumers read per-TU objects via `$<TARGET_OBJECTS:...>`)
- `fixture_<name>`: the exe (`$<TARGET_FILE:...>`, PDB via `$<TARGET_PDB_FILE:...>`)

Flags: `/Gy /hotpatch`, link `/DEBUG:FULL /INCREMENTAL:NO /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN`,
`/Ob0` in RelWithDebInfo, and the normal exception-enabled STL (the project-wide
`_HAS_EXCEPTIONS=0` is removed for this directory). Everything else is the compiler's ordinary
Debug/RelWithDebInfo flags, as in a user project.

Each fixture keeps its unified-diff edits in `<name>/edits/*.edit` (applied with `-p1` relative
to the fixture directory).

| Fixture | Status | Notes |
|---|---|---|
| `basic` | present | `logic.cpp` is the patch target (two externals: `g_tick`, `fixture_note`); `extras.cpp` covers COFF features for reader tests; edit `value_2.edit` changes the printed value 1 → 2 |
