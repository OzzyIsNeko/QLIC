#include "input.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <jxl/decode.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEBP_DECODER_ABI_VERSION 0x0210

typedef struct {
  int width;
  int height;
  int has_alpha;
  int has_animation;
  int format;
  uint32_t pad[5];
} WebPBitstreamFeatures;

typedef int(__cdecl *WebPGetFeaturesFn)(const uint8_t *, size_t,
                                        WebPBitstreamFeatures *, int);
typedef uint8_t *(__cdecl *WebPDecodeRGBAIntoFn)(
    const uint8_t *, size_t, uint8_t *, size_t, int);

typedef JxlDecoder *(__cdecl *JxlCreateFn)(const JxlMemoryManager *);
typedef void(__cdecl *JxlDestroyFn)(JxlDecoder *);
typedef JxlDecoderStatus(__cdecl *JxlSubscribeFn)(JxlDecoder *, int);
typedef JxlDecoderStatus(__cdecl *JxlSetInputFn)(JxlDecoder *, const uint8_t *,
                                                 size_t);
typedef void(__cdecl *JxlCloseInputFn)(JxlDecoder *);
typedef JxlDecoderStatus(__cdecl *JxlProcessFn)(JxlDecoder *);
typedef JxlDecoderStatus(__cdecl *JxlBasicInfoFn)(const JxlDecoder *,
                                                  JxlBasicInfo *);
typedef JxlDecoderStatus(__cdecl *JxlBufferSizeFn)(const JxlDecoder *,
                                                   const JxlPixelFormat *,
                                                   size_t *);
typedef JxlDecoderStatus(__cdecl *JxlSetBufferFn)(
    JxlDecoder *, const JxlPixelFormat *, void *, size_t);

typedef struct {
  HMODULE module;
  JxlCreateFn create;
  JxlDestroyFn destroy;
  JxlSubscribeFn subscribe;
  JxlSetInputFn set_input;
  JxlCloseInputFn close_input;
  JxlProcessFn process;
  JxlBasicInfoFn basic_info;
  JxlBufferSizeFn buffer_size;
  JxlSetBufferFn set_buffer;
} JxlApi;

static wchar_t runtime_directory[32768];

static void set_error(char *error, size_t capacity, const char *message) {
  if (!error || !capacity)
    return;
  strncpy_s(error, capacity, message, _TRUNCATE);
}

static uint16_t le16(const uint8_t *data) {
  return (uint16_t)(data[0] | (uint16_t)data[1] << 8);
}

static uint32_t le32(const uint8_t *data) {
  return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
         (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint16_t be16(const uint8_t *data) {
  return (uint16_t)((uint16_t)data[0] << 8 | data[1]);
}

static uint32_t be32(const uint8_t *data) {
  return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 |
         (uint32_t)data[2] << 8 | data[3];
}

static uint64_t be64(const uint8_t *data) {
  return (uint64_t)be32(data) << 32 | be32(data + 4);
}

static int read_file(const wchar_t *path, uint64_t limit, uint8_t **data,
                     size_t *size, char *error, size_t error_capacity) {
  FILE *file = NULL;
  if (_wfopen_s(&file, path, L"rb") || !file) {
    set_error(error, error_capacity, "could not open the input image");
    return 0;
  }
  if (_fseeki64(file, 0, SEEK_END)) {
    fclose(file);
    set_error(error, error_capacity, "could not measure the input image");
    return 0;
  }
  __int64 measured = _ftelli64(file);
  if (measured <= 0 || (uint64_t)measured > limit ||
      (uint64_t)measured > SIZE_MAX) {
    fclose(file);
    set_error(error, error_capacity, "the input image is too large");
    return 0;
  }
  if (_fseeki64(file, 0, SEEK_SET)) {
    fclose(file);
    set_error(error, error_capacity, "could not read the input image");
    return 0;
  }
  size_t bytes = (size_t)measured;
  uint8_t *buffer = (uint8_t *)malloc(bytes);
  if (!buffer) {
    fclose(file);
    set_error(error, error_capacity, "out of memory");
    return 0;
  }
  if (fread(buffer, 1, bytes, file) != bytes) {
    free(buffer);
    fclose(file);
    set_error(error, error_capacity, "could not read the input image");
    return 0;
  }
  fclose(file);
  *data = buffer;
  *size = bytes;
  return 1;
}

static int runtime_path(const wchar_t *name, wchar_t *path, size_t capacity) {
  if (runtime_directory[0]) {
    if (wcscpy_s(path, capacity, runtime_directory))
      return 0;
    size_t length = wcslen(path);
    if (length && path[length - 1u] != L'\\' && path[length - 1u] != L'/') {
      if (wcscat_s(path, capacity, L"\\"))
        return 0;
    }
  } else {
    DWORD length = GetModuleFileNameW(NULL, path, (DWORD)capacity);
    if (!length || length >= capacity)
      return 0;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash)
      return 0;
    slash[1] = 0;
    if (wcscat_s(path, capacity, L"image-codecs\\"))
      return 0;
  }
  return wcscat_s(path, capacity, name) == 0;
}

int qlic_input_set_runtime_directory(const wchar_t *directory) {
  if (!directory || !directory[0]) {
    runtime_directory[0] = 0;
    return 1;
  }
  return wcscpy_s(runtime_directory,
                  sizeof(runtime_directory) / sizeof(runtime_directory[0]),
                  directory) == 0;
}

static HMODULE load_runtime(const wchar_t *name) {
  wchar_t path[32768];
  if (!runtime_path(name, path, 32768))
    return NULL;
  return LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                         LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}

static int is_png(const uint8_t *data, size_t size) {
  static const uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  return size >= 8 && memcmp(data, signature, 8) == 0;
}

static uint32_t png_crc32(const uint8_t *data, size_t size) {
  static const uint32_t table[16] = {
      UINT32_C(0x00000000), UINT32_C(0x1db71064), UINT32_C(0x3b6e20c8),
      UINT32_C(0x26d930ac), UINT32_C(0x76dc4190), UINT32_C(0x6b6b51f4),
      UINT32_C(0x4db26158), UINT32_C(0x5005713c), UINT32_C(0xedb88320),
      UINT32_C(0xf00f9344), UINT32_C(0xd6d6a3e8), UINT32_C(0xcb61b38c),
      UINT32_C(0x9b64c2b0), UINT32_C(0x86d3d2d4), UINT32_C(0xa00ae278),
      UINT32_C(0xbdbdf21c)};
  uint32_t crc = UINT32_C(0xffffffff);
  for (size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    crc = table[crc & 15u] ^ (crc >> 4u);
    crc = table[crc & 15u] ^ (crc >> 4u);
  }
  return crc ^ UINT32_C(0xffffffff);
}

static int png_chunk_name_valid(const uint8_t *name) {
  for (size_t index = 0; index < 4u; ++index) {
    uint8_t c = name[index];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
      return 0;
  }
  return (name[2] & 32u) == 0;
}

static int check_png(const uint8_t *data, size_t size, char *error,
                     size_t error_capacity) {
  if (size < 33 || be32(data + 8) != 13 ||
      memcmp(data + 12, "IHDR", 4) != 0 || !be32(data + 16) ||
      !be32(data + 20)) {
    set_error(error, error_capacity, "the PNG header is invalid");
    return 0;
  }
  uint8_t depth = data[24];
  uint8_t color = data[25];
  int valid_depth =
      (color == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 ||
                      depth == 16)) ||
      (color == 2 && (depth == 8 || depth == 16)) ||
      (color == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8)) ||
      (color == 4 && (depth == 8 || depth == 16)) ||
      (color == 6 && (depth == 8 || depth == 16));
  if (!valid_depth) {
    set_error(error, error_capacity, "the PNG color format is unsupported");
    return 0;
  }
  if (data[26] != 0 || data[27] != 0 || data[28] > 1) {
    set_error(error, error_capacity, "the PNG coding method is unsupported");
    return 0;
  }

  size_t offset = 8u;
  int have_header = 0;
  int have_palette = 0;
  int have_pixels = 0;
  int pixels_ended = 0;
  int have_transparency = 0;
  uint32_t palette_entries = 0;
  while (offset < size) {
    if (size - offset < 12u) {
      set_error(error, error_capacity, "the PNG chunk layout is invalid");
      return 0;
    }
    uint32_t length = be32(data + offset);
    if (length > UINT32_C(0x7fffffff) ||
        (size_t)length > size - offset - 12u) {
      set_error(error, error_capacity, "the PNG chunk layout is invalid");
      return 0;
    }
    const uint8_t *name = data + offset + 4u;
    const uint8_t *chunk = name + 4u;
    if (!png_chunk_name_valid(name)) {
      set_error(error, error_capacity, "the PNG chunk name is invalid");
      return 0;
    }
    if (png_crc32(name, (size_t)length + 4u) != be32(chunk + length)) {
      set_error(error, error_capacity, "the PNG chunk checksum is invalid");
      return 0;
    }

    if (!memcmp(name, "IHDR", 4u)) {
      if (have_header || offset != 8u || length != 13u) {
        set_error(error, error_capacity, "the PNG chunk layout is invalid");
        return 0;
      }
      have_header = 1;
    } else if (!have_header) {
      set_error(error, error_capacity, "the PNG chunk layout is invalid");
      return 0;
    } else if (!memcmp(name, "PLTE", 4u)) {
      if (have_palette || have_pixels || !length || length > 768u ||
          length % 3u || color == 0u || color == 4u) {
        set_error(error, error_capacity, "the PNG palette is invalid");
        return 0;
      }
      palette_entries = length / 3u;
      if (color == 3u && palette_entries > (1u << depth)) {
        set_error(error, error_capacity, "the PNG palette is invalid");
        return 0;
      }
      have_palette = 1;
    } else if (!memcmp(name, "tRNS", 4u)) {
      int valid = !have_transparency && !have_pixels;
      valid = valid &&
              ((color == 0u && length == 2u) ||
               (color == 2u && length == 6u) ||
               (color == 3u && have_palette && length &&
                length <= palette_entries));
      if (!valid) {
        set_error(error, error_capacity,
                  "the PNG transparency data is invalid");
        return 0;
      }
      have_transparency = 1;
    } else if (!memcmp(name, "IDAT", 4u)) {
      if (pixels_ended || (color == 3u && !have_palette)) {
        set_error(error, error_capacity, "the PNG chunk layout is invalid");
        return 0;
      }
      have_pixels = 1;
    } else if (!memcmp(name, "IEND", 4u)) {
      if (length || !have_pixels || offset + 12u != size) {
        set_error(error, error_capacity, "the PNG ending is invalid");
        return 0;
      }
      return 1;
    } else {
      if ((name[0] & 32u) == 0) {
        set_error(error, error_capacity, "the PNG uses an unknown core chunk");
        return 0;
      }
      if (have_pixels)
        pixels_ended = 1;
    }
    if (have_pixels && memcmp(name, "IDAT", 4u) != 0)
      pixels_ended = 1;
    offset += (size_t)length + 12u;
  }
  set_error(error, error_capacity, "the PNG ending is missing");
  return 0;
}

static int png_has_transparency(const uint8_t *data, size_t size) {
  for (size_t offset = 8u; offset + 12u <= size;) {
    uint32_t length = be32(data + offset);
    if ((size_t)length > size - offset - 12u)
      return 0;
    if (!memcmp(data + offset + 4u, "tRNS", 4u))
      return 1;
    if (!memcmp(data + offset + 4u, "IEND", 4u))
      return 0;
    offset += (size_t)length + 12u;
  }
  return 0;
}

static int check_bmp(const uint8_t *data, size_t size, int *lossy,
                     char *error, size_t error_capacity) {
  if (size < 26) {
    set_error(error, error_capacity, "the BMP header is invalid");
    return 0;
  }
  uint32_t header = le32(data + 14);
  if (header == 12) {
    if (!le16(data + 24) || le16(data + 24) > 32) {
      set_error(error, error_capacity, "the BMP pixel format is unsupported");
      return 0;
    }
    return 1;
  }
  if (header < 40 || size < 54) {
    set_error(error, error_capacity, "the BMP header is unsupported");
    return 0;
  }
  uint16_t depth = le16(data + 28);
  uint32_t compression = le32(data + 30);
  int jpeg = compression == 4;
  if ((!depth && !jpeg) || depth > 32 ||
      (compression != 0 && compression != 1 && compression != 2 &&
       compression != 3 && compression != 4 && compression != 6)) {
    set_error(error, error_capacity, "the BMP pixel format is unsupported");
    return 0;
  }
  if (lossy)
    *lossy = jpeg;
  return 1;
}

static uint16_t tiff16(const uint8_t *data, int little) {
  return little ? le16(data) : be16(data);
}

static uint32_t tiff32(const uint8_t *data, int little) {
  if (little)
    return le32(data);
  return be32(data);
}

static uint64_t tiff64(const uint8_t *data, int little) {
  if (!little)
    return be64(data);
  return (uint64_t)le32(data) | (uint64_t)le32(data + 4) << 32;
}

static int tiff_range(uint64_t offset, uint64_t length, size_t size) {
  return offset <= size && length <= size - (size_t)offset;
}

static int check_tiff(const uint8_t *data, size_t size, int *lossy,
                      char *error, size_t error_capacity) {
  if (size < 8 ||
      !((data[0] == 'I' && data[1] == 'I') ||
        (data[0] == 'M' && data[1] == 'M'))) {
    set_error(error, error_capacity, "the TIFF header is invalid");
    return 0;
  }
  int little = data[0] == 'I';
  uint16_t version = tiff16(data + 2, little);
  int big = version == 43;
  if (version != 42 && !big) {
    set_error(error, error_capacity, "the TIFF version is unsupported");
    return 0;
  }
  if (big && (size < 16 || tiff16(data + 4, little) != 8 ||
              tiff16(data + 6, little) != 0)) {
    set_error(error, error_capacity, "the BigTIFF header is invalid");
    return 0;
  }
  uint64_t offset = big ? tiff64(data + 8, little) : tiff32(data + 4, little);
  for (uint32_t directory = 0; offset && directory < 4096; ++directory) {
    size_t count_bytes = big ? 8u : 2u;
    size_t entry_bytes = big ? 20u : 12u;
    size_t next_bytes = big ? 8u : 4u;
    if (!tiff_range(offset, count_bytes, size)) {
      set_error(error, error_capacity, "the TIFF directory is invalid");
      return 0;
    }
    uint64_t count =
        big ? tiff64(data + (size_t)offset, little)
            : tiff16(data + (size_t)offset, little);
    if (count > (size - (size_t)offset - count_bytes) / entry_bytes ||
        !tiff_range(offset + count_bytes, count * entry_bytes + next_bytes,
                    size)) {
      set_error(error, error_capacity, "the TIFF directory is invalid");
      return 0;
    }
    uint64_t compression = 1;
    const uint8_t *entries = data + (size_t)offset + count_bytes;
    for (uint64_t index = 0; index < count; ++index) {
      const uint8_t *entry = entries + (size_t)index * entry_bytes;
      uint16_t tag = tiff16(entry, little);
      uint16_t type = tiff16(entry + 2, little);
      uint64_t values =
          big ? tiff64(entry + 4, little) : tiff32(entry + 4, little);
      size_t value_field = big ? 12u : 8u;
      if (tag == 259 && type == 3 && values == 1)
        compression = tiff16(entry + value_field, little);
      if ((tag == 258 || tag == 339) && type == 3 && values) {
        if (values > UINT64_MAX / 2u) {
          set_error(error, error_capacity, "the TIFF values are invalid");
          return 0;
        }
        uint64_t bytes = values * 2u;
        const uint8_t *value = NULL;
        if (bytes <= (big ? 8u : 4u)) {
          value = entry + value_field;
        } else {
          uint64_t value_offset =
              big ? tiff64(entry + value_field, little)
                  : tiff32(entry + value_field, little);
          if (!tiff_range(value_offset, bytes, size)) {
            set_error(error, error_capacity, "the TIFF values are invalid");
            return 0;
          }
          value = data + (size_t)value_offset;
        }
        for (uint64_t value_index = 0; value_index < values; ++value_index) {
          uint16_t sample = tiff16(value + (size_t)value_index * 2u, little);
          if ((tag == 258 && sample > 16) ||
              (tag == 339 && sample != 1)) {
            set_error(
                error, error_capacity,
                tag == 258
                    ? "QLIC currently accepts TIFF images with up to 16 bits per channel"
                    : "floating point and signed TIFF images are unsupported");
            return 0;
          }
        }
      }
    }
    if (lossy && (compression == 6 || compression == 7))
      *lossy = 1;
    const uint8_t *next =
        entries + (size_t)count * entry_bytes;
    offset = big ? tiff64(next, little) : tiff32(next, little);
  }
  if (offset) {
    set_error(error, error_capacity, "the TIFF has too many directories");
    return 0;
  }
  return 1;
}

static int inspect_tiff_layout(const uint8_t *data, size_t size,
                               uint32_t *channels, uint32_t *bits,
                               uint32_t *alpha_mode) {
  if (size < 8u || !channels || !bits || !alpha_mode)
    return 0;
  int little = data[0] == 'I';
  int big = tiff16(data + 2u, little) == 43u;
  size_t count_bytes = big ? 8u : 2u;
  size_t entry_bytes = big ? 20u : 12u;
  size_t inline_bytes = big ? 8u : 4u;
  size_t value_field = big ? 12u : 8u;
  uint64_t offset = big ? tiff64(data + 8u, little)
                        : tiff32(data + 4u, little);
  if (!tiff_range(offset, count_bytes, size))
    return 0;
  uint64_t count = big ? tiff64(data + (size_t)offset, little)
                       : tiff16(data + (size_t)offset, little);
  uint64_t entries_offset = offset + count_bytes;
  if (count > SIZE_MAX / entry_bytes ||
      !tiff_range(entries_offset, count * entry_bytes, size))
    return 0;
  uint32_t samples = 1u;
  uint32_t depth = 1u;
  uint32_t association = 0u;
  for (uint64_t index = 0; index < count; ++index) {
    const uint8_t *entry =
        data + (size_t)entries_offset + (size_t)index * entry_bytes;
    uint16_t tag = tiff16(entry, little);
    uint16_t type = tiff16(entry + 2u, little);
    uint64_t values = big ? tiff64(entry + 4u, little)
                          : tiff32(entry + 4u, little);
    if (type != 3u || !values ||
        (tag != 258u && tag != 277u && tag != 338u))
      continue;
    uint64_t bytes64 = values * 2u;
    const uint8_t *value = entry + value_field;
    if (bytes64 > inline_bytes) {
      uint64_t value_offset = big ? tiff64(entry + value_field, little)
                                  : tiff32(entry + value_field, little);
      if (!tiff_range(value_offset, bytes64, size))
        return 0;
      value = data + (size_t)value_offset;
    }
    if (tag == 258u)
      depth = tiff16(value, little);
    else if (values == 1u) {
      if (tag == 277u) {
        samples = tiff16(value, little);
      } else {
        uint16_t extra = tiff16(value, little);
        association = extra == 1u ? 2u : extra == 2u ? 1u : 0u;
      }
    }
  }
  if (!depth || depth > 16u || !samples || samples > 4u)
    return 0;
  *bits = depth;
  *channels = samples == 2u ? 4u : samples;
  *alpha_mode = (*channels == 4u) ? (association ? association : 1u) : 0u;
  return *channels == 1u || *channels == 3u || *channels == 4u;
}

static int check_webp(const uint8_t *data, size_t size, int *lossy,
                      char *error, size_t error_capacity) {
  if (size < 20 || memcmp(data, "RIFF", 4) ||
      memcmp(data + 8, "WEBP", 4)) {
    set_error(error, error_capacity, "the WebP header is invalid");
    return 0;
  }
  int lossy_images = 0;
  int lossless_images = 0;
  for (size_t offset = 12; offset + 8 <= size;) {
    uint32_t chunk_size = le32(data + offset + 4);
    size_t payload = offset + 8u;
    if (chunk_size > size - payload) {
      set_error(error, error_capacity, "the WebP chunks are invalid");
      return 0;
    }
    if (!memcmp(data + offset, "VP8 ", 4))
      ++lossy_images;
    if (!memcmp(data + offset, "ANIM", 4) ||
        !memcmp(data + offset, "ANMF", 4)) {
      set_error(error, error_capacity,
                "animated WebP input is not supported yet");
      return 0;
    }
    if (!memcmp(data + offset, "VP8L", 4))
      ++lossless_images;
    size_t padded = (size_t)chunk_size + ((size_t)chunk_size & 1u);
    if (padded > size - payload)
      break;
    offset = payload + padded;
  }
  if (lossy_images + lossless_images != 1) {
    set_error(error, error_capacity, "the WebP image payload is unsupported");
    return 0;
  }
  if (lossy)
    *lossy = lossy_images != 0;
  return 1;
}

typedef struct {
  int avif_brand;
  int av1_config;
  int lossy_color;
  int bad_depth;
} AvifCheck;

static int fourcc(const uint8_t *type, const char *name) {
  return memcmp(type, name, 4) == 0;
}

static void inspect_avif_box(const uint8_t *type, const uint8_t *payload,
                             size_t size, AvifCheck *check) {
  if (fourcc(type, "ftyp") && size >= 8) {
    for (size_t offset = 0; offset + 4 <= size; offset += 4)
      if (fourcc(payload + offset, "avif") ||
          fourcc(payload + offset, "avis"))
        check->avif_brand = 1;
  } else if (fourcc(type, "av1C") && size >= 3) {
    ++check->av1_config;
    int high_depth = (payload[2] >> 6) & 1;
    int monochrome = (payload[2] >> 4) & 1;
    int subsample_x = (payload[2] >> 3) & 1;
    int subsample_y = (payload[2] >> 2) & 1;
    if (high_depth)
      check->bad_depth = 1;
    if (!monochrome && (subsample_x || subsample_y))
      check->lossy_color = 1;
  } else if (fourcc(type, "pixi") && size >= 5) {
    uint8_t channels = payload[4];
    if ((size_t)channels + 5u > size) {
      check->bad_depth = 1;
      return;
    }
    for (uint8_t channel = 0; channel < channels; ++channel)
      if (payload[5u + channel] > 8)
        check->bad_depth = 1;
  } else if (fourcc(type, "colr") && size >= 11 &&
             fourcc(payload, "nclx")) {
    uint16_t matrix = be16(payload + 8);
    int full_range = (payload[10] & 0x80u) != 0;
    if (matrix != 0 || !full_range)
      check->lossy_color = 1;
  }
}

static int avif_container(const uint8_t *type) {
  return fourcc(type, "meta") || fourcc(type, "iprp") ||
         fourcc(type, "ipco") || fourcc(type, "moov") ||
         fourcc(type, "trak") || fourcc(type, "mdia") ||
         fourcc(type, "minf") || fourcc(type, "stbl") ||
         fourcc(type, "stsd");
}

static int scan_avif_boxes(const uint8_t *data, size_t size, int depth,
                           AvifCheck *check) {
  if (depth > 12)
    return 0;
  for (size_t offset = 0; offset + 8 <= size;) {
    uint64_t box_size = be32(data + offset);
    size_t header = 8;
    if (box_size == 1) {
      if (size - offset < 16)
        return 0;
      box_size = be64(data + offset + 8);
      header = 16;
    } else if (box_size == 0) {
      box_size = size - offset;
    }
    if (box_size < header || box_size > size - offset)
      return 0;
    const uint8_t *type = data + offset + 4;
    const uint8_t *payload = data + offset + header;
    size_t payload_size = (size_t)box_size - header;
    inspect_avif_box(type, payload, payload_size, check);
    if (avif_container(type)) {
      size_t skip =
          fourcc(type, "meta") || fourcc(type, "stsd") ? 4u : 0u;
      if (payload_size < skip ||
          !scan_avif_boxes(payload + skip, payload_size - skip, depth + 1,
                           check))
        return 0;
    }
    offset += (size_t)box_size;
  }
  return 1;
}

static int looks_like_avif(const uint8_t *data, size_t size) {
  return size >= 16 && fourcc(data + 4, "ftyp") &&
         (fourcc(data + 8, "avif") || fourcc(data + 8, "avis") ||
          (size >= 20 &&
           (fourcc(data + 16, "avif") || fourcc(data + 16, "avis"))));
}

static int check_avif(const uint8_t *data, size_t size, int *lossy,
                      char *error, size_t error_capacity) {
  AvifCheck check = {0};
  if (!scan_avif_boxes(data, size, 0, &check) || !check.avif_brand ||
      !check.av1_config) {
    set_error(error, error_capacity, "the AVIF structure is unsupported");
    return 0;
  }
  if (check.bad_depth) {
    set_error(error, error_capacity,
              "only 8 bit AVIF is accepted");
    return 0;
  }
  if (lossy)
    *lossy = check.lossy_color;
  return 1;
}

#define QLIC_INPUT_MAX_METADATA_BYTES UINT64_C(16777216)

static int input_add_metadata(QlicInput *input, const uint8_t tag[4],
                              const uint8_t *data, size_t size, char *error,
                              size_t error_capacity) {
  uint64_t total = size;
  for (uint32_t index = 0; index < input->metadata_count; ++index)
    total += input->metadata[index].size;
  if (input->metadata_count >= QLIC_INPUT_MAX_METADATA ||
      total > QLIC_INPUT_MAX_METADATA_BYTES) {
    set_error(error, error_capacity,
              "the source metadata exceeds QLIC's safe metadata limits");
    return 0;
  }
  QlicInputMetadata *metadata = input->metadata + input->metadata_count++;
  memcpy(metadata->tag, tag, 4u);
  metadata->data = data;
  metadata->size = size;
  return 1;
}

static int png_xmp_packet(const uint8_t *data, size_t size,
                          const uint8_t **packet, size_t *packet_size) {
  static const char keyword[] = "XML:com.adobe.xmp";
  const uint8_t *keyword_end = (const uint8_t *)memchr(data, 0, size);
  if (!keyword_end || (size_t)(keyword_end - data) != sizeof(keyword) - 1u ||
      memcmp(data, keyword, sizeof(keyword) - 1u))
    return 0;
  size_t offset = sizeof(keyword);
  if (offset + 2u > size || data[offset] != 0u || data[offset + 1u] != 0u)
    return 0;
  offset += 2u;
  for (unsigned field = 0; field < 2u; ++field) {
    const uint8_t *end =
        (const uint8_t *)memchr(data + offset, 0, size - offset);
    if (!end)
      return 0;
    offset = (size_t)(end - data) + 1u;
  }
  *packet = data + offset;
  *packet_size = size - offset;
  return 1;
}

static int collect_png_metadata(QlicInput *input, char *error,
                                size_t error_capacity) {
  const uint8_t *data = input->data;
  size_t size = input->size;
  for (size_t offset = 8u; offset + 12u <= size;) {
    uint32_t length = be32(data + offset);
    if ((size_t)length > size - offset - 12u)
      return 0;
    const uint8_t *tag = data + offset + 4u;
    const uint8_t *payload = tag + 4u;
    if (!memcmp(tag, "IEND", 4u))
      return 1;
    if (!memcmp(tag, "eXIf", 4u)) {
      if (!input_add_metadata(input, (const uint8_t *)"EXIF", payload,
                              length, error, error_capacity))
        return 0;
    } else if (!memcmp(tag, "caBX", 4u)) {
      if (!input_add_metadata(input, (const uint8_t *)"JUMB", payload,
                              length, error, error_capacity))
        return 0;
    } else if (!memcmp(tag, "iTXt", 4u)) {
      const uint8_t *packet = NULL;
      size_t packet_size = 0;
      if (png_xmp_packet(payload, length, &packet, &packet_size)) {
        if (!input_add_metadata(input, (const uint8_t *)"XMP_", packet,
                                packet_size, error, error_capacity))
          return 0;
      } else if (!input_add_metadata(input, tag, payload, length, error,
                                     error_capacity)) {
        return 0;
      }
    } else if (!memcmp(tag, "iCCP", 4u) || !memcmp(tag, "pHYs", 4u)) {
      if (!input_add_metadata(input, tag, payload, length, error,
                              error_capacity))
        return 0;
    }
    offset += (size_t)length + 12u;
  }
  return 1;
}

static int collect_webp_metadata(QlicInput *input, char *error,
                                 size_t error_capacity) {
  const uint8_t *data = input->data;
  size_t size = input->size;
  for (size_t offset = 12u; offset + 8u <= size;) {
    uint32_t length = le32(data + offset + 4u);
    size_t payload = offset + 8u;
    if ((size_t)length > size - payload)
      return 0;
    const uint8_t *tag = data + offset;
    const uint8_t *bytes = data + payload;
    if (!memcmp(tag, "ICCP", 4u)) {
      if (input->icc_size) {
        set_error(error, error_capacity,
                  "the WebP source contains duplicate ICC profiles");
        return 0;
      }
      input->icc = bytes;
      input->icc_size = length;
    } else if (!memcmp(tag, "EXIF", 4u)) {
      if (!input_add_metadata(input, (const uint8_t *)"EXIF", bytes, length,
                              error, error_capacity))
        return 0;
    } else if (!memcmp(tag, "XMP ", 4u)) {
      if (!input_add_metadata(input, (const uint8_t *)"XMP_", bytes, length,
                              error, error_capacity))
        return 0;
    } else if (!memcmp(tag, "JUMB", 4u)) {
      if (!input_add_metadata(input, (const uint8_t *)"JUMB", bytes, length,
                              error, error_capacity))
        return 0;
    }
    size_t padded = (size_t)length + ((size_t)length & 1u);
    if (padded > size - payload)
      return 0;
    offset = payload + padded;
  }
  return 1;
}

static size_t tiff_type_size(uint16_t type) {
  static const uint8_t sizes[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4,
                                  8, 4, 8, 4, 8, 8, 8, 8, 8};
  return type < sizeof(sizes) ? sizes[type] : 0u;
}

static int collect_tiff_metadata(QlicInput *input, char *error,
                                 size_t error_capacity) {
  const uint8_t *data = input->data;
  size_t size = input->size;
  int little = data[0] == 'I';
  int big = tiff16(data + 2u, little) == 43u;
  uint64_t offset = big ? tiff64(data + 8u, little) : tiff32(data + 4u, little);
  size_t count_bytes = big ? 8u : 2u;
  size_t entry_bytes = big ? 20u : 12u;
  size_t inline_bytes = big ? 8u : 4u;
  unsigned directories = 0;
  while (offset && directories++ < 1024u) {
    if (!tiff_range(offset, count_bytes, size))
      return 0;
    uint64_t count = big ? tiff64(data + (size_t)offset, little)
                         : tiff16(data + (size_t)offset, little);
    uint64_t entries_offset = offset + count_bytes;
    if (count > SIZE_MAX / entry_bytes ||
        !tiff_range(entries_offset, count * entry_bytes + (big ? 8u : 4u),
                    size))
      return 0;
    for (uint64_t index = 0; index < count; ++index) {
      const uint8_t *entry =
          data + (size_t)entries_offset + (size_t)index * entry_bytes;
      uint16_t tag = tiff16(entry, little);
      if (tag != 700u && tag != 33723u && tag != 34377u &&
          tag != 34675u && tag != 52502u)
        continue;
      uint16_t type = tiff16(entry + 2u, little);
      size_t unit = tiff_type_size(type);
      uint64_t values = big ? tiff64(entry + 4u, little)
                            : tiff32(entry + 4u, little);
      if (!unit || values > UINT64_MAX / unit)
        return 0;
      uint64_t bytes64 = values * unit;
      if (bytes64 > SIZE_MAX)
        return 0;
      const uint8_t *value = NULL;
      if (bytes64 <= inline_bytes) {
        value = entry + (big ? 12u : 8u);
      } else {
        uint64_t value_offset = big ? tiff64(entry + 12u, little)
                                    : tiff32(entry + 8u, little);
        if (!tiff_range(value_offset, bytes64, size))
          return 0;
        value = data + (size_t)value_offset;
      }
      size_t bytes = (size_t)bytes64;
      if (tag == 34675u) {
        if (input->icc_size) {
          set_error(error, error_capacity,
                    "the TIFF source contains duplicate ICC profiles");
          return 0;
        }
        input->icc = value;
        input->icc_size = bytes;
      } else {
        const uint8_t *name = tag == 700u     ? (const uint8_t *)"XMP_"
                              : tag == 33723u ? (const uint8_t *)"IPTC"
                              : tag == 52502u ? (const uint8_t *)"JUMB"
                                             : (const uint8_t *)"8BIM";
        if (!input_add_metadata(input, name, value, bytes, error,
                                error_capacity))
          return 0;
      }
    }
    const uint8_t *next = data + (size_t)entries_offset + (size_t)count * entry_bytes;
    offset = big ? tiff64(next, little) : tiff32(next, little);
  }
  return offset == 0u;
}

static int collect_jxl_metadata(QlicInput *input, char *error,
                                size_t error_capacity) {
  static const uint8_t exif_box_type[] = {'E', 'x', 'i', 'f'};
  const uint8_t *data = input->data;
  size_t size = input->size;
  if (size < 12u || memcmp(data + 4u, "JXL ", 4u))
    return 1;
  for (size_t offset = 12u; offset + 8u <= size;) {
    uint64_t box_size = be32(data + offset);
    size_t header = 8u;
    if (box_size == 1u) {
      if (size - offset < 16u)
        return 0;
      box_size = be64(data + offset + 8u);
      header = 16u;
    } else if (!box_size) {
      box_size = size - offset;
    }
    if (box_size < header || box_size > size - offset)
      return 0;
    const uint8_t *type = data + offset + 4u;
    const uint8_t *payload = data + offset + header;
    size_t payload_size = (size_t)box_size - header;
    const uint8_t *tag = NULL;
    if (!memcmp(type, exif_box_type, sizeof(exif_box_type)))
      tag = (const uint8_t *)"EXIF";
    else if (!memcmp(type, "xml ", 4u))
      tag = (const uint8_t *)"XMP_";
    else if (!memcmp(type, "jumb", 4u))
      tag = (const uint8_t *)"JUMB";
    else if (!memcmp(type, "brob", 4u))
      tag = (const uint8_t *)"BROB";
    if (tag && !input_add_metadata(input, tag, payload, payload_size, error,
                                   error_capacity))
      return 0;
    offset += (size_t)box_size;
  }
  return 1;
}

static int jxl_signature(const uint8_t *data, size_t size) {
  static const uint8_t container[12] = {0, 0, 0, 12, 'J', 'X',
                                        'L', ' ', 13, 10, 135, 10};
  return (size >= 2 && data[0] == 0xff && data[1] == 0x0a) ||
         (size >= 12 && memcmp(data, container, 12) == 0);
}

static int load_jxl(JxlApi *api, char *error, size_t error_capacity) {
  memset(api, 0, sizeof(*api));
  api->module = load_runtime(L"jxl_dec.dll");
  if (!api->module) {
    set_error(error, error_capacity,
              "the JPEG XL decoder files are missing");
    return 0;
  }
#define JXL_FUNCTION(field, type, name)                                        \
  api->field = (type)(void *)GetProcAddress(api->module, name);                \
  if (!api->field) {                                                           \
    FreeLibrary(api->module);                                                  \
    memset(api, 0, sizeof(*api));                                              \
    set_error(error, error_capacity, "the JPEG XL decoder is incompatible");  \
    return 0;                                                                  \
  }
  JXL_FUNCTION(create, JxlCreateFn, "JxlDecoderCreate")
  JXL_FUNCTION(destroy, JxlDestroyFn, "JxlDecoderDestroy")
  JXL_FUNCTION(subscribe, JxlSubscribeFn, "JxlDecoderSubscribeEvents")
  JXL_FUNCTION(set_input, JxlSetInputFn, "JxlDecoderSetInput")
  JXL_FUNCTION(close_input, JxlCloseInputFn, "JxlDecoderCloseInput")
  JXL_FUNCTION(process, JxlProcessFn, "JxlDecoderProcessInput")
  JXL_FUNCTION(basic_info, JxlBasicInfoFn, "JxlDecoderGetBasicInfo")
  JXL_FUNCTION(buffer_size, JxlBufferSizeFn,
               "JxlDecoderImageOutBufferSize")
  JXL_FUNCTION(set_buffer, JxlSetBufferFn, "JxlDecoderSetImageOutBuffer")
#undef JXL_FUNCTION
  return 1;
}

static int valid_jxl_info(const JxlBasicInfo *info, uint64_t max_pixels,
                          char *error, size_t error_capacity) {
  uint64_t pixels = (uint64_t)info->xsize * info->ysize;
  if (!info->xsize || !info->ysize || pixels > max_pixels) {
    set_error(error, error_capacity, "the JPEG XL dimensions are too large");
    return 0;
  }
  if (info->bits_per_sample > 8 || info->exponent_bits_per_sample ||
      info->alpha_bits > 8 || info->alpha_exponent_bits) {
    set_error(error, error_capacity,
              "QLIC currently accepts images with up to 8 bits per channel");
    return 0;
  }
  uint32_t alpha_channels = info->alpha_bits ? 1u : 0u;
  if ((info->num_color_channels != 1 && info->num_color_channels != 3) ||
      info->num_extra_channels != alpha_channels ||
      info->alpha_premultiplied) {
    set_error(error, error_capacity,
              "the JPEG XL channel layout is unsupported");
    return 0;
  }
  if (info->have_animation) {
    set_error(error, error_capacity,
              "animated JPEG XL input is not supported yet");
    return 0;
  }
  return 1;
}

static int inspect_jxl(const uint8_t *data, size_t size, uint64_t max_pixels,
                       int *lossy, char *error, size_t error_capacity) {
  JxlApi api;
  if (!load_jxl(&api, error, error_capacity))
    return 0;
  JxlDecoder *decoder = api.create(NULL);
  int ok = decoder &&
           api.subscribe(decoder, JXL_DEC_BASIC_INFO) == JXL_DEC_SUCCESS &&
           api.set_input(decoder, data, size) == JXL_DEC_SUCCESS;
  if (ok)
    api.close_input(decoder);
  JxlBasicInfo info;
  memset(&info, 0, sizeof(info));
  while (ok) {
    JxlDecoderStatus status = api.process(decoder);
    if (status == JXL_DEC_BASIC_INFO) {
      ok = api.basic_info(decoder, &info) == JXL_DEC_SUCCESS &&
           valid_jxl_info(&info, max_pixels, error, error_capacity);
      if (ok && lossy)
        *lossy = !info.uses_original_profile;
      break;
    }
    if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT ||
        status == JXL_DEC_SUCCESS) {
      ok = 0;
      break;
    }
  }
  if (!ok && (!error || !error[0]))
    set_error(error, error_capacity, "the JPEG XL image is invalid");
  if (decoder)
    api.destroy(decoder);
  FreeLibrary(api.module);
  return ok;
}

static int decode_webp(const QlicInput *input, uint64_t max_pixels,
                       QlicInputImage *image, char *error,
                       size_t error_capacity) {
  HMODULE module = load_runtime(L"libwebp.dll");
  if (!module) {
    set_error(error, error_capacity, "the WebP decoder files are missing");
    return 0;
  }
  WebPGetFeaturesFn get_features =
      (WebPGetFeaturesFn)(void *)GetProcAddress(module,
                                                "WebPGetFeaturesInternal");
  WebPDecodeRGBAIntoFn decode =
      (WebPDecodeRGBAIntoFn)(void *)GetProcAddress(module,
                                                   "WebPDecodeRGBAInto");
  WebPBitstreamFeatures features;
  memset(&features, 0, sizeof(features));
  int ok = get_features && decode &&
           get_features(input->data, input->size, &features,
                        WEBP_DECODER_ABI_VERSION) == 0 &&
           (features.format == 1 || features.format == 2) &&
           !features.has_animation &&
           features.width > 0 && features.height > 0;
  uint64_t pixels =
      ok ? (uint64_t)features.width * (uint64_t)features.height : 0;
  if (!ok || pixels > max_pixels || pixels > SIZE_MAX / 4u ||
      features.width > INT_MAX / 4) {
    FreeLibrary(module);
    set_error(error, error_capacity,
              ok ? "the WebP dimensions are too large"
                 : "the WebP image is invalid");
    return 0;
  }
  size_t bytes = (size_t)pixels * 4u;
  uint8_t *rgba = (uint8_t *)malloc(bytes);
  if (!rgba || decode(input->data, input->size, rgba, bytes,
                      features.width * 4) != rgba) {
    free(rgba);
    FreeLibrary(module);
    set_error(error, error_capacity, "the WebP image could not be decoded");
    return 0;
  }
  image->rgba = rgba;
  image->width = (uint32_t)features.width;
  image->height = (uint32_t)features.height;
  FreeLibrary(module);
  return 1;
}

static int decode_jxl(const QlicInput *input, uint64_t max_pixels,
                      QlicInputImage *image, char *error,
                      size_t error_capacity) {
  JxlApi api;
  if (!load_jxl(&api, error, error_capacity))
    return 0;
  JxlDecoder *decoder = api.create(NULL);
  int ok = decoder &&
           api.subscribe(decoder, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) ==
               JXL_DEC_SUCCESS &&
           api.set_input(decoder, input->data, input->size) ==
               JXL_DEC_SUCCESS;
  if (ok)
    api.close_input(decoder);
  JxlBasicInfo info;
  memset(&info, 0, sizeof(info));
  JxlPixelFormat format = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
  size_t bytes = 0;
  int have_info = 0;
  int have_image = 0;
  while (ok) {
    JxlDecoderStatus status = api.process(decoder);
    if (status == JXL_DEC_BASIC_INFO) {
      ok = api.basic_info(decoder, &info) == JXL_DEC_SUCCESS &&
           valid_jxl_info(&info, max_pixels, error, error_capacity);
      have_info = ok;
    } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      ok = have_info &&
           api.buffer_size(decoder, &format, &bytes) == JXL_DEC_SUCCESS &&
           bytes == (size_t)info.xsize * info.ysize * 4u;
      if (ok)
        image->rgba = (uint8_t *)malloc(bytes);
      ok = ok && image->rgba &&
           api.set_buffer(decoder, &format, image->rgba, bytes) ==
               JXL_DEC_SUCCESS;
      if (!ok && (!error || !error[0]))
        set_error(error, error_capacity,
                  image->rgba ? "the JPEG XL output layout is unsupported"
                              : "out of memory");
    } else if (status == JXL_DEC_FULL_IMAGE) {
      have_image = 1;
    } else if (status == JXL_DEC_SUCCESS) {
      break;
    } else if (status == JXL_DEC_ERROR ||
               status == JXL_DEC_NEED_MORE_INPUT) {
      ok = 0;
      break;
    }
  }
  ok = ok && have_info && have_image && image->rgba;
  if (ok) {
    image->width = info.xsize;
    image->height = info.ysize;
  } else {
    free(image->rgba);
    memset(image, 0, sizeof(*image));
    if (!error || !error[0])
      set_error(error, error_capacity,
                "the JPEG XL image could not be decoded");
  }
  if (decoder)
    api.destroy(decoder);
  FreeLibrary(api.module);
  return ok;
}

static int inspect_input(QlicInput *input, uint64_t max_pixels, char *error,
                         size_t error_capacity) {
  const uint8_t *data = input->data;
  size_t size = input->size;
  int ok = 0;
  input->channels = 4u;
  input->bits_per_sample = 8u;
  if (is_png(data, size)) {
    ok = check_png(data, size, error, error_capacity);
    if (ok) {
      uint8_t color = data[25];
      input->channels =
          (color == 0 && !png_has_transparency(data, size))
              ? 1u
              : (color == 2 && !png_has_transparency(data, size)) ? 3u : 4u;
      input->bits_per_sample = data[24];
      ok = collect_png_metadata(input, error, error_capacity);
    }
  } else if (size >= 6 &&
             (!memcmp(data, "GIF87a", 6) || !memcmp(data, "GIF89a", 6))) {
    ok = 1;
  } else if (size >= 2 && data[0] == 'B' && data[1] == 'M') {
    ok = check_bmp(data, size, &input->lossy, error, error_capacity);
  } else if (size >= 4 &&
             ((!memcmp(data, "II*\0", 4)) ||
              (!memcmp(data, "MM\0*", 4)) ||
              (!memcmp(data, "II+\0", 4)) ||
              (!memcmp(data, "MM\0+", 4)))) {
    ok = check_tiff(data, size, &input->lossy, error, error_capacity);
    if (ok)
      ok = inspect_tiff_layout(data, size, &input->channels,
                               &input->bits_per_sample, &input->alpha_mode);
    if (ok)
      ok = collect_tiff_metadata(input, error, error_capacity);
  } else if (size >= 12 && !memcmp(data, "RIFF", 4) &&
             !memcmp(data + 8, "WEBP", 4)) {
    ok = check_webp(data, size, &input->lossy, error, error_capacity);
    if (ok)
      ok = collect_webp_metadata(input, error, error_capacity);
    input->decoder = QLIC_INPUT_WEBP;
  } else if (jxl_signature(data, size)) {
    ok = inspect_jxl(data, size, max_pixels, &input->lossy, error,
                     error_capacity);
    if (ok)
      ok = collect_jxl_metadata(input, error, error_capacity);
    input->decoder = QLIC_INPUT_JXL;
  } else if (looks_like_avif(data, size)) {
    ok = check_avif(data, size, &input->lossy, error, error_capacity);
  } else if (size >= 3 && data[0] == 0xff && data[1] == 0xd8 &&
             data[2] == 0xff) {
    input->lossy = 1;
    ok = 1;
  } else {
    set_error(error, error_capacity, "the input format is unsupported");
  }
  if (!ok)
    qlic_input_close(input);
  return ok;
}

int qlic_input_open(const wchar_t *path, uint64_t max_file_bytes,
                    uint64_t max_pixels, QlicInput *input, char *error,
                    size_t error_capacity) {
  if (!path || !input) {
    set_error(error, error_capacity, "invalid input image");
    return 0;
  }
  memset(input, 0, sizeof(*input));
  if (error && error_capacity)
    error[0] = 0;
  if (!read_file(path, max_file_bytes, &input->data, &input->size, error,
                 error_capacity))
    return 0;
  return inspect_input(input, max_pixels, error, error_capacity);
}

int qlic_input_open_memory(const uint8_t *data, size_t size,
                           uint64_t max_file_bytes, uint64_t max_pixels,
                           QlicInput *input, char *error,
                           size_t error_capacity) {
  if (!data || !size || (uint64_t)size > max_file_bytes || !input) {
    set_error(error, error_capacity, "invalid input image");
    return 0;
  }
  memset(input, 0, sizeof(*input));
  if (error && error_capacity)
    error[0] = 0;
  input->data = (uint8_t *)malloc(size);
  if (!input->data) {
    set_error(error, error_capacity, "out of memory");
    return 0;
  }
  memcpy(input->data, data, size);
  input->size = size;
  return inspect_input(input, max_pixels, error, error_capacity);
}

int qlic_input_decode(const QlicInput *input, uint64_t max_pixels,
                      QlicInputImage *image, char *error,
                      size_t error_capacity) {
  if (!input || !image || !input->data) {
    set_error(error, error_capacity, "invalid input image");
    return 0;
  }
  memset(image, 0, sizeof(*image));
  if (input->decoder == QLIC_INPUT_WEBP)
    return decode_webp(input, max_pixels, image, error, error_capacity);
  if (input->decoder == QLIC_INPUT_JXL)
    return decode_jxl(input, max_pixels, image, error, error_capacity);
  set_error(error, error_capacity, "the input uses the Windows image decoder");
  return 0;
}

void qlic_input_close(QlicInput *input) {
  if (!input)
    return;
  free(input->data);
  free(input->owned_icc);
  memset(input, 0, sizeof(*input));
}
