# Builds trigger_bench and runs it.  See README.md.
#   build_and_run.ps1 [-Work <dir>] [-Python <python>] [-Patterns <file>] [-Platform ARM64|x64] [-NoBuild]
#                     [-Mode verify|time|both] [-Passes N]
param([string]$Work = '', [string]$Python = '', [string[]]$Patterns = @(), [string]$Platform = 'ARM64',
      [switch]$NoBuild, [ValidateSet('verify', 'time', 'both')] [string]$Mode = 'both', [int]$Passes = 20)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Work) { $Work = Join-Path $env:TEMP 'trigger_bench' }
New-Item -ItemType Directory -Force $Work | Out-Null
$python = $Python
if (-not $python) { $python = (Get-Command python3 -ErrorAction SilentlyContinue).Source }

if (-not $NoBuild) {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  $vs = & $vswhere -latest -products * -property installationPath
  $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
  $arg = @{ 'ARM64' = 'arm64'; 'x64' = 'arm64_amd64' }[$Platform]
  $p2 = Join-Path $repo 'pcre2\src'
  # pcre2_jit_*_inc.h and the like are included by pcre2_jit_compile.c; the .c files stand alone
  $new = (Get-ChildItem $p2 -Filter 'pcre2_*.c' | ForEach-Object { '"' + $_.FullName + '"' }) -join ' '
  $bat = Join-Path $Work 'build.bat'
  @"
@echo off
call "$vcvars" $arg >nul 2>&1
mkdir "$Work\obj" 2>nul
cd /d "$Work\obj"
cl /nologo /c /O2 /MT /w /DHAVE_CONFIG_H /D_CRT_SECURE_NO_WARNINGS /I"$p2" /I"$repo\src" $new "$repo\src\mc_pcre.c" || exit /b 1
cd /d "$Work"
cl /nologo /EHsc /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /I"$p2" /I"$repo\src" "$PSScriptRoot\trigger_bench.cpp" obj\*.obj /Fe:trigger_bench.exe || exit /b 1
"@ | Set-Content $bat -Encoding ascii
  & cmd /c $bat 2>&1 | Select-String -Pattern 'error|warning|LNK' | ForEach-Object { $_.Line }
  if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}

$exe = Join-Path $Work 'trigger_bench.exe'
# every pattern pcre_compare knows of (plugins, worlds, rex.new, the tests, the edge cases),
# and those in the dumps; against its subjects plus the Discworld-like lines
$pc = Join-Path $repo 'tools\pcre_compare'
& $python -I -B (Join-Path $pc 'make_corpus.py') $Work $repo (Join-Path $repo 'packaging\discworld-plugins')
& $python -I -B (Join-Path $PSScriptRoot 'more_subjects.py') $Work (Join-Path $PSScriptRoot 'discworld_lines.txt') @Patterns
if ($Mode -ne 'time') {
  & $exe verify (Join-Path $Work 'all_patterns.txt') (Join-Path $Work 'all_subjects.hex')
  "verify exit $LASTEXITCODE"
}
if ($Mode -ne 'verify') {
  foreach ($p in $Patterns) {
    # more_subjects.py wrote each dump's triggers out as <dump>.patterns
    & $exe time "$p.patterns" (Join-Path $PSScriptRoot 'discworld_lines.txt') $Passes
  }
}
