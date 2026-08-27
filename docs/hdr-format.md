# Self-describing integer color and HDR (`QSW2`)

QLIC mode 19 (`QSW1`) is an exact unsigned-integer sample carrier. It does not
identify a transfer function, color primaries, signal range, or alpha
association, so mode 19 alone is not a self-describing HDR format.

Mode 20 is the additive self-describing wide-image format. Existing mode-19
encoders and decoders remain unchanged. Older decoders reject mode 20 at the
outer-mode check instead of misinterpreting or reducing it.

This first `QSW2` version preserves unsigned 8--24-bit samples and their color
or HDR metadata exactly. Eight-bit QSW2 is the metadata-bearing counterpart to
Core Still 1; 9--24-bit QSW2 covers high-precision and HDR code values. It
performs no color conversion, range expansion, alpha conversion, or tone
mapping. Half/float samples and banded streaming are reserved for later
additive sample methods.

## Outer header

The ordinary 28-byte QLIC header is used with these requirements:

| Field | Value |
|---|---|
| mode | 20 |
| transform | identity (0) |
| byte 14 | bits per sample, 8--24 |
| codec | store (0) plus the mandatory CRC flag |
| bytes 16--19 | channels: 1, 3, or 4 |
| decoded payload size | exact `QSW2` payload size |

The outer CRC covers the header and complete `QSW2` payload. `QSW2` is native
QLIC data; it does not contain a PNG, TIFF, JPEG XL, AVIF, or EXR file.

## Fixed header

All multi-byte integers are little-endian.

```text
0..3    "QSW2"
4       version: 1
5       sample type: 1 = unsigned integer
6       bits per sample
7       channels
8       alpha association: 0 none, 1 straight, 2 premultiplied
9       color authority:
          0 unspecified
          1 ICC only
          2 CICP only
          3 ICC preferred, CICP also retained
          4 CICP preferred, ICC also retained
10..11  zero
12..15  chunk count
16..23  sum of non-PIXL chunk payload lengths
24..31  PIXL chunk payload length
```

Channel meanings in version 1 are Gray, RGB, and RGBA for channel counts 1,
3, and 4. Four channels require straight or premultiplied alpha; one and three
channels require alpha association `none`.

The fixed header is followed by exactly `chunk count` records with no padding:

```text
0..3    ASCII tag
4..7    flags: bit 0 critical; every other bit is zero
8..15   payload length
16..    payload bytes
```

The default decoder limits are at most 256 chunks and 16 MiB of non-pixel
metadata. Limits are checked before allocating or copying metadata. Exactly one
critical `PIXL` chunk is required. Duplicate known chunks are invalid. Unknown
critical chunks are rejected. Unknown ancillary chunks are retained in their
original order and returned byte-for-byte by current decoders; older version-1
decoders may skip them safely.

## Version-1 chunks

### `PIXL`

The payload is one complete `QSW1` byte-slice stream whose precision, channel
count, width, and height match the outer header and `QSW2` descriptor. Its
existing full-sample checksum remains mandatory. Reusing `QSW1` keeps the
well-tested CPU-native byte-slice decoder and exactness guarantees across the
complete 8--24-bit QSW2 range.

### `ICCP`

The payload is the original ICC profile byte-for-byte. QLIC does not apply the
profile while encoding or decoding.

### `CICP`

Exactly eight bytes:

```text
0..1  colour_primaries
2..3  transfer_characteristics
4..5  matrix_coefficients
6     video_full_range_flag: 0 or 1
7     zero
```

The numeric identifiers follow ISO/IEC 23091-2. They are descriptive only;
samples are never transformed implicitly.

The named Rec. 2100 profiles use these exact CICP values:

| Profile | Primaries | Transfer | Matrix | Range |
|---|---:|---:|---:|---:|
| BT.2020/PQ RGB | 9 | 16 | 0 | full |
| BT.2020/HLG RGB | 9 | 18 | 0 | full |

PQ is SMPTE ST 2084 and HLG is ARIB STD-B67. These values describe the stored
RGB code values; they do not request tone mapping or synthesize missing HDR
metadata.

### `MDCV`

Exactly 24 bytes using the mastering-display representation common to HDR
containers: three display-primary x/y pairs and one white-point x/y pair as
unsigned 16-bit values in units of 0.00002, followed by maximum and minimum
display luminance as unsigned 32-bit values in units of 0.0001 cd/m2.

### `CLLI`

Exactly four bytes: unsigned 16-bit MaxCLL followed by unsigned 16-bit MaxFALL,
both in cd/m2.

### Photographic metadata blocks

Any non-critical tag other than the five core tags above is an ordered opaque
metadata block. Current APIs preserve its four-byte tag and payload exactly.
The conventional tags written by QLIC tools are `EXIF`, `XMP_`, `IPTC`, and
`JUMB`; other ancillary tags are allowed so provenance or application records
do not need a new QSW2 version. `PIXL`, `ICCP`, `CICP`, `MDCV`, and `CLLI`
cannot be supplied through the opaque-block API because their syntax and
uniqueness are validated separately.

This is preservation, not interpretation. QLIC does not rewrite orientation,
merge XMP packets, validate camera maker notes, or claim authenticity for a
provenance record.

## Color-authority rules

- `unspecified` requires neither ICC nor CICP.
- `ICC only` requires ICC and forbids CICP.
- `CICP only` requires CICP and forbids ICC.
- `ICC preferred` and `CICP preferred` require both and retain both verbatim.
- `MDCV` and `CLLI` may accompany any color authority.

When ICC and CICP disagree, the preferred field determines interpretation. The
codec never attempts to reconcile them by modifying samples.

## Compatibility and future extension

Mode 19 and the Core Still 1 modes keep their current syntax. QSW2 adds an
explicit metadata-bearing choice for 8-bit images without changing the
automatic Core Still encoder. Version 1 uses a single whole-image `PIXL`
payload. A future sample method may replace it with bounded
row-band chunks for streaming, or add raw IEEE half/float bit preservation;
those changes require a new method or version and cannot silently change
version-1 decoding.
