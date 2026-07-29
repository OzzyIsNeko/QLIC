# QLIC browser build

Keep qlic-web.js and qlic-web.wasm in the same directory. The browser build encodes still images and decodes files produced by this release.

```html
<canvas id="image"></canvas>
<script type="module">
  import { createQlic } from "./qlic-web.js";

  const qlic = await createQlic();
  const encoded = qlic.encode(rgba, width, height);
  const image = await qlic.decodeUrl("/images/picture.qlic");
  qlic.draw(image, document.getElementById("image"));
</script>
```

encode takes packed RGBA pixels, width, and height. It returns a Uint8Array containing the QLIC file.

decode, decodeUrl, and decodeBlob return this:

```js
{
  width,
  height,
  frames: [{ width, height, delay, rgba }],
  loopCount,
  animated
}
```

draw renders one frame. play renders an animation and returns a function that stops it.

The default limits are 256 MiB for input and payload data, 33,554,432 pixels, 4,096 frames, and 256 MiB of decoded animation pixels.

demo.html runs compression through qlic-worker.js so the page stays responsive. Serve all four files from the same directory. The Windows demo is a separate download.
