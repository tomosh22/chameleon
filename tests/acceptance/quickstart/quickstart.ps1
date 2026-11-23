# Chameleon quickstart test: follows QUICKSTART.md step by step.
# Each guide "prompt" is emulated by a cmd.exe fed its lines on stdin, so every line is parsed as it would be
# when typed at a prompt (single %d in for loops, set CML persists within that prompt).
$ErrorActionPreference = 'Continue'
$work = Join-Path $(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }) 'qs-work'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$bg = @()          # background processes: @{Name; Proc; Out; Err}
$script:failed = $false
$script:seq = 0
$setCml = 'for /d %d in (chameleon-*-win64) do set CML=C:\chameleon-demo\%d'

function Say($s) { Write-Host $s }
function Step($n, $name, $ok, $detail) {
    $d = ("$detail" -replace '\s+', ' ').Trim()
    if ($d.Length -gt 600) { $d = $d.Substring(0, 600) + '...' }
    $st = if ($ok) { 'PASS' } else { 'FAIL' }
    Say "QS-STEP $n $name $st $d"
    if (-not $ok) { $script:failed = $true; throw "step $n ($name) failed" }
}
function New-Prompt($lines) {
    $script:seq++
    $in = Join-Path $work "prompt$($script:seq).in"
    [IO.File]::WriteAllText($in, (($lines -join "`r`n") + "`r`n"), [Text.Encoding]::ASCII)
    return $in
}
# Foreground prompt: returns @{Code; Text}
function Run-Prompt($lines, $timeoutSec = 300) {
    $in = New-Prompt $lines
    $o = "$in.out"; $e = "$in.err"
    $p = Start-Process -FilePath cmd.exe -ArgumentList '/d', '/q' -RedirectStandardInput $in `
        -RedirectStandardOutput $o -RedirectStandardError $e -NoNewWindow -PassThru
    $null = $p.Handle
    if (-not $p.WaitForExit($timeoutSec * 1000)) {
        & taskkill /T /F /PID $p.Id | Out-Null
        $txt = (Get-Content $o -Raw -ErrorAction SilentlyContinue) + (Get-Content $e -Raw -ErrorAction SilentlyContinue)
        return @{ Code = -999; Text = "TIMEOUT after ${timeoutSec}s`n$txt" }
    }
    $p.WaitForExit()
    $txt = (Get-Content $o -Raw -ErrorAction SilentlyContinue) + (Get-Content $e -Raw -ErrorAction SilentlyContinue)
    Say "---- prompt output ($in) exit=$($p.ExitCode) ----"; Say $txt; Say '---- end ----'
    return @{ Code = $p.ExitCode; Text = $txt }
}
# Background prompt: leaves the command running, stdout/stderr to log files
function Start-BgPrompt($name, $lines) {
    $in = New-Prompt $lines
    $o = Join-Path $work "$name.out.log"; $e = Join-Path $work "$name.err.log"
    $p = Start-Process -FilePath cmd.exe -ArgumentList '/d', '/q' -RedirectStandardInput $in `
        -RedirectStandardOutput $o -RedirectStandardError $e -NoNewWindow -PassThru
    $null = $p.Handle
    $script:bg += @{ Name = $name; Proc = $p; Out = $o; Err = $e }
    return $script:bg[-1]
}
function Log-Text($b) {
    $t = ''
    foreach ($f in @($b.Out, $b.Err)) {
        try {
            $fs = [IO.File]::Open($f, 'Open', 'Read', 'ReadWrite')
            $sr = New-Object IO.StreamReader($fs); $t += $sr.ReadToEnd(); $sr.Close()
        } catch { }
    }
    return $t
}
function Wait-Log($b, $regex, $sec) {
    $end = (Get-Date).AddSeconds($sec)
    do {
        $t = Log-Text $b
        if ($t -match $regex) { return $true }
        if ($b.Proc.HasExited) { $t = Log-Text $b; return ($t -match $regex) }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $end)
    return $false
}

try {
    $zip = $env:CML_ZIP_PATH
    if (-not $zip -or -not (Test-Path $zip)) { Step 0 precheck $false "CML_ZIP_PATH missing or not found: '$zip'" }
    $zipName = Split-Path $zip -Leaf
    $zipVer = if ($zipName -match 'chameleon-(.+)-win64\.zip') { $Matches[1] } else { $null }
    Say "zip=$zip version-from-name=$zipVer"

    # ---- Step 1: unpack -------------------------------------------------------------------------------------
    $r = Run-Prompt @(
        'mkdir C:\chameleon-demo || exit /b 1',
        'cd /d C:\chameleon-demo',
        "tar -xf `"$zip`" || exit /b 1",
        'cd /d C:\chameleon-demo',
        $setCml,
        'echo CML=%CML%',
        'dir %CML%',
        'exit /b 0')
    Step 1 unpack ($r.Code -eq 0 -and (Test-Path 'C:\chameleon-demo\chameleon-*-win64')) "exit=$($r.Code) tree=$((Get-ChildItem C:\chameleon-demo -ErrorAction SilentlyContinue | % Name) -join ',')"
    $cmlDir = (Get-ChildItem C:\chameleon-demo -Directory -Filter 'chameleon-*-win64' | Select-Object -First 1).FullName
    $m = [regex]::Match($r.Text, '(?m)^CML=(.*)$')
    Step 2 cml-var ($m.Success -and $m.Groups[1].Value.Trim() -eq $cmlDir) "echo CML gave '$($m.Groups[1].Value.Trim())', folder is '$cmlDir'"
    $missing = @('bin', 'docs', 'examples', 'include', 'integrations', 'lib', 'licenses', 'scripts', 'MANIFEST.txt', 'README.txt') |
        Where-Object { $r.Text -notmatch "(?m)\s$([regex]::Escape($_))\s*$" }
    Step 3 dir-listing (-not $missing) $(if ($missing) { "missing from dir output: $($missing -join ',')" } else { 'all expected entries listed' })

    # ---- Step 1b: version -----------------------------------------------------------------------------------
    $r = Run-Prompt @('cd /d C:\chameleon-demo', $setCml, '%CML%\bin\cml_broker.exe --version', 'exit /b %errorlevel%')
    $okv = ($r.Code -eq 0) -and ($r.Text -match 'cml_broker \d+\.\d+\.\d+')
    if ($okv -and $zipVer -and ($r.Text -notmatch [regex]::Escape("cml_broker $zipVer"))) { $okv = $false }
    Step 4 version $okv "exit=$($r.Code) output=$($r.Text.Trim())"

    # ---- Step 2: build the example --------------------------------------------------------------------------
    $r = Run-Prompt @(
        'cd /d C:\chameleon-demo', $setCml,
        'xcopy /E /I /Y %CML%\examples\hello C:\chameleon-demo\hello || exit /b 1',
        'cd /d C:\chameleon-demo\hello',
        'cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=%CML% || exit /b 1',
        'exit /b 0') 600
    Step 5 configure ($r.Code -eq 0) "exit=$($r.Code) tail=$(($r.Text -split "`n" | Select-Object -Last 4) -join ' | ')"
    $r = Run-Prompt @('cd /d C:\chameleon-demo\hello', $setCml, 'cmake --build build || exit /b 1', 'exit /b 0') 600
    Step 6 build (($r.Code -eq 0) -and (Test-Path 'C:\chameleon-demo\hello\build\smoke_app.exe')) "exit=$($r.Code) exe=$(Test-Path 'C:\chameleon-demo\hello\build\smoke_app.exe') tail=$(($r.Text -split "`n" | Select-Object -Last 4) -join ' | ')"

    $r = Run-Prompt @('cd /d C:\chameleon-demo\hello', $setCml, '%CML%\bin\cml_broker.exe check build\smoke_app.exe', 'exit /b %errorlevel%')
    $lines = $r.Text -split "`r?`n" | Where-Object { $_.Trim() }
    $fails = $lines | Where-Object { $_ -match '^\s*FAIL' -or $_ -match '\bFAIL\b' }
    $passes = $lines | Where-Object { $_ -match '^\s*PASS' }
    $other = $lines | Where-Object { $_ -notmatch '^\s*(PASS|WARN)' }
    if ($other) { Say "QS-NOTE check output has lines not starting with PASS/WARN: $($other -join ' | ')" }
    Step 7 check (($r.Code -eq 0) -and -not $fails -and $passes) "exit=$($r.Code) pass=$(@($passes).Count) fail=$(@($fails).Count) failing: $($fails -join ' | ')"

    # ---- Step 3: broker (second prompt, left running) -------------------------------------------------------
    $broker = Start-BgPrompt 'broker' @('cd /d C:\chameleon-demo', $setCml, '%CML%\bin\cml_broker.exe --headless --verbose')
    $ok1 = Wait-Log $broker 'cml_broker \S+ pid=\S+ pipe=\S+' 60
    $ok2 = Wait-Log $broker 'licence check: allowed \(no-op licence provider: all features enabled\)' 30
    Step 8 broker-banner ($ok1 -and $ok2) "banner=$ok1 licence=$ok2 exited=$($broker.Proc.HasExited) log=$(Log-Text $broker)"

    # ---- Step 4: program (first prompt, left running) -------------------------------------------------------
    $app = Start-BgPrompt 'smoke_app' @('cd /d C:\chameleon-demo\hello', $setCml, 'build\smoke_app.exe --agent-dir %CML%\bin')
    $e1 = Wait-Log $app 'SMOKE enabled=1' 60
    $v1 = Wait-Log $app 'SMOKE tick=\d+ value=1' 30
    Step 9 program-start ($e1 -and $v1) "enabled=$e1 value1=$v1 exited=$($app.Proc.HasExited) log=$(Log-Text $app)"
    Start-Sleep -Seconds 2
    $bt = Log-Text $broker
    Step 10 broker-logs-module (($bt -match '(?i)enabl') -and ($bt -match '(?i)pdb')) "enabled-mention=$($bt -match '(?i)enabl') pdb-mention=$($bt -match '(?i)pdb') brokerlog=$bt"

    # ---- Step 5: change and reload (third prompt) -----------------------------------------------------------
    $r = Run-Prompt @(
        'cd /d C:\chameleon-demo\hello',
        'powershell -NoProfile -Command "(Get-Content logic.cpp) -replace ''return 1;'',''return 2;'' | Set-Content logic.cpp"',
        'exit /b %errorlevel%')
    $changed = (Get-Content C:\chameleon-demo\hello\logic.cpp -Raw) -match 'return 2;'
    Step 11 edit-source (($r.Code -eq 0) -and $changed) "exit=$($r.Code) logic.cpp contains 'return 2;'=$changed"

    $r = Run-Prompt @(
        'cd /d C:\chameleon-demo\hello', $setCml,
        'powershell -NoProfile -ExecutionPolicy Bypass -File %CML%\scripts\cml-reload.ps1',
        'exit /b %errorlevel%') 300
    Step 12 reload-json (($r.Code -eq 0) -and ($r.Text -match '"ok"\s*:\s*true')) "exit=$($r.Code) output=$($r.Text.Trim())"

    $v2 = Wait-Log $app 'SMOKE tick=\d+ value=2' 60
    $still = -not $app.Proc.HasExited
    Step 13 value-2-after-reload ($v2 -and $still) "value2=$v2 program-still-running-same-process=$still"
}
catch {
    Say "QS-ERROR $_"
    $script:failed = $true
}
finally {
    foreach ($b in $bg) {
        Say "==== tail of $($b.Name) log ===="
        Say ((Log-Text $b) -split "`r?`n" | Select-Object -Last 40 | Out-String)
        try { if (-not $b.Proc.HasExited) { & taskkill /T /F /PID $b.Proc.Id | Out-Null } } catch { }
    }
    if ($script:failed) { Say 'QS-RESULT FAIL'; exit 1 } else { Say 'QS-RESULT PASS'; exit 0 }
}
