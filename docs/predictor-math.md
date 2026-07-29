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
