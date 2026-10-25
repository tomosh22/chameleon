# WP-03 spike: replace one function by hand

Throwaway proof of plan sections 4.1 to 4.8. It patches the running fixture `basic` so that its
`compute_value()` returns 2 instead of 1, without touching the fixture's binary or source tree.
Results and findings: `docs/wp/WP-03.md` and `docs/DECISIONS.md` (ADR-0020).

The spike process is the **stub agent**: fixture `basic` runs without `--agent`, and everything
the real agent will later do from inside the process (reserve address space, `LoadLibraryW`,
memory writes, thread suspension) is done from outside with `OpenProcess`, `VirtualAllocEx`,
`CreateRemoteThread(LoadLibraryW)`, `WriteProcessMemory`, `SuspendThread` and
`FlushInstructionCache`.

## The demo as a script

Prerequisites: Windows x64, a developer prompt (`cl`/`link` on `PATH`), CMake, Ninja, Python 3,
git, and `cdb.exe` from the Windows SDK "Debugging Tools". From the repository root:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=cl
cmake --build build
python spike/run_spike.py --build-dir build --config Debug --compiler msvc --cxx "<path to cl.exe>"
```

For clang-cl, configure with `-DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe"` and
pass `--compiler clang-cl --cxx "<that path>"`. The linker is read from `CMAKE_LINKER` in the CMake
cache (override with `--linker`). Use `--config RelWithDebInfo` for the other configuration.
CI runs this step in all 8 matrix cells; it uploads `spike-work/` as part of the log artifact.

`run_spike.py` does, in order (every step prints what it did, with addresses):

1. Starts `build/bin/fixture_basic.exe` with stdout captured to a file and waits for `value=1`.
2. Copies `logic.cpp`/`logic.h` to a temp dir, applies `edits/value_2.edit` with `git apply`,
   recompiles with the cell's compiler: `/c /Z7 /Gy /hotpatch` plus the fixture's configuration
   flags, to `spike-work/obj/logic.obj`.
3. `cml_spike anchor`: rewrites the object, adding `__cml_anchor_<n>` (external) and
   `__cml_sanchor_<n>` (static) at offset 0 of every section (`logic_anchored.obj`).
   `cml_spike bind`: classifies every undefined external of the object against the fixture's PDB
   and writes `binding.obj` (thunk + `__cml_slot_<name>` for functions, placeholder for data).
4. `cml_spike reserve`: finds and reserves (`MEM_RESERVE`) a 64 MB block within 1.5 GB of the
   fixture image. The script links `patch_1.dll` from `logic_anchored.obj` and `binding.obj` with
   `/DLL /NOENTRY /NODEFAULTLIB /DEBUG:FULL /INCREMENTAL:NO /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN
   /FIXED /DYNAMICBASE:NO /BASE:<reserved>` using the linker of the cell.
5. `cml_spike load`: releases the reservation, runs `LoadLibraryW` in a remote thread and checks
   that the module sits exactly at the `/FIXED` base.
6. `cml_spike retarget`: reads the patch PDB with `cml_pdb`, locates each anchor and slot, rewrites
   every REL32/ADDR64 that referenced a bind-to-old data symbol, and writes the old function
   addresses into the slots.
7. `cml_spike hook`: suspends all threads, checks that no RIP is inside the 5-byte window, writes
   `E9 rel32` at the old `compute_value`, flushes the instruction cache and resumes.
8. Asserts `value=2` within 5 s, no `value=1` afterwards, increasing `n`, process alive.
9. Runs `cdb -pv -p <pid>` (non-invasive) and asserts: `patch_1` has PDB symbols,
   `patch_1!compute_value` resolves to the hook target, `logic.cpp @ <line>` is shown, and the
   old entry is a `jmp` into the patch. The raw cdb output is printed to the log.
10. Kills the fixture.

Any failed assertion makes the script exit non-zero.

## Layout

| File | Purpose |
|---|---|
| `run_spike.py` | the driver and all assertions |
| `main.cpp` | `cml_spike` sub-commands |
| `obj_tools.*` | ad-hoc anchor rewriter and `binding.obj` emitter (WP-06 replaces them) |
| `externals.*` | classification of undefined externals, retarget plan |
| `win_remote.*` | out-of-process primitives (the stub agent) |
