# QLIC command line

Keep `qlic.exe` beside `image-codecs`.

```text
qlic pack input.png output.qlic
qlic unpack input.qlic output.png
qlic info input.qlic --json
qlic verify input.qlic
```

Use `--threads N` or `--threads all` to set CPU use. There is no effort or
quality setting.

`--color-profile NAME`, `--icc FILE`, and `--alpha straight|premultiplied`
store color and alpha meaning without tone mapping. `--exif`, `--xmp`,
`--iptc`, and `--jumbf` attach exact records. Source adapters import supported
embedded records, and QLIC discovers a neighboring `.xmp` sidecar.

For HDR RGB input above 8 bits, use `rec2100-pq` or `rec2100-hlg`. They record
BT.2020 with CICP transfer 16 (PQ) or 18 (HLG) and preserve the integer samples
exactly.

`unpack` writes exact 8- or 16-bit pixels. PNG embeds valid EXIF, XMP, JUMBF,
and physical resolution. TIFF embeds ICC, XMP, IPTC, JUMBF, resolution,
orientation, and alpha association. Premultiplied samples require TIFF; PNG
export fails instead of changing the samples.

Exact `.icc`, `.exif`, `.xmp`, `.iptc`, `.jumbf`, and `.qlic-hdr.txt`
sidecars preserve duplicate, opaque, or destination-incompatible records. Use
the C, Rust, or Web API for 9--15- or 17--24-bit pixels.

QLIC does not support float/half HDR, RAW development, or gain-map
interpretation. TIFF import does not reconstruct a complete nested EXIF IFD
graph; use `--exif` when the exact block is required.

Run `qlic --help` for all commands. The program contacts no service.
