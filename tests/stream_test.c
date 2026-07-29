#include "stream.h"

#include <stdint.h>
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

int main(void) {
    if (!reject_dimension_mismatch(0)) return 1;
    if (!reject_dimension_mismatch(1)) return 2;
    if (!strided_palette_roundtrip()) return 3;
    if (!strided_crc_roundtrip()) return 4;
    if (!stream_info_roundtrip()) return 5;
    if (!sample_grid_roundtrip()) return 6;
    if (!sample_grid_gray_roundtrip()) return 7;
    if (!rgba_palette_alpha_roundtrip()) return 8;
    return 0;
}
