# tests/harness: cmltest

`cmltest.exe` (WP-13) runs a **scenario** against a fixture: it builds the fixture from a scratch copy, starts a
headless `cml_broker` (agent pipe + control pipe) and the fixture with the agent, then executes the scenario's
steps. The repository tree is never modified: `edit` steps patch the copy.

```
cmltest --scenario tests/harness/scenarios/basic.toml --build-dir build --generator ninja \
        --config Debug --cxx cl --junit out.xml
```

| Option | Meaning |
|---|---|
| `--scenario FILE` | scenario to run (required) |
| `--build-dir DIR` | build tree holding `bin/cml_broker.exe` and `bin/cml_agent.dll` (default `<repo>/build`) |
| `--generator ninja\|msbuild` | how the fixture is built: `-G Ninja` or `-G "Visual Studio NN YYYY" -A x64` (the version `cmake --help` marks as default; `--vs-generator` overrides) (`-T ClangCL` for clang-cl) |
| `--config Debug\|RelWithDebInfo` | fixture configuration |
| `--cxx cl\|clang-cl\|PATH` | compiler for the fixture (looked up on `PATH`, or a full path such as CI's `CML_CXX`) |
| `--junit FILE` | JUnit XML, one testcase per scenario, steps in `<system-out>` |
| `--artifacts DIR` | base directory: cmltest creates `cmltest-<scenario>-<UTC timestamp>-p<pid>` inside it (marked with `.cmltest-artifacts`) for logs, fixture stdout and dumps; the base itself is never deleted or cleaned (default base `<build-dir>/cmltest-artifacts`) |
| `--work-dir DIR` | scratch base, same rule; `--clean-work` deletes the scratch directory cmltest created after the run |
| `--require-all-capabilities` | steps skipped for missing broker capabilities fail the run (WP-12 switches CI to this) |
| `--repo-root`, `--broker`, `--agent-dir`, `--cmake`, `--work-dir`, `--vs-generator`, `--build-timeout-s` | locations and limits, see `--help` |
| `--prebuilt-fixture EXE` | debugging aid: skip configure/build and run this fixture exe |

Exit code: 0 passed or partial, 1 failed, 2 usage error. Console output ends with
`CMLTEST_RESULT scenario=... outcome=passed|partial|failed passed=N failed=N skipped=N not_run=N`.

## Scenario format (`scenario.toml`)

```toml
name = "basic"                 # [A-Za-z0-9_.-]{1,64}
fixture = "basic"              # tests/fixtures/<fixture>
timeout_s = 120                # 10..7200, default 120; bounds everything after the build
description = "free text"      # optional

[[step]]
kind = "launch"                # start broker + control pipe + fixture with the agent (once, before other runtime steps)

[[step]]
kind = "expect_stdout"         # regex must match fixture output produced since the last match / reload
regex = "TICK n=\\d+ value=1"
within_s = 10                  # 1..3600, default 10

[[step]]
kind = "expect_module_enabled" # `status` lists the fixture process with a module enabled
module = "fixture_basic.exe"   # file name or path suffix, case-insensitive
within_s = 10

[[step]]
kind = "edit"                  # apply a unified diff (-p1) to the scratch COPY of the fixture sources
patch = "edits/value_2.edit"   # relative to the fixture directory

[[step]]
kind = "reload"                # control command `reload`; needs capability "reload"
within_s = 60                  # default 60

[[step]]
kind = "expect_diag"           # a `diag` event with this id; needs capability "diag-events"
id = "CML3101"
within_s = 10
```

Unknown keys, unknown kinds, missing fields, bad values and steps that need a running process before `launch`
are rejected with `CML5411` before anything runs; a pattern that does not compile is `CML5420`.

### TOML subset

Supported: comments (`#`), bare keys (`[A-Za-z0-9_-]+`), `key = value` with basic strings (`"..."`, escapes
`\" \\ \n \t \r \b \f \uXXXX \UXXXXXXXX`), literal strings (`'...'`), decimal integers (optional sign, `_` between
digits) and booleans; tables `[name]` and arrays of tables `[[name]]` (bare names); LF or CRLF; a UTF-8 BOM.
Not supported (`CML5410`): dotted keys, quoted keys, inline tables, arrays of values, multi-line strings, floats,
dates, other integer bases, nested table paths. Limits: 1 MiB per file, 64 KiB per line, 4096 tables, 1024 keys
per table. Duplicate keys and tables are errors; every error names the line.

### Regex subset (`expect_stdout`)

Literals, `.` (not newline), classes `[abc] [a-z] [^x] [\d.]`, escapes `\d \D \w \W \s \S \b \B \t \n \r` and
escaped punctuation, groups `( )` / `(?: )` (non-capturing), alternation `|`, quantifiers `* + ? {n} {n,} {n,m}`
with lazy `?` suffix, anchors `^` `$` (line anchors). No back-references or look-around. Own engine, because the
repository builds with `_HAS_EXCEPTIONS=0`, where `std::regex` aborts on a bad pattern; matching uses an explicit
stack and a step budget, so no pattern can hang the harness.

## Capability gating (temporary, until WP-12)

Before a step that needs a broker capability (`reload` needs `reload`, `expect_diag` needs `diag-events`) cmltest
queries `status`. If the capability is missing, that step **and every later step** are reported as
`SKIPPED` with the reason `capability '<x>' not provided by broker (lands in WP-12)`, in the console and as
`<skipped message=.../>` in JUnit. A skipped step is never counted as passed. The scenario is `partial`, the run
exits 0, and the console prints `PARTIAL: N steps skipped for missing broker capabilities`.
`--require-all-capabilities` turns this into a failure.

## What runs where

- Every child (cmake, ninja/msbuild, broker, fixture) lives in a kill-on-close job object; every wait has a
  timeout. On a failure or timeout a minidump (`MiniDumpWriteDump`) is written for each live child before it is
  killed. Artifacts: `cmake-configure.log`, `cmake-build.log`, `broker.log`, `agent.log`, `fixture.stdout.txt`,
  `status.json`, `failure.txt`, `dumps/*.dmp`.
- The fixture is built standalone: `fixture_wrapper/CMakeLists.txt` wraps the copied fixture, includes
  `tests/fixtures/CmlFixture.cmake` and points it at this repository's `agent/include`; `cml_agent.dll` is copied
  beside the exe afterwards.
- The control protocol (named pipe, JSON lines) is specified in `docs/wp/WP-13.md`.

## Tests

`unit.harness` (TOML, scenario, regex, diff, report), `unit.control` (JSON, control codec, broker handler and, on
Windows, real pipes), `harness.cmltest_*` (command-line behaviour). The scenario runs themselves are CI steps.
