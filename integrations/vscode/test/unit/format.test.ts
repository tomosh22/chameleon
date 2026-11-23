import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import { describe, it } from "node:test";
import {
  CLANG_PATTERN,
  CL_PATTERN,
  StatusBarInput,
  formatEvent,
  formatReloadResult,
  parseDiagnosticLine,
  parseDiagnostics,
  problemsOfDiag,
  statusBarModel,
  subscriptionEvents,
} from "../../src/format";

const when = new Date(2026, 10, 23, 9, 5, 7);
const pkg = JSON.parse(fs.readFileSync(path.join(__dirname, "..", "..", "..", "package.json"), "utf8"));

describe("formatEvent", () => {
  it("formats diag with message verbatim and the action", () => {
    const text = formatEvent(
      {
        event: "diag",
        id: "CML2201",
        severity: "error",
        stage: "compile",
        pid: 12,
        message: "C:\\p\\logic.cpp(12,5): error C2065: 'x': undeclared identifier",
        action: "fix the error",
      },
      when,
    );
    assert.equal(
      text,
      "[09:05:07] error CML2201 (stage compile, pid 12)\nC:\\p\\logic.cpp(12,5): error C2065: 'x': undeclared identifier\n  what to do: fix the error",
    );
  });
  it("formats patch_applied, patch_reverted and restarted", () => {
    assert.equal(
      formatEvent({ event: "patch_applied", pid: 7, module: "a.exe", generation: 2, hooks: 3, new_functions: 0, new_globals: 1 }, when),
      "[09:05:07] patch applied: module a.exe, pid 7, generation 2, hooks 3, new functions 0, new globals 1",
    );
    assert.equal(
      formatEvent({ event: "patch_reverted", pid: 7, module: "a.exe", generation: 1 }, when),
      "[09:05:07] patch reverted: module a.exe, pid 7, back to generation 1",
    );
    assert.equal(formatEvent({ event: "restarted", old_pid: 7, new_pid: 9 }, when), "[09:05:07] restarted: process 7 replaced by 9");
  });
  it("formats exception", () => {
    const t = formatEvent(
      { event: "exception", pid: 7, tid: 8, code: "0xC0000005", module: "a.exe", function: "f", offset: 26, actions: ["continue", "leave", "ignore"] },
      when,
    );
    assert.match(t, /exception 0xC0000005 at a\.exe!f\+0x1a \(pid 7, tid 8\); the thread is held, choose: continue, leave, ignore/);
  });
  it("names an unknown event", () => {
    assert.equal(formatEvent({ event: "zzz" }, when), "[09:05:07] zzz");
  });
  it("summarises a reload result", () => {
    assert.equal(formatReloadResult({ generation: 2, patched: 1, hooks_installed: 4, timing_ms: { total: 380 } }), "generation 2, 1 patched, 4 hooks, 380 ms");
  });
});

describe("subscriptionEvents", () => {
  it("follows the capabilities", () => {
    assert.deepEqual(subscriptionEvents(new Set(["status"]), true), ["diag", "patch_applied"]);
    assert.deepEqual(subscriptionEvents(new Set(["revert", "restart", "exceptions"]), false), ["diag", "patch_applied", "patch_reverted", "restarted"]);
    assert.deepEqual(subscriptionEvents(new Set(["exceptions"]), true), ["diag", "patch_applied", "exception"]);
  });
});

describe("compiler line parsing (problem matcher)", () => {
  it("parses cl and clang-cl lines", () => {
    const d = parseDiagnosticLine("C:\\src\\logic.cpp(12): error C2065: 'y': undeclared identifier");
    assert.deepEqual(d, { file: "C:\\src\\logic.cpp", line: 12, column: undefined, severity: "error", code: "C2065", message: "'y': undeclared identifier" });
    const w = parseDiagnosticLine("logic.cpp(7,13): warning C4100: 'a': unreferenced parameter");
    assert.equal(w?.line, 7);
    assert.equal(w?.column, 13);
    assert.equal(w?.severity, "warning");
    const f = parseDiagnosticLine("src\\a.cpp(3): fatal error C1083: Cannot open include file: 'q.h'");
    assert.equal(f?.severity, "error");
    assert.equal(f?.code, "C1083");
  });
  it("parses clang lines, drive letters included", () => {
    const d = parseDiagnosticLine("C:/src/logic.cpp:12:5: error: use of undeclared identifier 'y'");
    assert.deepEqual(d, { file: "C:/src/logic.cpp", line: 12, column: 5, severity: "error", code: undefined, message: "use of undeclared identifier 'y'" });
    assert.equal(parseDiagnosticLine("logic.cpp:9: warning: unused variable")?.column, undefined);
  });
  it("ignores lines that name no file and line", () => {
    for (const l of ["", "LINK : fatal error LNK1104: cannot open file", "x.obj : error LNK2019: unresolved external symbol", "[09:05:07] error CML2201", "  what to do: fix"]) {
      assert.equal(parseDiagnosticLine(l), undefined, l);
    }
  });
  it("finds the lines in a multi-line diag message", () => {
    const all = parseDiagnostics("compile failed\r\na.cpp(1): error C1: one\nnoise\nb.cpp(2,3): warning C2: two\n");
    assert.deepEqual(
      all.map((x) => `${x.file}:${x.line}`),
      ["a.cpp:1", "b.cpp:2"],
    );
  });
  it("matches the problem matchers contributed in package.json", () => {
    const matchers = pkg.contributes.problemMatchers as { name: string; pattern: { regexp: string; file: number; line: number; message: number } }[];
    const byName = new Map(matchers.map((m) => [m.name, m]));
    assert.equal(new RegExp(byName.get("chameleon")!.pattern.regexp).source, CL_PATTERN.source);
    assert.equal(new RegExp(byName.get("chameleon-clang")!.pattern.regexp).source, CLANG_PATTERN.source);
    const cl = new RegExp(byName.get("chameleon")!.pattern.regexp).exec("logic.cpp(12): error C2065: bad");
    assert.equal(cl?.[1], "logic.cpp");
    assert.equal(cl?.[2], "12");
    const cc = new RegExp(byName.get("chameleon-clang")!.pattern.regexp).exec("logic.cpp:12:5: error: bad");
    assert.equal(cc?.[1], "logic.cpp");
    assert.equal(cc?.[2], "12");
    assert.equal(cc?.[3], "5");
  });
});

describe("problemsOfDiag (WP-35)", () => {
  const compilerLine = "C:\\p\\logic.cpp(12,15): error C2059: syntax error: ';'";
  it("uses file, line and column of a diag that has them, exactly (diag-locations path)", () => {
    // The text names another place on purpose: the fields win and the text is not parsed for the place.
    const text = "compiling 'logic.cpp' failed with exit code 2. First error: D:\\other\\x.cpp(99,1): error C2059: syntax error: ';'";
    const got = problemsOfDiag({ event: "diag", id: "CML2200", severity: "error", message: text, file: "C:\\p\\logic.cpp", line: 12, column: 15 });
    assert.deepEqual(got, [{ file: "C:\\p\\logic.cpp", line: 12, column: 15, severity: "error", code: "C2059", message: "syntax error: ';'" }]);
  });
  it("does not repeat the error: the real CML2200 text carries it twice (after the exit code and after 'First error:')", () => {
    const text =
      "compiling 'C:\\p\\logic.cpp' failed with exit code 2: C:\\p\\logic.cpp(12): error C2059: syntax error: ';' First error: C:\\p\\logic.cpp(12): error C2059: syntax error: ';'";
    const got = problemsOfDiag({ event: "diag", id: "CML2200", severity: "error", message: text, file: "C:\\p\\logic.cpp", line: 12 });
    assert.equal(got.length, 1);
    assert.equal(got[0].message, "syntax error: ';'");
    assert.equal(got[0].code, "C2059");
    // The fallback parse (no fields) gives the same wording.
    const old = problemsOfDiag({ event: "diag", id: "CML2200", severity: "error", message: text });
    assert.equal(old.length, 1);
    assert.equal(old[0].message, "syntax error: ';'");
  });
  it("has no column when the compiler gave none (cl without /diagnostics:column)", () => {
    const got = problemsOfDiag({ event: "diag", severity: "error", message: "failed", file: "C:\\p\\logic.cpp", line: 12 });
    assert.equal(got.length, 1);
    assert.equal(got[0].line, 12);
    assert.equal(got[0].column, undefined);
    assert.equal(got[0].message, "failed");
    assert.equal(got[0].code, undefined);
  });
  it("maps the severity of the event", () => {
    const at = { file: "C:\\a.cpp", line: 3, message: "m" };
    assert.equal(problemsOfDiag({ event: "diag", severity: "warning", ...at })[0].severity, "warning");
    assert.equal(problemsOfDiag({ event: "diag", severity: "info", ...at })[0].severity, "note");
    assert.equal(problemsOfDiag({ event: "diag", ...at })[0].severity, "error");
  });
  it("falls back to the regular-expression parse without file and line (older broker, or no position)", () => {
    const got = problemsOfDiag({ event: "diag", id: "CML2200", severity: "error", message: `compile failed\n${compilerLine}` });
    assert.deepEqual(got, [{ file: "C:\\p\\logic.cpp", line: 12, column: 15, severity: "error", code: "C2059", message: "syntax error: ';'" }]);
    assert.deepEqual(problemsOfDiag({ event: "diag", message: "no position here" }), []);
  });
  it("ignores unusable location fields and falls back", () => {
    const bads: Record<string, string | number>[] = [{ file: "", line: 12 }, { file: "C:\\a.cpp" }, { file: "C:\\a.cpp", line: 0 }, { file: "C:\\a.cpp", line: "12" }, { file: 5, line: 12 }];
    for (const bad of bads) {
      const got = problemsOfDiag({ event: "diag", message: "x.cpp(7): error C1: e", ...bad });
      assert.deepEqual(
        got.map((p) => `${p.file}:${p.line}`),
        ["x.cpp:7"],
        JSON.stringify(bad),
      );
    }
    assert.equal(problemsOfDiag({ event: "diag", message: "m", file: "C:\\a.cpp", line: 4, column: 0 })[0].column, undefined);
  });
});

describe("statusBarModel", () => {
  const base: StatusBarInput = {
    state: "connected",
    reloading: false,
    lastResult: undefined,
    lastFailed: false,
    generation: 0,
    processes: 1,
    missing: [],
    brokerVersion: "0.1.0",
    problem: undefined,
  };
  it("shows disconnected with the problem and a connect click", () => {
    const m = statusBarModel({ ...base, state: "disconnected", problem: "CML5990: no broker" });
    assert.match(m.text, /disconnected/);
    assert.match(m.tooltip, /CML5990/);
    assert.equal(m.command, "chameleon.connect");
  });
  it("shows connecting, reloading, ok with generation and failed", () => {
    assert.match(statusBarModel({ ...base, state: "connecting" }).text, /connecting/);
    assert.match(statusBarModel({ ...base, reloading: true }).text, /reloading/);
    const ok = statusBarModel({ ...base, lastResult: "reload ok", generation: 3 });
    assert.equal(ok.text, "$(check) Chameleon: reload ok, gen 3");
    assert.equal(ok.warning, false);
    const bad = statusBarModel({ ...base, lastResult: "reload failed CML2201", lastFailed: true });
    assert.match(bad.text, /error.*reload failed CML2201/);
    assert.equal(bad.warning, true);
  });
  it("names the capabilities a broker lacks", () => {
    assert.match(statusBarModel({ ...base, missing: ["revert"] }).tooltip, /lacks: revert/);
  });
});

describe("package.json", () => {
  it("contributes the commands, a Reload keybinding and the setting", () => {
    const ids = (pkg.contributes.commands as { command: string }[]).map((c) => c.command).sort();
    assert.deepEqual(ids, [
      "chameleon.connect",
      "chameleon.disconnect",
      "chameleon.reload",
      "chameleon.restart",
      "chameleon.revertAll",
      "chameleon.revertLast",
      "chameleon.showOutput",
    ]);
    assert.equal(pkg.contributes.keybindings[0].command, "chameleon.reload");
    assert.ok(pkg.contributes.configuration.properties["chameleon.controlPipe"]);
  });
  it("keeps modules other than extension.ts free of the vscode import", () => {
    const dir = path.join(__dirname, "..", "..", "..", "src");
    for (const f of fs.readdirSync(dir).filter((n) => n.endsWith(".ts") && n !== "extension.ts")) {
      assert.doesNotMatch(fs.readFileSync(path.join(dir, f), "utf8"), /from\s+["']vscode["']/, f);
    }
  });
});
