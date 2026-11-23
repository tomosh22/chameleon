# integrations/vs: Chameleon for Visual Studio

A VSIX for Visual Studio 2022 (17.x) and 2026 (18.x). It is a client of the broker's control pipe (ADR-0011/0032/0033/0038), nothing else.

* **Tools > Chameleon**: Reload, Hot-Restart, Revert Last, Revert All, Connect, Disconnect, "Re-attach debugger after hot-restart" (a check mark), Show Output.
  A command whose capability the broker does not advertise is disabled (ADR-0032). Bind keys in Tools > Options > Environment > Keyboard
  (commands `Tools.ChameleonReload`, `Tools.ChameleonHotRestart`, ...).
* **Output pane "Chameleon"**: `diag`, `patch_applied`, `patch_reverted`, `restarted` (and `exception` if enabled). A diagnostic that names a
  file and line is written as `path(line,col): error CMLnnnn: ...`, which the Output window turns into a link, and goes to the Error List, where a
  double click opens the line.
* **Automatic re-attach**: when the debugger is attached to a process that is hot-restarted, it is attached to the new process with the same debug
  engines (`restarted {old_pid, new_pid}`). Failures are CML5994-5997 (ADR-0038) with a "what to do" sentence. Turn it off in the Tools >
  Chameleon menu or in Tools > Options > Chameleon > General.

Layout: `src/Chameleon.Control` (netstandard2.0, no Visual Studio dependency: control client, broker session, re-attach decision),
`src/Chameleon.Vsix` (the thin shell), `tests/Chameleon.Control.Tests` (xunit), `tests/Chameleon.IntegrationTest` (real broker).
Details, the manual checklist and the limits: `docs/wp/WP-31.md`.

## Build

`integrations/vs/build.ps1 -Configuration Release` writes `integrations/vs/out/*.vsix`. It needs the .NET SDK and Visual Studio's MSBuild with the
"Visual Studio extension development" workload (windows-2022 images have both). CI: `.github/workflows/vs.yml`.

## Dependency licences

| Package | Use | Licence |
|---|---|---|
| Newtonsoft.Json 13.0.3 | JSON in the control client (shipped in the VSIX; Visual Studio ships the same major version) | MIT |
| System.Security.Principal.Windows 5.0.0 | logon-session SID for the default pipe name (shipped) | MIT |
| Microsoft.VisualStudio.SDK 17.6, Microsoft.VisualStudio.Debugger.InteropA 17.5, Microsoft.VSSDK.BuildTools 17.7 | compile references and VSIX build tooling only; not shipped | Microsoft Visual Studio SDK licence terms (a Microsoft SDK package, permitted by ADR-0038) |
| xunit 2.9.2 | unit tests | Apache-2.0 |
| xunit.runner.visualstudio 2.8.2, Microsoft.NET.Test.Sdk 17.12.0 | test runner | MIT |

No GPL or LGPL package is used, directly or transitively.
