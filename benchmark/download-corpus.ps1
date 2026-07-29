param(
  [string]$DownloadDirectory = "",
  [string]$SourceDirectory = ""
)

$ErrorActionPreference = "Stop"
$run = $PSScriptRoot
$downloads = if ($DownloadDirectory) {
  [IO.Path]::GetFullPath($DownloadDirectory)
} else {
  Join-Path $run "downloads"
}
$sources = if ($SourceDirectory) {
  [IO.Path]::GetFullPath($SourceDirectory)
} else {
  Join-Path $run "sources"
}
New-Item -ItemType Directory -Force -Path $downloads, $sources | Out-Null

$archives = @(
  [pscustomobject]@{
    Name = "qoi"
    File = "qoi_benchmark_suite.tar"
    Url = "https://qoiformat.org/benchmark/qoi_benchmark_suite.tar"
    SHA256 = "00166f555ca760e23647025e46d9b046e03cd2e22869d4ecb496edc527cd0d7d"
    Bytes = 1147070464
  },
  [pscustomobject]@{
    Name = "div2k"
    File = "DIV2K_valid_HR.zip"
    Url = "https://data.vision.ee.ethz.ch/cvl/DIV2K/DIV2K_valid_HR.zip"
    SHA256 = "20dd31fd84d777bc1cf5d6b7654a3f569c0aec74458ae094122ad1d0489900fc"
    Bytes = 448993893
  },
  [pscustomobject]@{
    Name = "clic2022"
    File = "clic2022_image_valid.zip"
    Url = "https://storage.googleapis.com/clic_datasets/clic2022_image_valid.zip"
    SHA256 = "f12d925bcf080cf941a95029156a26b8395424372ea9fe3a6d9dc122c0a67a83"
    Bytes = 126724438
  },
  [pscustomobject]@{
    Name = "enrico"
    File = "enrico_screenshots.zip"
    Url = "https://userinterfaces.aalto.fi/enrico/resources/screenshots.zip"
    SHA256 = "303c3fced6012f54606c32bcc3404a68e7aa2f9b046a298a749b4ba3f68e802d"
    Bytes = 115135063
  }
)
$files = @(
  [pscustomobject]@{
    Name = "enrico-topics"
    File = "enrico_design_topics.csv"
    Url = "https://raw.githubusercontent.com/luileito/enrico/2292f72ef2604139a2a205e291e688270f132943/design_topics.csv"
    SHA256 = "a3c7d74e00c1f9020cdeba9620f10c65edc3c91b74d545cf5b058e902283db54"
    Bytes = 18144
  }
)

function Download-Checked([object]$Item) {
  $destination = Join-Path $downloads $Item.File
  if (!(Test-Path -LiteralPath $destination)) {
    Write-Host "Downloading $($Item.File)"
    & curl.exe -L --fail --retry 5 --retry-delay 2 `
      --output $destination $Item.Url
    if ($LASTEXITCODE -ne 0) {
      throw "Download failed: $($Item.Url)"
    }
  }
  $actualLength = (Get-Item -LiteralPath $destination).Length
  if ($actualLength -ne $Item.Bytes) {
    throw "$($Item.File) has $actualLength bytes, expected $($Item.Bytes)"
  }
  $actualHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).
    Hash.ToLowerInvariant()
  if ($actualHash -ne $Item.SHA256) {
    throw "$($Item.File) has SHA-256 $actualHash, expected $($Item.SHA256)"
  }
  return $destination
}

function Assert-SafeArchive([string]$Archive) {
  $entries = @(tar -tf $Archive)
  if ($LASTEXITCODE -ne 0 -or !$entries.Count) {
    throw "Could not list archive: $Archive"
  }
  foreach ($entry in $entries) {
    if ($entry -match '(^[\\/])|(^[A-Za-z]:)|(^|[\\/])\.\.([\\/]|$)') {
      throw "Unsafe archive path: $entry"
    }
  }
}

foreach ($archive in $archives) {
  $path = Download-Checked $archive
  $destination = Join-Path $sources $archive.Name
  $marker = Join-Path $destination ".archive-sha256"
  if (Test-Path -LiteralPath $destination) {
    if (!(Test-Path -LiteralPath $marker) -or
        (Get-Content -LiteralPath $marker -Raw).Trim() -ne $archive.SHA256) {
      throw "Existing extraction does not match $($archive.File): $destination"
    }
    Write-Host "Verified $($archive.File)"
    continue
  }
  Assert-SafeArchive $path
  New-Item -ItemType Directory -Path $destination | Out-Null
  Write-Host "Extracting $($archive.File)"
  & tar -xf $path -C $destination
  if ($LASTEXITCODE -ne 0) {
    throw "Extraction failed: $($archive.File)"
  }
  Set-Content -LiteralPath $marker -Value $archive.SHA256 -Encoding ascii
}

foreach ($file in $files) {
  $path = Download-Checked $file
  $destination = Join-Path $sources $file.File
  Copy-Item -LiteralPath $path -Destination $destination -Force
}

$manifest = @($archives + $files | ForEach-Object {
  [pscustomobject][ordered]@{
    Name = $_.Name
    URL = $_.Url
    File = $_.File
    Bytes = $_.Bytes
    SHA256 = $_.SHA256
  }
})
$manifest | ConvertTo-Json -Depth 4 |
  Set-Content -LiteralPath (Join-Path $run "downloads.json") -Encoding utf8
$manifest | Format-Table Name, Bytes, SHA256 -AutoSize
