# WP-53 spike (R-61): builds every case of cases/ with one compiler at /O2 /Ob2, links the old image with its PDB, dumps the
# records of the old image and the patch object, and plans the patch against the old image with the planner of this commit
# (plan.exe, built by the spike workflow). Usage: run.ps1 -Compiler cl|clang-cl -Cxx <path> -Out <dir> -Plan <plan.exe>
param(
  [Parameter(Mandatory = $true)][string]$Compiler,
  [Parameter(Mandatory = $true)][string]$Cxx,
  [Parameter(Mandatory = $true)][string]$Out,
  [Parameter(Mandatory = $true)][string]$Plan,
  [Parameter(Mandatory = $true)][string]$PdbUtil
)
$ErrorActionPreference = 'Stop'
$ReadObj = Join-Path (Split-Path -Parent $PdbUtil) 'llvm-readobj.exe'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$flags = @('/nologo', '/O2', '/Ob2', '/Zi', '/std:c++20', '/EHsc', '/GS-', '/c')
& $Cxx @flags "$here\support.cpp" "/Fo$Out\support.obj" "/Fd$Out\support.pdb"
if ($LASTEXITCODE -ne 0) { throw 'support.cpp' }
$cases = Get-ChildItem "$here\cases\*.old.cpp" | ForEach-Object { $_.Name -replace '\.old\.cpp$', '' } | Sort-Object
foreach ($c in $cases) {
  Write-Host "=== $Compiler $c"
  # Each object gets a PDB of its own (/Fd): cl /Zi writes the types there; the patch object's types are read from it.
  & $Cxx @flags "$here\cases\$c.old.cpp" "/Fo$Out\$c.old.obj" "/Fd$Out\$c.old.vc.pdb"
  if ($LASTEXITCODE -ne 0) { throw "$c old" }
  & $Cxx @flags "$here\cases\$c.new.cpp" "/Fo$Out\$c.new.obj" "/Fd$Out\$c.new.vc.pdb"
  if ($LASTEXITCODE -ne 0) { throw "$c new" }
  link /nologo /DEBUG:FULL /OPT:REF /OPT:ICF "/OUT:$Out\$c.exe" "/PDB:$Out\$c.pdb" "$Out\support.obj" "$Out\$c.old.obj"
  if ($LASTEXITCODE -ne 0) { throw "$c link" }
  Write-Host "--- old image: the module of $c.old.obj"
  $dump = & $PdbUtil dump --symbols --ids --modules "$Out\$c.pdb" 2>&1 | Out-String
  $lines = $dump -split "`r?`n"
  $in = $false
  foreach ($l in $lines) {
    if ($l -match '^Mod \d+ \|') { $in = $l -match [regex]::Escape("$c.old.obj") }
    if ($in) { Write-Host $l }
  }
  Write-Host "--- old image: ids of call operators and closures"
  & $PdbUtil dump --ids "$Out\$c.pdb" 2>&1 | Select-String -Context 0,2 -Pattern 'operator\(\)|lambda' | ForEach-Object { Write-Host $_ }
  Write-Host "--- old object: symbols"
  dumpbin /nologo /symbols "$Out\$c.old.obj" | Select-String -Pattern 'Static|External' | Select-String -Pattern 'SECT' | ForEach-Object { Write-Host $_.Line }
  Write-Host "--- patch object: symbols"
  dumpbin /nologo /symbols "$Out\$c.new.obj" | Select-String -Pattern 'Static|External' | Select-String -Pattern 'SECT' | ForEach-Object { Write-Host $_.Line }
  Write-Host "--- patch object: .debug`$S symbols of f and of the call operators"
  & $ReadObj --codeview "$Out\$c.new.obj" 2>&1 | ForEach-Object { Write-Host $_ }
  Write-Host "--- plan (this commit's planner)"
  & $Plan "$Out\$c.pdb" "$Out\$c.new.obj" "$c.old.obj" "$Out\$c.exe" 2>&1 | ForEach-Object { Write-Host $_ }
}
