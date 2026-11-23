// The control pipe name, derived as the broker does (core/cml_ipc/src/control.cpp resolve_control_pipe_name, ADR-0021):
//   1. an explicit name (the 'chameleon.controlPipe' setting, like `cml_broker --control-pipe`),
//   2. else the CML_CONTROL_PIPE environment variable,
//   3. else \\.\pipe\cml_control_<logon-session SID>.
// The logon-session SID is the token group flagged SE_GROUP_LOGON_ID (S-1-5-5-X-Y); the broker falls back to the user SID when the
// token has none. Node cannot read tokens, so `whoami /groups` is asked (the logon-id group is the one whose SID starts with S-1-5-5-).
// No `vscode` import.

import { execFile } from "node:child_process";
import * as fs from "node:fs";

export const PIPE_PREFIX = "\\\\.\\pipe\\";

export function hasPipePrefix(name: string): boolean {
  return name.length > PIPE_PREFIX.length && name.toLowerCase().startsWith(PIPE_PREFIX.toLowerCase());
}

/** First logon-session SID (S-1-5-5-X-Y) in the CSV output of `whoami /groups /fo csv /nh`. */
export function parseLogonSid(whoamiGroupsCsv: string): string | undefined {
  const m = /S-1-5-5-\d+-\d+/.exec(whoamiGroupsCsv);
  return m ? m[0] : undefined;
}

/** The user SID in the CSV output of `whoami /user /fo csv /nh` ("DOMAIN\user","S-1-5-21-..."). */
export function parseUserSid(whoamiUserCsv: string): string | undefined {
  const m = /S-1-\d+(?:-\d+)+/.exec(whoamiUserCsv);
  return m ? m[0] : undefined;
}

export interface PipeNameDeps {
  env: NodeJS.ProcessEnv;
  /** Runs a program and returns its stdout; rejects on failure. */
  run(file: string, args: string[]): Promise<string>;
  /** Names in \\.\pipe\ (used only as a last resort). */
  listPipes(): string[];
}

export const realDeps: PipeNameDeps = {
  env: process.env,
  run: (file, args) =>
    new Promise((resolve, reject) => {
      execFile(file, args, { windowsHide: true, timeout: 10000 }, (err, stdout) => (err ? reject(err) : resolve(String(stdout))));
    }),
  listPipes: () => {
    try {
      return fs.readdirSync(PIPE_PREFIX);
    } catch {
      return [];
    }
  },
};

export class PipeNameError extends Error {}

export async function resolveControlPipe(explicitName: string, deps: PipeNameDeps = realDeps): Promise<string> {
  let name = explicitName.trim();
  if (name === "") {
    const fromEnv = (deps.env.CML_CONTROL_PIPE ?? "").trim();
    if (fromEnv !== "") {
      name = fromEnv;
    }
  }
  if (name === "") {
    let sid: string | undefined;
    try {
      sid = parseLogonSid(await deps.run("whoami", ["/groups", "/fo", "csv", "/nh"]));
      if (!sid) {
        sid = parseUserSid(await deps.run("whoami", ["/user", "/fo", "csv", "/nh"]));
      }
    } catch {
      sid = undefined;
    }
    if (sid) {
      name = `${PIPE_PREFIX}cml_control_${sid}`;
    } else {
      // Last resort: a broker of this user is serving a default-named pipe; take it when it is unambiguous.
      const found = deps.listPipes().filter((p) => /^cml_control_S-1-/.test(p));
      if (found.length === 1) {
        name = PIPE_PREFIX + found[0];
      } else {
        throw new PipeNameError(
          "the default control pipe name could not be derived (whoami gave no logon-session SID)",
        );
      }
    }
  }
  if (!hasPipePrefix(name)) {
    throw new PipeNameError(`control pipe name '${name}' does not start with ${PIPE_PREFIX} or has no name part`);
  }
  return name;
}
