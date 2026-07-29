import { createQlic } from "./qlic-web.js";

const qlicReady = createQlic();

self.addEventListener("message", async event => {
  const { id, rgba, width, height } = event.data || {};
  try {
    const qlic = await qlicReady;
    const source = new Uint8Array(rgba);
    const started = performance.now();
    const encoded = qlic.encode(source, width, height);
    const elapsed = performance.now() - started;
    self.postMessage(
      { id, ok: true, encoded: encoded.buffer, elapsed },
      [encoded.buffer]
    );
  } catch (error) {
    self.postMessage({
      id,
      ok: false,
      error: error && error.message ? error.message : String(error)
    });
  }
});
