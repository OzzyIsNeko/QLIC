#include <qlic/qlic.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  HEADER_SIZE = 28,
  FOOTER_SIZE = 4,
  MODE_NATIVE_WIRE = 9,
  MODE_TILES_WIRE = 14,
  CODEC_STORE_WITH_CRC = 0x80
};

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
         ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static void wr32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8u);
  p[2] = (uint8_t)(value >> 16u);
  p[3] = (uint8_t)(value >> 24u);
}

static void wr64(uint8_t *p, uint64_t value) {
  wr32(p, (uint32_t)value);
  wr32(p + 4, (uint32_t)(value >> 32u));
}

static uint32_t crc32(const uint8_t *data, size_t size) {
  uint32_t crc = UINT32_C(0xffffffff);
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8u; ++bit)
      crc = (crc & 1u) ? UINT32_C(0xedb88320) ^ (crc >> 1u) : crc >> 1u;
  }
  return crc ^ UINT32_C(0xffffffff);
}

static int read_fixture(const char *path, uint8_t **data, size_t *size) {
  FILE *file = NULL;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") || !file)
    return 0;
#else
  file = fopen(path, "rb");
  if (!file)
    return 0;
#endif
  int ok = fseek(file, 0, SEEK_END) == 0;
  long end = ok ? ftell(file) : -1;
  if (end <= 0 || fseek(file, 0, SEEK_SET) != 0)
    ok = 0;
  uint8_t *bytes = ok ? (uint8_t *)malloc((size_t)end) : NULL;
  if (!bytes)
    ok = 0;
  if (ok && fread(bytes, 1, (size_t)end, file) != (size_t)end)
    ok = 0;
  if (fclose(file) != 0)
    ok = 0;
  if (!ok) {
    free(bytes);
    return 0;
  }
  *data = bytes;
  *size = (size_t)end;
  return 1;
}

static int read_native(uint8_t **data, size_t *size) {
  char path[1024];
  int length = snprintf(path, sizeof(path), "%s/native.qlic", QLIC_FIXTURE_DIR);
  return length >= 0 && (size_t)length < sizeof(path) &&
         read_fixture(path, data, size);
}

static uint8_t *make_tiles(const uint8_t *native, size_t native_size,
                           uint32_t count, size_t *file_size) {
  if (!native || !file_size || count == 0 || count > 65536u ||
      native_size <= 32u ||
      memcmp(native, "QLIC", 4u) != 0 ||
      native[12] != MODE_NATIVE_WIRE)
    return NULL;
  uint32_t width = rd32(native + 4);
  uint32_t tile_height = rd32(native + 8);
  unsigned channels = native[HEADER_SIZE + 12u];
  uint64_t height = (uint64_t)tile_height * count;
  size_t chunk_size = native_size - HEADER_SIZE - FOOTER_SIZE;
  if (!width || !tile_height ||
      (channels != 1u && channels != 3u && channels != 4u) ||
      height > UINT32_MAX || chunk_size > UINT32_MAX)
    return NULL;
  size_t table_size = 4u + (size_t)count * 4u;
  if (chunk_size && (size_t)count > (SIZE_MAX - table_size) / chunk_size)
    return NULL;
  size_t payload_size = table_size + (size_t)count * chunk_size;
  if (payload_size > SIZE_MAX - HEADER_SIZE - FOOTER_SIZE)
    return NULL;
  size_t total = HEADER_SIZE + payload_size + FOOTER_SIZE;
  uint8_t *file = (uint8_t *)calloc(total, 1u);
  if (!file)
    return NULL;
  memcpy(file, "QLIC", 4u);
  wr32(file + 4, width);
  wr32(file + 8, (uint32_t)height);
  file[12] = MODE_TILES_WIRE;
  file[14] = (uint8_t)channels;
  file[15] = CODEC_STORE_WITH_CRC;
  wr32(file + 16, tile_height);
  wr64(file + 20, (uint64_t)payload_size);
  uint8_t *payload = file + HEADER_SIZE;
  wr32(payload, count);
  size_t offset = table_size;
  for (uint32_t i = 0; i < count; ++i) {
    wr32(payload + 4u + (size_t)i * 4u, (uint32_t)chunk_size);
    memcpy(payload + offset, native + HEADER_SIZE, chunk_size);
    offset += chunk_size;
  }
  wr32(file + total - FOOTER_SIZE, crc32(file, total - FOOTER_SIZE));
  *file_size = total;
  return file;
}

static uint32_t random32(uint32_t *state) {
  uint32_t value = *state;
  value ^= value << 13u;
  value ^= value >> 17u;
  value ^= value << 5u;
  *state = value;
  return value;
}

static void init_limits(qlic_decode_limits *limits, uint32_t threads) {
  qlic_decode_limits_default(limits);
  limits->threads = threads;
  limits->max_file_bytes = UINT64_C(16) * 1024u * 1024u;
  limits->max_payload_bytes = UINT64_C(16) * 1024u * 1024u;
  limits->max_pixels = UINT64_C(4) * 1024u * 1024u;
  limits->max_animation_bytes = UINT64_C(16) * 1024u * 1024u;
  limits->max_frames = 4u;
}

static int exercise_rgba_mode45(uint32_t *seed) {
  uint8_t *native = NULL;
  size_t native_size = 0;
  qlic_image source = {0};
  if (!read_fixture(QLIC_RGBA_MODE45_FIXTURE, &native, &native_size) ||
      native_size <= HEADER_SIZE + 15u + FOOTER_SIZE ||
      native[12] != MODE_NATIVE_WIRE || native[HEADER_SIZE + 12u] != 4u ||
      native[HEADER_SIZE + 14u] != 45u ||
      qlic_decode_rgba(native, native_size, NULL, &source) != QLIC_OK) {
    fprintf(stderr, "could not decode RGBA mode45 source fixture: %s\n",
            qlic_last_error());
    free(native);
    return 0;
  }
  size_t source_bytes = source.stride * source.height;
  size_t tiled_size = 0;
  uint8_t *tiled = make_tiles(native, native_size, 2u, &tiled_size);
  uint8_t *mutated = tiled ? (uint8_t *)malloc(tiled_size) : NULL;
  qlic_decode_limits limits;
  init_limits(&limits, 4u);
  qlic_image image = {0};
  int ok = tiled && mutated &&
           qlic_decode_rgba(tiled, tiled_size, &limits, &image) == QLIC_OK &&
           image.width == source.width && image.height == source.height * 2u &&
           image.stride == source.stride &&
           memcmp(image.rgba, source.rgba, source_bytes) == 0 &&
           memcmp(image.rgba + source_bytes, source.rgba, source_bytes) == 0;
  qlic_image_free(&image);
  if (!ok) {
    fprintf(stderr, "valid RGBA mode45 MODE_TILES stream failed: %s\n",
            qlic_last_error());
  }

  /* Compose repaired-checksum outer mutations with the real four-channel
     mode45 nested decoder and RGBA copy path.  Keep this fixed and bounded so
     the ordinary CTest remains quick; the general loop below supplies the
     high mutation count. */
  for (unsigned pass = 0; pass < 64u && ok; ++pass) {
    memcpy(mutated, tiled, tiled_size);
    unsigned changes = 1u + random32(seed) % 6u;
    for (unsigned change = 0; change < changes; ++change) {
      size_t position =
          4u + (size_t)random32(seed) % (tiled_size - 8u);
      mutated[position] ^= (uint8_t)(1u << (random32(seed) & 7u));
    }
    mutated[12] = MODE_TILES_WIRE;
    mutated[13] = 0u;
    mutated[14] = 4u;
    mutated[15] = CODEC_STORE_WITH_CRC;
    wr32(mutated + tiled_size - FOOTER_SIZE,
         crc32(mutated, tiled_size - FOOTER_SIZE));
    memset(&image, 0, sizeof(image));
    (void)qlic_decode_rgba(mutated, tiled_size, &limits, &image);
    qlic_image_free(&image);
    size_t truncated = (size_t)random32(seed) % tiled_size;
    memset(&image, 0, sizeof(image));
    (void)qlic_decode_rgba(mutated, truncated, &limits, &image);
    qlic_image_free(&image);
  }

  free(mutated);
  free(tiled);
  qlic_image_free(&source);
  free(native);
  return ok;
}

int main(int argc, char **argv) {
  unsigned fuzz_cases = 20000u;
  uint32_t seed = UINT32_C(0x14badd55);
  if (argc > 1) {
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(argv[1], &end, 10);
    if (errno || !end || *end || parsed > 10000000ul)
      return 2;
    fuzz_cases = (unsigned)parsed;
  }
  if (argc > 2) {
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(argv[2], &end, 0);
    if (errno || !end || *end || parsed > UINT32_MAX)
      return 2;
    seed = (uint32_t)parsed;
  }
  if (argc > 3)
    return 2;
  uint32_t initial_seed = seed;

  if (!exercise_rgba_mode45(&seed))
    return 3;

  uint8_t *native = NULL;
  size_t native_size = 0;
  qlic_image source = {0};
  if (!read_native(&native, &native_size) ||
      qlic_decode_rgba(native, native_size, NULL, &source) != QLIC_OK) {
    fprintf(stderr, "could not decode native source fixture: %s\n",
            qlic_last_error());
    free(native);
    return 4;
  }
  size_t source_bytes = source.stride * source.height;

  /* Exact valid streams exercise parallel batches without relying only on
     malformed inputs that fail before task dispatch. */
  for (unsigned pass = 0; pass < 256u; ++pass) {
    uint32_t count = 1u + random32(&seed) % 32u;
    size_t size = 0;
    uint8_t *file = make_tiles(native, native_size, count, &size);
    qlic_decode_limits limits;
    init_limits(&limits, 1u + random32(&seed) % 8u);
    qlic_image image = {0};
    int ok = file && qlic_decode_rgba(file, size, &limits, &image) == QLIC_OK &&
             image.width == source.width &&
             image.height == source.height * count &&
             image.stride == source.stride;
    for (uint32_t i = 0; i < count && ok; ++i)
      ok = memcmp(image.rgba + (size_t)i * source_bytes, source.rgba,
                  source_bytes) == 0;
    qlic_image_free(&image);
    free(file);
    if (!ok) {
      fprintf(stderr, "valid MODE_TILES stream failed at pass %u: %s\n", pass,
              qlic_last_error());
      qlic_image_free(&source);
      free(native);
      return 5;
    }
  }

  size_t base_size = 0;
  uint8_t *base = make_tiles(native, native_size, 4u, &base_size);
  uint8_t *mutated = base ? (uint8_t *)malloc(base_size) : NULL;
  if (!base || !mutated) {
    qlic_image_free(&source);
    free(native);
    free(base);
    free(mutated);
    return 6;
  }
  for (unsigned pass = 0; pass < fuzz_cases; ++pass) {
    memcpy(mutated, base, base_size);
    unsigned changes = 1u + random32(&seed) % 8u;
    for (unsigned change = 0; change < changes; ++change) {
      size_t position = 4u + (size_t)random32(&seed) % (base_size - 8u);
      mutated[position] ^= (uint8_t)(1u << (random32(&seed) & 7u));
    }
    mutated[12] = MODE_TILES_WIRE;
    mutated[13] = 0u;
    mutated[14] = 1u;
    mutated[15] = CODEC_STORE_WITH_CRC;
    wr32(mutated + base_size - FOOTER_SIZE,
         crc32(mutated, base_size - FOOTER_SIZE));
    qlic_decode_limits limits;
    init_limits(&limits, 1u + random32(&seed) % 8u);
    qlic_image image = {0};
    (void)qlic_decode_rgba(mutated, base_size, &limits, &image);
    qlic_image_free(&image);

    size_t truncated = (size_t)random32(&seed) % base_size;
    memset(&image, 0, sizeof(image));
    (void)qlic_decode_rgba(mutated, truncated, &limits, &image);
    qlic_image_free(&image);
  }

  size_t excessive_size = 0;
  uint8_t *excessive = make_tiles(native, native_size, 257u, &excessive_size);
  qlic_image excessive_image = {0};
  int status = excessive
                   ? qlic_decode_rgba(excessive, excessive_size, NULL,
                                      &excessive_image)
                   : QLIC_ERROR;
  qlic_image_free(&excessive_image);
  if (status != QLIC_LIMIT_EXCEEDED) {
    fprintf(stderr, "257-chunk MODE_TILES stream was not limit-rejected\n");
    free(excessive);
    free(mutated);
    free(base);
    qlic_image_free(&source);
    free(native);
    return 7;
  }

  free(excessive);
  free(mutated);
  free(base);
  qlic_image_free(&source);
  free(native);
  printf("MODE_TILES fuzz passed: valid=256 cases=%u mutations=%u seed=%u "
         "final-state=%u\n",
         fuzz_cases, fuzz_cases * 2u, initial_seed, seed);
  return 0;
}
