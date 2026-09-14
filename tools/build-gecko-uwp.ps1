#Requires -Version 5.0
<#
.SYNOPSIS
  Build the JIT-enabled SpiderMonkey/Gecko engine for Windows 10 Mobile (arm-uwp).

.DESCRIPTION
  Sets up the VS2017 + Windows 10 SDK ARM toolchain that mozbuild expects, points
  it at mozconfig/mozconfig.arm-uwp, and runs `mach build`.

  Prerequisites: MozillaBuild, Python 3, Rust (thumbv7-uwp target), the VS2017
  "Universal Windows Platform" + "C++ ARM" workloads, and the Windows 10 SDK.
#>
[CmdletBinding()]
param(
    [string]$VsRoot   = 'C:\Program Files (x86)\Microsoft Visual Studio\2017\Enterprise',
    [string]$SdkVer   = '10.0.15254.0',   # last W10M-capable Windows 10 SDK on this box
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'
$gecko = Join-Path $RepoRoot 'engine\firefox'

if (-not (Test-Path $VsRoot)) { throw "VS2017 not found at '$VsRoot'." }
if (-not (Test-Path (Join-Path $gecko 'mach'))) {
    throw "Gecko tree/mach not found. Run apply-patches-w10m.ps1 first."
}

# ---- Toolchain env ----------------------------------------------------------
$vcTools = Get-ChildItem (Join-Path $VsRoot 'VC\Tools\MSVC') |
           Sort-Object Name -Descending | Select-Object -First 1
if (-not $vcTools) { throw "No MSVC toolset under $VsRoot\VC\Tools\MSVC." }

$env:VCINSTALLDIR   = (Join-Path $VsRoot 'VC') + '\'
$env:VCToolsVersion = $vcTools.Name
$env:WindowsSdkDir  = 'C:\Program Files (x86)\Windows Kits\10\'
$env:WindowsSDKVersion = "$SdkVer\"
$env:UCRTVersion    = $SdkVer

# ARM cross compiler (x86-hosted -> arm target) and ARM libs.
$hostArch = 'Hostx86'
$env:PATH = (Join-Path $vcTools.FullName "bin\$hostArch\arm") + ';' +
            (Join-Path $vcTools.FullName "bin\$hostArch\x86") + ';' + $env:PATH
$env:CC  = 'cl.exe'
$env:CXX = 'cl.exe'

# UWP store-mode libs (append the /store subdir so the ARM UWP CRT is linked).
$env:LIB = @(
    (Join-Path $vcTools.FullName 'lib\arm\store'),
    (Join-Path $vcTools.FullName 'lib\arm'),
    "$($env:WindowsSdkDir)Lib\$SdkVer\ucrt\arm",
    "$($env:WindowsSdkDir)Lib\$SdkVer\um\arm"
) -join ';'
$env:INCLUDE = @(
    (Join-Path $vcTools.FullName 'include'),
    "$($env:WindowsSdkDir)Include\$SdkVer\ucrt",
    "$($env:WindowsSdkDir)Include\$SdkVer\um",
    "$($env:WindowsSdkDir)Include\$SdkVer\shared",
    "$($env:WindowsSdkDir)Include\$SdkVer\winrt"
) -join ';'

$env:MOZCONFIG = Join-Path $RepoRoot 'mozconfig\mozconfig.arm-uwp'

Write-Host "MSVC toolset : $($vcTools.Name)"        -ForegroundColor Cyan
Write-Host "Win10 SDK    : $SdkVer"                  -ForegroundColor Cyan
Write-Host "MOZCONFIG    : $env:MOZCONFIG"           -ForegroundColor Cyan

# ---- Build ------------------------------------------------------------------
Push-Location $gecko
try {
    & .\mach build
    if ($LASTEXITCODE -ne 0) { throw "mach build failed (exit $LASTEXITCODE)." }
    Write-Host "Engine build complete. Objdir: $RepoRoot\obj-arm-uwp" -ForegroundColor Green
}
finally {
    Pop-Location
}
