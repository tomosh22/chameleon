# Dot-sourced by tests/corpus/run_corpus.ps1 (variables: $src $buildDir $Compiler $Config $CxxPath $repo $here $out $WorkDir).
# Builds the Godot 4.3 editor (MIT) with scons and cl (RelWithDebInfo equivalent: /O2 /Zi, /DEBUG:FULL; the patch adds
# /Gy /hotpatch /Ob0 and /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN, the flags of docs/BUILD_REQUIREMENTS.md).
#
# Licensing: Godot's thirdparty tree is permissive, with two exceptions that this build leaves out:
#   * thirdparty/graphite (module text_server_adv) is offered under LGPL-2.1+/MPL-2.0/GPL-2+: module disabled, the
#     fallback text server (text_server_fb) is used;
#   * the Bison skeleton in glslang's generated parser is GPL-3 with the Bison exception: module glslang disabled.
# Dual-licensed components with a permissive option (zstd BSD/GPLv2, FreeType FTL/GPLv2) are used under that option.
if ($Compiler -ne 'msvc') { throw 'the Godot corpus runs with msvc only (plan: Godot msvc RelWithDebInfo only)' }
Invoke-Native 'pip' 'python' @('-m', 'pip', 'install', '--quiet', 'scons==4.8.1')
$jobs = [Math]::Max(2, [int]$env:NUMBER_OF_PROCESSORS)
$sconsArgs = @(
    'platform=windows', 'target=editor', 'arch=x86_64', 'debug_symbols=yes', 'optimize=speed_trace',
    'vulkan=no', 'd3d12=no', 'opengl3=yes', 'progress=no', 'tests=no',
    'module_text_server_adv_enabled=no', 'module_text_server_fb_enabled=yes', 'module_glslang_enabled=no',
    'module_lightmapper_rd_enabled=no', 'module_camera_enabled=no', 'module_webrtc_enabled=no', 'module_websocket_enabled=no',
    'module_upnp_enabled=no', 'module_openxr_enabled=no', 'module_mobile_vr_enabled=no', 'module_webxr_enabled=no',
    'module_vhacd_enabled=no', 'module_mono_enabled=no', "-j$jobs")
Push-Location $src
try {
    Invoke-Native 'scons' 'scons' $sconsArgs (Join-Path $out 'build.log')
} finally {
    Pop-Location
}
$script:ExePath = Join-Path $src 'bin/godot.windows.editor.x86_64.exe'

# A minimal project for the headless editor.
$proj = Join-Path $WorkDir 'godot_project'
New-Item -ItemType Directory -Force -Path $proj | Out-Null
Set-Content -Path (Join-Path $proj 'project.godot') -Value "config_version=5`n`n[application]`n`nconfig/name=`"chameleon-corpus`"`n"
