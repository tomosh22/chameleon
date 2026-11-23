// Event-to-text formatting, compiler-line parsing and the status bar model. No `vscode` import.

import type { ControlEvent, JsonObject } from "./controlClient";

function str(v: unknown, fallback = ""): string {
  return typeof v === "string" ? v : typeof v === "number" ? String(v) : fallback;
}
function num(v: unknown): number | undefined {
  return typeof v === "number" ? v : undefined;
}

export function timestamp(d: Date): string {
  const p = (n: number): string => String(n).padStart(2, "0");
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

export function stampLine(text: string, when: Date): string {
  return `[${timestamp(when)}] ${text}`;
}

/** Event names the extension asks the broker for, by capability (ADR-0032). `log` and `progress` are left out: they are chatty. */
export function subscriptionEvents(capabilities: ReadonlySet<string>, watchExceptions: boolean): string[] {
  const events = ["diag", "patch_applied"];
  if (capabilities.has("revert")) {
    events.push("patch_reverted");
  }
  if (capabilities.has("restart")) {
    events.push("restarted");
  }
  if (watchExceptions && capabilities.has("exceptions")) {
    events.push("exception");
  }
  return events;
}

/**
 * Text for one broker event; lines are separated by "\n". The `diag` message is copied verbatim on its own line(s), so compiler
 * lines in it keep the shape the problem matchers expect.
 */
export function formatEvent(ev: ControlEvent, when: Date): string {
  const t = `[${timestamp(when)}]`;
  switch (ev.event) {
    case "diag": {
      const severity = str(ev.severity, "error");
      const where = [ev.stage ? `stage ${str(ev.stage)}` : "", ev.pid ? `pid ${str(ev.pid)}` : ""].filter(Boolean).join(", ");
      let text = `${t} ${severity} ${str(ev.id, "CML????")}${where ? ` (${where})` : ""}`;
      const message = str(ev.message);
      if (message !== "") {
        text += `\n${message.replace(/\r\n/g, "\n")}`;
      }
      const action = str(ev.action);
      if (action !== "") {
        text += `\n  what to do: ${action}`;
      }
      return text;
    }
    case "patch_applied": {
      const parts = [`module ${str(ev.module, "?")}`, `pid ${str(ev.pid, "?")}`, `generation ${str(ev.generation, "?")}`];
      for (const k of ["hooks", "new_functions", "new_globals"]) {
        if (num(ev[k]) !== undefined) {
          parts.push(`${k.replace("_", " ")} ${num(ev[k])}`);
        }
      }
      return `${t} patch applied: ${parts.join(", ")}`;
    }
    case "patch_reverted":
      return `${t} patch reverted: module ${str(ev.module, "?")}, pid ${str(ev.pid, "?")}, back to generation ${str(ev.generation, "?")}`;
    case "restarted":
      return `${t} restarted: process ${str(ev.old_pid, "?")} replaced by ${str(ev.new_pid, "?")}`;
    case "exception": {
      const place = `${str(ev.module, "?")}!${str(ev.function, "?")}${num(ev.offset) !== undefined ? `+0x${(num(ev.offset) as number).toString(16)}` : ""}`;
      const actions = Array.isArray(ev.actions) ? ev.actions.join(", ") : "";
      return `${t} exception ${str(ev.code, "?")} at ${place} (pid ${str(ev.pid, "?")}, tid ${str(ev.tid, "?")}); the thread is held${actions ? `, choose: ${actions}` : ""}`;
    }
    default:
      return `${t} ${ev.event}`;
  }
}

/** One-line summary of a successful `reload` result. */
export function formatReloadResult(result: JsonObject): string {
  const parts = [`generation ${str(result.generation, "?")}`, `${str(result.patched, "0")} patched`];
  if (num(result.hooks_installed) !== undefined) {
    parts.push(`${num(result.hooks_installed)} hooks`);
  }
  const timing = result.timing_ms as JsonObject | undefined;
  if (timing && num(timing.total) !== undefined) {
    parts.push(`${num(timing.total)} ms`);
  }
  return parts.join(", ");
}

// --- compiler lines ----------------------------------------------------------------------------------------------------

export interface ParsedDiagnostic {
  file: string;
  line: number; // 1-based
  column: number | undefined; // 1-based
  severity: "error" | "warning" | "note";
  code: string | undefined;
  message: string;
}

// The same expressions as the two problem matchers contributed in package.json (a unit test compares them).
export const CL_PATTERN = /^\s*(.+?)\((\d+)(?:,(\d+))?\)\s*:\s+(?:fatal\s+)?(error|warning|note)\s+([A-Za-z]+\d+)\s*:\s*(.*)$/;
export const CLANG_PATTERN = /^\s*((?:[A-Za-z]:)?[^:\n]+?):(\d+):(?:(\d+):)?\s+(?:fatal\s+)?(error|warning|note):\s*(.*)$/;

/** `file(line[,col]): error C2065: text` (cl, clang-cl) or `file:line[:col]: error: text` (clang). undefined for any other line. */
export function parseDiagnosticLine(line: string): ParsedDiagnostic | undefined {
  const cl = CL_PATTERN.exec(line);
  if (cl) {
    return {
      file: cl[1].trim(),
      line: Number(cl[2]),
      column: cl[3] !== undefined ? Number(cl[3]) : undefined,
      severity: cl[4] as ParsedDiagnostic["severity"],
      code: cl[5],
      message: cl[6],
    };
  }
  const cc = CLANG_PATTERN.exec(line);
  if (cc) {
    return {
      file: cc[1].trim(),
      line: Number(cc[2]),
      column: cc[3] !== undefined ? Number(cc[3]) : undefined,
      severity: cc[4] as ParsedDiagnostic["severity"],
      code: undefined,
      message: cc[5],
    };
  }
  return undefined;
}

export function parseDiagnostics(text: string): ParsedDiagnostic[] {
  const out: ParsedDiagnostic[] = [];
  for (const line of text.split(/\r?\n/)) {
    const d = parseDiagnosticLine(line);
    if (d) {
      out.push(d);
    }
  }
  return out;
}

// --- status bar --------------------------------------------------------------------------------------------------------

export type ConnectionState = "disconnected" | "connecting" | "connected";

export interface StatusBarInput {
  state: ConnectionState;
  reloading: boolean;
  /** Text after the last command, e.g. "reload ok" or "reload failed CML2201". */
  lastResult: string | undefined;
  lastFailed: boolean;
  generation: number;
  processes: number;
  missing: string[]; // capabilities the broker lacks among the ones the extension uses
  brokerVersion: string | undefined;
  problem: string | undefined; // last connection problem, with its id
}

export interface StatusBarModel {
  text: string;
  tooltip: string;
  command: string;
  warning: boolean;
}

export function statusBarModel(s: StatusBarInput): StatusBarModel {
  if (s.state === "disconnected") {
    return {
      text: "$(debug-disconnect) Chameleon: disconnected",
      tooltip: `Not connected to the Chameleon broker.${s.problem ? `\n${s.problem}` : ""}\nClick to connect.`,
      command: "chameleon.connect",
      warning: true,
    };
  }
  if (s.state === "connecting") {
    return { text: "$(sync~spin) Chameleon: connecting", tooltip: "Connecting to the Chameleon broker.", command: "chameleon.showOutput", warning: false };
  }
  if (s.reloading) {
    return { text: "$(sync~spin) Chameleon: reloading", tooltip: "A reload is running.", command: "chameleon.showOutput", warning: false };
  }
  const bits: string[] = [];
  if (s.lastResult) {
    bits.push(s.lastResult);
  } else {
    bits.push(s.processes === 0 ? "no process" : `${s.processes} process${s.processes === 1 ? "" : "es"}`);
  }
  if (s.generation > 0) {
    bits.push(`gen ${s.generation}`);
  }
  const icon = s.lastFailed ? "$(error)" : "$(check)";
  const tip = [
    `Connected to the Chameleon broker${s.brokerVersion ? ` ${s.brokerVersion}` : ""}.`,
    `Processes: ${s.processes}. Patch generation: ${s.generation}.`,
  ];
  if (s.lastResult) {
    tip.push(`Last result: ${s.lastResult}.`);
  }
  if (s.missing.length > 0) {
    tip.push(`This broker lacks: ${s.missing.join(", ")} (those commands are unavailable, CML5992).`);
  }
  tip.push("Click to reload.");
  return { text: `${icon} Chameleon: ${bits.join(", ")}`, tooltip: tip.join("\n"), command: "chameleon.reload", warning: s.lastFailed };
}
