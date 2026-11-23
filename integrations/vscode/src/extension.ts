// VS Code glue. Everything testable lives in the modules next to this file, which do not import `vscode`.

import * as path from "node:path";
import * as vscode from "vscode";
import { ControlEvent, JsonObject } from "./controlClient";
import { Diag, DiagError, badAnswer, formatDiag, ids } from "./diagIds";
import { StatusBarInput, formatEvent, formatReloadResult, parseDiagnostics, stampLine, statusBarModel } from "./format";
import { PipeNameError, resolveControlPipe } from "./pipeName";
import { ProcessInfo, Session } from "./session";

const config = (): vscode.WorkspaceConfiguration => vscode.workspace.getConfiguration("chameleon");

export function activate(context: vscode.ExtensionContext): void {
  const output = vscode.window.createOutputChannel("Chameleon");
  const problems = vscode.languages.createDiagnosticCollection("chameleon");
  const bar = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 50);
  bar.name = "Chameleon";

  const session = new Session({
    resolvePipe: async () => {
      try {
        return await resolveControlPipe(config().get<string>("controlPipe", ""));
      } catch (e) {
        if (e instanceof PipeNameError) {
          throw new DiagError({
            id: ids.noBroker,
            message: e.message,
            action: "Set the 'chameleon.controlPipe' setting to the pipe the broker was started with (--control-pipe).",
          });
        }
        throw e;
      }
    },
    watchExceptions: () => config().get<boolean>("watchExceptions", false),
  });

  let reloading = false;
  let lastResult: string | undefined;
  let lastFailed = false;

  const log = (text: string): void => output.appendLine(text);
  const stamp = (text: string): string => stampLine(text, new Date());

  const refreshBar = (): void => {
    const input: StatusBarInput = {
      state: session.state,
      reloading,
      lastResult,
      lastFailed,
      generation: session.generation,
      processes: session.status?.processes.length ?? 0,
      missing: session.missingCapabilities,
      brokerVersion: session.status?.version || undefined,
      problem: session.lastProblem ? formatDiag(session.lastProblem) : undefined,
    };
    const m = statusBarModel(input);
    bar.text = m.text;
    bar.tooltip = m.tooltip;
    bar.command = m.command;
    bar.backgroundColor = m.warning ? new vscode.ThemeColor("statusBarItem.warningBackground") : undefined;
    bar.show();
  };

  const report = (d: Diag): void => {
    log(stamp(`${formatDiag(d)}`));
    void vscode.window.showWarningMessage(`Chameleon: ${formatDiag(d)}`);
  };

  /** Runs a command and reports a failure: the broker's own diagnostic, or CML5990-5993. */
  const run = async (cmd: string, args: JsonObject, what: string, timeoutMs?: number): Promise<JsonObject | undefined> => {
    try {
      return await session.call(cmd, args, timeoutMs);
    } catch (e) {
      const d = e instanceof DiagError ? e.diag : badAnswer(e instanceof Error ? e.message : String(e));
      lastResult = `${what} failed ${d.id}`;
      lastFailed = true;
      report(d);
      return undefined;
    }
  };

  // --- events -> output channel, problems, notifications ---
  session.on("event", (ev: ControlEvent) => {
    log(formatEvent(ev, new Date()));
    if (ev.event === "diag" && typeof ev.message === "string") {
      addProblems(ev.message);
    }
    if (ev.event === "exception") {
      void offerExceptionActions(ev);
    }
    refreshBar();
  });
  session.on("state", (s: string) => {
    if (s === "connected") {
      lastResult = undefined;
      lastFailed = false;
      log(stamp(`connected to the broker${session.status?.version ? ` ${session.status.version}` : ""} (capabilities: ${[...session.capabilities].join(", ")})`));
      const missing = session.missingCapabilities;
      if (missing.length > 0) {
        log(stamp(`${ids.unsupportedBroker}: this broker lacks ${missing.join(", ")}; the matching commands are disabled`));
      }
    } else if (s === "disconnected") {
      reloading = false;
    }
    refreshBar();
  });
  session.on("problem", (d: Diag) => {
    log(stamp(formatDiag(d)));
    refreshBar();
  });
  session.on("status", refreshBar);

  function addProblems(text: string): void {
    const byFile = new Map<string, vscode.Diagnostic[]>();
    for (const p of parseDiagnostics(text)) {
      const file = resolveFile(p.file);
      const line = Math.max(0, p.line - 1);
      const col = Math.max(0, (p.column ?? 1) - 1);
      const range = new vscode.Range(line, col, line, Number.MAX_SAFE_INTEGER);
      const sev =
        p.severity === "error" ? vscode.DiagnosticSeverity.Error : p.severity === "warning" ? vscode.DiagnosticSeverity.Warning : vscode.DiagnosticSeverity.Information;
      const d = new vscode.Diagnostic(range, p.message, sev);
      d.source = "chameleon";
      if (p.code) {
        d.code = p.code;
      }
      const list = byFile.get(file) ?? [];
      list.push(d);
      byFile.set(file, list);
    }
    for (const [file, list] of byFile) {
      const uri = vscode.Uri.file(file);
      problems.set(uri, [...(problems.get(uri) ?? []), ...list]);
    }
  }

  function resolveFile(file: string): string {
    if (path.isAbsolute(file)) {
      return file;
    }
    const root = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath ?? process.cwd();
    return path.join(root, file);
  }

  async function offerExceptionActions(ev: ControlEvent): Promise<void> {
    const actions = (Array.isArray(ev.actions) ? ev.actions : ["continue", "leave", "ignore"]).filter((a): a is string => typeof a === "string");
    const choice = await vscode.window.showWarningMessage(
      `Chameleon: exception ${String(ev.code)} in ${String(ev.module)}!${String(ev.function)} (pid ${String(ev.pid)}). The thread is held.`,
      ...actions,
    );
    if (choice) {
      await run("exception-action", { pid: ev.pid as number, exception_id: ev.exception_id as number, action: choice }, "exception action");
    }
  }

  // --- commands ---
  const needConnection = async (): Promise<boolean> => {
    if (session.state === "connected") {
      return true;
    }
    const d = session.lastProblem;
    void vscode.window.showWarningMessage(
      `Chameleon: not connected. ${d ? formatDiag(d) : "Start the broker, then run 'Chameleon: Connect'."}`,
    );
    return false;
  };

  const gated = (cmd: string): boolean => {
    const d = session.gate(cmd);
    if (d) {
      report(d);
      return false;
    }
    return true;
  };

  const reload = async (): Promise<void> => {
    if (!(await needConnection()) || !gated("reload") || reloading) {
      return;
    }
    reloading = true;
    problems.clear();
    refreshBar();
    log(stamp("reload requested"));
    const timeout = Math.min(3600000, Math.max(1000, Math.round(Number(config().get<number>("reloadTimeoutMs", 60000)) || 60000)));
    const result = await run("reload", { timeout_ms: timeout }, "reload", timeout + 15000);
    reloading = false;
    if (result) {
      lastResult = "reload ok";
      lastFailed = false;
      log(stamp(`reload ok: ${formatReloadResult(result)}`));
    }
    await session.refreshStatus();
    refreshBar();
  };

  const pickProcess = async (processes: ProcessInfo[]): Promise<ProcessInfo | undefined> => {
    if (processes.length <= 1) {
      return processes[0];
    }
    const items = processes.map((p) => {
      const mod = p.modules.find((m) => m.enabled) ?? p.modules[0];
      return { label: `pid ${p.pid}`, description: mod ? path.basename(mod.path.replace(/\\/g, "/")) : "", detail: mod?.path, process: p };
    });
    const picked = await vscode.window.showQuickPick(items, { placeHolder: "Process to hot-restart" });
    return picked?.process;
  };

  const restart = async (): Promise<void> => {
    if (!(await needConnection()) || !gated("restart")) {
      return;
    }
    const status = await session.refreshStatus();
    const processes = status?.processes ?? [];
    if (processes.length === 0) {
      void vscode.window.showWarningMessage("Chameleon: no process with the agent is connected to the broker, nothing to restart.");
      return;
    }
    const target = await pickProcess(processes);
    if (!target) {
      return;
    }
    log(stamp(`restart requested for pid ${target.pid}`));
    const result = await run("restart", { pid: target.pid }, "restart", 150000);
    if (result) {
      lastResult = "restart ok";
      lastFailed = false;
      log(stamp(`restart ok: pid ${String(result.old_pid)} -> ${String(result.new_pid)}`));
    }
    await session.refreshStatus();
    refreshBar();
  };

  const revert = async (scope: "last" | "all"): Promise<void> => {
    if (!(await needConnection()) || !gated("revert")) {
      return;
    }
    if (scope === "all") {
      const yes = await vscode.window.showWarningMessage("Revert every patch of every process back to the original code?", { modal: true }, "Revert All");
      if (yes !== "Revert All") {
        return;
      }
    }
    log(stamp(`revert ${scope} requested`));
    const result = await run("revert", { scope }, `revert ${scope}`, 90000);
    if (result) {
      const n = Array.isArray(result.reverted) ? result.reverted.length : 0;
      lastResult = `revert ${scope} ok`;
      lastFailed = false;
      log(stamp(`revert ${scope} ok: ${n} module(s) reverted`));
    }
    await session.refreshStatus();
    refreshBar();
  };

  const reg = (id: string, fn: () => unknown): void => {
    context.subscriptions.push(vscode.commands.registerCommand(id, fn));
  };
  reg("chameleon.reload", reload);
  reg("chameleon.restart", restart);
  reg("chameleon.revertLast", () => revert("last"));
  reg("chameleon.revertAll", () => revert("all"));
  reg("chameleon.showOutput", () => output.show(true));
  reg("chameleon.connect", async () => {
    await session.start();
    if (session.state !== "connected" && session.lastProblem) {
      void vscode.window.showWarningMessage(`Chameleon: ${formatDiag(session.lastProblem)}`);
    }
  });
  reg("chameleon.disconnect", () => {
    session.stop();
    lastResult = undefined;
    lastFailed = false;
    log(stamp("disconnected"));
    refreshBar();
  });

  context.subscriptions.push(
    output,
    problems,
    bar,
    { dispose: () => session.stop() },
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration("chameleon.controlPipe") || e.affectsConfiguration("chameleon.watchExceptions")) {
        if (session.state !== "disconnected" || config().get<boolean>("autoConnect", true)) {
          session.stop();
          void session.start();
        }
      }
    }),
  );

  refreshBar();
  if (config().get<boolean>("autoConnect", true)) {
    void session.start();
  }
}

export function deactivate(): void {
  // Disposables registered in activate() close the connections.
}
