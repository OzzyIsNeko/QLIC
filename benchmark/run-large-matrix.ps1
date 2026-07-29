param(
  [string]$Corpus = "",
  [string]$Output = "",
  [string]$Qlic = "",
  [string]$Cwebp = "",
  [string]$Dwebp = "",
  [string]$Cjxl = "",
  [string]$Djxl = "",
  [string]$Magick = "magick.exe",
  [ValidateRange(0, 1000000)]
  [int]$MaximumImages = 0,
  [ValidateRange(0, 62)]
  [int]$Processor = 0
)

$ErrorActionPreference = "Stop"
$run = $PSScriptRoot
$repo = Split-Path -Parent $run

function Resolve-Tool([string]$Value, [string]$Fallback, [string]$Label) {
  $candidate = if ($Value) {
    $Value
  } else {
    $Fallback
  }
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
  $peakPaged = [int64]0
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
      $peakPaged = [math]::Max(
        $peakPaged,
        [int64]$process.PeakPagedMemorySize64
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
    PeakPagedBytes = $peakPaged
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

function Tool-Version([string]$Exe, [string[]]$Arguments) {
  $result = Invoke-Checked $Exe $Arguments
  return (($result.Stdout + $result.Stderr) -split "\r?\n" |
    Where-Object { $_ } | Select-Object -First 1).Trim()
}

function Median([object[]]$Values) {
  $sorted = @($Values | ForEach-Object {
    [double]$_
  } | Sort-Object)
  if (!$sorted.Count) {
    return 0
  }
  $middle = [int][math]::Floor($sorted.Count / 2)
  if (($sorted.Count -band 1) -ne 0) {
    return $sorted[$middle]
  }
  return ($sorted[$middle - 1] + $sorted[$middle]) / 2.0
}

function Codec-Summary([object[]]$Rows, [string]$Prefix) {
  $bytesName = "${Prefix}Bytes"
  $secondsName = "${Prefix}EncodeSec"
  $cpuName = "${Prefix}CpuSec"
  $workingName = "${Prefix}PeakWorkingSetBytes"
  $privateName = "${Prefix}PeakPrivateBytes"
  $pagedName = "${Prefix}PeakPagedBytes"
  return [pscustomobject][ordered]@{
    Bytes = [int64](($Rows | Measure-Object $bytesName -Sum).Sum)
    Seconds = [math]::Round(
      ($Rows | Measure-Object $secondsName -Sum).Sum, 6)
    CpuSeconds = [math]::Round(
      ($Rows | Measure-Object $cpuName -Sum).Sum, 6)
    PeakWorkingSetMaximum = [int64](
      ($Rows | Measure-Object $workingName -Maximum).Maximum)
    PeakWorkingSetMedian = [int64](Median @($Rows.$workingName))
    PeakPrivateMaximum = [int64](
      ($Rows | Measure-Object $privateName -Maximum).Maximum)
    PeakPrivateMedian = [int64](Median @($Rows.$privateName))
    PeakPagedMaximum = [int64](
      ($Rows | Measure-Object $pagedName -Maximum).Maximum)
    PeakPagedMedian = [int64](Median @($Rows.$pagedName))
  }
}

$corpusPath = if ($Corpus) {
  (Resolve-Path -LiteralPath $Corpus).Path
} else {
  (Resolve-Path -LiteralPath (Join-Path $run "corpus")).Path
}
$outputPath = if ($Output) {
  [IO.Path]::GetFullPath($Output)
} else {
  Join-Path $run "run"
}
if (Test-Path -LiteralPath $outputPath) {
  throw "Output already exists: $outputPath"
}
New-Item -ItemType Directory -Path $outputPath | Out-Null

$qlicValue = if ($Qlic) { $Qlic } else {
  $existing = Get-ChildItem (Join-Path $repo "build\benchmark") `
    -Filter qlic.exe -File -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.Directory.Name -eq "Release" } |
    Select-Object -First 1
  if (!$existing) {
    & (Join-Path $repo "build.ps1") -Config Release `
      -BuildDir "build\benchmark"
    if ($LASTEXITCODE -ne 0) { throw "QLIC build failed" }
    $existing = Get-ChildItem (Join-Path $repo "build\benchmark") `
      -Filter qlic.exe -File -Recurse |
      Where-Object { $_.Directory.Name -eq "Release" } |
      Select-Object -First 1
  }
  $existing.FullName
}
$qlicExe = Resolve-Tool $qlicValue $qlicValue "QLIC"
$cwebpExe = Resolve-Tool $Cwebp "cwebp.exe" "cwebp"
$dwebpExe = Resolve-Tool $Dwebp "dwebp.exe" "dwebp"
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

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$work = Join-Path $tempRoot ("qlic-large-matrix-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
$resultsPath = Join-Path $outputPath "results.csv"
$rows = [Collections.Generic.List[object]]::new()

try {
  $warm = $files[[int][math]::Floor($files.Count / 2)].File.FullName
  $warmQlic = Join-Path $work "warm.qlic"
  $warmWebp = Join-Path $work "warm.webp"
  $warmJxl = Join-Path $work "warm.jxl"
  Invoke-Checked $qlicExe @(
    "pack", $warm, $warmQlic, "--threads", "1"
  ) | Out-Null
  Invoke-Checked $cwebpExe @(
    "-quiet", "-lossless", "-exact", "-z", "6", $warm, "-o", $warmWebp
  ) | Out-Null
  Invoke-Checked $cjxlExe @(
    $warm, $warmJxl, "-d", "0", "-e", "9", "--num_threads=0"
  ) | Out-Null

  for ($index = 0; $index -lt $files.Count; $index++) {
    $entry = $files[$index]
    $file = $entry.File
    $item = $entry.Manifest
    $prefix = "{0:D6}" -f ($index + 1)
    $qlicFile = Join-Path $work "$prefix.qlic"
    $webpFile = Join-Path $work "$prefix.webp"
    $jxlFile = Join-Path $work "$prefix.jxl"
    $qlicPng = Join-Path $work "$prefix-qlic.png"
    $webpPng = Join-Path $work "$prefix-webp.png"
    $jxlPng = Join-Path $work "$prefix-jxl.png"
    Write-Host "[$($index + 1)/$($files.Count)] $($item.Path)"

    $runs = @{}
    $orders = @(
      @("qlic", "webp", "jxl"),
      @("webp", "jxl", "qlic"),
      @("jxl", "qlic", "webp")
    )
    foreach ($codec in $orders[$index % 3]) {
      if ($codec -eq "qlic") {
        $runs.qlic = Invoke-Checked $qlicExe @(
          "pack", $file.FullName, $qlicFile, "--threads", "1"
        )
      } elseif ($codec -eq "webp") {
        $runs.webp = Invoke-Checked $cwebpExe @(
          "-quiet", "-lossless", "-exact", "-z", "6",
          $file.FullName, "-o", $webpFile
        )
      } else {
        $runs.jxl = Invoke-Checked $cjxlExe @(
          $file.FullName, $jxlFile, "-d", "0", "-e", "9",
          "--num_threads=0"
        )
      }
    }

    Invoke-Checked $qlicExe @(
      "unpack", $qlicFile, $qlicPng, "--threads", "1"
    ) | Out-Null
    Invoke-Checked $dwebpExe @(
      $webpFile, "-quiet", "-o", $webpPng
    ) | Out-Null
    Invoke-Checked $djxlExe @(
      $jxlFile, $jxlPng, "--num_threads=0"
    ) | Out-Null
    $qlicDifference = Pixel-Difference $file.FullName $qlicPng
    $webpDifference = Pixel-Difference $file.FullName $webpPng
    $jxlDifference = Pixel-Difference $file.FullName $jxlPng
    if ($qlicDifference -ne 0 -or $webpDifference -ne 0 -or
        $jxlDifference -ne 0) {
      throw "Exact pixel verification failed for $($item.Path)"
    }

    $row = [pscustomobject][ordered]@{
      Path = $item.Path
      Category = $item.Category
      Width = [int]$item.Width
      Height = [int]$item.Height
      Pixels = [int64]$item.Pixels
      ColorModel = [string]$item.ColorModel
      SHA256 = $item.NormalizedSHA256
      QlicBytes = (Get-Item -LiteralPath $qlicFile).Length
      WebPBytes = (Get-Item -LiteralPath $webpFile).Length
      JxlBytes = (Get-Item -LiteralPath $jxlFile).Length
      QlicEncodeSec = [math]::Round($runs.qlic.Seconds, 6)
      WebPEncodeSec = [math]::Round($runs.webp.Seconds, 6)
      JxlEncodeSec = [math]::Round($runs.jxl.Seconds, 6)
      QlicCpuSec = [math]::Round($runs.qlic.CpuSeconds, 6)
      WebPCpuSec = [math]::Round($runs.webp.CpuSeconds, 6)
      JxlCpuSec = [math]::Round($runs.jxl.CpuSeconds, 6)
      QlicPeakWorkingSetBytes = $runs.qlic.PeakWorkingSetBytes
      WebPPeakWorkingSetBytes = $runs.webp.PeakWorkingSetBytes
      JxlPeakWorkingSetBytes = $runs.jxl.PeakWorkingSetBytes
      QlicPeakPrivateBytes = $runs.qlic.PeakPrivateBytes
      WebPPeakPrivateBytes = $runs.webp.PeakPrivateBytes
      JxlPeakPrivateBytes = $runs.jxl.PeakPrivateBytes
      QlicPeakPagedBytes = $runs.qlic.PeakPagedBytes
      WebPPeakPagedBytes = $runs.webp.PeakPagedBytes
      JxlPeakPagedBytes = $runs.jxl.PeakPagedBytes
      QlicDifference = $qlicDifference
      WebPDifference = $webpDifference
      JxlDifference = $jxlDifference
    }
    $rows.Add($row)
    if ($rows.Count -eq 1) {
      $row | Export-Csv -LiteralPath $resultsPath -NoTypeInformation `
        -Encoding utf8
    } else {
      $row | Export-Csv -LiteralPath $resultsPath -NoTypeInformation `
        -Encoding utf8 -Append
    }
    Remove-Item -LiteralPath $qlicFile, $webpFile, $jxlFile, $qlicPng, `
      $webpPng, $jxlPng -Force
  }

  $qlicSummary = Codec-Summary $rows.ToArray() "Qlic"
  $webpSummary = Codec-Summary $rows.ToArray() "WebP"
  $jxlSummary = Codec-Summary $rows.ToArray() "Jxl"
  $categoryRows = @($rows | Group-Object Category | ForEach-Object {
    $group = @($_.Group)
    $categoryQlic = Codec-Summary $group "Qlic"
    $categoryWebp = Codec-Summary $group "WebP"
    $categoryJxl = Codec-Summary $group "Jxl"
    [pscustomobject][ordered]@{
      Category = $_.Name
      Images = $group.Count
      Pixels = [int64](($group | Measure-Object Pixels -Sum).Sum)
      QlicBytes = $categoryQlic.Bytes
      WebPBytes = $categoryWebp.Bytes
      JxlBytes = $categoryJxl.Bytes
      QlicVsWebPPercent = [math]::Round(
        100.0 * ($categoryQlic.Bytes - $categoryWebp.Bytes) /
        $categoryWebp.Bytes, 3)
      QlicVsJxlPercent = [math]::Round(
        100.0 * ($categoryQlic.Bytes - $categoryJxl.Bytes) /
        $categoryJxl.Bytes, 3)
      QlicSeconds = $categoryQlic.Seconds
      WebPSeconds = $categoryWebp.Seconds
      JxlSeconds = $categoryJxl.Seconds
      QlicVsWebPSpeedPercent = [math]::Round(
        100.0 * ($categoryWebp.Seconds - $categoryQlic.Seconds) /
        $categoryWebp.Seconds, 3)
      QlicJxlSizeWins = @($group | Where-Object {
        $_.QlicBytes -lt $_.JxlBytes
      }).Count
      QlicWebPSizeWins = @($group | Where-Object {
        $_.QlicBytes -lt $_.WebPBytes
      }).Count
    }
  } | Sort-Object Category)
  $categoryRows | Export-Csv -LiteralPath (
    Join-Path $outputPath "categories.csv"
  ) -NoTypeInformation -Encoding utf8

  try {
    $cpu = (Get-CimInstance Win32_Processor -ErrorAction Stop |
      Select-Object -First 1 -ExpandProperty Name).Trim()
  } catch {
    $cpu = [string]$env:PROCESSOR_IDENTIFIER
  }
  $summary = [pscustomobject][ordered]@{
    TimestampUtc = [DateTime]::UtcNow.ToString("o")
    Corpus = $corpusPath
    Images = $rows.Count
    Pixels = [int64](($rows | Measure-Object Pixels -Sum).Sum)
    ExactRoundTrips = @($rows | Where-Object {
      $_.QlicDifference -eq 0 -and $_.WebPDifference -eq 0 -and
      $_.JxlDifference -eq 0
    }).Count
    Cpu = $cpu
    LogicalProcessor = $Processor
    QlicVersion = Tool-Version $qlicExe @("version")
    QlicSHA256 = (Get-FileHash -LiteralPath $qlicExe -Algorithm SHA256).
      Hash.ToLowerInvariant()
    WebPVersion = Tool-Version $cwebpExe @("-version")
    JxlVersion = Tool-Version $cjxlExe @("--version")
    QlicSettings = "pack --threads 1"
    WebPSettings = "cwebp -lossless -exact -z 6"
    JxlSettings = "cjxl -d 0 -e 9 --num_threads=0"
    Timing = "wall time per process including startup and file IO"
    Memory = "Windows peak process working set, private bytes, and paged memory sampled every 5 ms"
    Qlic = $qlicSummary
    WebP = $webpSummary
    Jxl = $jxlSummary
    QlicVsWebPSizePercent = [math]::Round(
      100.0 * ($qlicSummary.Bytes - $webpSummary.Bytes) /
      $webpSummary.Bytes, 3)
    QlicVsJxlSizePercent = [math]::Round(
      100.0 * ($qlicSummary.Bytes - $jxlSummary.Bytes) /
      $jxlSummary.Bytes, 3)
    QlicVsWebPSpeedPercent = [math]::Round(
      100.0 * ($webpSummary.Seconds - $qlicSummary.Seconds) /
      $webpSummary.Seconds, 3)
    QlicVsWebPCpuPercent = [math]::Round(
      100.0 * ($webpSummary.CpuSeconds - $qlicSummary.CpuSeconds) /
      $webpSummary.CpuSeconds, 3)
    QlicJxlSizeWins = @($rows | Where-Object {
      $_.QlicBytes -lt $_.JxlBytes
    }).Count
    QlicJxlSizeTies = @($rows | Where-Object {
      $_.QlicBytes -eq $_.JxlBytes
    }).Count
    QlicWebPSizeWins = @($rows | Where-Object {
      $_.QlicBytes -lt $_.WebPBytes
    }).Count
    Categories = $categoryRows
  }
  $summary | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $outputPath "summary.json") `
      -Encoding utf8
  $summary | Format-List
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
