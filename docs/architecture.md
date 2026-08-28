# QLIC architecture

This implementation guide describes QLIC 1.0. Normative wire rules are in
[format.md](format.md), [hdr-format.md](hdr-format.md), and
[predictor-math.md](predictor-math.md).

## Overview

QLIC is a lossless image codec built around two layers:

1. A small outer container chooses a representation such as native QST1,
   filtered bytes, a compressed palette, horizontal bands, animation, or exact
   wide/HDR samples.
2. QST1 converts channels into reversible integer planes, predicts each plane,
   and range-codes exact residual decisions with adaptive context models.

The encoder runs a measured tournament. It builds an initial candidate, uses
low-cost image signals to gate specialists, stops candidates that cannot reduce
size, and keeps the smallest completed file. Research builds may compare other
routes or entropy mechanisms. The shipped encoder exposes one automatic policy
and emits one result.

The decoder is deliberately simpler than the encoder. The file already says
which representation, transform, predictor mode, tile size, and adaptation
rate won. Decode validates the container, dispatches once, reconstructs exact
pixels, and verifies them with a decoded-pixel checksum.

```text
input image
    |
    v
normalize exact pixels and classify channels/alpha/size
    |
    v
low-cost probes -> initial candidate
    |                    |
    +---- gated specialists
                         |
                         v
                 smallest file
                         |
                         v
              QLIC header + payload + CRC

QLIC bytes
    |
    v
validate limits/header/CRC -> decompress outer payload -> decode selected mode
    |                                                        |
    +--------------------------------------------------------+
                         exact pixels + pixel CRC
```

## Design rules

### Lossless data

RGBA8, animation, QSW1 9--24-bit integer samples, and QSW2 8--24-bit
self-described color/HDR code values are lossless. Color transforms are
reversible integer operations. Alpha is stored, not composited. HDR decode does
not tone-map, change transfer functions, premultiply alpha, or reduce precision.

Lossy source files are rejected by the import layer because lossless encoding
of already-lossy pixels is easy to misrepresent as preservation of the
original source.

### Bounded search

QLIC uses complex transforms, adaptive models, SIMD, profile-guided
optimization, and specialized representations when measurements justify them.
Narrow specialists must not add work to every image.

The ordinary encoder follows this order:

1. Reuse low-cost facts such as dimensions, channel shape, alpha shape, sampled
   palette cardinality, block repetition, transform score, and zero density.
2. Produce an initial candidate.
3. Run a specialist only when an existing fact predicts a possible size gain.
4. Pass the current size into the specialist as a cutoff.
5. Accept actual completed bytes, never a score estimate, as the authority.

### Decoder compatibility outlives encoder routing

Stopping selection of a slow mode does not delete its decoder. Native mode 54
is the clearest example: it remains valid syntax and has permanent fixtures,
but ordinary encoding no longer selects it because about one percent of size
gain cost about thirty percent in decode time.

New syntax is additive. Old decoders reject unknown modes or transforms at an
existing range check instead of interpreting them as another operation.

### Resource limits are part of correctness

Dimensions, file bytes, payload bytes, decoded bytes, pixels, frames, metadata,
and chunk counts are checked before large allocations. Nested streams must
match the dimensions and channel count promised by their outer container.

## Repository map

This tree is the complete product source. The larger development workspace
delegates its root build here.

| Path | Responsibility |
| --- | --- |
| `codec/src/qlic.c` | Outer container, encoder tournament, outer decoders, CLI on Windows |
| `codec/src/stream.c` | QST1 transforms, predictors, adaptive range coder, native routing |
| `codec/src/qlic_api.c` | Stable C ABI, limits, ownership, status conversion |
| `codec/src/input.c` | Exact input inspection and platform import adapters |
| `codec/src/lzms.c` | Portable clean-room LZMS decoder |
| `codec/src/map_avx2.c` | AVX2 predictor-map analysis kernels |
| `codec/src/parallel.c` | Bounded worker scheduling |
| `codec/include/qlic/qlic.h` | Installed C API |
| `rust/qlic-decoder` | Dependency-free safe Rust decoder |
| `web` | Freestanding Wasm codec and JavaScript/browser wrapper |
| `tests` | API, compatibility, fuzz, mutation, parallel, CLI, and WIC gates |
| `docs` | Wire, API, math, design, and architecture contracts |
| `scripts` | Native, PGO, sanitizer, Web, package, and fixture builds |
| `benchmark` | Reproducible benchmark scripts and the internal mode/memory probe sources |
| `tests/fixtures/retained` | Fixed self-contained native and palette conformance streams |

The larger development workspace keeps full corpora, per-experiment traces,
and rejected candidates outside the release source. Normal builds and tests do
not depend on that workspace.

`qlic.c` and `stream.c` are large because the outer tournament and native
format have accumulated proven specialists. The separation is still useful:
outer byte/palette/block choices do not leak into QST1 probability machinery,
and QST1 can be reused inside bands, wide byte slices, HDR, palettes, and
animation.

## Public data paths

### C API

The installed header exposes these main operations:

| Data | Encode | Decode |
| --- | --- | --- |
| Gray8, GrayA8, RGB8, or RGBA8 still | `qlic_encode_pixels` / `qlic_encode_rgba` | `qlic_decode_pixels` / `qlic_decode_rgba` |
| RGBA8 animation | `qlic_encode_animation` | `qlic_decode_animation` |
| 9--24-bit integers | `qlic_encode_wide` | `qlic_decode_wide` |
| Integer HDR + metadata | `qlic_encode_hdr` | `qlic_decode_hdr` |
| Header and metadata only | — | `qlic_get_info_v2` |
| Full integrity check, no retained output | — | `qlic_validate` |

The public ABI uses `struct_size` for extensible options and v2 metadata.
Returned allocations have explicit matching free functions. Error status is
numeric; `qlic_last_error()` adds thread-local detail.

QLIC exposes one automatic encoder policy. Candidate variants may exist in
benchmark builds to answer research questions, but the shipped encoder keeps
one tournament and one completed-file answer. A mechanism that needs a public
speed/density switch is not part of the QLIC 1.0 API.

### Command line

The CLI is intentionally small:

```text
qlic pack input.png output.qlic
qlic unpack input.qlic output.png
qlic info input.qlic
qlic info input.qlic --json
qlic verify input.qlic
```

PNG, WebP, JPEG XL, TIFF, AVIF where available, BMP, and WIC inputs are import
adapters. They are not embedded in QLIC files. Wide and HDR files use native
QSW1/QSW2 payloads, not PNG-in-a-wrapper.

### Windows GUI

The native GUI is a thin local process boundary around the same command-line
encoder. It does not contain a second codec policy. The default surface asks
only for an image; a collapsed disclosure exposes CPU use, an explicit
ICC/CICP color description, and alpha association. Those settings call the
real QSW2 path. There is no effort control.

The GUI reserves its temporary output before starting the child, captures a
bounded error stream, supports cancellation, and runs `qlic verify` before
enabling Save. A result that cannot decode and pass its checksums is never
presented as successful. Keeping the process boundary also means cancellation
can stop all codec work without adding asynchronous state to the public C ABI.

The built-in viewer decodes on a worker thread and keeps exact source RGBA8
pixels separate from any ICC-managed display copy. It supports nearest-neighbor
zoom, pan, pixel inspection, paused animation controls, and current-frame PNG
export. Wide and PQ/HLG samples are explicitly shown as linearly scaled 8-bit
previews without tone mapping; exact samples remain available through the SDK.

The browser page has a narrower import boundary. Browser image decoders and
canvas produce rendered RGBA8; they may apply color management, drop metadata
and animation, reduce high precision, and erase hidden color beneath zero
alpha. The page labels this behavior and directs exact metadata or wide-sample
work to the native app, CLI, or sample APIs.

### Windows WIC

`qlic-wic.dll` is a read-only COM decoder. Initialization decodes and validates
the complete file once under WIC-specific file, pixel, decoded-byte, frame,
chunk, and metadata limits; frames then expose immutable bitmap sources safe
for concurrent COM callers. `QueryCapability` performs the same validation on
a temporary decoder and restores the caller's stream position before returning.

Native pixel formats are 8- and 16-bit Gray/RGB plus distinct RGBA/PRGBA alpha
formats. Authoritative full-range BT.2020/PQ 10-bit RGB is packed losslessly as
WIC `R10G10B10A2HDR10` with opaque alpha. No other precision is reduced to fit
a WIC type. ICC profiles are WIC color contexts. General CICP, mastering, and
content-light records remain available through the QLIC SDK because ordinary
WIC integer bitmap formats have no equivalent general color-context contract.

QSW2 frames also implement `IWICMetadataBlockReader`. Windows' standard
metadata handlers parse valid IFD/EXIF, XMP, and IPTC blocks and the aggregate
query reader exposes paths such as `/ifd/{ushort=274}` and
`/xmp/xmp:CreatorTool`. Unsupported or malformed conventional payloads fall
back to WIC's opaque metadata reader, preserving access without claiming a
parse. PNG `pHYs` supplies frame DPI. There is no encoded preview or thumbnail,
so those methods return the WIC-defined absence results and the shell scales the
full frame when it needs a thumbnail.

### Rust

`rust/qlic-decoder` is a separate implementation:

- standard library only;
- `#![forbid(unsafe_code)]`;
- no C or FFI;
- fallible, limit-checked allocations;
- typed corrupt/limited/unsupported errors;
- current-corpus still, animation, wide, and HDR coverage.

It currently decodes every stream in the accepted 3,167-file corpus plus QAN1
and QAN2 animation. It does not implement every historical/experimental native
grammar. `validate` fully decodes any supported family and discards its output.

### Browser/Wasm

The browser package is a pure static deliverable: `qlic-web.js` and
`qlic-web.wasm` run without a native QLIC executable, DLL, or local service. It
handles RGBA8
still images and animation, including current native streams, LZMS, compressed
palettes, and stored horizontal bands. It returns exact QSW1 9--24-bit and QSW2
8--24-bit samples as typed arrays and preserves QSW2 color/HDR metadata. The
JavaScript wrapper copies returned data out of Wasm memory, exposes TypeScript
declarations, bounds URL
downloads, exposes validation without retained output, and includes a worker
so encoding need not block the page.

The packaged `index.html` is a separate self-contained convenience app. Its
build derives a non-ESM form of the same JavaScript wrapper and embeds it with
the Wasm binary in a blob-backed classic worker. Double-clicked offline use
therefore avoids both module-worker origin restrictions and browser access to
sibling `file://` resources. This embedding is presentation only; integrators
continue to use the normal ESM/Wasm pair.

## Outer file container

Every QLIC file begins with a 28-byte header:

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 4 | `QLIC` magic |
| 4 | 4 | width |
| 8 | 4 | height |
| 12 | 1 | outer mode |
| 13 | 1 | outer transform |
| 14 | 1 | index width or mode parameter |
| 15 | 1 | backend codec plus integrity flag |
| 16 | 4 | palette count, band height, channel count, or frame count |
| 20 | 8 | uncompressed payload size |

Current files set the integrity bit and append CRC32 over the header and body.
The payload may be stored directly or passed through an outer backend.

### Outer modes

Mode numbers are permanent wire identifiers.

| Mode | Name in source | Role |
| ---: | --- | --- |
| 1 | `MODE_GRAY` | grayscale bytes |
| 2 | `MODE_GRAYA` | grayscale plus alpha |
| 3 | `MODE_RGB` | RGB bytes |
| 4 | `MODE_RGBA` | RGBA bytes |
| 5 | `MODE_PALETTE` | palette plus packed indexes |
| 6 | `MODE_SOURCE` | reserved historical identifier; rejected |
| 7 | `MODE_SEPARABLE` | row/channel-separated bytes |
| 8 | `MODE_RESERVED` | rejected |
| 9 | `MODE_NATIVE` | QST1 native stream |
| 10 | `MODE_FILTERED` | row-filtered bytes |
| 11 | `MODE_PSTREAM` | QST1-compressed palette-index stream |
| 12 | `MODE_PPAL` | predictor-run palette form |
| 13 | `MODE_CPAL` | compressed palette family |
| 14 | `MODE_TILES` | independent horizontal QST1 bands |
| 15 | `MODE_TILE_MODEL` | reversible tile-model payload |
| 16 | `MODE_GMODEL` | grayscale model payload |
| 17 | `MODE_ANIM` | QAN1/QAN2 animation |
| 18 | `MODE_BLOCKS` | block programs and coordinate fields |
| 19 | `MODE_NATIVE_WIDE` | QSW1 9--24-bit integer samples |
| 20 | `MODE_HDR_WIDE` | QSW2 self-described 8--24-bit integer color/HDR |

Not every mode is selected often. Outer modes exist because the byte-level
structure of icons, screens, palettes, repetition-heavy assets, animation, and
wide samples differs from photographic residual structure.

### Outer compression backends

| Backend | Use |
| --- | --- |
| Store | QST1 and already compact payloads; also conformance fixtures |
| XPRESS | Windows byte-compression candidate/proxy |
| XPRESS Huffman | Low-cost ranking proxy before LZMS trials |
| LZMS | Long-range backend for selected filtered/palette/block payloads |

LZMS decode is portable and clean-room. Windows compression APIs or wimlib are
encoder-side options; a QLIC decoder does not require a Windows API.

QLIC does not run LZMS over QST1 by default. QST1 is already entropy-coded, so a
second backend normally adds work without finding structure.

## QST1 native stream

QST1 is the main photographic and textured-image representation. A QST1 object
has a 30-byte header followed by an optional small palette and one range-coded
payload.

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 4 | `QST1` marker |
| 4 | 4 | width |
| 8 | 4 | height |
| 12 | 1 | channels: 1, 3, or 4 |
| 13 | 1 | grayscale, constant-alpha, and sample-grid flags |
| 14 | 1 | native mode |
| 15 | 1 | reversible color transform |
| 16 | 1 | predictor tile log |
| 17 | 1 | constant alpha or adaptation control |
| 18 | 4 | decoded-pixel CRC32 |
| 22 | 4 | entropy payload length |
| 26 | 4 | QST1 self-CRC |

The C decoder normally reaches QST1 only after the outer container CRC has
authenticated the same bytes, so its hot trusted QST parser does not repeat a
second full compressed-byte CRC pass. The decoded-pixel CRC remains mandatory.
The safe Rust parser also validates the QST1 self-CRC because it exposes QST1 as
an independently parsed object internally.

### Channel handling

- One-channel input stores one plane.
- Pixel-gray RGB may keep the declared three-channel shape while coding one
  gray plane and replicating it during reconstruction.
- RGB stores three reversible transformed planes.
- RGBA stores those three color planes plus alpha.
- Constant alpha is a header byte and has no entropy plane.
- Variable alpha is an independent fourth native plane; color planes may still
  use alpha-aware outer routing.

If every RGB/gray value lies exactly on a 1--7-bit full-range scaling grid,
QST1 may code the smaller integers, restore the exact 8-bit grid values, and
verify the original pixel checksum. It is not quantization.

### Reversible color transforms

Transforms 0--40 are integer and exactly invertible. The families are:

| IDs | Family |
| --- | --- |
| 0 | identity RGB |
| 1--10 | green, red, blue, and YCoCg-style anchors/deltas |
| 11--28 | weighted red/green prediction for blue |
| 29--34 | reversible luma lift plus red/green difference |
| 35 | red anchor; blue predicted from average red/green |
| 36--37 | blue anchor with fixed 40/24 cross-color blends |
| 38 | quadratic tangent-space normal-map predictor |
| 39 | bounded integer spherical normal-map predictor |
| 40 | transform 39 with green kept at native eight-bit depth |

Most transformed difference planes need nine bits because the reversible range
is wider than 0--255. Transform selection uses a fixed sparse sample and
predictor-shaped cost, then exact candidate bytes. The nonlinear normal-map
transforms never enter the broad ordinary tournament; each gets one bounded
mode-52 trial when its measured score crosses the gate.

The formulas and floor-rounding rules are in
[predictor-math.md](predictor-math.md). Floor behavior for negative integers is
part of the format and cannot be replaced by implementation-defined signed
right shift.

### Spatial prediction

Each plane is reconstructed in raster order. Predictors use already-decoded
neighbors such as west, north, northwest, northeast, farther row/column values,
and causal residual history. Predictor IDs are chosen per tile where the mode
supports a map.

The map is analysis metadata, not a second image. Encoder AVX2 kernels score
predictors over tiles, and the scalar entropy loop consumes the chosen ID. Map
syntax can reuse the predictor to the left or above before coding a new ID.

Tile logs trade local adaptation against map overhead and locality. Current
ordinary photographic paths mostly use 8-by-8 or 16-by-16 predictor tiles.
Exact completed bytes decide between the retained sizes.

### Residual representation

For a predicted sample, QST1 codes the exact signed residual. The context modes
generally separate these decisions:

1. zero/nonzero;
2. unary magnitude class `k`;
3. lower mantissa bits;
4. sign relative to a causal sign hint.

Each binary decision is range-coded and updates its selected probability. This
gives excellent per-image adaptation with no transmitted histogram, but it also
creates the main decoder dependency chain.

### Native mode families

The current encoder output is concentrated in a few QST modes:

| Native mode | Main behavior |
| ---: | --- |
| 0 | base native plane coder |
| 1 | small native palette |
| 37 | bounded predictor-map and context path; important faster alternative |
| 39 | sparse event-order grammar |
| 40 | compact pattern grammar |
| 45 | spatial residual context; can omit a predictor map |
| 52 | local/root/exact-magnitude-sign contexts; main photographic path |
| 53 | refined zero, magnitude, sign, and update-rate mixtures |
| 54 | mode 53 plus causal weighted predictor 31; decode-only by default |

Historical modes 2--36 and 38, 41--44, 46--51 remain syntax where implemented,
but ordinary routing is deliberately concentrated. Modes 43--54 form a staged
context family on top of mode-37 residual and map syntax. Mode 52 is the center
of the current codec because it retains enough image-specific adaptation to
beat static alternatives while avoiding mode 54's weighted-predictor cost.

Mode 52 and 53 have constant-specialized first, middle, last, and independent
plane decoder entry points. The dispatch happens once per plane. Color-plane
state is causal: the first plane produces state, the middle plane consumes and
updates it, and the last consumes it. Alpha remains independent.

### Range coder and probability state

QST1 uses one adaptive binary range state across the native payload. Probability
entries are small integer state, updated by shifts selected from adaptation
rate 4, 5, or 6. Rate 5 is the default; 4 and 6 are explicit wire controls.

The hot model is deliberately mutable and image-local. This is why a static
rANS/FSE substitution is not automatically equivalent: QLIC's retained QST2
lab found that shared static profiles lost 1.8--2.2% on untouched holdout before
paying table/framing overhead.

The current mode-37-family probability grid stores only reachable mantissa
states. QST planes are at most nine bits, so magnitude class is only `2..9` and
mantissa bit position only `0..7`. An 8-by-8 rectangle removes 34,224 bytes of
hot mutable state and improves full-corpus native-PGO decode 0.597% without
changing one encoded decision.

## Outer specialist representations

### Filtered and separable bytes

Byte rows can use identity, sub, up, average, and Paeth-style filters. The
encoder chooses per row and may separate channels before compression. These
paths are useful when a general byte compressor sees structure more directly
than a native residual model.

Outer transform 11 goes farther: it stores B, R-B, G-B, and optional alpha as
whole modulo-256 planes after a causal median-edge filter, then uses LZMS. The
decoder reconstructs the planes in place and fuses blue-delta inversion with
RGBA emission.

### Palettes

QLIC has several palette forms because one layout is not best for every palette:

- ordinary packed global indexes;
- index runs and predictor runs;
- QST1-compressed index streams;
- CPAL payloads behind byte compression;
- tile-local palettes referencing a global palette;
- channel-planar palettes for 257--65,536 colors;
- a direct large-image palette builder that avoids a full-image `uint16_t` ID
  array; for more than 256 exact colors, the 128 colors that begin the most
  raster runs also compete in the one-byte varint-ID range while both palette
  partitions remain lexically ordered.

Palette probes are low-cost and bounded. Exact palette construction is delayed
until the incumbent and sampled cardinality justify it. XPRESS-Huffman ranks
candidate payloads so only the best one or two run LZMS. Complete file size
still decides.

### Blocks and repetition

Mode 18 can represent raw blocks, flat colors, left/up copies, small maps,
patterns, earlier arbitrary block references, coordinate fields, and folded
residual programs. These paths target sprite sheets, UI assets, textures, and
repetition that a purely local predictor misses.

Fixed-grid probes are supplemented by a bounded off-grid repeated-span probe.
Hashes are only a classifier; hits are verified byte-for-byte before an LZMS
trial.

### Horizontal bands

Outer mode 14 stores independently checked QST1 bands plus a length table. It
solves two problems:

- very large or vertically nonstationary images can reset contexts;
- selected tall RGBA assets can use much less peak decode memory and decode in
  parallel.

The accepted RGBA route is intentionally narrow: native mode 45/transform 2,
at least three million pixels, at least 2:1 tall, a fixed vertical histogram
variation signal, no more than 256 bands, and at least 8 KiB actual saving.
Only two corpus files select it. Broad band search was rejected because it
added too much encode work on holdout data.

### Animation

Mode 17 supports independent key frames, duplicates, changed rectangles, and
pixel moves. The first frame is a key frame; nested animation is invalid.
Decoded output is coalesced RGBA frames with millisecond delays and loop count.

C, safe Rust, and browser/Wasm implement this path.

## Wide integer and HDR paths

### QSW1

Mode 19 preserves one, three, or four unsigned channels at 9--24 bits. QSW1
does not introduce a second wide entropy coder. It splits each numeric sample
into two or three little-endian byte slices and encodes each slice as ordinary
QST1. Decode reassembles native `u16` or `u32` samples and verifies a canonical
endianness-independent sample CRC.

This design was chosen because it reuses the optimized, fuzzed byte decoder,
keeps every RGBA8 stream unchanged, and makes the first wide format small and
auditable. The tradeoff is that cross-byte sample correlation is indirect.

### QSW2

Mode 20 wraps the same byte-sliced pixel payload with a versioned self-describing color/HDR
header and chunks:

- `PIXL`: required exact sample payload;
- `ICCP`: ICC profile;
- `CICP`: primaries, transfer, matrix, and range;
- `MDCV`: mastering display;
- `CLLI`: MaxCLL and MaxFALL.

Other non-critical chunks are ordered opaque photographic/application
metadata. Current C, Rust, and Web decoders retain their tags and bytes; the
CLI uses `EXIF`, `XMP_`, `IPTC`, and `JUMB` for common records.

Color authority states which description wins when ICC and CICP coexist. Alpha
is none, straight, or premultiplied. Unknown ancillary chunks can be skipped;
current decoders instead preserve them, while unknown critical chunks fail
closed.

Current HDR is unsigned integer only. Floating-point/half-float preservation and
incremental pixel decode are not implemented.

## Encoder architecture

The main outer entry is `enc_best_base()` in `qlic.c`. It is easier to
understand as phases than as one flat tournament.

### 1. Normalize and classify

The input layer supplies exact RGBA or wide samples. The encoder determines
gray/gray-alpha/RGB/RGBA shape, pixels, constant alpha, low-cardinality palette
signals, noise, separability, block repetition, and other bounded facts.

Very noisy images can go directly to a raw bounded fallback. This prevents an
incompressible input from paying for deep prediction search only to produce a
larger result.

### 2. Build an early incumbent

For ordinary images, QST1 is the initial candidate. `stream.c` samples
reversible transforms, chooses a short trial list, runs native candidates, and
retains exact bytes. Transform-plane and predictor-map work is cached across
related trials where doing so measured better than recomputation.

Small separable, obvious block, or compact-palette inputs can establish an
outer incumbent first.

### 3. Escalate from evidence

Examples:

- exact low color count gates palette work;
- block/repeated-span signals gate LZMS payloads;
- sparse endpoint alpha gates one CPAL attempt;
- normal-map residual score gates transform 38, 39, or 40;
- stable transform/zero-density pockets gate a mode-37 trial;
- a narrow tall-RGBA signal gates one band height;
- completed mode 53 through one megapixel gates one strict-smaller mode-52
  fallback;
- a completed color mode-52 stream over one megapixel using 8-by-8 tiles gates
  one 16-by-16 retry, retained only when the complete stream is smaller;
- a direct large-image CPAL payload above 256 colors gates one hot-run palette
  ordering beside the lexical candidate, retained only when the complete file
  is smaller.

No path, filename, or corpus category participates in production routing.

### 4. Limit every candidate

Native range encoding, band building, CPAL payload construction, and LZMS
trials receive size cutoffs. A candidate that can no longer beat the incumbent
returns early and releases its buffer. This is essential: strict-smaller final
selection prevents output regression, but without an early cutoff it would not
prevent wasted CPU and RAM.

### 5. Store the winner

The final `Candidate` owns mode parameters, optional palette, compressed body,
and uncompressed length. The outer writer emits one header and CRC. Encoder
search policy is not serialized; only the winning representation is.

### Threading

Candidate trials and independent bands can use bounded workers. The default is
one thread so benchmark and application behavior is predictable. `--threads N`
or API options opt into more. Thread count is clamped to hardware and format
resource limits.

The main QST1 range state inside a plane is serial. More threads do not make one
monolithic plane arbitrarily parallel.

## Decoder architecture

### C decoder

The public API validates options and ownership in `qlic_api.c`, then calls the
core decoder in `qlic.c`.

1. `rd_head_limited()` validates magic, mode, parameters, dimensions, declared
   sizes, caller limits, and outer CRC.
2. The selected outer backend is decompressed into a bounded payload when
   needed.
3. The mode-specific decoder validates its internal table and exact expected
   size.
4. Native mode 9 calls the trusted expected-dimensions QST1 decoder.
5. QST1 decodes plane residuals, applies inverse color transform, restores
   constant/variable alpha and sample grids, then verifies the decoded-pixel
   CRC.
6. Any failure frees partially owned output before returning.

Native RGB decode deliberately reuses the output allocation as temporary plane
and cross-state storage where safe. This keeps memory below a naive
three-planes-plus-RGBA implementation, although whole-file APIs still require a
full output allocation.

### Rust decoder

The Rust implementation mirrors validation order but uses checked slices,
fallible vectors, and typed errors. It has independent safe implementations of
outer parsing, LZMS, QST modes selected by the current encoder, all current
color transforms, horizontal bands, QSW1, and QSW2.

Mutation tests repair enclosing CRCs so malformed inner syntax is actually
reached. `catch_unwind` gates ensure corrupt inputs return `Result` rather than
panic.

### Wasm decoder

Wasm is freestanding C with browser-specific allocation and API glue. It shares
the production QST, QSW1, and QSW2 decoding paths but not the native Windows
loader. Browser limits are lower than desktop defaults. Wide and HDR data are
copied into owned JavaScript typed arrays before the Wasm arena is reset.

## CPU-native implementation choices

### Why the hot loop is scalar

For each QST context-coded pixel, the decoder:

1. reads causal reconstructed neighbors;
2. computes prediction, activity, and residual-history contexts;
3. selects several mutable probability entries;
4. decodes zero, magnitude, mantissa, and sign decisions;
5. updates probabilities;
6. reconstructs the sample for the next pixel.

The range/code state and west-neighbor dependency form a serial chain. A wide
SIMD loop cannot decode eight adjacent pixels without changing the
format or modeling. Most accepted SIMD work therefore sits beside the chain:
predictor-map scoring, transform analysis, byte-plane operations, and copies.

### PGO and native builds

`QLIC_NATIVE=ON` uses `-march=native` under Clang/GCC. It is suitable for a
build deployed on the same CPU family, not a portable public binary.

Decode-only LLVM PGO is trained with retained QLIC streams. This gives the
compiler real branch frequencies and hot paths without charging the training
run to users. Portable builds still work; PGO is an optimization, not a format
requirement.

Matched PGO matters. Several attractive source changes appeared faster in a
stale or unprofiled build and regressed after both sides received their own
full-corpus profile.

### Small accepted layout change

The reachable 8-by-8 mantissa grid is representative of the preferred decoder
work: no wire change, no new branch, smaller mutable state, and a broad
same-core measurement. A tighter triangular packing was rejected because the
saved state required more address arithmetic and did not generalize.

### Accepted compiler-specific rate specialization

The common mode-52 adaptation rate is now specialized once per plane on
Windows Clang/x64. Adaptations 4 and 6 stay on the generic path. This is deliberately
compiler-specific: the matched-PGO binary grows 36,352 bytes, so architectures
that have not demonstrated a speed win keep the smaller decoder.

The code and both PGO profiles were frozen before a 243-image official Godot
asset holdout was acquired. Exact reciprocal timings improve the directly
affected 69 streams by 1.661%, all mode-52 streams by 1.255%, and all recorded
whole-holdout campaigns by 0.870%. One of four short whole-holdout campaigns
regressed 0.580% and is retained in the report. This is a wire-neutral CPU
optimization, not a content route; details are in
`benchmarks_and_tools/holdout-godot-assets/RESULTS.md`.

### Rejected micro-optimizations

The ledger closes many intuitive ideas:

- predictor-family lookup table: dependent load lost to predictable compares;
- earlier default-adaptation clones: fewer variable shifts but larger hot code;
- predictor tile-run loop: hoisted dispatch but worse layout/branch behavior;
- tile-row product hoist: compiler already handled the useful part;
- explicit cross-plane state caching: compiler already kept it live;
- branchless probability update and one-shift variants: slower after PGO;
- semantic multi-range lanes: more framing and no speed gain;
- adaptive six-symbol residual head: +1.534% size and +13.622% decode time;
- adaptive four-symbol nonzero length: +0.378% size and +13.937% decode time;
- fixed lower-tail bypass: the development hotset showed -2.778% decode, but
  frozen CLIC holdout was +0.0101% bytes, +3.227% encode CPU, and +0.340%
  matched-PGO decode;
- independent per-plane range states: +0.83% RGB and +0.93% RGBA sequential
  decode despite negligible framing cost;
- broad mode-37-to-52 cross-transform retry: 303 KB smaller in the corpus but
  +36.520% decode time on every changed stream as one selected set;
- broad mode-54 weighting: smaller but far slower to decode.

These are kept in `BENCHMARK_FAILURE_LEDGER.md` so they are not repeatedly
rediscovered.

## Memory model

Default limits are broad desktop limits, not recommendations for every service:

| Resource | Default C limit |
| --- | ---: |
| file bytes | 512 MiB |
| payload bytes | 512 MiB |
| pixels | 67,108,864 |
| animation memory | 512 MiB |
| decoded bytes | 512 MiB |
| metadata | 16 MiB |
| frames | 100,000 |
| chunks/bands | 256 |

Applications handling untrusted files should set tighter values and enforce an
external time budget. Limits prevent oversized allocation; they are not a
general CPU-complexity proof for every valid file.

Encoder memory is bounded by candidate-specific caps and by freeing losing
buffers promptly. Large exact palettes use a direct builder specifically to
avoid a two-byte ID per source pixel. Its alternate palette ordering adds at
most about 48 KiB of temporary builder state at the 4,096-color cap. Band
decoding can reduce selected peak memory because each nested stream is bounded,
though the current whole-image C API still allocates final output first.

## Integrity and malformed data

QLIC uses layered checks:

- outer header/parameter validation;
- outer header+body CRC;
- exact payload/table/trailing-byte validation;
- nested width/height/channel validation;
- QST1 decoded-pixel CRC;
- QSW1 canonical full-precision sample CRC;
- QSW2 metadata and authority validation.

Checksums are not memory-safety boundaries by themselves. The test suite also
covers truncation, repaired-checksum mutation, invalid indexes, chunk overflow,
dimension arithmetic, allocation limits, nested shape mismatch, unsupported
syntax, and panic-free Rust errors.

The canonical C decoder is built under ASan/UBSan. The Rust decoder forbids
unsafe code. Linux and Windows export lists are checked so internal helpers do
not accidentally become ABI.

## Benchmark method

The main corpus has 3,167 images and 1,964,362,720 pixels from natural photos,
screenshots, games, UI, icons, textures, objects, plants, and synthetic cases.
Every retained codec output is decoded and compared with the source. The corpus
is a development gate, not a universal ranking.

Routing constants and grammar rules are frozen before holdout evaluation.
Acceptance requires source-separated development reporting and a sealed
post-design corpus; a failed holdout is recorded and rejected rather than used
to tune another threshold. Reports also include per-source results and savings
concentration so a large aggregate result cannot hide dependence on one family
or a few files.

### Compression and encode speed

The accepted 3,167-image checkpoint is 1,160,913,984 bytes. Its pinned
one-thread release campaign took 618.673 seconds. Every file decoded exactly.
The source tree, binaries, corpus, codec versions, affinity, and result rows
remain in the development evidence workspace. QLIC, JPEG XL effort 9, and WebP
preset 6 were rerun together; the other selected effort rows are retained
same-machine context:

| Codec | Effort | Bytes | Encode seconds | Bytes vs QLIC |
| --- | ---: | ---: | ---: | ---: |
| QLIC | current | 1,160,913,984 | 618.673 | — |
| JPEG XL | 1 | 1,546,891,144 | 111.053 | +33.248% |
| JPEG XL | 3 | 1,311,022,795 | 274.373 | +12.930% |
| JPEG XL | 6 | 1,214,206,451 | 754.620 | +4.591% |
| JPEG XL | 8 | 1,171,971,513 | 2,977.934 | +0.952% |
| JPEG XL | 9 | 1,164,424,898 | 5,674.173 | +0.302% |
| WebP | 1 | 1,339,854,110 | 296.869 | +15.414% |
| WebP | 6 | 1,289,518,274 | 585.964 | +11.078% |
| WebP | 9 | 1,256,259,092 | 9,032.945 | +8.213% |
| OxiPNG | 6 | 1,618,637,169 | 9,105.634 | +39.428% |

QLIC is 3,510,914 bytes smaller than JPEG XL effort 9. It is smaller on 1,861
files, larger on 1,304, and tied on two. This is a strong aggregate result, not
proof that QLIC is smaller on every file or dataset.

The wider qualification line has 10,628 images. Before the current tile retry,
QLIC used 4,613,558,918 bytes, JPEG XL effort 8 used 4,607,544,830, and effort
9 used 4,586,294,329. The bounded large-mode52 16-by-16 retry plus the direct
CPAL hot-run ordering lower QLIC to 4,606,862,554 bytes: 682,276 below effort 8
and 20,568,225 above effort 9. Across the whole line they produce 722 smaller
files, 9,906 ties, no growth, and no decode or pixel mismatch. The tile retry's
paired line encodes in 2,009.02 seconds, about 5.28 times faster than effort 8
and 8.99 times faster than effort 9; a focused A/B projects about three more
seconds for the CPAL alternative but is not a replacement full-corpus timing.
The tile retry closes 71.36% of mode 52's effort-9 gap. The remaining
compression gap is led by mode 53, where the evidence points to JPEG XL's
learned entropy-context partition rather than one missing predictor.

### Decode speed

The final release decoder sweep uses the same 3,167 files and exact final
streams as the encode matrix. Each codec performs one untimed exact decode,
then three measured pinned process runs per file. Wall time includes process
startup and output file I/O.

| Decoder | Seconds | MP/s | Relative to measured QLIC |
| --- | ---: | ---: | ---: |
| QLIC portable release | 236.927 | 8.291 | baseline |
| JPEG XL effort 9 | 307.602 | 6.386 | 29.83% slower |
| WebP preset 6 | 117.887 | 16.663 | 50.24% faster |

The portable QLIC release uses 22.98% less time than JPEG XL effort 9. The
result includes the accepted transform and reachable-probability-grid work
instead of estimating their effect from older paired campaigns or substituting
the faster native-PGO lab binary.

The product-level weakness is plain: WebP decodes faster, and PNG-family
decoders are faster in the retained sweep. Their files are also materially
larger on this corpus. QLIC currently chooses density and reasonable CPU over
instant decode.

### Current output distribution

The current 3,167-file result is concentrated:

| Final native mode | Files | Bytes |
| ---: | ---: | ---: |
| 52 | 1,621 | 991,584,064 |
| 37 | 129 | 58,134,563 |
| 53 | 76 | 27,112,318 |
| 1 | 671 | 17,537,242 |
| 45 | 46 | 16,099,073 |
| 0 | 82 | 14,274,293 |
| 39 | 6 | 25,418 |
| 40 | 2 | 1,983 |
| outer representation | 534 | 36,145,030 |

Mode 52 dominates bytes. Decoder work should target its serial entropy path,
not spend equal effort on every historical mode.

## Measured strengths

- On the retained corpus, JPEG XL effort-9-level density with lower encode
  time.
- On that corpus, lower decode time and a slightly smaller total than JPEG XL
  effort 9.
- Exact RGBA, variable alpha, animation, 9--24-bit native-wide integers, and
  8--24-bit self-described color/HDR metadata without embedding another image
  format.
- Adaptation across natural photos, normal maps, screens, palettes,
  repetition-heavy assets, and locally low-cardinality textures.
- Pure safe-Rust decode and a browser package.
- Explicit limits, checksums, compatibility fixtures, sanitizers, and retained
  failed experiments.
- Encoder search without equivalent decoder search.

## Limits and edge cases

### Decode throughput

The adaptive binary range chain is roughly 6.75 times slower than WebP and 11.7
times slower than WIC PNG in the release corpus sweep. Inverse transform and CRC
are small parts of the measured native path; about 98% of selected hot-stream
time is plane entropy decode.

### Per-file compression losses

Aggregate totals hide individual losses. JPEG XL and WebP each win on subsets,
especially where their representation matches a source class better. QLIC
therefore keeps a per-file failure ledger and requires new
routes to pass discovery/holdout, completed-size, decode, memory, and fuzz
gates. A net corpus win alone is not enough.

### Whole-file APIs

The core APIs consume a complete file and return a complete image/frame set.
There is no streaming row callback, bounded incremental decode, general ROI
decode, or progressive preview. Horizontal bands are a limited internal form,
not a complete streaming API.

### HDR scope

HDR is exact unsigned integer sample storage plus ICC/CICP/mastering metadata.
It is true preservation of integer PQ, HLG, linear, or other explicitly
described code values: the codec never tone-maps or converts them. It is not a
half/float EXR carrier, RAW developer, gain-map renderer, or color-management
pipeline. Browser QSW1/QSW2 decode returns exact samples and metadata but
does not interpret or display-manage the color space for the application.

### Ecosystem maturity

QLIC is not standardized, broadly deployed, or supported by common browsers and
operating systems. Rust API stability has not reached 1.0. Companies should pin
the exact decoder and keep a conventional interchange fallback.

### Platform coverage

Automated gates cover Windows x64 and Linux x86-64. Other architectures may
work but are not part of the current acceptance matrix. `-march=native` builds
are machine-family-specific. PGO is compiler/profile-specific but remains
portable across the supported baseline ISA when native code generation is off.

### Encoder complexity

The evidence-gated tournament is large. New contributors need the
failure ledger and architecture guide to understand why apparently redundant
routes exist. Removing an old decoder mode or broadening a low-cost gate can
create
compatibility or CPU regressions far from the edited code.

## Practical decoder research

The next experiment should preserve QST1's adaptive modeling rather than use a
static backend that the QST2 lab already showed is too large.

### Plane row pipeline

Current color planes already have separate probability model resets, but share
one continuous range stream. A benchmark-only successor can give color planes
separate range states/lengths while preserving the exact probability contexts
inside each plane.

The first framing control is complete: 32 exact mode-52 development streams gained
only 929 bytes over 67,105,572 (+0.001384%), but sequential decode was 0.829561%
slower. Five RGBA streams were 0.93% slower. The benchmark codec path was
removed after recording the result. Separate states preserve density, but any
successor must show a speed gain from a real row pipeline, not just restart the
same scalar work.

Cross-plane state makes the planes dependent, but only causally by row. A
three-stage pipeline is possible in principle:

```text
stage 0: decode plane 0 row y+2 -> state row
stage 1: decode plane 1 row y+1 using plane 0 state
stage 2: decode plane 2 row y   using combined state -> final RGB(A)
```

This resembles wavefront processing: retain adaptation, expose a few coarse
CPU lanes, and avoid independent tiny tiles with large boundary loss. With row
ring buffers it may also reduce full-plane scratch toward `O(width)`.

Required gates:

- fixed current transforms, predictors, residuals, and probabilities;
- sequential decoder first, proving density/framing cost;
- row-step decoder second, proving exact cross-plane state order;
- at least 1.25x large-image decode with no payload loss, or 1.5x with at most
  0.35% payload cost;
- one-thread fallback for small images;
- bounded row memory, checksums, truncation, and mutation fuzzing;
- independent holdout and completed-file selection.

### Residual head and measured bypass tails

QST1 currently decodes magnitude class through serial unary binary decisions.
Two conventional adaptive multi-symbol replacements have now failed: combining
zero and magnitude into six symbols, and retaining zero while replacing only
nonzero length with four symbols. Both were materially slower and larger. Their
CDF updates and serial symbol scans cost more than QLIC's short, well-predicted
binary chain. A future fixed/table transition must prove less work at the
mechanism level before becoming another wire candidate.

The full context trace found 116,785,794 mantissa decisions. At the current
child-context granularity their ideal binary entropy is 107,392,435 bits, an
8.043238% saving over raw bits. Bypassing every tail bit would therefore be a
real density loss. A fixed bypass of the lower positions of magnitude widths
4--7 appeared 2.778% faster on its development hotset, but failed unchanged on
the official CLIC 2024 test source: +0.0101% bytes, +3.227% encode CPU, and
+0.340% matched-PGO decode. The prototype was removed. This is the concrete
reason future residual work must bring a materially different model instead of
moving the same bypass boundary.

### Ideas not to repeat unchanged

- static rANS/FSE profiles: 1.8--2.2% holdout loss before framing;
- four semantic adaptive range lanes: 28--30 bytes and 0.087% slower;
- conventional adaptive multi-symbol heads: larger and about 14% slower;
- fixed lower-tail bypasses selected from the current trace: failed frozen
  source holdout;
- broad independent tiles: boundary/context cost is unproven and likely high;
- SIMD inverse transforms as the main project: measured below one percent of
  hot native decode;
- more mutable context levels: modes 53/54 already show diminishing CPU return.

The changing research record belongs in the development workspace at
`benchmarks_and_tools/DECODE_RESEARCH.md`, not in the wire specification.

## How to change QLIC safely

For an encoder-only route:

1. Freeze a current full-corpus baseline.
2. Use content-derived features only.
3. Bound candidate work and memory before it starts.
4. Require completed-file size and exact pixels.
5. Measure encode, decode, and memory on selected files and full corpus.
6. Validate on an untouched source/hash holdout.
7. Keep old decoder syntax.

For a decoder change:

1. Build both sides from the same source state and compiler.
2. Train matched full-corpus PGO profiles.
3. Pin to the same CPU core and reverse DLL order.
4. Sum elapsed seconds, not per-shard percentages.
5. Require byte/pixel compatibility.
6. Run native tests, safe-Rust tests, Wasm, ASan/UBSan, and malformed fixtures.
7. Reject a narrow hotspot win when the complete corpus regresses.

For a wire change:

1. Keep it behind a benchmark-only build until density and speed gates pass.
2. Assign additive identifiers; old decoders must fail closed.
3. Write the parser and resource limits before ordinary encoder promotion.
4. Implement C, Rust, and Wasm support where the product claims support.
5. Add permanent compatibility bytes and repaired-checksum mutation tests.
6. Document exact integer math and validation order.

The current mode-53 compression hypothesis has a benchmark-only implementation
in `benchmark/gradient-topology-replay.md`. It compares fixed 4-, 8-, and
16-class gradient geometry against ideal conditional entropy, a causal KT
fragmentation check, and a small shared probability-bank replay. The tool does
not assign a wire identifier or alter ordinary builds.

## Reproduction map

- Build and test native C: `build.ps1`
- Build Clang native/PGO/sanitizer: `scripts/build-clang.ps1` and
  `scripts/build-pgo.ps1`
- Build safe Rust: `scripts/build-rust.ps1`
- Build browser/Wasm: `scripts/build-web.ps1`
- Inspect a stream: `qlic info file.qlic`
- Current effort sweep: `benchmarks_and_tools/codec-efforts-2026-08-15`
- Current decode sweep: `benchmarks_and_tools/decode-efforts-2026-08-15`
- Full accepted/rejected history:
  `benchmarks_and_tools/BENCHMARK_FAILURE_LEDGER.md`
- CPU and entropy research:
  `benchmarks_and_tools/CPU_NATIVE_DIRECTION.md`

## Glossary

| Term | Meaning |
| --- | --- |
| outer mode | container-level pixel/palette/block/band representation |
| QST1 | native 8-bit plane transform/prediction/range stream |
| QSW1 | exact 9--24-bit byte-slice wrapper around QST1 |
| QSW2 | self-described 8--24-bit integer color/HDR wrapper around byte-sliced QST1 streams |
| CPAL | compressed-palette outer family |
| incumbent | best completed candidate currently owned by the encoder |
| proxy | low-cost estimate used only to decide whether work is worth trying |
| completed-size gate | final exact byte comparison that can reject a proxy miss |
| predictor map | per-tile choice of causal spatial predictor |
| adaptation rate | probability-update shift stored by QST1 |
| matched PGO | baseline and candidate each trained with their own full profile |
| wire-neutral | changes implementation but produces the same encoded decisions |
