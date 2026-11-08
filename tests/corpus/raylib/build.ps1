# Dot-sourced by tests/corpus/run_corpus.ps1 (variables: $src $buildDir $Compiler $Config $CxxPath $repo $here $out $WorkDir).
# Builds raylib 5.5 and its core_2d_camera_platformer example with the flags of tests/corpus/common/CorpusFlags.cmake,
# then puts a software OpenGL next to the exe: raylib needs OpenGL 3.3 and a CI runner has no GPU, so Mesa's llvmpipe
# opengl32.dll (mesa-dist-win, MIT-licensed Mesa build, pinned) takes the place of the system opengl32.dll.
$cc = if ($CxxPath) { $CxxPath } elseif ($Compiler -eq 'clang-cl') { 'clang-cl' } else { 'cl' }
$cc = $cc.Replace('\', '/')
Invoke-Native 'cmake configure' 'cmake' @('-S', (Join-Path $here 'raylib'), '-B', $buildDir, '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$Config", "-DCMAKE_C_COMPILER=$cc", "-DCMAKE_CXX_COMPILER=$cc",
    "-DRAYLIB_DIR=$($src.Replace('\', '/'))", "-DCML_COMMON_DIR=$((Join-Path $here 'common').Replace('\', '/'))") (Join-Path $out 'build.log')
Invoke-Native 'cmake build' 'cmake' @('--build', $buildDir, '--parallel') (Join-Path $out 'build.log')
$script:ExePath = (Get-ChildItem $buildDir -Recurse -Filter 'raylib_game.exe' | Select-Object -First 1).FullName

$mesaVersion = '24.3.4'
$mesaDir = Join-Path $WorkDir "mesa-$mesaVersion"
if (-not (Test-Path (Join-Path $mesaDir 'x64/opengl32.dll'))) {
    New-Item -ItemType Directory -Force -Path $mesaDir | Out-Null
    $archive = Join-Path $WorkDir "mesa-$mesaVersion.7z"
    $url = "https://github.com/pal1000/mesa-dist-win/releases/download/$mesaVersion/mesa3d-$mesaVersion-release-msvc.7z"
    Write-Host "downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $archive
    Invoke-Native '7z' '7z' @('x', $archive, "-o$mesaDir", '-y') (Join-Path $out 'mesa.log')
}
foreach ($dll in 'opengl32.dll', 'libgallium_wgl.dll', 'libglapi.dll') {
    $f = Join-Path $mesaDir "x64/$dll"
    if (Test-Path $f) { Copy-Item $f (Split-Path $script:ExePath) -Force } elseif ($dll -eq 'opengl32.dll') { throw "mesa archive has no x64/opengl32.dll" }
}
