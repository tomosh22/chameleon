# WP-26b: static-library members are reloadable (ADR-0036 decision 1)

## What was built

Finding F-1 of the real-project corpus: `ModuleIndex::is_source_compiland` rejected every compiland whose PDB module names an
archive, so code linked from a static library was never tracked and a reload after editing it answered `ok`, `patched: 0`.

- **The rule** (`broker/cml_core/src/module_index.cpp`). A compiland is a *source compiland* when its object name is a real
  object (not `Import:...`, not `* Linker *`) and its build environment (S_ENVBLOCK, or S_BUILDINFO for clang-cl: cwd, compiler,
  command, source) is recorded, whether or not it came from an archive. `ModuleIndex::is_object_compiland` is the cheap
  structural half; `is_source_compiland` adds the build-environment lookup.
- **Tracking and sweep.** `ModuleIndex::run()` tracks the files of every source compiland, archive members included. The sweep maps
  a changed file to the source compilands that list it; the pipeline compiles, links and patches them like any other. An archive
  member whose source file is not on disk is skipped (a library built on another machine would otherwise produce one
  "file is gone" warning per file).
- **Audit call sites** (`module_audit.cpp`). The two samplers (function symbols, padding) use `is_object_compiland`, so archive
  members take part. The flag examination (`/GL`, `/Gy`, `/clr`, no command line) deliberately stays on the module's own
  objects: those checks can FAIL the whole module at enable time, and a prebuilt third-party library without `/Gy` must not stop
  the module from being enabled. A member that violates one fails when it is reloaded, with the per-compiland diagnostic.
- `core/cml_toolchain/tools/tc_probe.cpp` (a probe tool) uses the same rule.
- **Pipeline.** The patch object's path hash now includes the archive name (two libraries may hold the same member name). Nothing
  else assumed "no library": the object, the binding object and the anchored objects are per compiland, PCH (`/Yu`) handling reads
  the compiland's recorded command, and the planner receives the compiland index.

## Diagnostics (CML5920-5939, `static_lib_diag_ids.h`)

| id | when | text |
|----|------|------|
| CML5920 | warning, once per module at enable time (diag event, stage `enable`) | "N object(s) linked into X from static libraries cannot be hot-reloaded: a.obj (reason), ... and K more." Names up to five. |
| CML5921 | warning, per reload (reload result `warnings`, stage `sweep`) | "the changed file 'F' is only owned by compilands of X that cannot be reloaded: a.obj in foo.lib (no build environment recorded)". |

The PDB reader range CML1470-1479 is unused.

### What the broker can know for the "not owned" warning

The sweep only sees tracked files, so a file that no compiland lists is invisible to it. The choice: at index time the broker also
tracks the *own source files* (C/C++ extensions, existing on disk) of every real object it skipped. When one of them changes and no
reloadable compiland owns it, the reload says so (CML5921, naming the skipped objects and why) instead of answering a silent
`patched: 0`; the file is then accepted, so it is reported once. A file that no compiland at all lists stays `CML5303`
(unmapped), as before. Not covered: a *header* that only skipped compilands include (they are not scanned, so it is not tracked),
and a source edited that is in no PDB at all.

### What counts as skipped (and is counted/warned)

Real objects with no build environment, or archive members whose source file is missing. Not counted: import stubs and linker
records, and members of toolchain archives (CRT/STL/SDK/compiler runtime: libcmt, libvcruntime, msvcrt, ucrt, anything under
`VC\Tools\MSVC`, `Windows Kits`, `lib\clang`). These never record a build environment, and counting them would make CML5920 fire for
every module. The warning is a diag event; the log also gets an `index:` line with the totals.

A plain (non-archive) object without a build environment is now skipped as well (before, it was tracked and failed at reload
with the "no recorded command line" error); its own source change is reported with CML5921.

## Tests

- Fixture `tests/fixtures/static_lib` (`add_library(fixture_static_lib_lib STATIC lib.cpp)`; the exe's TICK loop calls
  `static_lib_value()` and `static_lib_calls()` from the library) with `edits/value_70.edit`. Registered by the orchestrator in
  `tests/fixtures/CMakeLists.txt` (directory only added here); `cmltest` builds it standalone through the fixture wrapper.
- Scenario `static_lib.toml`: value 7 -> edit -> reload -> value 70, `compiled == 1`, `calls=` keeps counting (file-static of the member).
- Unit `tests/unit/broker/test_static_lib.cpp` (synthetic PDB: plain object, archive member with and without environment, member whose
  source is missing, import stub, CRT member): the rule, the skipped list, CML5920 once with count and names, the tracked files, the
  sweep of a changed member, and the not-owned file (CML5921 data).
- `unit.link_patch`: the original-source lookup no longer skips archive members.

## Deferred / limitations

- Flag problems of archive members (`/GL`, no `/Gy`) are not audited at enable time (see above).
- Headers included only by skipped compilands are not tracked.
- Corpus: the two F-1 `expected_failures` of the Godot cell (`edit:static-lib-main`, `edit:static-lib-scene-tree`, the latter an edit of
  `scene/main/scene_tree.cpp`) are removed, so those edits must now reload. `corpus F-1` in `test_audit.cpp` now asserts the new rule.
  (The raylib and imgui projects carry no F-1 entry.)
