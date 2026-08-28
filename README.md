# QLIC

Quick Lossless Image Codec.

QLIC stores still images, RGBA animation, and unsigned 8--24-bit integer
samples losslessly. QSW2 carries ICC, CICP, alpha, and photographic metadata.
QLIC does not tone-map or infer HDR. The encoder has one automatic policy; it
has no effort or quality setting.

This is the only QLIC 1.0 source tree.

Try the [web demo](https://qlic.pages.dev/).
See [benchmarks](https://qlic.pages.dev/benchmarks/).

## Components

| Need | Use |
| --- | --- |
| Windows app | `qlic-gui.exe` |
| Pack, unpack, inspect, or verify | `qlic` |
| C or C++ | C SDK and [SDK guide](docs/sdk.md) |
| Safe Rust decode | [Rust decoder](rust/qlic-decoder) |
| Browser or JavaScript | [WebAssembly package](web/README.md) |
| Explorer, WIC apps, 8/16-bit, and HDR10 decode | [WIC decoder](packaging/README-wic.md) |

## Build

Windows requires CMake 3.25 or newer and Visual Studio 2022 or newer.

```powershell
.\build.ps1
.\scripts\build-rust.ps1
.\scripts\build-web.ps1
```

WebAssembly also requires LLVM and Node.js.

On Debian or Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
  libpng-dev libwebp-dev libjxl-dev libavif-dev libtiff-dev libwim-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

To build libraries and tests without importers or LZMS encoding:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DQLIC_BUILD_CLI=OFF -DQLIC_LINUX_LZMS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

## Command line

```text
qlic pack input.png output.qlic
qlic unpack input.qlic output.png
qlic info input.qlic --json
qlic verify input.qlic
```

Use `--threads N` or `--threads all` to set CPU use. `verify` fully decodes and
checks a file without writing an image.

## Color and metadata

The Windows CLI accepts:

```text
qlic pack input.png output.qlic --color-profile srgb
qlic pack input.png output.qlic --icc display.icc --alpha straight
qlic pack input.tif output.qlic --xmp edit.xmp --exif camera.exif
```

These options store integer samples without tone mapping. QLIC preserves
ordered opaque metadata blocks byte-for-byte. Source adapters import supported
ICC, EXIF, XMP, IPTC, and JUMBF records. A neighboring `input.xmp` Lightroom
sidecar is discovered automatically. Explicit metadata options handle records
the source adapter cannot expose.

`--color-profile rec2100-pq` and `rec2100-hlg` store full-range BT.2020 RGB
with the standard PQ (CICP 16) or HLG (CICP 18) transfer identifier. The
cross-platform C SDK exposes the same descriptors through `qlic_encode_hdr`.
Both paths preserve the original integer code values; display rendering belongs
to the viewer or calling application.

On Windows, PNG output embeds valid EXIF, XMP, JUMBF, and physical-resolution
chunks. TIFF output carries ICC, XMP, IPTC, JUMBF, DPI, orientation, and alpha
association. Exact sidecars are also written, including duplicate or
destination-incompatible records. Premultiplied samples export only to
associated-alpha TIFF; PNG export fails instead of changing the samples.

Windows and Linux import PNG, WebP, JPEG XL, TIFF, and BMP when their loaders
are present. Linux also imports AVIF. Windows can use installed WIC decoders,
including the system JPEG and GIF decoders. Known lossy sources are accepted
with a warning that the QLIC file will likely be larger.

## C library

```cmake
find_package(qlic 1.0 CONFIG REQUIRED)
target_link_libraries(app PRIVATE qlic::qlic_static)
```

Use `qlic::qlic` for the shared library. The public header is
`codec/include/qlic/qlic.h`. See the [SDK guide](docs/sdk.md) for ownership,
limits, and public entry points.

## Rust and WebAssembly

The Rust decoder has no dependencies or C/FFI and forbids unsafe code. The Web
package is a static ES module and Wasm binary; it needs no QLIC executable or
local service. Both reject unsupported syntax and do not reduce wide or HDR
data to RGBA8. See the [Rust](rust/qlic-decoder) and
[WebAssembly](web/README.md) guides.

## Limits

The decoder supports validated RGBA8 rows and exact rectangular output with
progress and cancellation. QST1 regions still require complete entropy
reconstruction; the region API bounds destination memory but is not random
access. QLIC 1.0 has no incremental input decoder, float/half HDR, RAW
development, gain-map interpretation, or Rust encoder. Its HDR contract is
exact unsigned 8--24-bit code values with explicit ICC or CICP meaning,
including retained PQ and HLG conformance fixtures.

Pin deployed decoders and keep a common interchange format where QLIC is not
available.

## Evidence

The retained release corpus and every comparison setting are recorded in
[benchmark-current.md](docs/benchmark-current.md). Results are corpus-specific,
and every encoded output is decoded and compared with its source. See
[architecture.md](docs/architecture.md) for design, qualification, and known
limits.

Other references: [profiles](docs/profiles.md), [file format](docs/format.md),
and [support](SUPPORT.md).

## Packages

Build the seven local packages with `.\scripts\package.ps1`. Output goes to
`dist` with checksums and SPDX SBOMs.

Stage a verified unsigned public release bundle with
`.\scripts\stage-community-release.ps1`. It writes the exact upload set under
`release` without committing, tagging, signing, or publishing anything. See
[community-release.md](docs/community-release.md).

Apache-2.0. The Rust LZMS port also retains its source MIT license.
