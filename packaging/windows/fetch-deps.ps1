<#
.SYNOPSIS
  Build the third-party pieces a 64-bit (x64 or ARM64) MUSHclient links and ships.

.DESCRIPTION
  The Win32 build uses prebuilts committed to the repository -- openssl\lib\
  libcrypto.lib + libssl.lib (static OpenSSL 3.5, VC-WIN32, /MT) and
  src\lua5.1.lib (import library for lua5.1.dll) -- plus DLLs that are not
  committed at all (lua5.1.dll, locale\en.dll, locale\en_small.dll,
  socket\core.dll, mime\core.dll) and come from an existing installation.
  Every one of those is 32-bit x86 and cannot be linked into, or loaded by, a
  64-bit process.  This script produces the equivalent set for one 64-bit
  architecture, from source wherever source exists:

    OpenSSL 3.5.x  static libcrypto.lib / libssl.lib + headers, built exactly
                   the way the committed x86 libs were (their embedded
                   "compiler:" string: VC-WIN32, no-shared, /MT /Zl).
                   Default: the newest 3.5.x release on GitHub, verified
                   against the .sha256 published with it.
    Lua 5.1.5      lua5.1.dll + lua5.1.lib, from lua.org's tarball (SHA-256
                   pinned below), exporting every symbol the shipped x86
                   lua5.1.dll exports (checked after the build).
    LuaSocket 2.0.2  socket\core.dll + mime\core.dll -- the version the
                   committed lua\socket.lua etc. are written for.
    en.dll, en_small.dll  the localised resource DLLs.  These are resource-
                   only, so their resources are copied, byte for byte, out of
                   the shipped x86 DLLs into a fresh DLL for the target
                   architecture (see Copy-ResourceDll for why, and why not
                   from source).

  Output, git-ignored, laid out for MUSHclient.vcxproj's 64-bit configurations:

    packaging\windows\deps\<Platform>\include\openssl\*.h
    packaging\windows\deps\<Platform>\lib\libcrypto.lib, libssl.lib, lua5.1.lib
    packaging\windows\deps\<Platform>\bin\lua5.1.dll
    packaging\windows\deps\<Platform>\bin\socket\core.dll, mime\core.dll
    packaging\windows\deps\<Platform>\bin\locale\en.dll, en_small.dll
    packaging\windows\deps\<Platform>\VERSIONS.txt     what was built, from what

  <Platform> is the MSBuild platform name: x64 or ARM64.

  ENVIRONMENT.  Run from any PowerShell (Windows PowerShell 5.1 or pwsh 7) on a
  machine with Visual Studio 2022 and its "Desktop development with C++"
  workload, including the x64/ARM64 build tools you are targeting.  The script
  finds VS with vswhere and imports vcvarsall.bat's environment itself, so it
  does NOT need a Developer PowerShell (it works from one too).  Also needed:

    - Perl for Windows (Strawberry Perl; preinstalled on GitHub's Windows
      runners).  An MSYS/Git-for-Windows perl will not do: OpenSSL needs one
      that reports $^O = MSWin32.
    - x64 only, unless -NoAsm: NASM 2.09+ on PATH (winget install NASM.NASM).
    - ARM64: OpenSSL's assembler for Windows-on-ARM is clang-cl (VS component
      "C++ Clang Compiler for Windows").  Without it the build falls back to
      the VC-WIN64-ARM target, which has no assembler at all.
    - tar.exe from Windows 10 1803+ (System32), for the .tar.gz sources.
    - The x86 en.dll / en_small.dll, for -Only locale (see -LocaleSource).

.PARAMETER Arch
  x64 or arm64 (case-insensitive).

.PARAMETER OpenSslVersion
  Exact OpenSSL release, e.g. 3.5.9.  Default: newest final 3.5.x on GitHub.

.PARAMETER NoAsm
  Build OpenSSL without assembler (no NASM / clang-cl needed; slower crypto).

.PARAMETER LocaleSource
  Directory holding the 32-bit en.dll and en_small.dll to rehost.  Default:
  the first of WinRel\locale, WinDebug\locale, "${env:ProgramFiles(x86)}\
  MUSHclient\locale" that has both.

.PARAMETER Only
  Build only these components: openssl, lua, luasocket, locale.  luasocket
  needs lua built first (it links lua5.1.lib).

.PARAMETER StageTo
  After building, copy bin\ (the runtime DLLs, in their runtime layout) into
  this directory -- e.g. WinDebug-x64 -- so the built exe can run there.

.PARAMETER Force
  Rebuild components that are already built.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File packaging\windows\fetch-deps.ps1 -Arch x64
.EXAMPLE
  pwsh packaging\windows\fetch-deps.ps1 -Arch arm64 -StageTo WinDebug-ARM64
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [ValidateSet('x64', 'arm64')] [string]$Arch,      # case-insensitive: ARM64 works too
  [string]$OpenSslVersion = '',
  [string]$OpenSslSeries = '3.5',
  [switch]$NoAsm,
  [string]$LocaleSource = '',
  [ValidateSet('openssl', 'lua', 'luasocket', 'locale')] [string[]]$Only = @(),
  [string]$StageTo = '',
  [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'     # Invoke-WebRequest is 10x slower with it
Set-StrictMode -Version 2

# Windows PowerShell 5.1 defaults to TLS 1.0/1.1, which GitHub and lua.org refuse.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

# ---------------------------------------------------------------- constants ---

$Platform = if ($Arch -eq 'x64') { 'x64' } else { 'ARM64' }     # MSBuild's spelling
$Machine  = if ($Arch -eq 'x64') { 'X64' } else { 'ARM64' }      # link /MACHINE:
$PeMachine = if ($Arch -eq 'x64') { 0x8664 } else { 0xAA64 }

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
$SocketTag    = 'v2.0.2'
$SocketUrl    = "https://github.com/lunarmodules/luasocket/archive/refs/tags/$SocketTag.tar.gz"
# Not pinned yet: nobody had downloaded it from a machine able to record the
# hash when this was written.  VERSIONS.txt records what was used; paste that
# hash here to pin it.
$SocketSha256 = ''

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

$Components = if ($Only.Count) { $Only } else { @('openssl', 'lua', 'luasocket', 'locale') }
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
  $hostArch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
  $vcArg = switch ($Platform) {
    'x64'   { if ($hostArch -eq 'Arm64') { 'arm64_x64' } else { 'x64' } }
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
}

# ---------------------------------------------------------------- helpers ---

function Get-Download {
  param([string]$Url, [string]$Name, [string]$Sha256 = '')
  New-Dir $Dl | Out-Null
  $dest = Join-Path $Dl $Name
  if (-not (Test-Path $dest)) {
    Note "downloading $Url"
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile "$dest.part"
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
  $want = if ($Platform -eq 'x64') { 'x64' } else { 'ARM64' }
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
  if ($Platform -eq 'x64') {
    $target = 'VC-WIN64A'
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
  $obj = New-Dir (Join-Path $Work 'lua-obj')
  New-Dir $OutBin | Out-Null; New-Dir $OutLib | Out-Null

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
  $luaLib = Join-Path $OutLib 'lua5.1.lib'
  if (-not (Test-Path $luaLib)) { throw 'LuaSocket links lua5.1.lib: build lua first (-Only lua)' }
  $luaInc = Join-Path $Work 'lua\src'
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
                 'udp', 'except', 'select', 'wsocket' | ForEach-Object { Join-Path $s "$_.c" }
  Exec cl ($common + @('/DLUASOCKET_API=__declspec(dllexport)') + $socketFiles) $objS
  # wsocket.h includes <winsock.h>: the x86 core.dll imported WSOCK32.dll.
  Exec link (@('/nologo', '/DLL', "/MACHINE:$Machine", "/OUT:$sock", "/IMPLIB:$objS\core.lib") +
             (Get-ChildItem (Join-Path $objS '*.obj') | ForEach-Object { $_.FullName }) +
             @($luaLib, 'ws2_32.lib', 'wsock32.lib'))

  Note "building mime\core.dll"
  $objM = New-Dir (Join-Path $Work 'mime-obj')
  Exec cl ($common + @('/DMIME_API=__declspec(dllexport)', (Join-Path $s 'mime.c'))) $objM
  Exec link @('/nologo', '/DLL', "/MACHINE:$Machine", "/OUT:$mime", "/IMPLIB:$objM\core.lib",
              (Join-Path $objM 'mime.obj'), $luaLib)

  foreach ($d in $sock, $mime) { Assert-Machine $d }
  $Versions['LuaSocket'] = "$SocketTag (github.com/lunarmodules/luasocket, sha256 $($tgz.Sha256))"
}

# ------------------------------------------------------- resource DLLs ---

# en.dll and en_small.dll are resource-only DLLs (MUSHclient.cpp LoadLibrary's
# one and makes it the MFC resource handle: every menu, dialog, string and
# accelerator lives there).  A 64-bit process cannot LoadLibrary a 32-bit DLL,
# so each needs a 64-bit twin.
#
# Why rehost instead of compile: en_small.dll has no source anywhere (it dates
# from 2007; github.com/nickgammon/mushclient_resources builds en.dll only),
# and copying the resources byte for byte is the one method that guarantees
# the 64-bit client shows exactly what the 32-bit one does.  Resources are
# architecture-neutral data; only the PE wrapper around them is not.
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
  if ($LocaleSource) { return $LocaleSource }
  $candidates = @((Join-Path $Repo 'WinRel\locale'), (Join-Path $Repo 'WinDebug\locale'))
  if (${env:ProgramFiles(x86)}) { $candidates += Join-Path ${env:ProgramFiles(x86)} 'MUSHclient\locale' }
  foreach ($c in $candidates) {
    if ((Test-Path (Join-Path $c 'en.dll')) -and (Test-Path (Join-Path $c 'en_small.dll'))) { return $c }
  }
  throw ("no 32-bit en.dll + en_small.dll found (looked in: $($candidates -join '; ')); " +
         'pass -LocaleSource <dir>, e.g. an installed MUSHclient 5.0x\locale')
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

function Build-Locale {
  $dir = New-Dir (Join-Path $OutBin 'locale')
  $done = (Test-Path (Join-Path $dir 'en.dll')) -and (Test-Path (Join-Path $dir 'en_small.dll'))
  if (-not $Force -and $done) {
    Note "locale DLLs ($Platform) are up to date"
    $Versions['locale'] = '(already built)'
    return
  }
  $src = Find-LocaleSource
  $hashes = @()
  foreach ($n in 'en.dll', 'en_small.dll') {
    $s = Join-Path $src $n
    if ((Get-PeMachine $s) -ne 0x14C) { Warn "$s is not an x86 DLL; rehosting it anyway" }
    Note "rehosting $n from $src"
    Copy-ResourceDll $s (Join-Path $dir $n)
    $hashes += "$n sha256 $((Get-FileHash -Algorithm SHA256 $s).Hash.ToLowerInvariant())"
  }
  $Versions['locale'] = "rehosted from $src ($($hashes -join ', '))"
}

# -------------------------------------------------------------------- main ---

Note "MUSHclient 64-bit dependencies: $Platform -> $Out"
New-Dir $Work | Out-Null
New-Dir $Out | Out-Null
Import-VsEnvironment

if ($Components -contains 'openssl')   { Build-OpenSsl }
if ($Components -contains 'lua')       { Build-Lua }
if ($Components -contains 'luasocket') { Build-LuaSocket }
if ($Components -contains 'locale')    { Build-Locale }

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
  Note "runtime DLLs copied to $dest"
}
Note 'done'
