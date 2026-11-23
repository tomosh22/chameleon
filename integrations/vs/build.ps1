<#
.SYNOPSIS
  Builds the Chameleon Visual Studio extension and writes integrations/vs/out/*.vsix (the ADR-0038 contract).

.DESCRIPTION
  1. Chameleon.Control (the plain .NET logic), its unit tests and the integration test: `dotnet build`.
  2. The VSIX project: Visual Studio's MSBuild (the VSIX targets need it, `dotnet build` cannot build a VSIX).
  3. The .vsix is copied to out/.
  It does not run the tests: CI runs them next (`dotnet test tests/Chameleon.Control.Tests`, then the integration test).

.PARAMETER Configuration
  Debug or Release (default Release).
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
# A developer prompt (vcvars, msvc-dev-cmd) sets Platform=x64, which moves managed outputs to bin/x64/ and breaks the VSIX project.
$env:Platform = 'AnyCPU'
$root = $PSScriptRoot
$out = Join-Path $root 'out'

function Invoke-Checked {
    param([string]$What, [scriptblock]$Command)
    Write-Host "==> $What"
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$What failed with exit code $LASTEXITCODE" }
}

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        # The newest installed Visual Studio that has MSBuild (2022 on windows-2022, 2026 on the newer image).
        $found = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        if ($found) { return $found }
    }
    $cmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw 'MSBuild from Visual Studio was not found (install the "Visual Studio extension development" workload)'
}

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force -Path $out | Out-Null

Invoke-Checked 'dotnet build Chameleon.Control' { dotnet build (Join-Path $root 'src/Chameleon.Control/Chameleon.Control.csproj') -c $Configuration -nologo -v:minimal }
Invoke-Checked 'dotnet build unit tests' { dotnet build (Join-Path $root 'tests/Chameleon.Control.Tests/Chameleon.Control.Tests.csproj') -c $Configuration -nologo -v:minimal }
Invoke-Checked 'dotnet build integration test' { dotnet build (Join-Path $root 'tests/Chameleon.IntegrationTest/Chameleon.IntegrationTest.csproj') -c $Configuration -nologo -v:minimal }

$msbuild = Find-MSBuild
Write-Host "MSBuild: $msbuild"
$vsixProject = Join-Path $root 'src/Chameleon.Vsix/Chameleon.Vsix.csproj'
Invoke-Checked 'msbuild Chameleon.Vsix' {
    & $msbuild $vsixProject -restore "-p:Configuration=$Configuration" '-p:Platform=AnyCPU' '-p:DeployExtension=false' -nologo -v:minimal "-bl:$(Join-Path $out 'vsix.binlog')"
}

$vsix = Get-ChildItem -Path (Join-Path $root "src/Chameleon.Vsix/bin/$Configuration") -Filter '*.vsix' -File -ErrorAction SilentlyContinue
if (-not $vsix) { throw "the VSIX build produced no .vsix under src/Chameleon.Vsix/bin/$Configuration" }
foreach ($f in $vsix) {
    Copy-Item $f.FullName -Destination $out
    Write-Host ("VSIX: {0} ({1:N0} bytes)" -f (Join-Path $out $f.Name), $f.Length)
}
