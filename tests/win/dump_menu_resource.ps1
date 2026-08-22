# tests/win/dump_menu_resource.ps1 — the menu the Windows client actually shows,
# as text, so the port's hand-built menu bar can be diffed against it.
#
#   powershell -File tests\win\dump_menu_resource.ps1 -ResId 129 -Out world.txt
#
# Three resources matter (resource.h):
#   128  IDR_MAINFRAME    the menu with NO world open      (44 commands)
#   129  IDR_MUSHCLTYPE   the world menu                  (147 commands)
#   130  IDR_NORMALTYPE   the notepad menu                 (79 commands)
#
# MUSHclient.rc carries no menus at all — they live in locale\en.dll, which
# CMUSHclientApp::I18N_Setup loads at runtime (docs/WINDOWS-VALIDATION.md §10ac).
# LOAD_LIBRARY_AS_DATAFILE is enough: LoadMenu only needs the resource.
#
# Diff by PATH AND CAPTION, not by id.  The port renumbers a good many commands
# onto ids of its own (ID_MENU_VIEW_ALWAYSONTOP, ID_MENU_GAME_LUA_EVAL, the
# port-local 40001 notepad, wxID_* for the standard File/Edit verbs), so an
# id-set comparison reports ~67 phantom gaps out of 147.  §10an has the detail.
param([int]$ResId = 129, [string]$Out = "winmenu.txt",
      [string]$Dll = "WinDebug\locale\en.dll")

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class M {
  [DllImport("kernel32.dll", CharSet=CharSet.Ansi, SetLastError=true)] public static extern IntPtr LoadLibraryExA(string f, IntPtr h, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr LoadMenuA(IntPtr h, IntPtr id);
  [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr m);
  [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr m, int pos);
  [DllImport("user32.dll")] public static extern int GetMenuItemID(IntPtr m, int pos);
  [DllImport("user32.dll", CharSet=CharSet.Ansi)] public static extern int GetMenuStringA(IntPtr m, int pos, StringBuilder s, int n, int flags);
  public static string Str(IntPtr m, int pos){ var s=new StringBuilder(512); GetMenuStringA(m,pos,s,512,0x0400); return s.ToString(); }
}
"@

$script:rows = New-Object System.Collections.ArrayList

function Walk([IntPtr]$m, [string]$path) {
  $n = [M]::GetMenuItemCount($m)
  for ($i = 0; $i -lt $n; $i++) {
    $txt = [M]::Str($m, $i)
    $sub = [M]::GetSubMenu($m, $i)
    if ($sub -ne [IntPtr]::Zero) {
      Walk $sub ($path + ($txt -replace '&','') + " > ")
    } else {
      $id = [M]::GetMenuItemID($m, $i)
      if ($id -ne 0 -and $id -ne -1) {
        [void]$script:rows.Add([pscustomobject]@{ Id = $id; Path = $path; Text = $txt })
      }
    }
  }
}

$h = [M]::LoadLibraryExA($Dll, [IntPtr]::Zero, 0x00000002)   # LOAD_LIBRARY_AS_DATAFILE
if ($h -eq [IntPtr]::Zero) { throw "cannot load $Dll" }
$menu = [M]::LoadMenuA($h, [IntPtr]$ResId)
if ($menu -eq [IntPtr]::Zero) { throw "no menu resource $ResId in $Dll" }

Walk $menu ""
$script:rows | Sort-Object Id |
  ForEach-Object { "{0,6}  {1}{2}" -f $_.Id, $_.Path, $_.Text } |
  Out-File -Encoding utf8 $Out
Write-Host "wrote $Out : $($script:rows.Count) commands from resource $ResId"
