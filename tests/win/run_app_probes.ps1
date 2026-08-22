# tests/win/run_app_probes.ps1 -- run the in-app probes against the REAL GUI app.
#
#   powershell -File tests\win\run_app_probes.ps1
#   powershell -File tests\win\run_app_probes.ps1 -NoBuild
#   powershell -File tests\win\run_app_probes.ps1 -Probe imageops
#
# This is the Windows counterpart of the macOS in-app selftests, and it exists
# for the same reason: MUSHclient_tests.vcxproj constructs a CMUSHclientDoc with
# no window, so a CMUSHView, a CSendView, a CMainFrame and real GDI are all
# unreachable from it.  Everything that only works when those exist -- the
# miniwindow drawing ops, the GetInfo window family, the command bar, the
# clipboard -- has no coverage there at all.
#
# It is deliberately NOT part of run_tests.ps1.  That runner drives a headless
# console binary whose failure mode is a bad assertion; this one launches a
# windowed app whose failure mode is a stuck modal.  Folding them together would
# let one hung dialog take down 194 healthy suites.
#
# How a probe works: a generated world file names a generated bootstrap script,
# which dofile()s the probe and registers a one-shot active_closed timer that
# calls probeMain two seconds later -- AFTER the view exists, which is the whole
# point.  The probe writes PASS/FAIL lines and one SUMMARY line, and this script
# counts them.  A missing SUMMARY is a crash, not a pass.
#
# Four hazards, each of which cost a run when this was first done by hand
# (docs/WINDOWS-VALIDATION.md section 10aw):
#
#   * The world attribute is script_filename, not script_file.  Getting it wrong
#     puts up a message box naming the line and otherwise looks like silence.
#   * A hand-written world file MUST say defaults="y".  Without it every omitted
#     boolean loads FALSE -- enable_timers included -- so the deferred probe
#     never runs, and the empty report looks exactly like a crash.  That
#     behaviour is deliberate; the reason is commented at
#     xml/xml_load_world.cpp:1046.
#   * Never SendMessage(BM_CLICK) into a modal from outside the process.  It
#     ends the modal loop inside the send, and MFC then trips
#     ASSERT(ContinueModal()) at wincore.cpp:4659 on its next pump.  Post
#     WM_LBUTTONDOWN/WM_LBUTTONUP instead.
#   * The app writes preferences.  They are backed up before the first launch
#     and restored in a finally block, so a failure cannot leave them modified.

param(
  [ValidateSet('Debug','Release')] [string]$Config = 'Debug',
  [switch]$NoBuild,
  [string]$Probe = '',
  # Per-probe wall clock.  A probe's own work is milliseconds; the budget is
  # almost entirely the app's startup and the two-second timer.
  [int]$TimeoutSec = 45
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root

$probeDir = Join-Path $root 'tests\win\app_probes'
$outDir = 'WinDebug'
if ($Config -eq 'Release') { $outDir = 'WinRelease' }
$appDir = Join-Path $root $outDir
$exe    = Join-Path $appDir 'MUSHclient.exe'
$work   = Join-Path ([System.IO.Path]::GetTempPath()) ("mc_probes_" + $PID)

if (-not $NoBuild) {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  $vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
  $msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
  Write-Host "building MUSHclient.vcxproj ($Config|Win32) ..."
  & $msbuild (Join-Path $root 'MUSHclient.vcxproj') -p:Configuration=$Config -p:Platform=Win32 -m -v:q -nologo
  if ($LASTEXITCODE -ne 0) { throw "build failed" }
}
if (-not (Test-Path $exe)) { throw "no app binary at $exe" }

New-Item -ItemType Directory -Force $work | Out-Null

# ---- preferences: backed up before the first launch, restored come what may --
$prefs = Join-Path $appDir 'mushclient_prefs.sqlite'
$prefsBackup = Join-Path $work 'prefs.backup'
if (Test-Path $prefs) { Copy-Item $prefs $prefsBackup -Force }

Add-Type @'
using System;using System.Text;using System.Runtime.InteropServices;
public class P{
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumWindowsProc cb, IntPtr l);
 public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
 [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
 [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
 // POST, never SEND.  A SendMessage(BM_CLICK) from outside the process ends the
 // modal loop inside the send, and MFC asserts ContinueModal() on its next pump.
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
'@

# Answer whatever the app puts up -- the first-run "Thanks for upgrading" box and
# the Tip of the Day, mostly.
#
# READ BEFORE DISMISSING.  Anything whose title is not on the benign list gets
# its full contents printed first: a "Scripting Error" box is the probe telling
# you exactly what went wrong, and swallowing it leaves a timeout with no cause.
# The first run of this runner did precisely that.
$benignDialogs = @('Thanks for upgrading MUSHclient!', 'Tip of the Day')

function Dismiss-Dialogs([int[]]$procIds) {
  $script:dismissed = 0
  [P]::EnumWindows({param($h,$l)
    $q = 0; [P]::GetWindowThreadProcessId($h, [ref]$q) | Out-Null
    $c = New-Object Text.StringBuilder 256; [P]::GetClassName($h, $c, 256) | Out-Null
    if ($procIds -contains $q -and $c.ToString() -eq '#32770') {
      $t = New-Object Text.StringBuilder 512; [P]::GetWindowText($h, $t, 512) | Out-Null
      $title = $t.ToString()
      if ($benignDialogs -notcontains $title) {
        Write-Host ("    !! unexpected dialog: " + $title)
        [P]::EnumChildWindows($h, {param($ch,$cl)
          $s = New-Object Text.StringBuilder 4096
          [P]::GetWindowText($ch, $s, 4096) | Out-Null
          if ($s.Length -gt 0) { Write-Host ("       | " + $s.ToString()) }
          return $true }, [IntPtr]::Zero) | Out-Null
      }
      foreach ($id in 1, 2, 6) {                       # IDOK, IDCANCEL, IDYES
        $b = [P]::GetDlgItem($h, $id)
        if ($b -ne [IntPtr]::Zero) {
          [P]::PostMessage($b, 0x0201, [IntPtr]1, [IntPtr]::Zero) | Out-Null
          [P]::PostMessage($b, 0x0202, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
          Write-Host ("    dismissed: " + $title)
          $script:dismissed++
          break
        }
      }
    }
    return $true }, [IntPtr]::Zero) | Out-Null
  return $script:dismissed
}

$probes = @(Get-ChildItem (Join-Path $probeDir '*.lua') |
            Where-Object { $_.BaseName -ne 'harness' } |
            Where-Object { $Probe -eq '' -or $_.BaseName -eq $Probe })
if ($probes.Count -eq 0) { throw "no probes matched '$Probe'" }

# The probes resolve the harness and their output directory from these.
$env:MC_PROBE_DIR = ($probeDir -replace '\\', '/')
$env:MC_PROBE_OUT = ($work -replace '\\', '/')

$totalPass = 0
$totalFail = 0
$crashed = @()

try {
  foreach ($p in $probes) {
    Write-Host ""
    Write-Host ("== " + $p.BaseName)
    $report = Join-Path $work ($p.BaseName + '.txt')
    Remove-Item $report -Force -ErrorAction SilentlyContinue

    $lua = ($p.FullName -replace '\\', '/')
    $boot = (Join-Path $work ($p.BaseName + '_boot.lua'))
    $bootFwd = ($boot -replace '\\', '/')

    # The probe file only DEFINES probeMain; the bootstrap is what defers it
    # until the view exists.  Flags 1 + 4 + 32 = enabled, one-shot, and active
    # while the world is closed -- the last is required, since these worlds
    # never connect.
    #
    # Two things here are load-bearing:
    #
    #  * The paths arrive as GLOBALS baked in here, not through os.getenv.  The
    #    scripting sandbox's environment access is not something to depend on,
    #    and the runner already knows both paths.
    #  * The dofile is wrapped in pcall and a failure WRITES A REPORT.  A load
    #    error otherwise surfaces only as a "Scripting Error" message box, whose
    #    child controls expose the field labels and not the message, so the run
    #    reads as an unexplained timeout.  That cost the first two runs of this
    #    runner.
    $nl = [Environment]::NewLine
    $bootText =
      "MC_PROBE_DIR = [[$($probeDir -replace '\\','/')]]" + $nl +
      "MC_PROBE_OUT = [[$($work -replace '\\','/')]]" + $nl +
      "local ok, err = pcall (dofile, [[$lua]])" + $nl +
      "if ok then" + $nl +
      '  world.AddTimer ("probe", 0, 0, 2.0, "", 1 + 4 + 32, "probeMain")' + $nl +
      "else" + $nl +
      "  local f = io.open (MC_PROBE_OUT .. [[/$($p.BaseName).txt]], 'w')" + $nl +
      "  f:write ('FAIL load: ', tostring (err), '\n')" + $nl +
      "  f:write ('SUMMARY 0 passed, 1 failed\n')" + $nl +
      "  f:close ()" + $nl +
      "end" + $nl
    # NO BOM.  Windows PowerShell 5.1's `Set-Content -Encoding utf8` writes one,
    # and Lua's loader rejects a script that starts with it -- which surfaces
    # only as a "Scripting Error" box whose controls expose field labels and
    # not the message.  That cost three runs.
    [System.IO.File]::WriteAllText($boot, $bootText, (New-Object System.Text.UTF8Encoding $false))

    $mclText = @"
<?xml version="1.0" encoding="ISO-8859-1"?>
<!DOCTYPE muclient>
<muclient>
<world defaults="y" name="Probe_$($p.BaseName)" site="127.0.0.1" port="65099"
       connect_method="0" enable_scripts="y" script_language="Lua"
       script_filename="$bootFwd" world_file_version="18">
</world>
</muclient>
"@
    $mcl = Join-Path $work ($p.BaseName + '.mcl')
    [System.IO.File]::WriteAllText($mcl, $mclText, (New-Object System.Text.UTF8Encoding $false))

    $proc = Start-Process -FilePath $exe -ArgumentList "`"$mcl`"" -WorkingDirectory $appDir -PassThru
    $deadline = [datetime]::UtcNow.AddSeconds($TimeoutSec)
    $done = $false
    while ([datetime]::UtcNow -lt $deadline) {
      Start-Sleep -Milliseconds 700
      Dismiss-Dialogs @($proc.Id) | Out-Null
      # The report file appears at open(), so its EXISTENCE is not completion.
      # SUMMARY is the last thing written, and is.
      if (Test-Path $report) {
        $sofar = Get-Content $report -Raw -ErrorAction SilentlyContinue
        if ($sofar -and $sofar -match 'SUMMARY') { $done = $true; break }
      }
      if ($proc.HasExited) { break }
    }
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    $proc.WaitForExit(5000) | Out-Null

    if (-not $done) {
      Write-Host ("  CRASHED or TIMED OUT (no SUMMARY): " + $p.BaseName)
      if (Test-Path $report) { Get-Content $report | ForEach-Object { Write-Host ("  " + $_) } }
      $crashed += $p.BaseName
      continue
    }

    $lines = Get-Content $report
    $lines | Where-Object { $_ -match '^FAIL' } | ForEach-Object { Write-Host ("  " + $_) }
    $sum = $lines | Where-Object { $_ -match '^SUMMARY (\d+) passed, (\d+) failed' } | Select-Object -Last 1
    if ($sum -match '^SUMMARY (\d+) passed, (\d+) failed') {
      $totalPass += [int]$Matches[1]
      $totalFail += [int]$Matches[2]
      Write-Host ("  " + $sum)
    }
  }
} finally {
  # Unconditional.  This is the only mode that runs the app far enough to write
  # preferences, and the user's own copy of MUSHclient shares the format.
  if (Test-Path $prefsBackup) { Copy-Item $prefsBackup $prefs -Force }
  Get-Process MUSHclient -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "$totalPass passed, $totalFail failed, across $($probes.Count) probes"
if ($crashed.Count -gt 0) { Write-Host ("crashed: " + ($crashed -join ', ')) }
if ($totalFail -gt 0 -or $crashed.Count -gt 0) { exit 1 }
exit 0
