# Chameleon quickstart

From a fresh Windows machine to your first hot reload, using only the release zip
(`chameleon-<version>-win64.zip`). You do not need the Chameleon source code or a build of Chameleon.

What you will do: unpack the zip, build the example program with Visual Studio's compiler, start the broker, start
the program, change one line, reload, and see the new value in the running program. About 10 minutes.

## 0. What you need

* Windows 10 or 11 x64 (or Windows Server 2022 or later).
* Visual Studio 2022 (Community is enough) or the Build Tools for Visual Studio 2022, with the workload
  "Desktop development with C++" (MSVC v143 and a Windows 10/11 SDK). This also provides the C++ runtime that
  `cml_broker.exe` needs. LLVM `clang-cl` 15 or newer works as well, on top of that install.
* CMake 3.24 or newer and Ninja. Both ship with Visual Studio's "C++ CMake tools for Windows" component; the command
  prompt in step 1 puts them on `PATH`. Check with `cmake --version` and `ninja --version`. If either is missing:
  `winget install Kitware.CMake Ninja-build.Ninja`, then open a new prompt.

Everything below is typed in an **x64 Native Tools Command Prompt for VS 2022** (Start menu, search for "x64 Native
Tools"). It has `cl.exe`, CMake and Ninja on `PATH`, which Chameleon needs when it recompiles your code.

## 1. Unpack

```bat
mkdir C:\chameleon-demo
cd /d C:\chameleon-demo
tar -xf "%USERPROFILE%\Downloads\chameleon-0.1.0-win64.zip"
```

Adjust the path and version to where the zip is. The zip unpacks into `chameleon-<version>-win64`. Use that folder as
`CML`:

```bat
cd /d C:\chameleon-demo
for /d %d in (chameleon-*-win64) do set CML=C:\chameleon-demo\%d
echo %CML%
dir %CML%
```

`dir` shows `bin`, `docs`, `examples`, `include`, `integrations`, `lib`, `licenses`, `scripts`, `MANIFEST.txt` and `README.txt`. Check the program runs:

```bat
%CML%\bin\cml_broker.exe --version
```

It prints `cml_broker 0.1.0` (the version of your zip). If Windows says a DLL such as `VCRUNTIME140.dll` is missing, install
the "Microsoft Visual C++ Redistributable" (x64) or use a Visual Studio prompt.

## 2. Build the example program

The package contains a small example, `examples\hello`: a loop that prints `SMOKE tick=<n> value=<hello value>` every 100 ms
and enables its own executable for hot reload. Copy it out of the package (so you can edit it) and build it:

```bat
xcopy /E /I /Y %CML%\examples\hello C:\chameleon-demo\hello
cd /d C:\chameleon-demo\hello
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=%CML%
cmake --build build
```

`CMakeLists.txt` of the example is the whole integration: `find_package(Chameleon REQUIRED)` finds the package through
`CMAKE_PREFIX_PATH`, and `chameleon_enable(smoke_app)` adds the compiler and linker flags Chameleon needs (full PDB,
function-level linking, hot-patch padding, no inlining in RelWithDebInfo; see `docs\BUILD_REQUIREMENTS.md`) and the
agent header.

Check that the program meets Chameleon's build requirements:

```bat
%CML%\bin\cml_broker.exe check build\smoke_app.exe
```

Every line starts with `PASS`. A `WARN` line is tolerated, a `FAIL` line names a diagnostic id: look it up in
`%CML%\docs\TROUBLESHOOTING.md`.

## 3. Start the broker

Open a **second** x64 Native Tools Command Prompt and start the broker. Leave it running:

```bat
cd /d C:\chameleon-demo
for /d %d in (chameleon-*-win64) do set CML=C:\chameleon-demo\%d
%CML%\bin\cml_broker.exe --headless --verbose
```

It prints `cml_broker 0.1.0 pid=... pipe=...` and `licence check: allowed (no-op licence provider: all features enabled)`, then
waits for programs.

## 4. Start the program

Back in the first prompt:

```bat
cd /d C:\chameleon-demo\hello
build\smoke_app.exe --agent-dir %CML%\bin
```

It prints `SMOKE enabled=1 ...` once and then `SMOKE tick=<n> value=1` ten times a second (it stops by itself after two
minutes; run it again if that happens). The broker window logs that the module was enabled and its PDB indexed.

## 5. Change the code and reload

In a **third** x64 Native Tools prompt (the compiler must be on `PATH`, because the broker is started from a prompt that
has it, the broker window, and recompiles with it; this prompt only sends the request):

```bat
cd /d C:\chameleon-demo\hello
powershell -NoProfile -Command "(Get-Content logic.cpp) -replace 'return 1;','return 2;' | Set-Content logic.cpp"
powershell -NoProfile -ExecutionPolicy Bypass -File %CML%\scripts\cml-reload.ps1
```

(Set `CML` in this prompt like in step 3 if it is not set.) The reload command compiles `logic.cpp`, links a patch and loads it
into the running program, and prints a JSON answer with `"ok":true`. In the prompt that runs `smoke_app.exe` the output
changes from `value=1` to `value=2` without a restart.

Instead of `cml-reload.ps1` you can use the VS Code or Visual Studio extension (install the `.vsix` files from
`%CML%\integrations`; they talk to the same broker) or `cml_ui` when your package has it.

## 6. If something does not work

* Every failure has an id such as `CML5300`. Look it up in `%CML%\docs\TROUBLESHOOTING.md` (cause, message, what to do).
* The broker window (run with `--verbose`) shows why. `--log broker.log` writes it to a file.
* `cml_broker check <your.exe>` tells you whether your build settings are acceptable.
* Not everything can be patched in place; see `%CML%\docs\KNOWN_LIMITATIONS.md`.

## 7. Use it in your own project

Add to your `CMakeLists.txt`:

```cmake
find_package(Chameleon REQUIRED)
chameleon_enable(my_target)
```

and configure with `-DCMAKE_PREFIX_PATH=<path to the unzipped package>`. In your program, include `Chameleon_API.h`,
create the agent with `cml::CreateDefaultAgent(<directory of cml_agent.dll>)` and call `EnableModule` for the
executable and DLLs you want to reload (see the header's comments). Ship `cml_agent.dll` only with development builds.
Without CMake, apply the flags of `docs\BUILD_REQUIREMENTS.md` by hand.
