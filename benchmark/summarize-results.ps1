param(
  [string]$Results = "",
  [string]$Output = "",
  [string]$Corpus = ""
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$resultsPath = if ($Results) {
  (Resolve-Path -LiteralPath $Results).Path
} else {
  (Resolve-Path -LiteralPath (Join-Path $root "run\results.csv")).Path
}
$outputPath = if ($Output) {
  [IO.Path]::GetFullPath($Output)
} else {
  Split-Path -Parent $resultsPath
}
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$rows = @(Import-Csv -LiteralPath $resultsPath)
if (!$rows.Count) {
  throw "The results file is empty"
}
$corpusPath = if ($Corpus) {
  (Resolve-Path -LiteralPath $Corpus).Path
} elseif (Test-Path -LiteralPath (Join-Path $root "corpus")) {
  (Resolve-Path -LiteralPath (Join-Path $root "corpus")).Path
} else {
  ""
}

function Png-ColorModel([string]$Path) {
  $stream = [IO.File]::OpenRead($Path)
  try {
    $stream.Position = 25
    $colorType = $stream.ReadByte()
  } finally {
    $stream.Dispose()
  }
  if ($colorType -eq 2) {
    return "RGB"
  }
  if ($colorType -eq 6) {
    return "RGBA"
  }
  throw "Unexpected normalized PNG color type $colorType in $Path"
}

foreach ($row in $rows) {
  if (!$row.PSObject.Properties["ColorModel"] -or !$row.ColorModel) {
    if (!$corpusPath) {
      $model = "unknown"
    } else {
      $model = Png-ColorModel (Join-Path $corpusPath $row.Path)
    }
    $row | Add-Member -NotePropertyName ColorModel -NotePropertyValue $model `
      -Force
  }
}

function Sum([object[]]$Items, [string]$Name) {
  return [double](($Items | Measure-Object $Name -Sum).Sum)
}

function Maximum([object[]]$Items, [string]$Name) {
  return [double](($Items | Measure-Object $Name -Maximum).Maximum)
}

function Median([object[]]$Items, [string]$Name) {
  $values = @($Items | ForEach-Object {
    [double]$_.$Name
  } | Sort-Object)
  $middle = [int][math]::Floor($values.Count / 2)
  if (($values.Count -band 1) -ne 0) {
    return $values[$middle]
  }
  return ($values[$middle - 1] + $values[$middle]) / 2.0
}

function Percent([double]$Value, [double]$Reference) {
  if ($Reference -eq 0) {
    return 0
  }
  return [math]::Round(100.0 * ($Value - $Reference) / $Reference, 3)
}

function Ratio([double]$Value, [double]$Reference) {
  if ($Reference -eq 0) {
    return 0
  }
  return [math]::Round($Value / $Reference, 3)
}

function Codec([object[]]$Items, [string]$Prefix) {
  $seconds = Sum $Items "${Prefix}EncodeSec"
  $pixels = Sum $Items "Pixels"
  return [pscustomobject][ordered]@{
    Bytes = [int64](Sum $Items "${Prefix}Bytes")
    EncodeSeconds = [math]::Round($seconds, 6)
    CpuSeconds = [math]::Round((Sum $Items "${Prefix}CpuSec"), 6)
    MegapixelsPerSecond = if ($seconds) {
      [math]::Round(($pixels / 1000000.0) / $seconds, 3)
    } else {
      0
    }
    PeakWorkingSetMaximumBytes = [int64](
      Maximum $Items "${Prefix}PeakWorkingSetBytes")
    PeakWorkingSetMedianBytes = [int64](
      Median $Items "${Prefix}PeakWorkingSetBytes")
    PeakPrivateMaximumBytes = [int64](
      Maximum $Items "${Prefix}PeakPrivateBytes")
    PeakPrivateMedianBytes = [int64](
      Median $Items "${Prefix}PeakPrivateBytes")
  }
}

function Category([object]$Group) {
  $items = @($Group.Group)
  $qlic = Codec $items "Qlic"
  $webp = Codec $items "WebP"
  $jxl = Codec $items "Jxl"
  return [pscustomobject][ordered]@{
    Category = $Group.Name
    Images = $items.Count
    Megapixels = [math]::Round((Sum $items "Pixels") / 1000000.0, 3)
    QlicBytes = $qlic.Bytes
    WebPBytes = $webp.Bytes
    JxlBytes = $jxl.Bytes
    QlicVsWebPSizePercent = Percent $qlic.Bytes $webp.Bytes
    QlicVsJxlSizePercent = Percent $qlic.Bytes $jxl.Bytes
    QlicSeconds = $qlic.EncodeSeconds
    WebPSeconds = $webp.EncodeSeconds
    JxlSeconds = $jxl.EncodeSeconds
    QlicVsWebPSpeedPercent = if ($webp.EncodeSeconds) {
      [math]::Round(
        100.0 * ($webp.EncodeSeconds - $qlic.EncodeSeconds) /
        $webp.EncodeSeconds,
        3
      )
    } else {
      0
    }
    JxlToQlicTimeRatio = Ratio $jxl.EncodeSeconds $qlic.EncodeSeconds
    QlicPeakWorkingSetMiB = [math]::Round(
      $qlic.PeakWorkingSetMaximumBytes / 1MB, 1)
    WebPPeakWorkingSetMiB = [math]::Round(
      $webp.PeakWorkingSetMaximumBytes / 1MB, 1)
    JxlPeakWorkingSetMiB = [math]::Round(
      $jxl.PeakWorkingSetMaximumBytes / 1MB, 1)
    QlicJxlSizeWins = @($items | Where-Object {
      [int64]$_.QlicBytes -lt [int64]$_.JxlBytes
    }).Count
    QlicWebPSizeWins = @($items | Where-Object {
      [int64]$_.QlicBytes -lt [int64]$_.WebPBytes
    }).Count
    ExactRoundTrips = @($items | Where-Object {
      [int64]$_.QlicDifference -eq 0 -and
      [int64]$_.WebPDifference -eq 0 -and
      [int64]$_.JxlDifference -eq 0
    }).Count
  }
}

$categories = @($rows | Group-Object Category | ForEach-Object {
  Category $_
} | Sort-Object Category)
$colorModels = @($rows | Group-Object ColorModel | ForEach-Object {
  $comparison = Category $_
  [pscustomobject][ordered]@{
    ColorModel = $_.Name
    Images = $comparison.Images
    Megapixels = $comparison.Megapixels
    QlicVsWebPSizePercent = $comparison.QlicVsWebPSizePercent
    QlicVsJxlSizePercent = $comparison.QlicVsJxlSizePercent
    QlicVsWebPSpeedPercent = $comparison.QlicVsWebPSpeedPercent
    JxlToQlicTimeRatio = $comparison.JxlToQlicTimeRatio
    QlicPeakWorkingSetMiB = $comparison.QlicPeakWorkingSetMiB
    WebPPeakWorkingSetMiB = $comparison.WebPPeakWorkingSetMiB
    JxlPeakWorkingSetMiB = $comparison.JxlPeakWorkingSetMiB
  }
} | Sort-Object ColorModel)
$qlicTotal = Codec $rows "Qlic"
$webpTotal = Codec $rows "WebP"
$jxlTotal = Codec $rows "Jxl"
$summary = [pscustomobject][ordered]@{
  TimestampUtc = [DateTime]::UtcNow.ToString("o")
  Results = $resultsPath
  Images = $rows.Count
  Categories = $categories.Count
  Pixels = [int64](Sum $rows "Pixels")
  ExactRoundTrips = @($rows | Where-Object {
    [int64]$_.QlicDifference -eq 0 -and
    [int64]$_.WebPDifference -eq 0 -and
    [int64]$_.JxlDifference -eq 0
  }).Count
  Qlic = $qlicTotal
  WebP = $webpTotal
  Jxl = $jxlTotal
  QlicVsWebPSizePercent = Percent $qlicTotal.Bytes $webpTotal.Bytes
  QlicVsJxlSizePercent = Percent $qlicTotal.Bytes $jxlTotal.Bytes
  QlicVsWebPSpeedPercent = if ($webpTotal.EncodeSeconds) {
    [math]::Round(
      100.0 * ($webpTotal.EncodeSeconds - $qlicTotal.EncodeSeconds) /
      $webpTotal.EncodeSeconds,
      3
    )
  } else {
    0
  }
  JxlToQlicTimeRatio = Ratio $jxlTotal.EncodeSeconds $qlicTotal.EncodeSeconds
  QlicJxlSizeWins = @($rows | Where-Object {
    [int64]$_.QlicBytes -lt [int64]$_.JxlBytes
  }).Count
  QlicWebPSizeWins = @($rows | Where-Object {
    [int64]$_.QlicBytes -lt [int64]$_.WebPBytes
  }).Count
  EqualCategoryMeanQlicVsJxlPercent = [math]::Round(
    ($categories | Measure-Object QlicVsJxlSizePercent -Average).Average,
    3
  )
  EqualCategoryMeanQlicVsWebPPercent = [math]::Round(
    ($categories | Measure-Object QlicVsWebPSizePercent -Average).Average,
    3
  )
  ColorModels = $colorModels
}

$categories | Export-Csv -LiteralPath (
  Join-Path $outputPath "analysis-categories.csv"
) -NoTypeInformation -Encoding utf8
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (
  Join-Path $outputPath "analysis-summary.json"
) -Encoding utf8

$lines = [Collections.Generic.List[string]]::new()
$lines.Add("# Large lossless benchmark")
$lines.Add("")
$lines.Add("Images: $($summary.Images)")
$lines.Add("")
$lines.Add("Pixels: $($summary.Pixels)")
$lines.Add("")
$lines.Add("Exact round trips: $($summary.ExactRoundTrips) of $($summary.Images)")
$lines.Add("")
$lines.Add(
  "QLIC size against WebP 6: $($summary.QlicVsWebPSizePercent) percent"
)
$lines.Add("")
$lines.Add(
  "QLIC size against JPEG XL 9: $($summary.QlicVsJxlSizePercent) percent"
)
$lines.Add("")
$lines.Add(
  "QLIC encode speed against WebP 6: $($summary.QlicVsWebPSpeedPercent) percent"
)
$lines.Add("")
$lines.Add(
  "JPEG XL 9 to QLIC encode time: $($summary.JxlToQlicTimeRatio) times"
)
$lines.Add("")
$lines.Add("| Color model | Images | MP | QLIC vs WebP size | QLIC vs JXL size | QLIC vs WebP speed | JXL time divided by QLIC |")
$lines.Add("| --- | ---: | ---: | ---: | ---: | ---: | ---: |")
foreach ($model in $colorModels) {
  $lines.Add(
    "| $($model.ColorModel) | $($model.Images) | " +
    "$($model.Megapixels) | $($model.QlicVsWebPSizePercent)% | " +
    "$($model.QlicVsJxlSizePercent)% | " +
    "$($model.QlicVsWebPSpeedPercent)% | " +
    "$($model.JxlToQlicTimeRatio)x |"
  )
}
$lines.Add("")
$lines.Add("| Category | Images | MP | QLIC vs WebP size | QLIC vs JXL size | QLIC vs WebP speed | JXL time divided by QLIC |")
$lines.Add("| --- | ---: | ---: | ---: | ---: | ---: | ---: |")
foreach ($category in $categories) {
  $lines.Add(
    "| $($category.Category) | $($category.Images) | " +
    "$($category.Megapixels) | $($category.QlicVsWebPSizePercent)% | " +
    "$($category.QlicVsJxlSizePercent)% | " +
    "$($category.QlicVsWebPSpeedPercent)% | " +
    "$($category.JxlToQlicTimeRatio)x |"
  )
}
$lines | Set-Content -LiteralPath (
  Join-Path $outputPath "REPORT.md"
) -Encoding utf8
$summary | Format-List
