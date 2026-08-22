# Open each world-properties page in the RUNNING client, press OK, save the
# world, and diff the file against the previous save.
#
# The invariant: a page the user opens and OKs without touching anything must
# not change the world.  Any diff is a page whose DDX reads a setting one way
# and writes it back another -- the round-trip defect class the 2026-07 settings
# audit was built around, asked here of real MFC DDX rather than of the port's
# bridge.
#
#   powershell -File tests/win/drive_world_prefs.ps1 -World <path.mcl>
#
# Two things make a run of this LIE, both learned the hard way (10aq):
#   * date_saved and the "Saved on" comment differ between two identical saves,
#     so they are normalised out -- otherwise the invariant is a clock.
#   * a POSTED WM_COMMAND(IDOK) closes a property sheet but DoModal returns
#     IDCANCEL, so the save half never runs and every page reports clean for the
#     wrong reason.  The OK has to be a BM_CLICK on the real button.
param(
  [Parameter(Mandatory=$true)][string]$World,
  [string]$Exe  = "C:\Users\shive\Documents\mushclient\mushclient\WinDebug\MUSHclient.exe",
  [string]$Cwd  = "C:\Users\shive\Documents\mushclient\mushclient\WinDebug"
)

Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class P {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Ansi)] public static extern int GetClassNameA(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Ansi)] public static extern int GetWindowTextA(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr PostMessageA(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public static string Cls(IntPtr h){ var s=new StringBuilder(64); GetClassNameA(h,s,64); return s.ToString(); }
  public static string Txt(IntPtr h){ var s=new StringBuilder(512); GetWindowTextA(h,s,512); return s.ToString(); }
}
"@

# date_saved is stamped on every save and on every section, so it differs
# between two identical saves.  Normalising it out is the difference between an
# invariant and a clock.
function Norm([string]$s) { return (($s -replace 'date_saved="[^"]*"', 'date_saved="X"') -replace '<!-- Saved on [^>]*-->', '<!-- Saved on X -->') }

$WM_COMMAND = 0x0111
$IDOK       = 1

function Get-Frame {
  $script:found = [IntPtr]::Zero
  $cb = [P+EnumProc]{
    param($h,$l)
    [uint32]$q=0; [void][P]::GetWindowThreadProcessId($h,[ref]$q)
    if ($q -eq $script:McPid -and [P]::Cls($h).StartsWith('Afx:') -and [P]::GetMenu($h) -ne [IntPtr]::Zero) { $script:found=$h; return $false }
    return $true
  }
  [void][P]::EnumWindows($cb,[IntPtr]::Zero)
  return $script:found
}

function Get-Dialogs {
  $out = New-Object System.Collections.ArrayList
  $cb = [P+EnumProc]{
    param($h,$l)
    [uint32]$q=0; [void][P]::GetWindowThreadProcessId($h,[ref]$q)
    if ($q -eq $script:McPid -and [P]::Cls($h) -eq '#32770' -and [P]::IsWindowVisible($h)) {
      [void]$out.Add([pscustomobject]@{ H=$h; Text=[P]::Txt($h) })
    }
    return $true
  }
  [void][P]::EnumWindows($cb,[IntPtr]::Zero)
  return $out
}

# The Configure submenu, by resource id.  Only pages -- nothing destructive.
$pages = [ordered]@{
  32295 = 'MUD name/IP address'
  32344 = 'Connecting'
  32297 = 'Logging'
  32298 = 'Info'
  32299 = 'Notes'
  32300 = 'Output'
  32301 = 'ANSI Colours'
  32345 = 'Custom Colours'
  32304 = 'Printing'
  32305 = 'Commands'
  32306 = 'Keypad'
  32307 = 'Macros'
  32308 = 'Auto say'
  32309 = 'Paste to world'
  32310 = 'Send file'
  32311 = 'Scripting'
  32312 = 'Variables'
  32313 = 'Triggers'
  32314 = 'Aliases'
  32315 = 'Timers'
  32972 = 'MXP / Pueblo'
  33025 = 'Chat'
}

Get-Process MUSHclient -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600

$proc = Start-Process -FilePath $Exe -ArgumentList "`"$World`"" -WorkingDirectory $Cwd -PassThru
Start-Sleep -Seconds 4
$script:McPid = $proc.Id

# Clear anything modal that came up at start-up (Tip of the Day, upgrade splash).
foreach ($d in (Get-Dialogs)) { [void][P]::PostMessageA($d.H, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
Start-Sleep -Seconds 2

$frame = Get-Frame
if ($frame -eq [IntPtr]::Zero) { Write-Host "FATAL: no framed window"; exit 1 }

# Baseline: save once so the file is in the client's own normal form, then again
# so we know a no-op save is stable before blaming any page for a diff.
[void][P]::PostMessageA($frame, $WM_COMMAND, [IntPtr]57603, [IntPtr]::Zero)   # ID_FILE_SAVE
Start-Sleep -Seconds 2
$baseline = Norm (Get-Content $World -Raw)
[void][P]::PostMessageA($frame, $WM_COMMAND, [IntPtr]57603, [IntPtr]::Zero)
Start-Sleep -Seconds 2
if ((Norm (Get-Content $World -Raw)) -ne $baseline) {
  Write-Host "UNSTABLE: two consecutive no-op saves differ; the invariant cannot be tested"
  $a = $baseline -split "`r?`n"; $b = (Norm (Get-Content $World -Raw)) -split "`r?`n"
  Compare-Object $a $b | Select-Object -First 20 | ForEach-Object { Write-Host ("   {0} {1}" -f $_.SideIndicator, $_.InputObject) }
  Get-Process -Id $script:McPid -ErrorAction SilentlyContinue | Stop-Process -Force
  exit 2
}
Write-Host ("baseline stable, {0} bytes" -f $baseline.Length)

foreach ($id in $pages.Keys) {
  $name = $pages[$id]
  [void][P]::PostMessageA($frame, $WM_COMMAND, [IntPtr]$id, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 1500

  $dlgs = Get-Dialogs
  if ($dlgs.Count -eq 0) { Write-Host ("{0,6}  {1,-22} NO DIALOG APPEARED" -f $id, $name); continue }

  # OK, not Cancel: Cancel would prove nothing about the write-back half.  And a
  # CLICK, not a posted WM_COMMAND -- see the header.
  foreach ($d in $dlgs) {
    $btn = [P]::GetDlgItem($d.H, $IDOK)
    if ($btn -ne [IntPtr]::Zero) { [void][P]::PostMessageA($btn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) }
    else { [void][P]::PostMessageA($d.H, $WM_COMMAND, [IntPtr]$IDOK, [IntPtr]::Zero) }
  }
  Start-Sleep -Milliseconds 1200

  # Anything still up (a confirmation, a validation complaint) gets closed and
  # reported -- an unexpected second dialog is itself a result.  If it will not
  # go, the sheet is still open and every row after this one would be measuring
  # nothing: say UNTESTED rather than printing "clean" for a page that was never
  # opened.  A driver that reports a pass for work it did not do is worse than
  # one that stops.
  $left = Get-Dialogs
  $extra = ''
  if ($left.Count -gt 0) {
    $msgs = @()
    foreach ($d in $left) {
      $body = ''
      $kids = New-Object System.Collections.ArrayList
      $cb2 = [P+EnumProc]{ param($c,$l); [void]$kids.Add($c); return $true }
      [void][P]::EnumChildWindows($d.H, $cb2, [IntPtr]::Zero)
      foreach ($k in $kids) { $t = [P]::Txt($k); if ($t.Length -gt $body.Length) { $body = $t } }
      $msgs += ("'" + $d.Text + "': " + ($body -replace "`r?`n"," "))
    }
    $extra = " [left open: " + ($msgs -join ' ^| ') + "]"
    # ANSWER it rather than closing it.  The Logging page asks "You are not
    # logging output from the MUD - is this intentional?"; WM_CLOSE means no,
    # which sends the user back to the page and wedges the sheet.  Yes (or OK)
    # lets the OK proceed, which is what the sweep is trying to measure.
    foreach ($d in $left) {
      $answered = $false
      foreach ($bid in 6, 1) {          # IDYES, then IDOK
        $b = [P]::GetDlgItem($d.H, $bid)
        if ($b -ne [IntPtr]::Zero) { [void][P]::PostMessageA($b, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero); $answered = $true; break }
      }
      if (-not $answered) { [void][P]::PostMessageA($d.H, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
    }
    Start-Sleep -Milliseconds 600
  }

  [void][P]::PostMessageA($frame, $WM_COMMAND, [IntPtr]57603, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 1500
  $now = Norm (Get-Content $World -Raw)

  $stuck = (Get-Dialogs).Count -gt 0
  if ($stuck) {
    Write-Host ("{0,6}  {1,-22} UNTESTED - a modal is stuck{2}" -f $id, $name, $extra)
  } elseif ($now -eq $baseline) {
    Write-Host ("{0,6}  {1,-22} clean{2}" -f $id, $name, $extra)
  } else {
    Write-Host ("{0,6}  {1,-22} CHANGED THE WORLD FILE{2}" -f $id, $name, $extra)
    $a = $baseline -split "`r?`n"
    $b = $now      -split "`r?`n"
    $d = Compare-Object $a $b | Select-Object -First 12
    foreach ($line in $d) { Write-Host ("         {0} {1}" -f $line.SideIndicator, $line.InputObject) }
    $baseline = $now     # re-baseline so the next page is attributed to itself
  }
}

Get-Process -Id $script:McPid -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host "done"
