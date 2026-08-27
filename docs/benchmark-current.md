# QLIC 1.0 benchmark checkpoint

This is the sanitized, human-readable record of the current 3,167-image gate.
The full development workspace retains per-file rows, executable and library
hashes, source manifests, PGO input, timing rotations, and rejected
experiments. Personal filesystem paths are intentionally omitted here.

## Corpus and method

The corpus contains 1,964,362,720 pixels. It combines DIV2K validation, CLIC
2022 validation, QOI benchmark categories, and a deterministic Enrico UI
selection. After exact normalization and duplicate removal it contains natural
photos, photographic and synthetic textures, game and web screenshots, mobile
interfaces, icons, transparent isolated objects, RGB, and RGBA. No production
route reads a filename, directory, dataset label, or category.

Every encoded result is decoded and compared byte-for-byte with the normalized
source. Encoders run on one pinned logical processor with rotated order. Encode
wall time includes startup and file I/O. Decode timing uses the exact retained
files, preloads encoded bytes, performs a warm-up that verifies RGBA output,
then averages three pinned in-memory runs per file. The machine was an AMD
Ryzen 9 9950X3D running Windows 11. The final QLIC decoder row uses the portable
x86-64 Clang 22.1.4 Release build that is shipped here: no `-march=native` and
no PGO. The comparison rows come from the retained same-machine, same-harness
effort sweep.

This is a development and release gate, not a universal codec ranking. Changes
must pass per-file exactness, worst-regression, content-class, decode-time, and
memory checks. New routing rules are developed on discovery data and frozen
before an untouched holdout; a same-sample win is not enough. Completed bytes,
not proxy scores, decide every candidate accepted by the encoder.

## Encode

| Codec setting | Bytes | Seconds |
| --- | ---: | ---: |
| JPEG XL lossless effort 1 | 1,546,891,144 | 111.053 |
| JPEG XL lossless effort 2 | 1,411,868,992 | 195.489 |
| JPEG XL lossless effort 3 | 1,311,022,795 | 274.373 |
| JPEG XL lossless effort 4 | 1,278,581,912 | 497.141 |
| JPEG XL lossless effort 5 | 1,230,084,951 | 629.126 |
| JPEG XL lossless effort 6 | 1,214,206,451 | 754.620 |
| JPEG XL lossless effort 7 | 1,187,851,480 | 1,087.755 |
| JPEG XL lossless effort 8 | 1,171,971,513 | 2,977.934 |
| JPEG XL lossless effort 9 | 1,164,424,898 | 5,286.575 |
| QLIC automatic policy | 1,163,128,771 | 514.989* |
| WebP lossless preset 1 | 1,339,854,110 | 296.869 |
| WebP lossless preset 2 | 1,310,683,488 | 378.188 |
| WebP lossless preset 3 | 1,297,243,720 | 442.578 |
| WebP lossless preset 4 | 1,296,153,364 | 455.294 |
| WebP lossless preset 5 | 1,292,381,866 | 472.913 |
| WebP lossless preset 6 | 1,289,518,274 | 544.911 |
| WebP lossless preset 7 | 1,286,907,370 | 737.723 |
| WebP lossless preset 8 | 1,272,997,810 | 1,211.999 |
| WebP lossless preset 9 | 1,256,259,092 | 9,032.945 |
| OxiPNG level 6 | 1,618,637,169 | 9,105.634 |

QLIC is 1,296,127 bytes, or 0.111310%, smaller than JPEG XL effort 9. It is
126,389,503 bytes, or 9.801%, smaller than WebP preset 6. QLIC is smaller than
JPEG XL effort 9 on 1,838 files, larger on 1,327, and tied on two.

The asterisk matters: 514.989 seconds is the last complete pinned encode timing
from the immediately preceding accepted checkpoint, where QLIC was 303,000
bytes larger. The final release was then re-encoded across all 3,167 files: 15
became smaller, 3,152 were byte-identical, none grew, and all decoded exactly.
The table retains the prior timing for scale; it does not pretend the final
15-route delta was retimed in that same sequential campaign.

## Decode

| Codec setting | Seconds | MP/s | Time relative to QLIC |
| --- | ---: | ---: | ---: |
| QLIC automatic policy | 133.949 | 14.665 | baseline |
| JPEG XL effort 1 files | 27.355 | 71.81 | 79.58% faster |
| JPEG XL effort 2 files | 49.175 | 39.95 | 63.29% faster |
| JPEG XL effort 3 files | 127.451 | 15.41 | 4.85% faster |
| JPEG XL effort 4 files | 156.735 | 12.53 | 17.01% slower |
| JPEG XL effort 5 files | 152.908 | 12.85 | 14.15% slower |
| JPEG XL effort 6 files | 167.404 | 11.73 | 24.98% slower |
| JPEG XL effort 7 files | 180.167 | 10.90 | 34.50% slower |
| JPEG XL effort 8 files | 186.240 | 10.55 | 39.04% slower |
| JPEG XL effort 9 files | 192.478 | 10.21 | 43.69% slower |
| WebP preset 1 files | 20.369 | 96.44 | 84.79% faster |
| WebP preset 6 files | 19.847 | 98.97 | 85.18% faster |
| WebP preset 9 files | 18.689 | 105.11 | 86.05% faster |
| OxiPNG level 6 files | 11.429 | 171.87 | 91.47% faster |

QLIC is not the universal decode-speed winner. WebP and PNG-family output are
far quicker to decode, and low-effort JPEG XL deliberately trades substantial
file size for speed. On this corpus, QLIC uses 30.41% less decode time at a
slightly smaller total than JPEG XL effort 9. It is 4.85% slower than JPEG XL
effort 3 while producing 11.28% fewer bytes.

## Corpus variation

QLIC is often smaller on interfaces, compact textures, structured graphics,
repetition, and mixed screenshot assets. JPEG XL remains smaller on a subset of
photographic, screenshot, and alpha-heavy files. WebP can remain preferable
when decode latency dominates storage. The encoder therefore uses low-cost
content signals only to decide which bounded candidate to try, then
requires an exact completed-file win. It never forces a specialist from the
content label, and rejected trials remain part of the benchmark ledger so a
future optimization cannot reintroduce them unnoticed.
