<#
.SYNOPSIS
  Build a complete, self-contained MUSHclient package for 64-bit Windows: an
  installer and a portable zip, for x64 or ARM64, optionally the Discworld
  edition.  The Windows counterpart of packaging/build-app.sh.

.DESCRIPTION
  1. Dependencies: packaging\windows\fetch-deps.ps1 for the architecture
     (idempotent -- what is already built is skipped).
  2. The client: MUSHclient.vcxproj, Release|<Platform>.
  3. The installer: install\mushclient.nsi, built with makensis, which is the
     ONE list of what a Windows package contains.
  4. The portable zip: that installer's own payload, extracted with 7-Zip --
     so the zip and the installer cannot disagree.  Unzip anywhere and run
     MUSHclient.exe; nothing is written outside that directory except the
     preferences every MUSHclient keeps in HKCU.
  5. A self-containment check on the extracted tree: every executable and DLL
     is the target architecture, and every DLL any of them imports is either
     in the package or part of Windows itself (the VC++ runtime does not
     count as part of Windows: it is shipped).
  6. Every scripting language's conformance suite, run from that tree.

  Output, in -OutDir (default dist\):
    MUSHclient-<version>-windows-<arch>[-discworld]-setup.exe
    MUSHclient-<version>-windows-<arch>[-discworld].zip
    SHA256SUMS-windows-<arch>[-discworld].txt

  Needs what fetch-deps.ps1 needs, plus NSIS 3.04+ and 7-Zip; when either is
  not installed, its official portable form is fetched (SHA-256 pinned) into
  packaging\windows\deps\_tools\.

.PARAMETER Arch
  x64 or arm64.  Default: this machine's.

.PARAMETER WithDiscworld
  The Discworld edition: Quow's plugins and maps from -DiscworldDir (default
  packaging\discworld-plugins, staged by packaging/fetch-discworld-plugins.sh).

.PARAMETER Version
  The version in the file names.  Default: VERSION_STRING in src\version.h.

.PARAMETER NoBuild
  Package the existing WinRel-<Platform>\MUSHclient.exe.

.PARAMETER SkipDeps
  Do not run fetch-deps.ps1 (its output must already be in place).

.PARAMETER NoSelftest
  Skip the last step: every scripting language's conformance suite
  (--engines-selftest), run from the extracted package.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File packaging\windows\build-app.ps1 -Arch x64
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File packaging\windows\build-app.ps1 -Arch arm64 -WithDiscworld
#>
[CmdletBinding()]
param(
  [ValidateSet('x64', 'arm64')] [string]$Arch = '',
  [switch]$WithDiscworld,
  [string]$DiscworldDir = '',
  [string]$Version = '',
  [switch]$NoBuild,
  [switch]$SkipDeps,
  [switch]$NoSelftest,
  [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version 2
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Deps = Join-Path $PSScriptRoot 'deps'

# The MACHINE's architecture, not this process's (an x64 PowerShell under
# emulation on ARM64 is told X64 by .NET); the system environment says.
$sysArch = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment' -ErrorAction SilentlyContinue).PROCESSOR_ARCHITECTURE
$hostArm = ($sysArch -eq 'ARM64')
if (-not $Arch) { $Arch = if ($hostArm) { 'arm64' } else { 'x64' } }
$Platform  = if ($Arch -eq 'x64') { 'x64' } else { 'ARM64' }
$PeMachine = if ($Arch -eq 'x64') { 0x8664 } else { 0xAA64 }
$Edition   = if ($WithDiscworld) { '-discworld' } else { '' }
if (-not $OutDir) { $OutDir = Join-Path $Repo 'dist' }
if (-not [IO.Path]::IsPathRooted($OutDir)) { $OutDir = Join-Path $Repo $OutDir }

if (-not $Version) {
  $m = Select-String -Path (Join-Path $Repo 'src\version.h') -Pattern '^\s*#define\s+VERSION_STRING\s+"([^"]+)"' | Select-Object -First 1
  if (-not $m) { throw 'no VERSION_STRING in src\version.h' }
  $Version = $m.Matches[0].Groups[1].Value
}
$Base = "MUSHclient-$Version-windows-$Arch$Edition"

# The pinned portable tools, fetched only when nothing is installed.
$NsisUrl    = 'https://prdownloads.sourceforge.net/nsis/nsis-3.10.zip'
$NsisSha256 = 'fcdce3229717a2a148e7cda0ab5bdb667f39d8fb33ede1da8dabc336bd5ad110'

function Note([string]$m) { Write-Host "==> $m" -ForegroundColor Cyan }

# Run a native tool; throw on a non-zero exit (PowerShell does not).  Its
# stderr is not an error by itself: Windows PowerShell 5.1 would otherwise
# turn the first line a compiler prints there into a terminating one.
function Exec {
  param([string]$Exe, [string[]]$Arguments, [string]$Dir = $null)
  if ($Dir) { Push-Location $Dir }
  $eap = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try { & $Exe @Arguments; $rc = $LASTEXITCODE }
  finally { $ErrorActionPreference = $eap; if ($Dir) { Pop-Location } }
  if ($rc -ne 0) { throw "$Exe failed (exit $rc): $($Arguments -join ' ')" }
}

function New-Dir([string]$p) { New-Item -ItemType Directory -Force -Path $p | Out-Null; $p }

function Get-PeMachine([string]$Path) {
  $fs = [System.IO.File]::OpenRead($Path)
  try {
    $br = New-Object System.IO.BinaryReader($fs)
    if ($br.ReadUInt16() -ne 0x5A4D) { return -1 }            # not MZ
    $fs.Position = 0x3C
    $fs.Position = $br.ReadInt32() + 4
    [int]$br.ReadUInt16()
  } finally { $fs.Dispose() }
}

# ── Visual Studio ─────────────────────────────────────────────────────────────
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found: install Visual Studio 2022 with the C++ workload' }
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vs) { throw 'no Visual Studio with MSBuild was found' }
$msbuild = Join-Path $vs ('MSBuild\Current\Bin\' + $(if ($hostArm) { 'arm64\' } else { 'amd64\' }) + 'MSBuild.exe')
if (-not (Test-Path $msbuild)) { $msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe' }

# The VC++ runtime the embedded Python and Ruby import, from VS's own
# redistributable directory: the newest MSVC version that has one.
$redistRoot = Join-Path $vs 'VC\Redist\MSVC'
$crtDir = Get-ChildItem $redistRoot -Directory -ErrorAction SilentlyContinue |
          Where-Object { $_.Name -match '^\d' } | Sort-Object { [version]$_.Name } -Descending |
          ForEach-Object { Join-Path $_.FullName "$($Arch.ToLower())\Microsoft.VC143.CRT" } |
          Where-Object { Test-Path (Join-Path $_ 'vcruntime140.dll') } | Select-Object -First 1
if (-not $crtDir) { throw "no VC++ redistributable for $Arch under $redistRoot (the C++ build tools for $Platform include it)" }

# ── NSIS and 7-Zip ────────────────────────────────────────────────────────────
function Find-MakeNsis {
  $c = Get-Command makensis -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  foreach ($p in ${env:ProgramFiles(x86)}, $env:ProgramFiles) {
    if ($p -and (Test-Path (Join-Path $p 'NSIS\makensis.exe'))) { return Join-Path $p 'NSIS\makensis.exe' }
  }
  $local = Join-Path $Deps '_tools\nsis-3.10\makensis.exe'
  if (Test-Path $local) { return $local }
  $dl = New-Dir (Join-Path $Deps '_downloads')
  $zip = Join-Path $dl 'nsis-3.10.zip'
  if (-not (Test-Path $zip)) {
    Note "downloading $NsisUrl"
    Invoke-WebRequest -UseBasicParsing -UserAgent 'curl/8.0' -Uri $NsisUrl -OutFile "$zip.part"
    Move-Item -Force "$zip.part" $zip
  }
  $got = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
  if ($got -ne $NsisSha256) { Remove-Item -Force $zip; throw "checksum mismatch for nsis-3.10.zip (got $got); deleted it, re-run to download again" }
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  [IO.Compression.ZipFile]::ExtractToDirectory($zip, (New-Dir (Join-Path $Deps '_tools')))
  $local
}

function Find-SevenZip {
  $c = Get-Command 7z -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  foreach ($p in $env:ProgramW6432, $env:ProgramFiles, ${env:ProgramFiles(x86)}) {
    if ($p -and (Test-Path (Join-Path $p '7-Zip\7z.exe'))) { return Join-Path $p '7-Zip\7z.exe' }
  }
  $local = Join-Path $Deps '_tools\7-Zip\Files\7-Zip\7z.exe'
  if (Test-Path $local) { return $local }
  # fetch-deps.ps1's "upstream" component unpacks the same pinned copy.
  throw 'no 7-Zip: install it, or run fetch-deps.ps1 -Only upstream, which unpacks a portable one'
}

# ── 1. dependencies ───────────────────────────────────────────────────────────
if (-not $SkipDeps) {
  Note "dependencies ($Platform)"
  & (Join-Path $PSScriptRoot 'fetch-deps.ps1') -Arch $Arch
  if (-not $?) { throw 'fetch-deps.ps1 failed' }
}
foreach ($f in 'bin\lua5.1.dll', 'bin\locale\en.dll', 'bin\python', 'bin\lib\ruby', 'bin\tcl8.6', 'lib\ruby.lib') {
  if (-not (Test-Path (Join-Path $Deps "$Platform\$f"))) { throw "packaging\windows\deps\$Platform\$f is missing: run fetch-deps.ps1 -Arch $Arch" }
}
if (-not (Test-Path (Join-Path $Deps 'upstream\help.db'))) { throw 'packaging\windows\deps\upstream is missing: run fetch-deps.ps1 -Only upstream' }

if ($WithDiscworld) {
  if (-not $DiscworldDir) { $DiscworldDir = Join-Path $Repo 'packaging\discworld-plugins' }
  $DiscworldDir = (Resolve-Path $DiscworldDir).Path
  if (-not (Get-ChildItem (Join-Path $DiscworldDir 'worlds\plugins') -Filter 'Quow*.xml' -ErrorAction SilentlyContinue)) {
    throw "no Quow*.xml under $DiscworldDir\worlds\plugins: stage them first (bash packaging/fetch-discworld-plugins.sh)"
  }
}

# ── 2. the client ─────────────────────────────────────────────────────────────
$exe = Join-Path $Repo "WinRel-$Platform\MUSHclient.exe"
if (-not $NoBuild) {
  Note "building MUSHclient $Version (Release|$Platform)"
  Exec $msbuild @((Join-Path $Repo 'MUSHclient.vcxproj'), '/nologo', '/m', '/v:minimal',
                  '/p:Configuration=Release', "/p:Platform=$Platform", "/p:SolutionDir=$Repo\")
}
if (-not (Test-Path $exe)) { throw "no $exe (build it, or drop -NoBuild)" }
if ((Get-PeMachine $exe) -ne $PeMachine) { throw "$exe is not a $Platform executable" }

# ── 3. the installer ──────────────────────────────────────────────────────────
New-Dir $OutDir | Out-Null
$makensis = Find-MakeNsis
$setupName = "$Base-setup.exe"
$nsisArgs = @('/V2', "/DARCH=$Arch", "/DVERSION=$Version", "/DCRTDIR=$crtDir")
if ($WithDiscworld) { $nsisArgs += "/DDISCWORLD=$DiscworldDir" }
Note "building the installer ($setupName)"
Exec $makensis ($nsisArgs + @('mushclient.nsi')) (Join-Path $Repo 'install')
$built = Join-Path $Repo "install\$setupName"
if (-not (Test-Path $built)) { throw "makensis wrote no $built" }
$setup = Join-Path $OutDir $setupName
Move-Item -Force $built $setup

# ── 4. the portable zip: the installer's payload ──────────────────────────────
$7z = Find-SevenZip
$tree = Join-Path $OutDir $Base
if (Test-Path $tree) { Remove-Item -Recurse -Force $tree }
Note "extracting the installer's payload for the portable zip"
Exec $7z @('x', '-y', '-bso0', '-bsp0', "-o$tree", $setup)
# NSIS's own bookkeeping, not part of the install.
foreach ($junk in '$PLUGINSDIR', '$TEMP', 'uninstall.exe', '[NSIS].nsi') {
  $p = Join-Path $tree $junk
  if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force }
}
if (-not (Test-Path (Join-Path $tree 'MUSHclient.exe'))) { throw "the extracted payload has no MUSHclient.exe ($tree)" }

# ── 5. self-containment ───────────────────────────────────────────────────────
Note 'checking the package is self-contained'
$dumpbin = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Recurse -Filter dumpbin.exe |
           Where-Object { $_.FullName -match '\\bin\\Host(x64|arm64)\\' } | Select-Object -First 1
if (-not $dumpbin) { throw 'dumpbin.exe not found under the Visual Studio C++ tools' }
$pes = Get-ChildItem -Recurse -File $tree | Where-Object { $_.Extension -in '.exe', '.dll', '.pyd', '.so' }
$shipped = @{}
foreach ($pe in $pes) { $shipped[$pe.Name.ToLowerInvariant()] = $true }
$sys = Join-Path $env:SystemRoot 'System32'
$problems = @()
foreach ($pe in $pes) {
  $m = Get-PeMachine $pe.FullName
  if ($m -eq -1) { continue }                                  # a .so that is not a PE (none expected)
  # x64 code in an ARM64 package is fine only as ARM64X/ARM64EC, which none
  # of this is; anything else is a wrong-architecture file.
  if ($m -ne $PeMachine) { $problems += ('{0}: machine 0x{1:X4}, not {2}' -f $pe.FullName.Substring($tree.Length + 1), $m, $Platform); continue }
  $deps = & $dumpbin.FullName /nologo /dependents $pe.FullName |
          ForEach-Object { if ($_ -match '^\s+(\S+\.dll)\s*$') { $Matches[1].ToLowerInvariant() } }
  foreach ($d in $deps) {
    if ($shipped.ContainsKey($d)) { continue }
    if ($d -like 'api-ms-win-*' -or $d -like 'ext-ms-*') { continue }   # the UCRT and API sets: in Windows 10+
    # Part of Windows -- but not the VC++ runtime, which a machine has only
    # if something else installed it, and not a debug CRT.
    # (Parenthesised: PowerShell's -and and -or bind equally, left to right.)
    $isRedist = ($d -match '^(vcruntime|msvcp|concrt|vccorlib)\d') -or ($d -match '^ucrtbased')
    if (-not $isRedist -and (Test-Path (Join-Path $sys $d))) { continue }
    $problems += "$($pe.FullName.Substring($tree.Length + 1)) imports $d, which is neither in the package nor part of Windows"
  }
}
if ($problems) { $problems | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }; throw "the package is not self-contained ($($problems.Count) problems)" }
Note "  $($pes.Count) executables and DLLs, all $Platform, every import satisfied"

$zip = Join-Path $OutDir "$Base.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Note "writing $Base.zip"
# The zip's top level is the MUSHclient folder, so it unpacks into one.
Exec $7z @('a', '-tzip', '-mx=9', '-bso0', '-bsp0', $zip, $Base) $OutDir

# ── 6. the package's own scripting engines ───────────────────────────────────
# The extracted tree IS the zip's content, so the conformance suite run from it
# (platform/engines_selftest.cpp, every language) proves the shipped files
# work together -- the runtimes find their libraries, the DLLs load.  After the
# zip is written: the run creates preferences files beside the exe.  Only where
# the package can run: an ARM64 one cannot on an x64 machine.
if ($NoSelftest) {
  Note 'engine selftest skipped (-NoSelftest)'
} elseif ($Arch -eq 'arm64' -and -not $hostArm) {
  Note 'engine selftest skipped: an ARM64 package cannot run on this x64 machine'
} else {
  Note 'running every scripting language''s selftest from the package'
  $out = Join-Path $OutDir "$Base-selftest.txt"
  $p = Start-Process (Join-Path $tree 'MUSHclient.exe') -ArgumentList '--engines-selftest' -WorkingDirectory $tree `
                     -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError "$out.err"
  $null = $p.Handle                                            # see tests\win\run_tests.ps1
  if (-not $p.WaitForExit(600000)) { $p.Kill(); throw "the engine selftest did not finish (see $out)" }
  Get-Content $out | Where-Object { $_ -match '^ENGINES-SELFTEST: \d|\[FAIL\]' } | ForEach-Object { "    $_" }
  if ($p.ExitCode -ne 0) { throw "the engine selftest failed (exit $($p.ExitCode); see $out)" }
  Remove-Item -Force "$out.err" -ErrorAction SilentlyContinue
}

$sums = Join-Path $OutDir "SHA256SUMS-windows-$Arch$Edition.txt"
($setup, $zip | ForEach-Object { '{0}  {1}' -f (Get-FileHash -Algorithm SHA256 $_).Hash.ToLowerInvariant(), (Split-Path -Leaf $_) }) |
  Set-Content -Path $sums -Encoding ASCII
Note 'done'
Get-Item $setup, $zip, $sums | ForEach-Object { '    {0,-60} {1,8:N1} MB' -f $_.Name, ($_.Length / 1MB) }
