# Builds and runs pcre_compare.  See README.md.
#   build_and_run.ps1 [-Work <dir>] [-Pcre8 <dir holding PCRE 8.45's pcre.h and sources>]
#                     [-InterpStride N] [-Scan <more dirs>] [-Platform ARM64|x64] [-NoBuild]
param([string]$Work = '', [string]$Pcre8 = '', [int]$InterpStride = 1, [string[]]$Scan = @(), [string]$Python = "", [string]$Platform = 'ARM64', [switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Work) { $Work = Join-Path $env:TEMP 'pcre_compare' }
New-Item -ItemType Directory -Force $Work | Out-Null
if (-not $Pcre8) { $Pcre8 = Join-Path $repo 'pcre' }
$python = $Python
if (-not $python) { $python = (Get-Command python3 -ErrorAction SilentlyContinue).Source }

if (-not $NoBuild) {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  $vs = & $vswhere -latest -products * -property installationPath
  $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
  $arg = @{ 'ARM64' = 'arm64'; 'x64' = 'arm64_amd64' }[$Platform]
  $p2 = Join-Path $repo 'pcre2\src'
  $old = (Get-ChildItem $Pcre8 -Filter 'pcre_*.c' | Where-Object { $_.Name -notin 'pcre_printint.c', 'pcre_byte_order.c', 'pcre_string_utils.c' } | ForEach-Object { '"' + $_.FullName + '"' }) -join ' '
  $new = (Get-ChildItem $p2 -Filter 'pcre2_*.c' | ForEach-Object { '"' + $_.FullName + '"' }) -join ' '
  $bat = Join-Path $Work 'build.bat'
  @"
@echo off
call "$vcvars" $arg >nul 2>&1
mkdir "$Work\old" 2>nul & mkdir "$Work\new" 2>nul
cd /d "$Work\old"
cl /nologo /c /O2 /MT /w /DHAVE_CONFIG_H /D_CRT_SECURE_NO_WARNINGS /I"$Pcre8" $old "$PSScriptRoot\p8_side.c" || exit /b 1
lib /nologo /OUT:..\p8.lib *.obj || exit /b 1
cd /d "$Work\new"
cl /nologo /c /O2 /MT /w /DHAVE_CONFIG_H /D_CRT_SECURE_NO_WARNINGS /I"$p2" /I"$repo\src" $new "$repo\src\mc_pcre.c" || exit /b 1
cd /d "$Work"
cl /nologo /EHsc /O2 /MT /W1 /D_CRT_SECURE_NO_WARNINGS /I"$p2" /I"$repo\src" /I"$PSScriptRoot" "$PSScriptRoot\compare.cpp" new\*.obj p8.lib /Fe:pcre_compare.exe || exit /b 1
"@ | Set-Content $bat -Encoding ascii
  & cmd /c $bat 2>&1 | Select-String -Pattern 'error|warning C4311|LNK' | ForEach-Object { $_.Line }
  if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}

& $python -I (Join-Path $PSScriptRoot 'make_corpus.py') $Work $repo @Scan
& (Join-Path $Work 'pcre_compare.exe') (Join-Path $Work 'patterns.txt') (Join-Path $Work 'subjects.txt') $InterpStride (Get-Content (Join-Path $Work 'real_count.txt'))
"exit $LASTEXITCODE"
