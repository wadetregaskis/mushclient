# tests/win/run_tests.ps1 — build and run the Windows unit-test target.
#
#   pwsh tests\win\run_tests.ps1                 # Debug
#   pwsh tests\win\run_tests.ps1 -Config Release
#   pwsh tests\win\run_tests.ps1 -NoBuild
#
# This is the Windows equivalent of `ctest --test-dir build`, and deliberately
# not CTest: the Windows build is MSBuild (MUSHclient.vcxproj), the CMake tree
# is entirely `if(NOT WIN32)`, and re-deriving the compile settings in CMake
# would mean two definitions of "how this builds on Windows".  Drift between two
# such definitions is the defect class docs/WINDOWS-VALIDATION.md exists to
# catch.  So: one project, one binary, one exit code.
#
# All the suites share one process (see tests/win/all_tests.cpp), so an abort in
# one takes the rest with it.  That is visible in the output rather than silent:
# the runner reports the exit code and whether the summary line was reached.

param(
  [ValidateSet('Debug','Release')] [string]$Config = 'Debug',
  [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found; is Visual Studio installed?" }
$vsroot = & $vswhere -latest -products * -property installationPath
$vcvars = Join-Path $vsroot 'VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path $vcvars)) { throw "vcvarsall.bat not found under $vsroot" }

if (-not $NoBuild) {
  Write-Host "building MUSHclient_tests.vcxproj ($Config|Win32) ..."
  $bat = Join-Path $env:TEMP "mc_build_tests_$PID.bat"
  @"
@echo off
call "$vcvars" x86 >nul 2>&1
cd /d "$root"
msbuild MUSHclient_tests.vcxproj /p:Configuration=$Config /p:Platform=Win32 /p:SolutionDir=$root\ /m /nologo /v:minimal
"@ | Set-Content $bat -Encoding ascii
  & cmd /c $bat
  $rc = $LASTEXITCODE
  Remove-Item $bat -Force -ErrorAction SilentlyContinue
  if ($rc -ne 0) { Write-Host "BUILD FAILED ($rc)"; exit $rc }
}

$exe = Join-Path $root 'WinTest\mushclient_tests.exe'
if (-not (Test-Path $exe)) { throw "no test binary at $exe" }

# Run from the directory holding the runtime assets (lua5.1.dll, locale\en.dll,
# spellchecker.lua ...); the engine resolves them relative to the executable's
# working directory, and a bare directory looks exactly like a broken build.
$assets = Join-Path $root 'WinDebug'
if (-not (Test-Path (Join-Path $assets 'lua5.1.dll'))) {
  Write-Host "WARNING: $assets has no lua5.1.dll - stage the runtime assets there (see docs/WINDOWS-VALIDATION.md section 2)"
}
# How many TEST() the enabled files declare, printed just ahead of the run so it
# sits beside the binary's own "N passed, M failed" summary.
#
# A suite that compiles to NOTHING still links, still runs, and still reports a
# clean exit -- which is how two suites in this target once fell through to a
# "Lua unavailable" stub and would have passed vacuously
# (docs/WINDOWS-VALIDATION.md section 10j).  Registration happens at static-init
# time, so whatever the preprocessor removed is simply not there to count, and
# the gap between these two numbers is exactly the tests guarded off Windows.
#
# Printed rather than asserted, and deliberately NOT captured from the run: the
# expected gap is a fact about the tree that a magic number here would only rot,
# and piping the binary's output would reintroduce the stdout/stderr reordering
# that section 10k had to fix with setvbuf.
$declared = 0
foreach ($m in (Select-String -Path 'MUSHclient_tests.vcxproj' `
                              -Pattern 'ClCompile Include="(tests\\test_[^"]+)"' -AllMatches)) {
  foreach ($g in $m.Matches) {
    $f = Join-Path $root $g.Groups[1].Value
    if (Test-Path $f) { $declared += (Select-String -Path $f -Pattern '^TEST\(').Count }
  }
}
Write-Host "TEST() declared by the enabled files: $declared"
Write-Host ""

Copy-Item $exe $assets -Force
Push-Location $assets
try {
  & .\mushclient_tests.exe
  $rc = $LASTEXITCODE
} finally { Pop-Location }

Write-Host ""
Write-Host "exit code: $rc"
exit $rc
