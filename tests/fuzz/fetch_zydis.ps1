# Fetches Zydis, the reference disassembler of the length decoder's tests (WP-38, ADR-0042), at a pinned commit.
#
#   pwsh tests/fuzz/fetch_zydis.ps1 -Destination <dir>
#
# Zydis (https://github.com/zyantific/zydis, MIT) and its one dependency Zycore (https://github.com/zyantific/zycore-c,
# MIT) are test oracles only: fetched at run time by .github/workflows/fuzz.yml (as the corpus projects are), handed to
# CMake as -DCML_ZYDIS_DIR=<dir>, linked into cml_fuzz_lde and cml_lde_diff and into nothing else. They are never
# vendored into this repository and never shipped. The pins and the licences are recorded in docs/wp/WP-38.md; change
# both places together.
#
# The script fails when the checkout is not exactly the pinned commits.
param(
    [Parameter(Mandatory = $true)][string]$Destination,
    # Zydis v4.1.1
    [string]$ZydisCommit = 'a2278f1d254e492f6a6b39f6cb5d1f5d515659dc',
    # zycore-c v1.5.2, the submodule commit that Zydis commit records
    [string]$ZycoreCommit = '0b2432ced0884fd152b471d97ecf0258ff4d859f'
)
$ErrorActionPreference = 'Stop'

function Invoke-Git {
    & git @args
    if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') failed with exit code $LASTEXITCODE" }
}

if (Test-Path $Destination) { Remove-Item -Recurse -Force $Destination }
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

$attempt = 0
while ($true) {
    $attempt++
    try {
        Invoke-Git -C $Destination init --quiet
        Invoke-Git -C $Destination remote add origin https://github.com/zyantific/zydis.git
        Invoke-Git -C $Destination fetch --quiet --depth 1 origin $ZydisCommit
        Invoke-Git -C $Destination checkout --quiet --detach FETCH_HEAD
        Invoke-Git -C $Destination submodule update --quiet --init --depth 1 dependencies/zycore
        break
    } catch {
        if ($attempt -ge 4) { throw }
        Write-Host "fetch attempt $attempt failed ($($_.Exception.Message)); retrying"
        Start-Sleep -Seconds (10 * $attempt)
        Remove-Item -Recurse -Force $Destination
        New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    }
}

$zydis = (& git -C $Destination rev-parse HEAD).Trim()
$zycore = (& git -C (Join-Path $Destination 'dependencies/zycore') rev-parse HEAD).Trim()
if ($zydis -ne $ZydisCommit) { throw "Zydis is at $zydis, expected the pinned $ZydisCommit" }
if ($zycore -ne $ZycoreCommit) { throw "zycore-c is at $zycore, expected the pinned $ZycoreCommit" }
foreach ($file in 'LICENSE', 'include/Zydis/Zydis.h', 'dependencies/zycore/LICENSE', 'dependencies/zycore/include/Zycore/Types.h') {
    if (-not (Test-Path (Join-Path $Destination $file))) { throw "the Zydis checkout lacks $file" }
}
Write-Host "WP38-ZYDIS zydis=$zydis zycore=$zycore licence=MIT dir=$Destination"
