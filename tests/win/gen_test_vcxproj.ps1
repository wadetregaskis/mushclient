# tests/win/gen_test_vcxproj.ps1 -- regenerate MUSHclient_tests.vcxproj from
# MUSHclient.vcxproj.
#
# The test target has to compile the same 286 engine TUs the app does, because
# MUSHCLIENT_TESTS changes a member's access and therefore its mangled name:
# a half-applied define makes doc.obj reference the protected constructor while
# doc_construct.obj defines the public one, and the link fails on a symbol that
# looks identical in both.  So the two source lists must agree exactly.
#
# Keeping the list in two files is a drift hazard, and drift between two
# definitions of "how this builds on Windows" is the bug class
# docs/WINDOWS-VALIDATION.md exists to catch.  Two things guard it: this
# generator, and tests/check_win_test_sources.py, which fails the suite if the
# two projects' <ClCompile> lists ever differ.  The lint is the real guard --
# regenerating is just the convenient way to fix it.
#
#   pwsh tests/win/gen_test_vcxproj.ps1        (from the repo root)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root

[xml]$app = Get-Content 'MUSHclient.vcxproj'
$ns = New-Object System.Xml.XmlNamespaceManager($app.NameTable)
$ns.AddNamespace('m', 'http://schemas.microsoft.com/developer/msbuild/2003')

# Keep the whole node, not just the path: 246 of the 286 carry PER-FILE
# settings, and dropping them is not cosmetic.  The 36 pcre\*.c files need
# HAVE_CONFIG_H, and without it pcre_internal.h stops the build with
# "#error LINK_SIZE must be either 2, 3, or 4" -- which is how this was found.
$sourceNodes = @($app.SelectNodes('//m:ClCompile[@Include]', $ns))
$sources = $sourceNodes | ForEach-Object { $_.Include }

# Test files that are meaningless on Windows: they test the port's SUBSTITUTES
# for things Windows has natively, so running them here would assert against a
# shim that is not present.  This is an EXCLUSION list on purpose -- a new test
# file is compiled into the Windows target by default, and has to be argued out
# rather than argued in.  Add a reason to every entry.
$excluded = [ordered]@{
  'test_mfc_ui.cpp'        = 'tests platform/mfc_ui.h, the 108 fake MFC classes; :3462 asserts App.platform against the shim fabricated OS info'
  'test_cstring.cpp'       = 'tests platform/mfc_string.h; on Windows CString is MFC own'
  'test_collections.cpp'   = 'tests platform/mfc_collections.h; on Windows these are CTypedPtrList etc.'
  'test_posix_socket.cpp'  = 'tests platform/posix_socket.h, which does not exist on Windows'
  'test_gdi_headless.cpp'  = 'tests platform/gdi_headless.cpp, the headless CDC; Windows has real GDI'
  'test_mc_dispatch.cpp'   = 'tests platform/mc_dispatch.cpp, the portable IDispatch substitute; Windows uses real COM'
  'test_pal_transport.cpp' = 'drives platform/posix_transport.cpp; the Windows adapter is pal/win_transport.cpp'
  'test_mc_test_framework.cpp' = 'uses dup2 to capture the harness own output'
  # --- needs a port-only seam; re-enterable once that seam has a Windows half
  'test_lua_surface.cpp'     = 'ACCESS VIOLATION in LuaSurface.UdpFamily, after 3 failures in the arms before it (UtilityMiscArms, ChangeDirRoundTrip, SoundFamilyHeadless -- chdir/getcwd shapes and the miniaudio backend, which is the non-Windows sound path). The notepad_fixture dependency it used to be excluded for is guarded now and was not the blocker; this suite drives a lot of port-only surface and wants its own pass'
  'test_notepad_methods.cpp' = 'every one of its 18 tests IS the notepad: notepad_fixture.h is #ifndef _WIN32 because a notepad on Windows is a real CEditView from the MFC doc/view framework, so guarding the users would leave an empty file'
  'test_sound.cpp'           = 'drives the miniaudio backend, which is the non-Windows sound path'
  'test_chat_sockets.cpp'    = 'builds real AF_UNIX/loopback pairs with socketpair, which Windows does not have'
  'test_notepad_convert.cpp' = 'NotepadDetectLineEndings is non-Windows only: CEditView::SerializeRaw means the buffer IS the representation on Windows (CLAUDE.md, macOS notes)'
  'test_pal_worldview.cpp'   = 'platform/activity_refresh.h includes <strings.h>; drives the port worldview'
  'test_accelerators.cpp'    = 'AcceleratorTo returns eBadParameter at its first line: GetFirstViewPosition() is NULL and this host has no view. Giving the host the two document templates did NOT fix this -- measured, the 11 failures were unchanged -- because the walk wants a CView attached to the document, which is a window, not a registry entry'
  'test_connection.cpp'      = 'ACCESS VIOLATION, and NOT the document template (the host has one now): ConnectionClosed -> Frame.OnUpdateBtnWorlds -> m_wndActivityToolBar.GetButtonInfo at mainfrm.cpp:712, unconditional, on a CToolBar that only exists after CMainFrame::OnCreate. 213 pass, then Tls.CertVerifyFailureBlocksAndRecordsReason dies there. This one needs a created main frame, which a console host cannot have. Its two strerror tests also assert the POSIX spelling, which WSAECONNREFUSED does not have'
  'test_world_load.cpp'      = 'built on CMUSHclientDoc::LoadWorldFromFile, which is port-only (doc.h declares it #ifndef _WIN32; Windows loads worlds through the MFC doc/view framework) -- 98 uses, i.e. the suite IS that entry point. The notepad_fixture dependency it used to be excluded for was real but incidental; this is the blocker. Same shape as test_lua_scripting, and the same fix would serve both'
  'test_lua_scripting.cpp'   = 'CMUSHclientDoc::LoadWorldFromFile is port-only (Windows loads worlds through the MFC doc/view framework), plus <utime.h>. A 5000-line suite: worth re-entering by giving the fixture a Windows world-load path'
  'test_menu_rules.cpp'      = 'needs platform/menu_ui_rules.cpp, the port menu-rule table; the app drives menus from MFC message maps and does not link it'
  'test_pal_surface.cpp'     = 'asserts on pal::CpuRaster, the port software rasteriser; Windows draws through pal/gdi_surface.cpp and never instantiates it'
}
# Everything above is opted OUT with a reason.  The last block is the interesting
# one: those suites are not Windows-hostile, they just reach a seam the port
# owns.  Guarding the handful of test cases that touch it -- the way
# test_telnet.cpp guards its one g_debug case -- brings each of them back, and
# that is how test_triggers and test_timers got here: two cases each, the wx
# clipboard hooks and the wx status-bar hooks, with the guard carrying a note on
# what a Windows-side observable would have to be.  The same route is open for
# most of what is left; test_lua_scripting is the biggest prize and the one that
# needs real work, since it wants a Windows world-load path rather than a guard.

$tests = Get-ChildItem 'tests\test_*.cpp' | Where-Object { -not $excluded.Contains($_.Name) } |
         ForEach-Object { 'tests\' + $_.Name }

$sb = New-Object System.Text.StringBuilder
function W([string]$s) { [void]$sb.AppendLine($s) }

W '<?xml version="1.0" encoding="utf-8"?>'
W '<!-- GENERATED by tests/win/gen_test_vcxproj.ps1 - do not edit by hand.'
W '     Pinned against MUSHclient.vcxproj by tests/check_win_test_sources.py. -->'
W '<Project DefaultTargets="Build" ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">'
W '  <ItemGroup Label="ProjectConfigurations">'
foreach ($cfg in @('Debug','Release')) {
  W "    <ProjectConfiguration Include=`"$cfg|Win32`">"
  W "      <Configuration>$cfg</Configuration>"
  W '      <Platform>Win32</Platform>'
  W '    </ProjectConfiguration>'
}
W '  </ItemGroup>'
W '  <PropertyGroup Label="Globals">'
W '    <ProjectGuid>{B7A41E52-9C3D-4F18-9E77-1D2A5C0F44A1}</ProjectGuid>'
W '    <RootNamespace>MUSHclientTests</RootNamespace>'
W '    <Keyword>MFCProj</Keyword>'
W '  </PropertyGroup>'
W '  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />'
foreach ($cfg in @('Debug','Release')) {
  W "  <PropertyGroup Condition=`"'`$(Configuration)|`$(Platform)'=='$cfg|Win32'`" Label=`"Configuration`">"
  W '    <ConfigurationType>Application</ConfigurationType>'
  W '    <UseOfMfc>Static</UseOfMfc>'
  W '    <CharacterSet>MultiByte</CharacterSet>'
  W '    <PlatformToolset>v143</PlatformToolset>'
  if ($cfg -eq 'Debug') { W '    <UseDebugLibraries>true</UseDebugLibraries>' }
  else                  { W '    <UseDebugLibraries>false</UseDebugLibraries>' }
  W '  </PropertyGroup>'
}
W '  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />'
W '  <PropertyGroup>'
W '    <OutDir>$(SolutionDir)WinTest\</OutDir>'
W '    <IntDir>$(SolutionDir)WinTest\obj\$(Configuration)\</IntDir>'
W '    <TargetName>mushclient_tests</TargetName>'
W '  </PropertyGroup>'
foreach ($cfg in @('Debug','Release')) {
  $dbg = ($cfg -eq 'Debug')
  W "  <ItemDefinitionGroup Condition=`"'`$(Configuration)|`$(Platform)'=='$cfg|Win32'`">"
  W '    <ClCompile>'
  # NOT $(SolutionDir)platform\ -- and not scripting\ either, though that one is
  # merely unnecessary.  tests/CMakeLists.txt puts both on the path, which is
  # safe when platform/ IS the platform; on Windows it is a trap.  platform/
  # holds direct.h, windows_types.h and friends, whose whole purpose is to
  # stand in for headers the Windows SDK really has, so putting that directory
  # ahead of the SDK makes `#include <direct.h>` find the shim.  Measured: it
  # the SDK.  Tests that want a port header spell the directory.
  # zlib\ and png\ are on the path because two test files include <zlib.h> and
  # <png.h>, which the app reaches through per-file settings this project does
  # not inherit.  Safe to add, unlike the port's own directories: neither holds
  # a header that masks a CRT or SDK one, and platform\direct.h masking
  # <direct.h> is what once turned a 36-error build into 36,810
  # (docs/WINDOWS-VALIDATION.md section 10j).
  W '      <AdditionalIncludeDirectories>$(SolutionDir);$(SolutionDir)luacom\;$(SolutionDir)openssl\include\;$(SolutionDir)tests\;$(SolutionDir)zlib\;$(SolutionDir)png\;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>'
  # _HAS_AUTO_PTR_ETC=1 goes with LanguageStandard stdcpp17 below: C++17
  # removed std::unary_function / std::binary_function / std::auto_ptr, and
  # stdafx.h derives a comparator from std::binary_function.  The app never
  # meets this because it is C++14.  This switch restores the three under C++17
  # in MSVC's STL, so the test target can have the language mc_test.h needs
  # without touching a line of shipping source.
  # MUSHCLIENT_LUA_SCRIPTING is what tests/test_database.cpp and
  # tests/test_lua_scripting.cpp gate their whole body on; without it each
  # falls through to a "Lua not available" stub that defines its own main(),
  # which then collides with the combined runner's.  The app's vcxproj does not
  # need the define because on Windows Lua is unconditional -- it links
  # lua5.1.lib -- but the TESTS ask the question explicitly, so answer it.
  $defs = '_WIN32_WINNT=0x0501;NO_HTMLHELP;WIN32;_WINDOWS;LUA51;MUSHCLIENT_LUA_SCRIPTING;_CRT_SECURE_NO_WARNINGS;_CRT_NON_CONFORMING_SWPRINTFS;NO_WARN_MBCS_MFC_DEPRECATION;_VC80_UPGRADE=0x0600;_MBCS;_HAS_AUTO_PTR_ETC=1;MUSHCLIENT_TESTS;MC_TEST_COMBINED'
  if ($dbg) { $defs = '_DEBUG;' + $defs } else { $defs = 'NDEBUG;' + $defs }
  W "      <PreprocessorDefinitions>$defs;%(PreprocessorDefinitions)</PreprocessorDefinitions>"
  W '      <PrecompiledHeader>NotUsing</PrecompiledHeader>'
  W '      <ExceptionHandling>Sync</ExceptionHandling>'
  W '      <DisableSpecificWarnings>4996</DisableSpecificWarnings>'
  W '      <MultiProcessorCompilation>true</MultiProcessorCompilation>'
  # The app is /std:c++14 (MUSHclient.vcxproj sets no LanguageStandard, so it
  # takes the MSVC default).  The TESTS need C++17: mc_test.h:149 uses
  # `if constexpr` for its value-rendering SFINAE.  Raising it here rather than
  # in the app keeps docs/WINDOWS-VALIDATION.md Â§2's "no C++17 anywhere in code
  # MSVC compiles" rule true where it matters -- the shipping client -- while
  # letting the harness use the language the harness is written in.
  W '      <LanguageStandard>stdcpp17</LanguageStandard>'
  if ($dbg) {
    W '      <Optimization>Disabled</Optimization>'
    W '      <RuntimeLibrary>MultiThreadedDebug</RuntimeLibrary>'
  } else {
    W '      <Optimization>MaxSpeed</Optimization>'
    W '      <RuntimeLibrary>MultiThreaded</RuntimeLibrary>'
  }
  W '    </ClCompile>'
  W '    <Link>'
  W '      <SubSystem>Console</SubSystem>'
  W '      <AdditionalLibraryDirectories>$(SolutionDir)openssl\lib;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>'
  W '      <AdditionalDependencies>lua5.1.lib;winmm.lib;dsound.lib;Shlwapi.lib;libssl.lib;libcrypto.lib;Crypt32.lib;%(AdditionalDependencies)</AdditionalDependencies>'
  W '      <GenerateDebugInformation>true</GenerateDebugInformation>'
  W '    </Link>'
  W '  </ItemDefinitionGroup>'
}
W '  <ItemGroup Label="engine">'
foreach ($node in $sourceNodes) {
  $kids = @($node.ChildNodes | Where-Object { $_.NodeType -eq 'Element' })
  if ($kids.Count -eq 0) {
    W "    <ClCompile Include=`"$($node.Include)`" />"
  } else {
    W "    <ClCompile Include=`"$($node.Include)`">"
    foreach ($k in $kids) {
      $cond = $k.GetAttribute('Condition')
      $attr = if ($cond) { " Condition=`"$([System.Security.SecurityElement]::Escape($cond))`"" } else { '' }
      $text = [System.Security.SecurityElement]::Escape($k.InnerText)
      W "      <$($k.LocalName)$attr>$text</$($k.LocalName)>"
    }
    W '    </ClCompile>'
  }
}
W '  </ItemGroup>'
# Test-SUPPORT sources: engine-shaped TUs the app does not link but the suite
# needs -- the Windows echo of CMake's MUSHCORE_TESTSUPPORT_SOURCES.  Currently
# EMPTY, and that is the interesting part.
#
# pal/cpu_raster.cpp was the obvious candidate, and it is the wrong one:
# CpuRaster is the PORT's software rasteriser, and Windows draws through
# pal/gdi_surface.cpp.  Compiling it here would test a class the Windows product
# never instantiates -- the same category as test_gdi_headless.cpp, which is
# excluded for exactly that reason.  It also does not want to be compiled here:
# the file deliberately includes no platform header, so MFC's "requires
# Winsock2.h" rule bites, and forcing winsock2.h in drags windows.h behind it
# and breaks the file 105 different ways.
#
# Keep this list tiny, and mirrored in tests/check_win_test_sources.py.  Every
# entry is a file whose Windows build nothing else covers.
$testSupport = @()

W '  <ItemGroup Label="testsupport">'
foreach ($s in $testSupport) {
  W "    <ClCompile Include=`"$s`">"
  # pal/cpu_raster.cpp includes no MFC and no stdafx.h -- it is deliberately
  # platform-free -- so nothing puts winsock2.h ahead of windows.h for it, and
  # MFC's headers stop with "#error MFC requires use of Winsock2.h".  Force it
  # in rather than editing a file whose whole point is that it needs no
  # platform header.
  W '      <ForcedIncludeFiles>winsock2.h</ForcedIncludeFiles>'
  W '    </ClCompile>'
}
W '  </ItemGroup>'
W '  <ItemGroup Label="tests">'
W '    <ClCompile Include="tests\win\all_tests.cpp" />'
# Windows-ONLY test files.  They live under tests\win\ rather than tests\
# because they drive code the macOS test executables cannot link -- the prefs
# pages are mushgui_adapters TUs there -- so the tests\test_*.cpp glob above
# must not pick them up.  Listed by hand, one line each.
W '    <ClCompile Include="tests\win\test_prefs_pages.cpp" />'
W '    <ClCompile Include="tests\win\test_gap_coverage.cpp" />'
foreach ($t in $tests) { W "    <ClCompile Include=`"$t`" />" }
W '  </ItemGroup>'
W '  <ItemGroup>'
W '    <ResourceCompile Include="MUSHclient.rc" />'
W '  </ItemGroup>'
W '  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />'
W '</Project>'

Set-Content -Path 'MUSHclient_tests.vcxproj' -Value $sb.ToString().TrimEnd() -Encoding UTF8
"wrote MUSHclient_tests.vcxproj: $($sources.Count) engine sources, $($tests.Count) test files + 2 Windows-only, $($excluded.Count) excluded"
