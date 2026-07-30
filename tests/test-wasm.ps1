param(
  [string]$Config = "Release",
  [string]$Qlic = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

function Find-QLIC {
  foreach ($p in @(
    (Join-Path $root "build\vs18\$Config\qlic.exe"),
    (Join-Path $root "build\vs17\$Config\qlic.exe"),
    (Join-Path $root "build\clang-nmake\qlic.exe"),
    (Join-Path $root "dist\qlic-cli\qlic.exe")
  )) {
    if (Test-Path $p) { return (Resolve-Path $p).Path }
  }
  return ""
}

$cli = if ($Qlic) { (Resolve-Path -LiteralPath $Qlic).Path } else { Find-QLIC }
if (!$cli) {
  & (Join-Path $root "build.ps1") -Config $Config
  $cli = Find-QLIC
}
if (!$cli) {
  throw "qlic.exe was not found after build."
}

& (Join-Path $root "scripts\build-web.ps1")

function B([int]$v) {
  return [byte]($v -band 255)
}

function Write-Bmp([string]$Path, [int]$W, [int]$H, [scriptblock]$Pixel) {
  $stride = $W * 4
  $img = $stride * $H
  $fs = [IO.File]::Open($Path, [IO.FileMode]::Create, [IO.FileAccess]::Write)
  $bw = [IO.BinaryWriter]::new($fs)
  try {
    $bw.Write([byte[]](0x42, 0x4D))
    $bw.Write([int32](54 + $img))
    $bw.Write([int16]0)
    $bw.Write([int16]0)
    $bw.Write([int32]54)
    $bw.Write([int32]40)
    $bw.Write([int32]$W)
    $bw.Write([int32](-$H))
    $bw.Write([int16]1)
    $bw.Write([int16]32)
    $bw.Write([int32]0)
    $bw.Write([int32]$img)
    $bw.Write([int32]2835)
    $bw.Write([int32]2835)
    $bw.Write([int32]0)
    $bw.Write([int32]0)
    for ($y = 0; $y -lt $H; $y++) {
      for ($x = 0; $x -lt $W; $x++) {
        $p = & $Pixel $x $y
        $bw.Write([byte[]]@((B $p[2]), (B $p[1]), (B $p[0]), (B $p[3])))
      }
    }
  } finally {
    $bw.Dispose()
    $fs.Dispose()
  }
}

$fixtureRoot = Join-Path $env:TEMP "qlic-wasm-fixtures"
$tempPath = [IO.Path]::GetFullPath($env:TEMP).TrimEnd(
  [IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$fixturePath = [IO.Path]::GetFullPath($fixtureRoot)
if (!$fixturePath.StartsWith($tempPath, [StringComparison]::OrdinalIgnoreCase)) {
  throw "Refusing to clear a fixture path outside the temporary directory."
}
if (Test-Path -LiteralPath $fixturePath) {
  Remove-Item -LiteralPath $fixturePath -Recurse -Force
}
New-Item -ItemType Directory -Force $fixtureRoot | Out-Null

# fixed input keeps wasm failures reproducible across machines
$rng = [Random]::new(12345)
$inputs = @(
  (Join-Path $fixtureRoot "zero_1x1.bmp"),
  (Join-Path $fixtureRoot "line_1x17.bmp"),
  (Join-Path $fixtureRoot "line_17x1.bmp"),
  (Join-Path $fixtureRoot "checker_64x64.bmp"),
  (Join-Path $fixtureRoot "random_54x272.bmp"),
  (Join-Path $fixtureRoot "repeated_256x256.bmp"),
  (Join-Path $fixtureRoot "animation_key_64x64.bmp"),
  (Join-Path $fixtureRoot "animation_patch_8x8.bmp"),
  (Join-Path $fixtureRoot "animation_move_64x64.bmp")
)

Write-Bmp -Path $inputs[0] -W 1 -H 1 -Pixel { param($x, $y); @(0, 0, 0, 255) }
Write-Bmp -Path $inputs[1] -W 1 -H 17 -Pixel { param($x, $y); @(($y * 7), ($y * 11), ($y * 13), 255) }
Write-Bmp -Path $inputs[2] -W 17 -H 1 -Pixel { param($x, $y); @(($x * 7), ($x * 11), ($x * 13), 255) }
Write-Bmp -Path $inputs[3] -W 64 -H 64 -Pixel {
  param($x, $y);
  if ((([int]($x / 8) + [int]($y / 8)) % 2) -eq 0) { @(20, 40, 80, 255) } else { @(230, 235, 240, 255) }
}
Write-Bmp -Path $inputs[4] -W 54 -H 272 -Pixel {
  param($x, $y);
  @($rng.Next(256), $rng.Next(256), $rng.Next(256), 255)
}
$repeated = [byte[]]::new(256 * 128 * 3)
$repeatedRng = [Random]::new(24680)
Write-Bmp -Path $inputs[5] -W 256 -H 256 -Pixel {
  param($x, $y);
  $i = (($y % 128) * 256 + $x) * 3
  if ($y -lt 128) {
    $repeated[$i] = $repeatedRng.Next(256)
    $repeated[$i + 1] = $repeatedRng.Next(256)
    $repeated[$i + 2] = $repeatedRng.Next(256)
  }
  @($repeated[$i], $repeated[$i + 1], $repeated[$i + 2], 255)
}
Write-Bmp -Path $inputs[6] -W 64 -H 64 -Pixel {
  param($x, $y);
  @(($x * 7 + $y * 3), ($x * 5 + $y * 11), (($x -bxor $y) * 13), 255)
}
Write-Bmp -Path $inputs[7] -W 8 -H 8 -Pixel {
  param($x, $y);
  @(($x * 29 + 17), ($y * 31 + 23), (($x + $y) * 19 + 41), 255)
}
Write-Bmp -Path $inputs[8] -W 64 -H 64 -Pixel {
  param($x, $y);
  if ($x -ge 5 -and $x -lt 17 -and $y -ge 6 -and $y -lt 16) {
    @(($x * 23 + $y * 7), ($x * 11 + $y * 29), (($x -bxor $y) * 31), 255)
  } else {
    @(12, 18, 25, 255)
  }
}

$files = @()
foreach ($input in $inputs) {
  $out = Join-Path $env:TEMP ("qlic-wasm-" + [IO.Path]::GetFileNameWithoutExtension($input) + ".qlic")
  & $cli pack $input $out | Out-Null
  if ($LASTEXITCODE -ne 0 -or !(Test-Path $out)) {
    throw "Fixture encoding failed: $input"
  }
  $files += $out
}

$env:QLIC_WASM_TEST_FILES = ($files | ConvertTo-Json -Compress)
$env:QLIC_WASM_ANIM_PARTS = (@($files[6], $files[7], $files[8]) | ConvertTo-Json -Compress)
$runner = Join-Path $env:TEMP "qlic-wasm-test.mjs"
$module = Join-Path $env:TEMP "qlic-web-test.mjs"
Copy-Item (Join-Path $root "web\dist\qlic-web.js") $module -Force

@'
import fs from "fs";
import { createQlic } from "./qlic-web-test.mjs";

const root = process.cwd();
const qlic = await createQlic({
  wasmBinary: fs.readFileSync(root + "/web/dist/qlic-web.wasm")
});

for (const file of JSON.parse(process.env.QLIC_WASM_TEST_FILES)) {
  const encoded = fs.readFileSync(file);
  if (file.includes("repeated_") && !encoded.includes(Buffer.from("QBR1"))) {
    throw new Error(`missing block references: ${file}`);
  }
  const image = qlic.decode(encoded);
  if (!image.width || !image.height || image.frames.length !== 1 ||
      image.animated) {
    throw new Error(`bad decode: ${file}`);
  }
  const bytes = image.frames[0].rgba.byteLength;
  if (bytes !== image.width * image.height * 4) {
    throw new Error(`bad RGBA size: ${file}`);
  }
  console.log(`${image.width}x${image.height} ${bytes}`);
}

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) {
      crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

const cpal = new Uint8Array(44);
cpal.set([0x51, 0x4c, 0x49, 0x43], 0);
const cpalView = new DataView(cpal.buffer);
cpalView.setUint32(4, 2, true);
cpalView.setUint32(8, 1, true);
cpal[12] = 13;
cpal[13] = 10;
cpal[14] = 1;
cpal[15] = 0x80;
cpalView.setUint32(16, 2, true);
cpalView.setBigUint64(20, 12n, true);
cpal.set([10, 20, 30, 255, 30, 30, 30, 0, 0, 0, 0, 1], 28);
cpalView.setUint32(40, crc32(cpal.subarray(0, 40)), true);
const cpalImage = qlic.decode(cpal);
const cpalPixels = cpalImage.frames[0].rgba;
if (cpalPixels.length !== 8 || cpalPixels.some((v, i) => v !== [10, 20, 30, 255, 40, 50, 60, 255][i])) {
  throw new Error("bad cpalette delta decode");
}
console.log("cpalette-delta 8");

const [keyPath, patchPath, moveKeyPath] = JSON.parse(process.env.QLIC_WASM_ANIM_PARTS);
const key = fs.readFileSync(keyPath);
const patch = fs.readFileSync(patchPath);
const keyPixels = qlic.decode(key).frames[0].rgba;
const patchPixels = qlic.decode(patch).frames[0].rgba;
const payloadSize = 12 + 16 + key.length + 8 + 32 + patch.length;
const payload = Buffer.alloc(payloadSize);
payload.write("QAN2", 0, "ascii");
payload.writeUInt32LE(3, 4);
payload.writeUInt32LE(2, 8);
let pos = 12;
payload.writeUInt32LE(10, pos);
payload.writeUInt32LE(0, pos + 4);
payload.writeBigUInt64LE(BigInt(key.length), pos + 8);
key.copy(payload, pos + 16);
pos += 16 + key.length;
payload.writeUInt32LE(20, pos);
payload.writeUInt32LE(1, pos + 4);
pos += 8;
payload.writeUInt32LE(30, pos);
payload.writeUInt32LE(2, pos + 4);
payload.writeUInt32LE(5, pos + 8);
payload.writeUInt32LE(7, pos + 12);
payload.writeUInt32LE(8, pos + 16);
payload.writeUInt32LE(8, pos + 20);
payload.writeBigUInt64LE(BigInt(patch.length), pos + 24);
patch.copy(payload, pos + 32);

const animation = Buffer.alloc(28 + payload.length + 4);
animation.write("QLIC", 0, "ascii");
animation.writeUInt32LE(64, 4);
animation.writeUInt32LE(64, 8);
animation[12] = 17;
animation[15] = 0x80;
animation.writeUInt32LE(3, 16);
animation.writeBigUInt64LE(BigInt(payload.length), 20);
payload.copy(animation, 28);
animation.writeUInt32LE(crc32(animation.subarray(0, animation.length - 4)), animation.length - 4);

const decodedAnimation = qlic.decode(animation);
if (!decodedAnimation.animated || decodedAnimation.frames.length !== 3 ||
    decodedAnimation.loopCount !== 2 ||
    decodedAnimation.frames[0].delay !== 10 || decodedAnimation.frames[1].delay !== 20 ||
    decodedAnimation.frames[2].delay !== 30) {
  throw new Error("bad temporal animation metadata");
}
const frame0 = decodedAnimation.frames[0].rgba;
const frame1 = decodedAnimation.frames[1].rgba;
const frame2 = decodedAnimation.frames[2].rgba;
for (let i = 0; i < keyPixels.length; i++) {
  if (frame0[i] !== keyPixels[i] || frame1[i] !== keyPixels[i]) {
    throw new Error("bad temporal animation key or duplicate");
  }
}
const expected = new Uint8Array(keyPixels);
for (let y = 0; y < 8; y++) {
  expected.set(patchPixels.subarray(y * 32, y * 32 + 32), ((y + 7) * 64 + 5) * 4);
}
for (let i = 0; i < expected.length; i++) {
  if (frame2[i] !== expected[i]) {
    throw new Error("bad temporal animation rectangle");
  }
}
console.log("animation-qan2 3");

const moveKey = fs.readFileSync(moveKeyPath);
const moveKeyPixels = qlic.decode(moveKey).frames[0].rgba;
const movePayload = Buffer.alloc(12 + 16 + moveKey.length + 36);
movePayload.write("QAN2", 0, "ascii");
movePayload.writeUInt32LE(2, 4);
pos = 12;
movePayload.writeUInt32LE(40, pos);
movePayload.writeUInt32LE(0, pos + 4);
movePayload.writeBigUInt64LE(BigInt(moveKey.length), pos + 8);
moveKey.copy(movePayload, pos + 16);
pos += 16 + moveKey.length;
movePayload.writeUInt32LE(50, pos);
movePayload.writeUInt32LE(3, pos + 4);
movePayload.writeUInt32LE(5, pos + 8);
movePayload.writeUInt32LE(6, pos + 12);
movePayload.writeUInt32LE(31, pos + 16);
movePayload.writeUInt32LE(29, pos + 20);
movePayload.writeUInt32LE(12, pos + 24);
movePayload.writeUInt32LE(10, pos + 28);
movePayload.writeUInt32LE(0xff19120c, pos + 32);

const moveAnimation = Buffer.alloc(28 + movePayload.length + 4);
moveAnimation.write("QLIC", 0, "ascii");
moveAnimation.writeUInt32LE(64, 4);
moveAnimation.writeUInt32LE(64, 8);
moveAnimation[12] = 17;
moveAnimation[15] = 0x80;
moveAnimation.writeUInt32LE(2, 16);
moveAnimation.writeBigUInt64LE(BigInt(movePayload.length), 20);
movePayload.copy(moveAnimation, 28);
moveAnimation.writeUInt32LE(
  crc32(moveAnimation.subarray(0, moveAnimation.length - 4)),
  moveAnimation.length - 4
);
const decodedMove = qlic.decode(moveAnimation);
const expectedMove = new Uint8Array(moveKeyPixels);
for (let y = 0; y < 10; y++) {
  for (let x = 0; x < 12; x++) {
    expectedMove.set([12, 18, 25, 255], ((y + 6) * 64 + x + 5) * 4);
  }
  expectedMove.set(
    moveKeyPixels.subarray(((y + 6) * 64 + 5) * 4, ((y + 6) * 64 + 17) * 4),
    ((y + 29) * 64 + 31) * 4
  );
}
if (!decodedMove.animated || decodedMove.frames.length !== 2 ||
    decodedMove.frames[1].delay !== 50) {
  throw new Error("bad temporal move metadata");
}
for (let i = 0; i < expectedMove.length; i++) {
  if (decodedMove.frames[1].rgba[i] !== expectedMove[i]) {
    throw new Error("bad temporal move pixels");
  }
}
console.log("animation-move 2");
'@ | Set-Content -Encoding ASCII $runner

node $runner
if ($LASTEXITCODE -ne 0) { throw "WebAssembly decode test failed." }
