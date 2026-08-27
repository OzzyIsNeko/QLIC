# Support

QLIC 1.0 starts the stable file and C API line. There is no paid support or
response-time promise. Pin the decoder you ship and test it on your own assets.

## Tested paths

| Path | Gate |
| --- | --- |
| Windows x64 C SDK and CLI | Visual Studio and LLVM Clang |
| Linux x86-64 C SDK and CLI | GCC and Clang on Ubuntu 24.04 |
| CMake and pkg-config | Installed static and shared consumers |
| Rust decoder | Rust 1.85 minimum plus current stable tests |
| WebAssembly | Freestanding `wasm32` plus Node differential tests |

Other systems may work. They are not claimed until they run the same tests.

## Before shipping

1. Keep the source revision or archive hash used to create the files.
2. Test representative assets, malformed files, decode time, and peak memory on
   the real hardware.
3. Set file, payload, pixel, decoded-byte, frame, metadata, and chunk limits for
   the product instead of relying only on broad defaults.
4. Put untrusted work behind an application time limit or process boundary.
5. Keep a common interchange fallback where QLIC is not installed.

The C decoder is canonical. The Rust decoder is pure safe Rust. The browser
decoder owns its JavaScript output. All three reject unsupported syntax instead
of reducing precision.

QLIC currently uses whole-file APIs and unsigned-integer samples. It has no
float/half HDR, Rust encoder, or general row/region decode. QSW2 preserves
ordered opaque ICC, EXIF, XMP, IPTC, and JUMBF payloads byte-for-byte; source
adapters import the records they can identify without rewriting their contents.
The Windows WIC decoder exposes exact 8- and 16-bit gray/RGB/RGBA formats,
premultiplied-alpha formats, and native packed HDR10 when QSW2 declares
full-range BT.2020/PQ 10-bit RGB. Other 9..24-bit data remains available through
the C, Rust, or WebAssembly sample APIs. This includes exact Rec. 2100 HLG data;
10-bit HLG is not exposed as WIC HDR10 because Windows provides no matching
native HLG pixel format.
