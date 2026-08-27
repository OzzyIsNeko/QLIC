# QLIC predictor math

Predictors change how residuals are represented before entropy coding. They do not change the decoded pixels.

## Byte filters

A filtered row stores one filter byte followed by its residual bytes. For sample x, left L, up U, and upper left UL, decoding uses:

```text
sample = residual + pred(filter, L, U, UL)
```

The filters are:

```text
0  zero
1  left
2  up
3  average: (left + up) / 2
4  Paeth(left, up, upper left)
5  clamped gradient: clamp(left + up - upper left, 0, 255)
```

All additions wrap to one byte. The encoder chooses the filter with the lowest estimated residual cost.

## Planar median-edge residual

The outer blue-delta planar transform uses the causal median-edge predictor on
each byte plane. With left `L`, up `U`, and upper-left `UL`:

```text
low  = min(L, U)
high = max(L, U)
pred = clamp(L + U - UL, low, high)
sample = residual + pred  (mod 256)
```

Missing neighbors on the top and left edges are zero. Therefore the first row
is a byte prefix sum and the first column predicts directly from above. The
decoder specializes those edges and fuses reconstruction of the B, R-B, G-B,
and optional alpha planes with final RGBA emission, leaving no boundary branch
in the interior hot loop.

## Color transforms

Every color transform is reversible.

```text
identity:       R, G, B
green delta:    G, R - G, B - G
red delta:      R, G - R, B - R
blue delta:     B, R - B, G - B
R G B chain:    R, G - R, B - G
B G R chain:    B, G - B, R - G
```

RGBA stores alpha separately. Decoding adds each delta back to its base channel. Native transforms store chroma delta planes in a 9 bit range.

Native transforms 11 through 28 store:

```text
G
R - G + 256
B - floor((wr*R + (d-wr)*G) / d) + 256
```

The d and wr pairs are:

```text
11 through 17  d=8   wr=1,2,3,5,6,7,8
18 through 20  d=16  wr=3,5,7
21 through 23  d=32  wr=9,11,13
24 through 28  d=64  wr=19,21,23,25,27
```

Native transforms 29 through 34 define u = R - G and v = B - G. They store:

```text
Y = G + floor((u + v) / 4)
U = u + 256
V = B - blend(R, G) + 256
```

The red weights in blend are 0, 1/4, 5/16, 11/32, 3/8, and 1/2. Decoding computes v = V - 256 + blend(u, 0), then G = Y - floor((u + v) / 4), R = G + u, and B = G + v.

Native transform 35 stores:

```text
R
G - R + 256
B - floor((R + G) / 2) + 256
```

Native transforms 36 and 37 fill the two measured blue-anchor gaps with a
fixed 40/24 blend:

```text
36: B, R - B + 256, G - floor((40*R + 24*B) / 64) + 256
37: B, G - B + 256, R - floor((40*G + 24*B) / 64) + 256
```

The encoder selects either transform only when the existing sampled residual
score beats the best transform through 35 by at least 0.7%. No additional
full encode trial is added.

Native transform 38 models the quadratic blue relation in tangent-space normal
maps. Define:

```text
x = R - 128
y = G - 128
q = max(128, 255 - floor((x*x + y*y + 127) / 256))
```

The stored planes are:

```text
R
G + 128
B - q + 256
```

They occupy exact ranges 0 through 255, 128 through 383, and 1 through 383.
Decoding restores `G` by subtracting 128, recomputes `q` from `R` and `G`, and
adds it back to the third plane. The inverse costs two bounded integer
multiplications per pixel. The encoder samples this candidate in the existing
transform pass, requires at least a 1.0% residual-score lead over the ordinary
winner, and still accepts it only after a completed mode-52 stream is strictly
smaller.

Native transform 39 keeps the same plane layout and replaces `q` with a
closer integer approximation of the spherical tangent-space relation:

```text
r2 = x*x + y*y
q = 128                                      when r2 >= 127*127
q = 255 - floor((r2 + 127) / 256) - C[r2/256] otherwise
```

`C` is a fixed 64-byte correction table. The radius check bounds its index,
and decoding remains integer-only with the same two multiplications per pixel.
The encoder samples transform 39 beside the existing transform scores, requires
at least a 0.75% lead over the best current score including transform 38, and
then accepts it only when the complete mode-52 stream is smaller. This gate was
validated independently on Khronos glTF normal textures; it does not inspect
paths, names, or asset categories.

Native transform 40 uses the same spherical `q` but stores `G` directly:

```text
R
G
B - q + 256
```

The second plane is therefore eight bits instead of transform 39's centered
nine-bit plane. The decoder does less work and the format adds no predictor,
table, allocation, or model. This fallback is tried only when transform 39
misses its stronger gate and its sampled score beats the best transform
through 38; the completed stream must still be strictly smaller.

Every signed division here uses mathematical floor division, including negative inputs.

## Native planes

A native stream stores integer planes with adaptive range coding. A transform can convert RGB into luma and chroma before coding. For each plane, the decoder predicts a sample from causal neighbors and adds the mapped residual.

The base predictor family uses:

```text
W   left
N   up
NW  upper left
NE  upper right
WW  two left
NN  two up
```

The predictor set includes median edge, left, up, average, clamped gradient, upper right, second order horizontal and vertical extrapolation, and weighted blends. A tiled predictor map lets different parts of the image choose different predictors.

Mode 41 predicts each tile map entry from the causal left and up entries before coding a full predictor ID. Residual prediction and reconstruction match mode 37.

Modes 43 through 53 preserve those predictors and residual values. Their added contexts only change probability estimates. Cross channel state records whether preceding transformed planes had zero, small, or large positive or negative residuals. Local state records causal residual magnitude and sign. Modes 49 through 53 mix fine contexts with coarser activity and root contexts. Modes 51 through 53 also use the selected predictor for sign estimation. Mode 53 splits sign adaptation into magnitude groups 1, 2 through 3, and 4 or more.

Residual mapping is reversible. For an unsigned plane, each signed error maps to a nonnegative symbol and maps back during decoding. Positional coding represents the negative half range edge with the maximum symbol.

For large natural images, QLIC can split the native predictor into independent 128 row bands for each plane. The predictor math stays the same inside each band. Splitting only changes scheduling and resets prediction at the start of a band.

## Tile models

A tile chunk chooses one reversible model for each tile.

```text
raw       native stream of original tile bytes
filtered  row filtered bytes, then native stream
x delta   causal left prediction
y delta   causal up prediction
gradient  clamped gradient prediction
h2        horizontal second order prediction
v2        vertical second order prediction
planar    integer plane from corner samples, plus residual
```

Every model except raw stores residual bytes. Decoding reconstructs each byte as:

```text
pixel = residual + predictor
```

The planar model stores three bytes for each channel.

```text
base    top left
right   top right
bottom  bottom left
```

For coordinate (x, y):

```text
px = (right - base)  * x / (width  - 1)
py = (bottom - base) * y / (height - 1)
predictor = base + px + py
```

The result is reduced to one byte before adding the residual.

## Coordinate fields

A coordinate field divides the image into 64 by 64 regions. Each region stores five 4 bit coordinates. The decoder derives its basis values from decoded top and left borders.

```text
flat      average border value
gradient  bilinear surface from border corners
left      pixel from the region on the left
up        pixel from the region above
causal    Paeth(left, up, upper left)
linear    clamp(left + up - upper left)
```

The coordinates blend these bases.

```text
axis  = mix(up, left, q1)
edge  = mix(causal, linear, q4)
near  = mix(axis, edge, q3)
plane = mix(flat, gradient, q2)
pred  = mix(near, plane, q0)
pixel = pred + residual
```

mix(a, b, q) is integer interpolation on a 0 through 15 scale. The residual is exact, so the coordinates only affect file size.

## Gray model

The gray model is only used for opaque grayscale images. It represents repeated textures and screen patterns as:

```text
value = a*x + b*y + phase[x mod p, y mod p] + block_base + residual
```

p is a small period. block_base is optional and gives the model a local starting point. Every term is byte reversible, so the residual restores the exact gray value.

## Integrity

A predictor can only change the file size. QLIC checks the container checksum, native stream checksum, native pixel checksum, dimensions, palette bounds, tile tables, and animation tables before returning pixels.
