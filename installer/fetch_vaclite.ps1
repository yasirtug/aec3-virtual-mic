<#
.SYNOPSIS
  Downloads the official Virtual Audio Cable Lite package, verifies its pinned
  SHA-256 and extracts it unmodified to installer/vendor/vaclite/ for
  AEC3VirtualMic.iss.

.DESCRIPTION
  Download only: nothing is installed. VAC Lite is freeware by Eugene
  Muzychenko; its license allows free, unmodified redistribution (see
  installer/README.md). Exits with a non-zero code on download, hash or
  extraction failure.

.PARAMETER Force
  Re-download and re-extract even if the extracted package is already present.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File installer/fetch_vaclite.ps1
#>
[CmdletBinding()]
param(
  [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# Pinned package. When a new VAC Lite version is published, update all three
# values (and the version noted in installer/README.md) after verifying it.
$PackUrl = 'https://software.muzychenko.net/freeware/vac471lite.zip'
$PackFile = 'vac471lite.zip'
$PackSha256 = '1ED42A7E763BF6237438FB62191AE88B2922F014B37658B29D7801D72C9D5357'

# Files the installer relies on (VAC's setup needs the whole package layout).
$RequiredFiles = @(
  'setup.exe',
  'setup64.exe',
  'setup.ini',
  'license.txt',
  'vrtaucbl.inf',
  'x64\vrtaucbl.sys'
)

$VendorDir = Join-Path $PSScriptRoot 'vendor'
$DestDir = Join-Path $VendorDir 'vaclite'
$ZipPath = Join-Path $VendorDir $PackFile
# Kept beside (not inside) the extracted package so the package stays unmodified.
$StampPath = Join-Path $VendorDir 'vaclite.sha256'

function Test-Extracted {
  if (-not (Test-Path -LiteralPath $StampPath)) { return $false }
  if ((Get-Content -LiteralPath $StampPath -Raw).Trim() -ne $PackSha256) { return $false }
  foreach ($name in $RequiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $DestDir $name))) { return $false }
  }
  return $true
}

if (-not $Force -and (Test-Extracted)) {
  Write-Host "VAC Lite package already present in $DestDir"
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
      'The package may have been republished; verify it and update the pin.')
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
    Write-Error "Extracted package is missing $name"
    exit 3
  }
}
Set-Content -LiteralPath $StampPath -Value $PackSha256 -Encoding ascii
Write-Host "VAC Lite package extracted to $DestDir"
exit 0
