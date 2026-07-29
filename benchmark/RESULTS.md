# Benchmark results

This run used 3,167 images and 1,964,362,720 pixels from DIV2K, CLIC 2022, the QOI benchmark suite, and Enrico.

Every output was decoded and compared with the normalized source. All 9,501 comparisons had zero different pixels.

The machine was an AMD Ryzen 9 9950X3D. Every encoder was pinned to logical processor 0.

QLIC 0.5.0 used pack with one thread.

WebP 1.6.0 used lossless, exact, and preset 6.

JPEG XL 0.12.0 used distance 0, effort 9, and no multithreading.

Wall time includes process startup and file IO. A negative size percentage means QLIC is smaller. A positive speed percentage means QLIC is faster.

## Overall

| Codec | Bytes | Encode seconds | MP per second | Maximum working set | Median working set |
| --- | ---: | ---: | ---: | ---: | ---: |
| QLIC | 1,172,509,205 | 561.743 | 3.497 | 217.5 MiB | 13.1 MiB |
| WebP 6 | 1,289,518,274 | 544.911 | 3.605 | 431.7 MiB | 19.6 MiB |
| JPEG XL 9 | 1,164,424,898 | 5,286.575 | 0.372 | 341.9 MiB | 25.6 MiB |

QLIC was 0.694 percent larger than JPEG XL 9 and encoded 9.411 times faster.

QLIC was 9.074 percent smaller than WebP 6 and encoded 3.089 percent slower.

Across equally weighted categories, QLIC was 0.715 percent larger than JPEG XL 9 and 7.706 percent smaller than WebP 6.

QLIC produced fewer bytes than JPEG XL on 1,536 of 3,167 images and fewer bytes than WebP on 2,083 images.

## Color model

| Color model | Images | MP | QLIC vs WebP size | QLIC vs JXL size | QLIC vs WebP speed | JXL time divided by QLIC |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| RGB | 2,448 | 1,507.856 | -8.972% | 0.495% | 4.106% | 10.059x |
| RGBA | 719 | 456.507 | -9.613% | 1.772% | -29.373% | 7.656x |

On opaque RGB images, QLIC was smaller and faster than WebP 6. RGBA candidate selection accounts for the overall speed difference.

## Categories

| Category | Images | MP | QLIC vs WebP size | QLIC vs JXL size | QLIC vs WebP speed | JXL time divided by QLIC |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| CLIC 2022 | 30 | 86.385 | -9.037% | -0.679% | 25.295% | 14.218x |
| DIV2K | 100 | 283.503 | -10.804% | 0.368% | 15.052% | 13.277x |
| QOI icon 512 | 213 | 55.837 | -7.160% | 2.800% | -44.789% | 4.858x |
| QOI icon 64 | 213 | 0.872 | -6.229% | -1.611% | 6.858% | 1.520x |
| Kodak | 24 | 9.437 | -9.000% | 3.074% | 27.437% | 12.420x |
| Tecnick | 100 | 144.000 | -9.303% | 1.589% | 3.193% | 12.459x |
| Wikipedia photos | 49 | 53.140 | -6.584% | -0.457% | 14.448% | 15.712x |
| PNG objects | 186 | 320.197 | -9.520% | 2.125% | -26.909% | 8.497x |
| Game screenshots | 618 | 391.207 | -8.549% | 1.443% | -14.935% | 8.545x |
| Web screenshots | 13 | 86.622 | -0.707% | 3.240% | 11.342% | 9.742x |
| Photographic textures | 20 | 20.972 | -11.141% | 0.424% | -2.803% | 12.680x |
| PK textures | 1,001 | 44.580 | 0.249% | -2.377% | -41.336% | 3.824x |
| PK01 textures | 113 | 14.680 | -5.204% | 1.622% | 26.457% | 5.658x |
| PK02 textures | 234 | 71.320 | -5.846% | 1.693% | 8.756% | 8.433x |
| Plant textures | 60 | 63.833 | -11.929% | 0.239% | -63.017% | 8.524x |
| Enrico UI | 193 | 317.779 | -12.538% | -2.055% | 23.815% | 9.755x |

Enrico UI was the strongest complete result. QLIC was smaller and faster than both comparison settings.

The main speed weaknesses were larger icons, PNG objects, game screenshots, the main PK texture set, and sparse alpha plant textures.

Two files were excluded before benchmarking. One exceeded the 16 million pixel corpus ceiling and one exceeded the shared WebP dimension limit. Two normalized duplicates were removed.
