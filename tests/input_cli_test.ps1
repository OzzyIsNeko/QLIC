param(
  [Parameter(Mandatory = $true)]
  [string]$Qlic,
  [Parameter(Mandatory = $true)]
  [string]$Fixtures,
  [Parameter(Mandatory = $true)]
  [string]$Output
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $Output | Out-Null

function Test-WicAvifUnavailable {
  param([object[]]$Message)
  return (($Message | Out-String) -match
    "WIC decoder failed: 0x88982f8b")
}

$accepted = @(
  "base.png",
  "base.bmp",
  "lossless.tiff",
  "lossless.webp",
  "lossless.jxl",
  "lossless.avif"
)
$avifDecoderAvailable = $true
$hashes = foreach ($name in $accepted) {
  $encoded = Join-Path $Output "$name.qlic"
  [IO.File]::Delete($encoded)
  $message = @(
    & $Qlic pack (Join-Path $Fixtures $name) $encoded --threads 1 2>&1
  )
  $status = $LASTEXITCODE
  if ($status -ne 0 -and $name -eq "lossless.avif" -and
      (Test-WicAvifUnavailable $message)) {
    $avifDecoderAvailable = $false
    Write-Host "lossless AVIF decode skipped because WIC is unavailable"
    continue
  }
  if ($status -ne 0) {
    throw "QLIC rejected lossless input: $name"
  }
  (Get-FileHash -LiteralPath $encoded -Algorithm SHA256).Hash
}
if (@($hashes | Select-Object -Unique).Count -ne 1) {
  throw "Lossless input formats produced different QLIC pixels."
}

$ambiguousAvif = Join-Path $Output "ambiguous.avif"
$ambiguousBytes = [IO.File]::ReadAllBytes(
  (Join-Path $Fixtures "lossless.avif"))
$changed = 0
for ($i = 0; $i -le $ambiguousBytes.Length - 4; $i++) {
  if ($ambiguousBytes[$i] -eq 99 -and
      $ambiguousBytes[$i + 1] -eq 111 -and
      $ambiguousBytes[$i + 2] -eq 108 -and
      $ambiguousBytes[$i + 3] -eq 114) {
    $ambiguousBytes[$i] = 102
    $ambiguousBytes[$i + 1] = 114
    $ambiguousBytes[$i + 2] = 101
    $ambiguousBytes[$i + 3] = 101
    $changed++
  }
}
if ($changed -ne 1) {
  throw "Could not make the ambiguous AVIF test input."
}
[IO.File]::WriteAllBytes($ambiguousAvif, $ambiguousBytes)
$ambiguousOutput = Join-Path $Output "ambiguous.avif.qlic"
[IO.File]::Delete($ambiguousOutput)
$message = @(
  & $Qlic pack $ambiguousAvif $ambiguousOutput --threads 1 2>&1
)
$status = $LASTEXITCODE
if ($status -ne 0 -and !$avifDecoderAvailable -and
    (Test-WicAvifUnavailable $message)) {
  Write-Host "ambiguous AVIF reached the unavailable WIC decoder"
} elseif ($status -ne 0 -or !(Test-Path -LiteralPath $ambiguousOutput)) {
  throw "QLIC rejected an image with ambiguous losslessness metadata."
}

$rejected = @(
  "lossy.jpg",
  "lossy.tiff",
  "lossy.webp",
  "lossy.jxl",
  "lossy.avif",
  "high16.png"
)
foreach ($name in $rejected) {
  $encoded = Join-Path $Output "$name.qlic"
  [IO.File]::Delete($encoded)
  & $Qlic pack (Join-Path $Fixtures $name) $encoded --threads 1 2>&1 |
    Out-Null
  if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath $encoded)) {
    throw "QLIC accepted unsupported or lossy input: $name"
  }
}

Write-Host "lossless input CLI checks passed"
