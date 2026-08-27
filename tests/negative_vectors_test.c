#include <qlic/qlic.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  OUTER_HEADER = 28,
  OUTER_FOOTER = 4,
  QST_HEADER = 30,
  QST_OFFSET = OUTER_HEADER
};

typedef enum {
  MUTATE_BYTE,
  MUTATE_U32,
  MUTATE_U64,
  TRUNCATE_END,
  APPEND_BODY
} MutationKind;

typedef struct {
  const char *name;
  MutationKind kind;
  size_t offset;
  uint64_t value;
  int repair_qst_crc;
  int repair_outer_crc;
} NegativeVector;

static const NegativeVector vectors[] = {
    {"truncated-footer", TRUNCATE_END, 0, 1, 0, 0},
    {"outer-crc-mismatch", MUTATE_BYTE, OUTER_FOOTER, 0x80, 0, 0},
    {"missing-crc-flag", MUTATE_BYTE, 15, 0x00, 0, 0},
    {"reserved-codec-bit", MUTATE_BYTE, 15, 0x84, 0, 1},
    {"zero-width", MUTATE_U32, 4, 0, 0, 1},
    {"invalid-outer-mode", MUTATE_BYTE, 12, 0xff, 0, 1},
    {"illegal-native-transform", MUTATE_BYTE, 13, 1, 0, 1},
    {"stored-payload-size-mismatch", MUTATE_U64, 20, 42, 0, 1},
    {"qst-magic", MUTATE_BYTE, QST_OFFSET, 'X', 0, 1},
    {"qst-invalid-mode", MUTATE_BYTE, QST_OFFSET + 14, 55, 1, 1},
    {"qst-invalid-tile-log", MUTATE_BYTE, QST_OFFSET + 16, 8, 1, 1},
    {"qst-payload-size-mismatch", MUTATE_U32, QST_OFFSET + 22, 12, 1, 1},
    {"qst-container-crc", MUTATE_BYTE, QST_OFFSET + 26, 0x01, 0, 1},
    {"qst-pixel-crc", MUTATE_BYTE, QST_OFFSET + 18, 0x01, 1, 1},
    {"trailing-stored-byte", APPEND_BODY, 0, 0, 0, 1},
};

static uint32_t crc32(const uint8_t *data, size_t size) {
  uint32_t crc = UINT32_C(0xffffffff);
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8u; ++bit)
      crc = (crc & 1u) ? UINT32_C(0xedb88320) ^ (crc >> 1u) : crc >> 1u;
  }
  return crc ^ UINT32_C(0xffffffff);
}

static void write32le(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8u);
  p[2] = (uint8_t)(value >> 16u);
  p[3] = (uint8_t)(value >> 24u);
}

static void write64le(uint8_t *p, uint64_t value) {
  write32le(p, (uint32_t)value);
  write32le(p + 4, (uint32_t)(value >> 32u));
}

static int read_file(const char *path, uint8_t **data, size_t *size) {
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

static int manifest_contains(const char *manifest, size_t manifest_size,
                             const char *name) {
  size_t name_size = strlen(name);
  for (size_t i = 0; i + name_size <= manifest_size; ++i)
    if (memcmp(manifest + i, name, name_size) == 0)
      return 1;
  return 0;
}

static int make_vector(const uint8_t *source, size_t source_size,
                       const NegativeVector *vector, uint8_t **output,
                       size_t *output_size) {
  size_t size = source_size;
  if (vector->kind == TRUNCATE_END) {
    if (vector->value >= size)
      return 0;
    size -= (size_t)vector->value;
  } else if (vector->kind == APPEND_BODY) {
    if (size == SIZE_MAX)
      return 0;
    size += 1u;
  }
  uint8_t *data = (uint8_t *)malloc(size);
  if (!data)
    return 0;
  if (vector->kind == APPEND_BODY) {
    memcpy(data, source, source_size - OUTER_FOOTER);
    data[source_size - OUTER_FOOTER] = 0xa5;
    memcpy(data + source_size - OUTER_FOOTER + 1u,
           source + source_size - OUTER_FOOTER, OUTER_FOOTER);
  } else {
    memcpy(data, source, size);
  }

  if (vector->kind == MUTATE_BYTE) {
    size_t offset = vector->offset;
    if (strcmp(vector->name, "outer-crc-mismatch") == 0)
      offset = size - OUTER_FOOTER;
    if (offset >= size) {
      free(data);
      return 0;
    }
    if (strcmp(vector->name, "missing-crc-flag") == 0)
      data[offset] &= UINT8_C(0x7f);
    else if (strcmp(vector->name, "outer-crc-mismatch") == 0 ||
             strcmp(vector->name, "qst-container-crc") == 0 ||
             strcmp(vector->name, "qst-pixel-crc") == 0)
      data[offset] ^= (uint8_t)vector->value;
    else
      data[offset] = (uint8_t)vector->value;
  } else if (vector->kind == MUTATE_U32) {
    if (vector->offset > size || size - vector->offset < 4u) {
      free(data);
      return 0;
    }
    write32le(data + vector->offset, (uint32_t)vector->value);
  } else if (vector->kind == MUTATE_U64) {
    if (vector->offset > size || size - vector->offset < 8u) {
      free(data);
      return 0;
    }
    write64le(data + vector->offset, vector->value);
  }

  if (vector->repair_qst_crc) {
    if (size < QST_OFFSET + QST_HEADER + OUTER_FOOTER) {
      free(data);
      return 0;
    }
    size_t qst_size = size - QST_OFFSET - OUTER_FOOTER;
    write32le(data + QST_OFFSET + 26u, 0);
    write32le(data + QST_OFFSET + 26u,
              crc32(data + QST_OFFSET, qst_size));
  }
  if (vector->repair_outer_crc) {
    if (size < OUTER_FOOTER) {
      free(data);
      return 0;
    }
    write32le(data + size - OUTER_FOOTER,
              crc32(data, size - OUTER_FOOTER));
  }
  *output = data;
  *output_size = size;
  return 1;
}

int main(void) {
  char source_path[1024];
  char manifest_path[1024];
  int source_length = snprintf(source_path, sizeof(source_path), "%s/native.qlic",
                               QLIC_FIXTURE_DIR);
  int manifest_length = snprintf(manifest_path, sizeof(manifest_path),
                                 "%s/negative-manifest.json", QLIC_FIXTURE_DIR);
  if (source_length < 0 || (size_t)source_length >= sizeof(source_path) ||
      manifest_length < 0 || (size_t)manifest_length >= sizeof(manifest_path)) {
    fprintf(stderr, "fixture path is too long\n");
    return 1;
  }
  uint8_t *source = NULL;
  size_t source_size = 0;
  uint8_t *manifest_bytes = NULL;
  size_t manifest_size = 0;
  if (!read_file(source_path, &source, &source_size) ||
      !read_file(manifest_path, &manifest_bytes, &manifest_size)) {
    fprintf(stderr, "could not read negative-vector inputs\n");
    free(source);
    free(manifest_bytes);
    return 1;
  }
  if (qlic_validate(source, source_size, NULL) != QLIC_OK) {
    fprintf(stderr, "negative-vector source is not valid: %s\n",
            qlic_last_error());
    free(source);
    free(manifest_bytes);
    return 1;
  }

  size_t count = sizeof(vectors) / sizeof(vectors[0]);
  for (size_t i = 0; i < count; ++i) {
    const NegativeVector *vector = &vectors[i];
    if (!manifest_contains((const char *)manifest_bytes, manifest_size,
                           vector->name)) {
      fprintf(stderr, "negative manifest is missing %s\n", vector->name);
      free(source);
      free(manifest_bytes);
      return 1;
    }
    uint8_t *mutated = NULL;
    size_t mutated_size = 0;
    if (!make_vector(source, source_size, vector, &mutated, &mutated_size)) {
      fprintf(stderr, "could not construct negative vector %s\n", vector->name);
      free(source);
      free(manifest_bytes);
      return 1;
    }
    int status = qlic_validate(mutated, mutated_size, NULL);
    if (status != QLIC_BAD_DATA) {
      fprintf(stderr, "%s returned %d, expected QLIC_BAD_DATA: %s\n",
              vector->name, status, qlic_last_error());
      free(mutated);
      free(source);
      free(manifest_bytes);
      return 1;
    }
    free(mutated);
  }
  free(source);
  free(manifest_bytes);
  printf("QLIC frozen negative vectors passed: %zu\n", count);
  return 0;
}
