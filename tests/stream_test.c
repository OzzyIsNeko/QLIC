#include "stream.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void refresh_container_crc(uint8_t *data, size_t size) {
    if (!data || size < 30u)
        return;
    put32(data + 26u, 0);
    put32(data + 26u, stream_crc32(data, size));
}

static int reject_dimension_mismatch(int rgba) {
    uint8_t data[30] = {0};
    uint8_t *pixels = (uint8_t *)(uintptr_t)1;
    uint32_t w = 1, h = 1;
    int channels = 1;
    memcpy(data, "QST1", 4);
    put32(data + 4, UINT32_MAX);
    put32(data + 8, UINT32_MAX);
    data[12] = 3;
    data[14] = 40;
    data[16] = 1;
    int result = rgba
        ? stream_decode_trusted_expected_rgba(
              data, sizeof(data), 1, 1, 3, &pixels, &w, &h, &channels)
        : stream_decode_trusted_expected(
              data, sizeof(data), 1, 1, 3, &pixels, &w, &h, &channels);
    return result == STREAM_E_CORRUPT && !pixels && !w && !h && !channels;
}

static int strided_palette_roundtrip(void) {
    enum { W = 64, H = 64 };
    uint8_t source[W * H * 4];
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t *p =
                source + ((size_t)y * W + (size_t)x) * 4u;
            p[0] = (uint8_t)((x >> 3) & 1 ? 240 : 12);
            p[1] = (uint8_t)((y >> 3) & 1 ? 180 : 28);
            p[2] = (uint8_t)(((x >> 3) ^ (y >> 3)) & 1 ? 220 : 44);
            p[3] = 255;
        }
    }
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    int result = stream_encode_strided_threads(
        source, W, H, 3, 4, 1, 1, &encoded, &encoded_size);
    if (result != STREAM_OK || encoded_size <= 30 || encoded[14] != 1) {
        stream_free(encoded);
        return 0;
    }
    uint8_t *decoded = NULL;
    uint32_t w = 0, h = 0;
    int channels = 0;
    result = stream_decode_trusted_expected(
        encoded, encoded_size, W, H, 3, &decoded, &w, &h, &channels);
    stream_free(encoded);
    if (result != STREAM_OK || w != W || h != H || channels != 3) {
        stream_free(decoded);
        return 0;
    }
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        if (memcmp(decoded + i * 3u, source + i * 4u, 3u)) {
            stream_free(decoded);
            return 0;
        }
    }
    stream_free(decoded);
    return 1;
}

static int rgba_palette_alpha_roundtrip(void) {
    enum { W = 64, H = 64 };
    uint8_t source[W * H * 4];
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            unsigned value = (unsigned)((x >> 4) + 4 * (y >> 4));
            uint8_t *pixel =
                source + ((size_t)y * W + (size_t)x) * 4u;
            pixel[0] = (uint8_t)(value * 13u);
            pixel[1] = (uint8_t)(value * 7u);
            pixel[2] = (uint8_t)(255u - value * 11u);
            pixel[3] = (uint8_t)(value * 17u);
        }
    }
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    int result = stream_encode_threads(
        source, W, H, 4, 1, 1, &encoded, &encoded_size);
    if (result != STREAM_OK || encoded_size <= 30 || encoded[14] != 1) {
        stream_free(encoded);
        return 0;
    }
    uint8_t *decoded = NULL;
    uint32_t w = 0, h = 0;
    int channels = 0;
    result = stream_decode_trusted_expected_rgba(
        encoded, encoded_size, W, H, 4, &decoded, &w, &h, &channels);
    stream_free(encoded);
    if (result != STREAM_OK || w != W || h != H || channels != 4 ||
        memcmp(source, decoded, sizeof(source))) {
        stream_free(decoded);
        return 0;
    }
    stream_free(decoded);
    return 1;
}

static int strided_crc_roundtrip(void) {
    enum { PIXELS = 17 };
    uint8_t source[PIXELS * 4];
    uint8_t rgb[PIXELS * 3];
    uint8_t gray[PIXELS];
    for (size_t i = 0; i < PIXELS; ++i) {
        source[i * 4u] = (uint8_t)(i * 13u + 7u);
        source[i * 4u + 1u] = (uint8_t)(i * 29u + 3u);
        source[i * 4u + 2u] = (uint8_t)(i * 47u + 11u);
        source[i * 4u + 3u] = (uint8_t)i;
        memcpy(rgb + i * 3u, source + i * 4u, 3u);
        gray[i] = source[i * 4u];
    }
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    int result = stream_encode_strided_threads(
        source, PIXELS, 1, 3, 4, 0, 1, &encoded, &encoded_size);
    if (result != STREAM_OK || encoded_size < 30 ||
        get32(encoded + 18) != stream_crc32(rgb, sizeof(rgb))) {
        stream_free(encoded);
        return 0;
    }
    stream_free(encoded);
    encoded = NULL;
    encoded_size = 0;
    result = stream_encode_strided_threads(
        source, PIXELS, 1, 1, 4, 0, 1, &encoded, &encoded_size);
    if (result != STREAM_OK || encoded_size < 30 ||
        get32(encoded + 18) != stream_crc32(gray, sizeof(gray))) {
        stream_free(encoded);
        return 0;
    }
    stream_free(encoded);
    return 1;
}

static int stream_info_roundtrip(void) {
    enum { W = 31, H = 17 };
    uint8_t source[W * H * 3];
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        source[i * 3u] = (uint8_t)(i * 17u + i / W);
        source[i * 3u + 1u] = (uint8_t)(i * 31u + i / 7u);
        source[i * 3u + 2u] = (uint8_t)(i * 47u + i / 11u);
    }
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    int result = stream_encode_threads(
        source, W, H, 3, 1, 1, &encoded, &encoded_size);
    if (result != STREAM_OK) {
        stream_free(encoded);
        return 0;
    }
    StreamInfo info;
    result = stream_get_info(encoded, encoded_size, &info);
    int expected_adaptation =
        encoded[17] == 4 || encoded[17] == 6 ? encoded[17] : 5;
    int valid =
        result == STREAM_OK &&
        info.width == W &&
        info.height == H &&
        info.channels == 3 &&
        info.mode == encoded[14] &&
        info.transform == encoded[15] &&
        info.tile_log == encoded[16] &&
        info.control == encoded[17] &&
        info.adaptation == expected_adaptation &&
        info.sample_bits == 0 &&
        info.pixel_checksum == get32(encoded + 18) &&
        info.payload_size == get32(encoded + 22);
    stream_free(encoded);
    return valid;
}

static uint8_t restore_sample(unsigned compact, unsigned maximum) {
    return (uint8_t)((compact * 255u + maximum / 2u) / maximum);
}

static int sample_grid_roundtrip(void) {
    enum { W = 257, H = 33, BITS = 5, MAXIMUM = 31 };
    uint8_t source[W * H * 3];
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t *pixel =
                source + ((size_t)y * W + (size_t)x) * 3u;
            pixel[0] = restore_sample((unsigned)(x + y * 3) & MAXIMUM,
                                      MAXIMUM);
            pixel[1] = restore_sample((unsigned)(x * 3 + y * 5) & MAXIMUM,
                                      MAXIMUM);
            pixel[2] = restore_sample((unsigned)(x * 7 + y * 11) & MAXIMUM,
                                      MAXIMUM);
        }
    }
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    int result = stream_encode_threads(
        source, W, H, 3, 1, 1, &encoded, &encoded_size);
    if (result != STREAM_OK) {
        stream_free(encoded);
        return 0;
    }
    StreamInfo info;
    result = stream_get_info(encoded, encoded_size, &info);
    if (result != STREAM_OK || info.sample_bits != BITS) {
        stream_free(encoded);
        return 0;
    }
    uint8_t *decoded = NULL;
    uint32_t w = 0, h = 0;
    int channels = 0;
    result = stream_decode_trusted_expected(
        encoded, encoded_size, W, H, 3, &decoded, &w, &h, &channels);
    if (result != STREAM_OK || w != W || h != H || channels != 3 ||
        memcmp(source, decoded, sizeof(source))) {
        stream_free(encoded);
        stream_free(decoded);
        return 0;
    }
    encoded[13] |= 32u;
    refresh_container_crc(encoded, encoded_size);
    result = stream_get_info(encoded, encoded_size, &info);
    stream_free(encoded);
    stream_free(decoded);
    return result == STREAM_E_FORMAT;
}

static int sample_grid_gray_roundtrip(void) {
    enum { W = 263, H = 31, BITS = 6, MAXIMUM = 63 };
    uint8_t source[W * H];
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            source[(size_t)y * W + (size_t)x] =
                restore_sample((unsigned)(x * 5 + y * 13) & MAXIMUM,
                               MAXIMUM);
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    int result = stream_encode_threads(
        source, W, H, 1, 1, 1, &encoded, &encoded_size);
    if (result != STREAM_OK) {
        stream_free(encoded);
        return 0;
    }
    StreamInfo info;
    result = stream_get_info(encoded, encoded_size, &info);
    if (result != STREAM_OK || info.sample_bits != BITS) {
        stream_free(encoded);
        return 0;
    }
    uint8_t *decoded = NULL;
    uint32_t w = 0, h = 0;
    int channels = 0;
    result = stream_decode_trusted_expected(
        encoded, encoded_size, W, H, 1, &decoded, &w, &h, &channels);
    stream_free(encoded);
    if (result != STREAM_OK || w != W || h != H || channels != 1 ||
        memcmp(source, decoded, sizeof(source))) {
        stream_free(decoded);
        return 0;
    }
    stream_free(decoded);
    return 1;
}

static int symmetric_transform_roundtrip(int expected_transform) {
    enum { W = 320, H = 257 };
    size_t bytes = (size_t)W * H * 3u;
    uint8_t *source = (uint8_t *)malloc(bytes);
    if (!source) return 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t *pixel =
                source + ((size_t)y * W + (size_t)x) * 3u;
            int blue = (x * 3 + y * 5 + (x ^ y)) & 255;
            int other = (x * 11 + y * 7 + (x * y >> 3)) & 255;
            pixel[2] = (uint8_t)blue;
            if (expected_transform == 36) {
                pixel[0] = (uint8_t)other;
                pixel[1] =
                    (uint8_t)((40 * other + 24 * blue) >> 6);
            } else {
                pixel[1] = (uint8_t)other;
                pixel[0] =
                    (uint8_t)((40 * other + 24 * blue) >> 6);
            }
        }
    }
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    int result = stream_encode_threads(
        source, W, H, 3, 0, 1, &encoded, &encoded_size);
    if (result != STREAM_OK || encoded_size <= 30 ||
        encoded[15] != expected_transform) {
        free(source);
        stream_free(encoded);
        return 0;
    }
    uint8_t *decoded = NULL;
    uint32_t w = 0, h = 0;
    int channels = 0;
    result = stream_decode_trusted_expected(
        encoded, encoded_size, W, H, 3, &decoded, &w, &h, &channels);
    int exact = result == STREAM_OK && w == W && h == H && channels == 3 &&
                !memcmp(source, decoded, bytes);
    free(source);
    stream_free(encoded);
    stream_free(decoded);
    return exact;
}

static int sphere_normal_prediction(int red, int green) {
    static const uint8_t correction[64] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
        1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5,
        6, 6, 7, 7, 8, 8, 9, 9, 10, 11, 12, 12, 13, 14, 15, 16,
        17, 18, 19, 21, 22, 24, 25, 27, 29, 31, 34, 37, 41, 45, 53, 63
    };
    int x = red - 128;
    int y = green - 128;
    unsigned radius2 = (unsigned)(x * x + y * y);
    if (radius2 >= 16129u) return 128;
    return 255 - (int)((radius2 + 127u) >> 8) -
           correction[radius2 >> 8];
}

static int normal_map_transform_roundtrip(uint32_t width, uint32_t height,
                                          int channels, int constant_alpha,
                                          int transform, unsigned seed) {
    size_t pixels = (size_t)width * height;
    if (!pixels || pixels > SIZE_MAX / (size_t)channels) return 0;
    size_t bytes = pixels * (size_t)channels;
    uint8_t *source = (uint8_t *)malloc(bytes);
    if (!source) return 0;
    static const uint8_t extrema[8][3] = {
        {0, 0, 0}, {255, 255, 255}, {0, 255, 128}, {255, 0, 127},
        {128, 128, 0}, {128, 128, 255}, {127, 129, 1}, {129, 127, 254}
    };
    for (size_t i = 0; i < pixels; ++i) {
        uint8_t *pixel = source + i * (size_t)channels;
        if (i < sizeof(extrema) / sizeof(extrema[0])) {
            size_t extrema_index = pixels == 1u ? 2u : i;
            memcpy(pixel, extrema[extrema_index], 3u);
        } else {
            unsigned x = (unsigned)(i % width);
            unsigned y = (unsigned)(i / width);
            int r = (int)((x * 37u + y * 13u + seed * 17u) & 255u);
            int g = (int)((x * 11u + y * 43u + seed * 29u) & 255u);
            int dx = r - 128;
            int dy = g - 128;
            int q = transform >= 39
                        ? sphere_normal_prediction(r, g)
                        : 255 - ((dx * dx + dy * dy + 127) >> 8);
            if (q < 128) q = 128;
            int residual = (int)((x * 5u + y * 7u + seed) % 31u) - 15;
            int b = q + residual;
            if (b < 0) b = 0;
            if (b > 255) b = 255;
            pixel[0] = (uint8_t)r;
            pixel[1] = (uint8_t)g;
            pixel[2] = (uint8_t)b;
        }
        if (channels == 4)
            pixel[3] = constant_alpha
                           ? 173u
                           : (uint8_t)(i * 53u + seed * 31u);
    }
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    int result = stream_benchmark_encode_trial(
        source, width, height, channels, 52, transform, 4, 5,
        &encoded, &encoded_size);
    StreamInfo info;
    memset(&info, 0, sizeof(info));
    int info_result = result == STREAM_OK
                          ? stream_get_info(encoded, encoded_size, &info)
                          : result;
    if (result != STREAM_OK || encoded_size <= 30u ||
        info_result != STREAM_OK ||
        info.mode != 52 || info.transform != transform ||
        info.width != width || info.height != height ||
        info.channels != channels) {
        fprintf(stderr,
                "transform%d metadata failure: %ux%u ch=%d error=%d info=%d "
                "size=%zu mode=%d transform=%d parsed=%ux%u/%d\n",
                transform, width, height, channels, result, info_result,
                encoded_size,
                encoded_size > 15u ? encoded[14] : -1,
                encoded_size > 15u ? encoded[15] : -1,
                info.width, info.height, info.channels);
        free(source);
        stream_free(encoded);
        return 0;
    }
    uint8_t *decoded = NULL;
    uint32_t decoded_width = 0, decoded_height = 0;
    int decoded_channels = 0;
    result = stream_decode_trusted_expected(
        encoded, encoded_size, width, height, channels, &decoded,
        &decoded_width, &decoded_height, &decoded_channels);
    int exact = result == STREAM_OK && decoded_width == width &&
                decoded_height == height && decoded_channels == channels &&
                !memcmp(source, decoded, bytes);
    if (!exact)
        fprintf(stderr,
                "transform%d exactness failure: %ux%u ch=%d error=%d "
                "decoded=%ux%u/%d\n",
                transform, width, height, channels, result, decoded_width,
                decoded_height, decoded_channels);
    encoded[15] = 41;
    refresh_container_crc(encoded, encoded_size);
    exact &= stream_get_info(encoded, encoded_size, &info) == STREAM_E_FORMAT;
    free(source);
    stream_free(encoded);
    stream_free(decoded);
    return exact;
}

static int mode52_adaptation_roundtrip(int adaptation) {
    enum { W = 67, H = 61, CHANNELS = 3 };
    uint8_t source[(size_t)W * H * CHANNELS];
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        unsigned x = (unsigned)(i % W);
        unsigned y = (unsigned)(i / W);
        source[i * CHANNELS] = (uint8_t)(x * 17u + y * 5u);
        source[i * CHANNELS + 1u] = (uint8_t)(x * 3u + y * 29u);
        source[i * CHANNELS + 2u] =
            (uint8_t)(x * 11u + y * 7u + (x ^ y));
    }
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    int result = stream_benchmark_encode_trial(
        source, W, H, CHANNELS, 52, 2, 4, adaptation,
        &encoded, &encoded_size);
    uint8_t *decoded = NULL;
    uint32_t width = 0, height = 0;
    int channels = 0;
    if (result == STREAM_OK)
        result = stream_decode_trusted_expected(
            encoded, encoded_size, W, H, CHANNELS, &decoded, &width, &height,
            &channels);
    int exact = result == STREAM_OK && width == W && height == H &&
                channels == CHANNELS && !memcmp(source, decoded, sizeof(source));
    stream_free(encoded);
    stream_free(decoded);
    return exact;
}

static int mode54_defaults_to_faster_mode53(void) {
    enum { OUTER_HEADER_SIZE = 28, OUTER_FOOTER_SIZE = 4 };
    FILE *file = NULL;
#ifdef _WIN32
    if (fopen_s(&file, QLIC_MODE54_FIXTURE, "rb")) file = NULL;
#else
    file = fopen(QLIC_MODE54_FIXTURE, "rb");
#endif
    if (!file || fseek(file, 0, SEEK_END)) {
        if (file) fclose(file);
        return 0;
    }
    long length = ftell(file);
    if (length <= OUTER_HEADER_SIZE + OUTER_FOOTER_SIZE ||
        fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return 0;
    }
    uint8_t *container = (uint8_t *)malloc((size_t)length);
    if (!container || fread(container, 1, (size_t)length, file) !=
                          (size_t)length) {
        free(container);
        fclose(file);
        return 0;
    }
    fclose(file);
    size_t native_size = get32(container + 20u);
    if (memcmp(container, "QLIC", 4u) || container[12] != 9u ||
        (container[15] & 0x7fu) != 0u || get32(container + 24u) != 0u ||
        native_size != (size_t)length - OUTER_HEADER_SIZE -
                           OUTER_FOOTER_SIZE) {
        free(container);
        return 0;
    }
    const uint8_t *native = container + OUTER_HEADER_SIZE;
    StreamInfo old_info;
    if (memcmp(native, "QST1", 4u) ||
        stream_get_info(native, native_size, &old_info) != STREAM_OK ||
        old_info.mode != 54) {
        free(container);
        return 0;
    }
    uint8_t *pixels = NULL;
    uint32_t width = 0, height = 0;
    int channels = 0;
    int result = stream_decode_trusted_expected(
        native, native_size, old_info.width, old_info.height,
        old_info.channels, &pixels, &width, &height, &channels);
    if (result != STREAM_OK || width != old_info.width ||
        height != old_info.height || channels != old_info.channels) {
        free(container);
        stream_free(pixels);
        return 0;
    }
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    result = stream_encode_threads(
        pixels, width, height, channels, 1, 1, &encoded, &encoded_size);
    StreamInfo new_info;
    int exact = result == STREAM_OK && encoded_size > 30u &&
                stream_get_info(encoded, encoded_size, &new_info) ==
                    STREAM_OK &&
                new_info.mode == 53 &&
                new_info.transform == old_info.transform;
    uint8_t *decoded = NULL;
    uint32_t decoded_width = 0, decoded_height = 0;
    int decoded_channels = 0;
    if (exact) {
        result = stream_decode_trusted_expected(
            encoded, encoded_size, width, height, channels, &decoded,
            &decoded_width, &decoded_height, &decoded_channels);
        size_t bytes = (size_t)width * height * (size_t)channels;
        exact = result == STREAM_OK && decoded_width == width &&
                decoded_height == height &&
                decoded_channels == channels &&
                !memcmp(pixels, decoded, bytes);
    }
    free(container);
    stream_free(pixels);
    stream_free(encoded);
    stream_free(decoded);
    return exact;
}

int main(void) {
    if (!reject_dimension_mismatch(0)) return 1;
    if (!reject_dimension_mismatch(1)) return 2;
    if (!strided_palette_roundtrip()) return 3;
    if (!strided_crc_roundtrip()) return 4;
    if (!stream_info_roundtrip()) return 5;
    if (!sample_grid_roundtrip()) return 6;
    if (!sample_grid_gray_roundtrip()) return 7;
    if (!rgba_palette_alpha_roundtrip()) return 8;
    if (!symmetric_transform_roundtrip(36)) return 9;
    if (!symmetric_transform_roundtrip(37)) return 10;
    if (!normal_map_transform_roundtrip(1, 1, 3, 0, 38, 1)) return 11;
    if (!normal_map_transform_roundtrip(7, 127, 3, 0, 38, 2)) return 12;
    if (!normal_map_transform_roundtrip(11, 128, 3, 0, 38, 3)) return 13;
    if (!normal_map_transform_roundtrip(13, 129, 3, 0, 38, 4)) return 14;
    if (!normal_map_transform_roundtrip(17, 129, 4, 1, 38, 5)) return 15;
    if (!normal_map_transform_roundtrip(19, 129, 4, 0, 38, 6)) return 16;
    if (!normal_map_transform_roundtrip(1, 1, 3, 0, 39, 7)) return 17;
    if (!normal_map_transform_roundtrip(11, 128, 3, 0, 39, 8)) return 18;
    if (!normal_map_transform_roundtrip(17, 129, 4, 1, 39, 9)) return 19;
    if (!normal_map_transform_roundtrip(19, 129, 4, 0, 39, 10)) return 20;
    if (!normal_map_transform_roundtrip(1, 1, 3, 0, 40, 11)) return 21;
    if (!normal_map_transform_roundtrip(11, 128, 3, 0, 40, 12)) return 22;
    if (!normal_map_transform_roundtrip(17, 129, 4, 1, 40, 13)) return 23;
    if (!normal_map_transform_roundtrip(19, 129, 4, 0, 40, 14)) return 24;
    for (unsigned seed = 11; seed < 75; ++seed) {
        uint32_t width = 1u + seed % 65u;
        uint32_t height = 1u + (seed * 17u) % 137u;
        int channels = seed & 1u ? 3 : 4;
        int constant_alpha = channels == 4 && (seed % 3u) == 0u;
        if (!normal_map_transform_roundtrip(
                width, height, channels, constant_alpha, 39, seed))
            return 25;
        if (!normal_map_transform_roundtrip(
                width, height, channels, constant_alpha, 40, seed))
            return 26;
    }
    if (!mode52_adaptation_roundtrip(4)) return 27;
    if (!mode52_adaptation_roundtrip(5)) return 28;
    if (!mode52_adaptation_roundtrip(6)) return 29;
    if (!mode54_defaults_to_faster_mode53()) return 30;
    return 0;
}
