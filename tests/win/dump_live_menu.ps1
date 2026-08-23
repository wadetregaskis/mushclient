# tests/win/dump_live_menu.ps1 — the menu bar of the RUNNING MUSHclient, with
# each item's enabled and checked state.
#
#   powershell -File tests\win\dump_live_menu.ps1
#
# Complements dump_menu_resource.ps1, which reads the resource: this reads what
# the user is actually being offered, right now, in whatever state the client is
# in. That difference is the whole point — the resource says an item exists, and
# only this says whether it is greyed or ticked.
#
# The load-bearing line is the WM_INITMENUPOPUP send. Without it the flags are
# whatever the resource template carried; MFC runs the ON_UPDATE_COMMAND_UI
# handlers from that message and nowhere else, so a dump taken without it
# reports the wrong answer confidently. (tests/win/test_menu_rules_win.cpp asks
# the same question through CCmdUI::DoUpdate with no window at all; this is the
# end-to-end check on that.)
#
# Requires a running client. Start one with a world open if you want the
# world-scoped items to mean anything — with no world open most of the list is
# greyed, which is itself the property §10am pins.
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class L {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Ansi)] public static extern int GetClassNameA(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr m, int pos);
  [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr m);
  [DllImport("user32.dll")] public static extern int GetMenuItemID(IntPtr m, int pos);
  [DllImport("user32.dll", CharSet=CharSet.Ansi)] public static extern int GetMenuStringA(IntPtr m, int pos, StringBuilder s, int n, int flags);
  [DllImport("user32.dll")] public static extern int GetMenuState(IntPtr m, int pos, int flags);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageA(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static string Cls(IntPtr h){ var s=new StringBuilder(64); GetClassNameA(h,s,64); return s.ToString(); }
  public static string Str(IntPtr m,int i){ var s=new StringBuilder(512); GetMenuStringA(m,i,s,512,0x0400); return s.ToString(); }
}
"@

$proc = Get-Process MUSHclient -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { Write-Host "no MUSHclient running"; exit 1 }
$target = $proc.Id

$frame = [IntPtr]::Zero
$cb = [L+EnumProc]{
  param($h,$l)
  [uint32]$q = 0
  [void][L]::GetWindowThreadProcessId($h,[ref]$q)
  if ($q -eq $target -and [L]::Cls($h).StartsWith('Afx:') -and [L]::GetMenu($h) -ne [IntPtr]::Zero) {
    $script:frame = $h; return $false
  }
  return $true
}
[void][L]::EnumWindows($cb,[IntPtr]::Zero)
if ($frame -eq [IntPtr]::Zero) { Write-Host "no framed window found"; exit 1 }

$bar = [L]::GetMenu($frame)
# WM_INITMENUPOPUP is what makes MFC run the update-UI handlers; without it the
# enabled/checked flags are whatever the resource had.
$n = [L]::GetMenuItemCount($bar)
for ($i = 0; $i -lt $n; $i++) {
  $sub = [L]::GetSubMenu($bar, $i)
  $top = ([L]::Str($bar,$i) -replace '&','')
  if ($sub -eq [IntPtr]::Zero) { continue }
  [void][L]::SendMessageA($frame, 0x0117, $sub, [IntPtr]($i -bor (0 -shl 16)))   # WM_INITMENUPOPUP
  $cnt = [L]::GetMenuItemCount($sub)
  for ($j = 0; $j -lt $cnt; $j++) {
    $txt = [L]::Str($sub,$j)
    if (-not $txt) { continue }
    $st  = [L]::GetMenuState($sub, $j, 0x0400)
    $id  = [L]::GetMenuItemID($sub, $j)
    $flags = @()
    if ($st -band 0x0003) { $flags += 'GREY' }      # MF_GRAYED|MF_DISABLED
    if ($st -band 0x0008) { $flags += 'CHECKED' }   # MF_CHECKED
    "{0,6}  {1,-10} {2,-30} {3}" -f $id, ($flags -join ','), ($top + ' >'), $txt
  }
}
