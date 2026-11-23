<#
.SYNOPSIS
  Builds the Chameleon release package (WP-32): chameleon-<version>-win64.zip, installer-free.

.DESCRIPTION
  1. Release build: RelWithDebInfo, CML_FAULT_INJECTION=OFF, CML_BUILD_TESTS=OFF (cl by default).
  2. cmake --install into a staging directory (layout of ADR-0038: bin/, include/, lib/, lib/cmake/Chameleon/).
  3. Adds integrations/*.vsix (VS Code and Visual Studio extensions), docs/, LICENSE and the licences of the vendored
     third-party code.
  4. Writes MANIFEST.txt (SHA-256 of every file), runs the package checks (required files, no debug-only or
     fault-injection symbols or strings) and zips the staging directory.

  Parts that depend on work that has not merged yet are skipped with a loud "WP32-PENDING <part>" line; -Strict turns
  every one of them into a failure (the release gate).

  Greppable output:  WP32-PACKAGE ...  and  WP32-CHECK ...   A failure prints WP32-CHECK FAIL <reason> and exits 1.

.EXAMPLE
  powershell -File tools\package\package.ps1 -OutDir build\package
#>
[CmdletBinding()]
param(
    [string]$Version,                         # default: project(Chameleon VERSION ...) of the top-level CMakeLists.txt
    [string]$OutDir = 'build/package',        # build tree, staging directory and the zip go below this directory
    [ValidateSet('cl', 'clang-cl')][string]$Compiler = 'cl',
    [switch]$SkipExtensions,                  # do not build the two .vsix files (they are then reported as pending)
    [switch]$Strict                           # a WP32-PENDING part is a failure
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script:pending = New-Object System.Collections.Generic.List[string]

function Fail([string]$what) { Write-Host "WP32-CHECK FAIL $what"; exit 1 }
function Pending([string]$part, [string]$why) {
    $script:pending.Add($part)
    Write-Host "WP32-PENDING $part ($why)"
}
function Invoke-Native([string]$what, [scriptblock]$command) {
    & $command
    if ($LASTEXITCODE -ne 0) { Fail "$what failed with exit code $LASTEXITCODE" }
}

# ---- version -------------------------------------------------------------------------------------------------------
if (-not $Version) {
    $top = Get-Content -Raw (Join-Path $repo 'CMakeLists.txt')
    if ($top -notmatch 'project\(\s*Chameleon[^)]*?VERSION\s+([0-9]+(\.[0-9]+){1,3})') { Fail 'cannot read the version from project(Chameleon VERSION ...) in CMakeLists.txt' }
    $Version = $Matches[1]
}
$name = "chameleon-$Version-win64"

# ---- toolchain: cl on PATH, or the newest Visual Studio's x64 developer environment --------------------------------
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { Fail 'cl.exe is not on PATH and vswhere.exe is not installed: run this from an x64 Native Tools prompt' }
    $vs = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if (-not $vs) { Fail 'no Visual Studio with the C++ x64 tools was found' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    foreach ($line in (cmd /c "`"$vcvars`" >nul && set")) {
        if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
}
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) { Fail 'cl.exe is still not on PATH' }
$cxx = if ($Compiler -eq 'clang-cl') { 'clang-cl' } else { 'cl' }

# A relative -OutDir is resolved from the caller's current directory (from the repository root when it is left at its default).
if ([IO.Path]::IsPathRooted($OutDir)) { $out = [IO.Path]::GetFullPath($OutDir) }
elseif ($PSBoundParameters.ContainsKey('OutDir')) { $out = [IO.Path]::GetFullPath((Join-Path (Get-Location).Path $OutDir)) }
else { $out = [IO.Path]::GetFullPath((Join-Path $repo $OutDir)) }
$build = Join-Path $out 'build'
$stage = Join-Path $out $name
$zip = Join-Path $out "$name.zip"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
if (Test-Path $zip) { Remove-Item -Force $zip }
New-Item -ItemType Directory -Force -Path $out | Out-Null

# ---- 1. release build ----------------------------------------------------------------------------------------------
if (Get-Command ninja -ErrorAction SilentlyContinue) {
    $generator = @('-G', 'Ninja'); $compilerArgs = @("-DCMAKE_CXX_COMPILER=$cxx")
} elseif ($Compiler -eq 'clang-cl') {
    $generator = @('-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'ClangCL'); $compilerArgs = @()
} else {
    $generator = @('-G', 'Visual Studio 17 2022', '-A', 'x64'); $compilerArgs = @()
}
Invoke-Native 'cmake configure' {
    cmake -S $repo -B $build @generator -DCMAKE_BUILD_TYPE=RelWithDebInfo @compilerArgs `
        -DCML_FAULT_INJECTION=OFF -DCML_BUILD_TESTS=OFF -DCML_BUILD_GUI=OFF
}
Invoke-Native 'cmake build' { cmake --build $build --config RelWithDebInfo --parallel }

# ---- 2. install into the staging directory -------------------------------------------------------------------------
Invoke-Native 'cmake install' { cmake --install $build --config RelWithDebInfo --prefix $stage }
if (-not (Test-Path (Join-Path $stage 'bin\cml_broker.exe'))) {
    # No install rules yet (WP-29): lay out the same tree from the build directory.
    Pending 'install-rules' 'cmake --install did not install bin\cml_broker.exe; copying from the build tree'
    $binDir = Join-Path $build 'bin'
    if (-not (Test-Path (Join-Path $binDir 'cml_broker.exe'))) { $binDir = Join-Path $build 'bin\RelWithDebInfo' }
    New-Item -ItemType Directory -Force -Path (Join-Path $stage 'bin'), (Join-Path $stage 'include') | Out-Null
    foreach ($f in 'cml_broker.exe', 'cml_agent.dll') { Copy-Item (Join-Path $binDir $f) (Join-Path $stage "bin\$f") }
    foreach ($f in 'cml_broker.pdb', 'cml_agent.pdb') { if (Test-Path (Join-Path $binDir $f)) { Copy-Item (Join-Path $binDir $f) (Join-Path $stage "bin\$f") } }
    Copy-Item (Join-Path $repo 'agent\include\Chameleon_API.h') (Join-Path $stage 'include\Chameleon_API.h')
}

# ---- 3. extensions -------------------------------------------------------------------------------------------------
$integrations = Join-Path $stage 'integrations'
New-Item -ItemType Directory -Force -Path $integrations | Out-Null
if ($SkipExtensions) {
    Pending 'vscode-vsix' '-SkipExtensions'
    Pending 'vs-vsix' '-SkipExtensions'
} else {
    $vscodeDir = Join-Path $repo 'integrations\vscode'
    if (Test-Path (Join-Path $vscodeDir 'package.json')) {
        Get-ChildItem -Path $vscodeDir -Filter '*.vsix' -File | Remove-Item -Force  # no stale package from an earlier build
        Push-Location $vscodeDir
        try {
            Invoke-Native 'npm ci' { npm ci }
            Invoke-Native 'npm run package' { npm run package }
        } finally { Pop-Location }
        $vsix = @(Get-ChildItem -Path $vscodeDir -Filter '*.vsix' -File)
        if ($vsix.Count -ne 1) { Fail "integrations/vscode: expected exactly one .vsix after 'npm run package', found $($vsix.Count)" }
        Copy-Item $vsix[0].FullName $integrations
    } else {
        Pending 'vscode-vsix' 'integrations/vscode has no package.json (WP-30)'
    }
    $vsDir = Join-Path $repo 'integrations\vs'
    if (Test-Path (Join-Path $vsDir 'build.ps1')) {
        Get-ChildItem -Path (Join-Path $vsDir 'out') -Filter '*.vsix' -File -ErrorAction SilentlyContinue | Remove-Item -Force
        Invoke-Native 'integrations/vs/build.ps1' { & (Join-Path $vsDir 'build.ps1') -Configuration Release }
        $vsix = @(Get-ChildItem -Path (Join-Path $vsDir 'out') -Filter '*.vsix' -File -ErrorAction SilentlyContinue)
        if ($vsix.Count -lt 1) { Fail 'integrations/vs/out has no .vsix after build.ps1 -Configuration Release' }
        foreach ($v in $vsix) { Copy-Item $v.FullName $integrations }
    } else {
        Pending 'vs-vsix' 'integrations/vs has no build.ps1 (WP-31)'
    }
}
# The CMake examples (find_package(Chameleon)) travel with the package: the smoke test and the user build them from here.
$examples = Join-Path $repo 'integrations\cmake\examples'
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'examples'), (Join-Path $stage 'scripts') | Out-Null
Copy-Item -Recurse (Join-Path $PSScriptRoot 'smoke') (Join-Path $stage 'examples\hello')  # the quickstart's example program
Copy-Item (Join-Path $PSScriptRoot 'cml-reload.ps1') (Join-Path $stage 'scripts\cml-reload.ps1')
if (Test-Path $examples) { Copy-Item -Recurse $examples (Join-Path $stage 'examples\cmake') }
else { Pending 'cmake-examples' 'integrations/cmake/examples does not exist (WP-29)' }

# ---- docs, licences ------------------------------------------------------------------------------------------------
$docs = Join-Path $stage 'docs'
New-Item -ItemType Directory -Force -Path $docs | Out-Null
foreach ($d in 'QUICKSTART.md', 'TROUBLESHOOTING.md', 'BUILD_REQUIREMENTS.md', 'KNOWN_LIMITATIONS.md') {
    $p = Join-Path $repo "docs\$d"
    if (-not (Test-Path $p)) { Fail "docs\$d is missing in the repository" }
    Copy-Item $p $docs
}
if (Test-Path (Join-Path $repo 'LICENSE')) { Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $stage 'LICENSE') }
else { Pending 'LICENSE' 'the repository has no LICENSE file; Chameleon is proprietary (ADR-0039); restore the LICENSE file at the repository root' }

$tp = Join-Path $stage 'licenses\third_party'
New-Item -ItemType Directory -Force -Path $tp | Out-Null
Copy-Item (Join-Path $repo 'third_party\README.md') (Join-Path $stage 'licenses\THIRD_PARTY_NOTICES.md')
foreach ($dir in Get-ChildItem -Directory (Join-Path $repo 'third_party')) {
    $files = @(Get-ChildItem -File $dir.FullName | Where-Object { $_.Name -match '^(LICENSE|LICENCE|COPYING|NOTICE)' })
    if ($files.Count -eq 0) { Fail "third_party\$($dir.Name) has no licence file; every vendored dependency ships its licence" }
    $target = Join-Path $tp $dir.Name
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    foreach ($f in $files) { Copy-Item $f.FullName $target }
}

$readme = @'
Chameleon @VERSION@ (Windows x64)

Native C/C++ hot reload. Start with docs\QUICKSTART.md.

  bin\cml_broker.exe      the broker (headless): cml_broker --headless
  bin\cml_agent.dll       the agent your program loads
  include\Chameleon_API.h the header your program includes
  lib\cmake\Chameleon\    find_package(Chameleon) and chameleon_enable(target)
  examples\hello\        the quickstart's example program
  scripts\cml-reload.ps1  sends `reload` (or any control command) to the running broker
  integrations\           VS Code and Visual Studio extensions (.vsix)
  docs\                   quickstart, build requirements, troubleshooting by diagnostic id, known limitations
  licenses\               licence texts of the bundled third-party code
  MANIFEST.txt            every file with its SHA-256
'@
$readme = $readme.Replace('@VERSION@', $Version)
Set-Content -Path (Join-Path $stage 'README.txt') -Value $readme -Encoding utf8

# ---- 4. checks -----------------------------------------------------------------------------------------------------
$required = @(
    'bin\cml_broker.exe', 'bin\cml_agent.dll', 'include\Chameleon_API.h', 'examples\hello\main.cpp', 'scripts\cml-reload.ps1',
    'docs\QUICKSTART.md', 'docs\TROUBLESHOOTING.md', 'docs\BUILD_REQUIREMENTS.md',
    'licenses\THIRD_PARTY_NOTICES.md', 'licenses\third_party\raw_pdb\LICENSE', 'licenses\third_party\xxhash\LICENSE', 'README.txt'
)
$missing = @($required | Where-Object { -not (Test-Path (Join-Path $stage $_)) })
if ($missing) { Fail ("required files are missing from the package: " + ($missing -join ', ')) }
foreach ($pattern in @(
        @{ Part = 'cmake-package'; Path = 'lib\cmake\Chameleon\ChameleonConfig.cmake' },
        @{ Part = 'cmake-package'; Path = 'lib\cmake\Chameleon\ChameleonConfigVersion.cmake' })) {
    if (-not (Test-Path (Join-Path $stage $pattern.Path))) { Pending $pattern.Part "$($pattern.Path) was not installed (WP-29)" }
}
if (@(Get-ChildItem -Path $integrations -Filter '*.vsix' -File).Count -lt 2 -and -not $SkipExtensions -and
    -not ($script:pending -contains 'vscode-vsix' -or $script:pending -contains 'vs-vsix')) {
    Fail 'integrations\ must hold the VS Code and the Visual Studio .vsix'
}

# Debug-only and fault-injection content must not be in the shipped binaries. Plain ASCII scan of every exe and dll.
#  - debug CRT (every binary): the import names of the debug runtime, or debug heap reporting
#  - fault injection (the broker): the capability name exists only when CML_FAULT_INJECTION
#    is on. (Some diagnostic texts that mention the control command `inject-failure` are not compiled out, so that
#    text is not a usable marker.)
#  - positive evidence (broker and agent): the "built without CML_FAULT_INJECTION" answers exist only in a build with it OFF
$banned = @(
    @{ Text = 'ucrtbased.dll';        Why = 'debug C runtime import';  Only = $null },
    @{ Text = 'vcruntime140d.dll';    Why = 'debug C runtime import';  Only = $null },
    @{ Text = 'vcruntime140_1d.dll';  Why = 'debug C runtime import';  Only = $null },
    @{ Text = 'msvcp140d.dll';        Why = 'debug C++ runtime import'; Only = $null },
    @{ Text = '_CrtDbgReport';        Why = 'debug heap reporting';    Only = $null },
    @{ Text = 'fault-injection';      Why = 'fault injection capability name'; Only = 'cml_broker.exe' }
)
$expected = @(
    @{ Text = 'built without CML_FAULT_INJECTION'; Why = 'proves CML_FAULT_INJECTION=OFF'; File = 'cml_broker.exe' },
    @{ Text = 'built without CML_FAULT_INJECTION'; Why = 'proves CML_FAULT_INJECTION=OFF'; File = 'cml_agent.dll' }
)
$latin1 = [Text.Encoding]::GetEncoding(28591)
$binaries = @(Get-ChildItem -Recurse -File -Path $stage | Where-Object { $_.Extension -in '.exe', '.dll' })
foreach ($b in $binaries) {
    $text = $latin1.GetString([IO.File]::ReadAllBytes($b.FullName))
    foreach ($entry in $banned) {
        if ($entry.Only -and $entry.Only -ne $b.Name) { continue }
        if ($text.IndexOf($entry.Text, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            Fail "$($b.FullName.Substring($stage.Length + 1)) contains '$($entry.Text)' ($($entry.Why))"
        }
    }
    foreach ($entry in $expected) {
        if ($entry.File -eq $b.Name -and $text.IndexOf($entry.Text, [StringComparison]::Ordinal) -lt 0) {
            Fail "$($b.FullName.Substring($stage.Length + 1)) lacks '$($entry.Text)' ($($entry.Why))"
        }
    }
}
Write-Host "WP32-CHECK banned-strings=ok binaries=$($binaries.Count) patterns=$($banned.Count + $expected.Count)"

# The release build itself must have been configured as promised.
$cache = Join-Path $build 'CMakeCache.txt'
$wanted = @('CML_FAULT_INJECTION:BOOL=OFF', 'CML_BUILD_TESTS:BOOL=OFF')
if ($generator[1] -eq 'Ninja') { $wanted += 'CMAKE_BUILD_TYPE:STRING=RelWithDebInfo' }
foreach ($want in $wanted) {
    if (-not (Select-String -Path $cache -SimpleMatch -Pattern $want -Quiet)) { Fail "CMakeCache.txt of the release build lacks $want" }
}
Write-Host 'WP32-CHECK build-options=ok (RelWithDebInfo, CML_FAULT_INJECTION=OFF, CML_BUILD_TESTS=OFF)'

if ($Strict -and $script:pending.Count -gt 0) { Fail ("-Strict and parts are pending: " + ($script:pending -join ', ')) }

# ---- manifest ------------------------------------------------------------------------------------------------------
$manifestPath = Join-Path $stage 'MANIFEST.txt'
if (Test-Path $manifestPath) { Remove-Item $manifestPath }
$entries = foreach ($f in (Get-ChildItem -Recurse -File -Path $stage | Sort-Object FullName)) {
    $rel = $f.FullName.Substring($stage.Length + 1).Replace('\', '/')
    '{0}  {1}' -f (Get-FileHash -Algorithm SHA256 $f.FullName).Hash.ToLowerInvariant(), $rel
}
$header = @("# Chameleon $Version (win64): SHA-256 of every file in this package (MANIFEST.txt itself excluded)")
[IO.File]::WriteAllLines($manifestPath, [string[]]($header + @($entries)), (New-Object Text.UTF8Encoding($false)))
Write-Host "WP32-CHECK manifest=ok files=$(@($entries).Count)"

# ---- zip (forward-slash entry names, whatever the .NET version) ----------------------------------------------------
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in (Get-ChildItem -Recurse -File -Path $stage | Sort-Object FullName)) {
        $rel = $f.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, "$name/$rel", [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $archive.Dispose() }

$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
$size = [math]::Round((Get-Item $zip).Length / 1MB, 2)
Write-Host "WP32-PACKAGE zip=$zip size_mb=$size sha256=$hash pending=$($script:pending.Count)"
if ($script:pending.Count -gt 0) { Write-Host ("WP32-PACKAGE pending-parts=" + ($script:pending -join ',')) }
