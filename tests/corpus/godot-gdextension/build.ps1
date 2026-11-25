# Dot-sourced by tests/corpus/run_corpus.ps1 (variables: $src $buildDir $Compiler $Config $CxxPath $repo $here $out $WorkDir $cfg).
# WP-42: a DLL-plugin architecture. The host is the Godot 4.5.1 editor binary of the cell `godot / msvc` (same checkout
# `src-godot`, same patch, same build script, same cache); the plugin is a GDExtension built against godot-cpp (MIT,
# pinned in project.json) that the host loads at run time with LoadLibraryExW, long after the agent sent its first
# module list.
#
#   1. host: copies chameleon_plugin_host.inc next to the support file chameleon_corpus.cpp of the checkout and appends
#      an #include of it (two exports for the plugin: the host's agent and its marker list), then runs
#      tests/corpus/godot/build.ps1 unchanged. scons recompiles that one file and relinks the editor; the cache is
#      not saved by this cell, so the cell `godot / msvc` never sees the change.
#   2. godot-cpp: git clone of the pinned tag, commit checked.
#   3. plugin: tests/corpus/godot-gdextension/plugin copied to <WorkDir>/gdext, Chameleon_API.h added, scons with the
#      flags of docs/BUILD_REQUIREMENTS.md (plugin/SConstruct); output gdext/project/bin/corpus_plugin.dll.
#
# Licensing: godot-cpp is MIT (LICENSE.md at the tag); it vendors no third-party code. Godot as in godot/build.ps1.
if ($Compiler -ne 'msvc') { throw 'the godot-gdextension corpus runs with msvc only (its host, the Godot editor, is built with cl only)' }
$projDir = Join-Path $here 'godot-gdextension'

# 1. the host
$hostDir = Join-Path $src 'platform/windows'
Copy-Item (Join-Path $projDir 'chameleon_plugin_host.inc') $hostDir -Force
$supportCpp = Join-Path $hostDir 'chameleon_corpus.cpp'
$supportText = [System.IO.File]::ReadAllText($supportCpp)
if (-not $supportText.Contains('chameleon_plugin_host.inc')) {
    [System.IO.File]::AppendAllText($supportCpp, "`n// WP-42 (tests/corpus/godot-gdextension/build.ps1): exports for the corpus plugin.`n#include `"chameleon_plugin_host.inc`"`n")
}
$watch = [System.Diagnostics.Stopwatch]::StartNew()
. (Join-Path $here 'godot/build.ps1')
$hostExe = $script:ExePath
Write-Host ("host build (scons, Godot editor): {0:N0} s" -f $watch.Elapsed.TotalSeconds)
$exports = & dumpbin /exports $hostExe 2>&1 | Out-String
if ($exports -notmatch 'cml_corpus_host_agent' -or $exports -notmatch 'cml_corpus_host_marker') {
    throw "the host $hostExe does not export cml_corpus_host_agent / cml_corpus_host_marker"
}

# 2. godot-cpp
$gc = $cfg.godot_cpp
$gcppDir = Join-Path $WorkDir 'godot-cpp'
$watch.Restart()
if (-not (Test-Path (Join-Path $gcppDir 'SConstruct'))) {
    if (Test-Path $gcppDir) { Remove-Item -Recurse -Force $gcppDir }
    Invoke-Native 'git clone godot-cpp' 'git' @('-c', 'core.autocrlf=false', 'clone', '--quiet', '--depth', '1', '--branch', $gc.tag, $gc.repo, $gcppDir)
}
$gcHead = (& git -C $gcppDir rev-parse HEAD).Trim()
if ($gcHead -ne $gc.commit) { throw "godot-cpp pinned commit mismatch: $($gc.tag) is $gcHead, expected $($gc.commit)" }

# 3. the plugin
$pluginDir = Join-Path $WorkDir 'gdext'
if (Test-Path $pluginDir) { Remove-Item -Recurse -Force $pluginDir }
Copy-Item -Recurse (Join-Path $projDir 'plugin') $pluginDir
Copy-Item (Join-Path $repo 'agent/include/Chameleon_API.h') (Join-Path $pluginDir 'src') -Force
$env:CML_GODOT_CPP_DIR = $gcppDir
Push-Location $pluginDir
try {
    Invoke-Native 'scons (plugin + godot-cpp)' 'scons' @('platform=windows', 'target=template_debug', 'arch=x86_64', 'debug_symbols=yes', "-j$jobs") (Join-Path $out 'plugin-build.log')
} finally {
    Pop-Location
}
$dll = Join-Path $pluginDir 'project/bin/corpus_plugin.dll'
if (-not (Test-Path $dll)) { throw "the plugin build did not produce $dll" }
if (-not (Test-Path ([System.IO.Path]::ChangeExtension($dll, '.pdb')))) { throw "no PDB next to $dll" }
Write-Host ("plugin build (godot-cpp $($gc.tag) + corpus_plugin): {0:N0} s, {1:N1} MB" -f $watch.Elapsed.TotalSeconds, ((Get-Item $dll).Length / 1MB))
$script:ExtraModules = @([pscustomobject]@{ path = $dll; min_functions = [int64]$cfg.plugin_min_functions })
Remove-Item (Join-Path $WorkDir 'gdext_ctl.txt') -ErrorAction SilentlyContinue   # the plugin is loaded only when the driver says so
$script:ExePath = $hostExe
