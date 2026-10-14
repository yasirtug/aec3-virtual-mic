<#
.SYNOPSIS
  Downloads RNNoise's pinned standard model and extracts the generated weights
  (src/rnnoise_data.c and .h) into third_party/rnnoise/src/.

.DESCRIPTION
  The weights are ~78 MB of generated C, so they are fetched like upstream's
  download_model.sh instead of being committed. The pin is the SHA-256 in
  third_party/rnnoise/model_version, which is also the tarball's name.
  build.bat runs this automatically when the weights are missing or stale.
  Exits non-zero on download, hash or extraction failure.

.PARAMETER Force
  Re-download and re-extract even if the weights are already present.
#>
[CmdletBinding()]
param(
  [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$RnnoiseDir = Join-Path $RepoRoot 'third_party\rnnoise'
$SrcDir = Join-Path $RnnoiseDir 'src'
$ModelSha256 = (Get-Content -LiteralPath (Join-Path $RnnoiseDir 'model_version') -Raw).Trim().ToLowerInvariant()
$ModelUrl = "https://media.xiph.org/rnnoise/models/rnnoise_data-$ModelSha256.tar.gz"
$StampPath = Join-Path $SrcDir '.rnnoise_data.sha256'
$DataFiles = @('rnnoise_data.c', 'rnnoise_data.h')

function Test-Extracted {
  if (-not (Test-Path -LiteralPath $StampPath)) { return $false }
  if ((Get-Content -LiteralPath $StampPath -Raw).Trim() -ne $ModelSha256) { return $false }
  foreach ($name in $DataFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $SrcDir $name))) { return $false }
  }
  return $true
}

if (-not $Force -and (Test-Extracted)) {
  Write-Host "RNNoise model weights are up to date ($ModelSha256)."
  exit 0
}

$DepsDir = if ($env:AEC3_DEPS_DIR) { $env:AEC3_DEPS_DIR } else { Join-Path $RepoRoot '.deps' }
$CacheDir = Join-Path $DepsDir 'rnnoise'
$Tarball = Join-Path $CacheDir "rnnoise_data-$ModelSha256.tar.gz"
New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null

$haveTarball = (Test-Path -LiteralPath $Tarball) -and
  ((Get-FileHash -LiteralPath $Tarball -Algorithm SHA256).Hash.ToLowerInvariant() -eq $ModelSha256)
if ($Force -or -not $haveTarball) {
  Write-Host "Downloading $ModelUrl"
  [Net.ServicePointManager]::SecurityProtocol =
    [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
  $partial = "$Tarball.partial"
  try {
    Invoke-WebRequest -Uri $ModelUrl -OutFile $partial -UseBasicParsing
  } catch {
    Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
    Write-Error "Download failed: $($_.Exception.Message)"
    exit 1
  }
  $actual = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($actual -ne $ModelSha256) {
    Remove-Item -LiteralPath $partial -Force
    Write-Error "SHA-256 mismatch for the RNNoise model`n  expected $ModelSha256`n  actual   $actual"
    exit 2
  }
  Move-Item -LiteralPath $partial -Destination $Tarball -Force
}

# Windows 10+ ships bsdtar as tar.exe; extract only the two weight files.
$staging = Join-Path $CacheDir 'extract'
if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
New-Item -ItemType Directory -Force -Path $staging | Out-Null
& "$env:SystemRoot\System32\tar.exe" -xzf $Tarball -C $staging 'src/rnnoise_data.c' 'src/rnnoise_data.h'
if ($LASTEXITCODE -ne 0) {
  Write-Error "Extracting $Tarball failed ($LASTEXITCODE)"
  exit 3
}
foreach ($name in $DataFiles) {
  $from = Join-Path $staging "src\$name"
  if (-not (Test-Path -LiteralPath $from)) {
    Write-Error "The RNNoise model archive is missing src/$name"
    exit 3
  }
  Move-Item -LiteralPath $from -Destination (Join-Path $SrcDir $name) -Force
}
Remove-Item -LiteralPath $staging -Recurse -Force
Set-Content -LiteralPath $StampPath -Value $ModelSha256 -Encoding ascii
Write-Host "RNNoise model weights extracted to $SrcDir"
exit 0
