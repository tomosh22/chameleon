// Integration test (Windows, run by .github/workflows/vscode.yml): the extension's own control client and session against a real
// cml_broker and a running `basic` fixture.
//
//   node out/test/integration/run.js --broker <cml_broker.exe> --fixture <fixture_basic.exe> --edit-file <logic.cpp of the fixture's
//        source copy> --work-dir <dir>
//
// Steps: start the broker on its DEFAULT control pipe (so the derivation of the pipe name is checked, no CML_CONTROL_PIPE, no
// --control-pipe), connect through Session (status + subscribe), start the fixture with the agent, wait until the module is
// enabled and indexed, edit the source, `reload` (expect patch_applied and value=2 on the fixture's stdout), `revert` (expect
// patch_reverted and value=1 again). Every step prints a greppable `WP30-IT` line; the last one is `WP30-IT reload=ok revert=ok`.

import { ChildProcess, spawn } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { ControlEvent, JsonObject } from "../../src/controlClient";
import { DiagError, formatDiag } from "../../src/diagIds";
import { realDeps, resolveControlPipe } from "../../src/pipeName";
import { Session } from "../../src/session";

function arg(name: string): string {
  const i = process.argv.indexOf(`--${name}`);
  if (i < 0 || i + 1 >= process.argv.length) {
    throw new Error(`missing --${name}`);
  }
  return process.argv[i + 1];
}

const sleep = (ms: number): Promise<void> => new Promise((r) => setTimeout(r, ms));

async function waitFor(what: string, cond: () => boolean | Promise<boolean>, ms: number): Promise<void> {
  const end = Date.now() + ms;
  for (;;) {
    if (await cond()) {
      return;
    }
    if (Date.now() > end) {
      throw new Error(`timed out after ${ms} ms waiting for ${what}`);
    }
    await sleep(100);
  }
}

function tail(file: string, lines = 30): string {
  try {
    return fs.readFileSync(file, "utf8").split(/\r?\n/).slice(-lines).join("\n");
  } catch {
    return "(no file)";
  }
}

async function main(): Promise<void> {
  const brokerExe = path.resolve(arg("broker"));
  const fixtureExe = path.resolve(arg("fixture"));
  const editFile = path.resolve(arg("edit-file"));
  const work = path.resolve(arg("work-dir"));
  fs.mkdirSync(work, { recursive: true });

  const env = { ...process.env };
  delete env.CML_CONTROL_PIPE;
  const agentPipe = `\\\\.\\pipe\\cml_vscode_it_${process.pid}_${Date.now()}`;
  const brokerLog = path.join(work, "broker.log");
  const brokerOut = path.join(work, "broker.stdout.txt");
  const fixtureOut = path.join(work, "fixture.stdout.txt");
  const agentLog = path.join(work, "agent.log");

  const children: ChildProcess[] = [];
  const stop = (): void => {
    for (const c of children) {
      try {
        c.kill();
      } catch {
        // already gone
      }
    }
  };

  const broker = spawn(brokerExe, ["--headless", "--pipe", agentPipe, "--log", brokerLog, "--startup-timeout", "300000", "--verbose"], {
    cwd: work,
    env: { ...env, CML_SESSION_DIR: path.join(work, "broker-session") },
    stdio: ["ignore", fs.openSync(brokerOut, "w"), fs.openSync(brokerOut, "a")],
    windowsHide: true,
  });
  children.push(broker);
  const defaultPipe = await resolveControlPipe("", { env, run: realDeps.run, listPipes: realDeps.listPipes });
  console.log(`WP30-IT pipe=${defaultPipe} derived=whoami-or-powershell`);
  let brokerExit: number | null | undefined;
  broker.on("exit", (code) => (brokerExit = code));

  const session = new Session({ resolvePipe: () => Promise.resolve(defaultPipe), watchExceptions: () => false, reconnectDelaysMs: [200, 500], requestTimeoutMs: 120000 });
  const events: ControlEvent[] = [];
  session.on("event", (e: ControlEvent) => events.push(e));
  const problems: string[] = [];
  session.on("problem", (d) => problems.push(formatDiag(d)));

  try {
    await session.start();
    await waitFor(
      "the broker's control pipe",
      () => {
        if (brokerExit !== undefined) {
          throw new Error(`the broker exited with code ${brokerExit}; log:\n${tail(brokerLog)}\n${tail(brokerOut)}`);
        }
        return session.state === "connected";
      },
      60000,
    );
    const caps = [...session.capabilities].sort();
    for (const need of ["status", "reload", "subscribe", "revert"]) {
      if (!session.capabilities.has(need)) {
        throw new Error(`the broker does not advertise '${need}' (has: ${caps.join(", ")})`);
      }
    }
    console.log(`WP30-IT status=ok version=${session.status?.version} capabilities=${caps.join(",")}`);
    console.log("WP30-IT subscribe=ok");

    const fixture = spawn(fixtureExe, ["--agent"], {
      cwd: path.dirname(fixtureExe),
      env: {
        ...env,
        CML_PIPE_NAME: agentPipe,
        CML_CONTROL_PIPE: defaultPipe,
        CML_AGENT_LOG: agentLog,
        CML_BROKER_PATH: path.join(work, "no_such_broker.exe"),
      },
      stdio: ["ignore", fs.openSync(fixtureOut, "w"), fs.openSync(fixtureOut, "a")],
      windowsHide: true,
    });
    children.push(fixture);
    let fixtureExit: number | null | undefined;
    fixture.on("exit", (code) => (fixtureExit = code));
    const stdoutHas = (re: RegExp): boolean => re.test(fs.readFileSync(fixtureOut, "utf8"));

    await waitFor("fixture TICK value=1", () => {
      if (fixtureExit !== undefined) {
        throw new Error(`the fixture exited with code ${fixtureExit}; stdout:\n${tail(fixtureOut)}\nagent log:\n${tail(agentLog)}`);
      }
      return stdoutHas(/TICK n=\d+ value=1\b/);
    }, 60000);

    // The module is enabled and its PDB index is ready (an edit before that would be baked into the baseline).
    const moduleName = path.basename(fixtureExe).toLowerCase();
    await waitFor(
      `${moduleName} enabled and indexed`,
      async () => {
        const raw: JsonObject = await session.call("status", {});
        const procs = (raw.processes as JsonObject[]) ?? [];
        return procs.some((p) =>
          ((p.modules as JsonObject[]) ?? []).some(
            (m) => String(m.path).toLowerCase().endsWith(moduleName) && m.enabled === true && (m.index === undefined || (m.index as JsonObject).state === "ready"),
          ),
        );
      },
      120000,
    );
    console.log(`WP30-IT module=${moduleName} enabled=yes`);

    // Scripted edit: compute_value() returns 2 instead of 1.
    const before = fs.readFileSync(editFile, "utf8");
    const after = before.replace(/return 1;/, "return 2;");
    if (after === before) {
      throw new Error(`the edit did not change ${editFile}`);
    }
    fs.writeFileSync(editFile, after);

    const mark = events.length;
    const reload = await session.call("reload", { timeout_ms: 90000 }, 120000);
    await waitFor("patch_applied event", () => events.slice(mark).some((e) => e.event === "patch_applied"), 10000);
    await waitFor("fixture TICK value=2", () => stdoutHas(/TICK n=\d+ value=2\b/), 30000);
    console.log(`WP30-IT reload=ok generation=${String(reload.generation)} patched=${String(reload.patched)} event=patch_applied value=2`);

    const mark2 = events.length;
    const revert = await session.call("revert", { scope: "last" }, 120000);
    await waitFor("patch_reverted event", () => events.slice(mark2).some((e) => e.event === "patch_reverted"), 10000);
    // The fixture prints value=1 again: look only at output written after the revert.
    const size = fs.statSync(fixtureOut).size;
    await waitFor(
      "fixture TICK value=1 after the revert",
      () => /TICK n=\d+ value=1\b/.test(fs.readFileSync(fixtureOut, "utf8").slice(size)),
      30000,
    );
    console.log(`WP30-IT revert=ok reverted=${Array.isArray(revert.reverted) ? revert.reverted.length : 0} event=patch_reverted value=1`);

    // A command the broker does not know is CML5402 for the raw client; through the Session it is CML5992 (feature unavailable).
    try {
      await session.call("no-such-command", {});
      throw new Error("an unknown command was accepted");
    } catch (e) {
      if (!(e instanceof DiagError) || (e.diag.id !== "CML5992" && e.diag.id !== "CML5402")) {
        throw e;
      }
      console.log(`WP30-IT unknown-command=${e.diag.id}`);
    }

    console.log("WP30-IT reload=ok revert=ok");
  } catch (e) {
    console.log(`WP30-IT FAILED ${e instanceof Error ? e.message : String(e)}`);
    console.log(`connection problems: ${problems.join(" | ") || "(none)"}`);
    console.log(`--- broker log ---\n${tail(brokerLog)}\n--- broker stdout ---\n${tail(brokerOut)}\n--- fixture stdout ---\n${tail(fixtureOut)}\n--- agent log ---\n${tail(agentLog)}`);
    process.exitCode = 1;
  } finally {
    session.stop();
    stop();
    await sleep(500);
  }
}

void main();
