# WP-05: what does clang-cl <version> record in the PDB, and does the recorded command replay?
#
# Builds fixture `basic` with the given clang-cl (/Zi, then link /DEBUG:FULL), once per variant:
#   default                  no CodeView command-line switch
#   explicit-command-line    /clang:-gcodeview-command-line
#   no-command-line          /clang:-gno-codeview-command-line
# and prints, per variant, whether cml_pdb finds S_ENVBLOCK / S_BUILDINFO and what the command is.
# For the default variant it also runs the cc1 replay check (cml_tc_probe recheck), the prototype of
# option (b) (--translate-cc1) and the clang-cl driver-form compile. Result lines start with TABLE.
#
#   clang_version_probe.ps1 -Clang <clang-cl.exe> -BinDir <build/bin> -Repo <repo root> -Work <dir> [-ExpectBuildInfo yes|no|any]
param(
    [Parameter(Mandatory)][string]$Clang,
    [Parameter(Mandatory)][string]$BinDir,
    [Parameter(Mandatory)][string]$Repo,
    [Parameter(Mandatory)][string]$Work,
    [string]$ExpectBuildInfo = 'any'
)
$ErrorActionPreference = 'Continue'
$dump = Join-Path $BinDir 'cml_pdb_dump.exe'
$probe = Join-Path $BinDir 'cml_tc_probe.exe'
$versionText = (& $Clang --version | Out-String).Trim()
Write-Host $versionText
$null = $versionText -match 'clang version (\d+\.\d+\.\d+)'
$ver = $Matches[1]
$sources = 'main.cpp', 'logic.cpp', 'extras.cpp' | ForEach-Object { Join-Path $Repo "tests/fixtures/basic/$_" }
$variants = @(
    @{ name = 'default'; flags = @() },
    @{ name = 'explicit-command-line'; flags = @('/clang:-gcodeview-command-line') },
    @{ name = 'no-command-line'; flags = @('/clang:-gno-codeview-command-line') }
)
$failed = $false
foreach ($v in $variants) {
    $dir = Join-Path $Work $v.name
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Write-Host "::group::clang-cl $ver variant $($v.name)"
    # _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH: the MSVC STL of the runner (14.44+) refuses clang older than 19 (STL1000).
    $compileArgs = @('/nologo', '/Zi', '/Od', '/EHsc', '/Gy', '/hotpatch', '/D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH', '/c', "/Fo$dir\", "/I$Repo\agent\include") + $v.flags + $sources
    & $Clang @compileArgs 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Host "TABLE llvm=$ver variant=$($v.name) compile=FAILED"; $failed = $true; Write-Host '::endgroup::'; continue }
    $exe = Join-Path $dir 'basic.exe'
    & link /nologo /DEBUG:FULL /INCREMENTAL:NO /OUT:$exe (Get-ChildItem $dir -Filter *.obj | ForEach-Object { $_.FullName }) 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Host "TABLE llvm=$ver variant=$($v.name) link=FAILED"; $failed = $true; Write-Host '::endgroup::'; continue }
    $text = (& $dump $exe --compiland logic 2>&1 | Out-String)
    Write-Host $text
    $envblock = if ($text -match 'S_ENVBLOCK: present') { 'present' } else { 'absent' }
    $source = if ($text -match 'build_env source: (\w+)') { $Matches[1] } else { 'unknown' }
    $cmd = if ($text -match '(?m)^\s+cmd = (.*)$') { $Matches[1] } else { '' }
    $cmdHead = if ($cmd.Length -gt 90) { $cmd.Substring(0, 90) } else { $cmd }
    $hasCc1 = $cmd -match '^\s*"?-cc1"?'
    Write-Host "TABLE llvm=$ver variant=$($v.name) S_ENVBLOCK=$envblock build_env_source=$source command_is_cc1=$hasCc1 command_head=[$cmdHead]"
    if ($v.name -eq 'default') {
        if ($ExpectBuildInfo -eq 'yes' -and $source -ne 'BuildInfo') { Write-Host "::error::clang-cl $ver default variant did not record S_BUILDINFO"; $failed = $true }
        if ($ExpectBuildInfo -eq 'no' -and $source -eq 'BuildInfo') { Write-Host "::error::clang-cl $ver unexpectedly recorded S_BUILDINFO"; $failed = $true }
        & $probe commands $exe --obj-dir $dir 2>&1 | Out-Host
        & $probe recheck $exe --obj-dir $dir --keep (Join-Path $dir 'replay') 2>&1 | Out-Host
        $rc = $LASTEXITCODE
        Write-Host "TABLE llvm=$ver variant=default cc1_replay_recheck_exit=$rc"
        if ($source -eq 'BuildInfo' -and $rc -ne 0) { $failed = $true }
        & $probe recheck $exe --obj-dir $dir --translate-cc1 --keep (Join-Path $dir 'translate') 2>&1 | Out-Host
        Write-Host "TABLE llvm=$ver variant=default translate_cc1_recheck_exit=$LASTEXITCODE"
        & $probe driver-compile --compiler $Clang --source (Join-Path $Repo 'tests/fixtures/basic/logic.cpp') --define _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH --out (Join-Path $dir 'driver') --include (Join-Path $Repo 'agent/include') 2>&1 | Out-Host
        Write-Host "TABLE llvm=$ver variant=default driver_form_compile_exit=$LASTEXITCODE"
    }
    Write-Host '::endgroup::'
}
if ($failed) { exit 1 }
exit 0
