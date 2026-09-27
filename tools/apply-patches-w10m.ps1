#Requires -Version 5.0
<#
.SYNOPSIS
  Put the Windows 10 Mobile port on a plain Firefox tree.

.DESCRIPTION
  patches/w10m holds the port as a series of commits (git format-patch) on top
  of the upstream release tag named in engine/release.txt. This checks out that
  tag on a new branch and replays the series onto it with `git am`, keeping
  authors, dates and messages -- the result is the same tree, byte for byte, as
  the branch the patches were exported from (tools/export-patches-w10m.sh).

  The clone only needs the release tag:
    git clone --depth 1 --branch FIREFOX_155_0_1_RELEASE `
        https://github.com/mozilla-firefox/firefox engine/firefox

  The same as tools/apply-patches-w10m.sh, for a shell without bash.
#>
[CmdletBinding()]
param(
    [string]$Gecko,
    [string]$Branch = 'w10m-port'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Gecko) { $Gecko = Join-Path $root 'engine\firefox' }
$tag = ((Get-Content (Join-Path $root 'engine\release.txt') -TotalCount 1).Trim() -split '\s+')[0]

& git -C $Gecko rev-parse -q --verify "$tag^{commit}" | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "$Gecko has no $tag. Fetch it first: git -C `"$Gecko`" fetch --depth 1 origin tag $tag"
}
& git -C $Gecko rev-parse -q --verify "refs/heads/$Branch" | Out-Null
if ($LASTEXITCODE -eq 0) {
    throw "$Gecko already has a branch $Branch; delete it or pass -Branch."
}

$patches = @(Get-ChildItem -Path (Join-Path $root 'patches\w10m') -Filter *.patch |
    Sort-Object Name | ForEach-Object { $_.FullName })
if ($patches.Count -eq 0) { throw "No patches in patches\w10m." }

& git -C $Gecko checkout -q -b $Branch $tag
if ($LASTEXITCODE -ne 0) { throw "git checkout failed" }
# --keep-cr: the patches carry CRLF lines as they are, and Firefox's
# .gitattributes (* -text) keeps git from converting them.
& git -C $Gecko am --keep-cr --3way --whitespace=nowarn @patches
if ($LASTEXITCODE -ne 0) { throw "git am stopped; see git -C `"$Gecko`" am --show-current-patch" }
Write-Host "Applied $($patches.Count) patches: $Gecko is on $Branch." -ForegroundColor Green
