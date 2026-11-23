// A fake control server on a named pipe (Windows) or a Unix socket (elsewhere), speaking the WP-13 line protocol.

import * as fs from "node:fs";
import * as net from "node:net";
import * as os from "node:os";
import * as path from "node:path";

let counter = 0;

export function testPipePath(): string {
  const name = `cml_vscode_test_${process.pid}_${Date.now()}_${counter++}`;
  return process.platform === "win32" ? `\\\\.\\pipe\\${name}` : path.join(os.tmpdir(), `${name}.sock`);
}

export type Handler = (cmd: string, args: Record<string, unknown>, conn: FakeConnection) => unknown;

export class FakeConnection {
  readonly requests: { cmd: string; args: Record<string, unknown> }[] = [];
  constructor(readonly socket: net.Socket) {}
  /** Sends raw bytes (to test partial lines). */
  raw(data: string | Buffer): void {
    this.socket.write(data);
  }
  send(obj: unknown): void {
    this.socket.write(JSON.stringify(obj) + "\n");
  }
  event(obj: Record<string, unknown>): void {
    this.send(obj);
  }
}

export class FakeServer {
  readonly connections: FakeConnection[] = [];
  private server: net.Server | undefined;

  constructor(
    readonly pipe: string,
    private readonly handler: Handler,
  ) {}

  async listen(): Promise<void> {
    if (process.platform !== "win32") {
      fs.rmSync(this.pipe, { force: true });
    }
    const server = net.createServer((socket) => {
      const conn = new FakeConnection(socket);
      this.connections.push(conn);
      let buf = "";
      socket.setEncoding("utf8");
      socket.on("data", (chunk: string) => {
        buf += chunk;
        let nl: number;
        while ((nl = buf.indexOf("\n")) >= 0) {
          const line = buf.slice(0, nl).replace(/\r$/, "");
          buf = buf.slice(nl + 1);
          if (line.trim() === "") {
            continue;
          }
          const req = JSON.parse(line) as { id: number; cmd: string; args?: Record<string, unknown> };
          conn.requests.push({ cmd: req.cmd, args: req.args ?? {} });
          const out = this.handler(req.cmd, req.args ?? {}, conn);
          if (out === undefined) {
            continue; // the handler answers itself (or never)
          }
          const diag = (out as { diag?: unknown }).diag;
          if (diag) {
            conn.send({ id: req.id, ok: false, diag });
          } else {
            conn.send({ id: req.id, ok: true, result: out });
          }
        }
      });
      socket.on("error", () => undefined);
    });
    this.server = server;
    await new Promise<void>((resolve, reject) => {
      server.once("error", reject);
      server.listen(this.pipe, () => resolve());
    });
  }

  /** Drops every connection (the broker went away) but keeps listening. */
  dropConnections(): void {
    for (const c of this.connections.splice(0)) {
      c.socket.destroy();
    }
  }

  async close(): Promise<void> {
    this.dropConnections();
    const server = this.server;
    this.server = undefined;
    if (server) {
      await new Promise<void>((resolve) => server.close(() => resolve()));
    }
    if (process.platform !== "win32") {
      fs.rmSync(this.pipe, { force: true });
    }
  }
}

export const STATUS_ALL = {
  version: "0.1.0",
  capabilities: ["status", "reload", "wait", "set-option", "subscribe", "diag-events", "revert", "restart", "exceptions"],
  processes: [{ pid: 4242, modules: [{ path: "C:\\x\\fixture_basic.exe", enabled: true }] }],
  generation: 0,
};

export function sleep(ms: number): Promise<void> {
  return new Promise((r) => setTimeout(r, ms));
}

export async function waitFor(cond: () => boolean, ms = 5000): Promise<void> {
  const end = Date.now() + ms;
  while (!cond()) {
    if (Date.now() > end) {
      throw new Error("waitFor timed out");
    }
    await sleep(10);
  }
}
