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
#endif

/* The native replay plumbing is shared so the topology experiment does not
   carry a second copy of file/container validation. */
#ifdef QLIC_GRADIENT_TOPOLOGY_REPLAY
#define QLIC_REPLAY_PREFIX "gradient-topology"
#define QLIC_REPLAY_MODE 53
#else
#define QLIC_REPLAY_PREFIX "context-cache"
#define QLIC_REPLAY_MODE 52
#endif

static int open_file(FILE **file, const char *path, const char *mode) {
#ifdef _WIN32
    return fopen_s(file, path, mode);
#else
    *file = fopen(path, mode);
    return *file ? 0 : 1;
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

static int read_file(const char *path, uint8_t **data_out, size_t *size_out) {
    FILE *file = NULL;
    long length = 0;
    uint8_t *data = NULL;
    *data_out = NULL;
    *size_out = 0;
    if (open_file(&file, path, "rb") || !file) return 0;
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
        memcmp(file, "QLIC", 4) != 0 || file[12] != 9 || file[13] != 0 ||
        file[14] != 0 || file[15] != 0x80u ||
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

static int replay_file(const char *path, size_t index) {
    uint8_t *stream = NULL;
    uint8_t *pixels = NULL;
    size_t stream_size = 0;
    StreamInfo info;
    uint32_t width = 0;
    uint32_t height = 0;
    int channels = 0;
    if (!read_native_payload(path, &stream, &stream_size)) {
        fprintf(stderr,
                QLIC_REPLAY_PREFIX
                "-file-end index=%zu status=read-error path=\"%s\"\n",
                index, path);
        return 0;
    }
    int error = stream_get_info(stream, stream_size, &info);
    if (error != STREAM_OK || info.mode != QLIC_REPLAY_MODE) {
        fprintf(stderr,
                QLIC_REPLAY_PREFIX
                "-file-end index=%zu status=wrong-stream "
                "error=%d mode=%d sample-bits=%d path=\"%s\"\n",
                index, error, error == STREAM_OK ? info.mode : -1,
                error == STREAM_OK ? info.sample_bits : -1, path);
        free(stream);
        return 0;
    }
    fprintf(stderr,
            QLIC_REPLAY_PREFIX
            "-file-begin index=%zu bytes=%zu width=%" PRIu32
            " height=%" PRIu32 " channels=%d path=\"%s\"\n",
            index, stream_size, info.width, info.height, info.channels, path);
    error = stream_decode_trusted_expected(
        stream, stream_size, info.width, info.height, info.channels, &pixels,
        &width, &height, &channels);
    int okay = error == STREAM_OK && width == info.width &&
               height == info.height && channels == info.channels;
    fprintf(stderr,
            QLIC_REPLAY_PREFIX
            "-file-end index=%zu status=%s error=%d path=\"%s\"\n",
            index, okay ? "ok" : "decode-error", error, path);
    stream_free(pixels);
    free(stream);
    return okay;
}

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s PROCESSOR MANIFEST [TRACE]\n", argv[0]);
        return 2;
    }
    char *end = NULL;
    errno = 0;
    unsigned long processor_value = strtoul(argv[1], &end, 10);
    if (errno || !end || *end || processor_value > UINT_MAX ||
        !pin_processor((unsigned)processor_value)) {
        fprintf(stderr, "could not pin processor: %s\n", argv[1]);
        return 2;
    }
    if (argc == 4) {
#ifdef _WIN32
        FILE *redirected = NULL;
        if (freopen_s(&redirected, argv[3], "w", stderr) || !redirected)
            return 2;
#else
        if (!freopen(argv[3], "w", stderr)) return 2;
#endif
    }
    FILE *manifest = NULL;
    if (open_file(&manifest, argv[2], "r") || !manifest) {
        fprintf(stderr, "could not open manifest: %s\n", argv[2]);
        return 2;
    }
    stream_set_threads(1);
    char line[32768];
    size_t index = 0;
    size_t passed = 0;
    while (fgets(line, (int)sizeof(line), manifest)) {
        size_t length = strlen(line);
        while (length &&
               (line[length - 1u] == '\n' || line[length - 1u] == '\r'))
            line[--length] = '\0';
        if (!length) continue;
        ++index;
        passed += (size_t)replay_file(line, index);
    }
    int failed = ferror(manifest) || fclose(manifest) || passed != index;
    fprintf(stderr,
            QLIC_REPLAY_PREFIX
            "-replay files=%zu passed=%zu failed=%zu\n",
            index, passed, index - passed);
    if (argc == 4 && (fflush(stderr) || ferror(stderr))) failed = 1;
    return failed ? 1 : 0;
}
