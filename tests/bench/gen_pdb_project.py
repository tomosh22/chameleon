#!/usr/bin/env python3
"""Generates a large synthetic C++ project whose PDB is used to benchmark cml_pdb.

The project is built once in CI (MSVC, /Z7 + /DEBUG:FULL) and the resulting PDB is cached by
actions/cache, keyed by the hash of this script and the parameters (see .github/workflows/ci.yml,
job `pdb-benchmark`). Everything is deterministic for a given (parameters, seed).

Each translation unit contains many distinct functions (several body shapes with locals, loops,
function-local statics), anonymous-namespace statics and UDTs, static functions, member functions,
class templates instantiated with TU-specific arguments, a dynamic initialiser, and it includes a
random subset of shared headers (inline functions and templates, which become COMDAT symbols
duplicated in every module that uses them). That gives the PDB what real projects have:
many compilands, large module symbol streams, many publics and a big file -> compilands map.

Usage:
    gen_pdb_project.py OUT_DIR [--tus 600] [--funcs-per-tu 1000] [--headers 24] [--seed 1]
                               [--debug-format Z7|Zi]

A 500 MB+ PDB is one flag away: raise --tus / --funcs-per-tu (PDB size grows linearly with both).
"""

import argparse
import json
import os
import random
import sys

SHARED_INLINES_PER_HEADER = 24
STATICS_PER_TU = 4
LOCAL_UDTS_PER_TU = 8
TEMPLATES_PER_TU = 4
HELPERS_PER_TU = 6


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


def tu_text(t, nfuncs, nheaders, rng):
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
    return "\n".join(out) + "\n"


def main_text(ntus):
    lines = ["// Generated: calls into every translation unit so that nothing is unreferenced.", "#include <cstdio>", ""]
    for t in range(ntus):
        lines.append(f"int bench_tu{t}_entry(int seed);")
    lines += ["", "int main(int argc, char**) {", "    int r = argc;"]
    for t in range(ntus):
        lines.append(f"    r += bench_tu{t}_entry(r);")
    lines += ['    std::printf("%d\\n", r);', "    return 0;", "}", ""]
    return "\n".join(lines)


def cmake_text(ntus, debug_format):
    srcs = "\n".join(f"    src/tu_{t:04d}.cpp" for t in range(ntus))
    return f"""cmake_minimum_required(VERSION 3.24)
project(PdbBenchProject CXX)
set(CMAKE_CXX_STANDARD 17)
# Flags are set explicitly (not inherited from CMAKE_CXX_FLAGS_*) so the PDB does not depend on the
# CMake version's defaults: unoptimised code with full debug info, function-level linking.
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "")
set(CMAKE_CXX_FLAGS_RELEASE "")
set(CMAKE_CXX_FLAGS_DEBUG "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
add_executable(bench_project main.cpp
{srcs})
target_include_directories(bench_project PRIVATE include)
target_compile_options(bench_project PRIVATE /{debug_format} /Od /Gy /EHsc /w)
target_link_options(bench_project PRIVATE /DEBUG:FULL /INCREMENTAL:NO /OPT:NOREF /OPT:NOICF)
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out_dir")
    ap.add_argument("--tus", type=int, default=600, help="translation units (compilands)")
    ap.add_argument("--funcs-per-tu", type=int, default=1000, help="distinct functions per translation unit")
    ap.add_argument("--headers", type=int, default=24, help="shared headers")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--debug-format", choices=["Z7", "Zi"], default="Z7")
    args = ap.parse_args()

    os.makedirs(os.path.join(args.out_dir, "src"), exist_ok=True)
    os.makedirs(os.path.join(args.out_dir, "include"), exist_ok=True)
    for h in range(args.headers):
        with open(os.path.join(args.out_dir, "include", f"bench_common_{h}.h"), "w", newline="\n") as f:
            f.write(header_text(h))
    for t in range(args.tus):
        rng = random.Random(args.seed * 1000003 + t)
        with open(os.path.join(args.out_dir, "src", f"tu_{t:04d}.cpp"), "w", newline="\n") as f:
            f.write(tu_text(t, args.funcs_per_tu, args.headers, rng))
    with open(os.path.join(args.out_dir, "main.cpp"), "w", newline="\n") as f:
        f.write(main_text(args.tus))
    with open(os.path.join(args.out_dir, "CMakeLists.txt"), "w", newline="\n") as f:
        f.write(cmake_text(args.tus, args.debug_format))
    with open(os.path.join(args.out_dir, "params.json"), "w", newline="\n") as f:
        json.dump(vars(args), f, indent=2)
    total = args.tus * args.funcs_per_tu
    print(f"generated {args.tus} translation units, {total} functions, {args.headers} headers in {args.out_dir}")


if __name__ == "__main__":
    sys.exit(main())
