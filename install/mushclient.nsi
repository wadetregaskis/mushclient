; -------- MUSHclient install script
; For use with NSIS installer (see https://sourceforge.net/projects/nsis/)

;
;  get a local copy of the .exe and compress it
;

;  !system "copy ..\WinRel\mushclient.exe mushclient.exe" = 0
;  !system "upx mushclient.exe" = 0

; The name of the installer
Name "MUSHclient"
Caption "MUSHclient Installer"
; BGGradient FE8100 FCFC04  771F0E
; InstallColors FF8080 000030

; Use compression
SetCompressor /SOLID lzma


; Target architecture.  Plain "makensis mushclient.nsi" is the 32-bit
; installer exactly as before.  "makensis /DARCH=x64 mushclient.nsi" (or
; /DARCH=arm64) packages WinRel-x64 (WinRel-ARM64) and the DLLs that
; packaging\windows\fetch-deps.ps1 built -- every DLL the installer ships must
; match the exe's architecture.  See docs/WINDOWS-64BIT.md.  64-bit needs
; NSIS 3.04+ (x64.nsh's IsNativeARM64).
;
; packaging\windows\build-app.ps1 drives the 64-bit builds and passes:
;   /DVERSION=6.00       names the output MUSHclient-<version>-windows-<arch>[-discworld]-setup.exe
;   /DCRTDIR=<dir>       the VC++ runtime DLLs the embedded Python and Ruby need
;   /DDISCWORLD=<dir>    the Discworld edition: Quow's plugins and maps from
;                        packaging\discworld-plugins (fetch-discworld-plugins.sh)
!ifndef ARCH
  !define ARCH x86
!endif
!if "${ARCH}" == "x86"
  !define BINDIR     "..\WinRel"
  !define LUADLL     "..\WinRel\lua5.1.dll"
  !define LUALIB     "..\src\lua5.1.lib"
  !define LOCALEDIR  "..\WinRel\locale"
  ; fetch-deps.ps1 -Arch x86 -Only luasocket
  !define SOCKETCORE "..\packaging\windows\deps\Win32\bin\socket\core.dll"
  !define MIMECORE   "..\packaging\windows\deps\Win32\bin\mime\core.dll"
  !define OUTFILE    "mushclient50x.exe"
  !define PROGDIR    "$PROGRAMFILES"
  !define APPNAME    "MUSHclient"
  ; files git does not carry, from the release tree
  !define HELPDB      "..\help.db"
  !define HLPFILE     "..\mushclient.HLP"
  !define DINAFON     "..\Dina.fon"
  !define LOCTEMPLATE "..\Localize_template.lua"
  ; The 32-bit release regenerates help.db and the translation templates
  ; first (lua5.1.exe, sqlite3 and xgettext on PATH).
  !system "..\tools\localize.bat" = 0
!else if "${ARCH}" == "x64"
  !define PLAT x64
!else if "${ARCH}" == "arm64"
  !define PLAT ARM64
!else
  !error "ARCH must be x86, x64 or arm64"
!endif
!ifdef PLAT
  !define DEPS       "..\packaging\windows\deps\${PLAT}"
  !define BINDIR     "..\WinRel-${PLAT}"
  !define LUADLL     "${DEPS}\bin\lua5.1.dll"
  !define LUALIB     "${DEPS}\lib\lua5.1.lib"
  ; locale\en.dll is built with the client (resources\en.vcxproj)
  !define LOCALEDIR  "${BINDIR}\locale"
  !define SOCKETCORE "${DEPS}\bin\socket\core.dll"
  !define MIMECORE   "${DEPS}\bin\mime\core.dll"
  !ifdef DISCWORLD
    !define EDITION "-discworld"
  !else
    !define EDITION ""
  !endif
  !ifdef VERSION
    !define OUTFILE  "MUSHclient-${VERSION}-windows-${ARCH}${EDITION}-setup.exe"
  !else
    !define OUTFILE  "mushclient50x-${ARCH}${EDITION}.exe"
  !endif
  !define PROGDIR    "$PROGRAMFILES64"
  !define APPNAME    "MUSHclient (${PLAT})"
  ; No help.db, WinHelp file or translation template: they came only from
  ; the 32-bit release tree (help.db is generated from documentation.sql,
  ; which is not in the repository; Windows 10 and 11 cannot open WinHelp; the
  ; template is tools\localize.bat's output, a translator's tool).  Dina is its
  ; author's own release, fetched by fetch-deps.ps1.
  !define DINAFON     "..\packaging\windows\deps\common\Dina.fon"
  !define DINALICENCE "..\packaging\windows\deps\common\Dina_license.txt"
  !ifndef CRTDIR
    !error "64-bit builds need /DCRTDIR=<the VC++ redist's Microsoft.VC143.CRT directory> (build-app.ps1 passes it)"
  !endif
  !include "x64.nsh"
  !include "LogicLib.nsh"
!endif

; The file to write
OutFile ${OUTFILE}

; Modern interface settings
!include "MUI.nsh"

!define MUI_ABORTWARNING
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install-colorful.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall-colorful.ico"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; Set languages (first is default language)
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_RESERVEFILE_LANGDLL


; The default installation directory
InstallDir ${PROGDIR}\MUSHclient

; This version's own keys and Start menu folder: MUSHclient 5 used
; "Gammon Software Solutions\MUSHclient", the "MUSHclient" uninstall entry and a
; "MUSHclient" Start menu folder, and 6.00 sharing them would take over -- and on
; uninstalling, delete -- a 5.x installed alongside it.
!define REGKEY    "Software\MUSHclient"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\MUSHclient 6"
!define SMDIR     "$SMPROGRAMS\MUSHclient 6"

; Registry key to check for directory (so if you install again, it will
; overwrite the old one automatically)
InstallDirRegKey HKCU "${REGKEY}" "InstallDir"

; The text to prompt the user to enter a directory
ComponentText "This will install MUSHclient on your computer. Select which optional components that you want installed."

; The text to prompt the user to enter a directory
DirText "Setup has determined the optimal location to install. If you would like to change the directory, do so now."

; options

CRCCheck on
ShowInstDetails show
SetOverwrite on
;SetOverwrite ifnewer
SetCompress auto
SetDateSave on

; install types

InstType Typical  ; 1
InstType Full     ; 2
InstType Compact  ; 3
CheckBitmap  "${NSISDIR}\Contrib\Graphics\Checks\simple-round2.bmp"

Function .onInit

!ifdef PLAT
  ; A 64-bit exe cannot start on 32-bit Windows, and an ARM64 one only on
  ; ARM64.  (x64 runs on ARM64 Windows 11 under emulation, so it is allowed.)
  !if "${PLAT}" == "ARM64"
    ${IfNot} ${IsNativeARM64}
      MessageBox MB_OK|MB_ICONSTOP "This is the ARM64 build of MUSHclient; this computer is not ARM64."
      Abort
    ${EndIf}
  !else
    ${IfNot} ${RunningX64}
      MessageBox MB_OK|MB_ICONSTOP "This is the 64-bit build of MUSHclient; it needs 64-bit Windows."
      Abort
    ${EndIf}
  !endif
  ; The installer itself is 32-bit: without this, HKLM writes land in
  ; WOW6432Node.  (HKCU\Software is not redirected, so the preferences the
  ; 32-bit and 64-bit builds read are the same keys.)
  SetRegView 64
!endif

; splash screen

# the plugins dir is automatically deleted when the installer exits
;     InitPluginsDir
;
;    File /oname=$PLUGINSDIR\splash.bmp "MUSHclient Installer.bmp"
;    splash::show 3000 $PLUGINSDIR\splash
;
; Pop $0

FunctionEnd


; The stuff to install
Section "-MUSHclient program (required)"
  ; Set output path to the installation directory.
  SetOutPath $INSTDIR

  ; Which section type it is in
  SectionIn 1 2 3  ; typical, full, compact

    WriteUninstaller "uninstall.exe"

  ; Put files there

  File "readme.txt" ; in install directory

  File "${BINDIR}\mushclient.exe"
  ; File "..\MUSHclient.exe.manifest"

  File "${LUADLL}"
  File "${LUALIB}"
!ifdef HLPFILE
  File "${HLPFILE}"
!endif
!ifdef HELPDB
  File "${HELPDB}"
!endif
  File "..\docs\mushclient.cnt"
  File "..\install\mushclient.ico"
  File "..\res\tips.txt"
  File "..\lua\Example_filters.lua"
  File "..\res\names.txt"
  File /oname=license.txt "..\LICENSE"
  File /nonfatal "${DINAFON}"

!ifdef PLAT
  ; The embedded scripting languages (MUSHclient.engines.props): Python,
  ; Ruby and Tcl as fetch-deps.ps1 staged them (JavaScript is QuickJS, linked
  ; into the exe), and the VC++ runtime the Python and Ruby DLLs import.  The
  ; DLLs sit beside the exe because the exe imports them; each runtime finds
  ; its library beside its DLL (python\, lib\ruby\, tcl8.6\).
  File "${DEPS}\bin\python3*.dll"
  File "${DEPS}\bin\*-ruby*.dll"
  ; Only what something imports (build-app.ps1 checks): vcruntime140_1.dll is
  ; x64's C++ exception runtime (Python's _wmi.pyd), and the ARM64 redist's
  ; copy is an x64-headed ARM64X binary nothing in an ARM64 package uses.
  File "${CRTDIR}\vcruntime140.dll"
  !if "${PLAT}" == "x64"
    File "${CRTDIR}\vcruntime140_1.dll"
  !endif
  ; Replaced, not merged, on an upgrade: a stale module of an older runtime
  ; must not shadow the new one's.
  RMDir /r "$INSTDIR\python"
  RMDir /r "$INSTDIR\lib\ruby"
  RMDir /r "$INSTDIR\tcl8.6"
  File /r "${DEPS}\bin\python"
  SetOutPath $INSTDIR\lib
  File /r "${DEPS}\bin\lib\ruby"
  SetOutPath $INSTDIR
  File /r "${DEPS}\bin\tcl8.6"
!endif

  ; Write the installation path into the registry -- 6.00's own key, not
  ; 5.x's "Gammon Software Solutions\MUSHclient": 6.00 does not share settings
  ; with 5.x, and does not install over it unless told to
  WriteRegStr HKCU "${REGKEY}" "InstallDir" $INSTDIR

  ; Write the uninstall keys for Windows (likewise 6.00's own, so a 5.x
  ; alongside keeps its entry in Apps)
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "Publisher" "Nick Gammon"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" "$INSTDIR\uninstall.exe"

  ; The folders a portable copy -- the zip, which build-app.ps1 makes from this
  ; installer's payload -- keeps the user's files in.  An installed copy keeps
  ; them in %APPDATA%\MUSHclient\ instead (src/user_data.h), since a program
  ; under Program Files cannot write beside itself; 6.00 no longer writes the
  ; folders' names into the registry, where they were 5.x's preferences and
  ; pointed 5.x into this folder.  The bundled plugins under worlds\ are copied
  ; from here into the data folder when this version first runs.
  CreateDirectory "$INSTDIR\worlds"
  CreateDirectory "$INSTDIR\worlds\plugins"
  CreateDirectory "$INSTDIR\worlds\plugins\state"
  CreateDirectory "$INSTDIR\logs"

  ;
  ;  directory for locale information
  ;
  CreateDirectory "$INSTDIR\locale"
  SetOutPath $INSTDIR\locale

!ifdef LOCTEMPLATE
  File "${LOCTEMPLATE}"
!endif
  ; en.dll only: en_small.dll (for screens under 1024x768) has no source, and
  ; without it the client uses en.dll on a small screen too.
  File "${LOCALEDIR}\en.dll"
  File "..\tools\count_locale_usage.lua"
  File "..\tools\detect_locale_changes.lua"
  File "..\docs\locale_notes.txt"

  SetOutPath $INSTDIR

  ;
  ;  put sounds here, hint hint
  ;
  CreateDirectory "$INSTDIR\sounds"

 ;
  ;  directory for name generation files
  ;
  CreateDirectory "$INSTDIR\names"
  SetOutPath $INSTDIR\names

  File "..\names\ALBION.NAM"
  File "..\names\ALVER.NAM"
  File "..\names\ALVER1.NAM"
  File "..\names\ALVER2.NAM"
  File "..\names\DEVERRY1.NAM"
  File "..\names\DEVERRY2.NAM"
  File "..\names\DVARGAR.NAM"
  File "..\names\Dragonl1.nam"
  File "..\names\Felana.nam"
  File "..\names\GALLER.NAM"
  File "..\names\HOBER.NAM"
  File "..\names\ORC1.NAM"
  File "..\names\ORC2.NAM"
  File "..\names\albion1.nam"
  File "..\names\albion2.nam"
  File "..\names\dvargar1.nam"
  File "..\names\dvargar2.nam"
  File "..\names\f_female.nam"
  File "..\names\f_male.nam"
  File "..\names\gnome1.nam"
  File "..\names\gnome2.nam"
  File "..\names\hober1.nam"
  File "..\names\hober2.nam"
  File "..\names\kender1.nam"
  File "..\names\kender2.nam"
  File "..\names\kerrel.nam"
  File "..\names\orc.nam"
  File "..\names\sparhawk.nam"

  SetOutPath $INSTDIR

SectionEnd

Section "Spell checker"
  ; Which section type it is in
  SectionIn 1 2   ; typical, full

  ; Set output path to the spell directory.
  SetOutPath $INSTDIR\spell

  ; Put files there

  File "..\spell\english-words.10"
  File "..\spell\english-words.20"
  File "..\spell\english-words.35"
  File "..\spell\english-words.40"
  File "..\spell\english-upper.10"
  File "..\spell\english-upper.35"
  File "..\spell\english-upper.40"
  File "..\spell\american-words.10"
  File "..\spell\american-words.20"
  File "..\spell\english-contractions.10"
  File "..\spell\english-contractions.35"


  ; Set output path to the installation directory.
  SetOutPath $INSTDIR

  ; New spell checker
  File "..\src\scripting\spellchecker.lua"

SectionEnd

Section "Documentation"
  ; Which section type it is in
  SectionIn 1 2   ; typical, full

  ; Set output path to the doc directory.
  SetOutPath $INSTDIR\docs

  ; Put files there
  File "..\docs\JSON License.txt"
  File "..\docs\Lua Colors LICENSE.txt"
  File "..\docs\LuaJSON.txt"
  File "..\docs\LuaSocket_license.txt"
  File "..\docs\PCRE_ChangeLog.txt"
  File "..\docs\RegularExpressions.txt"
  File "..\docs\gpl.txt"
  File "..\docs\lpeg.html"
  File "..\docs\lpeg-128.gif"
  File "..\docs\lsqlite3.html"
  File "..\docs\lua_license.txt"
  File "..\docs\Figlet_license.txt"
  File "..\docs\luacom.pdf"
  File "..\docs\mersenne_twister.txt"
  File "..\docs\re.html"

  CreateDirectory "$INSTDIR\docs\LuaSocket_documentation"
  SetOutPath $INSTDIR\docs\LuaSocket_documentation

  File "..\docs\LuaSocket_documentation\dns.html"
  File "..\docs\LuaSocket_documentation\ftp.html"
  File "..\docs\LuaSocket_documentation\http.html"
  File "..\docs\LuaSocket_documentation\index.html"
  File "..\docs\LuaSocket_documentation\installation.html"
  File "..\docs\LuaSocket_documentation\introduction.html"
  File "..\docs\LuaSocket_documentation\ltn12.html"
  File "..\docs\LuaSocket_documentation\luasocket.png"
  File "..\docs\LuaSocket_documentation\mime.html"
  File "..\docs\LuaSocket_documentation\reference.css"
  File "..\docs\LuaSocket_documentation\reference.html"
  File "..\docs\LuaSocket_documentation\smtp.html"
  File "..\docs\LuaSocket_documentation\socket.html"
  File "..\docs\LuaSocket_documentation\tcp.html"
  File "..\docs\LuaSocket_documentation\udp.html"
  File "..\docs\LuaSocket_documentation\url.html"

SectionEnd

Section "Licences"
  ; Which section type it is in
  SectionIn 1 2 3  ; compact, typical, full

  ; Set output path to the licences directory.
  CreateDirectory "$INSTDIR\licences"
  SetOutPath $INSTDIR\licences

  File "..\licences\FIGlet licence.txt"
  File "..\licences\GNU General Public License.txt"
  File "..\licences\JSON licence.txt"
  File "..\licences\Lua Colors licence.txt"
  File "..\licences\Lua licence.txt"
  File "..\licences\LuaCOM licence.txt"
  File "..\licences\Luasocket licence.txt"
  File "..\licences\MD5 licence.txt"
  File "..\licences\MUSHclient licence.txt"
  File "..\licences\Mersenne Twister licence.txt"
  File "..\licences\PCRE licence.txt"
  File "..\licences\README.txt"
  File "..\licences\SHA license.txt"
  File "..\licences\SHS licence.txt"
  File "..\licences\SQLite licence.txt"
  File "..\licences\bc licence.txt"
  File "..\licences\big numbers licence.txt"
  File "..\licences\libpng-LICENSE.txt"
  File "..\licences\zLib licence.txt"
!ifdef DINALICENCE
  File "/oname=Dina licence.txt" "${DINALICENCE}"
!endif


SectionEnd


Section "Fonts"

  ; Which section type it is in
  SectionIn 1 2   ; typical, full

  ; Set output path to the fonts directory.
  SetOutPath $INSTDIR\fonts

  ; Put files there
  File "..\fonts\banner.flf"
  File "..\fonts\big.flf"
  File "..\fonts\block.flf"
  File "..\fonts\bubble.flf"
  File "..\fonts\digital.flf"
  File "..\fonts\lean.flf"
  File "..\fonts\mini.flf"
  File "..\fonts\script.flf"
  File "..\fonts\shadow.flf"
  File "..\fonts\slant.flf"
  File "..\fonts\small.flf"
  File "..\fonts\smscript.flf"
  File "..\fonts\smshadow.flf"
  File "..\fonts\smslant.flf"
  File "..\fonts\standard.flf"
  File "..\fonts\term.flf"
SectionEnd

Section "Scripting"
  ; Which section type it is in
  SectionIn 1 2   ; typical, full

  ;
  ;  directory for scripts
  ;
  CreateDirectory "$INSTDIR\scripts"

  ;
  ;  directory for Lua scripts
  ;
  CreateDirectory "$INSTDIR\lua"

  ; Set output path to the lua subdirectory.
  SetOutPath $INSTDIR\lua

SetOverwrite ifnewer

   File "..\lua\InfoBox.lua"
   File "..\lua\addxml.lua"
   File "..\lua\alphanum.lua"
   File "..\lua\check.lua"
   File "..\lua\checkplugin.lua"
   File "..\lua\colors.lua"
   File "..\lua\commas.lua"
   File "..\lua\copytable.lua"
   File "..\lua\declare.lua"
   File "..\lua\gauge.lua"
   File "..\lua\getlines.lua"
   File "..\lua\getstyle.lua"
   File "..\lua\getworld.lua"
   File "..\lua\mapper.lua"
   File "..\lua\movewindow.lua"
   File "..\lua\mw.lua"
   File "..\lua\pairsbykeys.lua"
   File "..\lua\ppi.lua"
   File "..\lua\re.lua"
   File "..\lua\sandbox.lua"
   File "..\lua\serialize.lua"
   File "..\lua\show_loaded.lua"
   File "..\lua\strict.lua"
   File "..\lua\string_indexing.lua"
   File "..\lua\tabbed_window.lua"
   File "..\lua\tprint.lua"
   File "..\lua\var.lua"
   File "..\lua\wait.lua"
   File "..\lua\words_to_numbers.lua"
   File "..\luacom\luacom5.lua"

   ; LuaSocket

   File "..\lua\socket.lua"
   File "..\lua\ltn12.lua"
   File "..\lua\mime.lua"

  CreateDirectory "$INSTDIR\mime"
  SetOutPath $INSTDIR\mime

   File "${MIMECORE}"

  CreateDirectory "$INSTDIR\socket"
  SetOutPath $INSTDIR\socket

   File "${SOCKETCORE}"
   File "..\lua\socket\ftp.lua"
   File "..\lua\socket\headers.lua"
   File "..\lua\socket\http.lua"
   File "..\lua\socket\smtp.lua"
   File "..\lua\socket\tp.lua"
   File "..\lua\socket\url.lua"

   ; JSON stuff from http://luaforge.net/projects/luajson/
   ; version 1.1

  SetOutPath $INSTDIR\lua

   File "..\lua\json.lua"

  CreateDirectory "$INSTDIR\lua\json"
  SetOutPath $INSTDIR\lua\json

    File "..\lua\json\encode.lua"
    File "..\lua\json\decode.lua"
    File "..\lua\json\util.lua"

  CreateDirectory "$INSTDIR\lua\json\encode"
  SetOutPath $INSTDIR\lua\json\encode

    File "..\lua\json\encode\array.lua"
    File "..\lua\json\encode\calls.lua"
    File "..\lua\json\encode\number.lua"
    File "..\lua\json\encode\object.lua"
    File "..\lua\json\encode\others.lua"
    File "..\lua\json\encode\output.lua"
    File "..\lua\json\encode\output_utility.lua"
    File "..\lua\json\encode\strings.lua"

  CreateDirectory "$INSTDIR\lua\json\decode"
  SetOutPath $INSTDIR\lua\json\decode

    File "..\lua\json\decode\array.lua"
    File "..\lua\json\decode\calls.lua"
    File "..\lua\json\decode\composite.lua"
    File "..\lua\json\decode\number.lua"
    File "..\lua\json\decode\object.lua"
    File "..\lua\json\decode\others.lua"
    File "..\lua\json\decode\state.lua"
    File "..\lua\json\decode\strings.lua"
    File "..\lua\json\decode\util.lua"

  ; Set output path to the scripts subdirectory.
  SetOutPath $INSTDIR\scripts

  ; Put files there
SetOverwrite ifnewer
  File "..\src\scripting\exampscript.vbs"
  File "..\src\scripting\exampscript.js"
  File "..\src\scripting\exampscript.pl"
  File "..\src\scripting\exampscript.pys"
  File "..\src\scripting\exampscript.lua"
SetOverwrite on
  File "${BINDIR}\MUSHclient.tlb"
SectionEnd

Section "Plugins"
  ; Which section type it is in
  SectionIn 1 2   ; typical, full

  SetOutPath $INSTDIR\worlds\plugins

  ; Put files there
  File "..\plugins\constants.vbs"
  File "..\plugins\constants.js"
  File "..\plugins\constants.pl"
  File "..\plugins\constants.pys"
  File "..\plugins\constants.lua"

  ; plugins - alphabetic order
  File "..\plugins\ANSI_Log.xml"
  File "..\plugins\ATCP_Mapper.xml"
  File "..\plugins\ATCP_NJG.xml"
  File "..\plugins\Automatic_Backup.xml"
  File "..\plugins\Calculator.xml"
  File "..\plugins\chat.xml"
  File "..\plugins\Code_Chart.xml"
  File "..\plugins\ColourNote.xml"
  File "..\plugins\Config_Option_Changer.xml"
  File "..\plugins\Current_Output_Window.xml"
  File "..\plugins\gag.xml"
  File "..\plugins\Global_Option_Updater.xml"
  File "..\plugins\health_bar.xml"
  File "..\plugins\Hyperlink_URL.xml"
  File "..\plugins\InfoBox_Demo.xml"
  File "..\plugins\Installer_sumcheck.xml"
  File "..\plugins\MUSHclient_Help.xml"
  File "..\plugins\mush_teleport.xml"
  File "..\plugins\Messages_Window.xml"
  File "..\plugins\MudDatabase.xml"
  File "..\plugins\NewActivity.xml"
  File "..\plugins\Omit_Blank_Lines.xml"
  File "..\plugins\Reconnecter_Lua.xml"
  File "..\plugins\SMAUG_automapper_helper.xml"
  File "..\plugins\showactivity.xml"
  File "..\plugins\status_bar_prompt.xml"
  File "..\plugins\Summary.xml"
  File "..\plugins\Text_To_Speech.xml"
  File "..\plugins\timer.xml"
  File "..\plugins\Timestamps.xml"
  File "..\plugins\idle_message.xml"
  File "..\plugins\msp.xml"
  File "..\plugins\multiple_send.xml"
  File "..\plugins\plugin_list.xml"
  File "..\plugins\plugin_summary.xml"
  File "..\plugins\random_socials.xml"
  File "..\plugins\reverse_speedwalk.xml"
  File "..\plugins\scan.xml"
  File "..\plugins\Version_Check.xml"


SectionEnd


!ifdef DISCWORLD
; The Discworld edition: Quow's plugins and maps (packaging\discworld-plugins,
; staged by packaging/fetch-discworld-plugins.sh), where they look for each
; other -- the plugins in worlds\plugins, the maps in quow_plugins\maps
; relative to the current directory -- and a ready-made Discworld world.  An
; installed copy runs in %APPDATA%\MUSHclient\, and copies all three there on
; its first run (src/user_data.cpp, Seed), the mapper's database being one it
; writes to; a portable one runs here.
Section "-Discworld plugins and maps (required)"
  SectionIn 1 2 3  ; typical, full, compact

  SetOutPath $INSTDIR\worlds\plugins
  File "${DISCWORLD}\worlds\plugins\Quow*.xml"
  SetOutPath $INSTDIR
  ; The player's own mapper data is never shipped (and so never overwritten).
  File /r /x "_quowmap_userdata.db" /x ".DS_Store" "${DISCWORLD}\quow_plugins"
  ; The world file is the player's once it has been used: on an upgrade the
  ; existing one is kept.
  SetOutPath $INSTDIR\worlds
  SetOverwrite off
  File "${DISCWORLD}\worlds\Discworld.mcl"
  SetOverwrite on
  SetOutPath $INSTDIR
SectionEnd
!endif

; optional section
Section "Start Menu Shortcuts"
  ; Which section type it is in
  SectionIn 1 2   ; typical, full

  SetOutPath "${SMDIR}"
  SetOutPath $INSTDIR
  CreateShortCut "${SMDIR}\MUSHclient.lnk" "$INSTDIR\MUSHclient.exe" "" "$INSTDIR\MUSHclient.exe" 0
  CreateShortCut "${SMDIR}\Read Me.lnk" "$INSTDIR\Readme.txt" "" "$INSTDIR\Readme.txt" 0
  CreateShortCut "${SMDIR}\Uninstall MUSHclient.lnk" "$INSTDIR\uninstall.exe" "" "$INSTDIR\uninstall.exe" 0
SectionEnd

; uninstall stuff
UninstallText "Are you sure you want to remove MUSHclient and all of its components?"

!ifdef PLAT
Function un.onInit
  SetRegView 64     ; the uninstall key was written to the 64-bit view
FunctionEnd
!endif

; special uninstall section.
Section Uninstall

  ; remove registry keys -- this version's; a 5.x's are its own business.
  ; (Its settings and worlds, in %APPDATA%\MUSHclient\, are the user's: kept.)
  DeleteRegKey HKLM "${UNINSTKEY}"
  DeleteRegKey HKCU "${REGKEY}"

  ; remove files

  ; program
  Delete "$INSTDIR\MUSHclient.exe"
  ; Delete "$INSTDIR\MUSHclient.exe.manifest"
  Delete "$INSTDIR\lua5.1.dll"
  Delete "$INSTDIR\lua5.1.lib"
  Delete "$INSTDIR\MUSHclient.ico"
  Delete "$INSTDIR\MUSHclient.hlp"
  Delete "$INSTDIR\help.db"
  Delete "$INSTDIR\MUSHclient.cnt"
  Delete "$INSTDIR\license.txt"

  ; readme and tips
  Delete "$INSTDIR\readme.txt"
  Delete "$INSTDIR\tips.txt"

  ; example trigger/alias/timer list filters
  Delete "$INSTDIR\Example_filters.lua"

  ; and character name generation file
  Delete "$INSTDIR\names.txt"

  ; and Dina font
  Delete "$INSTDIR\Dina.fon"

!ifdef PLAT
  ; the embedded scripting runtimes and the VC++ runtime
  Delete "$INSTDIR\python3*.dll"
  Delete "$INSTDIR\*-ruby*.dll"
  Delete "$INSTDIR\vcruntime140.dll"
  Delete "$INSTDIR\vcruntime140_1.dll"
  RMDir /r "$INSTDIR\python"
  RMDir /r "$INSTDIR\lib\ruby"
  RMDir    "$INSTDIR\lib"
  RMDir /r "$INSTDIR\tcl8.6"
!endif
!ifdef DISCWORLD
  ; Quow's plugins and maps -- by name, never the directories wholesale: the
  ; mapper keeps the player's own data (_quowmap_userdata.db) beside its maps.
  Delete "$INSTDIR\worlds\plugins\Quow*.xml"
  Delete "$INSTDIR\quow_plugins\maps\*.png"
  Delete "$INSTDIR\quow_plugins\maps\*.wav"
  Delete "$INSTDIR\quow_plugins\maps\_quowmap_database.db"
  RMDir  "$INSTDIR\quow_plugins\maps"
  RMDir  "$INSTDIR\quow_plugins"
!endif

  ; scripting stuff
  Delete "$INSTDIR\scripts\MUSHclient.tlb"
  Delete "$INSTDIR\scripts\exampscript.vbs"
  Delete "$INSTDIR\scripts\exampscript.js"
  Delete "$INSTDIR\scripts\exampscript.pl"
  Delete "$INSTDIR\scripts\exampscript.pys"
  Delete "$INSTDIR\scripts\exampscript.lua"

  RMDir  "$INSTDIR\scripts"

  ; lua stuff
  Delete "$INSTDIR\lua\InfoBox.lua"
  Delete "$INSTDIR\lua\addxml.lua"
  Delete "$INSTDIR\lua\alphanum.lua"
  Delete "$INSTDIR\lua\check.lua"
  Delete "$INSTDIR\lua\checkplugin.lua"
  Delete "$INSTDIR\lua\colors.lua"
  Delete "$INSTDIR\lua\commas.lua"
  Delete "$INSTDIR\lua\copytable.lua"
  Delete "$INSTDIR\lua\declare.lua"
  Delete "$INSTDIR\lua\gauge.lua"
  Delete "$INSTDIR\lua\getlines.lua"
  Delete "$INSTDIR\lua\getstyle.lua"
  Delete "$INSTDIR\lua\getworld.lua"
  Delete "$INSTDIR\lua\luacom5.lua"
  Delete "$INSTDIR\lua\mapper.lua"
  Delete "$INSTDIR\lua\movewindow.lua"
  Delete "$INSTDIR\lua\mw.lua"
  Delete "$INSTDIR\lua\pairsbykeys.lua"
  Delete "$INSTDIR\lua\ppi.lua"
  Delete "$INSTDIR\lua\re.lua"
  Delete "$INSTDIR\lua\sandbox.lua"
  Delete "$INSTDIR\lua\serialize.lua"
  Delete "$INSTDIR\lua\show_loaded.lua"
  Delete "$INSTDIR\lua\strict.lua"
  Delete "$INSTDIR\lua\string_indexing.lua"
  Delete "$INSTDIR\lua\tabbed_window.lua"
  Delete "$INSTDIR\lua\tprint.lua"
  Delete "$INSTDIR\lua\var.lua"
  Delete "$INSTDIR\lua\wait.lua"
  Delete "$INSTDIR\lua\words_to_numbers.lua"

  Delete "$INSTDIR\lua\socket.lua"
  Delete "$INSTDIR\lua\ltn12.lua"
  Delete "$INSTDIR\lua\mime.lua"
  Delete "$INSTDIR\mime\core.dll"
  Delete "$INSTDIR\socket\core.dll"
  Delete "$INSTDIR\socket\ftp.lua"
  Delete "$INSTDIR\socket\headers.lua"
  Delete "$INSTDIR\socket\http.lua"
  Delete "$INSTDIR\socket\smtp.lua"
  Delete "$INSTDIR\socket\tp.lua"
  Delete "$INSTDIR\socket\url.lua"

  RMDir  "$INSTDIR\mime"
  RMDir  "$INSTDIR\socket"

  Delete "$INSTDIR\lua\json.lua"
  Delete "$INSTDIR\lua\json\encode.lua"
  Delete "$INSTDIR\lua\json\decode.lua"
  Delete "$INSTDIR\lua\json\util.lua"
  Delete "$INSTDIR\lua\json\encode\array.lua"
  Delete "$INSTDIR\lua\json\encode\calls.lua"
  Delete "$INSTDIR\lua\json\encode\number.lua"
  Delete "$INSTDIR\lua\json\encode\object.lua"
  Delete "$INSTDIR\lua\json\encode\others.lua"
  Delete "$INSTDIR\lua\json\encode\output.lua"
  Delete "$INSTDIR\lua\json\encode\output_utility.lua"
  Delete "$INSTDIR\lua\json\encode\strings.lua"
  Delete "$INSTDIR\lua\json\decode\array.lua"
  Delete "$INSTDIR\lua\json\decode\calls.lua"
  Delete "$INSTDIR\lua\json\decode\composite.lua"
  Delete "$INSTDIR\lua\json\decode\number.lua"
  Delete "$INSTDIR\lua\json\decode\object.lua"
  Delete "$INSTDIR\lua\json\decode\others.lua"
  Delete "$INSTDIR\lua\json\decode\state.lua"
  Delete "$INSTDIR\lua\json\decode\strings.lua"
  Delete "$INSTDIR\lua\json\decode\util.lua"

  RMDir  "$INSTDIR\lua\json\decode"
  RMDir  "$INSTDIR\lua\json\encode"
  RMDir  "$INSTDIR\lua\json"

  ; spell checker stuff
  Delete "$INSTDIR\spellchecker.lua"
  Delete "$INSTDIR\spell\english-words.10"
  Delete "$INSTDIR\spell\english-words.20"
  Delete "$INSTDIR\spell\english-words.35"
  Delete "$INSTDIR\spell\english-words.40"
  Delete "$INSTDIR\spell\english-upper.10"
  Delete "$INSTDIR\spell\english-upper.35"
  Delete "$INSTDIR\spell\english-upper.40"
  Delete "$INSTDIR\spell\american-words.10"
  Delete "$INSTDIR\spell\american-words.20"
  Delete "$INSTDIR\spell\english-contractions.10"
  Delete "$INSTDIR\spell\english-contractions.35"

  RMDir  "$INSTDIR\spell"

  ; docs
  Delete "$INSTDIR\docs\JSON License.txt"
  Delete "$INSTDIR\docs\Lua Colors LICENSE.txt"
  Delete "$INSTDIR\docs\LuaJSON.txt"
  Delete "$INSTDIR\docs\PCRE_ChangeLog.txt"
  Delete "$INSTDIR\docs\RegularExpressions.txt"
  Delete "$INSTDIR\docs\gpl.txt"
  Delete "$INSTDIR\docs\lpeg.html"
  Delete "$INSTDIR\docs\lpeg-128.gif"

  Delete "$INSTDIR\docs\lsqlite3.html"
  Delete "$INSTDIR\docs\lua_license.txt"
  Delete "$INSTDIR\docs\Figlet_license.txt"

  Delete "$INSTDIR\docs\luacom.pdf"
  Delete "$INSTDIR\docs\mersenne_twister.txt"
  Delete "$INSTDIR\docs\re.html"

  Delete "$INSTDIR\docs\LuaSocket_license.txt"
  Delete "$INSTDIR\docs\LuaSocket_documentation\dns.html"
  Delete "$INSTDIR\docs\LuaSocket_documentation\ftp.html"
  Delete "$INSTDIR\docs\LuaSocket_documentation\http.html"
  Delete "$INSTDIR\docs\LuaSocket_documentation\index.html"
  Delete "$INSTDIR\docs\LuaSocket_documentation\installation.html"
  Delete "$INSTDIR\docs\LuaSocket_documentation\introduction.html"
  Delete "$INSTDIR\docs\LuaSocket_documentation\ltn12.html"
  Delete "$INSTDIR\docs\LuaSocket_documentation\luasocket.png"
  Delete "$INSTDIR\docs\LuaSocket_documentation\mime.html"
  Delete "$INSTDIR\docs\LuaSocket_documentation\reference.css"
  Delete "$INSTDIR\docs\LuaSocket_documentation\reference.html"
  Delete "$INSTDIR\docs\LuaSocket_documentation\smtp.html"
  Delete "$INSTDIR\docs\LuaSocket_documentation\socket.html"
  Delete "$INSTDIR\docs\LuaSocket_documentation\tcp.html"
  Delete "$INSTDIR\docs\LuaSocket_documentation\udp.html"
  Delete "$INSTDIR\docs\LuaSocket_documentation\url.html"

  RMDir  "$INSTDIR\docs\LuaSocket_documentation"

  RMDir  "$INSTDIR\docs"

  Delete "$INSTDIR\licences\FIGlet licence.txt"
  Delete "$INSTDIR\licences\GNU General Public License.txt"
  Delete "$INSTDIR\licences\JSON licence.txt"
  Delete "$INSTDIR\licences\Lua Colors licence.txt"
  Delete "$INSTDIR\licences\Lua licence.txt"
  Delete "$INSTDIR\licences\LuaCOM licence.txt"
  Delete "$INSTDIR\licences\Luasocket licence.txt"
  Delete "$INSTDIR\licences\MD5 licence.txt"
  Delete "$INSTDIR\licences\MUSHclient licence.txt"
  Delete "$INSTDIR\licences\Mersenne Twister licence.txt"
  Delete "$INSTDIR\licences\PCRE licence.txt"
  Delete "$INSTDIR\licences\README.txt"
  Delete "$INSTDIR\licences\SHA license.txt"
  Delete "$INSTDIR\licences\SHS licence.txt"
  Delete "$INSTDIR\licences\SQLite licence.txt"
  Delete "$INSTDIR\licences\bc licence.txt"
  Delete "$INSTDIR\licences\big numbers licence.txt"
  Delete "$INSTDIR\licences\libpng-LICENSE.txt"
  Delete "$INSTDIR\licences\zLib licence.txt"
  Delete "$INSTDIR\licences\Dina licence.txt"

  RMDir  "$INSTDIR\licences"

  ; fonts
  Delete "$INSTDIR\fonts\*.flf"
  RMDir  "$INSTDIR\fonts"

  ; plugins
  Delete "$INSTDIR\worlds\plugins\constants.vbs"
  Delete "$INSTDIR\worlds\plugins\constants.js"
  Delete "$INSTDIR\worlds\plugins\constants.pl"
  Delete "$INSTDIR\worlds\plugins\constants.pys"
  Delete "$INSTDIR\worlds\plugins\constants.lua"

  ; plugins - alphabetic order
  Delete "$INSTDIR\worlds\plugins\ANSI_Log.xml"
  Delete "$INSTDIR\worlds\plugins\ATCP_Mapper.xml"
  Delete "$INSTDIR\worlds\plugins\ATCP_NJG.xml"
  Delete "$INSTDIR\worlds\plugins\Automatic_Backup.xml"
  Delete "$INSTDIR\worlds\plugins\Calculator.xml"
  Delete "$INSTDIR\worlds\plugins\chat.xml"
  Delete "$INSTDIR\worlds\plugins\Code_Chart.xml"
  Delete "$INSTDIR\worlds\plugins\ColourNote.xml"
  Delete "$INSTDIR\worlds\plugins\Config_Option_Changer.xml"
  Delete "$INSTDIR\worlds\plugins\Current_Output_Window.xml"
  Delete "$INSTDIR\worlds\plugins\gag.xml"
  Delete "$INSTDIR\worlds\plugins\Global_Option_Updater.xml"
  Delete "$INSTDIR\worlds\plugins\health_bar.xml"
  Delete "$INSTDIR\worlds\plugins\Hyperlink_URL.xml"
  Delete "$INSTDIR\worlds\plugins\InfoBox_Demo.xml"
  Delete "$INSTDIR\worlds\plugins\Installer_sumcheck.xml"
  Delete "$INSTDIR\worlds\plugins\MUSHclient_Help.xml"
  Delete "$INSTDIR\worlds\plugins\mush_teleport.xml"
  Delete "$INSTDIR\worlds\plugins\Messages_Window.xml"
  Delete "$INSTDIR\worlds\plugins\MudDatabase.xml"
  Delete "$INSTDIR\worlds\plugins\NewActivity.xml"
  Delete "$INSTDIR\worlds\plugins\Omit_Blank_Lines.xml"
  Delete "$INSTDIR\worlds\plugins\Reconnecter_Lua.xml"
  Delete "$INSTDIR\worlds\plugins\SMAUG_automapper_helper.xml"
  Delete "$INSTDIR\worlds\plugins\showactivity.xml"
  Delete "$INSTDIR\worlds\plugins\status_bar_prompt.xml"
  Delete "$INSTDIR\worlds\plugins\Summary.xml"
  Delete "$INSTDIR\worlds\plugins\Text_To_Speech.xml"
  Delete "$INSTDIR\worlds\plugins\timer.xml"
  Delete "$INSTDIR\worlds\plugins\Timestamps.xml"
  Delete "$INSTDIR\worlds\plugins\idle_message.xml"
  Delete "$INSTDIR\worlds\plugins\msp.xml"
  Delete "$INSTDIR\worlds\plugins\multiple_send.xml"
  Delete "$INSTDIR\worlds\plugins\plugin_list.xml"
  Delete "$INSTDIR\worlds\plugins\plugin_summary.xml"
  Delete "$INSTDIR\worlds\plugins\random_socials.xml"
  Delete "$INSTDIR\worlds\plugins\reverse_speedwalk.xml"
  Delete "$INSTDIR\worlds\plugins\scan.xml"
  Delete "$INSTDIR\worlds\plugins\Version_Check.xml"

  ; locale stuff

  Delete "$INSTDIR\locale\Localize_template.lua"
  Delete "$INSTDIR\locale\en.dll"
  Delete "$INSTDIR\locale\en_small.dll"
  Delete "$INSTDIR\locale\count_locale_usage.lua"
  Delete "$INSTDIR\locale\detect_locale_changes.lua"
  Delete "$INSTDIR\locale\locale_notes.txt"

   ; names files

  Delete "$INSTDIR\names\ALBION.NAM"
  Delete "$INSTDIR\names\ALVER.NAM"
  Delete "$INSTDIR\names\ALVER1.NAM"
  Delete "$INSTDIR\names\ALVER2.NAM"
  Delete "$INSTDIR\names\DEVERRY1.NAM"
  Delete "$INSTDIR\names\DEVERRY2.NAM"
  Delete "$INSTDIR\names\DVARGAR.NAM"
  Delete "$INSTDIR\names\Dragonl1.nam"
  Delete "$INSTDIR\names\Felana.nam"
  Delete "$INSTDIR\names\GALLER.NAM"
  Delete "$INSTDIR\names\HOBER.NAM"
  Delete "$INSTDIR\names\ORC1.NAM"
  Delete "$INSTDIR\names\ORC2.NAM"
  Delete "$INSTDIR\names\albion1.nam"
  Delete "$INSTDIR\names\albion2.nam"
  Delete "$INSTDIR\names\dvargar1.nam"
  Delete "$INSTDIR\names\dvargar2.nam"
  Delete "$INSTDIR\names\f_female.nam"
  Delete "$INSTDIR\names\f_male.nam"
  Delete "$INSTDIR\names\gnome1.nam"
  Delete "$INSTDIR\names\gnome2.nam"
  Delete "$INSTDIR\names\hober1.nam"
  Delete "$INSTDIR\names\hober2.nam"
  Delete "$INSTDIR\names\kender1.nam"
  Delete "$INSTDIR\names\kender2.nam"
  Delete "$INSTDIR\names\kerrel.nam"
  Delete "$INSTDIR\names\orc.nam"
  Delete "$INSTDIR\names\sparhawk.nam"

  RMDir  "$INSTDIR\names"
  RMDir  "$INSTDIR\sounds"

  ; MUST REMOVE UNINSTALLER, too
  Delete $INSTDIR\uninstall.exe

  ; remove shortcuts, if any.
  Delete "${SMDIR}\*.lnk"
  RMDir "${SMDIR}"

  ; delete worlds, lua and logs directory, if empty
  RMDir  "$INSTDIR\worlds"
  RMDir  "$INSTDIR\logs"
  RMDir  "$INSTDIR\lua"
  RMDir  "$INSTDIR\locale"

  ; remove directories used.
  RMDir "$INSTDIR"

  ; warn user if not all files removed
  IfErrors CannotRemoveDirectory CanRemoveDirectory

  CannotRemoveDirectory:

  MessageBox MB_OK|MB_ICONEXCLAMATION  \
    "Some files or folders (probably that you created) could not be removed and should be deleted manually if desired."

  CanRemoveDirectory:

SectionEnd

; post-install

Function .onInstSuccess
;  MessageBox MB_YESNO "Installation of MUSHclient is complete. View the readme file?" IDNO NoReadme
;    Exec 'notepad "$INSTDIR\readme.txt"'
;  NoReadme:

  MessageBox MB_YESNO "Launch MUSHclient now?" IDNO NoLaunch
    SetOutPath $INSTDIR
    ExecShell "open" "$INSTDIR\mushclient.exe"
  NoLaunch:
FunctionEnd


