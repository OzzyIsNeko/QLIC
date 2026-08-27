param(
  [string]$Output = "",
  [string]$Magick = "magick.exe",
  [ValidateRange(1, 64000000)]
  [int]$MaximumPixels = 16000000
)

$ErrorActionPreference = "Stop"
$run = $PSScriptRoot
$repo = Split-Path -Parent (Split-Path -Parent $run)
$sourcesRoot = Join-Path $run "sources"

function Resolve-Tool([string]$Value) {
  if (Test-Path -LiteralPath $Value) {
    return (Resolve-Path -LiteralPath $Value).Path
  }
  $command = Get-Command $Value -CommandType Application -ErrorAction SilentlyContinue
  if ($command) {
    return $command.Source
  }
  throw "ImageMagick was not found: $Value"
}

function Source-Category([object]$Source, [IO.FileInfo]$File) {
  if (!$Source.SplitFirst) {
    return $Source.Name
  }
  $relative = [IO.Path]::GetRelativePath($Source.Path, $File.FullName)
  $parts = $relative -split "[\\/]"
  if ($parts.Count -gt 1) {
    return "$($Source.Name)-$($parts[0])"
  }
  return $Source.Name
}

function Portable-SourcePath([IO.FileInfo]$File) {
  return [IO.Path]::GetRelativePath($sourcesRoot, $File.FullName).
    Replace("\", "/")
}

$magickExe = Resolve-Tool $Magick
$corpus = if ($Output) {
  [IO.Path]::GetFullPath($Output)
} else {
  Join-Path $run "corpus"
}
if (Test-Path -LiteralPath $corpus) {
  throw "Corpus already exists: $corpus"
}
New-Item -ItemType Directory -Path $corpus | Out-Null
$staging = Join-Path $corpus ".staging"
New-Item -ItemType Directory -Path $staging | Out-Null

$topicPath = Join-Path $sourcesRoot "enrico_design_topics.csv"
if (!(Test-Path -LiteralPath $topicPath)) {
  throw "Run download-corpus.ps1 first"
}
$enricoNames = [Collections.Generic.HashSet[string]]::new(
  [StringComparer]::OrdinalIgnoreCase
)
$topicRows = @(Import-Csv -LiteralPath $topicPath)
foreach ($group in $topicRows | Group-Object topic) {
  foreach ($row in $group.Group | Sort-Object {
      [int]$_.screen_id
    } | Select-Object -First 10) {
    [void]$enricoNames.Add("$($row.screen_id).jpg")
  }
}

$sources = @(
  [pscustomobject]@{
    Name = "qoi"
    Path = Join-Path $sourcesRoot "qoi\images"
    SplitFirst = $true
    IncludeNames = $null
  },
  [pscustomobject]@{
    Name = "natural-div2k"
    Path = Join-Path $sourcesRoot "div2k\DIV2K_valid_HR"
    SplitFirst = $false
    IncludeNames = $null
  },
  [pscustomobject]@{
    Name = "natural-clic2022"
    Path = Join-Path $sourcesRoot "clic2022"
    SplitFirst = $false
    IncludeNames = $null
  },
  [pscustomobject]@{
    Name = "ui-enrico"
    Path = Join-Path $sourcesRoot "enrico\screenshots"
    SplitFirst = $false
    IncludeNames = $enricoNames
  }
)

$extensions = @(
  ".png",
  ".jpg",
  ".jpeg",
  ".bmp",
  ".ppm",
  ".pgm",
  ".tif",
  ".tiff",
  ".webp"
)
$candidates = [Collections.Generic.List[object]]::new()
foreach ($source in $sources) {
  if (!(Test-Path -LiteralPath $source.Path)) {
    continue
  }
  $item = Get-Item -LiteralPath $source.Path
  $files = if ($item.PSIsContainer) {
    Get-ChildItem -LiteralPath $item.FullName -Recurse -File
  } else {
    @($item)
  }
  foreach ($file in $files) {
    if (($extensions -contains $file.Extension.ToLowerInvariant()) -and
        ($null -eq $source.IncludeNames -or
        $source.IncludeNames.Contains($file.Name))) {
      $candidates.Add([pscustomobject]@{
        Source = $source
        File = $file
      })
    }
  }
}
$ordered = @($candidates | Sort-Object {
  $_.Source.Name
}, {
  $_.File.FullName
})

$seen = [Collections.Generic.HashSet[string]]::new(
  [StringComparer]::OrdinalIgnoreCase
)
$rows = [Collections.Generic.List[object]]::new()
$excluded = [Collections.Generic.List[object]]::new()
$duplicates = 0
$totalPixels = [int64]0

for ($index = 0; $index -lt $ordered.Count; $index++) {
  $entry = $ordered[$index]
  $file = $entry.File
  $category = Source-Category $entry.Source $file
  $candidate = Join-Path $staging ("{0:D6}.png" -f ($index + 1))
  Write-Host "[$($index + 1)/$($ordered.Count)] $category $($file.Name)"
  try {
    $frame = "$($file.FullName)[0]"
    $opaqueText = (& $magickExe identify -format "%[opaque]" $frame 2>&1 |
      Out-String).Trim()
    if ($LASTEXITCODE -ne 0) {
      throw "alpha check failed: $opaqueText"
    }
    $colorType = if ($opaqueText -eq "True") {
      2
    } else {
      6
    }
    $message = (& $magickExe $frame -auto-orient -strip -depth 8 `
      -define "png:color-type=$colorType" $candidate 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $candidate)) {
      throw "normalization failed: $message"
    }
    $identity = (& $magickExe identify -format "%w,%h" $candidate 2>&1 |
      Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $identity -notmatch "^(\d+),(\d+)$") {
      throw "identity check failed: $identity"
    }
    $width = [int]$Matches[1]
    $height = [int]$Matches[2]
    $pixels = [int64]$width * $height
    if ($width -gt 16383 -or $height -gt 16383) {
      throw "outside the shared WebP dimension limit"
    }
    if ($pixels -gt $MaximumPixels) {
      throw "exceeds the $MaximumPixels pixel corpus ceiling"
    }
    $hash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).
      Hash.ToLowerInvariant()
    if (!$seen.Add($hash)) {
      $duplicates++
      Remove-Item -LiteralPath $candidate -Force
      continue
    }
    $categoryDirectory = Join-Path $corpus $category
    New-Item -ItemType Directory -Force -Path $categoryDirectory | Out-Null
    $safeStem = [regex]::Replace(
      [IO.Path]::GetFileNameWithoutExtension($file.Name),
      "[^A-Za-z0-9._]+",
      "_"
    ).Trim("_")
    if (!$safeStem) {
      $safeStem = "image"
    }
    $name = "{0:D6}_{1}_{2}.png" -f (
      $rows.Count + 1
    ), $safeStem, $hash.Substring(0, 12)
    $destination = Join-Path $categoryDirectory $name
    Move-Item -LiteralPath $candidate -Destination $destination
    $relativeOutput = [IO.Path]::GetRelativePath($corpus, $destination).
      Replace("\", "/")
    $rows.Add([pscustomobject][ordered]@{
      Path = $relativeOutput
      Category = $category
      Width = $width
      Height = $height
      Pixels = $pixels
      ColorModel = if ($colorType -eq 2) { "RGB" } else { "RGBA" }
      NormalizedSHA256 = $hash
    })
    $totalPixels += $pixels
  } catch {
    Remove-Item -LiteralPath $candidate -Force -ErrorAction SilentlyContinue
    $excluded.Add([pscustomobject][ordered]@{
      Category = $category
      Source = Portable-SourcePath $file
      Reason = $_.Exception.Message
    })
  }
}

Remove-Item -LiteralPath $staging -Force
$rows | Export-Csv -LiteralPath (Join-Path $run "corpus-manifest.csv") `
  -NoTypeInformation -Encoding utf8
$excluded | Export-Csv -LiteralPath (Join-Path $run "corpus-excluded.csv") `
  -NoTypeInformation -Encoding utf8

$sourceSummary = @($rows | Group-Object Category | ForEach-Object {
  [pscustomobject][ordered]@{
    Category = $_.Name
    Images = $_.Count
    Pixels = [int64](($_.Group | Measure-Object Pixels -Sum).Sum)
  }
})
$summary = [pscustomobject][ordered]@{
  TimestampUtc = [DateTime]::UtcNow.ToString("o")
  CandidateFiles = $ordered.Count
  IncludedImages = $rows.Count
  DuplicateNormalizedImages = $duplicates
  ExcludedFiles = $excluded.Count
  Pixels = $totalPixels
  MaximumPixelsPerImage = $MaximumPixels
  Normalization = "first frame, auto-oriented, metadata stripped, 8-bit RGB or RGBA PNG"
  SharedDimensionLimit = 16383
  EnricoSelection = "first 10 numeric screen IDs in each of 20 design topics"
  Categories = $sourceSummary
}
$summary | ConvertTo-Json -Depth 5 |
  Set-Content -LiteralPath (Join-Path $run "corpus-summary.json") -Encoding utf8
$summary | Format-List
