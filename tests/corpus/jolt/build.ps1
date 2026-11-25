# Dot-sourced by tests/corpus/run_corpus.ps1 (variables: $src $buildDir $Compiler $Config $CxxPath $repo $here $out $WorkDir).
# Builds Jolt Physics v5.7.0 (MIT) and its HelloWorld with tests/corpus/jolt/CMakeLists.txt and the flags of
# tests/corpus/common/CorpusFlags.cmake, then checks the three properties this corpus project exists for (ADR-0043, WP-41)
# in the build tree, so that a change of Jolt or of CMake that drops one fails the build step instead of passing unnoticed:
#   /arch:AVX2  on every compile command of compile_commands.json
#   unity       Jolt's sources are compiled through CMake's unity sources only (no Jolt .cpp compiled on its own)
#   static      Jolt.lib and jolt_corpus_support.lib are static libraries, linked into jolt_hello.exe
# Licensing: everything built is MIT (Jolt) or CC0-1.0 (HelloWorld.cpp); Jolt has no third-party code. Nothing is downloaded
# besides the pinned checkout.
$cc = if ($CxxPath) { $CxxPath } elseif ($Compiler -eq 'clang-cl') { 'clang-cl' } else { 'cl' }
$cc = $cc.Replace('\', '/')
Write-Host ("runner: {0} logical processors" -f [Environment]::ProcessorCount)
Invoke-Native 'cmake configure' 'cmake' @('-S', (Join-Path $here 'jolt'), '-B', $buildDir, '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$Config", "-DCMAKE_CXX_COMPILER=$cc",
    "-DJOLT_DIR=$($src.Replace('\', '/'))", "-DCML_COMMON_DIR=$((Join-Path $here 'common').Replace('\', '/'))") (Join-Path $out 'build.log')
$buildWatch = [System.Diagnostics.Stopwatch]::StartNew()
Invoke-Native 'cmake build' 'cmake' @('--build', $buildDir, '--parallel') (Join-Path $out 'build.log')
$buildSeconds = $buildWatch.Elapsed.TotalSeconds
$script:ExePath = (Get-ChildItem $buildDir -Recurse -Filter 'jolt_hello.exe' | Select-Object -First 1).FullName

$db = @(Get-Content (Join-Path $buildDir 'compile_commands.json') -Raw | ConvertFrom-Json)
if ($db.Count -eq 0) { throw 'compile_commands.json lists no compile command' }
$noAvx2 = @($db | Where-Object { ([string]$_.command) -cnotmatch '(^|\s)[/-]arch:AVX2(\s|$)' })
if ($noAvx2.Count -gt 0) { throw "compile commands without /arch:AVX2: $(@($noAvx2 | ForEach-Object { $_.file }) -join ', ')" }
$unity = @($db | Where-Object { ([string]$_.file) -cmatch '[\\/]Jolt\.dir[\\/]Unity[\\/]unity_\d+_cxx\.cxx$' })
$loose = @($db | Where-Object { ([string]$_.file) -cmatch '[\\/]Jolt[\\/].*\.cpp$' })
if ($unity.Count -lt 10) { throw "only $($unity.Count) unity sources of the Jolt library in compile_commands.json" }
if ($loose.Count -gt 0) { throw "Jolt sources compiled outside a unity batch: $(@($loose | ForEach-Object { $_.file }) -join ', ')" }
$batches = 0
foreach ($u in $unity) {
    $members = @(Select-String -Path $u.file -Pattern '^#include "(.*)"' | ForEach-Object { $_.Matches[0].Groups[1].Value })
    $batches++
    Write-Host ("unity batch {0}: {1}" -f (Split-Path $u.file -Leaf), (@($members | ForEach-Object { $_ -replace '^.*/Jolt/', 'Jolt/' }) -join ' '))
}
foreach ($lib in 'jolt/Jolt.lib', 'jolt_corpus_support.lib') {
    $p = Join-Path $buildDir $lib
    if (-not (Test-Path $p)) { throw "static library $lib was not built" }
}
Write-Host ("JOLT-BUILD compiler={0} build_s={1:N0} processors={2} compile_commands={3} jolt_unity_batches={4} arch=AVX2 static_libs=Jolt.lib,jolt_corpus_support.lib" -f `
        $Compiler, $buildSeconds, [Environment]::ProcessorCount, $db.Count, $batches)
