# Signs an appx package.
#
# Signing lives in PowerShell rather than in build-appx.sh for one concrete
# reason: msys2's bash hands its children an environment with TEMP and TMP
# empty. signtool unpacks and repacks an appx through a temp directory, and
# with none available it fails with "This file format cannot be signed because
# it is not recognized" -- which reads like a corrupt package but is really a
# missing temp dir. This script restores both before signing.
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [Parameter(Mandatory = $true)][string]$Certificate,
    # The manifest's Publisher: a certificate made here gets it as its subject,
    # and an existing one must already have it or the package will not install.
    [string]$Subject = '',
    [string]$Password = $(if ($env:GECKO_W10M_PFX_PASSWORD) { $env:GECKO_W10M_PFX_PASSWORD } else { 'gecko_w10m' }),
    [string]$SdkBin = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64'
)

$ErrorActionPreference = 'Stop'

# msys2 hands its children an environment with TEMP, TMP and USERPROFILE all
# empty, and GetTempPath() then falls back to C:\Windows\, which is not
# writable. Pick the first directory we can actually write to.
$tempCandidates = New-Object System.Collections.ArrayList
foreach ($v in @($env:TEMP, $env:TMP)) {
    if ($v) { [void]$tempCandidates.Add($v) }
}
if ($env:LOCALAPPDATA) { [void]$tempCandidates.Add((Join-Path $env:LOCALAPPDATA 'Temp')) }
[void]$tempCandidates.Add((Join-Path (Split-Path -Parent $Package) 'signtemp'))
$temp = $null
foreach ($c in $tempCandidates) {
    if (-not $c) { continue }
    try {
        if (-not (Test-Path $c)) { New-Item -ItemType Directory -Path $c -Force | Out-Null }
        $probe = Join-Path $c ([System.IO.Path]::GetRandomFileName())
        [System.IO.File]::WriteAllText($probe, 'x')
        Remove-Item $probe -Force
        $temp = $c
        break
    } catch { }
}
if (-not $temp) { throw 'no writable temp directory found' }
$env:TEMP = $temp
$env:TMP = $temp
$signtool = Join-Path $SdkBin 'signtool.exe'

if (-not (Test-Path $signtool)) { throw "signtool not found at $signtool" }
if (-not (Test-Path $Package)) { throw "package not found: $Package" }

if (-not (Test-Path $Certificate)) {
    if (-not $Subject) { throw "no certificate at $Certificate, and no -Subject to make one for" }
    Write-Output "    creating a self-signed certificate ($Subject)"
    $cert = New-SelfSignedCertificate -Type Custom -Subject $Subject `
        -KeyUsage DigitalSignature -FriendlyName 'GeckoW10m W10M' `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
    $secure = ConvertTo-SecureString -String $Password -Force -AsPlainText
    Export-PfxCertificate -Cert $cert -FilePath $Certificate -Password $secure | Out-Null
    Write-Output "    thumbprint $($cert.Thumbprint)"

    # The .cer next to it is what gets installed on the device.
    $cerPath = [System.IO.Path]::ChangeExtension($Certificate, '.cer')
    Export-Certificate -Cert $cert -FilePath $cerPath | Out-Null
    Write-Output "    exported $cerPath"
}

# Start-Process with explicit redirection rather than the call operator:
# invoked from a nested msys2 bash, "& $signtool" inherits a stdout handle it
# cannot write to and its output vanishes, taking the diagnostics with it.
$outFile = Join-Path $env:TEMP 'gecko-w10m-signtool-out.txt'
$errFile = Join-Path $env:TEMP 'gecko-w10m-signtool-err.txt'
$proc = Start-Process -FilePath $signtool -Wait -NoNewWindow -PassThru `
    -ArgumentList @('sign', '/fd', 'SHA256', '/f', $Certificate, '/p', $Password, $Package) `
    -RedirectStandardOutput $outFile -RedirectStandardError $errFile
foreach ($f in @($outFile, $errFile)) {
    if (Test-Path $f) {
        Get-Content $f | Where-Object { $_.Trim() } | ForEach-Object { Write-Output "    | $_" }
        Remove-Item $f -Force
    }
}
if ($proc.ExitCode -ne 0) { throw "signtool exited with $($proc.ExitCode)" }

# Do not trust $LASTEXITCODE here: when this script runs under a bash pipeline
# it has come back empty even on success. Ask the package itself instead --
# a signed appx carries AppxSignature.p7x.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($Package)
try {
    $signed = $null -ne ($zip.Entries | Where-Object { $_.FullName -eq 'AppxSignature.p7x' })
} finally {
    $zip.Dispose()
}

if (-not $signed) { throw "package is not signed: $Package" }
Write-Output "    signature present (AppxSignature.p7x)"
