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

static int check_png(const uint8_t *data, size_t size, char *error,
                     size_t error_capacity) {
  if (size < 29 || be32(data + 8) != 13 ||
      memcmp(data + 12, "IHDR", 4) != 0) {
    set_error(error, error_capacity, "the PNG header is invalid");
    return 0;
  }
  uint8_t depth = data[24];
  uint8_t color = data[25];
  if (depth > 8) {
    set_error(error, error_capacity,
              "QLIC currently accepts images with up to 8 bits per channel");
    return 0;
  }
  if (color != 0 && color != 2 && color != 3 && color != 4 && color != 6) {
    set_error(error, error_capacity, "the PNG color format is unsupported");
    return 0;
  }
  return 1;
}

static int check_bmp(const uint8_t *data, size_t size, char *error,
                     size_t error_capacity) {
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
  if (!depth || depth > 32 ||
      (compression != 0 && compression != 1 && compression != 2 &&
       compression != 3 && compression != 6)) {
    set_error(error, error_capacity,
              compression == 4
                  ? "lossy JPEG compressed BMP images are not accepted"
                  : "the BMP pixel format is unsupported");
    return 0;
  }
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

static int check_tiff(const uint8_t *data, size_t size, char *error,
                      size_t error_capacity) {
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
          if ((tag == 258 && sample > 8) ||
              (tag == 339 && sample != 1)) {
            set_error(
                error, error_capacity,
                tag == 258
                    ? "QLIC currently accepts images with up to 8 bits per channel"
                    : "floating point and signed TIFF images are unsupported");
            return 0;
          }
        }
      }
    }
    if (compression == 6 || compression == 7) {
      set_error(error, error_capacity,
                "lossy JPEG compressed TIFF images are not accepted");
      return 0;
    }
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

static int check_webp(const uint8_t *data, size_t size, char *error,
                      size_t error_capacity) {
  if (size < 20 || memcmp(data, "RIFF", 4) ||
      memcmp(data + 8, "WEBP", 4)) {
    set_error(error, error_capacity, "the WebP header is invalid");
    return 0;
  }
  int lossless = 0;
  for (size_t offset = 12; offset + 8 <= size;) {
    uint32_t chunk_size = le32(data + offset + 4);
    size_t payload = offset + 8u;
    if (chunk_size > size - payload) {
      set_error(error, error_capacity, "the WebP chunks are invalid");
      return 0;
    }
    if (!memcmp(data + offset, "VP8 ", 4)) {
      set_error(error, error_capacity,
                "lossy WebP images are not accepted");
      return 0;
    }
    if (!memcmp(data + offset, "ANIM", 4) ||
        !memcmp(data + offset, "ANMF", 4)) {
      set_error(error, error_capacity,
                "animated WebP input is not supported yet");
      return 0;
    }
    if (!memcmp(data + offset, "VP8L", 4))
      ++lossless;
    size_t padded = (size_t)chunk_size + ((size_t)chunk_size & 1u);
    if (padded > size - payload)
      break;
    offset = payload + padded;
  }
  if (lossless != 1) {
    set_error(error, error_capacity,
              "the WebP image is not tagged as lossless");
    return 0;
  }
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

static int check_avif(const uint8_t *data, size_t size, char *error,
                      size_t error_capacity) {
  AvifCheck check = {0};
  if (!scan_avif_boxes(data, size, 0, &check) || !check.avif_brand ||
      !check.av1_config) {
    set_error(error, error_capacity, "the AVIF structure is unsupported");
    return 0;
  }
  if (check.bad_depth) {
    set_error(error, error_capacity,
              "only 8 bit AVIF without chroma subsampling is accepted");
    return 0;
  }
  if (check.lossy_color) {
    set_error(error, error_capacity,
              "lossy AVIF color encoding is not accepted");
    return 0;
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
  if (!info->uses_original_profile) {
    set_error(error, error_capacity,
              "lossy JPEG XL images are not accepted");
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
                       char *error, size_t error_capacity) {
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
           features.format == 2 && !features.has_animation &&
           features.width > 0 && features.height > 0;
  uint64_t pixels =
      ok ? (uint64_t)features.width * (uint64_t)features.height : 0;
  if (!ok || pixels > max_pixels || pixels > SIZE_MAX / 4u ||
      features.width > INT_MAX / 4) {
    FreeLibrary(module);
    set_error(error, error_capacity,
              ok ? "the WebP dimensions are too large"
                 : "the lossless WebP image is invalid");
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
  if (is_png(data, size)) {
    ok = check_png(data, size, error, error_capacity);
  } else if (size >= 6 &&
             (!memcmp(data, "GIF87a", 6) || !memcmp(data, "GIF89a", 6))) {
    ok = 1;
  } else if (size >= 2 && data[0] == 'B' && data[1] == 'M') {
    ok = check_bmp(data, size, error, error_capacity);
  } else if (size >= 4 &&
             ((!memcmp(data, "II*\0", 4)) ||
              (!memcmp(data, "MM\0*", 4)) ||
              (!memcmp(data, "II+\0", 4)) ||
              (!memcmp(data, "MM\0+", 4)))) {
    ok = check_tiff(data, size, error, error_capacity);
  } else if (size >= 12 && !memcmp(data, "RIFF", 4) &&
             !memcmp(data + 8, "WEBP", 4)) {
    ok = check_webp(data, size, error, error_capacity);
    input->decoder = QLIC_INPUT_WEBP;
  } else if (jxl_signature(data, size)) {
    ok = inspect_jxl(data, size, max_pixels, error, error_capacity);
    input->decoder = QLIC_INPUT_JXL;
  } else if (looks_like_avif(data, size)) {
    ok = check_avif(data, size, error, error_capacity);
  } else if (size >= 3 && data[0] == 0xff && data[1] == 0xd8 &&
             data[2] == 0xff) {
    set_error(error, error_capacity, "lossy JPEG images are not accepted");
  } else {
    set_error(error, error_capacity,
              "the input format is unsupported or cannot be verified as lossless");
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
  memset(input, 0, sizeof(*input));
}
