param(
  [Parameter(Mandatory = $true)]
  [string]$Corpus,
  [string]$Output = "benchmark.json",
  [string]$Qlic = "",
  [string]$Cwebp = "cwebp.exe",
  [string]$Dwebp = "dwebp.exe",
  [string]$Cjxl = "cjxl.exe",
  [string]$Djxl = "djxl.exe",
  [string]$Magick = "magick.exe",
  [ValidateRange(0, 9)]
  [int]$WebPEffort = 6,
  [ValidateRange(1, 10)]
  [int]$JxlEffort = 7,
  [ValidateRange(1, 20)]
  [int]$DecodeRuns = 3,
  [ValidateRange(0, 62)]
  [int]$Processor = 0
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function Resolve-Executable([string]$Value, [string]$Label) {
  if (Test-Path -LiteralPath $Value) {
    return (Resolve-Path -LiteralPath $Value).Path
  }
  $command = Get-Command $Value -CommandType Application -ErrorAction SilentlyContinue
  if ($command) {
    return $command.Source
  }
  throw "$Label was not found: $Value"
}

function Find-Qlic {
  if ($Qlic) {
    return Resolve-Executable $Qlic "QLIC"
  }
  foreach ($candidate in @(
    (Join-Path $root "build\benchmark\qlic.exe"),
    (Join-Path $root "build\clang-nmake\qlic.exe"),
    (Join-Path $root "dist\qlic-cli\qlic.exe")
  )) {
    if (Test-Path -LiteralPath $candidate) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }
  & (Join-Path $root "build-clang.ps1") -Config Release `
    -BuildDir "build\benchmark"
  if ($LASTEXITCODE -ne 0) {
    throw "QLIC build failed."
  }
  return (Resolve-Path -LiteralPath (Join-Path $root "build\benchmark\qlic.exe")).Path
}

# ProcessStartInfo.Arguments needs the same quoting rules as Windows argv
function Quote-WindowsArgument([string]$Value) {
  if ($Value.Length -eq 0) {
    return '""'
  }
  if ($Value -notmatch '[\s"]') {
    return $Value
  }
  $result = [Text.StringBuilder]::new()
  [void]$result.Append('"')
  $slashes = 0
  foreach ($c in $Value.ToCharArray()) {
    if ($c -eq '\') {
      $slashes++
    } elseif ($c -eq '"') {
      [void]$result.Append(('\' * ($slashes * 2 + 1)))
      [void]$result.Append('"')
      $slashes = 0
    } else {
      if ($slashes) {
        [void]$result.Append(('\' * $slashes))
      }
      [void]$result.Append($c)
      $slashes = 0
    }
  }
  if ($slashes) {
    [void]$result.Append(('\' * ($slashes * 2)))
  }
  [void]$result.Append('"')
  return $result.ToString()
}

function Invoke-Pinned([string]$Exe, [string[]]$Arguments) {
  $start = [Diagnostics.ProcessStartInfo]::new()
  $start.FileName = $Exe
  $start.Arguments = (($Arguments | ForEach-Object {
    Quote-WindowsArgument $_
  }) -join " ")
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
    # pinning all three tools makes the single core comparison fair
    $process.ProcessorAffinity = [IntPtr]([int64]1 -shl $Processor)
  } catch {
    if (!$process.HasExited) {
      $process.Kill()
    }
    throw
  }
  $stdoutTask = $process.StandardOutput.ReadToEndAsync()
  $stderrTask = $process.StandardError.ReadToEndAsync()
  $process.WaitForExit()
  $clock.Stop()
  $result = [pscustomobject]@{
    ExitCode = $process.ExitCode
    Seconds = $clock.Elapsed.TotalSeconds
    Stdout = $stdoutTask.Result
    Stderr = $stderrTask.Result
  }
  $process.Dispose()
  return $result
}

function Invoke-Checked([string]$Exe, [string[]]$Arguments) {
  $run = Invoke-Pinned $Exe $Arguments
  if ($run.ExitCode -ne 0) {
    throw "$Exe failed: $($run.Stdout)$($run.Stderr)"
  }
  return $run
}

function Average([double[]]$Values) {
  return ($Values | Measure-Object -Average).Average
}

function Info-Value([string]$Text, [string]$Name) {
  $pattern = "(?:^|\s)$([regex]::Escape($Name))=([^\s]+)"
  $match = [regex]::Match($Text, $pattern)
  if (!$match.Success) {
    return $null
  }
  return $match.Groups[1].Value
}

function Info-Integer([string]$Text, [string]$Name) {
  $value = Info-Value $Text $Name
  if ($null -eq $value) {
    return $null
  }
  return [int]$value
}

function Measure-Decode(
  [string]$Exe,
  [string[]]$Arguments,
  [string]$Destination
) {
  # decoder startup gets one untimed run before the measured samples
  Remove-Item -LiteralPath $Destination -Force -ErrorAction SilentlyContinue
  Invoke-Checked $Exe $Arguments | Out-Null
  $samples = [Collections.Generic.List[double]]::new()
  for ($i = 0; $i -lt $DecodeRuns; $i++) {
    Remove-Item -LiteralPath $Destination -Force -ErrorAction SilentlyContinue
    $samples.Add((Invoke-Checked $Exe $Arguments).Seconds)
  }
  return Average $samples.ToArray()
}

function Pixel-Difference([string]$A, [string]$B) {
  $old = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    $text = (& $magick compare -metric AE $A $B "null:" 2>&1 |
      Out-String).Trim()
    $exitCode = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $old
  }
  if ($text -match '^\d+') {
    return [int64]$Matches[0]
  }
  if ($exitCode -eq 0) {
    return 0
  }
  throw "Image comparison failed: $text"
}

function Tool-Version([string]$Exe, [string[]]$Arguments) {
  $run = Invoke-Checked $Exe $Arguments
  return (($run.Stdout + $run.Stderr) -split '\r?\n' |
    Where-Object { $_ } | Select-Object -First 1).Trim()
}

$corpusPath = (Resolve-Path -LiteralPath $Corpus).Path
$files = @(Get-ChildItem -LiteralPath $corpusPath -Filter *.png -File -Recurse |
  Sort-Object FullName)
if ($files.Count -eq 0) {
  throw "No PNG files were found under $corpusPath"
}

$qlicExe = Find-Qlic
$cwebpExe = Resolve-Executable $Cwebp "cwebp"
$dwebpExe = Resolve-Executable $Dwebp "dwebp"
$cjxlExe = Resolve-Executable $Cjxl "cjxl"
$djxlExe = Resolve-Executable $Djxl "djxl"
$magick = Resolve-Executable $Magick "ImageMagick"
$outputPath = if ([IO.Path]::IsPathRooted($Output)) {
  [IO.Path]::GetFullPath($Output)
} else {
  [IO.Path]::GetFullPath((Join-Path $root $Output))
}
$outputDirectory = [IO.Path]::GetDirectoryName($outputPath)
New-Item -ItemType Directory -Force $outputDirectory | Out-Null
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$work = Join-Path $tempRoot ("qlic-benchmark-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $work | Out-Null

try {
  # warm every executable once so first launch work does not favor any codec
  $warm = $files[0].FullName
  $warmQlic = Join-Path $work "warmup.qlic"
  $warmWebp = Join-Path $work "warmup.webp"
  $warmJxl = Join-Path $work "warmup.jxl"
  $warmPng = Join-Path $work "warmup.png"
  Invoke-Checked $qlicExe @("pack", $warm, $warmQlic, "--threads", "1") |
    Out-Null
  Invoke-Checked $cwebpExe @(
    "-quiet", "-lossless", "-exact", "-z", "$WebPEffort", $warm, "-o",
    $warmWebp
  ) | Out-Null
  Invoke-Checked $cjxlExe @(
    $warm, $warmJxl, "-d", "0", "-e", "$JxlEffort", "--num_threads=0"
  ) | Out-Null
  Invoke-Checked $qlicExe @("unpack", $warmQlic, $warmPng) | Out-Null
  Invoke-Checked $dwebpExe @(
    $warmWebp, "-quiet", "-o", $warmPng
  ) | Out-Null
  Invoke-Checked $djxlExe @(
    $warmJxl, $warmPng, "--num_threads=0"
  ) | Out-Null

  $rows = [Collections.Generic.List[object]]::new()
  for ($index = 0; $index -lt $files.Count; $index++) {
    $file = $files[$index]
    $relative = [IO.Path]::GetRelativePath($corpusPath, $file.FullName).
      Replace("\", "/")
    $identity = ((& $magick identify -format "%w,%h,%[channels]" $file.FullName) `
      -join "")
    if ($LASTEXITCODE -ne 0) {
      throw "Could not identify $relative"
    }
    $parts = $identity.Split(",")
    if ($parts.Count -ne 3) {
      throw "Unexpected image metadata for $relative"
    }
    $prefix = "{0:D6}" -f ($index + 1)
    $qlicFile = Join-Path $work "$prefix.qlic"
    $webpFile = Join-Path $work "$prefix.webp"
    $jxlFile = Join-Path $work "$prefix.jxl"
    $qlicPng = Join-Path $work "$prefix-qlic.png"
    $webpPng = Join-Path $work "$prefix-webp.png"
    $jxlPng = Join-Path $work "$prefix-jxl.png"
    Write-Host "[$($index + 1)/$($files.Count)] $relative"

    $qlicEncode = Invoke-Checked $qlicExe @(
      "pack", $file.FullName, $qlicFile, "--threads", "1"
    )
    $webpEncode = Invoke-Checked $cwebpExe @(
      "-quiet", "-lossless", "-exact", "-z", "$WebPEffort",
      $file.FullName, "-o", $webpFile
    )
    $jxlEncode = Invoke-Checked $cjxlExe @(
      $file.FullName, $jxlFile, "-d", "0", "-e", "$JxlEffort",
      "--num_threads=0"
    )
    $qlicDecode = Measure-Decode $qlicExe @(
      "unpack", $qlicFile, $qlicPng, "--threads", "1"
    ) $qlicPng
    $webpDecode = Measure-Decode $dwebpExe @(
      $webpFile, "-quiet", "-o", $webpPng
    ) $webpPng
    $jxlDecode = Measure-Decode $djxlExe @(
      $jxlFile, $jxlPng, "--num_threads=0"
    ) $jxlPng
    $qlicDifference = Pixel-Difference $file.FullName $qlicPng
    $webpDifference = Pixel-Difference $file.FullName $webpPng
    $jxlDifference = Pixel-Difference $file.FullName $jxlPng
    # size and speed numbers are only accepted after exact pixel comparison
    if ($qlicDifference -ne 0 -or $webpDifference -ne 0 -or
        $jxlDifference -ne 0) {
      throw "Lossless verification failed for $relative"
    }
    $info = (Invoke-Checked $qlicExe @("info", $qlicFile)).Stdout -replace '\s+', ' '
    $rows.Add([pscustomobject][ordered]@{
      Path = $relative
      SHA256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).
        Hash.ToLowerInvariant()
      Width = [int]$parts[0]
      Height = [int]$parts[1]
      Channels = $parts[2]
      SourceBytes = $file.Length
      QlicBytes = (Get-Item -LiteralPath $qlicFile).Length
      WebPBytes = (Get-Item -LiteralPath $webpFile).Length
      JxlBytes = (Get-Item -LiteralPath $jxlFile).Length
      QlicEncodeSec = [math]::Round($qlicEncode.Seconds, 6)
      WebPEncodeSec = [math]::Round($webpEncode.Seconds, 6)
      JxlEncodeSec = [math]::Round($jxlEncode.Seconds, 6)
      QlicDecodeSec = [math]::Round($qlicDecode, 6)
      WebPDecodeSec = [math]::Round($webpDecode, 6)
      JxlDecodeSec = [math]::Round($jxlDecode, 6)
      QlicContainerMode = Info-Value $info "mode"
      QlicContainerTransform = Info-Value $info "transform"
      QlicPayloadCodec = Info-Value $info "codec"
      QlicNativeMode = Info-Integer $info "native-mode"
      QlicNativeTransform = Info-Integer $info "native-transform"
      QlicNativeTileLog = Info-Integer $info "native-tile-log"
      QlicNativeAdaptation = Info-Integer $info "native-adaptation"
      QlicNativeChannels = Info-Integer $info "native-channels"
    })
  }

  $totalQlic = ($rows | Measure-Object QlicBytes -Sum).Sum
  $totalWebp = ($rows | Measure-Object WebPBytes -Sum).Sum
  $totalJxl = ($rows | Measure-Object JxlBytes -Sum).Sum
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
    Cpu = $cpu
    LogicalProcessor = $Processor
    QlicVersion = Tool-Version $qlicExe @("version")
    WebPVersion = Tool-Version $cwebpExe @("-version")
    JxlVersion = Tool-Version $cjxlExe @("--version")
    QlicSettings = "pack --threads 1"
    QlicPortable = $false
    WebPSettings = "cwebp -lossless -exact -z $WebPEffort"
    JxlSettings = "cjxl -d 0 -e $JxlEffort --num_threads=0"
    DecodeRuns = $DecodeRuns
    QlicBytes = $totalQlic
    WebPBytes = $totalWebp
    JxlBytes = $totalJxl
    QlicEncodeAverageSec = [math]::Round(
      ($rows | Measure-Object QlicEncodeSec -Average).Average, 6)
    WebPEncodeAverageSec = [math]::Round(
      ($rows | Measure-Object WebPEncodeSec -Average).Average, 6)
    JxlEncodeAverageSec = [math]::Round(
      ($rows | Measure-Object JxlEncodeSec -Average).Average, 6)
    QlicDecodeAverageSec = [math]::Round(
      ($rows | Measure-Object QlicDecodeSec -Average).Average, 6)
    WebPDecodeAverageSec = [math]::Round(
      ($rows | Measure-Object WebPDecodeSec -Average).Average, 6)
    JxlDecodeAverageSec = [math]::Round(
      ($rows | Measure-Object JxlDecodeSec -Average).Average, 6)
    QlicWebPWins = @($rows | Where-Object {
      $_.QlicBytes -lt $_.WebPBytes
    }).Count
    QlicJxlWins = @($rows | Where-Object {
      $_.QlicBytes -lt $_.JxlBytes
    }).Count
    QlicVsWebPPercent = [math]::Round(
      (($totalQlic - $totalWebp) * 100.0) / $totalWebp, 3)
    QlicVsJxlPercent = [math]::Round(
      (($totalQlic - $totalJxl) * 100.0) / $totalJxl, 3)
  }
  [pscustomobject][ordered]@{
    Summary = $summary
    Images = $rows
  } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $outputPath -Encoding utf8
  $summary | Format-List
  Write-Host "Result: $outputPath"
} finally {
  $fullWork = [IO.Path]::GetFullPath($work)
  if ($fullWork.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
      $fullWork -ne $tempRoot) {
    Remove-Item -LiteralPath $fullWork -Recurse -Force -ErrorAction SilentlyContinue
  }
}
