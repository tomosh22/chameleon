#!/usr/bin/env python3
"""Generates a large synthetic C++ project whose PDB is used to benchmark cml_pdb and, since WP-39,
whose executable runs with the Chameleon agent so that a reload INSIDE a large PDB can be measured.

The project is built once in CI (MSVC, /Z7 + /DEBUG:FULL) and the resulting executable and PDB are
cached by actions/cache, keyed by the hash of everything this script writes (manifest.sha256), see
.github/workflows/ci.yml, job `pdb-benchmark`. Everything is deterministic for a given
(parameters, seed): the job generates the sources on every run, because the PDB records their paths
and the broker hashes and recompiles them.

Each translation unit contains many distinct functions (several body shapes with locals, loops,
function-local statics), anonymous-namespace statics and UDTs, static functions, member functions,
class templates instantiated with TU-specific arguments, a dynamic initialiser, and it includes a
random subset of shared headers (inline functions and templates, which become COMDAT symbols
duplicated in every module that uses them). That gives the PDB what real projects have:
many compilands, large module symbol streams, many publics and a big file -> compilands map.

WP-39, the reload target: one translation unit in the middle of the project (--reload-tu, default
tus // 2) ends with `int bench_reload_value()`, which main.cpp prints on every TICK line. The edits
edits/reload_2.edit .. reload_6.edit (unified diffs, -p1 from OUT_DIR, each on top of the previous
one) change its return value 1 -> 2 -> ... -> 6; the cmltest scenario
tests/bench/scenarios/large_pdb_reload.toml applies one, reloads, and reads the new value.
main.cpp uses the fixtures' shared loop (tests/fixtures/common/fixture_runtime.h) and the agent header
(agent/include/Chameleon_API.h); the generated CMakeLists.txt takes both from -DCML_REPO_ROOT=<repo>.
The link flags are the ones docs/BUILD_REQUIREMENTS.md asks for (`cml_broker check` passes).

Usage:
    gen_pdb_project.py OUT_DIR [--tus 900] [--funcs-per-tu 1000] [--headers 24] [--seed 1]
                               [--debug-format Z7|Zi] [--reload-tu N]
    gen_pdb_project.py --self-test     generate a tiny project twice and check the edits and the manifest

A 500 MB+ PDB is one flag away: raise --tus / --funcs-per-tu (PDB size grows linearly with both).
"""

import argparse
import hashlib
import json
import os
import random
import sys
import tempfile

SHARED_INLINES_PER_HEADER = 24
STATICS_PER_TU = 4
LOCAL_UDTS_PER_TU = 8
TEMPLATES_PER_TU = 4
HELPERS_PER_TU = 6
RELOAD_VALUES = range(1, 7)  # bench_reload_value() returns 1; the edits step it to 6 (five reloads)


def header_text(h):
    lines = [
        "#pragma once",
        f"// Shared header {h}: types, templates and inline functions used by many translation units.",
        f"namespace bench_h{h} {{",
        f"struct Vec{h} {{",
        "    double x, y, z;",
        f"    double dot(const Vec{h}& o) const {{ return x * o.x + y * o.y + z * o.z; }}",
        f"    Vec{h} scaled(double s) const {{ return Vec{h}{{x * s, y * s, z * s}}; }}",
        "    double len2() const { return dot(*this); }",
        "};",
        "template <typename T, int N>",
        f"struct Ring{h} {{",
        "    T data[N];",
        "    int head;",
        "    T push(T v) { data[head] = v; head = (head + 1) % N; return data[(head + N / 2) % N]; }",
        "    T sum() const { T s = T(); for (int i = 0; i < N; ++i) s += data[i]; return s; }",
        "};",
        f"template <typename T> T clamp_{h}(T v, T lo, T hi) {{ return v < lo ? lo : (v > hi ? hi : v); }}",
    ]
    for j in range(SHARED_INLINES_PER_HEADER):
        lines.append(
            f"inline int shared_inline_{h}_{j}(int v) {{ return v * {j + 3} + (v >> {1 + j % 5}) - {j}; }}"
        )
    lines.append(f"}}  // namespace bench_h{h}")
    return "\n".join(lines) + "\n"


def reload_tail(value):
    """The last lines of the reload-target TU. Three non-blank context lines precede the edited one."""
    return [
        "",
        "// WP-39: the reload target. edits/reload_N.edit change the returned value and nothing else;",
        "// main.cpp prints it on every TICK line.",
        "int bench_reload_value() {",
        f"    return {value};",
        "}",
    ]


def tu_lines(t, nfuncs, nheaders, rng):
    hs = sorted(rng.sample(range(nheaders), min(4, nheaders)))
    out = []
    w = out.append
    for h in hs:
        w(f'#include "bench_common_{h}.h"')
    w("")
    w("namespace {")
    for k in range(LOCAL_UDTS_PER_TU):
        w(f"struct Local_{t}_{k} {{ int a, b, c; int mix(int i) const {{ return a * i + b - c + {k}; }} }};")
    for k in range(STATICS_PER_TU):
        w(f"int s_static_{t}_{k} = {t * 100 + k};")
    for k in range(TEMPLATES_PER_TU):
        w(f"template <typename T> struct Box_{t}_{k} {{ T v; T get() const {{ return v + T({k + 1}); }} }};")
    w("}  // namespace")
    w("")
    for k in range(HELPERS_PER_TU):
        w(f"static int tu_helper_{t}_{k}(int v) {{ return (v ^ {rng.randrange(1, 1 << 20)}) + {k}; }}")
    w("")
    w(f"struct TuType_{t} {{")
    w("    int v;")
    w(f"    explicit TuType_{t}(int x) : v(x) {{}}")
    w(f"    int twice() const {{ return v * 2 + {t % 97}; }}")
    w("    int offset(int o) const { return v + o; }")
    w("};")
    w("")
    w(f"static int s_dynamic_{t} = tu_helper_{t}_0({t});")
    w("")

    for i in range(nfuncs):
        h = hs[rng.randrange(len(hs))]
        j = rng.randrange(SHARED_INLINES_PER_HEADER)
        k = rng.randrange(LOCAL_UDTS_PER_TU)
        k2 = rng.randrange(HELPERS_PER_TU)
        k3 = rng.randrange(STATICS_PER_TU)
        kb = rng.randrange(TEMPLATES_PER_TU)
        n = rng.choice([4, 6, 8, 12, 16])
        m = rng.randrange(2, 9)
        big = rng.randrange(100, 100000)
        shape = rng.randrange(4)
        w(f"int bench_tu{t}_fn{i}(int a, int b) {{")
        if shape == 0:
            w(f"    Local_{t}_{k} loc{{a, b, a ^ b}};")
            w(f"    bench_h{h}::Ring{h}<long long, {n}> ring{{}};")
            w("    long long acc = 0;")
            w(f"    for (int i = 0; i < {m}; ++i) {{")
            w("        acc += loc.mix(i) + ring.push(acc ^ i);")
            w(f"        if (acc > {big}) {{ acc -= loc.c; }}")
            w("    }")
            w("    static int calls = 0;")
            w("    ++calls;")
            w(
                f"    return static_cast<int>(acc) + bench_h{h}::shared_inline_{h}_{j}(calls)"
                f" + tu_helper_{t}_{k2}(a) + s_static_{t}_{k3};"
            )
        elif shape == 1:
            w(f"    bench_h{h}::Vec{h} v{{double(a), double(b), {rng.randrange(1, 50)}.0}};")
            w(f"    Box_{t}_{kb}<double> box{{v.len2()}};")
            w("    double d = box.get() + v.scaled(0.5).dot(v);")
            w(f"    int r = bench_h{h}::clamp_{h}(static_cast<int>(d), -{big}, {big});")
            w("    for (int i = a; i < b; ++i) {")
            w(f"        r += bench_h{h}::shared_inline_{h}_{j}(i) ^ tu_helper_{t}_{k2}(i);")
            w("    }")
            w("    return r;")
        elif shape == 2:
            w("    int r = a;")
            w("    switch (b & 7) {")
            for c in range(7):
                w(f"    case {c}: r {'+=' if c % 2 == 0 else '-='} {rng.randrange(1, 1000)}; break;")
            w(f"    default: r ^= {rng.randrange(1, 1000)}; break;")
            w("    }")
            w(f"    TuType_{t} obj(r);")
            w(f"    return obj.twice() + obj.offset({rng.randrange(1, 100)}) + bench_h{h}::shared_inline_{h}_{j}(r);")
        else:
            w(f"    int tab[{n}];")
            w(f"    for (int i = 0; i < {n}; ++i) tab[i] = a * i + b;")
            w("    int s = 0;")
            w(f"    for (int i = 0; i < {n}; ++i) for (int q = 0; q < i; ++q) s += tab[i] ^ tab[q];")
            w(f"    Box_{t}_{kb}<int> box{{s}};")
            w(f"    return box.get() + bench_h{h}::clamp_{h}(s, 0, {big}) + tu_helper_{t}_{k2}(s);")
        w("}")
        w("")
    w(f"int bench_tu{t}_entry(int seed) {{")
    w(f"    int r = s_dynamic_{t} + seed;")
    for i in sorted(rng.sample(range(nfuncs), min(6, nfuncs))):
        w(f"    r += bench_tu{t}_fn{i}(r, seed + {i});")
    w("    return r;")
    w("}")
    return out


def tu_text(t, nfuncs, nheaders, rng):
    return "\n".join(tu_lines(t, nfuncs, nheaders, rng)) + "\n"


def reload_edit(old_lines, new_lines, name):
    """One hunk: the `return N;` line of bench_reload_value() with three context lines before and one after."""
    i = len(old_lines) - 2
    assert old_lines[i].startswith("    return ") and old_lines[:i] == new_lines[:i] and old_lines[i + 1:] == new_lines[i + 1:]
    start = i - 3
    assert all(line.strip() for line in old_lines[start:i])
    out = [f"--- a/{name}\n", f"+++ b/{name}\n", f"@@ -{start + 1},5 +{start + 1},5 @@\n"]
    out += [" " + line + "\n" for line in old_lines[start:i]]
    out += ["-" + old_lines[i] + "\n", "+" + new_lines[i] + "\n", " " + old_lines[i + 1] + "\n"]
    return "".join(out)


def main_text(ntus):
    lines = [
        "// Generated. A program that runs with the Chameleon agent (WP-39): `bench_project --agent` enables this module and",
        "// prints one TICK line per 100 ms; `bench_project --ticks N` runs without the agent and stops after N lines.",
        "// The entry points of every translation unit are referenced from a table so that nothing is unreferenced; they are",
        "// not called (their loop bounds depend on their arguments, so a call with arbitrary values has no bounded run time).",
        "#include <cstdio>",
        "",
        '#include "fixture_runtime.h"',
        "",
        "int bench_reload_value();",
    ]
    for t in range(ntus):
        lines.append(f"int bench_tu{t}_entry(int seed);")
    lines += ["", "typedef int (*BenchEntry)(int);", "static const BenchEntry k_bench_entries[] = {"]
    for t in range(ntus):
        lines.append(f"    bench_tu{t}_entry,")
    lines += [
        "};",
        "",
        "int main(int argc, char** argv) {",
        "    return cml_fixture::run(argc, argv, [](int n) {",
        "        const int tus = static_cast<int>(sizeof(k_bench_entries) / sizeof(k_bench_entries[0]));",
        '        std::printf("TICK n=%d value=%d tus=%d\\n", n, bench_reload_value(), k_bench_entries[n % tus] != nullptr ? tus : 0);',
        "    });",
        "}",
        "",
    ]
    return "\n".join(lines)


def cmake_text(ntus, debug_format):
    srcs = "\n".join(f"    src/tu_{t:04d}.cpp" for t in range(ntus))
    return f"""cmake_minimum_required(VERSION 3.24)
project(PdbBenchProject CXX)
set(CMAKE_CXX_STANDARD 17)
# main.cpp includes the agent header and the fixtures' shared TICK loop of the Chameleon repository (WP-39).
if(NOT CML_REPO_ROOT OR NOT EXISTS "${{CML_REPO_ROOT}}/agent/include/Chameleon_API.h"
        OR NOT EXISTS "${{CML_REPO_ROOT}}/tests/fixtures/common/fixture_runtime.h")
    message(FATAL_ERROR "pass -DCML_REPO_ROOT=<the Chameleon repository> (agent/include/Chameleon_API.h, tests/fixtures/common/fixture_runtime.h)")
endif()
# Flags are set explicitly (not inherited from CMAKE_CXX_FLAGS_*) so the PDB does not depend on the
# CMake version's defaults: unoptimised code with full debug info, function-level linking.
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "")
set(CMAKE_CXX_FLAGS_RELEASE "")
set(CMAKE_CXX_FLAGS_DEBUG "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
add_executable(bench_project main.cpp
{srcs})
target_include_directories(bench_project PRIVATE include)
set_source_files_properties(main.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${{CML_REPO_ROOT}}/agent/include;${{CML_REPO_ROOT}}/tests/fixtures/common"
    COMPILE_DEFINITIONS "UNICODE;_UNICODE;NOMINMAX;WIN32_LEAN_AND_MEAN")
target_compile_options(bench_project PRIVATE /{debug_format} /Od /Gy /EHsc /w)
# docs/BUILD_REQUIREMENTS.md: full PDB, no dead stripping, no COMDAT folding, padded function entries.
target_link_options(bench_project PRIVATE /DEBUG:FULL /INCREMENTAL:NO /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN)
"""


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write(text)


def generate(out_dir, tus, funcs_per_tu, headers, seed, debug_format, reload_tu):
    """Writes the project and returns the manifest hash (sha256 over the relative path and content of every file)."""
    if not 0 <= reload_tu < tus:
        raise SystemExit(f"--reload-tu {reload_tu} is not a translation unit (0..{tus - 1})")
    values = list(RELOAD_VALUES)
    digest = hashlib.sha256()

    def emit(name, text):  # in a fixed order: the manifest depends on it
        data = text.encode("utf-8")
        digest.update(name.encode("utf-8") + b"\0" + str(len(data)).encode("ascii") + b"\0" + data)
        write(os.path.join(out_dir, *name.split("/")), text)

    for h in range(headers):
        emit(f"include/bench_common_{h}.h", header_text(h))
    for t in range(tus):
        rng = random.Random(seed * 1000003 + t)
        name = f"src/tu_{t:04d}.cpp"
        if t != reload_tu:
            emit(name, tu_text(t, funcs_per_tu, headers, rng))
            continue
        body = tu_lines(t, funcs_per_tu, headers, rng)
        versions = {v: body + reload_tail(v) for v in values}
        emit(name, "\n".join(versions[values[0]]) + "\n")
        for v in values[1:]:
            emit(f"edits/reload_{v}.edit", reload_edit(versions[v - 1], versions[v], name))
    emit("main.cpp", main_text(tus))
    emit("CMakeLists.txt", cmake_text(tus, debug_format))
    manifest = digest.hexdigest()
    write(os.path.join(out_dir, "manifest.sha256"), manifest + "\n")
    return manifest


def self_test():
    """A tiny project, generated twice: the manifest is stable and every edit applies exactly where its hunk says."""
    values = list(RELOAD_VALUES)
    with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
        first = generate(a, 3, 5, 4, 1, "Z7", 1)
        assert generate(b, 3, 5, 4, 1, "Z7", 1) == first, "the generator is not deterministic"
        assert generate(b, 3, 5, 4, 2, "Z7", 1) != first, "the manifest does not depend on the content"
        with open(os.path.join(a, "src", "tu_0001.cpp")) as f:
            lines = f.read().split("\n")
        for v in values[1:]:
            with open(os.path.join(a, "edits", f"reload_{v}.edit")) as f:
                edit = f.read().split("\n")
            assert edit[0] == "--- a/src/tu_0001.cpp" and edit[1] == "+++ b/src/tu_0001.cpp", edit[:2]
            start = int(edit[2].split()[1].split(",")[0][1:]) - 1
            body = edit[3:-1]
            old = [line[1:] for line in body if line[0] in " -"]
            new = [line[1:] for line in body if line[0] in " +"]
            assert lines[start:start + len(old)] == old, f"reload_{v}.edit does not match the file at line {start + 1}"
            assert f"    return {v};" in new and f"    return {v - 1};" in old
            lines[start:start + len(old)] = new
        assert lines[-3:] == [f"    return {values[-1]};", "}", ""], lines[-3:]
        with open(os.path.join(a, "main.cpp")) as f:
            text = f.read()
        assert "bench_reload_value()" in text and "bench_tu2_entry," in text and "fixture_runtime.h" in text
        with open(os.path.join(a, "src", "tu_0000.cpp")) as f:
            assert "bench_reload_value" not in f.read()
    print(f"gen_pdb_project self-test ok: {len(values) - 1} edits apply in order, manifest {first[:16]} is stable")


def main():
    if sys.argv[1:] == ["--self-test"]:
        self_test()
        return 0
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out_dir")
    ap.add_argument("--tus", type=int, default=900, help="translation units (compilands)")
    ap.add_argument("--funcs-per-tu", type=int, default=1000, help="distinct functions per translation unit")
    ap.add_argument("--headers", type=int, default=24, help="shared headers")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--debug-format", choices=["Z7", "Zi"], default="Z7")
    ap.add_argument("--reload-tu", type=int, default=None,
                    help="the translation unit that defines bench_reload_value() (default: tus // 2)")
    args = ap.parse_args()
    reload_tu = args.tus // 2 if args.reload_tu is None else args.reload_tu

    manifest = generate(args.out_dir, args.tus, args.funcs_per_tu, args.headers, args.seed, args.debug_format, reload_tu)
    params = dict(vars(args), reload_tu=reload_tu, manifest=manifest)
    with open(os.path.join(args.out_dir, "params.json"), "w", newline="\n") as f:
        json.dump(params, f, indent=2)
    total = args.tus * args.funcs_per_tu
    print(f"generated {args.tus} translation units, {total} functions, {args.headers} headers in {args.out_dir}")
    print(f"reload target: src/tu_{reload_tu:04d}.cpp (bench_reload_value), {len(RELOAD_VALUES) - 1} edits in edits/")
    print(f"manifest sha256: {manifest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
