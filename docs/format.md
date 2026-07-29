# QLIC file format

A still image decodes to RGBA pixels. An animation decodes to RGBA frames with millisecond delays.

## Container

Every file starts with this 28 byte header.

```text
0   4   magic: QLIC
4   4   width
8   4   height
12  1   mode
13  1   transform
14  1   index bits or mode parameter
15  1   codec plus integrity flag
16  4   palette count, tile height, channel count, or frame count
20  8   uncompressed payload size
```

The high bit of byte 15 means a CRC32 follows the body. It covers the header and body together.

## Still images

Still image modes cover grayscale, grayscale with alpha, RGB, RGBA, palettes, separable rows, native streams, filtered streams, tiled streams, reversible tile transforms, and block programs.

Mode 18 contains block programs.

QBL1 stores commands in row order. A command can hold raw pixels, a flat color, a copy from the left or above, a small color map, or a 2 by 2 pattern map.

QBR1 adds exact copies from any earlier block. Backward distance is an unsigned base 128 varint measured in scan order blocks. The encoder keeps QBL1 when no arbitrary reference makes the file smaller.

QCF1 stores a 64 by 64 coordinate field and one exact folded residual stream. Each region stores five 4 bit coordinates in three bytes. The decoder derives flat, gradient, left, up, causal, and linear bases from decoded borders. Those bases are blended before the residual is applied.

QBL2 stores block predictors in row order and one exact folded residual stream. Predictors include zero, flat, a bilinear corner gradient, copies from the left or above with byte deltas, causal Paeth prediction, and PDM predictor coordinates packed as five 4 bit values.

QPD1 stores one PDM coordinate model for each 64 by 64 region and one exact folded residual stream. Decoding takes one pass after reading the residual stream.

The outer header for a native stream always uses the identity transform. QST1 stores the actual predictor mode, color transform, tile setting, and adaptation rate. Container codec store only means QST1 is stored directly. It does not mean the pixels or residuals are raw.

QST1 stores the adaptive range coder update rate in byte 17 when that byte is not holding a constant alpha value. Zero selects the default. Values 4 and 6 select the faster and slower rates. Every other value is invalid.

QST1 flag bits 2 through 4 can store an exact grayscale or RGB sample depth from 1 through 7. The encoder only uses this when every affected value belongs to that depth's full range scaling grid and the complete stream is smaller. The decoder rejects samples outside the stated range, restores them to 8 bit values, then verifies the pixel checksum.

Native mode 41 uses the mode 37 residual syntax with causal predictor map coding. Predictor tiles are visited in raster order. A tile first signals whether it differs from the tile on its left. When it differs, it signals against the tile above if that tile exists and has another predictor. A zero bit selects the referenced predictor. Otherwise, a 5 bit adaptive tree stores the predictor. Left and up decisions use separate probability sets with four contexts for predictor IDs 0, 1 through 4, 5 through 10, and 11 through 31.

Native mode 42 stores the same predictor in independent 128 row bands for each plane. The payload starts with little endian chunk sizes followed by range coded band chunks. The bands decode independently and are joined by the reversible color transform.

Native modes 43 through 53 keep the mode 37 residual and predictor map syntax while adding richer residual contexts. Mode 43 carries causal state between transformed color planes. Mode 44 adds local zero and sign state. Mode 45 adds spatial residual state and can omit the predictor map when the tile log is zero. Modes 46 through 50 add child, coarse, full coarse, root, and slower root probability levels. Mode 51 adds sign state for each predictor. Mode 52 conditions sign state on residual magnitude. Mode 53 refines zero, magnitude, and sign mixtures with update rates based on magnitude. Every finalized mode has a distinct identifier and decoding behavior.

Native transforms 11 through 28 use reversible weighted red and green prediction for the blue plane. Transforms 29 through 34 add a reversible luma lift around the same red and green difference. Transform 35 uses red as the anchor and predicts blue from the red and green average. Native decoders accept transform identifiers through 35.

Every static path produces exact RGBA pixels. Alpha is stored as data and is not composited.

Predictor formulas and reversible transforms are in predictor-math.md.

## Animation

Animation uses mode 17. Older QAN1 files are still decoded. Its four byte marker is followed by the frame count and loop count. Each frame stores:

```text
delay_ms
flags
nested_qlic_size
nested_static_qlic_bytes
```

Each frame is an independent still QLIC image. Nested animation is invalid.

QAN2 starts with the same frame count and loop count. Every frame starts with its delay and one of these types:

```text
0  key frame: nested_qlic_size, nested_static_qlic_bytes
1  duplicate: no additional data
2  rectangle: x, y, width, height, nested_qlic_size, nested_static_qlic_bytes
3  move: source_x, source_y, destination_x, destination_y, width, height, clear_rgba
```

The first frame has to be a key frame. A duplicate copies the previous frame. A rectangle copies the previous frame and replaces one region with the nested image. A move clears the source rectangle with one RGBA color, then copies pixels from the unchanged previous frame to the destination. Nested image dimensions have to match the key frame or rectangle exactly.

## Integrity

The container checksum covers the header, palette, compressed payload, and animation table. Native streams also store a stream checksum and a pixel checksum. The decoder requires and checks all of them.
