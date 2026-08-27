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
  if (fopen_s(&f, path, "rb") != 0)
    return NULL;
#else
  f = fopen(path, "rb");
#endif
  if (!f)
    return NULL;
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

static int multiply_size(size_t left, size_t right, size_t *result) {
  if (left != 0u && right > SIZE_MAX / left)
    return 0;
  *result = left * right;
  return 1;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: decode image.qlic\n");
    return 2;
  }
  qlic_decode_limits limits;
  qlic_decode_limits_v2 limits_v2;
  qlic_decode_limits_default(&limits);
  qlic_decode_limits_v2_default(&limits_v2);
  size_t n = 0;
  unsigned char *data = read_file(argv[1], limits_v2.max_file_bytes, &n);
  if (!data) {
    fprintf(stderr, "could not read input\n");
    return 1;
  }
  qlic_info_v2 info = {0};
  info.struct_size = sizeof(info);
  int rc = qlic_get_info_v2(data, n, &limits_v2, &info);
  if (rc != QLIC_OK) {
    fprintf(stderr, "info failed: %s\n", qlic_last_error());
    free(data);
    return 1;
  }
  printf("%ux%u stored frames=%u animated=%u channels=%u bits=%u\n", info.width,
         info.height, info.frame_count, info.animated, info.channels,
         info.bits_per_sample);

  if (!info.animated && info.bits_per_sample == 8u) {
    size_t stride = 0;
    size_t pixel_bytes = 0;
    if (!multiply_size((size_t)info.width, 4u, &stride) ||
        !multiply_size(stride, (size_t)info.height, &pixel_bytes)) {
      fprintf(stderr, "decoded image is too large\n");
      free(data);
      return 1;
    }
    unsigned char *pixels =
        (unsigned char *)malloc(pixel_bytes ? pixel_bytes : 1u);
    if (!pixels) {
      fprintf(stderr, "could not allocate output\n");
      free(data);
      return 1;
    }
    qlic_pixel_buffer output = {0};
    output.struct_size = sizeof(output);
    output.format = QLIC_PIXELS_RGBA8;
    output.pixels = pixels;
    output.pixels_size = pixel_bytes;
    output.stride = stride;
    rc = qlic_decode_pixels(data, n, &limits_v2, &output);
    free(data);
    if (rc != QLIC_OK) {
      fprintf(stderr, "decode failed: %s\n", qlic_last_error());
      free(pixels);
      return 1;
    }
    printf("decoded into caller memory: %ux%u, %zu bytes\n", output.width,
           output.height, pixel_bytes);
    free(pixels);
    return 0;
  }

  if (!info.animated) {
    fprintf(stderr,
            "this small example uses RGBA8 output; use qlic_decode_wide "
            "or qlic_decode_hdr for %u-bit samples\n",
            info.bits_per_sample);
    free(data);
    return 2;
  }
  qlic_animation anim = {0};
  rc = qlic_decode_animation(data, n, &limits, &anim);
  free(data);
  if (rc != QLIC_OK) {
    fprintf(stderr, "decode failed: %s\n", qlic_last_error());
    return 1;
  }
  printf("%ux%u frames=%u\n", anim.width, anim.height, anim.frame_count);
  for (uint32_t i = 0; i < anim.frame_count; ++i) {
    printf("frame %u: %ux%u delay=%u ms\n", i, anim.frames[i].image.width,
           anim.frames[i].image.height, anim.frames[i].delay_ms);
  }
  qlic_animation_free(&anim);
  return 0;
}
