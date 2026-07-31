# Independent benchmarks based on DIV2K, CLIC 2022, QOI, and Enrico

3,167 images, 1,964,362,720 pixels, one logical processor, and decoded pixel comparison.

| Codec | Settings | Total size | Encode time | Peak memory |
| --- | --- | ---: | ---: | ---: |
| QLIC 0.5.0 | 1 thread | 1,172,509,205 bytes | 561.743 s | 217.5 MiB |
| WebP 1.6.0 | lossless, exact, effort 6 | 1,289,518,274 bytes | 544.911 s | 431.7 MiB |
| JPEG XL 0.12.0 | lossless, effort 9 | 1,164,424,898 bytes | 5,286.575 s | 341.9 MiB |

QLIC was 9.074 percent smaller than WebP 6 and 0.694 percent larger than JPEG XL 9. It encoded 9.411 times faster than JPEG XL 9 and 3.089 percent slower than WebP 6. Every output decoded to the exact source pixels.

[Benchmark record](docs/benchmark.json)

The same corpus was used for a separate JPEG XL effort sweep.

| Codec | Settings | Total size | Encode time | Decode time |
| --- | --- | ---: | ---: | ---: |
| QLIC 0.5.0 | 1 thread | 1,172,509,205 bytes | 573.480 s | 314.416 s |
| JPEG XL 0.12.0 | lossless, effort 6 | 1,214,206,451 bytes | 754.620 s | 297.093 s |
| JPEG XL 0.12.0 | lossless, effort 7 | 1,187,851,480 bytes | 1,087.755 s | 309.095 s |
| JPEG XL 0.12.0 | lossless, effort 8 | 1,171,971,513 bytes | 2,977.934 s | 314.935 s |

QLIC was 3.434 percent smaller than JPEG XL 6, 1.292 percent smaller than JPEG XL 7, and 0.046 percent larger than JPEG XL 8. It encoded 5.193 times faster than JPEG XL 8. Decode time includes startup, file IO, and PNG output.

A practical PNG run used OxiPNG 10.1.1 at level 2. QLIC produced 1,172,509,205 bytes in 586.392 seconds. OxiPNG produced 1,629,681,431 bytes in 958.226 seconds. QLIC was 28.053 percent smaller and encoded 1.634 times faster. The separate in-memory decode test took 172.676 seconds for QLIC and 11.757 seconds for PNG.

[Full benchmark results](benchmark/RESULTS.md)

# QLIC 0.5 Demo

QLIC is a lossless still image and RGBA animation codec. The codec, C SDK, and command line tool run on Windows and Linux. The browser demo encodes still images and decodes the same QLIC files. Windows also includes a desktop demo and WIC decoder.

This release is built to show and test QLIC. This is not necessarily a production release.

## Layout

codec contains the implementation and public C header.

benchmark contains the reproducible benchmark.

third_party contains external headers and license records.

scripts contains the secondary build, package, and Windows integration commands.

## Build

Windows requires CMake 3.25 or newer and Visual Studio 2022 or newer with the C++ workload.

```powershell
.\build.ps1
```

LLVM is also required for the Clang and WebAssembly builds.

On Debian or Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build pkg-config libpng-dev libwebp-dev libjxl-dev libavif-dev libtiff-dev libwim-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

PNG and wimlib are required by the default Linux build. WebP, JPEG XL, AVIF, and TIFF input support is enabled when their development packages are available. Wimlib supplies the Linux LZMS encoder, while QLIC uses its own decoder.

For a decoder or SDK build without image libraries or wimlib, set `QLIC_BUILD_CLI=OFF` and `QLIC_LINUX_LZMS=OFF`.

## CLI

```text
qlic pack input.png output.qlic
qlic unpack input.qlic output.png
qlic info input.qlic
qlic batch output-directory image1.png image2.webp image3.jxl
```

The default is one thread. Use `--threads all` or `--threads N` when more are wanted. Every QLIC file created by this release decodes with the browser build.

Input supports PNG, lossless WebP, lossless JPEG XL, TIFF, and BMP on Windows and Linux. Lossless AVIF is built in on Linux and uses an installed WIC decoder on Windows. Windows also accepts GIF through WIC. JPEG and inputs known to be lossy are rejected. QLIC currently accepts up to 8 bits per channel.

Linux and Windows both test the optional LZMS outer stage and keep it only when it makes the file smaller. Both forms decode on Windows, Linux, and in the browser.

## GUI

Drop or choose an image, compress it, and see the percentage saved. The original file stays untouched until the QLIC result is saved.

## SDK

The public header is `codec/include/qlic/qlic.h`.

```cmake
find_package(qlic 0.5 CONFIG REQUIRED)
target_link_libraries(app PRIVATE qlic::qlic_static)
```

Use `qlic::qlic` for the DLL. See [docs/sdk.md](docs/sdk.md).

## WIC

```powershell
.\scripts\install-wic.ps1
.\scripts\uninstall-wic.ps1
```

Both scripts default to the current user. Pass `-Scope Machine` from an elevated PowerShell for a system-wide install.

## Package

```powershell
.\scripts\package.ps1
```

Release archives and SHA256 checksums are written to `dist`.

Apache 2.0.
