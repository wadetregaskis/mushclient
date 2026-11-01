<#
.SYNOPSIS
  Build the third-party pieces a 64-bit (x64 or ARM64) MUSHclient links and ships.

.DESCRIPTION
  The Win32 build uses prebuilts committed to the repository -- openssl\lib\
  libcrypto.lib + libssl.lib (static OpenSSL 3.5, VC-WIN32, /MT) and
  src\lua5.1.lib (import library for lua5.1.dll) -- plus lua5.1.dll from an
  existing installation.  Every one is 32-bit x86.  This script produces the
  64-bit set for one architecture, from source wherever source exists, plus the
  scripting runtimes the 64-bit builds embed (MUSHclient.engines.props).

  -Arch x86 builds just the two Win32 pieces that are made from source:
  -Only openssl (then copy deps\Win32\lib\*.lib to openssl\lib\ and
  deps\Win32\include\openssl\*.h to openssl\include\openssl\, and commit them)
  and -Only luasocket (the installer takes those from deps\Win32\bin\; a
  development copy wants them beside the exe, as socket\core.dll and
  mime\core.dll, because lua\socket*.lua, shared by every build, need them).

    openssl    OpenSSL 3.5.x, static libcrypto.lib / libssl.lib + headers,
               built the way the committed x86 libs were (VC-WIN32,
               no-shared, /MT /Zl).  Default: the newest 3.5.x on GitHub,
               verified against its published .sha256.
    lua        Lua 5.1.5 as lua5.1.dll + lua5.1.lib (lua.org, pinned), every
               symbol the x86 DLL exports, x86's rounding (Build-Lua).
    luasocket  LuaSocket 3.1.0: socket\core.dll + mime\core.dll.
    dina       the Dina bitmap font one bundled plugin uses, from its author's
               own release (pinned; MIT licence) -- architecture-neutral.
    python     python.org's NuGet package: python3XX.dll + python\.
    tcl        Tcl 8.6, static, + its script library (tcl8.6\).
    quickjs    QuickJS-ng, static: the JavaScript engine.
    ruby       Ruby 4.0, MSVC build: its DLL + lib\ruby\.
    locale     NOT run by default: rehost a 32-bit resource DLL -- a
               translation made for the 32-bit client -- for this
               architecture (-LocaleSource).  The client's own locale\en.dll
               is built from source by resources\en.vcxproj.

  Output, git-ignored, laid out for MUSHclient.vcxproj's 64-bit configurations:

    packaging\windows\deps\<Platform>\include\...   openssl, python, tcl, quickjs, ruby
    packaging\windows\deps\<Platform>\lib\...       the import / static libraries
    packaging\windows\deps\<Platform>\bin\...       what ships beside the exe, in its layout
    packaging\windows\deps\<Platform>\VERSIONS.txt  what was built, from what
    packaging\windows\deps\common\                  architecture-neutral files (Dina)

  <Platform> is the MSBuild platform name: x64 or ARM64.

  ENVIRONMENT.  Run from any PowerShell (Windows PowerShell 5.1 or pwsh 7) on a
  machine with Visual Studio 2022 and its "Desktop development with C++"
  workload, including the x64/ARM64 build tools you are targeting.  The script
  finds VS with vswhere and imports vcvarsall.bat's environment itself, so it
  does NOT need a Developer PowerShell (it works from one too).  Also needed:

    - Perl for Windows (Strawberry Perl; preinstalled on GitHub's Windows
      runners; the portable zip on PATH works too).  An MSYS/Git-for-Windows
      perl will not do: OpenSSL needs one that reports $^O = MSWin32.
    - x64 only, unless -NoAsm: NASM 2.09+ on PATH (winget install NASM.NASM).
    - ARM64: OpenSSL's assembler for Windows-on-ARM is clang-cl (VS component
      "C++ Clang Compiler for Windows"; found in VS without being on PATH).
      Without it the build falls back to VC-WIN64-ARM, which has no assembler.
    - tar.exe from Windows 10 1803+ (System32), for the .tar.gz sources.

.PARAMETER Arch
  x64 or arm64 (case-insensitive), or x86 with -Only openssl and/or luasocket.

.PARAMETER OpenSslVersion
  Exact OpenSSL release, e.g. 3.5.9.  Default: newest final 3.5.x on GitHub.

.PARAMETER NoAsm
  Build OpenSSL without assembler (no NASM / clang-cl needed; slower crypto).

.PARAMETER LocaleSource
  For -Only locale: the directory holding the 32-bit resource DLLs to rehost.

.PARAMETER Only
  Build only these components (see above).  luasocket needs lua built first
  (it links lua5.1.lib); ruby needs openssl.  From PowerShell call the script
  in-process (& .\fetch-deps.ps1 -Only a,b): powershell -File passes a list
  as one string.

.PARAMETER StageTo
  After building, copy bin\ (the runtime files, in their runtime layout) and
  the VC++ runtime into this directory -- e.g. WinDebug-x64 -- so the built
  exe can run there.

.PARAMETER Force
  Rebuild components that are already built.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File packaging\windows\fetch-deps.ps1 -Arch x64
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File packaging\windows\fetch-deps.ps1 -Arch arm64 -StageTo WinDebug-ARM64
.EXAMPLE
  & .\packaging\windows\fetch-deps.ps1 -Arch x64 -Only locale -LocaleSource 'C:\Program Files (x86)\MUSHclient\locale'
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [ValidateSet('x64', 'arm64', 'x86')] [string]$Arch,      # case-insensitive: ARM64 works too
  [string]$OpenSslVersion = '',
  [string]$OpenSslSeries = '3.5',
  [switch]$NoAsm,
  [string]$LocaleSource = '',
  [ValidateSet('openssl', 'lua', 'luasocket', 'dina', 'locale', 'python', 'tcl', 'ruby', 'quickjs')] [string[]]$Only = @(),
  [string]$StageTo = '',
  [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'     # Invoke-WebRequest is 10x slower with it
Set-StrictMode -Version 2

# Windows PowerShell 5.1 defaults to TLS 1.0/1.1, which GitHub and lua.org refuse.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

# ---------------------------------------------------------------- constants ---

$Platform = @{ x64 = 'x64'; arm64 = 'ARM64'; x86 = 'Win32' }[$Arch.ToLowerInvariant()]     # MSBuild's spelling
$Machine  = @{ x64 = 'X64'; ARM64 = 'ARM64'; Win32 = 'X86' }[$Platform]      # link /MACHINE:
$PeMachine = @{ x64 = 0x8664; ARM64 = 0xAA64; Win32 = 0x14C }[$Platform]
if ($Platform -eq 'Win32' -and (-not $Only.Count -or ($Only | Where-Object { $_ -notin 'openssl', 'luasocket' }))) {
  throw '-Arch x86 builds only -Only openssl and/or luasocket: the rest of Win32 is committed or comes from an installation'
}

$Repo   = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Deps   = Join-Path $PSScriptRoot 'deps'
$Dl     = Join-Path $Deps '_downloads'
$Work   = Join-Path $Deps "_build\$Platform"
$Out    = Join-Path $Deps $Platform
$OutInc = Join-Path $Out 'include'
$OutLib = Join-Path $Out 'lib'
$OutBin = Join-Path $Out 'bin'

$LuaVersion   = '5.1.5'
$LuaUrl       = "https://www.lua.org/ftp/lua-$LuaVersion.tar.gz"
$LuaSha256    = '2640fc56a795f29d28ef15e13c34a47e223960b0240e8cb0a82d9b0738695333'   # lua.org/ftp
$SocketTag    = 'v3.1.0'
$SocketUrl    = "https://github.com/lunarmodules/luasocket/archive/refs/tags/$SocketTag.tar.gz"
$SocketSha256 = 'bf033aeb9e62bcaa8d007df68c119c966418e8c9ef7e4f2d7e96bddeca9cca6e'   # GitHub's tag archive

# The embedded scripting languages: the same versions packaging/fetch-*.sh
# bundle on macOS and Linux, give or take a patch release.
$RubyVersion  = '4.0.5'
$RubyUrl      = "https://cache.ruby-lang.org/pub/ruby/4.0/ruby-$RubyVersion.tar.gz"
$RubySha256   = '7d6149079a63f8ae1d326c9fa65c6019ba2dc3155eae7b39159817911c88958e'   # cache.ruby-lang.org/pub/ruby/index.txt
$YamlVersion  = '0.2.5'
$YamlUrl      = "https://github.com/yaml/libyaml/releases/download/$YamlVersion/yaml-$YamlVersion.tar.gz"
$YamlSha256   = 'c642ae9b75fee120b2d96c712538bd2cf283228d2337df2cf2988e3c02678ef4'
# python.org's own builds, as NuGet packages: one per architecture, each the
# complete runtime (DLL, stdlib, extension modules, headers, import library).
$PythonVersion = '3.14.10'
$PythonPkg     = @{ x64 = 'python'; ARM64 = 'pythonarm64' }
$PythonSha256  = @{ x64   = 'f54ba67c9951c3d773ea47d2bb095150c037c399f0423a968bcda22809241784'
                    ARM64 = '79cac95bc8219587eaf4b073a92162388f567a2509cd2bc497102feaf7dc8a38' }
$TclVersion    = '8.6.18'
$TclUrl        = "https://prdownloads.sourceforge.net/tcl/tcl$TclVersion-src.tar.gz"
$TclSha256     = '14f9af32b1767ff718477a8f974ad03c34341097e6b43f4ce54644ee974e268e'
$QuickJsVersion = '0.17.0'
$QuickJsUrl     = "https://github.com/quickjs-ng/quickjs/archive/refs/tags/v$QuickJsVersion.tar.gz"
$QuickJsSha256  = '559bc4c420475e55c7ab4510adbc562f55d7524d75e8e89d79ce4bb02f5687d9'   # GitHub's tag archive
# Dina, the bitmap font the Current_Output_Window plugin loads (Dina.fon in
# the install directory): Joergen Ibsen's own release, MIT-licensed.  Its
# regular face, DinaR.fon, ships as Dina.fon, the name the plugin uses.
$DinaUrl       = 'https://www.dcmembers.com/jibsen/_dl/61/Dina.zip'
$DinaSha256    = '1f51bba53f75a64d2d8bd037e8e0f84b6f8064e50a72ee954033bede173508cf'

# Every name the shipped 32-bit lua5.1.dll exports (Lua 5.1.4, read from its
# export table).  A stock 5.1.5 built with LUA_BUILD_AS_DLL exports all of
# them; the build fails if any is missing, because a plugin's C module that
# imports one would otherwise fail to load on 64-bit only.
$LuaExports = @(
  'luaL_addlstring','luaL_addstring','luaL_addvalue','luaL_argerror','luaL_buffinit',
  'luaL_callmeta','luaL_checkany','luaL_checkinteger','luaL_checklstring','luaL_checknumber',
  'luaL_checkoption','luaL_checkstack','luaL_checktype','luaL_checkudata','luaL_error',
  'luaL_findtable','luaL_getmetafield','luaL_gsub','luaL_loadbuffer','luaL_loadfile',
  'luaL_loadstring','luaL_newmetatable','luaL_newstate','luaL_openlib','luaL_openlibs',
  'luaL_optinteger','luaL_optlstring','luaL_optnumber','luaL_prepbuffer','luaL_pushresult',
  'luaL_ref','luaL_register','luaL_typerror','luaL_unref','luaL_where',
  'lua_atpanic','lua_call','lua_checkstack','lua_close','lua_concat','lua_cpcall',
  'lua_createtable','lua_dump','lua_equal','lua_error','lua_gc','lua_getallocf',
  'lua_getfenv','lua_getfield','lua_gethook','lua_gethookcount','lua_gethookmask',
  'lua_getinfo','lua_getlocal','lua_getmetatable','lua_getstack','lua_gettable',
  'lua_gettop','lua_getupvalue','lua_insert','lua_iscfunction','lua_isnumber',
  'lua_isstring','lua_isuserdata','lua_lessthan','lua_load','lua_newstate',
  'lua_newthread','lua_newuserdata','lua_next','lua_objlen','lua_pcall',
  'lua_pushboolean','lua_pushcclosure','lua_pushfstring','lua_pushinteger',
  'lua_pushlightuserdata','lua_pushlstring','lua_pushnil','lua_pushnumber',
  'lua_pushstring','lua_pushthread','lua_pushvalue','lua_pushvfstring','lua_rawequal',
  'lua_rawget','lua_rawgeti','lua_rawset','lua_rawseti','lua_remove','lua_replace',
  'lua_resume','lua_setallocf','lua_setfenv','lua_setfield','lua_sethook',
  'lua_setlevel','lua_setlocal','lua_setmetatable','lua_settable','lua_settop',
  'lua_setupvalue','lua_status','lua_toboolean','lua_tocfunction','lua_tointeger',
  'lua_tolstring','lua_tonumber','lua_topointer','lua_tothread','lua_touserdata',
  'lua_type','lua_typename','lua_xmove','lua_yield',
  'luaopen_base','luaopen_debug','luaopen_io','luaopen_math','luaopen_os',
  'luaopen_package','luaopen_string','luaopen_table'
)

$Components = if ($Only.Count) { $Only } else { @('openssl', 'lua', 'luasocket', 'dina', 'python', 'tcl', 'ruby', 'quickjs') }
$Versions = [ordered]@{}

function Note([string]$m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Warn([string]$m) { Write-Host "warning: $m" -ForegroundColor Yellow }

# Run a native tool; throw on a non-zero exit (PowerShell does not).
function Exec {
  param([string]$Exe, [string[]]$Arguments, [string]$Dir = $null)
  if ($Dir) { Push-Location $Dir }
  # Under 'Stop', Windows PowerShell 5.1 can turn a tool's stderr chatter
  # into a terminating error; the exit code is the only verdict wanted.
  $eap = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    & $Exe @Arguments
    $rc = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $eap
    if ($Dir) { Pop-Location }
  }
  if ($rc -ne 0) { throw "$Exe failed (exit $rc): $($Arguments -join ' ')" }
}

function New-Dir([string]$p) { New-Item -ItemType Directory -Force -Path $p | Out-Null; $p }

# ------------------------------------------------------- Visual Studio env ---

# vcvarsall's argument is HOST_TARGET.  Use native tools where the host is the
# target, the cross compiler otherwise (x64 -> ARM64 is the usual case: a VM
# on an Intel/AMD machine).
function Import-VsEnvironment {
  # The MACHINE, not this process: an x64 PowerShell under emulation on an ARM64
  # machine is told X64 by .NET and by PROCESSOR_ARCHITECTURE alike; the SYSTEM
  # environment (what a native process would inherit) says what is underneath.
  $hostArch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
  $sysArch = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment' -ErrorAction SilentlyContinue).PROCESSOR_ARCHITECTURE
  if ($sysArch -eq 'ARM64') { $hostArch = 'Arm64' }
  $vcArg = switch ($Platform) {
    'x64'   { if ($hostArch -eq 'Arm64') { 'arm64_x64' } else { 'x64' } }
    'Win32' { if ($hostArch -eq 'Arm64') { 'arm64_x86' } else { 'x64_x86' } }
    'ARM64' { if ($hostArch -eq 'Arm64') { 'arm64' } else { 'x64_arm64' } }
  }
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found: install Visual Studio 2022 with the C++ workload' }
  $component = if ($Platform -eq 'ARM64') { 'Microsoft.VisualStudio.Component.VC.Tools.ARM64' }
               else { 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' }
  $vs = & $vswhere -latest -products * -requires $component -property installationPath
  if (-not $vs) { throw "no Visual Studio with $component (the $Platform C++ build tools) was found" }
  $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
  Note "Visual Studio: $vs  (vcvarsall $vcArg)"
  $envDump = & cmd.exe /s /c "`"$vcvars`" $vcArg >nul 2>&1 && set"
  if ($LASTEXITCODE -ne 0) { throw "vcvarsall.bat $vcArg failed" }
  foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
  }
  foreach ($tool in 'cl', 'link', 'lib', 'rc', 'nmake', 'dumpbin') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool not on PATH after vcvarsall $vcArg" }
  }
  # vcvarsall does not put VS's own clang-cl (the "C++ Clang Compiler for
  # Windows" component) on PATH; OpenSSL's ARM64 assembler is that clang-cl.
  if (-not (Get-Command clang-cl -ErrorAction SilentlyContinue)) {
    $llvmHost = if ($hostArch -eq 'Arm64') { 'ARM64' } else { 'x64' }
    $llvm = Join-Path $vs "VC\Tools\Llvm\$llvmHost\bin"
    if (Test-Path (Join-Path $llvm 'clang-cl.exe')) { $env:PATH = "$llvm;$env:PATH" }
  }
}

# ---------------------------------------------------------------- helpers ---

function Get-Download {
  param([string]$Url, [string]$Name, [string]$Sha256 = '')
  New-Dir $Dl | Out-Null
  $dest = Join-Path $Dl $Name
  if (-not (Test-Path $dest)) {
    Note "downloading $Url"
    # A non-browser User-Agent: SourceForge answers a browser's with an HTML
    # download page instead of the file.
    Invoke-WebRequest -UseBasicParsing -UserAgent 'curl/8.0' -Uri $Url -OutFile "$dest.part"
    Move-Item -Force "$dest.part" $dest
  }
  $got = (Get-FileHash -Algorithm SHA256 $dest).Hash.ToLowerInvariant()
  if ($Sha256 -and $got -ne $Sha256.ToLowerInvariant()) {
    Remove-Item -Force $dest
    throw "checksum mismatch for $Name (got $got, want $Sha256); deleted it, re-run to download again"
  }
  if (-not $Sha256) { Warn "$Name is not pinned; its SHA-256 is $got (recorded in VERSIONS.txt)" }
  @{ Path = $dest; Sha256 = $got }
}

function Expand-TarGz([string]$Archive, [string]$Into) {
  if (Test-Path $Into) { Remove-Item -Recurse -Force $Into }
  New-Dir $Into | Out-Null
  # The System32 bsdtar, explicitly: a Git or MSYS tar earlier on PATH reads
  # C:\... as a remote host name.
  Exec "$env:SystemRoot\System32\tar.exe" @('-xzf', $Archive, '-C', $Into, '--strip-components=1')
}

# The architecture a PE file was built for, from its COFF header.
function Get-PeMachine([string]$Path) {
  $fs = [System.IO.File]::OpenRead($Path)
  try {
    $br = New-Object System.IO.BinaryReader($fs)
    $fs.Position = 0x3C
    $pe = $br.ReadInt32()
    $fs.Position = $pe + 4
    [int]$br.ReadUInt16()
  } finally { $fs.Dispose() }
}

function Assert-Machine([string]$Path) {
  $m = Get-PeMachine $Path
  if ($m -ne $PeMachine) { throw ('{0} is machine 0x{1:X4}, expected 0x{2:X4} ({3})' -f $Path, $m, $PeMachine, $Platform) }
}

# The COFF machine of the first object member of a static library (enough to
# catch an x86 OpenSSL in the wrong directory).
function Assert-LibMachine([string]$Path) {
  $out = & dumpbin /nologo /headers $Path | Select-String -Pattern 'machine \((\w+)\)' | Select-Object -First 1
  if (-not $out) { Warn "could not read the machine type of $Path"; return }
  $want = @{ x64 = 'x64'; ARM64 = 'ARM64'; Win32 = 'x86' }[$Platform]
  if ($out.Matches[0].Groups[1].Value -ne $want) { throw "$Path is $($out.Matches[0].Groups[1].Value), expected $want" }
}

# ---------------------------------------------------------------- OpenSSL ---

function Get-LatestOpenSsl([string]$Series) {
  $rel = Invoke-RestMethod -UseBasicParsing -Uri 'https://api.github.com/repos/openssl/openssl/releases?per_page=100'
  $best = -1
  foreach ($r in $rel) {
    if ($r.draft -or $r.prerelease) { continue }
    if ($r.tag_name -match ('^openssl-' + [regex]::Escape($Series) + '\.(\d+)$')) {
      if ([int]$Matches[1] -gt $best) { $best = [int]$Matches[1] }
    }
  }
  if ($best -lt 0) { throw "no OpenSSL $Series release found on GitHub" }
  "$Series.$best"
}

function Build-OpenSsl {
  $ver = $OpenSslVersion
  if (-not $ver) { $ver = Get-LatestOpenSsl $OpenSslSeries }
  $stamp = Join-Path $Out '.openssl-version'
  if (-not $Force -and (Test-Path $stamp) -and ((Get-Content $stamp) -eq $ver) -and
      (Test-Path (Join-Path $OutLib 'libcrypto.lib'))) {
    Note "OpenSSL $ver ($Platform) is up to date"
    $Versions['OpenSSL'] = "$ver (already built)"
    return
  }

  if (-not (Get-Command perl -ErrorAction SilentlyContinue)) {
    throw 'perl not found: install Strawberry Perl (winget install StrawberryPerl.StrawberryPerl)'
  }
  $perlOs = & perl -e 'print $^O'
  if ($perlOs -ne 'MSWin32') {
    throw "OpenSSL needs a native Windows Perl (Strawberry Perl) first on PATH; 'perl' reports '$perlOs'"
  }

  $base = "https://github.com/openssl/openssl/releases/download/openssl-$ver"
  $name = "openssl-$ver.tar.gz"
  $shaText = (Invoke-WebRequest -UseBasicParsing -Uri "$base/$name.sha256").Content
  if ($shaText -is [byte[]]) { $shaText = [System.Text.Encoding]::ASCII.GetString($shaText) }
  $want = ($shaText.Trim() -split '\s+')[0].ToLowerInvariant()
  if ($want -notmatch '^[0-9a-f]{64}$') { throw "unexpected checksum file for OpenSSL ${ver}: $shaText" }
  $tgz = Get-Download -Url "$base/$name" -Name $name -Sha256 $want

  # Target and assembler.  The committed x86 libs say "platform: VC-WIN32";
  # these are its 64-bit siblings.
  $cfgArgs = @()
  if ($Platform -ne 'ARM64') {
    $target = if ($Platform -eq 'x64') { 'VC-WIN64A' } else { 'VC-WIN32' }
    if ($NoAsm) { $cfgArgs += 'no-asm' }
    elseif (-not (Get-Command nasm -ErrorAction SilentlyContinue)) {
      throw 'NASM is not on PATH (winget install NASM.NASM, then reopen the shell), or pass -NoAsm'
    }
  } else {
    # VC-WIN64-CLANGASM-ARM: MSVC compiles, clang-cl assembles the AArch64
    # perlasm output.  VC-WIN64-ARM (the OneCore target) has no asm at all.
    if (-not $NoAsm -and (Get-Command clang-cl -ErrorAction SilentlyContinue)) {
      $target = 'VC-WIN64-CLANGASM-ARM'
    } else {
      if (-not $NoAsm) { Warn 'clang-cl not found: building OpenSSL for ARM64 without assembler (VC-WIN64-ARM)' }
      $target = 'VC-WIN64-ARM'
      $cfgArgs += 'no-asm'
    }
  }

  $src = Join-Path $Work 'openssl'
  Expand-TarGz $tgz.Path $src
  Note "building OpenSSL $ver ($target, static, /MT) -- this takes a while"
  # no-shared: static libcrypto.lib/libssl.lib with /MT /Zl, as for x86 --
  # MUSHclient links the CRT statically and the installer ships no OpenSSL DLL.
  #
  # No --prefix / --openssldir, deliberately.  Both are compiled into the
  # library (OPENSSLDIR is where openssl.cnf is looked for at start-up), so
  # they must name directories an ordinary user cannot create: a build-tree
  # path such as C:\src\... can be created by anyone on the user's machine,
  # which is CVE-2019-1552's shape.  The defaults are under Program Files.
  # Nothing is installed: the libraries and the generated headers are taken
  # straight from the build tree.
  Exec perl (@('Configure', $target, 'no-shared', 'no-tests') + $cfgArgs) $src
  Exec nmake @('/nologo', 'build_libs') $src

  New-Dir $OutLib | Out-Null
  if (Test-Path (Join-Path $OutInc 'openssl')) { Remove-Item -Recurse -Force (Join-Path $OutInc 'openssl') }
  New-Dir $OutInc | Out-Null
  $incDest = New-Dir (Join-Path $OutInc 'openssl')
  # *.h only: the tree also holds the *.h.in templates they were generated from.
  Copy-Item (Join-Path $src 'include\openssl\*.h') $incDest
  foreach ($l in 'libcrypto.lib', 'libssl.lib') {
    Copy-Item -Force (Join-Path $src $l) $OutLib
    Assert-LibMachine (Join-Path $OutLib $l)
  }
  Set-Content -Path $stamp -Value $ver
  $Versions['OpenSSL'] = "$ver ($target, no-shared, sha256 $($tgz.Sha256))"
}

# --------------------------------------------------------------------- Lua ---

function Build-Lua {
  $dll = Join-Path $OutBin 'lua5.1.dll'
  if (-not $Force -and (Test-Path $dll) -and (Test-Path (Join-Path $OutLib 'lua5.1.lib'))) {
    Note "lua5.1.dll ($Platform) is up to date"
    $Versions['Lua'] = "$LuaVersion (already built)"
    return
  }
  $tgz = Get-Download -Url $LuaUrl -Name "lua-$LuaVersion.tar.gz" -Sha256 $LuaSha256
  $src = Join-Path $Work 'lua'
  Expand-TarGz $tgz.Path $src
  $s = Join-Path $src 'src'
  if (Test-Path (Join-Path $Work 'lua-obj')) { Remove-Item -Recurse -Force (Join-Path $Work 'lua-obj') }
  $obj = New-Dir (Join-Path $Work 'lua-obj')
  New-Dir $OutBin | Out-Null; New-Dir $OutLib | Out-Null

  # Parity with the 32-bit DLL in how a number becomes an integer.  On x86,
  # luaconf.h ROUNDS (the 2^52+2^51 union trick under GCC, which is how the
  # shipped DLL was built, or fistp under MSVC: round half to even either
  # way).  Every 64-bit target falls through to a plain C cast, which
  # TRUNCATES.  So lua_tointeger / luaL_checkint / luaL_optint on 2.7 give 3
  # in the 32-bit client and 2 in a stock 64-bit build: a plugin passing a
  # computed coordinate to WindowRectOp would draw a pixel off.  lrint and
  # llrint round half to even in the default floating-point mode, which is
  # the x86 answer.  (lua_Integer is ptrdiff_t, 64-bit here, so values past
  # 2^31 now survive where x86 wrapped them; nothing sensible can copy that.)
  $conf = Join-Path $s 'luaconf.h'
  $text = [System.IO.File]::ReadAllText($conf)
  $old1 = "#define lua_number2int(i,d)`t((i)=(int)(d))"
  $old2 = "#define lua_number2integer(i,d)`t((i)=(lua_Integer)(d))"
  if (-not $text.Contains($old1) -or -not $text.Contains($old2)) {
    throw "luaconf.h: the generic lua_number2int definitions were not found; re-check the rounding patch against Lua $LuaVersion"
  }
  $text = $text.Replace($old1, "#include <math.h>`n#define lua_number2int(i,d)`t((i)=(int)lrint(d))")
  $text = $text.Replace($old2, "#define lua_number2integer(i,d)`t((i)=(lua_Integer)llrint(d))")
  [System.IO.File]::WriteAllText($conf, $text)

  # The library is every .c in src\ except the two programs and luac's helper.
  $files = Get-ChildItem (Join-Path $s '*.c') | Where-Object { $_.Name -notin @('lua.c', 'luac.c', 'print.c') } |
           ForEach-Object { $_.FullName }
  Note "building lua5.1.dll $LuaVersion"
  # LUA_BUILD_AS_DLL makes LUA_API/LUALIB_API __declspec(dllexport) (luaconf.h).
  # /MT: like the exe, no VC++ redistributable to ship.  The x86 DLL used
  # msvcrt.dll, so the exe and the DLL already had separate CRTs; nothing in
  # the Lua C API passes CRT objects across, so that stays fine.
  # Compiled from inside the object directory: an argument ending in a
  # backslash (/Fo<dir>\) is mis-quoted by Windows PowerShell 5.1 when the
  # path holds a space.
  Exec cl (@('/nologo', '/c', '/O2', '/MT', '/W3', '/Zi', '/Fdlua51.pdb', '/DLUA_BUILD_AS_DLL',
             '/D_CRT_SECURE_NO_DEPRECATE') + $files) $obj
  Exec link (@('/nologo', '/DLL', '/DEBUG', '/OPT:REF', '/OPT:ICF', "/MACHINE:$Machine",
               "/OUT:$dll", "/IMPLIB:$OutLib\lua5.1.lib", "/PDB:$OutBin\lua5.1.pdb") +
             (Get-ChildItem (Join-Path $obj '*.obj') | ForEach-Object { $_.FullName }))
  Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $OutLib 'lua5.1.exp')
  Assert-Machine $dll

  $exports = & dumpbin /nologo /exports $dll | ForEach-Object {
    if ($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)') { $Matches[1] }
  }
  $missing = $LuaExports | Where-Object { $exports -notcontains $_ }
  if ($missing) { throw "lua5.1.dll is missing exports the x86 DLL has: $($missing -join ', ')" }
  Note "lua5.1.dll exports all $($LuaExports.Count) names the x86 build does ($(@($exports).Count) in total)"
  $Versions['Lua'] = "$LuaVersion (lua.org, sha256 $($tgz.Sha256))"
}

# --------------------------------------------------------------- LuaSocket ---

function Build-LuaSocket {
  $sock = Join-Path $OutBin 'socket\core.dll'
  $mime = Join-Path $OutBin 'mime\core.dll'
  if (-not $Force -and (Test-Path $sock) -and (Test-Path $mime)) {
    Note "LuaSocket ($Platform) is up to date"
    $Versions['LuaSocket'] = "$SocketTag (already built)"
    return
  }
  # Win32 links the committed import library of the shipped x86 lua5.1.dll,
  # against the headers beside it.
  $luaLib = if ($Platform -eq 'Win32') { Join-Path $Repo 'src\lua5.1.lib' } else { Join-Path $OutLib 'lua5.1.lib' }
  if (-not (Test-Path $luaLib)) { throw 'LuaSocket links lua5.1.lib: build lua first (-Only lua)' }
  $luaInc = if ($Platform -eq 'Win32') { Join-Path $Repo 'src' } else { Join-Path $Work 'lua\src' }
  if (-not (Test-Path (Join-Path $luaInc 'lua.h'))) {
    $t = Get-Download -Url $LuaUrl -Name "lua-$LuaVersion.tar.gz" -Sha256 $LuaSha256
    Expand-TarGz $t.Path (Join-Path $Work 'lua')
  }
  $tgz = Get-Download -Url $SocketUrl -Name "luasocket-$SocketTag.tar.gz" -Sha256 $SocketSha256
  $src = Join-Path $Work 'luasocket'
  Expand-TarGz $tgz.Path $src
  $s = Join-Path $src 'src'
  New-Dir (Split-Path $sock) | Out-Null; New-Dir (Split-Path $mime) | Out-Null
  $common = @('/nologo', '/c', '/O2', '/MT', '/W3', "/I$luaInc", '/D_CRT_SECURE_NO_WARNINGS',
              '/D_WINSOCK_DEPRECATED_NO_WARNINGS')

  Note "building socket\core.dll (LuaSocket $SocketTag)"
  $objS = New-Dir (Join-Path $Work 'socket-obj')
  $socketFiles = 'luasocket', 'timeout', 'buffer', 'io', 'auxiliar', 'options', 'inet', 'tcp',
                 'udp', 'except', 'select', 'wsocket', 'compat' | ForEach-Object { Join-Path $s "$_.c" }
  Exec cl ($common + @('/DLUASOCKET_API=__declspec(dllexport)') + $socketFiles) $objS
  Exec link (@('/nologo', '/DLL', "/MACHINE:$Machine", "/OUT:$sock", "/IMPLIB:$objS\core.lib") +
             (Get-ChildItem (Join-Path $objS '*.obj') | ForEach-Object { $_.FullName }) +
             @($luaLib, 'ws2_32.lib'))

  Note "building mime\core.dll"
  $objM = New-Dir (Join-Path $Work 'mime-obj')
  Exec cl ($common + @('/DMIME_API=__declspec(dllexport)', '/DLUASOCKET_API=__declspec(dllexport)',
                       (Join-Path $s 'mime.c'), (Join-Path $s 'compat.c'))) $objM
  Exec link @('/nologo', '/DLL', "/MACHINE:$Machine", "/OUT:$mime", "/IMPLIB:$objM\core.lib",
              (Join-Path $objM 'mime.obj'), (Join-Path $objM 'compat.obj'), $luaLib)

  foreach ($d in $sock, $mime) { Assert-Machine $d }
  $Versions['LuaSocket'] = "$SocketTag (github.com/lunarmodules/luasocket, sha256 $($tgz.Sha256))"
}

# ------------------------------------------------------------------ Python ---

# Python for the embedded engine (src/platform/py_engine.cpp), from python.org's
# NuGet package rather than built: it IS python.org's Windows build, and the
# package carries the import library and headers an embedder needs.
#
#   bin\python3XX.dll, python3.dll   beside MUSHclient.exe (the exe imports
#                                    python3XX.dll, so it must be there)
#   bin\python\Lib, bin\python\DLLs  the stdlib and extension modules; the
#                                    engine sets PyConfig.home to bin\python
function Build-Python {
  $stamp = Join-Path $Out '.python-version'
  if (-not $Force -and (Test-Path $stamp) -and ((Get-Content $stamp) -eq $PythonVersion)) {
    Note "Python $PythonVersion ($Platform) is up to date"
    $Versions['Python'] = "$PythonVersion (already built)"
    return
  }
  $pkg = $PythonPkg[$Platform]
  $nupkg = Get-Download -Url "https://api.nuget.org/v3-flatcontainer/$pkg/$PythonVersion/$pkg.$PythonVersion.nupkg" `
                        -Name "$pkg.$PythonVersion.nupkg" -Sha256 $PythonSha256[$Platform]
  $x = Join-Path $Work 'python-pkg'
  if (Test-Path $x) { Remove-Item -Recurse -Force $x }
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  [IO.Compression.ZipFile]::ExtractToDirectory($nupkg.Path, $x)
  $tools = Join-Path $x 'tools'
  $short = ($PythonVersion.Split('.')[0..1]) -join ''            # 3.14.10 -> 314
  $dll = Join-Path $tools "python$short.dll"
  Assert-Machine $dll

  Get-ChildItem $OutBin -Filter 'python3*.dll' -ErrorAction SilentlyContinue | Remove-Item -Force
  $home_ = Join-Path $OutBin 'python'
  if (Test-Path $home_) { Remove-Item -Recurse -Force $home_ }
  New-Dir $home_ | Out-Null
  Copy-Item $dll, (Join-Path $tools 'python3.dll') $OutBin
  # The runtime's own C runtime DLLs: the newest of these and VS's redist is
  # what build-app.ps1 ships beside the exe (Ruby needs them too).
  New-Dir (Join-Path $Out 'crt-python') | Out-Null
  Copy-Item (Join-Path $tools 'vcruntime140*.dll') (Join-Path $Out 'crt-python')
  Copy-Item -Recurse (Join-Path $tools 'DLLs') (Join-Path $home_ 'DLLs')
  Copy-Item -Recurse (Join-Path $tools 'Lib') (Join-Path $home_ 'Lib')
  # Not useful inside a MUD client: the IDE, Tk (whose DLLs the package does
  # not even carry) and its demos.  Everything else stays, ensurepip included.
  foreach ($d in 'idlelib', 'tkinter', 'turtledemo', 'test') {
    $p = Join-Path $home_ "Lib\$d"
    if (Test-Path $p) { Remove-Item -Recurse -Force $p }
  }
  # Nor pip: there is no python.exe in a MUSHclient package to run it with,
  # and it carries launcher templates for three other architectures
  # (distlib\t32.exe, w64-arm.exe, ...), which build-app.ps1's single-
  # architecture check rightly refuses.  A plugin author who wants packages
  # installs the matching python.org Python and runs pip install --user: the
  # embedded interpreter reads the per-user site-packages.  (ensurepip's own
  # wheel stays, for a future python.exe.)
  Get-ChildItem (Join-Path $home_ 'Lib\site-packages') -Filter 'pip*' -ErrorAction SilentlyContinue |
    Remove-Item -Recurse -Force
  $inc = Join-Path $OutInc 'python'
  if (Test-Path $inc) { Remove-Item -Recurse -Force $inc }
  Copy-Item -Recurse (Join-Path $tools 'include') $inc
  Copy-Item -Force (Join-Path $tools "libs\python$short.lib") $OutLib
  Set-Content -Path $stamp -Value $PythonVersion
  $Versions['Python'] = "$PythonVersion (nuget.org $pkg, sha256 $($nupkg.Sha256))"
}

# --------------------------------------------------------------------- Tcl ---

# Tcl 8.6 for the embedded engine (src/platform/tcl_engine.cpp), static and
# /MT like the exe -- packaging/fetch-tcl.sh builds it static on macOS and Linux
# too.  Its script library (init.tcl, encodings, msgs) is the one runtime file:
#
#   bin\tcl8.6\                         the engine points TCL_LIBRARY here
function Build-Tcl {
  $stamp = Join-Path $Out '.tcl-version'
  if (-not $Force -and (Test-Path $stamp) -and ((Get-Content $stamp) -eq $TclVersion)) {
    Note "Tcl $TclVersion ($Platform) is up to date"
    $Versions['Tcl'] = "$TclVersion (already built)"
    return
  }
  $tgz = Get-Download -Url $TclUrl -Name "tcl$TclVersion-src.tar.gz" -Sha256 $TclSha256
  $src = Join-Path $Work 'tcl'
  Expand-TarGz $tgz.Path $src
  $inst = Join-Path $Work 'tcl-install'
  if (Test-Path $inst) { Remove-Item -Recurse -Force $inst }
  $win = Join-Path $src 'win'
  # OPTS=static: tcl86ts.lib, compiled /MT.  'core' only -- the bundled
  # packages (itcl, tdbc, thread, sqlite) are separate DLLs in 8.6 and nothing
  # in the engine loads them.
  #
  # TCLSH_NATIVE: rules.vc insists on a host-runnable tclsh whenever the target
  # is not the host's own architecture (an x64 build on an ARM64 machine counts),
  # and the install step runs one (tools\installData.tcl).  Build the target's
  # own tclsh first and use that: x64 runs everywhere this script does, and an
  # ARM64 build is only ever made on ARM64 (release.yml's windows-11-arm).
  Note "building Tcl $TclVersion ($Platform, static)"
  $nm = @('/nologo', '-f', 'makefile.vc', 'OPTS=static', "INSTALLDIR=$inst")
  Exec nmake ($nm + @('TCLSH_NATIVE=tclsh', 'core', 'shell', 'dlls')) $win
  $tclsh = Get-ChildItem -Recurse $win -Filter 'tclsh86*.exe' | Select-Object -First 1
  if (-not $tclsh) { throw "Tcl built no tclsh under $win" }
  Exec nmake ($nm + @("TCLSH_NATIVE=$($tclsh.FullName)", 'install-binaries', 'install-libraries')) $win
  $lib = Get-ChildItem (Join-Path $inst 'lib') -Filter 'tcl86*s*.lib' | Where-Object { $_.Name -notmatch 'stub' } |
         Select-Object -First 1
  if (-not $lib) { throw "Tcl installed no static library under $inst\lib" }
  Assert-LibMachine $lib.FullName
  Copy-Item -Force $lib.FullName (Join-Path $OutLib 'tcl86.lib')   # one name for the vcxproj
  $inc = Join-Path $OutInc 'tcl'
  if (Test-Path $inc) { Remove-Item -Recurse -Force $inc }
  New-Dir $inc | Out-Null
  Copy-Item (Join-Path $inst 'include\*.h') $inc
  $rt = Join-Path $OutBin 'tcl8.6'
  if (Test-Path $rt) { Remove-Item -Recurse -Force $rt }
  Copy-Item -Recurse (Join-Path $inst 'lib\tcl8.6') $rt
  if (-not (Test-Path (Join-Path $rt 'init.tcl'))) { throw "no init.tcl in the installed Tcl library ($rt)" }
  Set-Content -Path $stamp -Value $TclVersion
  $Versions['Tcl'] = "$TclVersion ($($lib.Name), sha256 $($tgz.Sha256))"
}

# ----------------------------------------------------------------- QuickJS ---

# QuickJS-ng for the JavaScript engine on Windows (src/platform/qjs_engine.cpp;
# macOS and Linux use JavaScriptCore, which Windows does not have).  Four C
# files, compiled here with the flags its own CMakeLists.txt uses for MSVC, into
# one static /MT library.
function Build-QuickJs {
  $stamp = Join-Path $Out '.quickjs-version'
  if (-not $Force -and (Test-Path $stamp) -and ((Get-Content $stamp) -eq $QuickJsVersion)) {
    Note "QuickJS-ng $QuickJsVersion ($Platform) is up to date"
    $Versions['QuickJS'] = "$QuickJsVersion (already built)"
    return
  }
  $tgz = Get-Download -Url $QuickJsUrl -Name "quickjs-ng-$QuickJsVersion.tar.gz" -Sha256 $QuickJsSha256
  $src = Join-Path $Work 'quickjs'
  Expand-TarGz $tgz.Path $src
  $obj = Join-Path $Work 'quickjs-obj'
  if (Test-Path $obj) { Remove-Item -Recurse -Force $obj }
  New-Dir $obj | Out-Null
  Note "building QuickJS-ng $QuickJsVersion ($Platform)"
  # /J is its -funsigned-char; /experimental:c11atomics its MSVC atomics.  /Zl:
  # no default-library record, so the Debug app links it against the debug CRT
  # (as the OpenSSL libraries do) instead of LIBCMT and LIBCMTD both.
  $files = 'dtoa', 'libregexp', 'libunicode', 'quickjs' | ForEach-Object { Join-Path $src "$_.c" }
  Exec cl (@('/nologo', '/c', '/O2', '/MT', '/Zl', '/W1', '/Zi', '/Fdquickjs.pdb', '/std:c11', '/experimental:c11atomics', '/J',
             '/D_GNU_SOURCE', '/DWIN32_LEAN_AND_MEAN', '/D_WIN32_WINNT=0x0601', '/D_CRT_SECURE_NO_WARNINGS') + $files) $obj
  Exec lib (@('/nologo', "/OUT:$OutLib\quickjs.lib") + (Get-ChildItem (Join-Path $obj '*.obj') | ForEach-Object { $_.FullName }))
  Copy-Item -Force (Join-Path $obj 'quickjs.pdb') $OutLib
  Assert-LibMachine (Join-Path $OutLib 'quickjs.lib')
  $inc = New-Dir (Join-Path $OutInc 'quickjs')
  Copy-Item -Force (Join-Path $src 'quickjs.h') $inc
  Set-Content -Path $stamp -Value $QuickJsVersion
  $Versions['QuickJS'] = "QuickJS-ng $QuickJsVersion (sha256 $($tgz.Sha256))"
}

# -------------------------------------------------------------------- Ruby ---

# Ruby for the embedded engine (src/platform/ruby_engine.cpp): built from the
# release tarball as a native MSVC ("mswin") Ruby, the way packaging/
# fetch-ruby.sh builds it on macOS and Linux -- one shared library plus its
# stdlib, with psych (libyaml), zlib and openssl compiled in.
#
# Layout, so that the DLL finds its own library with no environment:
#
#   bin\<arch>-vcruntime140-ruby400.dll   beside MUSHclient.exe
#   bin\lib\ruby\4.0.0\...                stdlib + C extensions
#
# Ruby on Windows computes its load path from the directory its DLL is in
# (ruby.c ruby_init_loadpath: dirname of the DLL, less a trailing \bin, plus
# \lib\ruby\...), which is exactly the --enable-load-relative layout the other
# platforms ask for.
#
# CRT: Ruby is /MD (it needs vcruntime140.dll; build-app.ps1 ships it).  Its
# zlib and libyaml are built /MD to match.  Its OpenSSL is the static
# libcrypto/libssl above, which are /MT /Zl: /Zl leaves the CRT choice to the
# final link, so the same libraries link into the /MT exe and the /MD Ruby
# extension alike.
function Build-RubyDeps([string]$Into) {
  $inc = New-Dir (Join-Path $Into 'include')
  $lib = New-Dir (Join-Path $Into 'lib')

  # zlib: the repository's own copy (zlib\), the one the app links.
  $zobj = New-Dir (Join-Path $Work 'ruby-zlib-obj')
  $zsrc = Join-Path $Repo 'zlib'
  $zfiles = 'adler32','compress','crc32','deflate','gzclose','gzlib','gzread','gzwrite','infback',
            'inffast','inflate','inftrees','trees','uncompr','zutil' |
            ForEach-Object { Join-Path $zsrc "$_.c" } | Where-Object { Test-Path $_ }
  Exec cl (@('/nologo', '/c', '/O2', '/MD', '/W1', '/D_CRT_SECURE_NO_DEPRECATE') + $zfiles) $zobj
  Exec lib (@('/nologo', "/OUT:$lib\zlib.lib") + (Get-ChildItem (Join-Path $zobj '*.obj') | ForEach-Object { $_.FullName }))
  Copy-Item (Join-Path $zsrc 'zlib.h'), (Join-Path $zsrc 'zconf.h') $inc

  # libyaml, static (ext/psych/extconf.rb defines YAML_DECLARE_STATIC on mswin).
  $tgz = Get-Download -Url $YamlUrl -Name "yaml-$YamlVersion.tar.gz" -Sha256 $YamlSha256
  $ysrc = Join-Path $Work 'yaml'
  Expand-TarGz $tgz.Path $ysrc
  $yobj = New-Dir (Join-Path $Work 'yaml-obj')
  $parts = $YamlVersion.Split('.')
  Exec cl (@('/nologo', '/c', '/O2', '/MD', '/W1', '/DYAML_DECLARE_STATIC', '/D_CRT_SECURE_NO_DEPRECATE',
             "/DYAML_VERSION_MAJOR=$($parts[0])", "/DYAML_VERSION_MINOR=$($parts[1])",
             "/DYAML_VERSION_PATCH=$($parts[2])", "/DYAML_VERSION_STRING=\`"$YamlVersion\`"",
             "/I$ysrc\include") +
           (Get-ChildItem (Join-Path $ysrc 'src\*.c') | ForEach-Object { $_.FullName })) $yobj
  Exec lib (@('/nologo', "/OUT:$lib\yaml.lib") + (Get-ChildItem (Join-Path $yobj '*.obj') | ForEach-Object { $_.FullName }))
  Copy-Item (Join-Path $ysrc 'include\yaml.h') $inc

  # OpenSSL: the static libraries built above.
  if (-not (Test-Path (Join-Path $OutLib 'libcrypto.lib'))) { throw 'Ruby links OpenSSL: build openssl first (-Only openssl)' }
  # libcrypto gets advapi32 folded in (lib.exe merges an import library like
  # any other): a static OpenSSL 3 needs it (CryptoAPI entropy, the event
  # log), and ext/openssl's link line names only crypt32, ws2_32 and user32.
  Copy-Item -Recurse -Force (Join-Path $OutInc 'openssl') $inc
  Exec lib @('/nologo', "/OUT:$lib\libcrypto.lib", (Join-Path $OutLib 'libcrypto.lib'), 'advapi32.lib')
  Copy-Item (Join-Path $OutLib 'libssl.lib') $lib
}

function Build-Ruby {
  $stamp = Join-Path $Out '.ruby-version'
  if (-not $Force -and (Test-Path $stamp) -and ((Get-Content $stamp) -eq $RubyVersion)) {
    Note "Ruby $RubyVersion ($Platform) is up to date"
    $Versions['Ruby'] = "$RubyVersion (already built)"
    return
  }
  $tgz = Get-Download -Url $RubyUrl -Name "ruby-$RubyVersion.tar.gz" -Sha256 $RubySha256
  $src = Join-Path $Work 'ruby'
  Expand-TarGz $tgz.Path $src
  $rdeps = Join-Path $Work 'ruby-deps'
  if (Test-Path $rdeps) { Remove-Item -Recurse -Force $rdeps }
  Build-RubyDeps $rdeps

  $bld = Join-Path $Work 'ruby-build'
  $inst = Join-Path $Work 'ruby-install'
  foreach ($d in $bld, $inst) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }
  New-Dir $bld | Out-Null

  # No host Ruby: the release tarball carries nearly every generated file, and
  # win32\Makefile.sub falls back to tool\missing-baseruby.bat without one.
  # The exception is ext/ripper, whose ripper.c is generated from parse.y by
  # a Ruby script; ripper is the parser-as-a-library behind irb and rdoc, which
  # a MUD client's embedded interpreter has no use for, so it is left out
  # rather than paying for a two-stage bootstrap.
  # --with-opt-dir in forward slashes: mkmf hands it to each extension's
  # checks through a Ruby string, and C:\Users\... arrived there as
  # -IC:Users..., so zlib.h and yaml.h were "not found" and the two extensions
  # quietly skipped.
  Note "configuring Ruby $RubyVersion ($Platform)"
  Exec cmd.exe @('/c', (Join-Path $src 'win32\configure.bat'), "--prefix=$inst", "--with-opt-dir=$($rdeps.Replace('\', '/'))",
                 '--disable-install-doc', '--without-baseruby', '--without-ext=ripper') $bld
  Note "building Ruby $RubyVersion ($Platform) -- nmake is serial, this takes a while"
  Exec nmake @('/nologo') $bld
  Exec nmake @('/nologo', 'install-nodoc') $bld

  $dll = Get-ChildItem (Join-Path $inst 'bin\*ruby*.dll') | Select-Object -First 1
  if (-not $dll) { throw "Ruby installed no DLL under $inst\bin" }
  Assert-Machine $dll.FullName
  $implib = Join-Path $inst ("lib\" + [IO.Path]::GetFileNameWithoutExtension($dll.Name) + '.lib')
  if (-not (Test-Path $implib)) { throw "no import library $implib" }
  foreach ($ext in 'zlib', 'psych', 'openssl') {
    if (-not (Get-ChildItem -Recurse (Join-Path $inst 'lib\ruby') -Filter "$ext.so" -ErrorAction SilentlyContinue)) {
      throw "Ruby was built without its $ext extension (see $bld\ext\$ext\mkmf.log)"
    }
  }

  # Stage: DLL + lib\ruby beside it, headers and the import library for the app.
  Get-ChildItem $OutBin -Filter '*ruby*.dll' -ErrorAction SilentlyContinue | Remove-Item -Force
  $rbLib = Join-Path $OutBin 'lib\ruby'
  if (Test-Path $rbLib) { Remove-Item -Recurse -Force $rbLib }
  New-Dir (Split-Path $rbLib) | Out-Null
  Copy-Item $dll.FullName $OutBin
  Copy-Item -Recurse (Join-Path $inst 'lib\ruby') $rbLib
  # The .gem archives of the bundled gems, which are installed beside them
  # already: only `gem pristine` would read these.
  Get-ChildItem (Join-Path $rbLib 'gems\*\cache') -Directory -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
  # The import library MUSHclient links is NOT Ruby's own.  The DLL also
  # exports some 150 POSIX, CRT and Winsock names as aliases of its rb_w32_*
  # wrappers -- accept, connect, close, fclose, getenv, Sleep, ... -- and
  # linking Ruby's import library would let any of the client's own calls to
  # those bind to Ruby's versions instead of the CRT's or ws2_32's (the link
  # only noticed getenv and strerror, as duplicates).  So: Ruby's generated
  # .def, cut to Ruby's own namespaces (it marks the DATA exports itself),
  # made into the import library.
  $def = Join-Path $bld ([IO.Path]::GetFileNameWithoutExtension($dll.Name) + '.def')
  if (-not (Test-Path $def)) { throw "no export definition file $def" }
  $keep = @("LIBRARY $($dll.Name)")
  foreach ($l in Get-Content $def) {
    if ($l -match '^\s*(VERSION|EXPORTS)\b') { $keep += $l; continue }
    if ($l -match '^\s*([A-Za-z_]\w*)' -and $Matches[1] -match '^(rb_|ruby_|rbimpl_|st_|onig|Onig|ONIG|RUBY_|Init_)') { $keep += $l }
  }
  $apiDef = Join-Path $Work 'ruby-api.def'
  Set-Content -Path $apiDef -Value $keep -Encoding ASCII
  Exec lib @('/nologo', "/DEF:$apiDef", "/MACHINE:$Machine", "/OUT:$(Join-Path $OutLib 'ruby.lib')")
  Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $OutLib 'ruby.exp')
  # Headers: include\ruby-4.0.0 as include\ruby, and its per-architecture
  # subdirectory (x64-mswin64_140\ruby\config.h and friends) as include\ruby-arch,
  # so MUSHclient.engines.props can name both without knowing the triple.
  $rbInc = Join-Path $OutInc 'ruby'
  $rbArch = Join-Path $OutInc 'ruby-arch'
  foreach ($d in $rbInc, $rbArch) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }
  Copy-Item -Recurse (Get-ChildItem (Join-Path $inst 'include') -Directory | Select-Object -First 1).FullName $rbInc
  $archDir = Get-ChildItem $rbInc -Directory | Where-Object { $_.Name -match 'mswin' } | Select-Object -First 1
  if (-not $archDir) { throw "no <arch>-mswin* directory under $rbInc" }
  Move-Item $archDir.FullName $rbArch
  Set-Content -Path (Join-Path $Out 'ruby-dll.txt') -Value $dll.Name
  Set-Content -Path $stamp -Value $RubyVersion
  $Versions['Ruby'] = "$RubyVersion ($($dll.Name), sha256 $($tgz.Sha256)); libyaml $YamlVersion"
}

# -------------------------------------------------------------------- Dina ---

# packaging\windows\deps\common\ (architecture-neutral): Dina.fon + its licence.
$CommonDir = Join-Path $Deps 'common'

function Build-Dina {
  $stamp = Join-Path $CommonDir '.dina-sha256'
  if (-not $Force -and (Test-Path $stamp) -and ((Get-Content $stamp) -eq $DinaSha256)) {
    Note 'Dina is up to date'
    $Versions['Dina'] = '(already fetched)'
    return
  }
  $zip = Get-Download -Url $DinaUrl -Name 'Dina.zip' -Sha256 $DinaSha256
  $x = Join-Path $Work 'dina'
  if (Test-Path $x) { Remove-Item -Recurse -Force $x }
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  [IO.Compression.ZipFile]::ExtractToDirectory($zip.Path, $x)
  New-Dir $CommonDir | Out-Null
  Copy-Item -Force (Join-Path $x 'DinaR.fon') (Join-Path $CommonDir 'Dina.fon')
  Copy-Item -Force (Join-Path $x 'LICENSE') (Join-Path $CommonDir 'Dina_license.txt')
  Set-Content -Path $stamp -Value $DinaSha256
  $Versions['Dina'] = "Dina.zip (dcmembers.com/jibsen, sha256 $DinaSha256)"
}

# ------------------------------------------------------- resource DLLs ---

# A resource DLL (locale\<lang>.dll) is what MUSHclient.cpp's I18N_Setup
# LoadLibrary's and makes the MFC resource handle: every menu, dialog, string
# and accelerator lives there.  A 64-bit process cannot LoadLibrary a 32-bit
# DLL, so a translation made for the 32-bit client -- by copying and editing
# en.dll, as locale_notes.txt describes -- needs a 64-bit twin.  The client's
# own en.dll needs none: it is built from source (resources\en.vcxproj).
#
# Why rehost rather than ask for the translation's source: there usually is
# none, and copying the resources byte for byte guarantees the 64-bit client
# shows exactly what the 32-bit one did.  Resources are architecture-neutral
# data; only the PE wrapper around them is not.
#
# Mechanism: link an empty resource-only DLL for the target machine, open the
# x86 DLL as a data file (works across architectures), and UpdateResource
# every (type, name, language) across.  RT_MANIFEST is the one resource whose
# content names an architecture; any processorArchitecture="x86" in it is
# rewritten to "*".  The copy is then re-read and compared.
$RehostSource = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class MushResRehost
{
    const uint LOAD_LIBRARY_AS_DATAFILE = 0x00000002;
    delegate bool EnumTypesProc(IntPtr hModule, IntPtr type, IntPtr lParam);
    delegate bool EnumNamesProc(IntPtr hModule, IntPtr type, IntPtr name, IntPtr lParam);
    delegate bool EnumLangsProc(IntPtr hModule, IntPtr type, IntPtr name, ushort lang, IntPtr lParam);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr LoadLibraryExW(string file, IntPtr reserved, uint flags);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool FreeLibrary(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool EnumResourceTypesW(IntPtr h, EnumTypesProc cb, IntPtr l);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool EnumResourceNamesW(IntPtr h, IntPtr type, EnumNamesProc cb, IntPtr l);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool EnumResourceLanguagesW(IntPtr h, IntPtr type, IntPtr name, EnumLangsProc cb, IntPtr l);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr FindResourceExW(IntPtr h, IntPtr type, IntPtr name, ushort lang);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr LoadResource(IntPtr h, IntPtr res);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr LockResource(IntPtr g);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint SizeofResource(IntPtr h, IntPtr res);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr BeginUpdateResourceW(string file, bool deleteExisting);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool UpdateResourceW(IntPtr h, IntPtr type, IntPtr name, ushort lang, byte[] data, uint cb);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool EndUpdateResourceW(IntPtr h, bool discard);

    // A resource type or name: an integer id, or a string.
    public sealed class Id
    {
        public readonly int Num; public readonly string Str;
        public Id(IntPtr p)
        {
            long v = p.ToInt64();
            if ((v >> 16) == 0) { Num = (int)v; Str = null; }
            else { Num = -1; Str = Marshal.PtrToStringUni(p); }
        }
        public override string ToString() { return Str ?? ("#" + Num); }
    }
    public sealed class Entry
    {
        public Id Type, Name; public ushort Lang; public byte[] Data;
        public string Key { get { return Type + "/" + Name + "/" + Lang; } }
    }

    // Marshal an Id for a call; strings must stay alive until freed.
    static IntPtr Ptr(Id id, List<IntPtr> toFree)
    {
        if (id.Str == null) return new IntPtr(id.Num);
        IntPtr p = Marshal.StringToHGlobalUni(id.Str);
        toFree.Add(p);
        return p;
    }

    public static List<Entry> Read(string path)
    {
        IntPtr h = LoadLibraryExW(path, IntPtr.Zero, LOAD_LIBRARY_AS_DATAFILE);
        if (h == IntPtr.Zero) throw new Exception("LoadLibraryEx(" + path + ") failed: " + Marshal.GetLastWin32Error());
        var result = new List<Entry>();
        var types = new List<Id>();
        try
        {
            EnumResourceTypesW(h, (m, t, l) => { types.Add(new Id(t)); return true; }, IntPtr.Zero);
            foreach (Id type in types)
            {
                var free = new List<IntPtr>();
                try
                {
                    IntPtr tp = Ptr(type, free);
                    var names = new List<Id>();
                    EnumResourceNamesW(h, tp, (m, t, n, l) => { names.Add(new Id(n)); return true; }, IntPtr.Zero);
                    foreach (Id name in names)
                    {
                        IntPtr np = Ptr(name, free);
                        var langs = new List<ushort>();
                        EnumResourceLanguagesW(h, tp, np, (m, t, n, lg, l) => { langs.Add(lg); return true; }, IntPtr.Zero);
                        foreach (ushort lang in langs)
                        {
                            IntPtr r = FindResourceExW(h, tp, np, lang);
                            if (r == IntPtr.Zero) throw new Exception("FindResourceEx " + type + "/" + name + " failed");
                            uint size = SizeofResource(h, r);
                            IntPtr data = LockResource(LoadResource(h, r));
                            byte[] bytes = new byte[size];
                            if (size > 0) Marshal.Copy(data, bytes, 0, (int)size);
                            result.Add(new Entry { Type = type, Name = name, Lang = lang, Data = bytes });
                        }
                    }
                }
                finally { foreach (IntPtr p in free) Marshal.FreeHGlobal(p); }
            }
        }
        finally { FreeLibrary(h); }
        return result;
    }

    static byte[] FixManifest(byte[] data, List<string> log, string key)
    {
        string text = Encoding.UTF8.GetString(data);
        string fixedText = text.Replace("processorArchitecture=\"x86\"", "processorArchitecture=\"*\"")
                               .Replace("processorArchitecture='x86'", "processorArchitecture='*'")
                               .Replace("processorArchitecture=\"X86\"", "processorArchitecture=\"*\"");
        if (fixedText == text) return data;
        log.Add("rewrote processorArchitecture in manifest " + key);
        return Encoding.UTF8.GetBytes(fixedText);
    }

    // Replace every resource in `target` with those of `source`.  Returns a log.
    public static string[] Copy(string source, string target)
    {
        var log = new List<string>();
        List<Entry> entries = Read(source);
        IntPtr u = BeginUpdateResourceW(target, true);       // true: drop the placeholder
        if (u == IntPtr.Zero) throw new Exception("BeginUpdateResource(" + target + ") failed: " + Marshal.GetLastWin32Error());
        bool ok = false;
        try
        {
            foreach (Entry e in entries)
            {
                byte[] data = e.Data;
                if (e.Type.Str == null && e.Type.Num == 24) data = FixManifest(data, log, e.Key);  // RT_MANIFEST
                var free = new List<IntPtr>();
                try
                {
                    if (!UpdateResourceW(u, Ptr(e.Type, free), Ptr(e.Name, free), e.Lang, data, (uint)data.Length))
                        throw new Exception("UpdateResource " + e.Key + " failed: " + Marshal.GetLastWin32Error());
                }
                finally { foreach (IntPtr p in free) Marshal.FreeHGlobal(p); }
            }
            ok = true;
        }
        finally
        {
            if (!EndUpdateResourceW(u, !ok) && ok)
                throw new Exception("EndUpdateResource(" + target + ") failed: " + Marshal.GetLastWin32Error());
        }
        log.Add("copied " + entries.Count + " resources");
        return log.ToArray();
    }

    // Every (type, name, language) of source is in target with the same bytes
    // (manifests excepted, which Copy may have rewritten); nothing extra.
    public static string Verify(string source, string target)
    {
        var a = Read(source); var b = Read(target);
        var map = new Dictionary<string, byte[]>();
        foreach (Entry e in b) map[e.Key] = e.Data;
        if (a.Count != b.Count) return "resource count differs: " + a.Count + " vs " + b.Count;
        foreach (Entry e in a)
        {
            byte[] d;
            if (!map.TryGetValue(e.Key, out d)) return "missing " + e.Key;
            if (e.Type.Str == null && e.Type.Num == 24) continue;
            if (d.Length != e.Data.Length) return "size differs for " + e.Key;
            for (int i = 0; i < d.Length; i++) if (d[i] != e.Data[i]) return "bytes differ for " + e.Key;
        }
        return null;
    }
}
'@

function Find-LocaleSource {
  if (-not $LocaleSource) {
    throw ('-Only locale rehosts the 32-bit resource DLLs in -LocaleSource <dir> -- a translation made ' +
           'for the 32-bit client.  The client''s own locale\en.dll is built from resources\ by the solution.')
  }
  if (-not (Get-ChildItem $LocaleSource -Filter '*.dll' -ErrorAction SilentlyContinue)) { throw "no DLLs in $LocaleSource" }
  $LocaleSource
}

function Copy-ResourceDll([string]$Source, [string]$Target) {
  if (-not ('MushResRehost' -as [type])) { Add-Type -TypeDefinition $RehostSource -Language CSharp }
  $tmp = New-Dir (Join-Path $Work 'locale-tmp')
  $rc = Join-Path $tmp 'placeholder.rc'
  # One throwaway resource so link has something to put in .rsrc;
  # BeginUpdateResource(..., true) deletes it again.
  Set-Content -Path $rc -Value 'MUSHCLIENT_REHOST_PLACEHOLDER RCDATA { "x" }' -Encoding ASCII
  Exec rc @('/nologo', "/fo$tmp\placeholder.res", $rc)
  # /NOENTRY: a resource-only DLL, no code.  The x86 one carries a do-nothing
  # LibMain; MUSHclient never calls into it, only LoadLibrary + resources.
  Exec link @('/nologo', '/DLL', '/NOENTRY', "/MACHINE:$Machine", "/OUT:$Target", "$tmp\placeholder.res")
  foreach ($line in [MushResRehost]::Copy($Source, $Target)) { Write-Host "    $line" }
  $diff = [MushResRehost]::Verify($Source, $Target)
  if ($diff) { throw "rehosted $Target does not match ${Source}: $diff" }
  Assert-Machine $Target
}

# Into locale-rehosted\, never bin\locale\: the client's own en.dll is built
# from source (resources\en.vcxproj), and a rehosted DLL there would shadow it
# in every -StageTo copy and package.  Copy what you need into an install's
# locale\ yourself.
function Build-Locale {
  $src = Find-LocaleSource
  $dir = New-Dir (Join-Path $Out 'locale-rehosted')
  $hashes = @()
  foreach ($s in Get-ChildItem $src -Filter '*.dll') {
    if ((Get-PeMachine $s.FullName) -ne 0x14C) { Warn "$($s.FullName) is not an x86 DLL; rehosting it anyway" }
    Note "rehosting $($s.Name) from $src"
    Copy-ResourceDll $s.FullName (Join-Path $dir $s.Name)
    $hashes += "$($s.Name) sha256 $((Get-FileHash -Algorithm SHA256 $s.FullName).Hash.ToLowerInvariant())"
  }
  $Versions['locale'] = "rehosted from $src into $dir ($($hashes -join ', '))"
}

# -------------------------------------------------------------------- main ---

Note "MUSHclient 64-bit dependencies: $Platform -> $Out"
# Ruby's and Tcl's makefiles run tools they have just built, or generated, by
# bare name from the current directory ("cmd /c verconf.bat", nmakehlp.exe).
# NoDefaultCurrentDirectoryInExePath -- which some shells and agents set --
# turns every one of those into "not recognized".  This process only.
$env:NoDefaultCurrentDirectoryInExePath = $null
New-Dir $Work | Out-Null
New-Dir $Out | Out-Null
Import-VsEnvironment

if ($Components -contains 'openssl')   { Build-OpenSsl }
if ($Components -contains 'lua')       { Build-Lua }
if ($Components -contains 'luasocket') { Build-LuaSocket }
if ($Components -contains 'dina')      { Build-Dina }
if ($Components -contains 'locale')    { Build-Locale }
if ($Components -contains 'python')    { Build-Python }
if ($Components -contains 'tcl')       { Build-Tcl }
if ($Components -contains 'quickjs')   { Build-QuickJs }
if ($Components -contains 'ruby')      { Build-Ruby }

$report = @("MUSHclient $Platform dependencies, built $(Get-Date -Format s) by packaging\windows\fetch-deps.ps1",
            "compiler: MSVC $env:VCToolsVersion, Windows SDK $env:WindowsSDKVersion")
foreach ($k in $Versions.Keys) { $report += ('{0,-10} {1}' -f $k, $Versions[$k]) }
$versionsFile = Join-Path $Out 'VERSIONS.txt'
# Append, so a partial (-Only) run keeps what earlier runs recorded.
Add-Content -Path $versionsFile -Value ($report + '')
$report | ForEach-Object { Write-Host "    $_" }

if ($StageTo) {
  $dest = if ([System.IO.Path]::IsPathRooted($StageTo)) { $StageTo } else { Join-Path $Repo $StageTo }
  New-Dir $dest | Out-Null
  Copy-Item -Recurse -Force (Join-Path $OutBin '*') $dest
  # The VC++ runtime python3XX.dll and the Ruby DLL import (a package ships
  # the redistributable's own copies; build-app.ps1).
  $crt = Join-Path $Out 'crt-python'
  if (Test-Path $crt) { Copy-Item -Force (Join-Path $crt '*.dll') $dest }
  Note "runtime DLLs copied to $dest"
}
Note 'done'
