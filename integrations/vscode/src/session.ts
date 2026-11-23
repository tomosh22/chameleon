// The extension's view of one broker: connection with reconnect, capability gate, event subscription, and the commands.
// Two connections, like cml_ui: one for commands (`status`, `reload`, ...) and one that only carries events after `subscribe`, so a
// long reload never delays event delivery. No `vscode` import.

import { EventEmitter } from "node:events";
import { ControlClient, ControlEvent, JsonObject } from "./controlClient";
import { Diag, DiagError, badAnswer, formatDiag, unsupportedBroker } from "./diagIds";
import { subscriptionEvents } from "./format";
import type { ConnectionState } from "./format";

/** Capability a command needs (ADR-0032). `status` is the one command every broker has. */
export const COMMAND_CAPABILITY: Record<string, string> = {
  status: "status",
  reload: "reload",
  restart: "restart",
  revert: "revert",
  subscribe: "subscribe",
  "exception-action": "exceptions",
};

/** Capabilities the extension uses; the status bar names the ones a broker lacks. */
export const USED_CAPABILITIES = ["reload", "restart", "revert", "subscribe", "diag-events"];

export interface ProcessInfo {
  pid: number;
  modules: { path: string; enabled: boolean }[];
}

export interface StatusInfo {
  version: string;
  capabilities: string[];
  processes: ProcessInfo[];
  generation: number;
  reloadRunning: boolean;
}

export function parseStatus(result: JsonObject): StatusInfo {
  if (!Array.isArray(result.capabilities)) {
    throw new DiagError(badAnswer("the 'status' answer has no capability list"));
  }
  const processes: ProcessInfo[] = [];
  for (const p of Array.isArray(result.processes) ? result.processes : []) {
    const po = p as JsonObject;
    if (typeof po.pid !== "number") {
      continue;
    }
    const modules: ProcessInfo["modules"] = [];
    for (const m of Array.isArray(po.modules) ? po.modules : []) {
      const mo = m as JsonObject;
      if (typeof mo.path === "string") {
        modules.push({ path: mo.path, enabled: mo.enabled === true });
      }
    }
    processes.push({ pid: po.pid, modules });
  }
  return {
    version: typeof result.version === "string" ? result.version : "",
    capabilities: result.capabilities.filter((c): c is string => typeof c === "string"),
    processes,
    generation: typeof result.generation === "number" ? result.generation : 0,
    reloadRunning: result.reload_running === true,
  };
}

export interface SessionOptions {
  /** Resolves the pipe name at every connection attempt (the setting may change). */
  resolvePipe(): Promise<string>;
  watchExceptions(): boolean;
  reconnectDelaysMs?: number[];
  connectTimeoutMs?: number;
  requestTimeoutMs?: number;
}

/**
 * Events: 'state' (ConnectionState), 'event' (ControlEvent), 'status' (StatusInfo), 'problem' (Diag, a connection failure),
 * 'connected' (StatusInfo).
 */
export class Session extends EventEmitter {
  state: ConnectionState = "disconnected";
  status: StatusInfo | undefined;
  lastProblem: Diag | undefined;
  generation = 0;

  private commands: ControlClient | undefined;
  private events: ControlClient | undefined;
  private wanted = false;
  private timer: NodeJS.Timeout | undefined;
  private attempt = 0;
  private epoch = 0;
  /** Commands the broker answered with CML5402 although it advertised them: disabled until the next connection. */
  private readonly refused = new Set<string>();

  constructor(private readonly options: SessionOptions) {
    super();
  }

  get capabilities(): ReadonlySet<string> {
    return new Set(this.status?.capabilities ?? []);
  }

  /** Capabilities this extension uses that the connected broker does not advertise. */
  get missingCapabilities(): string[] {
    if (!this.status) {
      return [];
    }
    const have = this.capabilities;
    return USED_CAPABILITIES.filter((c) => !have.has(c));
  }

  /** Starts connecting and keeps reconnecting until stop(). Resolves when the first attempt has ended (connected or not). */
  async start(): Promise<void> {
    this.wanted = true;
    if (this.state !== "disconnected") {
      return; // already connected, or an attempt is running: a second pair of connections would deliver every event twice
    }
    this.cancelTimer();
    await this.connectOnce();
  }

  /** Ends the connection and stops reconnecting. */
  stop(): void {
    this.wanted = false;
    this.cancelTimer();
    this.epoch++;
    this.teardown();
    this.setState("disconnected");
  }

  /** Sends a command on the command connection; gated on the broker's capabilities. Rejects with a DiagError. */
  async call(cmd: string, args: JsonObject = {}, timeoutMs?: number): Promise<JsonObject> {
    const client = this.commands;
    if (this.state !== "connected" || !client) {
      throw new DiagError({
        id: "CML5990",
        message: "not connected to the Chameleon broker",
        action: "Run 'Chameleon: Connect' (and start cml_broker or cml_ui if no broker is running).",
      });
    }
    const gate = this.gate(cmd);
    if (gate) {
      throw new DiagError(gate);
    }
    try {
      return await client.request(cmd, args, timeoutMs ?? this.options.requestTimeoutMs ?? 30000);
    } catch (e) {
      if (e instanceof DiagError && e.diag.id === "CML5402") {
        this.refused.add(cmd);
        this.emit("status", this.status);
        throw new DiagError(unsupportedBroker(`the '${cmd}' command`, this.status?.version));
      }
      throw e;
    }
  }

  /** undefined if `cmd` can be used, else the CML5992 diagnostic that says why not. */
  gate(cmd: string): Diag | undefined {
    const needed = COMMAND_CAPABILITY[cmd];
    if (this.refused.has(cmd) || (needed !== undefined && this.status && !this.capabilities.has(needed))) {
      return unsupportedBroker(`the '${cmd}' command (capability '${needed ?? cmd}')`, this.status?.version);
    }
    return undefined;
  }

  /** Reads `status` again; failures leave the old status in place (the connection's own errors reconnect). */
  async refreshStatus(): Promise<StatusInfo | undefined> {
    try {
      const status = parseStatus(await this.call("status", {}, 10000));
      this.applyStatus(status);
      return status;
    } catch {
      return undefined;
    }
  }

  private applyStatus(status: StatusInfo): void {
    this.status = status;
    this.generation = status.generation;
    this.emit("status", status);
  }

  private async connectOnce(): Promise<void> {
    const epoch = ++this.epoch;
    this.setState("connecting");
    let pipe = "";
    let commands: ControlClient | undefined;
    let events: ControlClient | undefined;
    // A connection that closes during the handshake is remembered here; once committed, a close means the pipe was lost.
    let committed = false;
    let early: Diag | undefined;
    const onClosed = (d: Diag): void => {
      if (committed) {
        this.onLost(epoch, d);
      } else {
        early = early ?? d;
      }
    };
    try {
      pipe = await this.options.resolvePipe();
      commands = await ControlClient.connect(pipe, this.options.connectTimeoutMs ?? 5000);
      commands.on("close", onClosed);
      const status = parseStatus(await commands.request("status", {}, 10000));
      const caps = new Set(status.capabilities);
      if (epoch !== this.epoch) {
        commands.close();
        return;
      }
      if (caps.has("subscribe")) {
        events = await ControlClient.connect(pipe, this.options.connectTimeoutMs ?? 5000);
        events.on("close", onClosed);
        events.on("event", (ev: ControlEvent) => this.onEvent(ev));
        const names = subscriptionEvents(caps, this.options.watchExceptions());
        await events.request("subscribe", { events: names }, 15000);
      }
      if (epoch !== this.epoch) {
        commands.close();
        events?.close();
        return;
      }
      if (early) {
        throw new DiagError(early);
      }
      committed = true;
      this.commands = commands;
      this.events = events;
      this.refused.clear();
      this.attempt = 0;
      this.lastProblem = undefined;
      this.status = undefined;
      this.applyStatus(status);
      this.setState("connected");
      this.emit("connected", status);
    } catch (e) {
      commands?.close();
      events?.close();
      if (epoch !== this.epoch) {
        return;
      }
      const diag = e instanceof DiagError ? e.diag : badAnswer(e instanceof Error ? e.message : String(e));
      this.lastProblem = diag;
      this.status = undefined;
      this.commands = undefined;
      this.events = undefined;
      this.setState("disconnected");
      this.emit("problem", diag);
      this.scheduleReconnect();
    }
  }

  private onLost(epoch: number, diag: Diag): void {
    if (epoch !== this.epoch) {
      return;
    }
    this.epoch++;
    this.teardown();
    this.lastProblem = diag;
    this.status = undefined;
    this.setState("disconnected");
    this.emit("problem", diag);
    this.scheduleReconnect();
  }

  private scheduleReconnect(): void {
    if (!this.wanted) {
      return;
    }
    const delays = this.options.reconnectDelaysMs ?? [500, 1000, 2000, 5000];
    const delay = delays[Math.min(this.attempt, delays.length - 1)];
    this.attempt++;
    this.cancelTimer();
    this.timer = setTimeout(() => {
      this.timer = undefined;
      if (this.wanted) {
        void this.connectOnce();
      }
    }, delay);
  }

  private cancelTimer(): void {
    if (this.timer) {
      clearTimeout(this.timer);
      this.timer = undefined;
    }
  }

  private teardown(): void {
    const c = this.commands;
    const e = this.events;
    this.commands = undefined;
    this.events = undefined;
    c?.close();
    e?.close();
  }

  private onEvent(ev: ControlEvent): void {
    if (ev.event === "patch_applied" && typeof ev.generation === "number") {
      this.generation = Math.max(this.generation, ev.generation);
    }
    if (ev.event === "patch_applied" || ev.event === "patch_reverted" || ev.event === "restarted") {
      void this.refreshStatus(); // the broker-wide generation and the process list follow the event
    }
    this.emit("event", ev);
  }

  private setState(state: ConnectionState): void {
    if (this.state !== state) {
      this.state = state;
      this.emit("state", state);
    }
  }
}

export { formatDiag };
