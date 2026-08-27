const QLIC_MAX_FILE_BYTES = 268435456;
const QLIC_MAX_PIXELS = 33554432;
const QLIC_MAX_FRAMES = 4096;
const QLIC_MAX_ANIMATION_BYTES = 268435456;
const PNG_SIGNATURE = new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]);

let pngCrcTable;

function crcTable() {
  if (pngCrcTable) return pngCrcTable;
  pngCrcTable = new Uint32Array(256);
  for (let value = 0; value < 256; value++) {
    let crc = value;
    for (let bit = 0; bit < 8; bit++)
      crc = crc & 1 ? 0xedb88320 ^ (crc >>> 1) : crc >>> 1;
    pngCrcTable[value] = crc >>> 0;
  }
  return pngCrcTable;
}

function writeU32(bytes, offset, value) {
  bytes[offset] = value >>> 24;
  bytes[offset + 1] = value >>> 16;
  bytes[offset + 2] = value >>> 8;
  bytes[offset + 3] = value;
}

function writePngChunk(output, offset, name, data) {
  writeU32(output, offset, data.byteLength);
  for (let index = 0; index < 4; index++)
    output[offset + 4 + index] = name.charCodeAt(index);
  output.set(data, offset + 8);
  const table = crcTable();
  let crc = 0xffffffff;
  for (let index = offset + 4; index < offset + 8 + data.byteLength; index++)
    crc = table[(crc ^ output[index]) & 255] ^ (crc >>> 8);
  writeU32(output, offset + 8 + data.byteLength, (crc ^ 0xffffffff) >>> 0);
  return offset + 12 + data.byteLength;
}

function storedDeflate(input) {
  const blocks = Math.ceil(input.byteLength / 65535);
  const output = new Uint8Array(2 + blocks * 5 + input.byteLength + 4);
  output.set([0x78, 0x01]);
  let source = 0;
  let target = 2;
  let a = 1;
  let b = 0;
  while (source < input.byteLength) {
    const size = Math.min(65535, input.byteLength - source);
    output[target++] = source + size === input.byteLength ? 1 : 0;
    output[target++] = size;
    output[target++] = size >>> 8;
    output[target++] = ~size;
    output[target++] = ~size >>> 8;
    output.set(input.subarray(source, source + size), target);
    for (let index = source; index < source + size; index++) {
      a += input[index];
      if (a >= 65521) a -= 65521;
      b += a;
    }
    b %= 65521;
    source += size;
    target += size;
  }
  writeU32(output, target, (b << 16 | a) >>> 0);
  return output;
}

async function deflate(input) {
  if (typeof CompressionStream === "function") {
    try {
      const stream = new CompressionStream("deflate");
      const result = new Response(stream.readable).arrayBuffer();
      const writer = stream.writable.getWriter();
      await writer.write(input);
      await writer.close();
      return new Uint8Array(await result);
    } catch (_) {
    }
  }
  return storedDeflate(input);
}

function enforceInputSize(size) {
  if (!Number.isSafeInteger(size) || size < 0 || size > QLIC_MAX_FILE_BYTES) {
    throw new Error("QLIC input exceeds the browser file limit.");
  }
}

function declaredResponseSize(response) {
  if (!response.headers || typeof response.headers.get !== "function") return null;
  const raw = response.headers.get("content-length");
  if (raw === null) return null;
  const value = raw.trim();
  if (!/^\d+$/.test(value)) return null;
  const size = Number(value);
  enforceInputSize(size);
  return size;
}

function streamChunk(value) {
  if (value instanceof Uint8Array) return new Uint8Array(value);
  if (ArrayBuffer.isView(value)) {
    return new Uint8Array(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
  }
  if (value instanceof ArrayBuffer) return new Uint8Array(value.slice(0));
  throw new Error("QLIC download returned invalid data.");
}

async function responseBytes(response) {
  const declared = declaredResponseSize(response);
  if (!response.body || typeof response.body.getReader !== "function") {
    throw new Error("Bounded response streaming is unavailable.");
  }
  const reader = response.body.getReader();
  const fixed = declared === null ? null : new Uint8Array(declared);
  const chunks = fixed ? null : [];
  let total = 0;
  try {
    for (;;) {
      const part = await reader.read();
      if (part.done) break;
      const chunk = streamChunk(part.value);
      if (chunk.byteLength > QLIC_MAX_FILE_BYTES - total) {
        throw new Error("QLIC input exceeds the browser file limit.");
      }
      if (fixed) {
        if (chunk.byteLength > fixed.byteLength - total) {
          throw new Error("QLIC download size does not match its response.");
        }
        fixed.set(chunk, total);
      } else {
        chunks.push(chunk);
      }
      total += chunk.byteLength;
    }
  } catch (error) {
    try {
      await reader.cancel();
    } catch (_) {
    }
    throw error;
  } finally {
    try {
      reader.releaseLock();
    } catch (_) {
    }
  }
  if (fixed) {
    if (total !== fixed.byteLength) {
      throw new Error("QLIC download size does not match its response.");
    }
    return fixed;
  }
  const data = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    data.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return data;
}

export async function createQlic(options = {}) {
  const wasmUrl = options.wasmUrl || (options.wasmBinary
    ? null
    : new URL("qlic-web.wasm", import.meta.url));
  const instance = await loadWasm(wasmUrl, options.wasmBinary);
  const api = instance.exports;
  const textDecoder = new TextDecoder();
  const requiredExports = [
    "memory", "qlic_alloc", "qlic_reset", "qlic_encode", "qlic_validate",
    "qlic_encoded_ptr", "qlic_encoded_size", "qlic_decode", "qlic_width",
    "qlic_decode_wide", "qlic_decode_hdr",
    "qlic_height", "qlic_frame_count", "qlic_loop_count", "qlic_animated",
    "qlic_frame_width", "qlic_frame_height", "qlic_frame_delay",
    "qlic_frame_ptr", "qlic_frame_size", "qlic_sample_width",
    "qlic_sample_height", "qlic_sample_channels", "qlic_sample_bits",
    "qlic_sample_ptr", "qlic_sample_size", "qlic_sample_stride",
    "qlic_hdr_metadata_ptr", "qlic_hdr_metadata_size",
    "qlic_hdr_block_count", "qlic_hdr_block_tag", "qlic_hdr_block_ptr",
    "qlic_hdr_block_size", "qlic_error_ptr"
  ];
  for (const name of requiredExports) {
    if (!(name in api)) throw new Error(`QLIC WebAssembly export is missing: ${name}`);
  }

  // Memory growth detaches prior views.
  const bytes = () => new Uint8Array(api.memory.buffer);

  function readString(ptr) {
    const mem = bytes();
    let end = ptr;
    while (end < mem.length && mem[end]) end++;
    return textDecoder.decode(mem.subarray(ptr, end));
  }

  function asBytes(input) {
    if (input instanceof Uint8Array) return input;
    if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
    if (input instanceof ArrayBuffer) return new Uint8Array(input);
    throw new TypeError("QLIC decode expects an ArrayBuffer or Uint8Array.");
  }

  async function encodePng(input, width, height) {
    const src = asBytes(input);
    if (!Number.isInteger(width) || !Number.isInteger(height) ||
        width < 1 || height < 1 || width * height > QLIC_MAX_PIXELS ||
        src.byteLength !== width * height * 4) {
      throw new TypeError("PNG encode expects packed RGBA pixels and valid dimensions.");
    }
    const rowBytes = width * 4;
    let scanlines = new Uint8Array((rowBytes + 1) * height);
    for (let row = 0; row < height; row++)
      scanlines.set(src.subarray(row * rowBytes, (row + 1) * rowBytes),
        row * (rowBytes + 1) + 1);
    const compressed = await deflate(scanlines);
    scanlines = null;
    const header = new Uint8Array(13);
    writeU32(header, 0, width);
    writeU32(header, 4, height);
    header.set([8, 6, 0, 0, 0], 8);
    const output = new Uint8Array(8 + 25 + compressed.byteLength + 12 + 12);
    output.set(PNG_SIGNATURE);
    let offset = writePngChunk(output, 8, "IHDR", header);
    offset = writePngChunk(output, offset, "IDAT", compressed);
    writePngChunk(output, offset, "IEND", new Uint8Array());
    return output;
  }

  function decode(input) {
    const src = asBytes(input);
    enforceInputSize(src.byteLength);
    api.qlic_reset();
    try {
      const ptr = api.qlic_alloc(src.byteLength);
      if (!ptr) throw new Error("QLIC WASM memory allocation failed.");
      bytes().set(src, ptr);
      if (!api.qlic_decode(ptr, src.byteLength)) {
        throw new Error(readString(api.qlic_error_ptr()) || "QLIC decode failed.");
      }
      const frames = [];
      const count = api.qlic_frame_count();
      if (!Number.isInteger(count) || count < 1 || count > QLIC_MAX_FRAMES) {
        throw new Error("QLIC produced an invalid frame count.");
      }
      const mem = bytes();
      let total = 0;
      for (let i = 0; i < count; i++) {
        const width = api.qlic_frame_width(i);
        const height = api.qlic_frame_height(i);
        const size = api.qlic_frame_size(i);
        const framePtr = api.qlic_frame_ptr(i);
        if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1 ||
            width * height > QLIC_MAX_PIXELS || size !== width * height * 4 ||
            !Number.isInteger(framePtr) || framePtr < 0 || framePtr + size > mem.byteLength ||
            size > QLIC_MAX_ANIMATION_BYTES - total) {
          throw new Error("QLIC produced invalid frame data.");
        }
        // decoded frames live in the wasm arena, copy them before reset
        const rgba = new Uint8ClampedArray(size);
        rgba.set(mem.subarray(framePtr, framePtr + size));
        total += size;
        frames.push({ width, height, delay: api.qlic_frame_delay(i), rgba });
      }
      return {
        width: api.qlic_width(),
        height: api.qlic_height(),
        frames,
        loopCount: api.qlic_loop_count(),
        animated: api.qlic_animated() !== 0
      };
    } finally {
      api.qlic_reset();
    }
  }

  function validate(input) {
    const src = asBytes(input);
    enforceInputSize(src.byteLength);
    api.qlic_reset();
    try {
      const ptr = api.qlic_alloc(src.byteLength);
      if (!ptr) throw new Error("QLIC WASM memory allocation failed.");
      bytes().set(src, ptr);
      if (!api.qlic_validate(ptr, src.byteLength)) {
        throw new Error(readString(api.qlic_error_ptr()) || "QLIC validation failed.");
      }
      return true;
    } finally {
      api.qlic_reset();
    }
  }

  function copySampleResult() {
    const width = api.qlic_sample_width();
    const height = api.qlic_sample_height();
    const channels = api.qlic_sample_channels();
    const bitsPerSample = api.qlic_sample_bits();
    const ptr = api.qlic_sample_ptr();
    const size = api.qlic_sample_size();
    const stride = api.qlic_sample_stride();
    const bytesPerSample = bitsPerSample <= 16 ? 2 : 4;
    const elements = width * height * channels;
    const expectedSize = elements * bytesPerSample;
    const expectedStride = width * channels * bytesPerSample;
    const mem = api.memory.buffer;
    if (!Number.isInteger(width) || !Number.isInteger(height) ||
        !Number.isInteger(channels) || !Number.isInteger(bitsPerSample) ||
        width < 1 || height < 1 || width * height > QLIC_MAX_PIXELS ||
        ![1, 3, 4].includes(channels) || bitsPerSample < 8 || bitsPerSample > 24 ||
        !Number.isSafeInteger(elements) || !Number.isSafeInteger(expectedSize) ||
        size !== expectedSize || stride !== expectedStride ||
        !Number.isInteger(ptr) || ptr < 0 || ptr + size > mem.byteLength) {
      throw new Error("QLIC produced invalid wide sample data.");
    }
    const samples = bitsPerSample <= 16
      ? new Uint16Array(mem, ptr, elements).slice()
      : new Uint32Array(mem, ptr, elements).slice();
    return { width, height, channels, bitsPerSample, stride, samples };
  }

  function decodeWide(input) {
    const src = asBytes(input);
    enforceInputSize(src.byteLength);
    api.qlic_reset();
    try {
      const ptr = api.qlic_alloc(src.byteLength);
      if (!ptr) throw new Error("QLIC WASM memory allocation failed.");
      bytes().set(src, ptr);
      if (!api.qlic_decode_wide(ptr, src.byteLength)) {
        throw new Error(readString(api.qlic_error_ptr()) || "QLIC wide decode failed.");
      }
      return copySampleResult();
    } finally {
      api.qlic_reset();
    }
  }

  function decodeHdr(input) {
    const src = asBytes(input);
    enforceInputSize(src.byteLength);
    api.qlic_reset();
    try {
      const ptr = api.qlic_alloc(src.byteLength);
      if (!ptr) throw new Error("QLIC WASM memory allocation failed.");
      bytes().set(src, ptr);
      if (!api.qlic_decode_hdr(ptr, src.byteLength)) {
        throw new Error(readString(api.qlic_error_ptr()) || "QLIC HDR decode failed.");
      }
      const pixels = copySampleResult();
      const metadataPtr = api.qlic_hdr_metadata_ptr();
      const metadataSize = api.qlic_hdr_metadata_size();
      const mem = api.memory.buffer;
      if (!Number.isInteger(metadataPtr) || metadataPtr < 0 || metadataSize !== 68 ||
          metadataPtr + metadataSize > mem.byteLength) {
        throw new Error("QLIC produced invalid HDR metadata.");
      }
      const metadata = new DataView(mem, metadataPtr, metadataSize);
      const sampleTypeCode = metadata.getUint32(0, true);
      const alphaCode = metadata.getUint32(4, true);
      const authorityCode = metadata.getUint32(8, true);
      const iccPtr = metadata.getUint32(12, true);
      const iccSize = metadata.getUint32(16, true);
      const alphaNames = ["none", "straight", "premultiplied"];
      const authorityNames = [
        "unspecified", "icc", "cicp", "icc-preferred", "cicp-preferred"
      ];
      if (sampleTypeCode !== 1 || !alphaNames[alphaCode] ||
          !authorityNames[authorityCode] || iccPtr + iccSize > mem.byteLength) {
        throw new Error("QLIC produced invalid HDR metadata values.");
      }
      const icc = iccSize ? new Uint8Array(mem, iccPtr, iccSize).slice() : null;
      const hasCicp = metadata.getUint32(20, true) !== 0;
      const hasMasteringDisplay = metadata.getUint32(32, true) !== 0;
      const hasContentLight = metadata.getUint32(60, true) !== 0;
      const metadataCount = api.qlic_hdr_block_count();
      if (!Number.isInteger(metadataCount) || metadataCount < 0 ||
          metadataCount > 256) {
        throw new Error("QLIC produced an invalid HDR metadata block count.");
      }
      const metadataBlocks = [];
      for (let index = 0; index < metadataCount; ++index) {
        const tagCode = api.qlic_hdr_block_tag(index) >>> 0;
        const blockPtr = api.qlic_hdr_block_ptr(index) >>> 0;
        const blockSize = api.qlic_hdr_block_size(index) >>> 0;
        if (blockPtr > mem.byteLength || blockSize > mem.byteLength - blockPtr) {
          throw new Error("QLIC produced an invalid HDR metadata block.");
        }
        metadataBlocks.push({
          tag: String.fromCharCode(
            tagCode & 255, (tagCode >>> 8) & 255,
            (tagCode >>> 16) & 255, (tagCode >>> 24) & 255
          ),
          data: new Uint8Array(mem, blockPtr, blockSize).slice()
        });
      }
      return {
        pixels,
        sampleType: "uint",
        alphaAssociation: alphaNames[alphaCode],
        colorAuthority: authorityNames[authorityCode],
        icc,
        cicp: hasCicp ? {
          colorPrimaries: metadata.getUint16(24, true),
          transferCharacteristics: metadata.getUint16(26, true),
          matrixCoefficients: metadata.getUint16(28, true),
          fullRange: metadata.getUint8(30) !== 0
        } : null,
        masteringDisplay: hasMasteringDisplay ? {
          primaries: [0, 1, 2].map(index => ({
            x: metadata.getUint16(36 + index * 4, true),
            y: metadata.getUint16(38 + index * 4, true)
          })),
          whitePoint: {
            x: metadata.getUint16(48, true),
            y: metadata.getUint16(50, true)
          },
          maxLuminance: metadata.getUint32(52, true),
          minLuminance: metadata.getUint32(56, true)
        } : null,
        contentLight: hasContentLight ? {
          maxContentLightLevel: metadata.getUint16(64, true),
          maxFrameAverageLightLevel: metadata.getUint16(66, true)
        } : null,
        metadata: metadataBlocks
      };
    } finally {
      api.qlic_reset();
    }
  }

  function encode(input, width, height) {
    const src = asBytes(input);
    if (!Number.isInteger(width) || !Number.isInteger(height) ||
        width < 1 || height < 1 || width * height > QLIC_MAX_PIXELS ||
        src.byteLength !== width * height * 4) {
      throw new TypeError("QLIC encode expects packed RGBA pixels and valid dimensions.");
    }
    api.qlic_reset();
    try {
      const ptr = api.qlic_alloc(src.byteLength);
      if (!ptr) throw new Error("QLIC WASM memory allocation failed.");
      bytes().set(src, ptr);
      if (!api.qlic_encode(ptr, src.byteLength, width, height)) {
        throw new Error(readString(api.qlic_error_ptr()) || "QLIC encode failed.");
      }
      const outputPtr = api.qlic_encoded_ptr();
      const outputSize = api.qlic_encoded_size();
      const mem = bytes();
      if (!Number.isInteger(outputPtr) || !Number.isInteger(outputSize) ||
          outputPtr < 0 || outputSize < 1 ||
          outputPtr + outputSize > mem.byteLength ||
          outputSize > QLIC_MAX_FILE_BYTES) {
        throw new Error("QLIC produced invalid encoded data.");
      }
      const output = new Uint8Array(outputSize);
      output.set(mem.subarray(outputPtr, outputPtr + outputSize));
      return output;
    } finally {
      api.qlic_reset();
    }
  }

  async function urlBytes(url, fetchOptions, errorPrefix) {
    const response = await fetch(url, fetchOptions);
    if (!response.ok) throw new Error(`${errorPrefix}: ${response.status}`);
    return responseBytes(response);
  }

  async function blobBytes(blob, operation) {
    if (!blob || typeof blob.arrayBuffer !== "function" ||
        !Number.isSafeInteger(blob.size)) {
      throw new TypeError(`QLIC ${operation} expects a Blob.`);
    }
    enforceInputSize(blob.size);
    const data = await blob.arrayBuffer();
    enforceInputSize(data.byteLength);
    return data;
  }

  async function decodeUrl(url, fetchOptions) {
    return decode(await urlBytes(url, fetchOptions, "Could not fetch QLIC"));
  }

  async function validateUrl(url, fetchOptions) {
    return validate(await urlBytes(url, fetchOptions, "QLIC fetch failed"));
  }

  async function decodeWideUrl(url, fetchOptions) {
    return decodeWide(await urlBytes(url, fetchOptions, "Could not fetch QLIC"));
  }

  async function decodeHdrUrl(url, fetchOptions) {
    return decodeHdr(await urlBytes(url, fetchOptions, "Could not fetch QLIC"));
  }

  async function decodeBlob(blob) {
    return decode(await blobBytes(blob, "decodeBlob"));
  }

  async function validateBlob(blob) {
    return validate(await blobBytes(blob, "validateBlob"));
  }

  async function decodeWideBlob(blob) {
    return decodeWide(await blobBytes(blob, "decodeWideBlob"));
  }

  async function decodeHdrBlob(blob) {
    return decodeHdr(await blobBytes(blob, "decodeHdrBlob"));
  }

  function imageData(frame) {
    return new ImageData(frame.rgba, frame.width, frame.height);
  }

  function canvasContext(canvas) {
    if (!canvas || typeof canvas.getContext !== "function") {
      throw new TypeError("QLIC drawing expects a canvas.");
    }
    const context = canvas.getContext("2d");
    if (!context) throw new Error("A 2D canvas context is unavailable.");
    return context;
  }

  function firstImageData(input) {
    const decoded = input && input.frames ? input : decode(input);
    if (!decoded.frames.length) throw new Error("QLIC produced no frames.");
    return imageData(decoded.frames[0]);
  }

  function draw(input, canvas, frameIndex = 0) {
    const decoded = input && input.frames ? input : decode(input);
    if (!Number.isInteger(frameIndex) || frameIndex < 0 || frameIndex >= decoded.frames.length) {
      throw new RangeError("QLIC frame index is out of range.");
    }
    const frame = decoded.frames[frameIndex];
    const context = canvasContext(canvas);
    canvas.width = frame.width;
    canvas.height = frame.height;
    context.putImageData(imageData(frame), 0, 0);
    return decoded;
  }

  function play(input, canvas) {
    const decoded = input && input.frames ? input : decode(input);
    if (!decoded.frames.length) throw new Error("QLIC produced no frames.");
    const ctx = canvasContext(canvas);
    if (!decoded.animated || decoded.frames.length === 1) {
      draw(decoded, canvas, 0);
      return () => {};
    }
    let index = 0;
    let stopped = false;
    let loops = 0;
    let timer = null;
    canvas.width = decoded.width;
    canvas.height = decoded.height;
    const step = () => {
      if (stopped) return;
      const frame = decoded.frames[index];
      ctx.putImageData(imageData(frame), 0, 0);
      const delay = Math.min(0x7fffffff, Math.max(1, frame.delay || 100));
      index++;
      if (index >= decoded.frames.length) {
        index = 0;
        loops++;
        if (decoded.loopCount && loops >= decoded.loopCount) {
          stopped = true;
          return;
        }
      }
      timer = setTimeout(step, delay);
    };
    step();
    return () => {
      stopped = true;
      if (timer !== null) clearTimeout(timer);
    };
  }

  return {
    encode, decode, validate, decodeUrl, decodeBlob, validateUrl, validateBlob,
    decodeWide, decodeWideUrl, decodeWideBlob,
    decodeHdr, decodeHdrUrl, decodeHdrBlob,
    encodePng, firstImageData, imageData, draw, play
  };
}

async function loadWasm(url, binary) {
  const imports = {};
  if (binary) {
    const result = await WebAssembly.instantiate(binary, imports);
    return result.instance;
  }
  const response = await fetch(url);
  if (!response.ok) throw new Error(`Could not fetch QLIC WASM: ${response.status}`);
  if (typeof WebAssembly.instantiateStreaming === "function") {
    try {
      const result = await WebAssembly.instantiateStreaming(response.clone(), imports);
      return result.instance;
    } catch (_) {
    }
  }
  const result = await WebAssembly.instantiate(await response.arrayBuffer(), imports);
  return result.instance;
}

export default createQlic;
