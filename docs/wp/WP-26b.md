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

The sweep only sees tracked files, so a file that no compiland lists is invisible to it. The choice: a skipped real object keeps
everything it lists tracked, exactly as before WP-26b (every file the PDB lists for it that exists on disk, headers included), and the
include scan and the precompiled-header detection still run for it whenever a compile command is recorded. When one of those files
changes and no reloadable compiland owns it, the reload says so (CML5921, naming the skipped objects and why) instead of answering a
silent `patched: 0`. A changed precompiled header of a skipped `/Yc` object is still refused (CML5550). The not-owned files are
accepted into the baseline right after the sweep, before any path that ends the reload (a refusal, a failure, nothing to compile),
so one edit is reported once. A file that no compiland at all lists stays `CML5303`
(unmapped), as before. Not covered: a source edited that is in no PDB at all; an archive member whose own sources are not on this machine (a library built
elsewhere: nothing of it is tracked, it only counts in CML5920).

### What counts as skipped (and is counted/warned)

Real objects with no build environment, or archive members whose source file is missing. Not counted: import stubs and linker
records, and members of toolchain archives (CRT/STL/SDK/compiler runtime). `ModuleIndex::is_toolchain_archive` is the one rule, shared
by the index and the audit's samplers: a path under `VC\Tools\MSVC`, `Windows Kits` or `lib\clang`, or a file name that is exactly
one of libcmt, libcpmt, libvcruntime, libucrt, msvcrt, msvcprt, libconcrt, oldnames, vcruntime, ucrt, clang_rt, legacy_stdio_definitions
(optionally with a `d` suffix), or such a name followed by `_`, `.` or `-`. These never record a build environment, and counting them
would make CML5920 fire for every module; the audit's padding and function-symbol samplers would also be skewed by a static CRT that is
not built with `/hotpatch`. The warning is a diag event; the log also gets an `index:` line with the totals.

A plain (non-archive) object without a build environment is skipped as well (before, it was tracked and a reload failed with the "no
recorded command line" error). Review decision: it keeps its headers tracked and its include scan and PCH detection, and a change is
reported with CML5921 (not the old compile error, and never a silent `patched: 0`). `is_source_compiland` does not check that an
archive member's source file exists (that needs the file system); the index does, and skips such a member (documented in
`module_index.h`).

## Known limitation found by the corpus

The corpus edit of `scene/main/scene_tree.cpp` gets past the sweep with this package and is then refused by the planner (CML3207 /
CML3214): the `SNAME` lambdas of `SceneTree` define several function-local statics with the same name (`$TSS0`, `sname`) that the
planner cannot tell apart. It belongs to the F-4 family (several statics of one name in a translation unit); the orchestrator records it
in KNOWN_LIMITATIONS. The corpus pins a `core/` edit instead (`core/object/message_queue.cpp`, `CallQueue::flush()`).

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
- Corpus: the two F-1 `expected_failures` of the Godot cell are removed. `edit:static-lib-main` (`main/main.cpp`, a member of `main.lib`)
  now reloads (`patched=1 hooks=57` in the corpus run of c6968bf). The second edit was `scene/main/scene_tree.cpp`; with F-1 fixed it
  gets past the sweep and is refused by the planner (CML3207 / CML3214: SceneTree's `SNAME` lambdas define several function-local
  statics with the same name, `$TSS0` and `sname`, which the planner cannot tell apart; the same family as F-4). That is a separate planner
  limitation, so the step became `edit:static-lib-core-message-queue` (`CallQueue::flush()` in `core/object/message_queue.cpp`, which
  every frame runs and which has no such statics). The scene/ refusal is reported in the PR. `corpus F-1` in `test_audit.cpp` now asserts the new rule.
  (The raylib and imgui projects carry no F-1 entry.)
