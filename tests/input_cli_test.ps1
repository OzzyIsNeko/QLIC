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
  $restored = Join-Path $Output "$name.restored.png"
  [IO.File]::Delete($encoded)
  [IO.File]::Delete($restored)
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
  & $Qlic unpack $encoded $restored --threads 1 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "QLIC could not restore lossless input: $name"
  }
  (Get-FileHash -LiteralPath $restored -Algorithm SHA256).Hash
}
if (@($hashes | Select-Object -Unique).Count -ne 1) {
  throw "Lossless input formats restored different pixels."
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
  "lossy.avif"
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

function Read-U32BE {
  param([byte[]]$Bytes, [int]$Offset)
  return ([uint32]$Bytes[$Offset] -shl 24) -bor
    ([uint32]$Bytes[$Offset + 1] -shl 16) -bor
    ([uint32]$Bytes[$Offset + 2] -shl 8) -bor
    [uint32]$Bytes[$Offset + 3]
}

function Write-U32BE {
  param([byte[]]$Bytes, [int]$Offset, [uint32]$Value)
  $Bytes[$Offset] = [byte](($Value -shr 24) -band 0xff)
  $Bytes[$Offset + 1] = [byte](($Value -shr 16) -band 0xff)
  $Bytes[$Offset + 2] = [byte](($Value -shr 8) -band 0xff)
  $Bytes[$Offset + 3] = [byte]($Value -band 0xff)
}

function Get-PngChunkPayloads {
  param([string]$Path, [string]$Name)
  [byte[]]$bytes = [IO.File]::ReadAllBytes($Path)
  $payloads = @()
  for ($offset = 8; $offset + 12 -le $bytes.Length;) {
    [uint32]$length = Read-U32BE $bytes $offset
    if ([uint64]$length + $offset + 12 -gt $bytes.Length) {
      throw "Invalid PNG chunk layout in $Path"
    }
    $chunkName = [Text.Encoding]::ASCII.GetString($bytes, $offset + 4, 4)
    if ($chunkName -eq $Name) {
      [byte[]]$payload = [byte[]]::new($length)
      if ($length) {
        [Buffer]::BlockCopy($bytes, $offset + 8, $payload, 0, $length)
      }
      $payloads += [Convert]::ToHexString($payload)
    }
    $offset += [int]$length + 12
    if ($chunkName -eq "IEND") { break }
  }
  return ,$payloads
}

function Get-PngCrc32 {
  param([byte[]]$Bytes)
  [uint64]$crc = 0xffffffffL
  foreach ($value in $Bytes) {
    $crc = ($crc -bxor [uint64]$value) -band 0xffffffffL
    for ($bit = 0; $bit -lt 8; $bit++) {
      if (($crc -band 1) -ne 0) {
        $crc = (0xedb88320L -bxor ($crc -shr 1)) -band 0xffffffffL
      } else {
        $crc = ($crc -shr 1) -band 0xffffffffL
      }
    }
  }
  return [uint32](($crc -bxor 0xffffffffL) -band 0xffffffffL)
}

function Assert-ExactWidePng {
  param([string]$Path, [int]$Width, [int]$Height, [int]$Channels,
        [int]$ColorType, [string]$ExpectedPattern = "direct")
  [byte[]]$png = [IO.File]::ReadAllBytes($Path)
  if ($png.Length -lt 33 -or $png[24] -ne 16 -or
      $png[25] -ne $ColorType) {
    throw "Wide roundtrip did not preserve the 16-bit PNG color layout."
  }
  $idat = [Collections.Generic.List[byte]]::new()
  $offset = 8
  while ($offset + 12 -le $png.Length) {
    $length = [int](Read-U32BE $png $offset)
    if ($length -lt 0 -or $offset + 12 + $length -gt $png.Length) {
      throw "Wide roundtrip PNG has invalid chunks."
    }
    $type = [Text.Encoding]::ASCII.GetString($png, $offset + 4, 4)
    if ($type -eq "IDAT") {
      for ($i = 0; $i -lt $length; $i++) {
        $idat.Add($png[$offset + 8 + $i])
      }
    }
    $offset += 12 + $length
    if ($type -eq "IEND") { break }
  }
  $compressed = [IO.MemoryStream]::new($idat.ToArray(), $false)
  $decoded = [IO.MemoryStream]::new()
  $zlib = [IO.Compression.ZLibStream]::new(
    $compressed, [IO.Compression.CompressionMode]::Decompress)
  $zlib.CopyTo($decoded)
  $zlib.Dispose()
  $compressed.Dispose()
  [byte[]]$scanlines = $decoded.ToArray()
  $decoded.Dispose()
  $bytesPerPixel = $Channels * 2
  $rowBytes = $Width * $bytesPerPixel
  if ($scanlines.Length -ne ($rowBytes + 1) * $Height) {
    throw "Wide roundtrip PNG has an unexpected pixel layout."
  }
  [byte[]]$previous = [byte[]]::new($rowBytes)
  $source = 0
  for ($y = 0; $y -lt $Height; $y++) {
    $filter = $scanlines[$source++]
    [byte[]]$row = [byte[]]::new($rowBytes)
    for ($i = 0; $i -lt $rowBytes; $i++) {
      $left = if ($i -ge $bytesPerPixel) {
        [int]$row[$i - $bytesPerPixel]
      } else { 0 }
      $up = [int]$previous[$i]
      $upperLeft = if ($i -ge $bytesPerPixel) {
        [int]$previous[$i - $bytesPerPixel]
      } else { 0 }
      $predictor = switch ($filter) {
        0 { 0 }
        1 { $left }
        2 { $up }
        3 { [Math]::Floor(($left + $up) / 2) }
        4 {
          $p = $left + $up - $upperLeft
          $pa = [Math]::Abs($p - $left)
          $pb = [Math]::Abs($p - $up)
          $pc = [Math]::Abs($p - $upperLeft)
          if ($pa -le $pb -and $pa -le $pc) { $left }
          elseif ($pb -le $pc) { $up }
          else { $upperLeft }
        }
        default { throw "Wide roundtrip PNG uses an invalid filter." }
      }
      $row[$i] = [byte](($scanlines[$source++] + $predictor) -band 255)
    }
    for ($x = 0; $x -lt $Width; $x++) {
      for ($channel = 0; $channel -lt $Channels; $channel++) {
        $sample = ($x * $Channels + $channel) * 2
        $actual = ([int]$row[$sample] -shl 8) -bor [int]$row[$sample + 1]
        if ($ExpectedPattern -eq "gray-alpha") {
          $sourceChannel = if ($channel -eq 3) { 1 } else { 0 }
          $expected = ($x * 3001 + $y * 7919 +
            $sourceChannel * 11003 + 123) -band 0xffff
        } elseif ($ExpectedPattern -eq "transparent-gray") {
          $gray = ($x * 3001 + $y * 7919 + 123) -band 0xffff
          $expected = if ($channel -eq 3) {
            if ($gray -eq 123) { 0 } else { 65535 }
          } else { $gray }
        } else {
          $expected = ($x * 3001 + $y * 7919 + $channel * 11003 + 123) `
            -band 0xffff
        }
        if ($actual -ne $expected) {
          throw "16-bit sample mismatch at ($x,$y,$channel): " +
            "$actual != $expected"
        }
      }
    }
    $previous = $row
  }
}

$dense16 = Join-Path $Output "dense16.png"
$dense16Bytes = [Convert]::FromBase64String(
  "iVBORw0KGgoAAAANSUhEUgAAABEAAAAJEAAAAAChE0STAAABRklEQVR42gE7AcT+AAB7DDQX7SOmL187GEbRUopeQ2n8dbWBbo0nmOCkmbBSvAsAH2orIzbcQpVOTloHZcBxeX0yiOuUpKBdrBa3z8OIz0Ha+gA+WUoSVcthhG09ePaEr5BonCGn2rOTv0zLBda+4nfuMPnpAF1IaQF0uoBzjCyX5aOer1e7EMbJ0oLeO+n09a0BZg0fGNgAfDeH8JOpn2KrG7bUwo3ORtn/5bjxcf0qCOMUnCBVLA43xwCbJqbfspi+UcoK1cPhfO01+O4EpxBgHBkn0jOLP0RK/Va2ALoVxc7Rh91A6Pn0sgBrDCQX3SOWL087CEbBUnpeM2nsdaUA2QTkvfB2/C8H6BOhH1orEzbMQoVOPln3ZbBxaX0iiNuUlAD38wOsD2UbHibXMpA+SUoCVbthdG0teOaEn5BYnBGnyrOD6dKWZYkT7hEAAAAASUVORK5CYII=")
[IO.File]::WriteAllBytes($dense16, $dense16Bytes)
$wideQlic = Join-Path $Output "dense16.qlic"
$widePng = Join-Path $Output "dense16-roundtrip.png"
[IO.File]::Delete($wideQlic)
[IO.File]::Delete($widePng)
$wideMessage = @(& $Qlic pack $dense16 $wideQlic --threads 1 2>&1)
if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $wideQlic)) {
  throw "QLIC rejected an exact 16-bit PNG: $($wideMessage -join ' ')"
}
$wideInfo = @(& $Qlic info $wideQlic 2>&1) | Out-String
if ($LASTEXITCODE -ne 0 -or $wideInfo -notmatch "mode=native-wide" -or
    $wideInfo -notmatch "channels=1 bits-per-sample=16") {
  throw "QLIC did not record the 16-bit stream metadata."
}
$wideMessage = @(& $Qlic unpack $wideQlic $widePng 2>&1)
if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $widePng)) {
  throw "QLIC could not restore an exact 16-bit PNG: $($wideMessage -join ' ')"
}
Assert-ExactWidePng $widePng 17 9 1 0

$wideColorCases = @(
  [pscustomobject]@{
    Name = "dense16-rgb"; Width = 11; Height = 7; Channels = 3; Color = 2
    Base64 = "iVBORw0KGgoAAAANSUhEUgAAAAsAAAAHEAIAAAABnPxJAAAB4ElEQVR42gHVASr+AAB7K3ZWcQw0Ny9iKhftQuht4yOmTqF5nC9fWlqFVTsYZhORDkbRccycx1KKfYWogF5DiT60OWn8lPe/8nW1oLDLqwAfakpldWArI1YegRk23GHXjNJClW2QmItOTnlJpERaB4UCr/1lwJC7u7ZxeZx0x299Mqgt0yiI67Pm3uGUpL+f6poAPllpVJRPShJ1DaAIVcuAxqvBYYSMf7d6bT2YOMMzePaj8c7shK+vqtqlkGi7Y+ZenCHHHPIXp9rS1f3Qs5PejgmJAF1IiEOzPmkBk/y+93S6n7XKsIBzq27WaYwstyfiIpflwuDt26Oezpn5lK9X2lIFTbsQ5gsRBsbJ8cQcv9KC/X0oeAB8N6cy0i2H8LLr3eaTqb6k6Z+fYspd9VirG9YWARG21OHPDMrCje2IGIPORvlBJDzZ/wT6L/XluBCzO67xcRxsR2cAmybGIfEcpt/R2vzVspjdkwiOvlHpTBRHygr1BSAA1cMAviu54XwMdzdy7TUYMEMr+O4j6U7kBKcvolqdEGA7W2ZWALoV5RAQC8XO8MkbxNGH/IInfd1ACDszNuj5E/Q+7/SyH61KqABrK2ZWYQwkNx9iGhfdQtht0yOWTpF5jC9PWkqFRd5Z6oa92CcDAAAAAElFTkSuQmCC"
  },
  [pscustomobject]@{
    Name = "dense16-rgba"; Width = 11; Height = 7; Channels = 4; Color = 6
    Base64 = "iVBORw0KGgoAAAANSUhEUgAAAAsAAAAHEAYAAACO/mseAAACeklEQVR42gFvApD9AAB7K3ZWcYFsDDQ3L2IqjSUX7ULobeOY3iOmTqF5nKSXL19aWoVVsFA7GGYTkQ68CUbRccycx8fCUop9haiA03teQ4k+tDnfNGn8lPe/8urtdbWgsMur9qYAH2pKZXVgoFsrI1YegRmsFDbcYdeM0rfNQpVtkJiLw4ZOTnlJpETPP1oHhQKv/dr4ZcCQu7u25rFxeZx0x2/yan0yqC3TKP4jiOuz5t7hCdyUpL+f6poVlQA+WWlUlE+/SkoSdQ2gCMsDVcuAxqvB1rxhhIx/t3ridW09mDjDM+4uePaj8c7s+eeEr6+q2qUFoJBou2PmXhFZnCHHHPIXHRKn2tLV/dAoy7OT3o4JiTSEAF1IiEOzPt45aQGT/L736fJ0up+1yrD1q4Bzq27WaQFkjCy3J+IiDR2X5cLg7dsY1qOezpn5lCSPr1faUgVNMEi7EOYLEQY8AcbJ8cQcv0e60oL9fSh4U3MAfDenMtIt/SiH8LLr3eYI4ZOpvqTpnxSan2LKXfVYIFOrG9YWAREsDLbU4c8MyjfFwo3tiBiDQ37ORvlBJDxPN9n/BPov9Vrw5bgQszuuZqnxcRxsR2dyYgCbJsYh8RwcF6bf0dr81SfQspjdkwiOM4m+UelMFEc/QsoK9QUgAEr71cMAviu5VrThfAx3N3Jibe01GDBDK24m+O4j6U7ked8Epy+iWp2FmBBgO1tmVpFRALoV5RAQCzsGxc7wyRvERr/Rh/yCJ31SeN1ACDszNl4x6PkT9D7vaer0sh+tSqh1owBrK2ZWYYFcDCQ3H2IajRUX3ULYbdOYziOWTpF5jKSHL09aSoVFsEBLdTVGlVSFAQAAAABJRU5ErkJggg=="
  },
  [pscustomobject]@{
    Name = "dense16-gray-alpha"; Width = 7; Height = 5
    Channels = 4; Color = 6; Pattern = "gray-alpha"
    Base64 = "iVBORw0KGgoAAAANSUhEUgAAAAcAAAAFEAQAAABzA+IQAAAAnElEQVR42gGRAG7/AAB7K3YMNDcvF+1C6COmTqEvX1paOxhmE0bRccwAH2pKZSsjVh423GHXQpVtkE5OeUlaB4UCZcCQuwA+WWlUShJ1DVXLgMZhhIx/bT2YOHj2o/GEr6+qAF1IiENpAZP8dLqftYBzq26MLLcnl+XC4KOezpkAfDenMofwsuuTqb6kn2LKXasb1ha21OHPwo3tiHYvQtKgsYAvAAAAAElFTkSuQmCC"
  },
  [pscustomobject]@{
    Name = "dense16-transparent-gray"; Width = 7; Height = 5
    Channels = 4; Color = 6; Pattern = "transparent-gray"
    Base64 = "iVBORw0KGgoAAAANSUhEUgAAAAcAAAAFEAAAAAD8YXVHAAAAAnRSTlMAe7FEZYwAAABWSURBVHjaAUsAtP8AAHsMNBftI6YvXzsYRtEAH2orIzbcQpVOTloHZcAAPllKElXLYYRtPXj2hK8AXUhpAXS6gHOMLJflo54AfDeH8JOpn2KrG7bUwo3UCx5RcnBbaQAAAABJRU5ErkJggg=="
  }
)
foreach ($case in $wideColorCases) {
  $sourcePng = Join-Path $Output "$($case.Name).png"
  $encodedQlic = Join-Path $Output "$($case.Name).qlic"
  $restoredPng = Join-Path $Output "$($case.Name)-roundtrip.png"
  [IO.File]::WriteAllBytes(
    $sourcePng, [Convert]::FromBase64String($case.Base64))
  [IO.File]::Delete($encodedQlic)
  [IO.File]::Delete($restoredPng)
  $message = @(& $Qlic pack $sourcePng $encodedQlic --threads 1 2>&1)
  if ($LASTEXITCODE -ne 0) {
    throw "QLIC rejected $($case.Channels)-channel 16-bit PNG: " +
      ($message -join ' ')
  }
  $message = @(& $Qlic unpack $encodedQlic $restoredPng 2>&1)
  if ($LASTEXITCODE -ne 0) {
    throw "QLIC could not restore $($case.Channels)-channel 16-bit PNG: " +
      ($message -join ' ')
  }
  Assert-ExactWidePng $restoredPng $case.Width $case.Height `
    $case.Channels $case.Color $case.Pattern
}

# Self-described color metadata is a real QSW2 encode path for both ordinary
# 8-bit input and native 16-bit samples.
$described8 = Join-Path $Output "described8-srgb.qlic"
[IO.File]::Delete($described8)
& $Qlic pack (Join-Path $Fixtures "base.png") $described8 `
  --color-profile srgb --threads 1 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "QLIC rejected 8-bit sRGB metadata." }
$described8Info = (& $Qlic info $described8 --json | Out-String) |
  ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $described8Info.mode -ne 20 -or
    $described8Info.hdr.channels -ne 3 -or
    $described8Info.hdr.bits_per_sample -ne 8 -or
    $described8Info.hdr.color_authority -ne 2 -or
    !$described8Info.hdr.cicp -or $described8Info.hdr.icc) {
  throw "QLIC recorded incorrect 8-bit sRGB metadata."
}
& $Qlic verify $described8 --json 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "QLIC could not verify 8-bit QSW2." }

$described16 = Join-Path $Output "described16-rgba.qlic"
[IO.File]::Delete($described16)
& $Qlic pack (Join-Path $Output "dense16-rgba.png") $described16 `
  --color-profile display-p3 --alpha straight --threads 1 2>&1 |
  Out-Null
if ($LASTEXITCODE -ne 0) { throw "QLIC rejected 16-bit RGBA metadata." }
$described16Info = (& $Qlic info $described16 --json | Out-String) |
  ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $described16Info.mode -ne 20 -or
    $described16Info.hdr.channels -ne 4 -or
    $described16Info.hdr.bits_per_sample -ne 16 -or
    $described16Info.hdr.alpha -ne 1 -or !$described16Info.hdr.cicp) {
  throw "QLIC recorded incorrect 16-bit RGBA metadata."
}
& $Qlic verify $described16 --json 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "QLIC could not verify 16-bit QSW2." }
$described16Png = Join-Path $Output "described16-rgba-roundtrip.png"
& $Qlic unpack $described16 $described16Png --threads 1 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "QLIC could not export 16-bit QSW2." }
Assert-ExactWidePng $described16Png 11 7 4 6

foreach ($hdrProfile in @(
  [pscustomobject]@{ Name = "rec2100-pq"; Transfer = 16 },
  [pscustomobject]@{ Name = "rec2100-hlg"; Transfer = 18 }
)) {
  $hdrQlic = Join-Path $Output ($hdrProfile.Name + ".qlic")
  $hdrPng = Join-Path $Output ($hdrProfile.Name + ".png")
  $hdrDescriptor = $hdrPng + ".qlic-hdr.txt"
  [IO.File]::Delete($hdrQlic)
  [IO.File]::Delete($hdrPng)
  [IO.File]::Delete($hdrDescriptor)
  & $Qlic pack (Join-Path $Output "dense16-rgba.png") $hdrQlic `
    --color-profile $hdrProfile.Name --alpha straight --threads 1 2>&1 |
    Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "QLIC rejected $($hdrProfile.Name) input."
  }
  $hdrInfo = (& $Qlic info $hdrQlic --json | Out-String) | ConvertFrom-Json
  if ($LASTEXITCODE -ne 0 -or $hdrInfo.mode -ne 20 -or
      $hdrInfo.hdr.channels -ne 4 -or $hdrInfo.hdr.bits_per_sample -ne 16 -or
      $hdrInfo.hdr.color_authority -ne 2 -or !$hdrInfo.hdr.cicp -or
      $hdrInfo.hdr.icc) {
    throw "QLIC recorded incorrect $($hdrProfile.Name) metadata."
  }
  & $Qlic unpack $hdrQlic $hdrPng --threads 1 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "QLIC could not restore $($hdrProfile.Name) input."
  }
  Assert-ExactWidePng $hdrPng 11 7 4 6
  $descriptor = Get-Content -LiteralPath $hdrDescriptor -Raw
  if ($descriptor -notmatch
      "(?m)^cicp=9,$($hdrProfile.Transfer),0,1\r?$") {
    throw "QLIC did not retain the $($hdrProfile.Name) CICP transfer."
  }
}

$premultiplied16 = Join-Path $Output "described16-premultiplied.qlic"
$premultipliedPng = Join-Path $Output "described16-premultiplied.png"
$premultipliedTiff = Join-Path $Output "described16-premultiplied.tiff"
$premultipliedRepacked = Join-Path $Output "described16-premultiplied-repacked.qlic"
$premultipliedRoundtrip = Join-Path $Output "described16-premultiplied-roundtrip.tiff"
[IO.File]::Delete($premultipliedPng)
[IO.File]::Delete($premultipliedTiff)
[IO.File]::Delete($premultipliedRepacked)
[IO.File]::Delete($premultipliedRoundtrip)
& $Qlic pack (Join-Path $Output "dense16-rgba.png") $premultiplied16 `
  --color-profile display-p3 --alpha premultiplied --threads 1 2>&1 |
  Out-Null
if ($LASTEXITCODE -ne 0) { throw "QLIC rejected premultiplied QSW2 input." }
& $Qlic unpack $premultiplied16 $premultipliedPng --threads 1 2>&1 | Out-Null
if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath $premultipliedPng)) {
  throw "QLIC mislabeled premultiplied QSW2 as straight-alpha output."
}
& $Qlic unpack $premultiplied16 $premultipliedTiff --threads 1 2>&1 |
  Out-Null
if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $premultipliedTiff)) {
  throw "QLIC could not export associated-alpha TIFF."
}
& $Qlic pack $premultipliedTiff $premultipliedRepacked `
  --color-profile display-p3 --alpha premultiplied --threads 1 2>&1 |
  Out-Null
if ($LASTEXITCODE -ne 0) {
  throw "QLIC could not re-import associated-alpha TIFF."
}
& $Qlic unpack $premultipliedRepacked $premultipliedRoundtrip --threads 1 `
  2>&1 | Out-Null
if ($LASTEXITCODE -ne 0 -or
    (Get-FileHash -Algorithm SHA256 $premultipliedTiff).Hash -ne
    (Get-FileHash -Algorithm SHA256 $premultipliedRoundtrip).Hash) {
  throw "Associated-alpha TIFF did not preserve exact premultiplied samples."
}

$iccPath = Join-Path $Output "test-profile.icc"
[IO.File]::WriteAllBytes($iccPath, [Text.Encoding]::ASCII.GetBytes(
  "QLIC deterministic test ICC payload"))
$describedIcc = Join-Path $Output "described8-icc.qlic"
[IO.File]::Delete($describedIcc)
& $Qlic pack (Join-Path $Fixtures "base.png") $describedIcc `
  --icc $iccPath --threads 1 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "QLIC rejected ICC metadata." }
$iccInfo = (& $Qlic info $describedIcc --json | Out-String) | ConvertFrom-Json
if ($iccInfo.mode -ne 20 -or !$iccInfo.hdr.icc -or $iccInfo.hdr.cicp -or
    $iccInfo.hdr.color_authority -ne 1) {
  throw "QLIC recorded incorrect ICC authority."
}

# Opaque photographic blocks stay byte-exact and CLI export always writes
# explicit sidecars instead of silently dropping camera/editor metadata.
$photoQlic = Join-Path $Output "photo-metadata.qlic"
$photoPng = Join-Path $Output "photo-metadata-restored.png"
$photoSources = @{
  exif = Join-Path $Fixtures "base.bmp"
  xmp = Join-Path $Fixtures "base.png"
  iptc = Join-Path $Fixtures "lossless.webp"
  jumbf = Join-Path $Fixtures "lossless.jxl"
}
& $Qlic pack (Join-Path $Fixtures "base.png") $photoQlic `
  --exif $photoSources.exif --xmp $photoSources.xmp `
  --iptc $photoSources.iptc --jumbf $photoSources.jumbf --threads 1 `
  2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "QLIC rejected photographic metadata." }
$photoInfo = (& $Qlic info $photoQlic --json | Out-String) | ConvertFrom-Json
if ($photoInfo.mode -ne 20 -or $photoInfo.hdr.metadata_blocks -lt 4 -or
    $photoInfo.hdr.color_authority -ne 0) {
  throw "QLIC reported incorrect photographic metadata."
}
& $Qlic unpack $photoQlic $photoPng --threads 1 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "QLIC could not export photo metadata." }
foreach ($kind in @("exif", "xmp", "iptc", "jumbf")) {
  $sidecar = $photoPng + "." + $kind
  if (!(Test-Path -LiteralPath $sidecar) -or
      (Get-FileHash -Algorithm SHA256 $sidecar).Hash -ne
      (Get-FileHash -Algorithm SHA256 $photoSources[$kind]).Hash) {
    throw "QLIC changed the $kind metadata block."
  }
}
if (!(Test-Path -LiteralPath ($photoPng + ".qlic-hdr.txt"))) {
  throw "QLIC did not emit the HDR descriptor sidecar."
}

# WIC integration fixtures use standards-shaped metadata and a known pHYs
# chunk so the COM metadata and physical-resolution surfaces are testable.
$wicXmpPath = Join-Path $Output "wic-metadata.xmp"
$wicExifPath = Join-Path $Output "wic-metadata.exif"
$wicMetadataQlic = Join-Path $Output "wic-metadata.qlic"
$wicXmp = @'
<x:xmpmeta xmlns:x="adobe:ns:meta/">
  <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
    <rdf:Description rdf:about="" xmlns:xmp="http://ns.adobe.com/xap/1.0/" xmp:CreatorTool="QLIC WIC Test" />
  </rdf:RDF>
</x:xmpmeta>
'@
[IO.File]::WriteAllText(
  $wicXmpPath, $wicXmp, [Text.UTF8Encoding]::new($false))
[byte[]]$wicExif = @(
  0x49, 0x49, 0x2a, 0x00, 0x08, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x12, 0x01, 0x03, 0x00, 0x01, 0x00,
  0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00)
[IO.File]::WriteAllBytes($wicExifPath, $wicExif)
& $Qlic pack (Join-Path $Fixtures "base.png") $wicMetadataQlic `
  --xmp $wicXmpPath --exif $wicExifPath --threads 1 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
  throw "QLIC rejected standards-shaped XMP and EXIF."
}
$wicMetadataPng = Join-Path $Output "wic-metadata-restored.png"
& $Qlic unpack $wicMetadataQlic $wicMetadataPng --threads 1 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
  throw "QLIC could not embed standards-shaped PNG metadata."
}
$embeddedExif = @(Get-PngChunkPayloads $wicMetadataPng "eXIf")
if ($embeddedExif.Count -ne 1 -or
    $embeddedExif[0] -ne
    [Convert]::ToHexString($wicExif)) {
  throw "QLIC did not embed the exact EXIF payload in PNG."
}
$embeddedText = @(Get-PngChunkPayloads $wicMetadataPng "iTXt")
[byte[]]$xmpPrefix = [Text.Encoding]::ASCII.GetBytes("XML:com.adobe.xmp") +
  [byte[]](0, 0, 0, 0, 0)
[byte[]]$expectedXmp = $xmpPrefix + [IO.File]::ReadAllBytes($wicXmpPath)
if ($embeddedText.Count -ne 1 -or
    $embeddedText[0] -ne
    [Convert]::ToHexString($expectedXmp)) {
  throw "QLIC did not embed the exact XMP packet in PNG."
}

[byte[]]$resolutionSource = [IO.File]::ReadAllBytes(
  (Join-Path $Fixtures "base.png"))
if ($resolutionSource.Length -lt 33 -or
    [Text.Encoding]::ASCII.GetString($resolutionSource, 12, 4) -ne "IHDR") {
  throw "The WIC resolution source has an invalid PNG header."
}
[byte[]]$resolutionPng = [byte[]]::new($resolutionSource.Length + 21)
[Buffer]::BlockCopy($resolutionSource, 0, $resolutionPng, 0, 33)
Write-U32BE $resolutionPng 33 9
[byte[]]$physicalChunk = [byte[]]::new(13)
[Text.Encoding]::ASCII.GetBytes("pHYs").CopyTo($physicalChunk, 0)
Write-U32BE $physicalChunk 4 11811
Write-U32BE $physicalChunk 8 5906
$physicalChunk[12] = 1
[Buffer]::BlockCopy($physicalChunk, 0, $resolutionPng, 37, 13)
Write-U32BE $resolutionPng 50 (Get-PngCrc32 $physicalChunk)
[Buffer]::BlockCopy($resolutionSource, 33, $resolutionPng, 54,
  $resolutionSource.Length - 33)
$resolutionPngPath = Join-Path $Output "wic-resolution.png"
$resolutionQlic = Join-Path $Output "wic-resolution.qlic"
[IO.File]::WriteAllBytes($resolutionPngPath, $resolutionPng)
& $Qlic pack $resolutionPngPath $resolutionQlic --threads 1 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "QLIC rejected PNG physical resolution." }
$resolutionInfo = (& $Qlic info $resolutionQlic --json | Out-String) |
  ConvertFrom-Json
if ($resolutionInfo.mode -ne 20 -or
    $resolutionInfo.hdr.metadata_blocks -lt 1) {
  throw "QLIC did not preserve PNG physical resolution metadata."
}

$automaticPng = Join-Path $Output "automatic-xmp.png"
$automaticXmp = Join-Path $Output "automatic-xmp.xmp"
$automaticQlic = Join-Path $Output "automatic-xmp.qlic"
[IO.File]::Copy((Join-Path $Fixtures "base.png"), $automaticPng, $true)
[IO.File]::Copy((Join-Path $Fixtures "lossless.jxl"), $automaticXmp, $true)
& $Qlic pack $automaticPng $automaticQlic --threads 1 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "QLIC rejected an adjacent XMP sidecar." }
$automaticInfo = (& $Qlic info $automaticQlic --json | Out-String) |
  ConvertFrom-Json
if ($automaticInfo.mode -ne 20 -or
    $automaticInfo.hdr.metadata_blocks -lt 1) {
  throw "QLIC did not automatically preserve an adjacent XMP sidecar."
}

$invalidIndex = 0
foreach ($invalid in @(
  @("--color-profile", "rec2100-pq"),
  @("--color-profile", "rec2100-hlg"),
  @("--color-profile", "unknown-profile"),
  @("--color-profile", "srgb", "--alpha", "premultiplied"),
  @("--color-profile", "srgb", "--icc", $iccPath)
)) {
  $bad = Join-Path $Output ("invalid-metadata-{0}.qlic" -f $invalidIndex)
  $invalidIndex++
  [IO.File]::Delete($bad)
  & $Qlic pack (Join-Path $Fixtures "base.png") $bad @invalid --threads 1 `
    2>&1 | Out-Null
  if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath $bad)) {
    throw "QLIC accepted an invalid metadata option combination."
  }
}

Write-Host "lossless input CLI checks passed"
