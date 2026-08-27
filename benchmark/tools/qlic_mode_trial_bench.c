#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "stream.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

typedef struct {
    char *path;
    uint8_t *baseline;
    uint8_t *candidate;
    size_t baseline_size;
    size_t candidate_size;
    uint32_t width;
    uint32_t height;
    int channels;
    int baseline_mode;
    int candidate_mode;
    int transform;
    int candidate_transform;
    int baseline_tile_log;
    int candidate_tile_log;
    int baseline_adaptation;
    int candidate_adaptation;
    uint64_t current_transform_score;
    uint64_t candidate_transform_score;
    uint64_t transform_samples;
    double candidate_encode_seconds;
} Pair;

static double now_seconds(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec value;
    (void)clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
#endif
}

static int pin_processor(unsigned processor) {
#ifdef _WIN32
    if (processor >= (unsigned)(sizeof(DWORD_PTR) * CHAR_BIT)) return 0;
    DWORD_PTR mask = (DWORD_PTR)1u << processor;
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#else
    (void)processor;
    return 1;
#endif
}

static FILE *open_file(const char *path, const char *mode) {
#ifdef _WIN32
    FILE *file = NULL;
    return fopen_s(&file, path, mode) == 0 ? file : NULL;
#else
    return fopen(path, mode);
#endif
}

static char *duplicate_text(const char *text) {
    size_t length = strlen(text) + 1u;
    char *copy = (char *)malloc(length);
    if (copy) memcpy(copy, text, length);
    return copy;
}

static int read_file(const char *path, uint8_t **data_out, size_t *size_out) {
    FILE *file = open_file(path, "rb");
    long length = 0;
    uint8_t *data = NULL;
    *data_out = NULL;
    *size_out = 0;
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return 0;
    }
    size_t size = (size_t)length;
    data = (uint8_t *)malloc(size ? size : 1u);
    if (!data || fread(data, 1, size, file) != size) {
        free(data);
        fclose(file);
        return 0;
    }
    if (fclose(file)) {
        free(data);
        return 0;
    }
    *data_out = data;
    *size_out = size;
    return 1;
}

static uint32_t read_u32le(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint64_t read_u64le(const uint8_t *data) {
    return (uint64_t)read_u32le(data) |
           ((uint64_t)read_u32le(data + 4) << 32);
}

static int read_native_payload(const char *path, uint8_t **data_out,
                               size_t *size_out) {
    enum { QLIC_HEADER_SIZE = 28, QLIC_FOOTER_SIZE = 4 };
    uint8_t *file = NULL;
    size_t file_size = 0;
    *data_out = NULL;
    *size_out = 0;
    if (!read_file(path, &file, &file_size)) return 0;
    if (file_size >= 4u && memcmp(file, "QST1", 4) == 0) {
        *data_out = file;
        *size_out = file_size;
        return 1;
    }
    if (file_size < QLIC_HEADER_SIZE + QLIC_FOOTER_SIZE ||
        memcmp(file, "QLIC", 4) != 0 || file[12] != 9 ||
        file[13] != 0 || file[14] != 0 || file[15] != 0x80u ||
        read_u32le(file + 16) != 0 ||
        stream_crc32(file, file_size - QLIC_FOOTER_SIZE) !=
            read_u32le(file + file_size - QLIC_FOOTER_SIZE)) {
        free(file);
        return 0;
    }
    uint64_t payload64 = read_u64le(file + 20);
    if (payload64 > SIZE_MAX ||
        (size_t)payload64 !=
            file_size - QLIC_HEADER_SIZE - QLIC_FOOTER_SIZE) {
        free(file);
        return 0;
    }
    size_t payload_size = (size_t)payload64;
    uint8_t *payload = (uint8_t *)malloc(payload_size ? payload_size : 1u);
    if (!payload) {
        free(file);
        return 0;
    }
    memcpy(payload, file + QLIC_HEADER_SIZE, payload_size);
    free(file);
    *data_out = payload;
    *size_out = payload_size;
    return 1;
}

static void free_pair(Pair *pair) {
    free(pair->path);
    free(pair->baseline);
    stream_free(pair->candidate);
    memset(pair, 0, sizeof(*pair));
}

static void free_pairs(Pair *pairs, size_t count) {
    for (size_t i = 0; i < count; ++i) free_pair(&pairs[i]);
    free(pairs);
}

static int read_manifest(const char *path, Pair **pairs_out,
                         size_t *count_out) {
    FILE *manifest = open_file(path, "r");
    Pair *pairs = NULL;
    size_t count = 0;
    size_t capacity = 0;
    char line[32768];
    *pairs_out = NULL;
    *count_out = 0;
    if (!manifest) return 0;
    while (fgets(line, (int)sizeof(line), manifest)) {
        size_t length = strlen(line);
        while (length && (line[length - 1u] == '\n' ||
                          line[length - 1u] == '\r'))
            line[--length] = '\0';
        if (!length) continue;
        if (count == capacity) {
            size_t next = capacity ? capacity * 2u : 32u;
            if (next < capacity || next > SIZE_MAX / sizeof(*pairs)) {
                fclose(manifest);
                free_pairs(pairs, count);
                return 0;
            }
            Pair *grown = (Pair *)realloc(pairs, next * sizeof(*pairs));
            if (!grown) {
                fclose(manifest);
                free_pairs(pairs, count);
                return 0;
            }
            pairs = grown;
            memset(pairs + capacity, 0,
                   (next - capacity) * sizeof(*pairs));
            capacity = next;
        }
        pairs[count].path = duplicate_text(line);
        if (!pairs[count].path) {
            fclose(manifest);
            free_pairs(pairs, count + 1u);
            return 0;
        }
        ++count;
    }
    if (ferror(manifest) || fclose(manifest) || !count) {
        free_pairs(pairs, count);
        return 0;
    }
    *pairs_out = pairs;
    *count_out = count;
    return 1;
}

static int prepare_pair(Pair *pair, int candidate_mode, int tile_override,
                        int adaptation_override, int transform_override) {
    StreamInfo baseline_info;
    StreamInfo candidate_info;
    uint8_t *pixels = NULL;
    uint8_t *check = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    int channels = 0;
    int status = 0;
    if (!read_native_payload(pair->path, &pair->baseline,
                             &pair->baseline_size)) {
        fprintf(stderr, "could not read stored native payload: %s\n",
                pair->path);
        return 0;
    }
    int error = stream_get_info(pair->baseline, pair->baseline_size,
                                &baseline_info);
    if (error != STREAM_OK ||
        (baseline_info.mode != 37 && baseline_info.mode != 41 &&
         baseline_info.mode != 45 && baseline_info.mode != 52 &&
         baseline_info.mode != 53 && baseline_info.mode != 54)) {
        fprintf(stderr,
                "expected a full-precision context-mode stream: %s (%s)\n",
                pair->path, stream_strerror(error));
        return 0;
    }
    error = stream_decode_trusted_expected(
        pair->baseline, pair->baseline_size, baseline_info.width,
        baseline_info.height, baseline_info.channels, &pixels, &width,
        &height, &channels);
    if (error != STREAM_OK || width != baseline_info.width ||
        height != baseline_info.height || channels != baseline_info.channels) {
        fprintf(stderr, "baseline decode failed: %s (%s)\n", pair->path,
                stream_strerror(error));
        goto cleanup;
    }
    int candidate_tile_log =
        tile_override >= 0 ? tile_override : baseline_info.tile_log;
    int candidate_adaptation = adaptation_override >= 0
                                   ? adaptation_override
                                   : baseline_info.adaptation;
    int candidate_transform = transform_override >= 0
                                  ? transform_override
                                  : baseline_info.transform;
    if (candidate_transform >= 38) {
        error = stream_benchmark_transform_scores(
            pixels, width, height, channels,
            candidate_transform,
            &pair->current_transform_score,
            &pair->candidate_transform_score, &pair->transform_samples);
        if (error != STREAM_OK) {
            fprintf(stderr, "candidate transform score failed: %s (%s)\n",
                    pair->path, stream_strerror(error));
            goto cleanup;
        }
    }
    double encode_begin = now_seconds();
    error = stream_benchmark_encode_trial(
        pixels, width, height, channels, candidate_mode,
        candidate_transform, candidate_tile_log, candidate_adaptation,
        &pair->candidate, &pair->candidate_size);
    pair->candidate_encode_seconds = now_seconds() - encode_begin;
    if (error != STREAM_OK) {
        fprintf(stderr, "candidate encode failed: %s (%s)\n", pair->path,
                stream_strerror(error));
        goto cleanup;
    }
    error = stream_get_info(pair->candidate, pair->candidate_size,
                            &candidate_info);
    if (error != STREAM_OK || candidate_info.mode != candidate_mode ||
        candidate_info.transform != candidate_transform ||
        candidate_info.tile_log != candidate_tile_log ||
        candidate_info.adaptation != candidate_adaptation ||
        candidate_info.sample_bits != baseline_info.sample_bits) {
        fprintf(stderr, "candidate metadata mismatch: %s\n", pair->path);
        goto cleanup;
    }
    error = stream_decode_trusted_expected(
        pair->candidate, pair->candidate_size, width, height, channels,
        &check, &width, &height, &channels);
    size_t pixel_count = (size_t)baseline_info.width * baseline_info.height;
    size_t byte_count = pixel_count * (size_t)baseline_info.channels;
    if (error != STREAM_OK || memcmp(pixels, check, byte_count) != 0) {
        if (error == STREAM_OK && check) {
            size_t first = 0;
            while (first < byte_count && pixels[first] == check[first])
                ++first;
            if (first < byte_count)
                fprintf(stderr,
                        "first pixel difference: byte=%zu pixel=%zu "
                        "channel=%zu expected=%u actual=%u\n",
                        first, first / (size_t)baseline_info.channels,
                        first % (size_t)baseline_info.channels,
                        (unsigned)pixels[first], (unsigned)check[first]);
        }
        fprintf(stderr, "candidate exactness failed: %s (%s)\n", pair->path,
                stream_strerror(error));
        goto cleanup;
    }
    pair->width = baseline_info.width;
    pair->height = baseline_info.height;
    pair->channels = baseline_info.channels;
    pair->baseline_mode = baseline_info.mode;
    pair->candidate_mode = candidate_mode;
    pair->transform = baseline_info.transform;
    pair->candidate_transform = candidate_transform;
    pair->baseline_tile_log = baseline_info.tile_log;
    pair->candidate_tile_log = candidate_tile_log;
    pair->baseline_adaptation = baseline_info.adaptation;
    pair->candidate_adaptation = candidate_adaptation;
    status = 1;

cleanup:
    stream_free(check);
    stream_free(pixels);
    return status;
}

static int decode_set(const Pair *pairs, size_t count, int candidate,
                      uint64_t *checksum) {
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *data = candidate ? pairs[i].candidate
                                        : pairs[i].baseline;
        size_t size = candidate ? pairs[i].candidate_size
                                : pairs[i].baseline_size;
        uint8_t *pixels = NULL;
        uint32_t width = 0;
        uint32_t height = 0;
        int channels = 0;
        int error = stream_decode_trusted_expected(
            data, size, pairs[i].width, pairs[i].height, pairs[i].channels,
            &pixels, &width, &height, &channels);
        if (error != STREAM_OK || width != pairs[i].width ||
            height != pairs[i].height || channels != pairs[i].channels) {
            stream_free(pixels);
            return 0;
        }
        size_t bytes = (size_t)width * height * (size_t)channels;
        *checksum += pixels[bytes / 2u];
        stream_free(pixels);
    }
    return 1;
}

static void write_csv_text(FILE *file, const char *text) {
    fputc('"', file);
    for (const char *cursor = text; *cursor; ++cursor) {
        if (*cursor == '"') fputc('"', file);
        fputc(*cursor, file);
    }
    fputc('"', file);
}

static int write_results(const char *path, const Pair *pairs, size_t count,
                         long runs, double baseline_seconds,
                         double candidate_seconds) {
    FILE *file = open_file(path, "w");
    if (!file) return 0;
    fprintf(file,
            "Index,Path,Pixels,Width,Height,Channels,"
            "BaselineMode,CandidateMode,"
            "Transform,BaselineTileLog,CandidateTileLog,"
            "BaselineAdaptation,CandidateAdaptation,BaselineBytes,"
            "CandidateBytes,DeltaBytes,SizeDeltaPercent,"
            "CurrentTransformScore,CandidateTransformScore,TransformSamples,"
            "CandidateEncodeSeconds,Exact,Runs,BaselineSeconds,"
            "CandidateSeconds,DecodeDeltaPercent,CandidateTransform\n");
    double decode_delta =
        100.0 * (candidate_seconds / baseline_seconds - 1.0);
    for (size_t i = 0; i < count; ++i) {
        uint64_t pixels = (uint64_t)pairs[i].width * pairs[i].height;
        int64_t delta = (int64_t)pairs[i].candidate_size -
                        (int64_t)pairs[i].baseline_size;
        double size_delta =
            100.0 * ((double)pairs[i].candidate_size /
                         (double)pairs[i].baseline_size -
                     1.0);
        fprintf(file, "%zu,", i);
        write_csv_text(file, pairs[i].path);
        fprintf(file,
                ",%" PRIu64 ",%u,%u,%d,%d,%d,%d,%d,%d,%d,%d,%zu,%zu,%" PRId64
                ",%.9f,%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%.9f,1,%ld,%.9f,%.9f,%.9f,%d\n",
                pixels, pairs[i].width, pairs[i].height, pairs[i].channels,
                pairs[i].baseline_mode,
                pairs[i].candidate_mode, pairs[i].transform,
                pairs[i].baseline_tile_log, pairs[i].candidate_tile_log,
                pairs[i].baseline_adaptation,
                pairs[i].candidate_adaptation,
                pairs[i].baseline_size, pairs[i].candidate_size, delta,
                size_delta, pairs[i].current_transform_score,
                pairs[i].candidate_transform_score,
                pairs[i].transform_samples,
                pairs[i].candidate_encode_seconds, runs,
                baseline_seconds, candidate_seconds,
                decode_delta, pairs[i].candidate_transform);
    }
    return fclose(file) == 0;
}

static int parse_long(const char *text, long minimum, long maximum,
                      long *value_out) {
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || !end || *end || value < minimum || value > maximum)
        return 0;
    *value_out = value;
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 6 && argc != 8 && argc != 9) {
        fprintf(stderr,
                "usage: qlic-mode-trial-bench runs processor "
                "candidate-mode streams.txt results.csv "
                "[candidate-tile-log candidate-adaptation "
                "candidate-transform]\n");
        return 2;
    }
    long runs = 0;
    long processor = 0;
    long candidate_mode = 0;
    long tile_override = -1;
    long adaptation_override = -1;
    long transform_override = -1;
    if (!parse_long(argv[1], 1, 100000, &runs) ||
        !parse_long(argv[2], 0, 62, &processor) ||
        !parse_long(argv[3], 37, 54, &candidate_mode) ||
        (candidate_mode != 37 && candidate_mode != 41 &&
         candidate_mode != 45 && candidate_mode != 52 &&
         candidate_mode != 53 && candidate_mode != 54)) {
        fprintf(stderr, "invalid numeric argument\n");
        return 2;
    }
    if (argc >= 8 &&
        (!parse_long(argv[6], -1, 7, &tile_override) ||
         !parse_long(argv[7], -1, 6, &adaptation_override) ||
         (adaptation_override != -1 && adaptation_override != 4 &&
          adaptation_override != 5 && adaptation_override != 6))) {
        fprintf(stderr, "invalid candidate override\n");
        return 2;
    }
    if (argc == 9 &&
        !parse_long(argv[8], -1, 40, &transform_override)) {
        fprintf(stderr, "invalid candidate transform\n");
        return 2;
    }
    Pair *pairs = NULL;
    size_t count = 0;
    if (!read_manifest(argv[4], &pairs, &count)) {
        fprintf(stderr, "could not load stream manifest\n");
        return 3;
    }
    if (!pin_processor((unsigned)processor)) {
        fprintf(stderr, "could not pin processor %ld\n", processor);
        free_pairs(pairs, count);
        return 5;
    }
    stream_set_threads(1);
    for (size_t i = 0; i < count; ++i) {
        if (!prepare_pair(&pairs[i], (int)candidate_mode,
                          (int)tile_override,
                          (int)adaptation_override,
                          (int)transform_override)) {
            free_pairs(pairs, count);
            return 4;
        }
    }
    uint64_t checksum = 0;
    if (!decode_set(pairs, count, 0, &checksum) ||
        !decode_set(pairs, count, 1, &checksum)) {
        fprintf(stderr, "warm decode failed\n");
        free_pairs(pairs, count);
        return 6;
    }
    double baseline_seconds = 0.0;
    double candidate_seconds = 0.0;
    for (long run = 0; run < runs; ++run) {
        for (int pass = 0; pass < 2; ++pass) {
            int candidate = (int)((run + pass) & 1L);
            double begin = now_seconds();
            if (!decode_set(pairs, count, candidate, &checksum)) {
                fprintf(stderr, "timed decode failed\n");
                free_pairs(pairs, count);
                return 7;
            }
            double elapsed = now_seconds() - begin;
            if (candidate) candidate_seconds += elapsed;
            else baseline_seconds += elapsed;
        }
    }
    if (!write_results(argv[5], pairs, count, runs, baseline_seconds,
                       candidate_seconds)) {
        fprintf(stderr, "could not write results\n");
        free_pairs(pairs, count);
        return 8;
    }
    uint64_t baseline_bytes = 0;
    uint64_t candidate_bytes = 0;
    double candidate_encode_seconds = 0.0;
    for (size_t i = 0; i < count; ++i) {
        baseline_bytes += pairs[i].baseline_size;
        candidate_bytes += pairs[i].candidate_size;
        candidate_encode_seconds += pairs[i].candidate_encode_seconds;
    }
    printf("streams=%zu runs=%ld exact=%zu baseline-bytes=%" PRIu64
           " candidate-bytes=%" PRIu64 " size-delta-percent=%.6f "
           "candidate-encode=%.9f baseline=%.9f candidate=%.9f "
           "decode-delta-percent=%.6f "
           "check=%" PRIu64 "\n",
           count, runs, count, baseline_bytes, candidate_bytes,
           100.0 * ((double)candidate_bytes / (double)baseline_bytes - 1.0),
           candidate_encode_seconds,
           baseline_seconds, candidate_seconds,
           100.0 * (candidate_seconds / baseline_seconds - 1.0), checksum);
    free_pairs(pairs, count);
    return 0;
}
