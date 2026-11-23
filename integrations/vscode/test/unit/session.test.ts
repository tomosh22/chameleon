import * as assert from "node:assert/strict";
import { describe, it } from "node:test";
import { ControlEvent } from "../../src/controlClient";
import { DiagError } from "../../src/diagIds";
import { Session } from "../../src/session";
import { FakeServer, STATUS_ALL, sleep, testPipePath, waitFor } from "./fakeServer";

function makeSession(pipe: string, watch = false): Session {
  return new Session({
    resolvePipe: async () => pipe,
    watchExceptions: () => watch,
    reconnectDelaysMs: [30, 60],
    connectTimeoutMs: 1000,
    requestTimeoutMs: 3000,
  });
}

function broker(pipe: string, status: Record<string, unknown> = STATUS_ALL, extra?: (cmd: string, args: Record<string, unknown>) => unknown): FakeServer {
  return new FakeServer(pipe, (cmd, args) => {
    if (cmd === "status") {
      return status;
    }
    if (cmd === "subscribe") {
      return { events: args.events };
    }
    const e = extra?.(cmd, args);
    if (e !== undefined) {
      return e;
    }
    return { diag: { id: "CML5402", message: `unknown command '${cmd}'`, action: "use status" } };
  });
}

describe("Session", () => {
  it("connects, reads status, subscribes on a second connection and delivers events", async () => {
    const pipe = testPipePath();
    const server = broker(pipe, { ...STATUS_ALL, generation: 3 }); // the broker-wide generation after the patch
    await server.listen();
    const s = makeSession(pipe);
    try {
      const events: ControlEvent[] = [];
      s.on("event", (e: ControlEvent) => events.push(e));
      await s.start();
      assert.equal(s.state, "connected");
      assert.equal(s.status?.processes[0].pid, 4242);
      assert.equal(server.connections.length, 2);
      const sub = server.connections.flatMap((c) => c.requests).find((r) => r.cmd === "subscribe");
      assert.deepEqual(sub?.args.events, ["diag", "patch_applied", "patch_reverted", "restarted"]);
      const evConn = server.connections.find((c) => c.requests.some((r) => r.cmd === "subscribe"));
      evConn?.event({ event: "patch_applied", pid: 4242, module: "fixture_basic.exe", generation: 3 });
      await waitFor(() => events.length === 1);
      await waitFor(() => s.generation === 3);
    } finally {
      s.stop();
      await server.close();
    }
  });

  it("subscribes to exception events only when asked and advertised", async () => {
    const pipe = testPipePath();
    const server = broker(pipe);
    await server.listen();
    const s = makeSession(pipe, true);
    try {
      await s.start();
      const sub = server.connections.flatMap((c) => c.requests).find((r) => r.cmd === "subscribe");
      assert.ok((sub?.args.events as string[]).includes("exception"));
    } finally {
      s.stop();
      await server.close();
    }
  });

  it("disables a command the broker does not advertise (CML5992) without sending it", async () => {
    const pipe = testPipePath();
    const old = { ...STATUS_ALL, capabilities: ["status", "reload", "wait", "set-option", "subscribe", "diag-events"] };
    const server = broker(pipe, old, (cmd) => (cmd === "reload" ? { generation: 1 } : undefined));
    await server.listen();
    const s = makeSession(pipe);
    try {
      await s.start();
      assert.deepEqual(s.missingCapabilities.sort(), ["restart", "revert"]);
      await assert.rejects(s.call("revert", { scope: "last" }), (e: unknown) => e instanceof DiagError && e.diag.id === "CML5992");
      assert.equal(server.connections.flatMap((c) => c.requests).filter((r) => r.cmd === "revert").length, 0);
      assert.equal((await s.call("reload", {})).generation, 1);
      assert.ok(s.gate("restart"));
      assert.equal(s.gate("reload"), undefined);
    } finally {
      s.stop();
      await server.close();
    }
  });

  it("maps an unexpected CML5402 to CML5992 and remembers it", async () => {
    const pipe = testPipePath();
    const server = broker(pipe); // advertises revert, but answers CML5402 to every other command
    await server.listen();
    const s = makeSession(pipe);
    try {
      await s.start();
      await assert.rejects(s.call("revert", { scope: "all" }), (e: unknown) => e instanceof DiagError && e.diag.id === "CML5992");
      assert.ok(s.gate("revert"));
    } finally {
      s.stop();
      await server.close();
    }
  });

  it("passes the broker's own diagnostic through", async () => {
    const pipe = testPipePath();
    const server = broker(pipe, STATUS_ALL, (cmd) =>
      cmd === "reload" ? { diag: { id: "CML5319", message: "no changes", action: "edit a file" } } : undefined,
    );
    await server.listen();
    const s = makeSession(pipe);
    try {
      await s.start();
      await assert.rejects(s.call("reload", {}), (e: unknown) => e instanceof DiagError && e.diag.id === "CML5319");
    } finally {
      s.stop();
      await server.close();
    }
  });

  it("reports CML5990 while no broker runs and connects once it appears", async () => {
    const pipe = testPipePath();
    const s = makeSession(pipe);
    const problems: string[] = [];
    s.on("problem", (d: { id: string }) => problems.push(d.id));
    await s.start();
    assert.equal(s.state, "disconnected");
    assert.equal(problems[0], "CML5990");
    const server = broker(pipe);
    await server.listen();
    try {
      await waitFor(() => s.state === "connected");
    } finally {
      s.stop();
      await server.close();
    }
  });

  it("reconnects after the pipe is lost (CML5991)", async () => {
    const pipe = testPipePath();
    const server = broker(pipe);
    await server.listen();
    const s = makeSession(pipe);
    try {
      const problems: string[] = [];
      s.on("problem", (d: { id: string }) => problems.push(d.id));
      await s.start();
      assert.equal(s.state, "connected");
      server.dropConnections();
      await waitFor(() => problems.includes("CML5991"));
      await waitFor(() => s.state === "connected" && server.connections.length === 2);
      assert.equal((await s.call("status")).version, "0.1.0");
    } finally {
      s.stop();
      await server.close();
    }
  });

  it("does not reconnect after stop()", async () => {
    const pipe = testPipePath();
    const server = broker(pipe);
    await server.listen();
    const s = makeSession(pipe);
    try {
      await s.start();
      s.stop();
      await sleep(200);
      assert.equal(s.state, "disconnected");
      assert.equal(server.connections.length <= 2, true);
      await assert.rejects(s.call("status"), (e: unknown) => e instanceof DiagError && e.diag.id === "CML5990");
    } finally {
      await server.close();
    }
  });

  it("rejects an answer without a capability list (CML5993)", async () => {
    const pipe = testPipePath();
    const server = broker(pipe, { version: "9" });
    await server.listen();
    const s = makeSession(pipe);
    try {
      const problems: string[] = [];
      s.on("problem", (d: { id: string }) => problems.push(d.id));
      await s.start();
      assert.equal(s.state, "disconnected");
      assert.equal(problems[0], "CML5993");
    } finally {
      s.stop();
      await server.close();
    }
  });
});
