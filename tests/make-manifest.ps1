param(
  [string]$FixtureDir = (Join-Path $PSScriptRoot 'fixtures'),
  [string]$Output = (Join-Path $PSScriptRoot 'fixtures/manifest.json')
)

$ErrorActionPreference = 'Stop'

function Read-U32([byte[]]$Bytes, [int]$Offset) {
  return [uint32]([uint32]$Bytes[$Offset] -bor
    ([uint32]$Bytes[$Offset + 1] -shl 8) -bor
    ([uint32]$Bytes[$Offset + 2] -shl 16) -bor
    ([uint32]$Bytes[$Offset + 3] -shl 24))
}

function Hex32([uint32]$Value) {
  return ('{0:x8}' -f $Value)
}

$cases = @(
  @{ name='animation.qlic'; profile='animation-1'; crc=@('bc5aab1c','d6538a42') },
  @{ name='blocks.qlic'; profile='legacy-0x'; crc=@('5f370f87') },
  @{ name='cpalette-lzms.qlic'; profile='core-still-1'; crc=@('e1d7051e') },
  @{ name='gray-model-lzms.qlic'; profile='legacy-0x'; crc=@('3bf96fd4') },
  @{ name='gray-rle.qlic'; profile='core-still-1'; crc=@('d59a4f1e') },
  @{ name='native.qlic'; profile='core-still-1'; crc=@('b385d194') },
  @{ name='normal-map-quadratic.qlic'; profile='core-still-1'; crc=@('9122c65a') },
  @{ name='normal-map-sphere-green8.qlic'; profile='core-still-1'; crc=@('9dd0c466') },
  @{ name='palette-filtered.qlic'; profile='legacy-0x'; crc=@('2b1e5d73') },
  @{ name='palette.qlic'; profile='core-still-1'; crc=@('5f370f87') },
  @{ name='planar-med-lzms.qlic'; profile='core-still-1'; crc=@('d04203d4') },
  @{ name='rgb-lzms.qlic'; profile='core-still-1'; crc=@('10528ad2') },
  @{ name='separable.qlic'; profile='core-still-1'; crc=@('0c463091') },
  @{ name='tile-model.qlic'; profile='legacy-0x'; crc=@('db03a251') },
  @{ name='tile-palette-lzms.qlic'; profile='core-still-1'; crc=@('55533a44') },
  @{ name='wide-u16-10-boundary.qlic'; profile='wide-integer-1' },
  @{ name='wide-u16-16-rgba.qlic'; profile='wide-integer-1' },
  @{ name='wide-u32-17-boundary.qlic'; profile='wide-integer-1' },
  @{ name='wide-u32-24-rgb.qlic'; profile='wide-integer-1' },
  @{ name='hdr-u16-10-pq-rgb.qlic'; profile='hdr-1' },
  @{ name='hdr-u16-10-hlg-rgb.qlic'; profile='hdr-1' },
  @{ name='hdr-u16-12-pq-rgba.qlic'; profile='hdr-1' },
  @{ name='described-u16-8-srgb-rgb.qlic'; profile='hdr-1' }
)

$fixtures = foreach ($case in $cases) {
  $path = Join-Path $FixtureDir $case.name
  if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "missing fixture: $path"
  }
  [byte[]]$bytes = [IO.File]::ReadAllBytes($path)
  if ($bytes.Length -lt 32 -or [Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ne 'QLIC') {
    throw "invalid QLIC fixture: $path"
  }
  $mode = [int]$bytes[12]
  $entry = [ordered]@{
    file = $case.name
    profile = $case.profile
    bytes = $bytes.Length
    sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    width = [uint64](Read-U32 $bytes 4)
    height = [uint64](Read-U32 $bytes 8)
    outer_mode = $mode
    outer_transform = [int]$bytes[13]
    outer_codec = [int]($bytes[15] -band 3)
  }
  if ($case.crc) {
    $entry.decoded_rgba_crc32 = @($case.crc)
  }
  if ($mode -eq 9) {
    $qst = 28
    if ([Text.Encoding]::ASCII.GetString($bytes, $qst, 4) -ne 'QST1') {
      throw "stored native fixture does not start with QST1: $path"
    }
    $entry.native_mode = [int]$bytes[$qst + 14]
    $entry.native_transform = [int]$bytes[$qst + 15]
    $entry.native_tile_log = [int]$bytes[$qst + 16]
    $entry.native_control = [int]$bytes[$qst + 17]
    $entry.native_pixel_crc32 = Hex32 (Read-U32 $bytes ($qst + 18))
  }
  if ($mode -eq 19 -or $mode -eq 20) {
    $marker = [Text.Encoding]::ASCII.GetBytes('QSW1')
    $qsw = -1
    for ($i = 28; $i -le $bytes.Length - 16; ++$i) {
      if ($bytes[$i] -eq $marker[0] -and $bytes[$i + 1] -eq $marker[1] -and
          $bytes[$i + 2] -eq $marker[2] -and $bytes[$i + 3] -eq $marker[3]) {
        $qsw = $i
        break
      }
    }
    if ($qsw -lt 0) { throw "QSW1 payload not found: $path" }
    $entry.sample_crc32 = Hex32 (Read-U32 $bytes ($qsw + 8))
    $entry.bits_per_sample = [int]$bytes[14]
    $entry.channels = [uint64](Read-U32 $bytes 16)
  }
  [pscustomobject]$entry
}

$profilePath = Join-Path $PSScriptRoot '../docs/profiles.json'
$manifest = [ordered]@{
  schema = 1
  format = 'QLIC conformance fixtures'
  profiles_sha256 = (Get-FileHash -LiteralPath $profilePath -Algorithm SHA256).Hash.ToLowerInvariant()
  crc32 = [ordered]@{
    name = 'CRC-32/ISO-HDLC'
    reflected_polynomial = 'edb88320'
    initial = 'ffffffff'
    final_xor = 'ffffffff'
    check_123456789 = 'cbf43926'
  }
  fixtures = @($fixtures)
}

$json = $manifest | ConvertTo-Json -Depth 8
$directory = Split-Path -Parent $Output
[IO.Directory]::CreateDirectory($directory) | Out-Null
[IO.File]::WriteAllText($Output, $json + "`n", [Text.UTF8Encoding]::new($false))
Write-Output "Wrote $($fixtures.Count) fixtures to $Output"
