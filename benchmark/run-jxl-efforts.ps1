param(
  [string]$Corpus = "",
  [string]$Output = "",
  [string]$Qlic = "",
  [string]$Cjxl = "",
  [string]$Djxl = "",
  [string]$Magick = "magick.exe",
  [ValidateRange(1, 20)]
  [int]$DecodeRuns = 3,
  [ValidateRange(0, 1000000)]
  [int]$MaximumImages = 0,
  [ValidateRange(0, 62)]
  [int]$Processor = 0
)

$ErrorActionPreference = "Stop"
$run = $PSScriptRoot
$repo = Split-Path -Parent $run
$codecNames = @("qlic", "jxl6", "jxl7", "jxl8")

function Resolve-Tool([string]$Value, [string]$Fallback, [string]$Label) {
  $candidate = if ($Value) { $Value } else { $Fallback }
  if (Test-Path -LiteralPath $candidate) {
    return (Resolve-Path -LiteralPath $candidate).Path
  }
  $command = Get-Command $candidate -CommandType Application `
    -ErrorAction SilentlyContinue
  if ($command) {
    return $command.Source
  }
  throw "$Label was not found: $candidate"
}

function Invoke-Pinned([string]$Exe, [string[]]$Arguments) {
  $start = [Diagnostics.ProcessStartInfo]::new()
  $start.FileName = $Exe
  foreach ($argument in $Arguments) {
    [void]$start.ArgumentList.Add($argument)
  }
  $start.UseShellExecute = $false
  $start.CreateNoWindow = $true
  $start.RedirectStandardOutput = $true
  $start.RedirectStandardError = $true
  $process = [Diagnostics.Process]::new()
  $process.StartInfo = $start
  $clock = [Diagnostics.Stopwatch]::StartNew()
  if (!$process.Start()) {
    throw "Could not start $Exe"
  }
  try {
    $process.ProcessorAffinity = [IntPtr]([int64]1 -shl $Processor)
  } catch {
    if (!$process.HasExited) {
      $process.Kill()
    }
    throw
  }
  $stdoutTask = $process.StandardOutput.ReadToEndAsync()
  $stderrTask = $process.StandardError.ReadToEndAsync()
  $peakWorkingSet = [int64]0
  $peakPrivate = [int64]0
  do {
    try {
      $process.Refresh()
      $peakWorkingSet = [math]::Max(
        $peakWorkingSet,
        [int64]$process.PeakWorkingSet64
      )
      $peakPrivate = [math]::Max(
        $peakPrivate,
        [int64]$process.PrivateMemorySize64
      )
    } catch {
    }
    $finished = $process.WaitForExit(5)
  } while (!$finished)
  $clock.Stop()
  $process.Refresh()
  $result = [pscustomobject][ordered]@{
    ExitCode = $process.ExitCode
    Seconds = $clock.Elapsed.TotalSeconds
    CpuSeconds = $process.TotalProcessorTime.TotalSeconds
    PeakWorkingSetBytes = $peakWorkingSet
    PeakPrivateBytes = $peakPrivate
    Stdout = $stdoutTask.Result
    Stderr = $stderrTask.Result
  }
  $process.Dispose()
  return $result
}

function Invoke-Checked([string]$Exe, [string[]]$Arguments) {
  $result = Invoke-Pinned $Exe $Arguments
  if ($result.ExitCode -ne 0) {
    throw "$Exe failed with exit $($result.ExitCode): $($result.Stdout)$($result.Stderr)"
  }
  return $result
}

function Tool-Version([string]$Exe, [string[]]$Arguments) {
  $result = Invoke-Checked $Exe $Arguments
  return (($result.Stdout + $result.Stderr) -split "\r?\n" |
    Where-Object { $_ } | Select-Object -First 1).Trim()
}

function Pixel-Difference([string]$A, [string]$B) {
  $old = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    $text = (& $magickExe compare -metric AE $A $B "null:" 2>&1 |
      Out-String).Trim()
    $exitCode = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $old
  }
  if ($text -match "^\d+") {
    return [int64]$Matches[0]
  }
  if ($exitCode -eq 0) {
    return 0
  }
  throw "Image comparison failed: $text"
}

function Rotated([string[]]$Items, [int]$Offset) {
  for ($i = 0; $i -lt $Items.Count; $i++) {
    $Items[($i + $Offset) % $Items.Count]
  }
}

function Average-Runs([object[]]$Runs) {
  return [pscustomobject][ordered]@{
    Seconds = [math]::Round(
      ($Runs | Measure-Object Seconds -Average).Average, 6)
    CpuSeconds = [math]::Round(
      ($Runs | Measure-Object CpuSeconds -Average).Average, 6)
    PeakWorkingSetBytes = [int64](
      ($Runs | Measure-Object PeakWorkingSetBytes -Maximum).Maximum)
    PeakPrivateBytes = [int64](
      ($Runs | Measure-Object PeakPrivateBytes -Maximum).Maximum)
  }
}

function Sum-Field([object[]]$Rows, [string]$Name) {
  return [double](($Rows | ForEach-Object {
    [double]($_.$Name)
  } | Measure-Object -Sum).Sum)
}

function Maximum-Field([object[]]$Rows, [string]$Name) {
  return [int64](($Rows | ForEach-Object {
    [int64]($_.$Name)
  } | Measure-Object -Maximum).Maximum)
}

function Codec-Summary(
  [object[]]$Rows,
  [string]$Prefix
) {
  return [pscustomobject][ordered]@{
    Bytes = [int64](Sum-Field $Rows "${Prefix}Bytes")
    EncodeSeconds = [math]::Round(
      (Sum-Field $Rows "${Prefix}EncodeSec"), 6)
    EncodeCpuSeconds = [math]::Round(
      (Sum-Field $Rows "${Prefix}EncodeCpuSec"), 6)
    DecodeSeconds = [math]::Round(
      (Sum-Field $Rows "${Prefix}DecodeSec"), 6)
    DecodeCpuSeconds = [math]::Round(
      (Sum-Field $Rows "${Prefix}DecodeCpuSec"), 6)
    EncodePeakWorkingSetMaximum = Maximum-Field $Rows `
      "${Prefix}EncodePeakWorkingSetBytes"
    DecodePeakWorkingSetMaximum = Maximum-Field $Rows `
      "${Prefix}DecodePeakWorkingSetBytes"
    EncodePeakPrivateMaximum = Maximum-Field $Rows `
      "${Prefix}EncodePeakPrivateBytes"
    DecodePeakPrivateMaximum = Maximum-Field $Rows `
      "${Prefix}DecodePeakPrivateBytes"
  }
}

function Encode-Arguments(
  [string]$Codec,
  [string]$Source,
  [string]$Destination
) {
  if ($Codec -eq "qlic") {
    return @("pack", $Source, $Destination, "--threads", "1")
  }
  $effort = $Codec.Substring(3)
  return @(
    $Source, $Destination, "-d", "0", "-e", $effort,
    "--num_threads=0"
  )
}

function Decode-Arguments(
  [string]$Codec,
  [string]$Source,
  [string]$Destination
) {
  if ($Codec -eq "qlic") {
    return @("unpack", $Source, $Destination, "--threads", "1")
  }
  return @($Source, $Destination, "--num_threads=0")
}

function Codec-Executable([string]$Codec, [switch]$Decode) {
  if ($Codec -eq "qlic") {
    return $qlicExe
  }
  if ($Decode) {
    return $djxlExe
  }
  return $cjxlExe
}

$corpusPath = if ($Corpus) {
  (Resolve-Path -LiteralPath $Corpus).Path
} else {
  (Resolve-Path -LiteralPath (Join-Path $run "corpus")).Path
}
$outputPath = if ($Output) {
  [IO.Path]::GetFullPath($Output)
} else {
  Join-Path $run "run-jxl-efforts"
}

$qlicValue = if ($Qlic) {
  $Qlic
} else {
  $existing = Get-ChildItem (Join-Path $repo "build\benchmark-jxl-efforts") `
    -Filter qlic.exe -File -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.Directory.Name -eq "Release" } |
    Select-Object -First 1
  if (!$existing) {
    & (Join-Path $repo "build.ps1") -Config Release `
      -BuildDir "build\benchmark-jxl-efforts"
    if ($LASTEXITCODE -ne 0) {
      throw "QLIC build failed"
    }
    $existing = Get-ChildItem (
      Join-Path $repo "build\benchmark-jxl-efforts"
    ) -Filter qlic.exe -File -Recurse |
      Where-Object { $_.Directory.Name -eq "Release" } |
      Select-Object -First 1
  }
  $existing.FullName
}

$qlicExe = Resolve-Tool $qlicValue $qlicValue "QLIC"
$cjxlExe = Resolve-Tool $Cjxl "cjxl.exe" "cjxl"
$djxlExe = Resolve-Tool $Djxl "djxl.exe" "djxl"
$magickExe = Resolve-Tool $Magick $Magick "ImageMagick"

$manifestPath = Join-Path $run "corpus-manifest.csv"
$manifest = @(Import-Csv -LiteralPath $manifestPath | Sort-Object Path)
if (!$manifest.Count) {
  throw "The corpus manifest is empty"
}
if ($MaximumImages -gt 0) {
  $manifest = @($manifest | Select-Object -First $MaximumImages)
}
$files = foreach ($item in $manifest) {
  $path = Join-Path $corpusPath $item.Path
  if (!(Test-Path -LiteralPath $path)) {
    throw "Corpus file is missing: $path"
  }
  [pscustomobject]@{
    Manifest = $item
    File = Get-Item -LiteralPath $path
  }
}

$configuration = [pscustomobject][ordered]@{
  Corpus = $corpusPath
  ManifestSHA256 = (Get-FileHash -LiteralPath $manifestPath `
      -Algorithm SHA256).Hash.ToLowerInvariant()
  Images = $files.Count
  Processor = $Processor
  DecodeRuns = $DecodeRuns
  QlicPath = $qlicExe
  QlicSHA256 = (Get-FileHash -LiteralPath $qlicExe `
      -Algorithm SHA256).Hash.ToLowerInvariant()
  QlicVersion = Tool-Version $qlicExe @("version")
  JxlPath = $cjxlExe
  JxlSHA256 = (Get-FileHash -LiteralPath $cjxlExe `
      -Algorithm SHA256).Hash.ToLowerInvariant()
  JxlVersion = Tool-Version $cjxlExe @("--version")
  QlicSettings = "pack --threads 1"
  Jxl6Settings = "cjxl -d 0 -e 6 --num_threads=0"
  Jxl7Settings = "cjxl -d 0 -e 7 --num_threads=0"
  Jxl8Settings = "cjxl -d 0 -e 8 --num_threads=0"
  QlicDecodeSettings = "unpack --threads 1"
  JxlDecodeSettings = "djxl --num_threads=0"
  Timing = "wall time per process including startup and file IO"
  DecodeMeasurement = "$DecodeRuns measured runs after one untimed run per image"
}

$configurationPath = Join-Path $outputPath "configuration.json"
$resultsPath = Join-Path $outputPath "results.csv"
if (Test-Path -LiteralPath $outputPath) {
  if (!(Test-Path -LiteralPath $configurationPath)) {
    throw "Output exists without a run configuration: $outputPath"
  }
  $previous = Get-Content -Raw -LiteralPath $configurationPath |
    ConvertFrom-Json
  foreach ($name in @(
    "Corpus", "ManifestSHA256", "Images", "Processor", "DecodeRuns",
    "QlicSHA256", "JxlSHA256"
  )) {
    if ([string]$previous.$name -ne [string]$configuration.$name) {
      throw "Existing run configuration differs at $name"
    }
  }
} else {
  New-Item -ItemType Directory -Path $outputPath | Out-Null
  $configuration | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath $configurationPath -Encoding utf8
}

$rows = [Collections.Generic.List[object]]::new()
$completed = [Collections.Generic.HashSet[string]]::new(
  [StringComparer]::Ordinal
)
if (Test-Path -LiteralPath $resultsPath) {
  foreach ($row in @(Import-Csv -LiteralPath $resultsPath)) {
    $rows.Add($row)
    [void]$completed.Add([string]$row.Path)
  }
}
$pending = @($files | Where-Object {
  !$completed.Contains([string]$_.Manifest.Path)
})

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$work = Join-Path $tempRoot (
  "qlic-jxl-efforts-" + [guid]::NewGuid().ToString("N")
)
New-Item -ItemType Directory -Path $work | Out-Null

try {
  if ($pending.Count) {
    $warm = $pending[0].File.FullName
    foreach ($codec in $codecNames) {
      $encoded = Join-Path $work "warm-$codec.bin"
      $decoded = Join-Path $work "warm-$codec.png"
      $encoder = Codec-Executable $codec
      Invoke-Checked $encoder (
        Encode-Arguments $codec $warm $encoded
      ) | Out-Null
      $decoder = Codec-Executable $codec -Decode
      Invoke-Checked $decoder (
        Decode-Arguments $codec $encoded $decoded
      ) | Out-Null
    }
  }

  for ($index = 0; $index -lt $files.Count; $index++) {
    $entry = $files[$index]
    $item = $entry.Manifest
    if ($completed.Contains([string]$item.Path)) {
      continue
    }
    $file = $entry.File
    $prefix = "{0:D6}" -f ($index + 1)
    Write-Host "[$($index + 1)/$($files.Count)] $($item.Path)"

    $encodedPaths = @{
      qlic = Join-Path $work "$prefix.qlic"
      jxl6 = Join-Path $work "$prefix-e6.jxl"
      jxl7 = Join-Path $work "$prefix-e7.jxl"
      jxl8 = Join-Path $work "$prefix-e8.jxl"
    }
    $decodedPaths = @{
      qlic = Join-Path $work "$prefix-qlic.png"
      jxl6 = Join-Path $work "$prefix-e6.png"
      jxl7 = Join-Path $work "$prefix-e7.png"
      jxl8 = Join-Path $work "$prefix-e8.png"
    }
    $encodeRuns = @{}
    foreach ($codec in @(Rotated $codecNames ($index % 4))) {
      $encodeRuns[$codec] = Invoke-Checked (
        Codec-Executable $codec
      ) (Encode-Arguments $codec $file.FullName $encodedPaths[$codec])
    }

    foreach ($codec in @(Rotated $codecNames (($index + 1) % 4))) {
      Remove-Item -LiteralPath $decodedPaths[$codec] -Force `
        -ErrorAction SilentlyContinue
      Invoke-Checked (
        Codec-Executable $codec -Decode
      ) (Decode-Arguments $codec $encodedPaths[$codec] `
          $decodedPaths[$codec]) | Out-Null
    }

    $decodeSamples = @{}
    foreach ($codec in $codecNames) {
      $decodeSamples[$codec] = [Collections.Generic.List[object]]::new()
    }
    for ($sample = 0; $sample -lt $DecodeRuns; $sample++) {
      $offset = ($index + $sample + 2) % 4
      foreach ($codec in @(Rotated $codecNames $offset)) {
        Remove-Item -LiteralPath $decodedPaths[$codec] -Force `
          -ErrorAction SilentlyContinue
        $decodeSamples[$codec].Add((Invoke-Checked (
          Codec-Executable $codec -Decode
        ) (Decode-Arguments $codec $encodedPaths[$codec] `
            $decodedPaths[$codec])))
      }
    }

    $differences = @{}
    foreach ($codec in $codecNames) {
      $differences[$codec] = Pixel-Difference $file.FullName `
        $decodedPaths[$codec]
      if ($differences[$codec] -ne 0) {
        throw "Exact pixel verification failed for $($item.Path), $codec"
      }
    }
    $decodeAverages = @{}
    foreach ($codec in $codecNames) {
      $decodeAverages[$codec] = Average-Runs `
        $decodeSamples[$codec].ToArray()
    }

    $row = [pscustomobject][ordered]@{
      Path = $item.Path
      Category = $item.Category
      Width = [int]$item.Width
      Height = [int]$item.Height
      Pixels = [int64]$item.Pixels
      ColorModel = [string]$item.ColorModel
      SHA256 = $item.NormalizedSHA256
      QlicBytes = (Get-Item -LiteralPath $encodedPaths.qlic).Length
      Jxl6Bytes = (Get-Item -LiteralPath $encodedPaths.jxl6).Length
      Jxl7Bytes = (Get-Item -LiteralPath $encodedPaths.jxl7).Length
      Jxl8Bytes = (Get-Item -LiteralPath $encodedPaths.jxl8).Length
      QlicEncodeSec = [math]::Round($encodeRuns.qlic.Seconds, 6)
      Jxl6EncodeSec = [math]::Round($encodeRuns.jxl6.Seconds, 6)
      Jxl7EncodeSec = [math]::Round($encodeRuns.jxl7.Seconds, 6)
      Jxl8EncodeSec = [math]::Round($encodeRuns.jxl8.Seconds, 6)
      QlicEncodeCpuSec = [math]::Round($encodeRuns.qlic.CpuSeconds, 6)
      Jxl6EncodeCpuSec = [math]::Round($encodeRuns.jxl6.CpuSeconds, 6)
      Jxl7EncodeCpuSec = [math]::Round($encodeRuns.jxl7.CpuSeconds, 6)
      Jxl8EncodeCpuSec = [math]::Round($encodeRuns.jxl8.CpuSeconds, 6)
      QlicEncodePeakWorkingSetBytes = $encodeRuns.qlic.PeakWorkingSetBytes
      Jxl6EncodePeakWorkingSetBytes = $encodeRuns.jxl6.PeakWorkingSetBytes
      Jxl7EncodePeakWorkingSetBytes = $encodeRuns.jxl7.PeakWorkingSetBytes
      Jxl8EncodePeakWorkingSetBytes = $encodeRuns.jxl8.PeakWorkingSetBytes
      QlicEncodePeakPrivateBytes = $encodeRuns.qlic.PeakPrivateBytes
      Jxl6EncodePeakPrivateBytes = $encodeRuns.jxl6.PeakPrivateBytes
      Jxl7EncodePeakPrivateBytes = $encodeRuns.jxl7.PeakPrivateBytes
      Jxl8EncodePeakPrivateBytes = $encodeRuns.jxl8.PeakPrivateBytes
      QlicDecodeSec = $decodeAverages.qlic.Seconds
      Jxl6DecodeSec = $decodeAverages.jxl6.Seconds
      Jxl7DecodeSec = $decodeAverages.jxl7.Seconds
      Jxl8DecodeSec = $decodeAverages.jxl8.Seconds
      QlicDecodeCpuSec = $decodeAverages.qlic.CpuSeconds
      Jxl6DecodeCpuSec = $decodeAverages.jxl6.CpuSeconds
      Jxl7DecodeCpuSec = $decodeAverages.jxl7.CpuSeconds
      Jxl8DecodeCpuSec = $decodeAverages.jxl8.CpuSeconds
      QlicDecodePeakWorkingSetBytes = `
        $decodeAverages.qlic.PeakWorkingSetBytes
      Jxl6DecodePeakWorkingSetBytes = `
        $decodeAverages.jxl6.PeakWorkingSetBytes
      Jxl7DecodePeakWorkingSetBytes = `
        $decodeAverages.jxl7.PeakWorkingSetBytes
      Jxl8DecodePeakWorkingSetBytes = `
        $decodeAverages.jxl8.PeakWorkingSetBytes
      QlicDecodePeakPrivateBytes = $decodeAverages.qlic.PeakPrivateBytes
      Jxl6DecodePeakPrivateBytes = $decodeAverages.jxl6.PeakPrivateBytes
      Jxl7DecodePeakPrivateBytes = $decodeAverages.jxl7.PeakPrivateBytes
      Jxl8DecodePeakPrivateBytes = $decodeAverages.jxl8.PeakPrivateBytes
      QlicDifference = $differences.qlic
      Jxl6Difference = $differences.jxl6
      Jxl7Difference = $differences.jxl7
      Jxl8Difference = $differences.jxl8
    }
    $rows.Add($row)
    if (!(Test-Path -LiteralPath $resultsPath)) {
      $row | Export-Csv -LiteralPath $resultsPath -NoTypeInformation `
        -Encoding utf8
    } else {
      $row | Export-Csv -LiteralPath $resultsPath -NoTypeInformation `
        -Encoding utf8 -Append
    }
    foreach ($path in @($encodedPaths.Values) + @($decodedPaths.Values)) {
      Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    }
  }

  $allRows = $rows.ToArray()
  if ($allRows.Count -ne $files.Count) {
    throw "The run ended with $($allRows.Count) of $($files.Count) rows"
  }
  $qlicSummary = Codec-Summary $allRows "Qlic"
  $jxl6Summary = Codec-Summary $allRows "Jxl6"
  $jxl7Summary = Codec-Summary $allRows "Jxl7"
  $jxl8Summary = Codec-Summary $allRows "Jxl8"
  try {
    $cpu = (Get-CimInstance Win32_Processor -ErrorAction Stop |
      Select-Object -First 1 -ExpandProperty Name).Trim()
  } catch {
    $cpu = [string]$env:PROCESSOR_IDENTIFIER
  }

  $summary = [pscustomobject][ordered]@{
    TimestampUtc = [DateTime]::UtcNow.ToString("o")
    Corpus = $corpusPath
    Images = $allRows.Count
    Pixels = [int64](Sum-Field $allRows "Pixels")
    ExactRoundTrips = @($allRows | Where-Object {
      [int64]$_.QlicDifference -eq 0 -and
      [int64]$_.Jxl6Difference -eq 0 -and
      [int64]$_.Jxl7Difference -eq 0 -and
      [int64]$_.Jxl8Difference -eq 0
    }).Count
    Cpu = $cpu
    LogicalProcessor = $Processor
    DecodeRuns = $DecodeRuns
    QlicVersion = $configuration.QlicVersion
    QlicSHA256 = $configuration.QlicSHA256
    JxlVersion = $configuration.JxlVersion
    JxlSHA256 = $configuration.JxlSHA256
    Qlic = $qlicSummary
    Jxl6 = $jxl6Summary
    Jxl7 = $jxl7Summary
    Jxl8 = $jxl8Summary
    QlicVsJxl6SizePercent = [math]::Round(
      100.0 * ($qlicSummary.Bytes - $jxl6Summary.Bytes) /
      $jxl6Summary.Bytes, 3)
    QlicVsJxl7SizePercent = [math]::Round(
      100.0 * ($qlicSummary.Bytes - $jxl7Summary.Bytes) /
      $jxl7Summary.Bytes, 3)
    QlicVsJxl8SizePercent = [math]::Round(
      100.0 * ($qlicSummary.Bytes - $jxl8Summary.Bytes) /
      $jxl8Summary.Bytes, 3)
    QlicJxl6SizeWins = @($allRows | Where-Object {
      [int64]$_.QlicBytes -lt [int64]$_.Jxl6Bytes
    }).Count
    QlicJxl7SizeWins = @($allRows | Where-Object {
      [int64]$_.QlicBytes -lt [int64]$_.Jxl7Bytes
    }).Count
    QlicJxl8SizeWins = @($allRows | Where-Object {
      [int64]$_.QlicBytes -lt [int64]$_.Jxl8Bytes
    }).Count
    Jxl6EncodeTimeOverQlic = [math]::Round(
      $jxl6Summary.EncodeSeconds / $qlicSummary.EncodeSeconds, 3)
    Jxl7EncodeTimeOverQlic = [math]::Round(
      $jxl7Summary.EncodeSeconds / $qlicSummary.EncodeSeconds, 3)
    Jxl8EncodeTimeOverQlic = [math]::Round(
      $jxl8Summary.EncodeSeconds / $qlicSummary.EncodeSeconds, 3)
    Jxl6DecodeTimeOverQlic = [math]::Round(
      $jxl6Summary.DecodeSeconds / $qlicSummary.DecodeSeconds, 3)
    Jxl7DecodeTimeOverQlic = [math]::Round(
      $jxl7Summary.DecodeSeconds / $qlicSummary.DecodeSeconds, 3)
    Jxl8DecodeTimeOverQlic = [math]::Round(
      $jxl8Summary.DecodeSeconds / $qlicSummary.DecodeSeconds, 3)
  }
  $summary | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $outputPath "summary.json") `
      -Encoding utf8

  $categoryRows = @($allRows | Group-Object Category | ForEach-Object {
    $group = @($_.Group)
    $qlicCategory = Codec-Summary $group "Qlic"
    $jxl6Category = Codec-Summary $group "Jxl6"
    $jxl7Category = Codec-Summary $group "Jxl7"
    $jxl8Category = Codec-Summary $group "Jxl8"
    [pscustomobject][ordered]@{
      Category = $_.Name
      Images = $group.Count
      Pixels = [int64](Sum-Field $group "Pixels")
      QlicBytes = $qlicCategory.Bytes
      Jxl6Bytes = $jxl6Category.Bytes
      Jxl7Bytes = $jxl7Category.Bytes
      Jxl8Bytes = $jxl8Category.Bytes
      QlicVsJxl6Percent = [math]::Round(
        100.0 * ($qlicCategory.Bytes - $jxl6Category.Bytes) /
          $jxl6Category.Bytes, 3)
      QlicVsJxl7Percent = [math]::Round(
        100.0 * ($qlicCategory.Bytes - $jxl7Category.Bytes) /
          $jxl7Category.Bytes, 3)
      QlicVsJxl8Percent = [math]::Round(
        100.0 * ($qlicCategory.Bytes - $jxl8Category.Bytes) /
          $jxl8Category.Bytes, 3)
      QlicEncodeSec = $qlicCategory.EncodeSeconds
      Jxl6EncodeSec = $jxl6Category.EncodeSeconds
      Jxl7EncodeSec = $jxl7Category.EncodeSeconds
      Jxl8EncodeSec = $jxl8Category.EncodeSeconds
      QlicDecodeSec = $qlicCategory.DecodeSeconds
      Jxl6DecodeSec = $jxl6Category.DecodeSeconds
      Jxl7DecodeSec = $jxl7Category.DecodeSeconds
      Jxl8DecodeSec = $jxl8Category.DecodeSeconds
    }
  } | Sort-Object Category)
  $categoryRows | Export-Csv -LiteralPath (
    Join-Path $outputPath "categories.csv"
  ) -NoTypeInformation -Encoding utf8
  $summary | Format-List
  Write-Host "Result: $outputPath"
} finally {
  $fullWork = [IO.Path]::GetFullPath($work)
  if ($fullWork.StartsWith(
      $tempRoot,
      [StringComparison]::OrdinalIgnoreCase
    ) -and $fullWork -ne $tempRoot) {
    Remove-Item -LiteralPath $fullWork -Recurse -Force `
      -ErrorAction SilentlyContinue
  }
}
