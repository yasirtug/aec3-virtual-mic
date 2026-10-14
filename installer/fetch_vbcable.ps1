<#
.SYNOPSIS
  Downloads the official VB-CABLE driver pack, verifies its pinned SHA-256 and
  extracts it to installer/vendor/vbcable/ for AEC3VirtualMic.iss.

.DESCRIPTION
  Download only: nothing is installed. The pack is VB-Audio donationware
  (https://vb-cable.com); see installer/README.md for licensing and sources.
  Exits with a non-zero code on download, hash or extraction failure.

.PARAMETER Force
  Re-download and re-extract even if the extracted pack is already present.

.EXAMPLE
  pwsh -NoProfile -File installer/fetch_vbcable.ps1
#>
[CmdletBinding()]
param(
  [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# Pinned pack. When VB-Audio publishes a new pack, update all three values
# (and the version noted in installer/README.md) after verifying the download.
$PackUrl = 'https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip'
$PackFile = 'VBCABLE_Driver_Pack45.zip'
$PackSha256 = 'B950E39F01AF1D04EA623C8F6D8EB9B6EA5C477C637295FABF20631C85116BFB'

# Files the installer needs from the pack.
$RequiredFiles = @(
  'VBCABLE_Setup_x64.exe',
  'vbMmeCable64_win10.inf',
  'vbaudio_cable64_win10.sys',
  'vbaudio_cable64_win10.cat',
  'vbaudio_cable64arm_win10.sys',
  'readme.txt'
)

$VendorDir = Join-Path $PSScriptRoot 'vendor'
$DestDir = Join-Path $VendorDir 'vbcable'
$ZipPath = Join-Path $VendorDir $PackFile
$StampPath = Join-Path $DestDir '.sha256'

function Test-Extracted {
  if (-not (Test-Path -LiteralPath $StampPath)) { return $false }
  if ((Get-Content -LiteralPath $StampPath -Raw).Trim() -ne $PackSha256) { return $false }
  foreach ($name in $RequiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $DestDir $name))) { return $false }
  }
  return $true
}

if (-not $Force -and (Test-Extracted)) {
  Write-Host "VB-CABLE pack already present in $DestDir"
  exit 0
}

New-Item -ItemType Directory -Force -Path $VendorDir | Out-Null

$haveZip = (Test-Path -LiteralPath $ZipPath) -and
  ((Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash -eq $PackSha256)
if ($Force -or -not $haveZip) {
  Write-Host "Downloading $PackUrl"
  [Net.ServicePointManager]::SecurityProtocol =
    [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
  $partial = "$ZipPath.partial"
  try {
    Invoke-WebRequest -Uri $PackUrl -OutFile $partial -UseBasicParsing
  } catch {
    Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
    Write-Error "Download failed: $($_.Exception.Message)"
    exit 1
  }
  $actual = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash
  if ($actual -ne $PackSha256) {
    Remove-Item -LiteralPath $partial -Force
    Write-Error ("SHA-256 mismatch for $PackFile`n  expected $PackSha256`n  actual   $actual`n" +
      'VB-Audio may have republished the pack; verify it and update the pin.')
    exit 2
  }
  Move-Item -LiteralPath $partial -Destination $ZipPath -Force
}

if (Test-Path -LiteralPath $DestDir) {
  Remove-Item -LiteralPath $DestDir -Recurse -Force
}
Expand-Archive -LiteralPath $ZipPath -DestinationPath $DestDir -Force

foreach ($name in $RequiredFiles) {
  if (-not (Test-Path -LiteralPath (Join-Path $DestDir $name))) {
    Write-Error "Extracted pack is missing $name"
    exit 3
  }
}
Set-Content -LiteralPath $StampPath -Value $PackSha256 -Encoding ascii
Write-Host "VB-CABLE pack extracted to $DestDir"
exit 0
