import { readFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";

if (process.argv.length < 9) {
  throw new Error("web_test.mjs expects module, wasm, still, animation, and LZMS paths");
}

const moduleUrl = pathToFileURL(process.argv[2]).href;
const { createQlic } = await import(moduleUrl);
const wasm = await readFile(process.argv[3]);
const stillBytes = await readFile(process.argv[4]);
const animationBytes = await readFile(process.argv[5]);
const qlic = await createQlic({ wasmBinary: wasm });

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; ++bit)
      crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

const encodeWidth = 37;
const encodeHeight = 29;
const encodePixels = new Uint8Array(encodeWidth * encodeHeight * 4);
for (let y = 0; y < encodeHeight; ++y) {
  for (let x = 0; x < encodeWidth; ++x) {
    const offset = (y * encodeWidth + x) * 4;
    encodePixels[offset] = (x * 7 + y * 3) & 255;
    encodePixels[offset + 1] = (x ^ y) * 9 & 255;
    encodePixels[offset + 2] = (x * 5 + y * 11) & 255;
    encodePixels[offset + 3] = (x + y) % 7 ? 255 : 96;
  }
}
const encoded = qlic.encode(encodePixels, encodeWidth, encodeHeight);
if (encoded.length < 32 ||
    String.fromCharCode(...encoded.subarray(0, 4)) !== "QLIC") {
  throw new Error("WebAssembly encode produced an invalid container");
}
const encodedRoundtrip = qlic.decode(encoded);
if (encodedRoundtrip.width !== encodeWidth ||
    encodedRoundtrip.height !== encodeHeight ||
    encodedRoundtrip.frames.length !== 1 ||
    encodedRoundtrip.frames[0].rgba.length !== encodePixels.length) {
  throw new Error("WebAssembly encode dimensions did not roundtrip");
}
for (let i = 0; i < encodePixels.length; ++i) {
  if (encodedRoundtrip.frames[0].rgba[i] !== encodePixels[i])
    throw new Error(`WebAssembly encode changed pixel byte ${i}`);
}

const still = qlic.decode(stillBytes);
if (still.width !== 64 || still.height !== 64 || still.frames.length !== 1 ||
    still.animated || still.frames[0].rgba.byteLength !== 64 * 64 * 4) {
  throw new Error("still image WebAssembly decode failed");
}

const animation = qlic.decode(animationBytes);
if (animation.width !== 2 || animation.height !== 2 ||
    animation.frames.length !== 2 || !animation.animated ||
    animation.frames[0].delay !== 40 || animation.frames[1].delay !== 70) {
  throw new Error("animation WebAssembly decode failed");
}

const corrupt = new Uint8Array(stillBytes);
corrupt[corrupt.length - 1] ^= 1;
let rejected = false;
try {
  qlic.decode(corrupt);
} catch {
  rejected = true;
}
if (!rejected) throw new Error("corrupt WebAssembly input was accepted");

const lzmsCases = [
  [process.argv[6], 512, 512, 0xe1d7051e],
  [process.argv[7], 384, 256, 0x3bf96fd4],
  [process.argv[8], 257, 9, 0x10528ad2],
];
for (const [path, width, height, expectedCrc] of lzmsCases) {
  const decoded = qlic.decode(await readFile(path));
  if (decoded.width !== width || decoded.height !== height ||
      decoded.frames.length !== 1 ||
      crc32(decoded.frames[0].rgba) !== expectedCrc) {
    throw new Error(`LZMS WebAssembly decode failed: ${path}`);
  }
}

for (const path of process.argv.slice(9)) {
  const decoded = qlic.decode(await readFile(path));
  if (!decoded.width || !decoded.height || !decoded.frames.length)
    throw new Error(`additional WebAssembly decode failed: ${path}`);
}

console.log("QLIC WebAssembly tests passed");
