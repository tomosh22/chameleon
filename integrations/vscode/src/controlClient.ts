// Client of the broker's control pipe (ADR-0021 framing, docs/wp/WP-13.md): UTF-8 JSON objects, one per line.
//   request  {"id":N,"cmd":"...","args":{...}}
//   response {"id":N,"ok":true,"result":{...}}  |  {"id":N,"ok":false,"diag":{"id","message","action"}}
//   event    {"event":"...", ...}                unsolicited, may appear between any two lines
// One request at a time per connection (requests are queued here); events are emitted as they arrive. No `vscode` import.

import { EventEmitter } from "node:events";
import * as net from "node:net";
import { StringDecoder } from "node:string_decoder";
import { DiagError, Diag, badAnswer, noBroker, pipeLost } from "./diagIds";

export const MAX_LINE_BYTES = 1 << 20;

export type Json = null | boolean | number | string | Json[] | { [k: string]: Json };
export type JsonObject = { [k: string]: Json };

export interface ControlEvent extends JsonObject {
  event: string;
}

/** Splits a byte stream into lines (`\n`, a trailing `\r` tolerated, blank lines dropped); a line may arrive in any number of chunks. */
export class LineSplitter {
  private readonly decoder = new StringDecoder("utf8");
  private pending = "";
  private pendingBytes = 0;

  /** Feeds a chunk; returns the complete lines. Throws RangeError when one line exceeds MAX_LINE_BYTES. */
  push(chunk: Buffer): string[] {
    const text = this.decoder.write(chunk);
    const out: string[] = [];
    let start = 0;
    for (;;) {
      const nl = text.indexOf("\n", start);
      if (nl < 0) {
        break;
      }
      const piece = text.slice(start, nl);
      let line = this.pending + piece;
      this.pending = "";
      this.pendingBytes = 0;
      if (line.endsWith("\r")) {
        line = line.slice(0, -1);
      }
      if (Buffer.byteLength(line, "utf8") > MAX_LINE_BYTES) {
        throw new RangeError("control line over 1 MiB");
      }
      if (line.trim() !== "") {
        out.push(line);
      }
      start = nl + 1;
    }
    const rest = text.slice(start);
    this.pending += rest;
    this.pendingBytes += Buffer.byteLength(rest, "utf8");
    if (this.pendingBytes > MAX_LINE_BYTES) {
      throw new RangeError("control line over 1 MiB");
    }
    return out;
  }
}

interface Pending {
  id: number;
  line: string;
  cmd: string;
  timeoutMs: number;
  resolve(result: JsonObject): void;
  reject(err: Error): void;
  timer?: NodeJS.Timeout;
}

/**
 * Events: 'event' (ControlEvent), 'close' (Diag | undefined; once, when the connection ends for any reason but close()).
 */
export class ControlClient extends EventEmitter {
  private readonly splitter = new LineSplitter();
  private readonly queue: Pending[] = [];
  private current: Pending | undefined;
  private nextId = 1;
  private closed = false;

  private constructor(
    private readonly socket: net.Socket,
    readonly pipe: string,
  ) {
    super();
    socket.on("data", (chunk: Buffer) => this.onData(chunk));
    socket.on("error", (err: Error) => this.fail(pipeLost(pipe, err.message)));
    socket.on("close", () => this.fail(pipeLost(pipe, "the broker closed the connection")));
  }

  /** Connects; rejects with a DiagError CML5990 when nobody answers on `pipe`. */
  static connect(pipe: string, timeoutMs = 5000): Promise<ControlClient> {
    return new Promise((resolve, reject) => {
      const socket = net.connect(pipe);
      let settled = false;
      const timer = setTimeout(() => {
        if (!settled) {
          settled = true;
          socket.destroy();
          reject(new DiagError(noBroker(pipe, `no connection within ${timeoutMs} ms`)));
        }
      }, timeoutMs);
      socket.once("connect", () => {
        if (settled) {
          return;
        }
        settled = true;
        clearTimeout(timer);
        socket.removeAllListeners("error");
        resolve(new ControlClient(socket, pipe));
      });
      socket.once("error", (err: NodeJS.ErrnoException) => {
        if (settled) {
          return;
        }
        settled = true;
        clearTimeout(timer);
        socket.destroy();
        const why = err.code === "ENOENT" ? "the pipe does not exist (no broker is serving it)" : err.message;
        reject(new DiagError(noBroker(pipe, why)));
      });
    });
  }

  get isOpen(): boolean {
    return !this.closed;
  }

  /** Sends a command; resolves with `result`; rejects with a DiagError (the broker's diag, or CML5991/CML5993). */
  request(cmd: string, args: JsonObject = {}, timeoutMs = 30000): Promise<JsonObject> {
    if (this.closed) {
      return Promise.reject(new DiagError(pipeLost(this.pipe, "the connection is closed")));
    }
    return new Promise((resolve, reject) => {
      const id = this.nextId++;
      const line = JSON.stringify({ id, cmd, args });
      if (Buffer.byteLength(line, "utf8") > MAX_LINE_BYTES) {
        reject(new DiagError(badAnswer("the request is over 1 MiB")));
        return;
      }
      this.queue.push({ id, line, cmd, timeoutMs, resolve, reject });
      this.pump();
    });
  }

  /** Ends the connection without a 'close' event. Pending requests fail with CML5991. */
  close(): void {
    if (this.closed) {
      return;
    }
    this.closed = true;
    this.socket.destroy();
    this.rejectAll(new DiagError(pipeLost(this.pipe, "the connection was closed by the client")));
  }

  private pump(): void {
    if (this.current || this.closed) {
      return;
    }
    const next = this.queue.shift();
    if (!next) {
      return;
    }
    this.current = next;
    next.timer = setTimeout(() => {
      if (this.current === next) {
        // The answer may still arrive later and would be taken for the next request's: the connection is not usable any more.
        this.current = undefined;
        next.reject(new DiagError(badAnswer(`no answer to '${next.cmd}' within ${next.timeoutMs} ms`)));
        this.fail(badAnswer(`no answer to '${next.cmd}' within ${next.timeoutMs} ms`));
      }
    }, next.timeoutMs);
    this.socket.write(next.line + "\n", (err) => {
      if (err) {
        this.fail(pipeLost(this.pipe, err.message));
      }
    });
  }

  private onData(chunk: Buffer): void {
    let lines: string[];
    try {
      lines = this.splitter.push(chunk);
    } catch {
      this.fail(badAnswer("a line from the broker is over 1 MiB"));
      return;
    }
    for (const line of lines) {
      this.onLine(line);
    }
  }

  private onLine(line: string): void {
    let msg: unknown;
    try {
      msg = JSON.parse(line);
    } catch {
      this.fail(badAnswer("a line from the broker is not valid JSON"));
      return;
    }
    if (typeof msg !== "object" || msg === null || Array.isArray(msg)) {
      this.fail(badAnswer("a line from the broker is not a JSON object"));
      return;
    }
    const obj = msg as JsonObject;
    if (typeof obj.event === "string") {
      this.emit("event", obj as ControlEvent);
      return;
    }
    const cur = this.current;
    if (!cur || typeof obj.ok !== "boolean") {
      return; // an answer nobody waits for: ignore (forward compatible)
    }
    if (typeof obj.id === "number" && obj.id !== cur.id) {
      return; // a late answer to a request that was already given up
    }
    this.current = undefined;
    if (cur.timer) {
      clearTimeout(cur.timer);
    }
    if (obj.ok) {
      const result = obj.result;
      cur.resolve(typeof result === "object" && result !== null && !Array.isArray(result) ? (result as JsonObject) : {});
    } else {
      const d = obj.diag as JsonObject | undefined;
      const diag: Diag = {
        id: typeof d?.id === "string" ? d.id : "CML5407",
        message: typeof d?.message === "string" ? d.message : `'${cur.cmd}' failed`,
        action: typeof d?.action === "string" ? d.action : "",
      };
      cur.reject(new DiagError(diag));
    }
    this.pump();
  }

  private rejectAll(err: Error): void {
    const all = [...(this.current ? [this.current] : []), ...this.queue.splice(0)];
    if (this.current?.timer) {
      clearTimeout(this.current.timer);
    }
    this.current = undefined;
    for (const p of all) {
      p.reject(err);
    }
  }

  private fail(diag: Diag): void {
    if (this.closed) {
      return;
    }
    this.closed = true;
    this.socket.destroy();
    this.rejectAll(new DiagError(diag));
    this.emit("close", diag);
  }
}
