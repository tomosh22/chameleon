# WP-29 CI driver: installs Chameleon from a finished build, checks the install tree, builds the example projects against
# it with find_package(Chameleon CONFIG REQUIRED) + chameleon_enable(), and runs `cml_broker check` on every binary.
#
#   ./integrations/cmake/tests/run_examples.ps1 -BuildDir build -Work wp29-work -Config Debug -Compiler msvc -Os windows-2022 `
#       [-Generator Ninja] [-Cxx <path of cl or clang-cl>]
#
# Prints, per example: `WP29-EXAMPLE <name> <compiler> <config> check=PASS warn=0 modules=<n> os=<os>`; and for the install
# tree `WP29-INSTALL layout=OK relocatable=OK`. Exits 1 on a missing file, an absolute build path in the exported files, a
# failed build, a FAIL, or any WARN.
param(
    [Parameter(Mandatory)] [string]$BuildDir,
    [Parameter(Mandatory)] [string]$Work,
    [Parameter(Mandatory)] [string]$Config,
    [Parameter(Mandatory)] [string]$Compiler,
    [string]$Os = '',
    [string]$Generator = 'Ninja',
    [string]$Cxx = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$build = (Resolve-Path $BuildDir).Path
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Work = (Resolve-Path $Work).Path
$failed = $false

# ---- install, check the layout -------------------------------------------------------------------------------------
$stage = Join-Path $Work 'stage'
cmake --install $build --prefix $stage --config $Config
if ($LASTEXITCODE -ne 0) { throw "cmake --install failed ($LASTEXITCODE)" }
$required = @('bin/cml_broker.exe', 'bin/cml_agent.dll', 'include/Chameleon_API.h', 'lib/cml_agent.lib',
              'lib/cmake/Chameleon/ChameleonConfig.cmake', 'lib/cmake/Chameleon/ChameleonConfigVersion.cmake',
              'lib/cmake/Chameleon/ChameleonTargets.cmake', 'lib/cmake/Chameleon/ChameleonEnable.cmake')
foreach ($f in $required) {
    if (-not (Test-Path (Join-Path $stage $f))) { Write-Host "WP29-INSTALL missing $f"; $failed = $true }
}
Write-Host "WP29-INSTALL files:"
Get-ChildItem -Recurse -File $stage | ForEach-Object { '  ' + $_.FullName.Substring($stage.Length + 1).Replace('\', '/') }
$ui = Test-Path (Join-Path $stage 'bin/cml_ui.exe')
Write-Host "WP29-INSTALL cml_ui.exe present=$ui"

# ---- relocatable: no absolute build or source path in the exported files; then use a moved copy -----------------------
$absent = $true
foreach ($p in @($repo, $build, $stage)) {
    foreach ($form in @($p, $p.Replace('\', '/'))) {
        $hits = Get-ChildItem -Recurse -File (Join-Path $stage 'lib/cmake') | Select-String -SimpleMatch -Pattern $form
        if ($hits) { Write-Host "WP29-INSTALL absolute path '$form' in $($hits[0].Path)"; $absent = $false }
    }
}
if (-not $absent) { $failed = $true }
$prefix = Join-Path $Work 'moved/chameleon-install'
New-Item -ItemType Directory -Force -Path (Split-Path $prefix) | Out-Null
Move-Item $stage $prefix
if (-not $failed) { Write-Host 'WP29-INSTALL layout=OK relocatable=OK' }
$broker = Join-Path $prefix 'bin/cml_broker.exe'

# ---- the examples --------------------------------------------------------------------------------------------------
foreach ($name in @('minimal', 'exe_plus_dll')) {
    $src = Join-Path $repo "integrations/cmake/examples/$name"
    $bdir = Join-Path $Work "build-$name"
    $args1 = @('-S', $src, '-B', $bdir, '-G', $Generator, "-DCMAKE_PREFIX_PATH=$prefix")
    if ($Generator -like 'Visual Studio*') { $args1 += @('-A', 'x64') }
    else { $args1 += @("-DCMAKE_BUILD_TYPE=$Config"); if ($Cxx) { $args1 += "-DCMAKE_CXX_COMPILER=$Cxx" } }
    cmake @args1
    if ($LASTEXITCODE -ne 0) { Write-Host "WP29-EXAMPLE $name $Compiler $Config check=FAIL configure"; $failed = $true; continue }
    cmake --build $bdir --config $Config
    if ($LASTEXITCODE -ne 0) { Write-Host "WP29-EXAMPLE $name $Compiler $Config check=FAIL build"; $failed = $true; continue }

    $modules = Get-ChildItem -Recurse -File $bdir -Include *.exe, *.dll |
        Where-Object { $_.FullName -notmatch 'CMakeFiles' -and $_.Name -ne 'cml_agent.dll' }
    $warn = 0; $fail = 0
    foreach ($m in $modules) {
        $out = (& $broker check --verbose $m.FullName *>&1 | Out-String)
        $rc = $LASTEXITCODE
        Write-Host "== $($m.Name) (exit code $rc) =="
        Write-Host $out
        $warn += [regex]::Matches($out, '(?m)^\s+WARN CML\d+').Count
        $fail += [regex]::Matches($out, '(?m)^\s+FAIL CML\d+').Count
        if ($rc -ne 0) { $fail++ }
        if ($m.Extension -eq '.exe' -and -not (Test-Path (Join-Path $m.DirectoryName 'cml_agent.dll'))) {
            Write-Host "  cml_agent.dll was not copied next to $($m.Name)"; $fail++
        }
    }
    $verdict = if ($fail -eq 0 -and @($modules).Count -gt 0) { 'PASS' } else { 'FAIL' }
    Write-Host "WP29-EXAMPLE $name $Compiler $Config check=$verdict warn=$warn modules=$(@($modules).Count) os=$Os"
    if ($verdict -ne 'PASS' -or $warn -ne 0) { $failed = $true }
}
if ($failed) { exit 1 }
