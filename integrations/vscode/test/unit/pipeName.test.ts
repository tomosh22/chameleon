import * as assert from "node:assert/strict";
import { describe, it } from "node:test";
import { PipeNameDeps, PipeNameError, hasPipePrefix, parseLogonSid, parseUserSid, resolveControlPipe } from "../../src/pipeName";

const GROUPS = [
  '"Everyone","Well-known group","S-1-1-0","Mandatory group, Enabled by default, Enabled group"',
  '"NT AUTHORITY\\LogonSessionId_0_123456","Well-known group","S-1-5-5-0-123456","Mandatory group, Enabled by default, Enabled group, Group used for logon"',
].join("\r\n");

function deps(over: Partial<PipeNameDeps> = {}): PipeNameDeps {
  return { env: {}, run: async () => GROUPS, listPipes: () => [], ...over };
}

describe("resolveControlPipe", () => {
  it("prefers the explicit name, then CML_CONTROL_PIPE", async () => {
    assert.equal(await resolveControlPipe("\\\\.\\pipe\\mine", deps({ env: { CML_CONTROL_PIPE: "\\\\.\\pipe\\env" } })), "\\\\.\\pipe\\mine");
    assert.equal(await resolveControlPipe("  ", deps({ env: { CML_CONTROL_PIPE: "\\\\.\\pipe\\env" } })), "\\\\.\\pipe\\env");
  });
  it("derives cml_control_<logon SID> from whoami", async () => {
    assert.equal(await resolveControlPipe("", deps()), "\\\\.\\pipe\\cml_control_S-1-5-5-0-123456");
  });
  it("falls back to the user SID, as the broker does", async () => {
    const run = async (_f: string, args: string[]): Promise<string> => (args[0] === "/groups" ? '"Everyone","x","S-1-1-0","y"' : '"PC\\me","S-1-5-21-1-2-3-1001"');
    assert.equal(await resolveControlPipe("", deps({ run })), "\\\\.\\pipe\\cml_control_S-1-5-21-1-2-3-1001");
  });
  it("falls back to a single cml_control_ pipe that exists", async () => {
    const run = async (): Promise<string> => {
      throw new Error("no whoami");
    };
    assert.equal(await resolveControlPipe("", deps({ run, listPipes: () => ["x", "cml_control_S-1-5-5-0-9"] })), "\\\\.\\pipe\\cml_control_S-1-5-5-0-9");
  });
  it("fails clearly when nothing can be derived or the name has no pipe prefix", async () => {
    const run = async (): Promise<string> => {
      throw new Error("no whoami");
    };
    await assert.rejects(resolveControlPipe("", deps({ run })), PipeNameError);
    await assert.rejects(resolveControlPipe("C:\\tmp\\x", deps()), /does not start with/);
  });
});

describe("pipe name helpers", () => {
  it("parses SIDs and the prefix", () => {
    assert.equal(parseLogonSid(GROUPS), "S-1-5-5-0-123456");
    assert.equal(parseLogonSid("nothing"), undefined);
    assert.equal(parseUserSid('"PC\\me","S-1-5-21-1-2-3-1001"'), "S-1-5-21-1-2-3-1001");
    assert.equal(hasPipePrefix("\\\\.\\PIPE\\x"), true);
    assert.equal(hasPipePrefix("\\\\.\\pipe\\"), false);
  });
});
