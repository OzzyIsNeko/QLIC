#ifndef QLIC_STATIC
#define QLIC_STATIC
#endif
#include <qlic/qlic.h>

#include <stdio.h>
#include <stdlib.h>

static unsigned char *read_file(const char *path, uint64_t limit,
                                size_t *size) {
    FILE *f = NULL;
#ifdef _WIN32
    if (fopen_s(&f, path, "rb") != 0) return NULL;
#else
    f = fopen(path, "rb");
#endif
    if (!f) return NULL;
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    __int64 n = _ftelli64(f);
    if (n < 0 || (uint64_t)n > limit || (uint64_t)n > SIZE_MAX) {
        fclose(f);
        return NULL;
    }
    if (_fseeki64(f, 0, SEEK_SET) != 0) {
#else
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0 || (uint64_t)n > limit || (uint64_t)n > SIZE_MAX) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
#endif
        fclose(f);
        return NULL;
    }
    unsigned char *p = (unsigned char *)malloc((size_t)n ? (size_t)n : 1u);
    if (!p) {
        fclose(f);
        return NULL;
    }
    if ((size_t)n && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return p;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: decode image.qlic\n");
        return 2;
    }
    qlic_decode_limits limits;
    qlic_decode_limits_default(&limits);
    size_t n = 0;
    unsigned char *data = read_file(argv[1], limits.max_file_bytes, &n);
    if (!data) {
        fprintf(stderr, "could not read input\n");
        return 1;
    }
    qlic_info info = {0};
    if (qlic_get_info(data, n, &limits, &info) == QLIC_OK) {
        printf("%ux%u stored frames=%u animated=%u\n",
               info.width, info.height, info.frame_count, info.animated);
    }
    qlic_animation anim = {0};
    int rc = qlic_decode_animation(data, n, &limits, &anim);
    free(data);
    if (rc != QLIC_OK) {
        fprintf(stderr, "decode failed: %s\n", qlic_last_error());
        return 1;
    }
    printf("%ux%u frames=%u\n", anim.width, anim.height, anim.frame_count);
    for (uint32_t i = 0; i < anim.frame_count; ++i) {
        printf("frame %u: %ux%u delay=%u ms\n",
               i, anim.frames[i].image.width, anim.frames[i].image.height,
               anim.frames[i].delay_ms);
    }
    qlic_animation_free(&anim);
    return 0;
}
