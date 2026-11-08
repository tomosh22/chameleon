# WP-26: runs Chameleon against one open-source project built with one compiler (docs/wp/WP-26.md).
#
#   pwsh tests/corpus/run_corpus.ps1 -Project raylib|imgui|godot -Compiler msvc|clang-cl `
#        -ChameleonBin <build>/bin -WorkDir <dir> [-CxxPath <full path of cl / clang-cl>]
#
# Steps (each is a row of the pass/fail matrix, <WorkDir>/out-<project>-<compiler>/matrix.json):
#   fetch      git clone of the pinned tag (never vendored), tests/corpus/<project>/chameleon.patch + support files
#   build      the project's build.ps1 with the flags of docs/BUILD_REQUIREMENTS.md
#   check      cml_broker check (WP-18) must pass on the build
#   decode     cml_lde_probe: the hook installer's instruction-length decoder over every function entry of the PDB
#   enable     broker + the application with the agent: module enabled, PDB index ready, frames printed
#   edit:<id>  one edit of project.json, `reload`, the change is observable, the application keeps running
#   refused:<id>  the deliberately unsupported edit is refused with its diagnostic and the application keeps running
# Exit code 1 when a step failed (an `expected_failure` of project.json is asserted and counts as passed).
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('raylib', 'imgui', 'godot')][string]$Project,
    [Parameter(Mandatory)][ValidateSet('msvc', 'clang-cl')][string]$Compiler,
    [Parameter(Mandatory)][string]$ChameleonBin,
    [Parameter(Mandatory)][string]$WorkDir,
    [string]$Config = 'RelWithDebInfo',
    [string]$CxxPath = '',
    [string[]]$Skip = @()
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $here '../..')).Path
$ChameleonBin = (Resolve-Path $ChameleonBin).Path
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$WorkDir = (Resolve-Path $WorkDir).Path
$cfg = Get-Content (Join-Path $here "$Project/project.json") -Raw | ConvertFrom-Json
$out = Join-Path $WorkDir "out-$Project-$Compiler"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$src = Join-Path $WorkDir "src-$Project"
$buildDir = Join-Path $WorkDir "build-$Project-$Compiler"
if ($Project -eq 'godot') { $src = Join-Path $WorkDir "src-godot" }   # one checkout (and one scons build tree) per cell
$latin1 = [System.Text.Encoding]::GetEncoding(28591)
$script:Matrix = New-Object System.Collections.Generic.List[object]
$script:Originals = @{}
$script:Prev = @{}
$script:ExePath = $null
$script:Broker = $null
$script:App = $null
$script:Ctl = $null
$script:Failed = $false

function Add-Result([string]$Step, [string]$Result, [string]$Detail = '', [double]$Seconds = 0) {
    $row = [ordered]@{ project = $Project; compiler = $Compiler; step = $Step; result = $Result; detail = $Detail; seconds = [math]::Round($Seconds, 1) }
    $script:Matrix.Add([pscustomobject]$row)
    if ($Result -eq 'fail') { $script:Failed = $true }
    Write-Host ("MATRIX {0} {1} {2}: {3} {4} ({5:N1}s)" -f $Project, $Compiler, $Step, $Result.ToUpper(), $Detail, $Seconds)
    if ($Result -eq 'fail') { Write-Host "::error::$Project/$Compiler/$Step failed: $Detail" }
}

# Runs a step; a thrown exception is a failed step. Returns $true when the step passed.
function Invoke-Step([string]$Name, [scriptblock]$Body) {
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    Write-Host "::group::$Project/$Compiler/$Name"
    try {
        $detail = & $Body
        Add-Result $Name 'pass' ([string]($detail | Select-Object -Last 1)) $watch.Elapsed.TotalSeconds
        return $true
    } catch {
        $msg = ($_ | Out-String).Trim()
        if ($msg.Length -gt 1500) { $msg = $msg.Substring(0, 1500) }
        $expected = $cfg.expected_failures | Where-Object { $_.step -eq $Name -and ($_.compiler -eq $Compiler -or $_.compiler -eq '*') } | Select-Object -First 1
        if ($expected) {
            Add-Result $Name 'expected-fail' ("{0} -- {1}" -f $expected.reason, $msg) $watch.Elapsed.TotalSeconds
        } else {
            Add-Result $Name 'fail' $msg $watch.Elapsed.TotalSeconds
        }
        return $false
    } finally {
        Write-Host "::endgroup::"
    }
}

function Skip-Step([string]$Name, [string]$Why) { Add-Result $Name 'skipped' $Why 0 }

function Invoke-Native([string]$Label, [string]$Exe, [string[]]$ArgList, [string]$LogFile = '') {
    Write-Host ">> $Label : $Exe $($ArgList -join ' ')"
    if ($LogFile) {
        & $Exe @ArgList *>&1 | Tee-Object -FilePath $LogFile -Append | Out-Host
    } else {
        & $Exe @ArgList
    }
    if ($LASTEXITCODE -ne 0) { throw "$Label failed with exit code $LASTEXITCODE" }
}

function Get-CorpusLines { if (Test-Path $script:CorpusLog) { @(Get-Content $script:CorpusLog -ErrorAction SilentlyContinue) } else { @() } }

# Waits until a line of the corpus log after line index $From (0-based count of lines already seen) matches $Regex.
function Wait-Corpus([string]$Regex, [int]$From, [int]$TimeoutSec) {
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    while ($watch.Elapsed.TotalSeconds -lt $TimeoutSec) {
        $lines = Get-CorpusLines
        for ($i = $From; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match $Regex) { return $lines[$i] }
        }
        if ($script:App -and $script:App.HasExited) { throw "the application exited (code $($script:App.ExitCode)) while waiting for /$Regex/" }
        Start-Sleep -Milliseconds 250
    }
    $tail = (Get-CorpusLines | Select-Object -Last 3) -join ' | '
    throw "no output line matching /$Regex/ within ${TimeoutSec}s; last lines: $tail"
}

function Get-BrokerCaps {
    $raw = $script:Ctl.Call('status', $null, 30000)
    if ($null -eq $raw) { throw 'no answer to status on the control pipe (connection closed or timeout)' }
    Write-Host "status: $raw"
    return ($raw | ConvertFrom-Json)
}

# ---------------------------------------------------------------------------------------------------------------------
# fetch + patch
# ---------------------------------------------------------------------------------------------------------------------
$fetched = Invoke-Step 'fetch' {
    $marker = Join-Path $src '.chameleon-corpus-patched'
    $patchFile = Join-Path $here "$Project/$($cfg.patch)"
    $patchHash = (Get-FileHash $patchFile -Algorithm SHA256).Hash
    $support = Join-Path $src $cfg.support_dir
    if ((Test-Path $marker) -and ((Get-Content $marker -Raw) -match $patchHash)) {
        # A restored cache: the patched checkout and its build tree. The support files are refreshed (they are ours).
        Copy-Item (Join-Path $here 'common/chameleon_corpus.h') $support -Force
        Copy-Item (Join-Path $here 'common/chameleon_corpus.cpp') $support -Force
        Copy-Item (Join-Path $repo 'agent/include/Chameleon_API.h') $support -Force
        "reused checkout (cache): $((Get-Content $marker -Raw).Trim())"
        return
    }
    if (Test-Path $src) { Remove-Item -Recurse -Force $src }
    Invoke-Native 'git clone' 'git' @('-c', 'core.autocrlf=false', 'clone', '--quiet', '--depth', '1', '--branch', $cfg.tag, $cfg.repo, $src)
    $head = (& git -C $src rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'git rev-parse failed' }
    if ($cfg.commit -and $head -ne $cfg.commit) { throw "pinned commit mismatch: $($cfg.tag) is $head, expected $($cfg.commit)" }
    Copy-Item (Join-Path $here 'common/chameleon_corpus.h') $support
    Copy-Item (Join-Path $here 'common/chameleon_corpus.cpp') $support
    Copy-Item (Join-Path $repo 'agent/include/Chameleon_API.h') $support
    Invoke-Native 'git apply' 'git' @('-C', $src, '-c', 'core.autocrlf=false', 'apply', '--whitespace=nowarn', '--ignore-whitespace', $patchFile)
    Set-Content -Path $marker -Value "$($cfg.tag) $head patch=$patchHash"
    "$($cfg.repo) $($cfg.tag) $head"
}

# ---------------------------------------------------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------------------------------------------------
$built = $false
if ($fetched) {
    $built = Invoke-Step 'build' {
        . (Join-Path $here "$Project/build.ps1")
        if (-not (Test-Path $script:ExePath)) { throw "build did not produce $($script:ExePath)" }
        $pdb = [System.IO.Path]::ChangeExtension($script:ExePath, '.pdb')
        if (-not (Test-Path $pdb)) { throw "no PDB next to the executable: $pdb" }
        "{0} ({1:N0} MB), pdb {2:N0} MB" -f $script:ExePath, ((Get-Item $script:ExePath).Length / 1MB), ((Get-Item $pdb).Length / 1MB)
    }
} else {
    Skip-Step 'build' 'fetch failed'
}

# ---------------------------------------------------------------------------------------------------------------------
# check (WP-18) and decode (plan section 11)
# ---------------------------------------------------------------------------------------------------------------------
if ($built) {
    Invoke-Step 'check' {
        $log = Join-Path $out 'check.txt'
        Remove-Item $log -ErrorAction SilentlyContinue
        & (Join-Path $ChameleonBin 'cml_broker.exe') check --verbose $script:ExePath *>&1 | Tee-Object -FilePath $log | Out-Host
        $rc = $LASTEXITCODE
        $text = Get-Content $log -Raw
        $ids = ([regex]::Matches($text, 'CML\d{4}') | ForEach-Object { $_.Value } | Sort-Object -Unique) -join ','
        $warn = ([regex]::Matches($text, '(?m)^\s*WARN\b')).Count
        if ($rc -ne 0) { throw "cml_broker check exited with $rc (ids: $ids)" }
        "exit 0, $warn WARN, ids: $ids"
    } | Out-Null

    Invoke-Step 'decode' {
        $log = Join-Path $out 'decode.txt'
        Remove-Item $log -ErrorAction SilentlyContinue
        $probeArgs = @($script:ExePath, '--quiet', '--fail-on-unknown-opcode', '--min-functions', [string]$cfg.min_functions)
        & (Join-Path $ChameleonBin 'cml_lde_probe.exe') @probeArgs *>&1 | Tee-Object -FilePath $log | Out-Host
        $rc = $LASTEXITCODE
        $text = Get-Content $log -Raw
        $m = [regex]::Match($text, 'LDE-OUTCOME (.*)')
        $summary = if ($m.Success) { $m.Groups[1].Value.Trim() } else { 'no LDE-OUTCOME line' }
        if ($rc -ne 0) { throw "cml_lde_probe exited with ${rc}: $summary" }
        $summary
    } | Out-Null
} else {
    Skip-Step 'check' 'build failed'
    Skip-Step 'decode' 'build failed'
}

# ---------------------------------------------------------------------------------------------------------------------
# run: broker + application + edits
# ---------------------------------------------------------------------------------------------------------------------
function Stop-Session {
    if ($script:Ctl) { $script:Ctl.Dispose(); $script:Ctl = $null }
    if ($script:App -and -not $script:App.HasExited) { try { $script:App.Kill($true) } catch { } }
    if ($script:Broker -and -not $script:Broker.HasExited) { try { $script:Broker.Kill($true) } catch { } }
}

# Describes a CRASH line of the application's log (chameleon_corpus.cpp): the module and offset, symbolised with the PDB of
# the executable or of the patch DLL.
function Report-Crash {
    foreach ($line in (Get-CorpusLines | Where-Object { $_ -like 'CRASH *' })) {
        Write-Host "::error::$line"
        if ($line -match 'module=(\S+)\+0x([0-9a-f]+)') {
            $mod = $Matches[1]; $off = $Matches[2]
            $target = $null
            if ($mod -ieq (Split-Path $script:ExePath -Leaf)) { $target = $script:ExePath }
            else { $target = (Get-ChildItem (Join-Path $out 'session') -Recurse -Filter $mod -ErrorAction SilentlyContinue | Select-Object -First 1).FullName }
            if ($target) {
                $pdb = [System.IO.Path]::ChangeExtension($target, '.pdb')
                Write-Host "symbolising $mod+0x$off with $pdb"
                & (Join-Path $ChameleonBin 'cml_pdb_dump.exe') $pdb --rva $off *>&1 | Out-Host
            } else { Write-Host "no file found for module $mod" }
        }
    }
}

function Restore-Originals {
    foreach ($path in $script:Originals.Keys) {
        [System.IO.File]::WriteAllBytes($path, $latin1.GetBytes($script:Originals[$path]))
    }
    $script:Originals = @{}
}

function Restore-File([string]$Path) {
    if ($script:Prev.ContainsKey($Path)) {   # the content before the last edit of this file
        [System.IO.File]::WriteAllBytes($Path, $latin1.GetBytes($script:Prev[$Path]))
    }
}

function Apply-Edit($edit) {
    $path = Join-Path $src $edit.file
    if (-not (Test-Path $path)) { throw "edit $($edit.id): $($edit.file) does not exist" }
    $text = $latin1.GetString([System.IO.File]::ReadAllBytes($path))
    if (-not $script:Originals.ContainsKey($path)) { $script:Originals[$path] = $text }
    $script:Prev[$path] = $text
    # Some projects are checked out with CRLF line endings (.gitattributes) and a patch applied with --ignore-whitespace can
    # leave LF lines in them: the edits are written with LF and matched against either.
    foreach ($r in @($edit.replace)) {
        $findLf = ([string]$r.find).Replace("`r`n", "`n")
        $pattern = [regex]::Escape($findLf).Replace('\n', '\r?\n')
        $found = [regex]::Matches($text, $pattern)
        if ($found.Count -ne 1) { throw "edit $($edit.id): '$($r.find)' occurs $($found.Count) times in $($edit.file), expected once" }
        $m = $found[0]
        $eol = if ($m.Value.Contains("`r`n")) { "`r`n" } else { "`n" }
        $with = ([string]$r.with).Replace("`r`n", "`n").Replace("`n", $eol)
        $text = $text.Substring(0, $m.Index) + $with + $text.Substring($m.Index + $m.Length)
    }
    if ($edit.include) {
        $anchor = if ($edit.include_anchor) { [regex]::Escape([string]$edit.include_anchor) } else { '^#\s*include' }
        $m = [regex]::Match($text, "(?m)$anchor")
        if (-not $m.Success) { throw "edit $($edit.id): no include anchor /$anchor/ in $($edit.file)" }
        $eol = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
        $text = $text.Insert($m.Index, "#include `"$($edit.include)`"$eol")
    }
    [System.IO.File]::WriteAllBytes($path, $latin1.GetBytes($text))
}

$ran = $false
if ($built) {
    $script:CorpusLog = Join-Path $out 'corpus.log'
    Remove-Item $script:CorpusLog -ErrorAction SilentlyContinue
    $agentPipe = "\\.\pipe\cml_corpus_agent_$PID"
    $ctlPipe = "\\.\pipe\cml_corpus_ctl_$PID"
    try {
        $ran = Invoke-Step 'enable' {
            try {
                Add-Type -Path (Join-Path $here 'CmlControl.cs')
            } catch {
                Add-Type -Path (Join-Path $here 'CmlControl.cs') -ReferencedAssemblies ([System.IO.Pipes.NamedPipeClientStream].Assembly.Location), 'System.Collections', 'System.Threading.Tasks', 'System.Diagnostics.Process', 'System.Runtime', 'System.Text.Encoding.Extensions', 'System.Security.Principal'
            }
            $session = Join-Path $out 'session'
            New-Item -ItemType Directory -Force -Path $session | Out-Null
            $brokerArgs = @('--headless', '--pipe', $agentPipe, '--control-pipe', $ctlPipe, '--log', (Join-Path $out 'broker.log'), '--verbose', '--session-dir', $session)
            $script:Broker = Start-Process -FilePath (Join-Path $ChameleonBin 'cml_broker.exe') -ArgumentList $brokerArgs -PassThru -WindowStyle Hidden
            $connected = $false
            for ($i = 0; $i -lt 60 -and -not $connected; $i++) {
                try { $script:Ctl = New-Object CmlControl($ctlPipe, 1000); $connected = $true } catch { Start-Sleep -Milliseconds 500 }
            }
            if (-not $connected) { throw 'cannot connect to the broker control pipe' }
            $caps = Get-BrokerCaps
            Write-Host "broker capabilities: $($caps.result.capabilities -join ' ')"
            foreach ($need in 'reload', 'wait', 'subscribe', 'diag-events') {
                if ($caps.result.capabilities -notcontains $need) { throw "the broker lacks the capability $need" }
            }
            $sub = $script:Ctl.Call('subscribe', '{"events":["diag","patch_applied"]}', 15000) | ConvertFrom-Json
            if (-not $sub.ok) { throw "subscribe failed: $($sub | ConvertTo-Json -Compress)" }

            $env:CML_PIPE_NAME = $agentPipe
            $env:CML_CORPUS_AGENT = '1'
            $env:CML_CORPUS_AGENT_DIR = $ChameleonBin
            $env:CML_CORPUS_LOG = $script:CorpusLog
            if ($cfg.env) { foreach ($p in $cfg.env.PSObject.Properties) { Set-Item -Path "env:$($p.Name)" -Value ([string]$p.Value) } }
            $appArgs = @(); foreach ($a in @($cfg.args)) { $appArgs += ([string]$a).Replace('{work}', $WorkDir).Replace('{src}', $src) }
            $sp = @{ FilePath = $script:ExePath; PassThru = $true; WorkingDirectory = (Split-Path $script:ExePath); RedirectStandardOutput = (Join-Path $out 'app.stdout.txt'); RedirectStandardError = (Join-Path $out 'app.stderr.txt') }
            if ($appArgs.Count -gt 0) { $sp.ArgumentList = $appArgs }
            $script:App = Start-Process @sp
            Wait-Corpus 'AGENT enabled=1' 0 $cfg.start_timeout_s | Out-Null
            $idx = $script:Ctl.Call('wait', ('{"for":"index","timeout_ms":' + ($cfg.index_timeout_s * 1000) + '}'), (($cfg.index_timeout_s + 30) * 1000)) | ConvertFrom-Json
            if (-not $idx.ok) { throw "the PDB index did not become ready: $($idx | ConvertTo-Json -Compress -Depth 6)" }
            $first = Wait-Corpus 'CORPUS frame=' 0 $cfg.start_timeout_s
            "module enabled and indexed in the running process (pid $($script:App.Id)); first line: $first"
        }
        if (-not $ran) {
            foreach ($e in $cfg.edits) { if (-not $e.compiler -or $e.compiler -eq $Compiler) { Skip-Step "edit:$($e.id)" 'enable failed' } }
        } else {
            $doEdit = {
                param($edit)
                $refused = [bool]$edit.expect_error
                $name = if ($refused) { "refused:$($edit.id)" } else { "edit:$($edit.id)" }
                Invoke-Step $name {
                    if ($script:App.HasExited) { throw "the application had already exited (code $($script:App.ExitCode))" }
                    Apply-Edit $edit
                    $from = (Get-CorpusLines).Count
                    $script:Ctl.Events.Clear()
                    $watch = [System.Diagnostics.Stopwatch]::StartNew()
                    $resp = $script:Ctl.Call('reload', ('{"timeout_ms":' + ($cfg.reload_timeout_s * 1000) + '}'), (($cfg.reload_timeout_s + 30) * 1000))
                    $wall = $watch.Elapsed.TotalSeconds
                    if ($null -eq $resp) { throw "no reply to reload within $($cfg.reload_timeout_s + 30)s" }
                    $script:Ctl.Pump(1500)
                    Add-Content -Path (Join-Path $out 'reloads.jsonl') -Value ($resp)
                    $show = if ($resp.Length -gt 3000) { $resp.Substring(0, 3000) + '...' } else { $resp }
                    Write-Host "reload response: $show"
                    Write-Host "events: $(@($script:Ctl.Events | ForEach-Object { if ($_.Length -gt 400) { $_.Substring(0, 400) } else { $_ } }) -join ' || ')"
                    $r = $resp | ConvertFrom-Json
                    if ($refused) {
                        $seen = ($resp -match $edit.expect_error) -or (@($script:Ctl.Events | Where-Object { $_ -match $edit.expect_error }).Count -gt 0)
                        if ($r.ok) { throw "the reload was accepted but must be refused with $($edit.expect_error)" }
                        if (-not $seen) { throw "the reload failed, but not with $($edit.expect_error): $resp" }
                        $before = (Get-CorpusLines).Count
                        Wait-Corpus 'CORPUS frame=' $before 20 | Out-Null     # still running, still producing frames
                        Restore-File (Join-Path $src $edit.file)                # a refused change would be retried by every later reload
                        "refused with $($edit.expect_error) as expected; application keeps running"
                    } else {
                        if (-not $r.ok) { throw "reload failed: $resp" }
                        $line = Wait-Corpus $edit.expect $from $cfg.observe_timeout_s
                        $t = $r.result.timing_ms
                        "reload {0:N1}s wall, total {1} ms (compile {2}, link {3}, load {4}, hooks {5}); patched={6} hooks={7}; observed: {8}" -f $wall, $t.total, $t.compile, $t.link, $t.load, $t.hooks, $r.result.patched, $r.result.hooks_installed, $line
                    }
                } | Out-Null
            }
            $stillRunning = {
                if ($script:App.HasExited) { throw "the application exited (code $($script:App.ExitCode))" }
                $before = (Get-CorpusLines).Count
                Wait-Corpus 'CORPUS frame=' $before 20 | Out-Null
                'frames still printed after the last edit'
            }
            $mine = @($cfg.edits | Where-Object { -not $_.compiler -or $_.compiler -eq $Compiler })
            foreach ($e in @($mine | Where-Object { -not $_.final })) { & $doEdit $e }
            Invoke-Step 'still-running' $stillRunning | Out-Null
            # `final` edits last: one that is expected to take the process down comes after the check above.
            $finals = @($mine | Where-Object { $_.final })
            foreach ($e in $finals) { & $doEdit $e }
            if ($finals.Count -gt 0) { Invoke-Step 'still-running-final' $stillRunning | Out-Null }
        }
    } finally {
        Stop-Session
        Restore-Originals
        Report-Crash
        foreach ($f in 'broker.log', 'app.stderr.txt') {
            $p = Join-Path $out $f
            if ((Test-Path $p) -and $script:Failed) { Write-Host "---- tail of $f"; Get-Content $p -Tail 40 | Out-Host }
        }
    }
} else {
    Skip-Step 'enable' 'build failed'
}

# ---------------------------------------------------------------------------------------------------------------------
# report
# ---------------------------------------------------------------------------------------------------------------------
$script:Matrix | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'matrix.json')
if ($env:GITHUB_STEP_SUMMARY) {
    $md = New-Object System.Collections.Generic.List[string]
    $md.Add("### corpus: $Project / $Compiler")
    $md.Add('| step | result | detail |')
    $md.Add('|---|---|---|')
    foreach ($row in $script:Matrix) {
        $d = ([string]$row.detail) -replace '\|', '/' -replace "[\r\n]+", ' '
        if ($d.Length -gt 300) { $d = $d.Substring(0, 300) + '...' }
        $md.Add("| $($row.step) | $($row.result) | $d |")
    }
    Add-Content -Path $env:GITHUB_STEP_SUMMARY -Value ($md -join "`n")
}
$pass = @($script:Matrix | Where-Object { $_.result -eq 'pass' }).Count
$xfail = @($script:Matrix | Where-Object { $_.result -eq 'expected-fail' }).Count
$fail = @($script:Matrix | Where-Object { $_.result -eq 'fail' }).Count
$skip = @($script:Matrix | Where-Object { $_.result -eq 'skipped' }).Count
Write-Host "CORPUS-RESULT project=$Project compiler=$Compiler pass=$pass expected_fail=$xfail fail=$fail skipped=$skip"
if ($script:Failed) { exit 1 }
exit 0
