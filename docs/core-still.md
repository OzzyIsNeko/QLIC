# Core Still 1 wire format

This is the normative outer-wire description for QLIC 1.0 Core Still.
The key words **must**, **must not**, **required**, **should**, and **may** have
their usual standards meaning.

All multi-byte integers are unsigned little-endian unless a field explicitly
says otherwise. All sizes are byte counts. Arithmetic that would overflow the
implementation's checked integer type is invalid input; it never wraps for
allocation, offsets, dimensions, or bounds checks.

## CRC-32

Every CRC in Core Still 1 uses CRC-32/ISO-HDLC:

```text
width       32
polynomial  0x04c11db7
reflected   yes (table polynomial 0xedb88320)
initial     0xffffffff
final xor   0xffffffff
check       CRC32("123456789") = 0xcbf43926
```

No padding bytes are implied in a checksum range.

## Outer file

Every file is:

```text
28 bytes        header
0 or more       external palette bytes
0 or more       stored or LZMS-compressed payload bytes
4 bytes         CRC-32 of every preceding file byte
```

The footer is mandatory. A decoder must verify it before decompressing the
payload or returning metadata derived from an untrusted body.

### Header

```text
offset  size  field
0       4     ASCII "QLIC"
4       4     width, nonzero
8       4     height, nonzero
12      1     outer mode
13      1     outer transform
14      1     index bits, channel count, precision, or mode parameter
15      1     bit 7 CRC-present; bits 0..1 codec; bits 2..6 zero
16      4     palette count, band height, channels, or frame count
20      8     exact byte count after outer decompression
```

Bit 7 of byte 15 must be one. Codec 0 is Store and codec 3 is LZMS. Values 1
and 2 are reserved historical encoder backends and are not Core Still 1.
Store requires compressed byte count to equal the declared payload count.
LZMS must produce exactly the declared payload count and consume a valid LZMS
wrapper. Short, long, or malformed output is corrupt.

The external palette is present only in outer mode 5. It contains
`palette_count` consecutive RGBA entries. Its byte count is therefore
`4 * palette_count`. The compressed byte count is determined from the file:

```text
file_size - 28 - external_palette_size - 4
```

The product `width * height`, every derived row size, palette size, output
size, and offset sum must be checked before use.

### Core mode table

```text
mode  name                 required transforms
1     gray                 0, 2, 4
2     gray-alpha           0, 2, 4
3     RGB                  0, 1, 2, 3, 4, 5, 8, 9, 11
4     RGBA                 0, 1, 2, 3, 4, 5, 8, 9, 11
5     external palette     0, 2, 4, 6
7     separable            0, 7
9     native QST1          0 only
13    compressed palette   2, 6, 10, 12, 13
14    QST1 horizontal bands 0 only
```

Mode 9 requires byte 14 and bytes 16..19 to be zero. Its outer payload is one
QST1 stream. Mode 14 uses byte 14 as channel count 1, 3, or 4 and bytes 16..19
as a nonzero band height. Palette forms and the row representations are
defined in [format.md](format.md). Predictor arithmetic is in
[predictor-math.md](predictor-math.md).

An encoder should emit only combinations in this table. A decoder must reject
a combination whose individual field values are known but illegal together.

## Unsigned base-128 integer

The first byte contains bits 0..6 of the value and bit 7 is the continuation
flag. Each following byte advances by seven value bits. Encoders must use the
shortest representation. Decoders must reject overflow and truncated input.
Core Still 1 decoders should reject a non-shortest representation; canonical
vectors never contain one.

## QST1

QST1 is the native byte-plane stream. Its fixed header is:

```text
offset  size  field
0       4     ASCII "QST1"
4       4     width
8       4     height
12      1     channels: 1, 3, or 4
13      1     flags
14      1     native mode
15      1     native reversible transform
16      1     predictor tile log2
17      1     control
18      4     CRC-32 of decoded interleaved channel bytes
22      4     exact QST1 payload byte count
26      4     QST1 container CRC-32
```

The QST1 container CRC is computed over the complete QST1 byte sequence with
bytes 26..29 treated as four zero bytes. The decoded-pixel CRC is computed in
raster order over exactly `channels` bytes per pixel before the outer decoder
adds an opaque alpha channel to RGB or grayscale output.

Flags are:

```text
bit 0      one encoded gray plane expanded to three equal RGB channels
bit 1      constant alpha; byte 17 is the alpha value
bits 2..4  exact sample-grid depth, zero or 1..7
bits 5..7  zero
```

Gray requires three declared channels and transform 0. Constant alpha requires
four channels. With constant alpha clear, control 0 or 5 selects adaptation 5,
control 4 selects adaptation 4, and control 6 selects adaptation 6. Other
control values are invalid. With constant alpha set, the adaptation is 5.

The payload byte count includes any mode-1 palette prefix. It must end exactly
at the end of the QST1 stream. Mode 1 starts its payload with a little-endian
16-bit palette count from 1 through 256 followed by `count * channels` palette
bytes. The range-coded bytes follow that prefix.

Core Still 1 native modes are 0, 1, 37, 39, 40, 45, 52, 53, and 54. Native
transforms are 0 through 40. The exact reversible transforms are normative in
[predictor-math.md](predictor-math.md). A decoder must reject a mode,
transform, flag, channel, or tile-log combination it does not implement; it
must never substitute a nearby mode.

## QST1 range coder

The coder is integer-only. Every probability is an unsigned 12-bit-scale value
stored in a 16-bit cell.

```text
probability scale     4096
initial probability  2048
initial range         0xffffffff
normalization limit   0x01000000
decoder code          first five entropy bytes, shifted in big-endian order
```

For a probability `p`, current `range`, and adaptation `a`:

```text
bound = (range >> 12) * p
bit   = code >= bound

if bit == 0:
    range = bound
    p = p + ((4096 - p) >> a)
else:
    code = code - bound
    range = range - bound
    p = p - (p >> a)
```

While `range < 0x01000000`, shift `range` left by eight and shift the next
entropy byte into `code`. The current format guarantees at most two shifts for
one coded decision; implementations may express this as a loop. Reading past
the payload is corrupt even if zero fill would produce the expected pixel CRC.

Mixed decisions form a temporary probability and update both source cells:

```text
mixed = (5 * child + 3 * parent + 4) >> 3
```

The bit is coded with `mixed`; `child` and `parent` are then independently
updated by the same bit and adaptation. Weighted mode-53 decisions and all
model indexes are defined by the QST1 model annex and locked by the positive
and adversarial conformance vectors.

The complete probability-array layout, context formulas, required native-mode
dispatch, hierarchical mixing, cross-channel state, and weighted predictor are
normative in [qst1-model-annex.md](qst1-model-annex.md).

## Horizontal QST1 bands

Mode 14's stored payload is:

```text
4 bytes            band count
4 * count bytes    QST1 byte count for each band
remaining bytes    consecutive QST1 streams
```

`count` must equal `1 + (height - 1) / band_height`. Each nested QST1 width and
channel count must match the outer header. Its height must be `band_height`
except for the clipped last band. Nested outer QLIC files and recursive mode
14 payloads are invalid. No bytes may trail the last stream.

## Validation order

A conforming decoder should use this failure order so hostile files do not
turn metadata into allocations:

1. Check caller limits and the minimum outer size.
2. Parse fixed fields with checked arithmetic.
3. Reject invalid field combinations.
4. Verify the outer CRC.
5. Check declared decoded sizes and chunk counts against caller limits.
6. Decompress exactly the declared payload size.
7. Validate nested headers and lengths before large model allocations.
8. Decode pixels, verify every inner checksum, then return output.

The configured resource policy may reject an otherwise valid file. That is a
limit error, not corrupt data.

## Conformance boundary

The outer container, CRCs, QST1 header, transforms, range update, entropy-model
annex, profile surface, and frozen vectors are fixed for QLIC 1.0. The C and
independently written safe-Rust decoders are hash-locked executable models.
A conforming implementation must produce the retained decoded checksums and
must reject the frozen negative recipes. The lock is a release gate, not a
standards-body certification claim.
