import * as assert from "node:assert/strict";
import { describe, it } from "node:test";
import { ControlClient, ControlEvent, LineSplitter, MAX_LINE_BYTES } from "../../src/controlClient";
import { DiagError } from "../../src/diagIds";
import { FakeServer, STATUS_ALL, sleep, testPipePath, waitFor } from "./fakeServer";

describe("LineSplitter", () => {
  it("joins a line that arrives in pieces", () => {
    const s = new LineSplitter();
    assert.deepEqual(s.push(Buffer.from('{"a":')), []);
    assert.deepEqual(s.push(Buffer.from("1}\n{")), ['{"a":1}']);
    assert.deepEqual(s.push(Buffer.from('"b":2}\n')), ['{"b":2}']);
  });
  it("tolerates CRLF and ignores blank lines", () => {
    const s = new LineSplitter();
    assert.deepEqual(s.push(Buffer.from('{"a":1}\r\n\r\n\n{"b":2}\n')), ['{"a":1}', '{"b":2}']);
  });
  it("keeps a multi-byte character that is split between chunks", () => {
    const s = new LineSplitter();
    const bytes = Buffer.from('{"m":"h\u00e9llo \u20ac"}\n', "utf8");
    const out: string[] = [];
    for (const b of bytes) {
      out.push(...s.push(Buffer.from([b])));
    }
    assert.deepEqual(out, ['{"m":"h\u00e9llo \u20ac"}']);
  });
  it("rejects a line over 1 MiB", () => {
    const s = new LineSplitter();
    assert.throws(() => s.push(Buffer.alloc(MAX_LINE_BYTES + 10, 0x61)), RangeError);
  });
});

describe("ControlClient", () => {
  it("answers a request and passes the result", async () => {
    const server = new FakeServer(testPipePath(), (cmd) => (cmd === "status" ? STATUS_ALL : undefined));
    await server.listen();
    try {
      const c = await ControlClient.connect(server.pipe);
      const r = await c.request("status");
      assert.equal(r.version, "0.1.0");
      c.close();
    } finally {
      await server.close();
    }
  });

  it("copes with an answer that arrives one byte at a time, and an event between request and answer", async () => {
    const server = new FakeServer(testPipePath(), (cmd, _args, conn) => {
      if (cmd === "reload") {
        conn.event({ event: "patch_applied", pid: 1, module: "m.exe", generation: 1 });
        const text = JSON.stringify({ id: 1, ok: true, result: { generation: 1 } }) + "\n";
        void (async () => {
          for (const ch of text) {
            conn.raw(ch);
            await sleep(1);
          }
        })();
      }
      return undefined;
    });
    await server.listen();
    try {
      const c = await ControlClient.connect(server.pipe);
      const events: ControlEvent[] = [];
      c.on("event", (e: ControlEvent) => events.push(e));
      const r = await c.request("reload", {}, 5000);
      assert.equal(r.generation, 1);
      assert.equal(events.length, 1);
      assert.equal(events[0].event, "patch_applied");
      c.close();
    } finally {
      await server.close();
    }
  });

  it("serialises requests, answered in order", async () => {
    const server = new FakeServer(testPipePath(), (cmd, args) => ({ echo: cmd, n: args.n }));
    await server.listen();
    try {
      const c = await ControlClient.connect(server.pipe);
      const all = await Promise.all([1, 2, 3].map((n) => c.request("x", { n })));
      assert.deepEqual(
        all.map((r) => r.n),
        [1, 2, 3],
      );
      c.close();
    } finally {
      await server.close();
    }
  });

  it("keeps file, line and column of an error response's diag (WP-35) and none when the broker sent none", async () => {
    let n = 0;
    const server = new FakeServer(testPipePath(), () =>
      n++ === 0
        ? { diag: { id: "CML2200", message: "compile failed", action: "fix", file: "C:\\p\\logic.cpp", line: 12, column: 15 } }
        : { diag: { id: "CML2200", message: "compile failed", action: "fix" } },
    );
    await server.listen();
    try {
      const c = await ControlClient.connect(server.pipe);
      await assert.rejects(c.request("reload"), (e: unknown) => {
        assert.ok(e instanceof DiagError);
        assert.equal(e.diag.file, "C:\\p\\logic.cpp");
        assert.equal(e.diag.line, 12);
        assert.equal(e.diag.column, 15);
        return true;
      });
      await assert.rejects(c.request("reload"), (e: unknown) => {
        assert.ok(e instanceof DiagError);
        assert.equal(e.diag.file, undefined);
        assert.equal(e.diag.line, undefined);
        return true;
      });
      c.close();
    } finally {
      await server.close();
    }
  });

  it("turns an error response into a DiagError with the broker's id (unknown command CML5402)", async () => {
    const server = new FakeServer(testPipePath(), () => ({
      diag: { id: "CML5402", message: "unknown command 'nope'", action: "query status" },
    }));
    await server.listen();
    try {
      const c = await ControlClient.connect(server.pipe);
      await assert.rejects(c.request("nope"), (e: unknown) => {
        assert.ok(e instanceof DiagError);
        assert.equal(e.diag.id, "CML5402");
        assert.equal(e.diag.action, "query status");
        return true;
      });
      // The connection stays usable after an error response.
      await assert.rejects(c.request("again"), DiagError);
      c.close();
    } finally {
      await server.close();
    }
  });

  it("reports CML5990 when no broker listens", async () => {
    await assert.rejects(ControlClient.connect(testPipePath(), 1000), (e: unknown) => {
      assert.ok(e instanceof DiagError);
      assert.equal(e.diag.id, "CML5990");
      assert.match(e.diag.action, /chameleon\.controlPipe/);
      return true;
    });
  });

  it("reports CML5991 on 'close' and for the pending request when the pipe is lost", async () => {
    const server = new FakeServer(testPipePath(), () => undefined);
    await server.listen();
    try {
      const c = await ControlClient.connect(server.pipe);
      let closeId = "";
      c.on("close", (d: { id: string }) => (closeId = d.id));
      const pending = c.request("reload", {}, 5000);
      await waitFor(() => server.connections[0]?.requests.length === 1);
      server.dropConnections();
      await assert.rejects(pending, (e: unknown) => e instanceof DiagError && e.diag.id === "CML5991");
      await waitFor(() => closeId !== "");
      assert.equal(closeId, "CML5991");
      assert.equal(c.isOpen, false);
      await assert.rejects(c.request("status"), (e: unknown) => e instanceof DiagError && e.diag.id === "CML5991");
    } finally {
      await server.close();
    }
  });

  it("reports CML5993 when no answer comes in time, and gives the connection up", async () => {
    const server = new FakeServer(testPipePath(), () => undefined);
    await server.listen();
    try {
      const c = await ControlClient.connect(server.pipe);
      let closed = false;
      c.on("close", () => (closed = true));
      await assert.rejects(c.request("reload", {}, 100), (e: unknown) => e instanceof DiagError && e.diag.id === "CML5993");
      await waitFor(() => closed);
    } finally {
      await server.close();
    }
  });

  it("reports CML5993 for a line that is not JSON", async () => {
    const server = new FakeServer(testPipePath(), (_cmd, _args, conn) => {
      conn.raw("this is not json\n");
      return undefined;
    });
    await server.listen();
    try {
      const c = await ControlClient.connect(server.pipe);
      await assert.rejects(c.request("status", {}, 3000), (e: unknown) => e instanceof DiagError && e.diag.id === "CML5993");
    } finally {
      await server.close();
    }
  });
});
