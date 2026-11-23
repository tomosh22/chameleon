<#
.SYNOPSIS
  Sends one command to a running cml_broker over its control pipe and prints the JSON answer.

.EXAMPLE
  .\cml-reload.ps1                      # reload what changed (the default command)
  .\cml-reload.ps1 -Command status      # processes, modules, capabilities
  .\cml-reload.ps1 -Pipe my_control_pipe

  Exit code 0 when the broker answered ok, 1 when it answered with a diagnostic (its id, message and advice are
  printed; docs\TROUBLESHOOTING.md explains every id), 2 when no broker could be reached.
#>
[CmdletBinding()]
param(
    [string]$Command = 'reload',
    [string]$Arguments = '{}',            # JSON object with the command's arguments, e.g. '{"timeout_ms":120000}'
    [string]$Pipe,                        # pipe name without \\.\pipe\ ; default: cml_control_<logon session SID>, like the broker
    [int]$TimeoutMs = 130000
)
$ErrorActionPreference = 'Stop'
if (-not $Pipe) {
    $sid = (whoami /groups /fo csv | ConvertFrom-Csv | Where-Object { $_.SID -like 'S-1-5-5-*' } | Select-Object -First 1).SID
    if (-not $sid) { Write-Host 'cml-reload: cannot determine the logon session SID; pass -Pipe'; exit 2 }
    $Pipe = "cml_control_$sid"
}
$client = New-Object System.IO.Pipes.NamedPipeClientStream('.', $Pipe, [System.IO.Pipes.PipeDirection]::InOut)
try {
    try { $client.Connect(5000) } catch {
        Write-Host "cml-reload: no broker on \\.\pipe\$Pipe. Start one with: cml_broker.exe --headless  (CML5404)"
        exit 2
    }
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $writer = New-Object System.IO.StreamWriter($client, $utf8); $writer.NewLine = "`n"; $writer.AutoFlush = $true
    $reader = New-Object System.IO.StreamReader($client, $utf8)
    $request = [ordered]@{ id = 1; cmd = $Command; args = ($Arguments | ConvertFrom-Json) }
    $writer.WriteLine(($request | ConvertTo-Json -Compress -Depth 8))
    $task = $reader.ReadLineAsync()
    if (-not $task.Wait($TimeoutMs)) { Write-Host "cml-reload: no answer within $TimeoutMs ms (CML5408)"; exit 2 }
    Write-Output $task.Result
    $answer = $task.Result | ConvertFrom-Json
    if ($answer.ok) { exit 0 }
    Write-Host ("{0}: {1} What to do: {2}" -f $answer.diag.id, $answer.diag.message, $answer.diag.action)
    exit 1
} finally { $client.Dispose() }
