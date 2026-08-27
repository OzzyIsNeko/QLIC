# QLIC for Windows

Run `qlic-gui.exe`. Keep it beside `qlic.exe` and `image-codecs`.

Open or drop a `.qlic` file to use the built-in viewer. It decodes in the
background through the canonical codec, preserves the original filename,
handles 8--24-bit integer stills and RGBA8 animation, composites straight and
premultiplied alpha, and applies embedded ICC profiles through Windows Color
Management when the host provides a usable display profile. **Fit** and
**100%** switch the display scale. PQ/HLG input is labeled as an SDR preview;
the file itself remains exact
and unchanged.

Choose any other supported image to use the encoder. QLIC verifies the new file
before **Save QLIC** is enabled.

**Options** contains CPU use, ICC/CICP color metadata, and alpha association.
There is no effort setting. The CLI path used by the app
automatically retains supported embedded photographic metadata and a neighboring
`.xmp` sidecar. Use the CLI or SDK to select explicit EXIF/XMP/IPTC/JUMBF
blocks. Float/half HDR and RAW development are not supported.

Cancel stops the codec process. The app is local and does not upload files.
