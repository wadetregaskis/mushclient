# tests/win/run_tests.ps1 -- build and run the Windows unit-test target.
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
  [switch]$NoBuild,
  # Per-suite wall clock.  Generous: the slowest honest suite is a few seconds,
  # so anything near this is hung rather than slow.
  [int]$SuiteTimeoutMs = 120000
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
# sits beside the binary's own totals.
#
# A suite that compiles to NOTHING still links, still runs, and still reports a
# clean exit -- which is how two suites in this target once fell through to a
# "Lua unavailable" stub and would have passed vacuously
# (docs/WINDOWS-VALIDATION.md section 10j).  Registration happens at static-init
# time, so whatever the preprocessor removed is simply not there to count, and
# the gap between these two numbers is exactly the tests guarded off Windows.
$declared = 0
foreach ($m in (Select-String -Path 'MUSHclient_tests.vcxproj' `
                              -Pattern 'ClCompile Include="(tests.[^"]*test_[^"]+)"' -AllMatches)) {
  foreach ($g in $m.Matches) {
    $f = Join-Path $root $g.Groups[1].Value
    if (Test-Path $f) { $declared += (Select-String -Path $f -Pattern '^TEST\(').Count }
  }
}
Write-Host "TEST() declared by the enabled files: $declared"


# Run the test binary once, with a clock on it, and hand back stdout and stderr
# separately.
#
# Separately because the two carry different things and one of the callers must
# not see both: --list-suites prints suite names on stdout while MFC's leak
# report goes to stderr, and a runner that concatenated them would take
# "0 bytes in 0 Free Blocks." for a suite name and launch a process for it.
#
# With a clock because EVERY wait on this binary needs one.  That was learned
# twice in an hour: first on the suites (an unanswerable modal dialog, section
# 10r), then on --list-suites itself, which looked too trivial to guard and was
# the thing that hung (section 10u).
function Invoke-TestExe {
  param([string[]]$ExeArgs, [int]$TimeoutMs)
  $o = "$env:TEMP\mc_run_$PID.out"
  $e = "$env:TEMP\mc_run_$PID.err"
  Remove-Item $o, $e -Force -ErrorAction SilentlyContinue
  $j = Start-Process -FilePath '.\mushclient_tests.exe' -ArgumentList $ExeArgs `
                     -RedirectStandardOutput $o -RedirectStandardError $e `
                     -PassThru -NoNewWindow
  # Touch .Handle before waiting.  Start-Process -PassThru returns a Process
  # object with no cached handle, and both WaitForExit(ms) and ExitCode
  # misbehave without one -- the timeout silently never fires, which reads
  # exactly like the run being slow.  Nothing in either signature says so.
  $null = $j.Handle
  $ok = $j.WaitForExit($TimeoutMs)
  if (-not $ok) { $j.Kill() | Out-Null; $j.WaitForExit(5000) | Out-Null }
  $out = @(); $err = @()
  if (Test-Path $o) { $out = @(Get-Content $o) }
  if (Test-Path $e) { $err = @(Get-Content $e) }
  Remove-Item $o, $e -Force -ErrorAction SilentlyContinue
  [pscustomobject]@{ TimedOut = (-not $ok); Out = $out; Err = $err }
}

Copy-Item $exe $assets -Force
Push-Location $assets
try {
  # ---- One process per suite --------------------------------------------
  # Not one process for everything.  All the enabled tests/test_*.cpp link into
  # a single executable (tests/win/all_tests.cpp), which trades link time for
  # isolation -- and its own comment says "suites are no longer isolated from
  # each other's global state ... if a suite ever needs isolation, give it its
  # own vcxproj."  Six suites turned out to need it: each compiled and linked,
  # then killed the process partway through, always inside a DIFFERENT suite
  # from the one just added (section 10t).
  #
  # Launching the one binary once per suite is the cheap half of that vcxproj.
  # It costs a process start per suite and no extra link, and it restores the
  # property macOS gets for free by building one executable per test file: a
  # suite that corrupts process state takes only itself down, and SAYS SO,
  # instead of killing an unrelated suite thirty tests later.
  $ls = Invoke-TestExe -ExeArgs @('--list-suites') -TimeoutMs $SuiteTimeoutMs
  if ($ls.TimedOut) {
    throw "--list-suites did not return; the binary is hung before printing anything. On this host that is almost always an invisible modal dialog (section 10q)."
  }
  # stdout only, and only lines shaped like a C identifier: a suite name is a
  # TEST() first argument, so anything else on the stream is not one.
  $suites = @($ls.Out | Where-Object { $_ -match '^[A-Za-z_][A-Za-z0-9_]*$' })
  if (-not $suites.Count) {
    throw "--list-suites printed no suite names; is the binary current? ($($ls.Out.Count) stdout lines, $($ls.Err.Count) stderr lines)"
  }

  $passed = 0; $failed = 0; $crashed = @(); $blocked = @()
  foreach ($suite in $suites) {
    $r = Invoke-TestExe -ExeArgs @("--suite=$suite") -TimeoutMs $SuiteTimeoutMs
    if ($r.TimedOut) {
      $blocked += $suite
      Write-Host "  BLOCKED (killed after $([int]($SuiteTimeoutMs/1000))s): $suite"
      continue
    }
    $all = @($r.Out) + @($r.Err)
    $sum = $all | Where-Object { $_ -match '^\s*(\d+) passed, (\d+) failed' } | Select-Object -Last 1
    if ($sum -match '^\s*(\d+) passed, (\d+) failed') {
      $passed += [int]$Matches[1]; $failed += [int]$Matches[2]
    } else {
      # No summary line means the process died before printing one.  Report the
      # suite by name: the whole point of this loop is that a failure names its
      # own culprit rather than an innocent bystander thirty tests later.
      $crashed += $suite
      Write-Host "  CRASHED (no summary): $suite"
    }
    $all | Where-Object { $_ -match '\[FAIL\]|uncaught' } | ForEach-Object { Write-Host $_ }
  }
} finally { Pop-Location }

Write-Host ""
Write-Host "$passed passed, $failed failed, across $($suites.Count) suites"
if ($crashed.Count) { Write-Host "CRASHED (no summary line): $($crashed -join ', ')" }
if ($blocked.Count) { Write-Host "BLOCKED (killed on timeout): $($blocked -join ', ')" }
if ($crashed.Count -or $blocked.Count) { exit 2 }
if ($failed) { exit 1 }
exit 0
