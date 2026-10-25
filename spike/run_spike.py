#!/usr/bin/env python3
"""WP-03 end-to-end spike driver (throwaway). See spike/README.md.

Starts fixture `basic` (no agent), recompiles an edited copy of logic.cpp, builds anchors and the
binding object, reserves address space in the running process, links a /FIXED patch DLL, loads it
with a remote LoadLibraryW thread, retargets the data references, installs a 5-byte jump, and
asserts that the printed value changes from 1 to 2 while the process keeps running. Then runs a
non-invasive cdb.exe session against the patched process and asserts that the debugger sees the
patch module with symbols, the source file and line, and the jump.

Every assertion failure exits non-zero. Windows only.
"""

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # pragma: no cover
    pass


def say(msg):
    print(f"[run_spike] {msg}", flush=True)


class SpikeFailure(Exception):
    pass


def fail(msg):
    raise SpikeFailure(msg)


def run(cmd, log_path, timeout=300, check=True, cwd=None):
    """Runs a command, echoes its output into the CI log and into log_path."""
    say("RUN " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout, cwd=cwd)
    out = p.stdout.decode("utf-8", errors="replace")
    with open(log_path, "w", encoding="utf-8") as f:
        f.write(out)
    for line in out.splitlines():
        print("    | " + line, flush=True)
    say(f"exit code {p.returncode}")
    if check and p.returncode != 0:
        fail(f"command failed with exit code {p.returncode}: {cmd[0]}")
    return p.returncode, out


def read_cache_value(build_dir, key):
    path = os.path.join(build_dir, "CMakeCache.txt")
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith(key + ":"):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return None


def find_cdb():
    cands = []
    sdk = os.environ.get("WindowsSdkDir")
    if sdk:
        cands.append(os.path.join(sdk, "Debuggers", "x64", "cdb.exe"))
    for pf in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles"),
               r"C:\Program Files (x86)", r"C:\Program Files"):
        if pf:
            cands.append(os.path.join(pf, "Windows Kits", "10", "Debuggers", "x64", "cdb.exe"))
            cands.extend(glob.glob(os.path.join(pf, "Windows Kits", "*", "Debuggers", "x64", "cdb.exe")))
            cands.extend(glob.glob(os.path.join(pf, "Debugging Tools for Windows*", "cdb.exe")))
    w = shutil.which("cdb")
    if w:
        cands.append(w)
    for c in cands:
        if os.path.isfile(c):
            return c
    return None


TICK_RE = re.compile(r"^TICK n=(\d+) value=(-?\d+) extras=(-?\d+)\s*$")


def read_ticks(path):
    """Complete TICK lines of the fixture output as (n, value) tuples."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return []
    text = data.decode("utf-8", errors="replace")
    text = text[: text.rfind("\n") + 1] if "\n" in text else ""
    ticks = []
    for line in text.splitlines():
        m = TICK_RE.match(line)
        if m:
            ticks.append((int(m.group(1)), int(m.group(2))))
    return ticks


def wait_for(pred, timeout, step=0.05):
    end = time.time() + timeout
    while time.time() < end:
        r = pred()
        if r:
            return r
        time.sleep(step)
    return pred()


def hexints(text):
    """All hex numbers of at least 8 digits in text (cdb prints 00007ff6`12345678)."""
    out = []
    for m in re.finditer(r"\b[0-9a-fA-F]{4,8}`[0-9a-fA-F]{8}\b|\b[0-9a-fA-F]{8,16}\b", text):
        try:
            out.append(int(m.group(0).replace("`", ""), 16))
        except ValueError:
            pass
    return out


def section(text, name):
    """Text between `@@name@@` and the next `@@` marker line of the cdb output."""
    m = re.search(r"@@%s@@\s*\n(.*?)(?=\n@@[A-Z0-9-]+@@|\Z)" % re.escape(name), text, re.S)
    return m.group(1) if m else ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--config", required=True, choices=["Debug", "RelWithDebInfo"])
    ap.add_argument("--compiler", required=True, choices=["msvc", "clang-cl"])
    ap.add_argument("--cxx", help="cl.exe / clang-cl.exe (default: CMAKE_CXX_COMPILER from the CMake cache)")
    ap.add_argument("--linker", help="link.exe / lld-link.exe (default: CMAKE_LINKER from the CMake cache)")
    ap.add_argument("--work-dir", default="spike-work")
    ap.add_argument("--repo-root", default=os.getcwd())
    ap.add_argument("--reserve-size", default="0x4000000")
    args = ap.parse_args()

    repo = os.path.abspath(args.repo_root)
    build = os.path.abspath(args.build_dir)
    work = os.path.abspath(args.work_dir)
    if os.path.isdir(work):
        shutil.rmtree(work)
    for d in ("src", "obj", "patch", "logs"):
        os.makedirs(os.path.join(work, d))
    logs = os.path.join(work, "logs")
    obj_dir = os.path.join(work, "obj")
    patch_dir = os.path.join(work, "patch")

    spike = os.path.join(build, "bin", "cml_spike.exe")
    fixture = os.path.join(build, "bin", "fixture_basic.exe")
    for p in (spike, fixture, os.path.join(build, "bin", "fixture_basic.pdb")):
        if not os.path.isfile(p):
            fail(f"missing build output {p}")

    cxx = args.cxx or read_cache_value(build, "CMAKE_CXX_COMPILER")
    linker = args.linker or read_cache_value(build, "CMAKE_LINKER")
    if not linker or linker.endswith("NOTFOUND"):
        linker = shutil.which("link")
    if not cxx or not linker:
        fail(f"cannot determine compiler ({cxx}) / linker ({linker})")
    say(f"cell: compiler={args.compiler} config={args.config}")
    say(f"compiler: {cxx}")
    say(f"LINKER-USED: {linker} ({'lld-link' if 'lld' in os.path.basename(linker).lower() else 'link.exe'} family)")

    # What the fixture build really used for logic.cpp (for comparison with the hard-coded command).
    cc = os.path.join(build, "compile_commands.json")
    if os.path.isfile(cc):
        for e in json.load(open(cc, encoding="utf-8")):
            if e.get("file", "").replace("\\", "/").endswith("fixtures/basic/logic.cpp"):
                say("fixture build command for logic.cpp: " + e.get("command", str(e.get("arguments"))))

    fixture_out = os.path.join(logs, "fixture_stdout.txt")
    fout = open(fixture_out, "wb")
    proc = subprocess.Popen([fixture], stdout=fout, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                            cwd=os.path.dirname(fixture))
    try:
        run_pipeline(args, repo, build, work, logs, obj_dir, patch_dir, spike, fixture, cxx, linker,
                     fixture_out, proc)
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait()
        fout.close()
        say(f"fixture killed (exit code {proc.returncode})")


def run_pipeline(args, repo, build, work, logs, obj_dir, patch_dir, spike, fixture, cxx, linker, fixture_out, proc):
    pid = proc.pid
    say(f"STEP 1: started {fixture} pid={pid} (no --agent; the spike process is the stub agent)")
    if not wait_for(lambda: len([t for t in read_ticks(fixture_out) if t[1] == 1]) >= 3, 20):
        fail("the fixture did not print three `value=1` TICK lines within 20 s")
    say(f"SPIKE-BEFORE fixture output: {read_ticks(fixture_out)[-3:]} (n, value)")

    # ---- 2. edit a COPY of logic.cpp, recompile it -------------------------------------------
    say("STEP 2: apply edits/value_2.edit to a copy and recompile with /Z7")
    fx = os.path.join(repo, "tests", "fixtures", "basic")
    tmp = tempfile.mkdtemp(prefix="cmlspike-")
    for f in ("logic.cpp", "logic.h"):
        shutil.copy(os.path.join(fx, f), os.path.join(tmp, f))
    git = shutil.which("git")
    if not git:
        fail("git is needed to apply edits/value_2.edit")
    run([git, "apply", "--verbose", "--ignore-whitespace", os.path.join(fx, "edits", "value_2.edit")],
        os.path.join(logs, "apply_edit.txt"), cwd=tmp)
    src_dir = os.path.join(work, "src")
    for f in ("logic.cpp", "logic.h"):
        shutil.copy(os.path.join(tmp, f), os.path.join(src_dir, f))
    patched = open(os.path.join(src_dir, "logic.cpp"), encoding="utf-8").read()
    if not re.search(r"return\s+2\s*;", patched):
        fail("the edit was not applied to the copy of logic.cpp")
    say("edit applied to " + os.path.join(src_dir, "logic.cpp") + " (source tree untouched)")

    flags = ["/nologo", "/c", "/Z7", "/Gy", "/hotpatch", "/EHsc", "/GR", "/std:c++20", "/DWIN32", "/D_WINDOWS",
             "/DUNICODE", "/D_UNICODE", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN"]
    if args.config == "Debug":
        flags += ["/MTd", "/Od", "/Ob0", "/RTC1"]
    else:
        flags += ["/MT", "/O2", "/Ob0", "/DNDEBUG"]
    logic_obj = os.path.join(obj_dir, "logic.obj")
    run([cxx] + flags + [f"/Fo{logic_obj}", os.path.join(src_dir, "logic.cpp")], os.path.join(logs, "compile.txt"))
    if not os.path.isfile(logic_obj):
        fail("the compiler produced no logic.obj")

    # ---- 3. anchors + binding object --------------------------------------------------------
    say("STEP 3: anchor symbols and binding object")
    anchored = os.path.join(obj_dir, "logic_anchored.obj")
    binding = os.path.join(obj_dir, "binding.obj")
    run([spike, "anchor", logic_obj, anchored], os.path.join(logs, "anchor.txt"))
    run([spike, "bind", logic_obj, fixture, binding, os.path.join(logs, "externals.txt")],
        os.path.join(logs, "bind.txt"))

    # ---- 4. reserve in the target, link /FIXED ---------------------------------------------
    say("STEP 4: reserve address space in the target and link a /FIXED DLL there")
    reserve_file = os.path.join(logs, "reserved.txt")
    run([spike, "reserve", str(pid), "fixture_basic.exe", args.reserve_size, reserve_file],
        os.path.join(logs, "reserve.txt"))
    base = int(open(reserve_file).read().strip(), 16)
    dll = os.path.join(patch_dir, "patch_1.dll")
    pdb = os.path.join(patch_dir, "patch_1.pdb")
    link_cmd = [linker, "/nologo", "/DLL", "/NOENTRY", "/NODEFAULTLIB", "/DEBUG:FULL", "/INCREMENTAL:NO",
                "/OPT:NOREF", "/OPT:NOICF", "/FUNCTIONPADMIN", "/FIXED", "/DYNAMICBASE:NO",
                f"/BASE:0x{base:X}", f"/PDB:{pdb}", f"/OUT:{dll}", "/IGNORE:4001,4254,4281", anchored, binding]
    # First link WITHOUT /IGNORE into a scratch directory to record what the linker really prints.
    probe_dir = os.path.join(work, "probe")
    os.makedirs(probe_dir)
    probe_cmd = [c for c in link_cmd if not c.startswith("/IGNORE:")]
    probe_cmd = [f"/PDB:{os.path.join(probe_dir, 'probe.pdb')}" if c.startswith("/PDB:") else c for c in probe_cmd]
    probe_cmd = [f"/OUT:{os.path.join(probe_dir, 'probe.dll')}" if c.startswith("/OUT:") else c for c in probe_cmd]
    _, probe_out = run(probe_cmd, os.path.join(logs, "link_without_ignore.txt"))
    printed = [l for l in probe_out.splitlines() if l.strip()]
    say(f"SPIKE-LINK-NOIGNORE the linker printed {len(printed)} line(s) without /IGNORE" + (":" if printed else ""))
    for l in printed:
        say("SPIKE-LINK-NOIGNORE   " + l)
    run(link_cmd, os.path.join(logs, "link.txt"))
    if not os.path.isfile(dll) or not os.path.isfile(pdb):
        fail("the linker produced no patch_1.dll / patch_1.pdb")

    # ---- 5. load -----------------------------------------------------------------------------
    say("STEP 5: release the reservation and LoadLibraryW remotely")
    _, out = run([spike, "load", str(pid), dll, f"0x{base:X}"], os.path.join(logs, "load.txt"))
    m = re.search(r"patch module \S+ loaded at (0x[0-9a-f]+) size=(0x[0-9a-f]+)", out)
    if not m or "LOADED-AT-FIXED-BASE" not in out:
        fail("the patch DLL was not confirmed at its /FIXED base")
    patch_base, patch_size = int(m.group(1), 16), int(m.group(2), 16)

    # ---- 6. retarget, 7. hook ----------------------------------------------------------------
    say("STEP 6: retarget data references and fill the function slots")
    run([spike, "retarget", "--pid", str(pid), "--exe", fixture, "--obj", logic_obj, "--dll", dll],
        os.path.join(logs, "retarget.txt"))
    say("STEP 7: install the 5-byte jump with all threads suspended")
    with open(fixture_out, "rb") as f:
        size_pre_hook = len(f.read())
    _, out = run([spike, "hook", str(pid), fixture, dll], os.path.join(logs, "hook.txt"))
    m = re.search(r"HOOK installed: old entry (0x[0-9a-f]+) -> new entry (0x[0-9a-f]+)", out)
    if not m:
        fail("no HOOK installed line")
    old_entry, new_entry = int(m.group(1), 16), int(m.group(2), 16)
    say(f"SPIKE-HOOK old entry {m.group(1)} -> new entry {m.group(2)}")

    # ---- 8. the output must change -----------------------------------------------------------
    say("STEP 8: the printed value must change from 1 to 2 and the target must keep running")

    def has_two():
        return any(v == 2 for _, v in read_ticks(fixture_out))

    if not wait_for(has_two, 5):
        fail("no `value=2` TICK line within 5 s of the hook")
    time.sleep(1.5)
    if proc.poll() is not None:
        fail(f"the fixture exited (code {proc.returncode}) after the patch")
    ticks = read_ticks(fixture_out)
    first2 = next(i for i, t in enumerate(ticks) if t[1] == 2)
    say(f"SPIKE-TRANSITION TICK n={ticks[first2 - 1][0]} value={ticks[first2 - 1][1]}  ->  "
        f"TICK n={ticks[first2][0]} value={ticks[first2][1]}")
    if first2 == 0 or any(t[1] != 1 for t in ticks[:first2]):
        fail("unexpected values before the first value=2 line")
    if any(t[1] != 2 for t in ticks[first2:]):
        fail("a value other than 2 was printed after the transition (value=1 did not stop)")
    ns = [t[0] for t in ticks]
    if ns != list(range(ns[0], ns[0] + len(ns))):
        fail("the TICK counter is not strictly increasing by one")
    say(f"SPIKE-AFTER {len(ticks) - first2} value=2 lines since the transition, last={ticks[-1]}; "
        f"values before: {first2} x value=1; process alive pid={pid}")

    # ---- 9. debugger proof (ADR-0011) ----------------------------------------------------------
    say("STEP 9: non-invasive cdb.exe session")
    cdb = find_cdb()
    if not cdb:
        fail("cdb.exe not found (looked under WindowsSdkDir, Windows Kits\\*\\Debuggers\\x64, PATH)")
    say(f"cdb: {cdb}")
    cmds = [
        ".lines -e",
        ".echo @@RELOAD@@", ".reload /f patch_1.dll",
        ".echo @@LM@@", "lmvm patch_1",
        ".echo @@X@@", "x patch_1!compute_value*",
        ".echo @@LN@@", "ln patch_1!compute_value",
        ".echo @@LSA@@", "lsa patch_1!compute_value",
        ".echo @@U@@", "u fixture_basic!compute_value L2",
        ".echo @@UADDR@@", f"u 0x{old_entry:x} L2",
        ".echo @@END@@", "qd",
    ]
    cdb_cmd = [cdb, "-pv", "-p", str(pid), "-y", f"{patch_dir};{os.path.join(build, 'bin')}", "-c", ";".join(cmds)]
    say("cdb command line: " + " ".join(cdb_cmd))
    try:
        p = subprocess.run(cdb_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
        cdb_out = p.stdout.decode("utf-8", errors="replace")
        cdb_rc = p.returncode
    except subprocess.TimeoutExpired as e:
        cdb_out = (e.stdout or b"").decode("utf-8", errors="replace")
        cdb_rc = "timeout"
    with open(os.path.join(logs, "cdb_output.txt"), "w", encoding="utf-8") as f:
        f.write(cdb_out)
    say(f"cdb exit code {cdb_rc}; raw output follows")
    print("----- cdb output begin -----", flush=True)
    print(cdb_out, flush=True)
    print("----- cdb output end -----", flush=True)

    lm = section(cdb_out, "LM")
    mod_line = next((l for l in lm.splitlines() if re.search(r"\bpatch_1\b", l) and "(" in l), "")
    if not re.search(r"\((private )?pdb symbols\)", mod_line):
        fail(f"cdb does not show patch_1 with PDB symbols; module line: {mod_line!r}")
    say(f"SPIKE-CDB module: {mod_line.strip()}")
    xs = section(cdb_out, "X")
    xline = next((l for l in xs.splitlines() if "patch_1!" in l and "compute_value" in l), "")
    if not xline:
        fail("cdb `x patch_1!compute_value*` did not resolve the function")
    say(f"SPIKE-CDB symbol: {xline.strip()}")
    sx = hexints(xline)
    if not sx or not (patch_base <= sx[0] < patch_base + patch_size):
        fail(f"cdb resolved compute_value outside the patch module: {xline!r}")
    if sx[0] != new_entry:
        fail(f"cdb resolves patch_1!compute_value to 0x{sx[0]:x} but the hook targets 0x{new_entry:x}")
    ln_text = section(cdb_out, "LN")
    lsa_text = section(cdb_out, "LSA")
    expect_src = os.path.normcase(os.path.join(work, "src", "logic.cpp"))
    m_src = re.search(r"^(\S.*logic\.cpp)\((\d+)\)\s*$", ln_text, re.M)
    if not m_src:
        fail("cdb `ln` shows no `<path>\\logic.cpp(<line>)` for patch_1!compute_value")
    if os.path.normcase(os.path.normpath(m_src.group(1))) != os.path.normpath(expect_src):
        fail(f"cdb reports the source {m_src.group(1)!r}, expected the patched copy {expect_src!r}")
    if not re.search(r"^\s*\d+:\s+return 2;\s*$", lsa_text, re.M):
        fail("cdb `lsa` does not show `return 2;` (the patched source)")
    say(f"SPIKE-CDB source: {m_src.group(1)} line {m_src.group(2)} == the patched copy under spike-work\\src; lsa shows `return 2;`")
    for name in ("U", "UADDR"):
        jl = next((l for l in section(cdb_out, name).splitlines() if re.search(r"\bjmp\b", l)), "")
        if not jl:
            fail(f"cdb disassembly ({name}) of the old entry shows no jmp")
        m_t = re.search(r"\bjmp\s+(?:\S+\s+)?\(?([0-9a-fA-F]{8}`[0-9a-fA-F]{8})\)?\s*$", jl.strip())
        if not m_t:
            fail(f"cannot parse the jmp target from {jl!r}")
        target = int(m_t.group(1).replace("`", ""), 16)
        if target != new_entry or "patch_1!compute_value" not in jl:
            fail(f"jmp target 0x{target:x} in {jl.strip()!r} is not patch_1!compute_value at the hook target 0x{new_entry:x}")
        say(f"SPIKE-CDB jmp ({name}): {jl.strip()}  -> parsed target 0x{target:x} == new_entry 0x{new_entry:x}")

    # ---- 10. still alive ------------------------------------------------------------------------
    n_before = read_ticks(fixture_out)[-1][0]
    time.sleep(1.0)
    if proc.poll() is not None:
        fail("the fixture died after the cdb session")
    after = read_ticks(fixture_out)
    if after[-1][0] <= n_before or after[-1][1] != 2:
        fail("the fixture stopped printing value=2 after the cdb session")
    # ---- 11. exercise the thunk + slot + retargeted data reference at run time -------------------
    say("STEP 11: reset g_tick to 0 so the patched function reads the OLD g_tick and calls the old fixture_note()")

    def read_all():
        with open(fixture_out, "rb") as f:
            return f.read()

    # (a) no NOTE between the hook and the exercise: the real g_tick is large; a failed data retarget
    # (zero placeholder) would print NOTE on every tick.
    data = read_all()
    window = data[size_pre_hook:]
    notes_window = window.count(b"NOTE ")
    n_ticks_window = len(re.findall(rb"TICK n=", window))
    say(f"SPIKE-NONOTE between the hook and the exercise: {n_ticks_window} TICK line(s) printed, {notes_window} NOTE line(s)")
    if notes_window != 0 or n_ticks_window < 10:
        fail("ASSERT FAILED: expected 0 NOTE lines and >= 10 ticks between the hook and the exercise")
    mark = len(data)
    seen = False
    for attempt in range(1, 4):  # the reset can be lost to the non-atomic ++g_tick in the target
        run([spike, "exercise", str(pid), fixture], os.path.join(logs, f"exercise{attempt}.txt"))
        if wait_for(lambda: b"NOTE logic-start" in read_all()[mark:], 2.0):
            seen = True
            say(f"exercise attempt {attempt}: NOTE appeared")
            break
        say(f"exercise attempt {attempt}: no NOTE within 2 s (reset lost?), retrying")
    if not seen:
        fail("no `NOTE logic-start` after resetting g_tick: the thunk/slot/data retarget path did not work")
    time.sleep(1.5)
    if proc.poll() is not None:
        fail("the fixture died after fixture_note() was called through the thunk")
    after_text = read_all()[mark:]
    n_notes = after_text.count(b"NOTE ")
    n_logic = after_text.count(b"NOTE logic-start")
    say(f"SPIKE-ONENOTE after the reset: {n_logic} `NOTE logic-start` line(s), {n_notes} NOTE line(s) in total")
    if n_logic != 1 or n_notes != 1:
        fail("ASSERT FAILED: expected exactly one `NOTE logic-start` after the reset")
    tail = read_ticks(fixture_out)
    if tail[-1][1] != 2:
        fail("the fixture does not print value=2 after the exercise")
    say("SPIKE-THUNK `NOTE logic-start` printed once by the OLD fixture_note() called from the patch via thunk and slot; "
        f"fixture alive, last TICK {tail[-1]}")
    say(f"SPIKE-PASS value 1 -> 2, hook {old_entry:#x} -> {new_entry:#x}, patch at {patch_base:#x}, "
        f"cdb symbols + source + jmp verified, fixture alive (last {after[-1]})")


if __name__ == "__main__":
    try:
        main()
    except SpikeFailure as e:
        say(f"SPIKE-FAIL {e}")
        sys.exit(1)
