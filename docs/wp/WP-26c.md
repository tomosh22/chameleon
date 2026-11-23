# WP-26c: the clang-cl C-statics crash after an accepted reload (ADR-0036 decision 2)

## What the trace showed

The corpus run `raylib / clang-cl` (workflow run 42474760896, commit 752b253 of PR #46, `log_level trace`) printed the planner's
`bind:` decisions for statics when the crash was logged. Every static *function* and every named static datum of `rcore.c`
was bound to the running process. The only static datum that was **not** bound:

```
bind: sym=platform.0 display=platform.0 type=data defined=1 static=1 decision=patch-new-data source=none addr=-
      reason=static without a source-level name: treated as new
```

`platform.0` is `static PlatformData platform` of the `#include`d `rcore_desktop_glfw.c`. The patched `WindowShouldClose` read
`platform.handle` from the patch image's own zero-filled copy, passed a null `GLFWwindow*` to `glfwWindowShouldClose`, and faulted
reading `0x20` at `glfwWindowShouldClose+0x10`. The hypothesis in the brief was right about the symbol and the effect.

## Root cause

The planner took the symbol for new because it could not read a name from it, not because the static was missing from the old
image. `bind_names.cpp::base_identifier` accepts a plain identifier (clang's spelling of an internal-linkage C++ datum) or a
Microsoft-decorated name. A C translation unit gives neither:

| Shape | clang-cl (C) | cl (C) |
|---|---|---|
| file-scope static | `platform` | `platform` |
| the same name taken (LLVM's uniquifier) | `platform.0`, `platform.1` | - |
| function-local static | `next.counter` (`next.counter.1`) | `?counter@?1??next@@9@9` (decorated even in C) |
| private constant copy of a local initialiser | `__const.f.x` (read-only) | - |

`.` and `$` are not identifier characters, so `base_identifier` returned an empty string and `bridge_statics` decided "static
without a source-level name: treated as new". The PDB side was fine: its S_LDATA32 records carry the source names (`platform`,
`counter`), function-local ones inside their proc. The same hole applies to function-local statics of C under clang-cl. **cl is not affected**: its C objects use the
decorated names the planner already reads (observed in the CI plan dump of the `c_statics` fixture: `inc_state`, `main_state`
plain; `?local_cell@?1??cs_read@@9@9`, all `bind-old-data` / `compiland-static`).

Every matched static is bound; every one that fell into this hole silently became a fresh zero-initialised copy. An accepted
reload with silent state loss, or a crash when the state is a pointer.

## The fix

* `c_static_name()` (`bind_names.cpp`) reads `(base, enclosing function)` out of the C spellings above: a trailing `.<digits>` is
  the uniquifier, `a.b` is function `a`, static `b`. Anything else (`__const.*`, `.str`, `a.b.c`) yields no name.
* `bridge_statics` uses it for non-decorated data names. The group key of such a name ignores its spelling, so `platform` and
  `platform.0` cannot both be bound to the one PDB record. Ambiguity is decided by the existing passes (the RVA the unchanged
  section layout predicts); what is left is **CML3207** (refuse), as before.
* **CML3500** (new, planner range): writable static data (a section with `IMAGE_SCN_MEM_WRITE`, not a runtime table) whose name
  yields no source identity is refused with a diagnostic naming the symbol and the object, before anything is applied. It is
  deliberately narrow: an explicit list of compiler-generated shapes stays `patch-new-data` exactly as before this package
  (`is_compiler_generated_data_name`): names starting with `.` (clang's `.compoundliteral[.N]`, `.str`), `$` or `?`, `__const.*`,
  `__func__.*`, PGO/coverage (`__profc_`, `__profd_`, `__profn_`, `__covrec_`, `__llvm_`), sanitizer (`___asan_`, `__asan_`,
  `__odr_`, `__sancov`) and the `__real@`/`__xmm@` constants. Everything else that is writable and unreadable looks like a user's
  static and is refused: a wrong "new" for those is the crash this package fixes, while a refusal for a generated name would turn a
  working reload into a failing one. Read-only constants are `patch-fresh`.

## Tests

* `unit.bind` (`tests/unit/bind/test_c_statics.cpp`, synthetic COFF + PDB, no compiler): the name reader; clang-cl
  `platform.0` / `next.counter` / `next.counter.1` bound to the old records (including a file-scope record of the same name); the
  cl shapes; CML3500 for an unnameable writable static; a genuinely new static and another function's local are new.
* `unit.bind_fixtures`: the edited `c_statics.c` is planned against the real fixture PDB in every cell (golden
  `golden/fixtures/c_statics.json`); the decisions of every cell are printed (`STATIC c_statics ...`). clang 22 names the two
  structs `inc_state.0` / `main_state.0` and the local `cs_read.local_state.0`; all six statics are `bind-old-data`.
* Fixture `c_statics` + scenario `c_statics` (every matrix cell, both compilers, Debug and RelWithDebInfo): a C program with three
  zero-initialised `static struct { int* p; }`, one in an `#include`d `.c` file, one in the edited TU, one function-local, read
  through the pointer on every TICK. The edit adds 1000000 to what `cs_read()` returns. After the reload each value must be
  `1000000 + count` with `count` between 20 and 999 (regex bounds; a value >= 1000000 can only come from the patched code), and `expect_monotonic` on all three catches a restart.

## clang 22 and CML3206 (ADR-0037)

With the VS-bundled clang-cl 22.1.3, `unit.bind_fixtures` failed for `virtuals` with CML3206 ("two different fix-ups write the
same site at rva 0x100008"). Cause: the test's synthetic layout puts section `n` at `0x10000 * n` and the slot symbols from
`0x100000` up, and a clang 22 object has 16+ sections, so section 16 + 8 collided with the first slot. That is a test-layout
artefact, not a planner defect (the planner and `make_fixup_triples` were right to refuse two different writes to one site). The
slots now start at `0x4000000`; `test_triples.cpp` pins both halves (identical writes merge, different ones are refused).

## Corpus

The two raylib clang-cl F-3 `expected_failures` entries are removed (corpus #46 is on main). The corpus run is listed in the PR.

## Deferred / limitations

* A C static whose name is genuinely unreadable is now refused (CML3500) instead of silently reset. Only user-looking names are refused (see the list under CML3500); no
  case is known in the fixtures or the corpus.
