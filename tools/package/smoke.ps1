<#
.SYNOPSIS
  Smoke test of an UNZIPPED Chameleon package (WP-32): verifies the manifest, builds smoke/ against the package, runs it
  under cml_broker and does one reload. Never touches the build tree of the repository.

.DESCRIPTION
  Run it from an environment where cl.exe works (x64 Native Tools prompt, or ilammy/msvc-dev-cmd in CI).
  Every result is a greppable line:  WP32-SMOKE <what>=<value>.  A failure prints WP32-SMOKE FAIL <reason> and exits 1.

  .\smoke.ps1 -Root C:\path\to\unzipped\chameleon-0.1.0-win64 -Work C:\path\to\scratch
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [string]$Work = (Join-Path ([IO.Path]::GetTempPath()) 'cml-smoke'),
    [int]$ReloadTimeoutMs = 120000
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

function Fail([string]$what) { Write-Host "WP32-SMOKE FAIL $what"; exit 1 }
function Note([string]$what) { Write-Host "WP32-SMOKE $what" }

$Root = (Resolve-Path $Root).Path
if (-not (Test-Path (Join-Path $Root 'bin\cml_broker.exe'))) { Fail "no bin\cml_broker.exe under $Root" }
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Work = (Resolve-Path $Work).Path

# 1. The manifest of the unzipped tree.
$manifest = Join-Path $Root 'MANIFEST.txt'
if (-not (Test-Path $manifest)) { Fail 'MANIFEST.txt is missing' }
$bad = 0; $count = 0
foreach ($line in Get-Content $manifest) {
    if ($line -match '^([0-9a-f]{64})  (.+)$') {
        $count++
        $file = Join-Path $Root $Matches[2]
        if (-not (Test-Path $file) -or (Get-FileHash -Algorithm SHA256 $file).Hash.ToLowerInvariant() -ne $Matches[1]) { $bad++; Write-Host "  mismatch: $($Matches[2])" }
    }
}
if ($bad -ne 0 -or $count -eq 0) { Fail "manifest: $bad of $count files differ or are missing" }
Note "manifest=ok files=$count"

# 2. The tools run.
$broker = Join-Path $Root 'bin\cml_broker.exe'
$version = (& $broker --version) -join ' '
if ($LASTEXITCODE -ne 0) { Fail "cml_broker --version exited with $LASTEXITCODE" }
Note "broker=$version"
if (-not (Test-Path (Join-Path $Root 'bin\cml_agent.dll'))) { Fail 'bin\cml_agent.dll is missing' }
if (-not (Test-Path (Join-Path $Root 'include\Chameleon_API.h'))) { Fail 'include\Chameleon_API.h is missing' }

# 3. Build the example against the package: find_package(Chameleon) when the package has the CMake integration.
$usePackage = Test-Path (Join-Path $Root 'lib\cmake\Chameleon\ChameleonConfig.cmake')
if (-not $usePackage) { Write-Host 'WP32-PENDING find_package(Chameleon): the package has no lib\cmake\Chameleon (WP-29); building the example with manual flags' }
$src = Join-Path $Work 'hello-src'
if (-not (Test-Path (Join-Path $Root 'examples\hello\CMakeLists.txt'))) { Fail 'examples\hello is missing from the package' }
Copy-Item -Recurse (Join-Path $Root 'examples\hello') $src
$build = Join-Path $Work 'smoke-build'
$cfg = @('-S', $src, '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=RelWithDebInfo')
if ($usePackage) { $cfg += @("-DCMAKE_PREFIX_PATH=$Root", '-DSMOKE_USE_PACKAGE=ON') }
else { $cfg += @('-DSMOKE_USE_PACKAGE=OFF', "-DCHAMELEON_ROOT=$Root") }
& cmake @cfg
if ($LASTEXITCODE -ne 0) { Fail "configuring the example failed ($LASTEXITCODE)" }
& cmake --build $build
if ($LASTEXITCODE -ne 0) { Fail "building the example failed ($LASTEXITCODE)" }
$app = Join-Path $build 'smoke_app.exe'
if (-not (Test-Path $app)) { Fail "smoke_app.exe was not produced in $build" }
Note ("build=ok find_package=" + $usePackage.ToString().ToLowerInvariant())

# 4. The build requirements audit of the example (docs/BUILD_REQUIREMENTS.md).
$checkOut = (& $broker check --json $app) -join "`n"
$checkExit = $LASTEXITCODE
Set-Content -Path (Join-Path $Work 'check.json') -Value $checkOut
$plain = (& $broker check $app) -join "`n"
Write-Host $plain
$warn = ([regex]::Matches($plain, '(?m)^\s*WARN\b')).Count
$fail = ([regex]::Matches($plain, '(?m)^\s*FAIL\b')).Count
if ($checkExit -ne 0 -or $fail -ne 0) { Fail "cml_broker check reports $fail FAIL (exit $checkExit)" }
Note "check=ok warn=$warn fail=0"
if ($usePackage -and $warn -ne 0) { Fail "cml_broker check reports $warn WARN on a target built with chameleon_enable()" }

# 5. Broker + application + one reload.
$tag = [Guid]::NewGuid().ToString('N').Substring(0, 12)
$agentPipe = "\\.\pipe\cml_smoke_agent_$tag"
$controlName = "cml_smoke_control_$tag"
$env:CML_PIPE_NAME = $agentPipe
$env:CML_CONTROL_PIPE = "\\.\pipe\$controlName"
$brokerLog = Join-Path $Work 'broker.log'
$appOut = Join-Path $Work 'app.out'
$appErr = Join-Path $Work 'app.err'
$brokerProc = $null; $appProc = $null
function Stop-All {
    foreach ($p in @($script:appProc, $script:brokerProc)) {
        if ($p -and -not $p.HasExited) { try { $p.Kill() } catch { } }
    }
}
function Show-Logs {
    foreach ($f in @($brokerLog, $appOut, $appErr)) {
        if (Test-Path $f) { Write-Host "--- $f (tail) ---"; Get-Content $f -Tail 60 }
    }
}

function Invoke-Control([string]$cmd, $arguments, [int]$timeoutMs) {
    $client = New-Object System.IO.Pipes.NamedPipeClientStream('.', $controlName, [System.IO.Pipes.PipeDirection]::InOut)
    try {
        $client.Connect(15000)
        $utf8 = New-Object System.Text.UTF8Encoding($false)
        $writer = New-Object System.IO.StreamWriter($client, $utf8)
        $writer.NewLine = "`n"; $writer.AutoFlush = $true
        $reader = New-Object System.IO.StreamReader($client, $utf8)
        $request = [ordered]@{ id = 1; cmd = $cmd }
        if ($null -ne $arguments) { $request['args'] = $arguments }
        $writer.WriteLine(($request | ConvertTo-Json -Compress -Depth 6))
        $task = $reader.ReadLineAsync()
        if (-not $task.Wait($timeoutMs)) { throw "no answer to '$cmd' within $timeoutMs ms" }
        return ($task.Result | ConvertFrom-Json)
    } finally { $client.Dispose() }
}

try {
    $brokerProc = Start-Process -FilePath $broker -PassThru -WindowStyle Hidden -ArgumentList @(
        '--headless', '--verbose', '--log', $brokerLog, '--session-dir', (Join-Path $Work 'session'), '--exit-when-idle', '60000')
    $appProc = Start-Process -FilePath $app -PassThru -WindowStyle Hidden -WorkingDirectory $build `
        -RedirectStandardOutput $appOut -RedirectStandardError $appErr -ArgumentList @('--agent-dir', (Join-Path $Root 'bin'))

    # The broker answers `status` as soon as its control pipe is up.
    $status = $null
    for ($i = 0; $i -lt 60 -and -not $status; $i++) {
        try { $status = Invoke-Control 'status' $null 5000 } catch { Start-Sleep -Milliseconds 500 }
    }
    if (-not $status -or -not $status.ok) { Fail 'the broker did not answer on its control pipe' }
    Note ("broker-status=ok version=" + $status.result.version + " capabilities=" + ($status.result.capabilities -join ','))
    if ($status.result.capabilities -notcontains 'reload') { Fail 'the broker does not provide the reload capability' }

    # The application enabled its module (its PDB index is loaded when `wait` for the index returns).
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline -and -not ((Test-Path $appOut) -and (Select-String -Path $appOut -Pattern 'SMOKE enabled=1' -Quiet))) { Start-Sleep -Milliseconds 300 }
    if (-not ((Test-Path $appOut) -and (Select-String -Path $appOut -Pattern 'SMOKE enabled=1' -Quiet))) { Show-Logs; Fail 'the application never reported SMOKE enabled=1' }
    $wait = Invoke-Control 'wait' @{ for = 'index'; timeout_ms = 60000 } 70000
    if (-not $wait.ok) { Show-Logs; Fail ("wait for the index failed: " + ($wait.diag | ConvertTo-Json -Compress)) }
    if (-not (Select-String -Path $appOut -Pattern 'SMOKE tick=\d+ value=1' -Quiet)) { Fail 'the application did not print value=1' }
    Note 'before=value1'

    # Edit the source the application was built from, then reload.
    $logic = Join-Path $src 'logic.cpp'
    $text = Get-Content -Raw $logic
    if ($text -notmatch 'return 1;') { Fail 'logic.cpp has no "return 1;" to edit' }
    Set-Content -NoNewline -Path $logic -Value ($text -replace 'return 1;', 'return 2;')
    $watch = [Diagnostics.Stopwatch]::StartNew()
    # The packaged helper, exactly as docs\QUICKSTART.md uses it.
    $reloadOut = & (Join-Path $Root 'scripts\cml-reload.ps1') -Pipe $controlName -Arguments "{`"timeout_ms`":$ReloadTimeoutMs}" -TimeoutMs ($ReloadTimeoutMs + 10000)
    $reloadExit = $LASTEXITCODE
    $watch.Stop()
    if ($reloadExit -ne 0) { Show-Logs; Fail ("reload failed (exit $reloadExit): " + ($reloadOut -join ' ')) }
    Note ("reload-result=" + ($reloadOut -join ' '))
    $deadline = (Get-Date).AddSeconds(30)
    $seen = $false
    while ((Get-Date) -lt $deadline -and -not $seen) {
        $seen = Select-String -Path $appOut -Pattern 'SMOKE tick=\d+ value=2' -Quiet
        if (-not $seen) { Start-Sleep -Milliseconds 200 }
    }
    if (-not $seen) { Show-Logs; Fail 'the application never printed value=2 after the reload' }
    Note ("after=value2 reload_ms=" + $watch.ElapsedMilliseconds)
    Note 'reload=ok'
} finally {
    Stop-All
    Show-Logs
}
Note 'RESULT=PASS'
