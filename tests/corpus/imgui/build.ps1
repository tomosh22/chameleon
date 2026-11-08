# Dot-sourced by tests/corpus/run_corpus.ps1 (variables: $src $buildDir $Compiler $Config $CxxPath $repo $here $out $WorkDir).
# Builds examples/example_win32_directx11 of Dear ImGui with the flags of tests/corpus/common/CorpusFlags.cmake.
# The example falls back to the WARP software rasteriser when there is no GPU (the patch widens that fallback).
$cc = if ($CxxPath) { $CxxPath } elseif ($Compiler -eq 'clang-cl') { 'clang-cl' } else { 'cl' }
$cc = $cc.Replace('\', '/')
Invoke-Native 'cmake configure' 'cmake' @('-S', (Join-Path $here 'imgui'), '-B', $buildDir, '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$Config", "-DCMAKE_CXX_COMPILER=$cc",
    "-DIMGUI_DIR=$($src.Replace('\', '/'))", "-DCML_COMMON_DIR=$((Join-Path $here 'common').Replace('\', '/'))") (Join-Path $out 'build.log')
Invoke-Native 'cmake build' 'cmake' @('--build', $buildDir, '--parallel') (Join-Path $out 'build.log')
$script:ExePath = (Get-ChildItem $buildDir -Recurse -Filter 'imgui_dx11.exe' | Select-Object -First 1).FullName
