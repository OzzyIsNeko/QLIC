# QST1 entropy-model annex

This annex is normative for QLIC 1.0 Core Still native streams. It closes the
model-definition boundary left by the outer-wire document. The normative set
is this document, [core-still.md](core-still.md),
[predictor-math.md](predictor-math.md), `profiles.json`, the frozen positive
and negative manifests, and the safe-Rust executable model named in
`conformance.lock.json`.

The Rust model is incorporated as an executable part of this annex. It is an
independently written, bounds-checked decoder rather than a binding to the C
implementation. A release is invalid if either decoder's SHA-256 differs from
the lock file or if the two decoders disagree on a retained vector.

## Integer constants and initialization

```text
PROB_BITS             12
PROB_ONE              4096
PROB_INIT             2048
RANGE_TOP             1 << 24
ADAPT_DEFAULT         5
ADAPT_FAST            4
ADAPT_SLOW            6
ACTIVITY_CONTEXTS     12
ERROR_CONTEXTS        6
BASE_CONTEXTS         72
PREDICTOR_CONTEXTS    288
MAX_MAGNITUDE_BITS    10
RUN_BITS              24
PREDICTORS_BASE       8
PREDICTORS_WIDE       32
```

All probability cells start at `PROB_INIT`, except lazily populated
cross-channel cells in modes 52 through 54. A zero cross-channel cell is
initialized from its corresponding fine cell immediately before its first
decision. Calculations use unsigned integer arithmetic of at least 32 bits;
the weighted predictor's sums use signed or unsigned 64-bit arithmetic as
shown in the executable model. Division is truncation toward zero unless a
floor operation is explicitly named.

After coding bit `b` with adaptation `a`, update a probability `p` as follows:

```text
b == 0: p += (4096 - p) >> a
b == 1: p -= p >> a
```

The range-coder state and normalization are defined in `core-still.md`. Five
initial bytes are mandatory. Every normalization byte must exist; no implicit
zero padding is allowed.

## Context indexes

The activity class `A(v)` is:

```text
v < 0       -> 0
v <= 2      -> v
v > 1024    -> 11
otherwise   -> 1 + bit_length(v - 1)
```

For unsigned residual contexting, error bucket `E0(k)` is 0 for `k=0`, 1 for
`k=1..2`, and 2 otherwise. The context is `A * 3 + E0`.

For signed contexting, `E(k,s)` is 0 for zero; 1/2 for positive/negative with
`k=1..2`; 3/4 for positive/negative with `k=3..4`; and 5 otherwise. The base
context is `A * 6 + E`.

The predictor-group offset is:

```text
predictor 0        0
predictors 1..4    72
predictors 5..15   144
predictors 16..31  216
```

Modes using the alternate grouping place predictors 5..10 at 144 and 11..31
at 216. A full context is `base + predictor-group offset` and is therefore in
`0..287`.

The causal activity input for context modes is the sum of absolute local
gradients:

```text
abs(W-NW) + abs(NW-N) + abs(N-NE)
+ (abs(W-WW) + abs(N-NN)) / 2
+ min(3, previous-row magnitude-bit count)
```

Missing neighbors use the edge substitutions in the executable model. The
larger of the left and upper residual bit counts, with its corresponding sign,
selects the signed error bucket.

## Model arrays

The base/event model arrays are row-major with these logical dimensions:

```text
unary              [context][0..10]
mantissa           [context][length 0..10][bit 0..9]
nonzero sign       [context]
hinted sign        [context][hint is positive, hint is negative]
run unary          [context][0..24]                   mode 39 only
run mantissa       [context][length 0..24][bit 0..23] mode 39 only
predictor tree     [8] or [32]
```

Mode-37-family mantissa storage only uses lengths 2 through 9 and bits below
the length. Its compact index is `(context * 8 + length - 2) * 8 + bit`.
Unary index is `context * 11 + bit`.

Every predictor tree starts at node 1. A 3-bit tree performs three decisions
and returns `node & 7`; a 5-bit tree performs five and returns `node & 31`.
Tiles are raster ordered. Predictor-reuse modes first test the left reference,
then a distinct upper reference, with independent `[direction][group]`
probabilities; decision zero means reuse. If neither is reused, decode the
5-bit predictor tree.

## Residual representation

For a prediction not using the positive-edge representation:

```text
e >= 0 -> u = 2*e
e < 0  -> u = -2*e - 1
```

For the positive-edge representation, `-half` maps to `maximum`; positive
errors map to `2*e-1`; all other errors map to `-2*e`. The inverse mapping is
exactly the reverse of those cases.

Magnitude is represented as a zero/nonzero decision, unary bit length, then
mantissa bits from most significant to least significant. A nonzero value of
length `k` starts with `1 << (k-1)`. Each following mantissa decision sets the
corresponding descending bit. A sign decision follows the magnitude where the
selected plane method uses a signed residual. Values outside the plane's
declared depth are corrupt.

## Required native-mode dispatch

```text
mode 0   base plane; fixed or 3-bit per-tile predictor
mode 1   palette prefix, then mode-0 index plane
mode 37  context plane without context-map reuse
mode 39  event/run plane; tile_log must be zero
mode 40  pattern plane; tile_log must be one
mode 45  context plane with map-free predictor signaling
mode 52  local-root context plane and cross-channel state
mode 53  mode 52 with refined sign mixing
mode 54  mode 53 with predictor 31 selecting weighted prediction
```

Planes are decoded in transform order, not display-channel order. Modes 52,
53, and 54 carry state from the first plane to the second and from the paired
first/second state to the last plane. One-channel, gray, and constant-alpha
forms follow the independent/first/middle/last flow selected by the executable
model. State bytes use:

```text
residual class = 0 zero, 1/2 positive short/long, 3/4 negative short/long
state          = (activity > 0 ? 5 : 0) + residual class
zero LUT       = [0,1,1,2,2,3,4,4,5,5]
magnitude LUT  = [0,1,2,1,2,0,1,2,1,2]
```

For a paired state byte with high and low nibbles:

```text
zero state      = zero(high) * 6 + zero(low)
magnitude state = magnitude(high) * 3 + magnitude(low)
```

## Hierarchical probability mixing

Let `fine`, `coarse`, and `root` be the selected cells:

```text
coarse_mix = (coarse + root + 1) >> 1
parent_mix = (fine + coarse_mix + 1) >> 1
```

A root decision codes with `(fine + coarse_mix + 1) >> 1`, then updates
`fine` at adaptation `a` and `coarse` and `root` at `a + slow`.

A child/root decision codes with:

```text
(weight*child + (8-weight)*parent_mix + 4) >> 3
```

It updates `child` at `a - child_rate`, `fine` at `a`, and the two parent
levels at `a + slow`. A negative encoded rate therefore slows adaptation.

Modes 53 and 54 use these fixed parameters:

```text
zero:       weight 5, child_rate 0
magnitude:  weight 4, child_rate -1
sign by magnitude bucket 0..2:
  weight       [4,8,12]
  child_rate   [1,2,3]
  exact_rate   [0,2,2]
root rate      [-1,0,1]
```

The cross/exact sign decision is:

```text
(weight*child + (16-weight)*exact + 8) >> 4
```

It updates child, exact, fine, coarse, and root using the rates in the
executable model. The exact-sign index dimensions are
`[predictor 0..31][base context 0..71][magnitude bucket 0..2][hint 0..1]`.

## Weighted predictor (mode 54)

Predictor 31 selects a causal blend of four predictions. Their base weights
are `[13,12,12,11]`. Each weight is multiplied by an inverse error estimate
using `floor(2^24/(index+1))` for indexes 0 through 63. Error estimates use the
north, northeast, and northwest accumulated errors, reduced by
`max(0, bit_length(error+1)-6)`. The weighted sum, clamping condition, error
propagation, and signed rounding are defined exactly by `WeightedPredictor` in
the locked Rust executable model. No floating-point arithmetic is permitted.

## Completion and rejection

After all planes are decoded, inverse transforms and sample-grid expansion are
applied exactly as specified. The decoded interleaved bytes must match the
QST1 pixel CRC. The QST1 container CRC must match with bytes 26 through 29
treated as zero, and the entropy decoder must not read beyond its declared
payload. Invalid array indexes, impossible tree values, invalid residuals,
arithmetic overflow, and any checksum mismatch are corrupt data.

The frozen manifest is the compatibility oracle for QLIC 1.0. Future encoder
changes may produce smaller streams, but a 1.x decoder must continue to decode
all retained positive vectors identically and reject all negative recipes.
