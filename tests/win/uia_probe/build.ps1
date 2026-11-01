# Builds uia_probe.exe beside this script, for the machine's own architecture.
param([string]$Arch = 'arm64')
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
$bat = Join-Path $env:TEMP "claude_uia_$PID.bat"
"@echo off`r`ncall `"$vs\VC\Auxiliary\Build\vcvarsall.bat`" $Arch >nul 2>&1`r`ncd /d `"$PSScriptRoot`"`r`ncl /nologo /EHsc /W3 uia_probe.cpp /Fe:uia_probe.exe`r`n" | Set-Content $bat -Encoding ascii
& cmd /c $bat
Get-Item $bat | ForEach-Object { [IO.File]::Delete($_.FullName) }
