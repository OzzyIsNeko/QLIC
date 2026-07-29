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

$accepted = @(
  "base.png",
  "base.bmp",
  "lossless.tiff",
  "lossless.webp",
  "lossless.jxl",
  "lossless.avif"
)
$hashes = foreach ($name in $accepted) {
  $encoded = Join-Path $Output "$name.qlic"
  [IO.File]::Delete($encoded)
  & $Qlic pack (Join-Path $Fixtures $name) $encoded --threads 1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
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
& $Qlic pack $ambiguousAvif $ambiguousOutput --threads 1 | Out-Null
if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $ambiguousOutput)) {
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
