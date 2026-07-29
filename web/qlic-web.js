const QLIC_MAX_FILE_BYTES = 268435456;
const QLIC_MAX_PIXELS = 33554432;
const QLIC_MAX_FRAMES = 4096;
const QLIC_MAX_ANIMATION_BYTES = 268435456;

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
  const wasmUrl = options.wasmUrl || new URL("qlic-web.wasm", import.meta.url);
  const instance = await loadWasm(wasmUrl, options.wasmBinary);
  const api = instance.exports;
  const textDecoder = new TextDecoder();
  const requiredExports = [
    "memory", "qlic_alloc", "qlic_reset", "qlic_encode",
    "qlic_encoded_ptr", "qlic_encoded_size", "qlic_decode", "qlic_width",
    "qlic_height", "qlic_frame_count", "qlic_loop_count", "qlic_animated",
    "qlic_frame_width", "qlic_frame_height", "qlic_frame_delay",
    "qlic_frame_ptr", "qlic_frame_size", "qlic_error_ptr"
  ];
  for (const name of requiredExports) {
    if (!(name in api)) throw new Error(`QLIC WebAssembly export is missing: ${name}`);
  }

  // always make a new view because memory growth detaches the old buffer
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

  async function decodeUrl(url, fetchOptions) {
    const response = await fetch(url, fetchOptions);
    if (!response.ok) throw new Error(`Could not fetch QLIC: ${response.status}`);
    return decode(await responseBytes(response));
  }

  async function decodeBlob(blob) {
    if (!blob || typeof blob.arrayBuffer !== "function" || !Number.isSafeInteger(blob.size)) {
      throw new TypeError("QLIC decodeBlob expects a Blob.");
    }
    enforceInputSize(blob.size);
    const data = await blob.arrayBuffer();
    enforceInputSize(data.byteLength);
    return decode(data);
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
    encode, decode, decodeUrl, decodeBlob, firstImageData, imageData, draw, play
  };
}

async function loadWasm(url, binary) {
  const imports = {};
  if (binary) {
    const result = await WebAssembly.instantiate(binary, imports);
    return result.instance;
  }
  if (typeof WebAssembly.instantiateStreaming === "function") {
    try {
      const result = await WebAssembly.instantiateStreaming(fetch(url), imports);
      return result.instance;
    } catch (_) {
    }
  }
  const response = await fetch(url);
  if (!response.ok) throw new Error(`Could not fetch QLIC WASM: ${response.status}`);
  const result = await WebAssembly.instantiate(await response.arrayBuffer(), imports);
  return result.instance;
}

export default createQlic;
