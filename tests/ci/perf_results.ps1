# WP-27: collects the numbers of the `pdb-benchmark` job into perf-results.json, writes them to the step summary and
# compares them with the previous successful run on main (a regression of more than 25 % prints a ::warning:: line; only the
# absolute budgets, asserted by the measuring steps themselves, fail the job).
#
#   perf_results.ps1 -PdbJson f -MemJson f -ReloadJson f -LargeReloadJson f -Out perf-results.json [-PreviousDir dir] [-Sha s]
#                    [-Runner r] [-RunId n]
#
# Inputs are the JSON files of cml_pdb_bench --json, cml_broker_mem --json, the `perf` scenario (perf-reload.json) and, since
# WP-39, the `large_pdb_reload` scenario (the reloads inside the 544 MiB PDB of the generated project); any of them may be
# missing (that measuring step failed): the metric is then absent and the table says so.
param(
    [string]$PdbJson = '',
    [string]$MemJson = '',
    [string]$ReloadJson = '',
    [string]$LargeReloadJson = '',
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$PreviousDir = '',
    [string]$Sha = '',
    [string]$Runner = '',
    [string]$RunId = ''
)
$ErrorActionPreference = 'Stop'

function Read-Json([string]$path) {
    if ($path -and (Test-Path $path)) { return (Get-Content $path -Raw | ConvertFrom-Json) }
    return $null
}

$pdb = Read-Json $PdbJson
$mem = Read-Json $MemJson
$rel = Read-Json $ReloadJson
$big = Read-Json $LargeReloadJson

# Flat metrics: name -> value, the unit and the smallest change worth a warning (noise floor).
$metrics = [ordered]@{}
if ($pdb) { $metrics['pdb_index_ms_per_500mb'] = [double]$pdb.ms_per_500mb }
if ($rel) {
    $metrics['reload_median_excl_compile_ms'] = [double]$rel.reload_median_excl_compile_ms
    $metrics['reload_max_excl_compile_ms'] = [double]$rel.reload_max_excl_compile_ms
    if ($null -ne $rel.broker.idle_cpu_percent) { $metrics['broker_idle_cpu_percent'] = [double]$rel.broker.idle_cpu_percent }
    if ($null -ne $rel.broker.peak_private_bytes) { $metrics['broker_session_peak_private_mib'] = [math]::Round([double]$rel.broker.peak_private_bytes / 1MB, 1) }
}
if ($mem) { $metrics['broker_peak_private_over_pdb'] = [double]$mem.peak_private_over_pdb }
if ($big) {
    # WP-39: the same figures for the reloads inside the large PDB, and the real broker's peak during them against its limit.
    $metrics['large_pdb_reload_median_excl_compile_ms'] = [double]$big.reload_median_excl_compile_ms
    $metrics['large_pdb_reload_max_excl_compile_ms'] = [double]$big.reload_max_excl_compile_ms
    if ($null -ne $big.broker.peak_private_bytes -and $null -ne $big.broker.module_pdb_bytes -and [double]$big.broker.module_pdb_bytes -gt 0) {
        $metrics['large_pdb_broker_peak_private_over_pdb'] = [math]::Round([double]$big.broker.peak_private_bytes / [double]$big.broker.module_pdb_bytes, 3)
    }
}
$floors = @{
    pdb_index_ms_per_500mb = 50.0; reload_median_excl_compile_ms = 30.0; reload_max_excl_compile_ms = 60.0
    broker_idle_cpu_percent = 0.05; broker_session_peak_private_mib = 5.0; broker_peak_private_over_pdb = 0.02
    large_pdb_reload_median_excl_compile_ms = 100.0; large_pdb_reload_max_excl_compile_ms = 200.0; large_pdb_broker_peak_private_over_pdb = 0.05
}
# name -> display budget, CI threshold
$limits = [ordered]@{
    pdb_index_ms_per_500mb = @('2000 ms', '4000 ms (2x slack)')
    reload_median_excl_compile_ms = @('1500 ms', '3000 ms (2x slack)')
    reload_max_excl_compile_ms = @('report only', 'report only')
    broker_idle_cpu_percent = @('0.5 %', '1 % (2x slack)')
    broker_session_peak_private_mib = @('report only', 'report only')
    broker_peak_private_over_pdb = @('1.5 x PDB', '1.5 x PDB (no slack)')
    large_pdb_reload_median_excl_compile_ms = @('1500 ms', '3000 ms (2x slack)')
    large_pdb_reload_max_excl_compile_ms = @('report only', 'report only')
    large_pdb_broker_peak_private_over_pdb = @('1.5 x PDB + 32 MiB', '1.5 x PDB + 32 MiB (no slack)')
}

$result = [ordered]@{
    sha = $Sha
    ref = $env:GITHUB_REF
    run_id = $RunId
    runner = $Runner
    image_version = $env:ImageVersion
    generated_utc = (Get-Date).ToUniversalTime().ToString('o')
    metrics = $metrics
    pdb_index = $pdb
    broker_memory = $mem
    reload = $rel
    large_pdb_reload = $big
}
$result | ConvertTo-Json -Depth 12 | Set-Content -Path $Out -Encoding utf8
Write-Host "wrote $Out"

# The previous successful run on main (its perf-results artifact), when the caller downloaded one.
$prev = $null
if ($PreviousDir) {
    $file = Join-Path $PreviousDir 'perf-results.json'
    if (Test-Path $file) { $prev = Read-Json $file }
}

$lines = @()
$lines += '### Performance budgets (WP-27, WP-39)'
$lines += ''
if ($prev) { $lines += "Compared with the last successful run on main (sha $($prev.sha))." } else { $lines += 'No previous successful main run with a perf-results artifact to compare with.' }
$lines += ''
$lines += '| metric | this run | budget | CI threshold | previous main |'
$lines += '|---|---|---|---|---|'
$warnings = @()
foreach ($name in $limits.Keys) {
    $cur = $null
    if ($metrics.Contains($name)) { $cur = $metrics[$name] }
    $before = $null
    if ($prev -and $prev.metrics -and $null -ne $prev.metrics.$name) { $before = [double]$prev.metrics.$name }
    $curText = if ($null -ne $cur) { ('{0:N3}' -f $cur) } else { 'not measured' }
    $beforeText = if ($null -ne $before) { ('{0:N3}' -f $before) } else { '-' }
    $lines += "| $name | $curText | $($limits[$name][0]) | $($limits[$name][1]) | $beforeText |"
    if ($null -ne $cur -and $null -ne $before -and $cur -gt $before * 1.25 -and ($cur - $before) -gt $floors[$name]) {
        $warnings += "perf regression: $name is $('{0:N3}' -f $cur), more than 25 % above $('{0:N3}' -f $before) on the last successful main run"
    }
}
if ($rel) {
    $lines += ''
    $lines += "Reload stages over $($rel.reloads) reloads (ms, median / max); compile is reported and excluded from the budget:"
    $lines += ''
    $lines += '| stage | median | max |'
    $lines += '|---|---|---|'
    foreach ($stage in $rel.stages.PSObject.Properties) {
        $lines += "| $($stage.Name) | $('{0:N1}' -f $stage.Value.median_ms) | $('{0:N1}' -f $stage.Value.max_ms) |"
    }
}
if ($big) {
    $lines += ''
    $lines += "Reload inside the large PDB (WP-39, scenario large_pdb_reload, $('{0:N1}' -f ([double]$big.broker.module_pdb_bytes / 1MB)) MiB PDB), stages over $($big.reloads) reloads (ms, median / max):"
    $lines += ''
    $lines += '```'
    $lines += [string]$big.evidence
    $lines += '```'
    $lines += ''
    $lines += '| stage | median | max |'
    $lines += '|---|---|---|'
    foreach ($stage in $big.stages.PSObject.Properties) {
        $lines += "| $($stage.Name) | $('{0:N1}' -f $stage.Value.median_ms) | $('{0:N1}' -f $stage.Value.max_ms) |"
    }
} else {
    $lines += ''
    $lines += 'Reload inside the large PDB (WP-39): not measured (the scenario large_pdb_reload wrote no result).'
}
foreach ($w in $warnings) { $lines += ''; $lines += "WARNING: $w" }

$lines | ForEach-Object { Write-Host $_ }
if ($env:GITHUB_STEP_SUMMARY) { $lines | Out-File -Append -Encoding utf8 $env:GITHUB_STEP_SUMMARY }
foreach ($w in $warnings) { Write-Host "::warning::$w" }
exit 0
