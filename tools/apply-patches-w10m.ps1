#Requires -Version 5.0
<#
.SYNOPSIS
  Apply the Windows 10 Mobile (arm-uwp) patch set to the Gecko tree and drop in
  the JIT executable-memory wrappers.

.DESCRIPTION
  Run after `git submodule update --init --recursive engine/firefox`.
  Applies every *.patch under patches/w10m/ with `git apply` (3-way, with fuzz),
  then copies the runtime shim sources into js/src so the engine build picks
  them up.
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'
$gecko   = Join-Path $RepoRoot 'engine\firefox'
$patches = Join-Path $RepoRoot 'patches\w10m'
$runtime = Join-Path $patches   'runtime'

if (-not (Test-Path (Join-Path $gecko 'js\src'))) {
    throw "Gecko tree not found at '$gecko'. Run: git submodule update --init --recursive engine/firefox"
}

Write-Host "Applying W10M patch set to $gecko" -ForegroundColor Cyan

# 1. Apply diffs (skip the runtime/ folder, which holds drop-in sources).
Get-ChildItem -Path $patches -Recurse -Filter *.patch | ForEach-Object {
    Write-Host "  apply $($_.FullName.Substring($patches.Length + 1))"
    & git -C $gecko apply --3way --whitespace=nowarn --recount $_.FullName
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "  '$($_.Name)' did not apply cleanly; resolve manually or re-cut against $(Get-Content (Join-Path $RepoRoot 'engine\release.txt'))"
    }
}

# 2. Drop the JIT wrapper source + compat header into js/src.
$jsSrc = Join-Path $gecko 'js\src'
Copy-Item (Join-Path $runtime 'gecko_w10m_winuwp_jit.cpp')   $jsSrc -Force
Copy-Item (Join-Path $runtime 'gecko_w10m_winuwp_compat.h')  $jsSrc -Force
Write-Host "  copied gecko_w10m_winuwp_jit.cpp + gecko_w10m_winuwp_compat.h -> js/src"

# 2b. Drop the new UWP configure module into the tree (new file, not a diff).
$uwpConf = Join-Path $patches 'build\moz.configure\uwp.configure'
Copy-Item $uwpConf (Join-Path $gecko 'build\moz.configure') -Force
Write-Host "  copied uwp.configure -> build/moz.configure"

# 3. Register the wrapper in js/src/moz.build (idempotent).
$mozBuild = Join-Path $jsSrc 'moz.build'
$marker   = '# GeckoW10m/W10M JIT wrapper'
if (-not (Select-String -Path $mozBuild -SimpleMatch $marker -Quiet)) {
    @"

$marker
if CONFIG.get('OS_TARGET') == 'WINNT' and CONFIG.get('GECKO_W10M'):
    SOURCES += ['gecko_w10m_winuwp_jit.cpp']
"@ | Add-Content -Path $mozBuild -Encoding utf8
    Write-Host "  registered gecko_w10m_winuwp_jit.cpp in js/src/moz.build"
} else {
    Write-Host "  moz.build already registers the wrapper"
}

Write-Host "W10M patch set applied." -ForegroundColor Green
