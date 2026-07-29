# Reproduce the benchmark

This compares QLIC 0.5.0, WebP lossless preset 6, and JPEG XL lossless effort 9 on one logical processor.

[Completed results](RESULTS.md)

## Corpus

The corpus uses DIV2K validation, CLIC 2022 validation, the QOI benchmark suite, and a deterministic selection from Enrico. It includes natural photos, game and web screenshots, mobile interfaces, icons, isolated PNG objects, and textures.

The original sets are described by [DIV2K](https://data.vision.ee.ethz.ch/cvl/DIV2K/), [CLIC](https://archive.compression.cc/2022/tasks/index.html), the [QOI benchmark suite](https://qoiformat.org/benchmark/), and [Enrico](https://userinterfaces.aalto.fi/enrico/).

Enrico contributes the first 10 numeric screen IDs from each design topic. This keeps the selection deterministic and stops one source from dominating the benchmark.

Preparation takes the first frame, applies stored orientation, removes metadata, and writes an 8 bit RGB or RGBA PNG. Images are not resized or cropped. Duplicate normalized pixels are removed.

Files above the shared WebP dimension limit or the 16 million pixel corpus limit are written to corpus-excluded.csv.

## Requirements

PowerShell 7, curl, tar, and ImageMagick 7 are required.

The scripts use the binaries in this repository by default. Other binaries can be passed to the runner.

## Run

Run this from the benchmark directory.

```powershell
pwsh ./download-corpus.ps1
pwsh ./prepare-corpus.ps1
pwsh ./run-large-matrix.ps1
pwsh ./summarize-results.ps1
```

If ImageMagick is not on PATH, pass its full path with the Magick parameter to the preparation and run scripts.

Downloads are pinned by URL, byte length, and SHA256 in download-corpus.ps1. Archive paths are checked before extraction. downloads.json records what was downloaded.

## Codec settings

QLIC uses pack with one thread.

WebP uses cwebp with lossless, exact, and z 6.

JPEG XL uses cjxl with distance 0, effort 9, and zero worker threads.

Every encoder is pinned to the same logical processor. Their order rotates for every image. Wall time includes startup and file IO. Peak process memory is sampled every 5 milliseconds.

Every result is decoded and compared against the normalized source. The run stops if one pixel is different.

## Output

run/results.csv stores every image measurement and is updated after each verified image.

run/summary.json and run/categories.csv are created after the full run succeeds.

summarize-results.ps1 creates run/analysis-summary.json, run/analysis-categories.csv, and run/REPORT.md. It can also summarize a partial run.
