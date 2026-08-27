import { createQlic } from "./qlic-web.js";

const qlicReady = createQlic();

function previewWide(image, alphaAssociation = "none") {
  const { width, height, channels, bitsPerSample, samples } = image;
  const maximum = 2 ** bitsPerSample - 1;
  const rgba = new Uint8ClampedArray(width * height * 4);
  for (let pixel = 0; pixel < width * height; pixel++) {
    const source = pixel * channels;
    const target = pixel * 4;
    const scale = channel =>
      Math.round(samples[source + channel] * 255 / maximum);
    rgba[target] = scale(0);
    rgba[target + 1] = channels === 1 ? rgba[target] : scale(1);
    rgba[target + 2] = channels === 1 ? rgba[target] : scale(2);
    rgba[target + 3] = channels === 4 ? scale(3) : 255;
    if (alphaAssociation === "premultiplied") {
      const alpha = rgba[target + 3];
      for (let channel = 0; channel < 3; channel++) {
        rgba[target + channel] = alpha
          ? Math.min(255, Math.round(rgba[target + channel] * 255 / alpha))
          : 0;
      }
    }
  }
  return rgba;
}

function decodedPreview(qlic, source) {
  const mode = source.byteLength > 12 ? source[12] : -1;
  if (mode === 19) {
    const image = qlic.decodeWide(source);
    return {
      kind: "wide",
      width: image.width,
      height: image.height,
      channels: image.channels,
      bitsPerSample: image.bitsPerSample,
      alphaAssociation: "none",
      transfer: null,
      animated: false,
      loopCount: 0,
      frames: [{
        width: image.width,
        height: image.height,
        delay: 0,
        rgba: previewWide(image)
      }]
    };
  }
  if (mode === 20) {
    const image = qlic.decodeHdr(source);
    const transfer = image.cicp && image.cicp.transferCharacteristics === 16
      ? "PQ"
      : image.cicp && image.cicp.transferCharacteristics === 18
        ? "HLG"
        : null;
    return {
      kind: "hdr",
      width: image.pixels.width,
      height: image.pixels.height,
      channels: image.pixels.channels,
      bitsPerSample: image.pixels.bitsPerSample,
      alphaAssociation: image.alphaAssociation,
      transfer,
      animated: false,
      loopCount: 0,
      frames: [{
        width: image.pixels.width,
        height: image.pixels.height,
        delay: 0,
        rgba: previewWide(image.pixels, image.alphaAssociation)
      }]
    };
  }
  const image = qlic.decode(source);
  return {
    kind: "rgba8",
    width: image.width,
    height: image.height,
    channels: 4,
    bitsPerSample: 8,
    alphaAssociation: "straight",
    transfer: null,
    animated: image.animated,
    loopCount: image.loopCount,
    frames: image.frames
  };
}

self.addEventListener("message", async event => {
  const message = event.data || {};
  const { id, operation = "encode", width, height } = message;
  try {
    const qlic = await qlicReady;
    const started = performance.now();
    if (operation === "decode") {
      const decoded = decodedPreview(qlic, new Uint8Array(message.input));
      const transfer = decoded.frames.map(frame => frame.rgba.buffer);
      self.postMessage({
        id,
        ok: true,
        operation,
        decoded,
        elapsed: performance.now() - started
      }, transfer);
      return;
    }
    const source = new Uint8Array(message.rgba);
    if (operation === "png") {
      const png = await qlic.encodePng(source, width, height);
      self.postMessage({
        id,
        ok: true,
        operation,
        png: png.buffer,
        elapsed: performance.now() - started
      }, [png.buffer]);
      return;
    }
    const encoded = qlic.encode(source, width, height);
    self.postMessage({
      id,
      ok: true,
      operation: "encode",
      encoded: encoded.buffer,
      elapsed: performance.now() - started
    }, [encoded.buffer]);
  } catch (error) {
    self.postMessage({
      id,
      ok: false,
      operation,
      error: error && error.message ? error.message : String(error)
    });
  }
});
