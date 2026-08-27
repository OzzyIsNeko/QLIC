import createQlic, {
  createQlic as createNamedQlic,
  type CreateQlicOptions,
  type QlicApi,
  type QlicBytes,
  type QlicFrame,
  type QlicImage,
} from "../web/qlic-web.js";

async function checkTypes(bytes: QlicBytes, canvas: HTMLCanvasElement, blob: Blob) {
  const options: CreateQlicOptions = {
    wasmUrl: new URL("qlic-web.wasm", location.href),
  };
  const qlic: QlicApi = await createQlic(options);
  const sameApi: QlicApi = await createNamedQlic();
  const image: QlicImage = qlic.decode(bytes);
  const frame: QlicFrame = image.frames[0];
  const encoded: Uint8Array = sameApi.encode(frame.rgba, frame.width, frame.height);
  const png: Uint8Array = await sameApi.encodePng(frame.rgba, frame.width, frame.height);
  const fetched: QlicImage = await qlic.decodeUrl("image.qlic");
  const fromBlob: QlicImage = await qlic.decodeBlob(blob);
  const data: ImageData = qlic.imageData(frame);
  qlic.firstImageData(encoded);
  qlic.draw(fetched, canvas);
  const stop: () => void = qlic.play(fromBlob, canvas);
  stop();
  return { data, png };
}

void checkTypes;
