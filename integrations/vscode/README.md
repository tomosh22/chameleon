# Chameleon for VS Code

Reload, hot-restart and revert native C/C++ code from VS Code. The extension is a client of the Chameleon broker's control pipe
(ADR-0021 transport and framing, ADR-0032 shapes, ADR-0038 integration contract); it never touches the application or the agent.
Details, diagnostics and the manual checklist: `docs/wp/WP-30.md`.

## Commands

| Command | What it does | Broker capability |
|---|---|---|
| `Chameleon: Reload` (default key `Ctrl+Alt+F9`) | `reload`: recompile what changed and patch the running processes | `reload` |
| `Chameleon: Hot-Restart` | `restart` one process (a quick pick appears when several are connected) | `restart` |
| `Chameleon: Revert Last` / `Revert All` | `revert` the newest reload / every patch (All asks first) | `revert` |
| `Chameleon: Show Output` | opens the **Chameleon** output channel | |
| `Chameleon: Connect` / `Disconnect` | connect to the broker (reconnects by itself) / stop | |

A command whose capability the broker does not advertise is not sent: the extension says so (CML5992) and the status bar tooltip
names the missing capabilities. The default key `Ctrl+Alt+F9` was chosen because it is not one of VS Code's default bindings and
differs from the `Ctrl+Alt+F11` global hotkey of `cml_ui`; check `Preferences: Open Keyboard Shortcuts` if another extension uses it.

## What you see

* **Output channel "Chameleon"**: the broker's `diag`, `patch_applied`, `patch_reverted`, `restarted` and `exception` events, and the
  result of every command. A `diag` message is printed verbatim, so compiler lines stay intact.
* **Problems panel**: lines of a `diag` message that name a file and a line (`file(12): error C2065: ...` or
  `file:12:5: error: ...`) become Problems entries; they are cleared by the next reload and when a patch is applied. The same two
  patterns are contributed as problem matchers `$chameleon` and `$chameleon-clang` (for a task that runs a compiler and the like).
* **Status bar**: disconnected / connecting / connected, a spinner while reloading, the last result (`reload ok`,
  `reload failed CML2201`) and the patch generation. Click: connect when disconnected, reload when connected.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `chameleon.controlPipe` | empty | The broker's control pipe. Empty = what the broker serves by default: `CML_CONTROL_PIPE` if set in VS Code's environment, else `\\.\pipe\cml_control_<logon-session SID>`. |
| `chameleon.autoConnect` | true | Connect at start-up and keep reconnecting. |
| `chameleon.reloadTimeoutMs` | 60000 | Time limit given to the broker for a reload. |
| `chameleon.watchExceptions` | false | Also subscribe to `exception` events. While subscribed the broker holds a faulting thread until you answer in the notification (Continue / Leave function / Ignore). |

The default pipe name is derived like the broker does (`core/cml_ipc/src/control.cpp`): the logon-session SID is the token group
`S-1-5-5-X-Y`, which Node cannot read directly, so the extension reads it from `whoami /groups`.

## Build

```
cd integrations/vscode
npm ci
npm run lint            # tsc --noEmit (strict, no unused code)
npm test                # compile + node:test unit tests (fake control server on a pipe)
npm run package         # writes exactly one integrations/vscode/*.vsix
```

`npm run test:integration -- --broker ... --fixture ... --edit-file ... --work-dir ...` runs the real-broker test; CI does that in
`.github/workflows/vscode.yml`. Install the package with `code --install-extension chameleon-hot-reload-0.1.0.vsix`.

## Structure

`src/controlClient.ts` (framing, requests, events), `src/session.ts` (two connections, reconnect, capability gate), `src/pipeName.ts`,
`src/format.ts` (event text, compiler-line parser, status bar model), `src/diagIds.ts` (CML5990-5993) have no `vscode` import (a test
enforces it) and run in plain Node. Only `src/extension.ts` talks to the `vscode` API.

## Dependencies and licences

No runtime dependencies: the shipped extension uses the `vscode` API and Node built-ins only. Dev dependencies (exact versions in
`package-lock.json`):

| Package | Licence | Use |
|---|---|---|
| `typescript` 5.6.3 | Apache-2.0 | compiler, lint (`tsc --noEmit`) |
| `@vscode/vsce` 3.2.1 | MIT | packages the `.vsix` |
| `@types/node` 20.17.6 | MIT | types |
| `@types/vscode` 1.85.0 | MIT | types |

The 200-odd transitive packages of this tree are MIT (148), ISC, BSD-2/3-Clause, BlueOak-1.0.0, Apache-2.0, 0BSD, Python-2.0,
`(MIT OR WTFPL)` and `(BSD-2-Clause OR MIT OR Apache-2.0)`; none is GPL or LGPL. The one exception to "permissive" is
`@vscode/vsce-sign` (and its per-platform binary packages), a Microsoft package under the licence in its own `LICENSE.txt`; `vsce` pulls
it in as an optional dependency, it is used only while building and is not part of the `.vsix` (ADR-0038 allows Microsoft packages for
building the VSIX). Test runner: `node:test`, built into Node 20+.
