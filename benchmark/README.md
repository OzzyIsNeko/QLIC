# Benchmark runner

This builds the 3,167-image development corpus and compares QLIC, JPEG XL, and
WebP on one pinned logical processor. The current results and exact method are
in [docs/benchmark-current.md](../docs/benchmark-current.md).

## Corpus

The scripts use DIV2K validation, CLIC 2022 validation, the QOI benchmark suite,
and a deterministic Enrico selection. Preparation applies stored orientation,
removes metadata, writes 8-bit RGB or RGBA PNG, and removes duplicate pixels.
It does not resize or crop images.

Downloads are pinned by URL, size, and SHA-256 in `download-corpus.ps1`.

## Run

PowerShell 7, curl, tar, and ImageMagick 7 are required.

```powershell
pwsh ./download-corpus.ps1
pwsh ./prepare-corpus.ps1
pwsh ./run-large-matrix.ps1
pwsh ./summarize-results.ps1
```

Every encoded file is decoded and compared with its normalized source. The run
stops on a mismatch. Per-file results go to `run/results.csv`; the summary
script writes the aggregate report and category tables.

## Mode-53 context research

The benchmark-only [gradient-topology replay](gradient-topology-replay.md)
measures whether a small fixed geometry class can expose additional conditional
sparsity. It leaves the production bitstream and decoder unchanged and requires
separate discovery and holdout results before any decoder prototype is allowed.
