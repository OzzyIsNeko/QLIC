#include <qlic/qlic.h>

#include "parallel.h"
#include <png.h>

#ifdef QLIC_HAVE_AVIF
#include <avif/avif.h>
#endif
#ifdef QLIC_HAVE_JXL
#include <jxl/decode.h>
#endif
#ifdef QLIC_HAVE_TIFF
#include <tiffio.h>
#endif
#ifdef QLIC_HAVE_WEBP
#include <webp/decode.h>
#endif

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define INPUT_LIMIT UINT64_C(536870912)
#define PIXEL_LIMIT UINT64_C(67108864)

typedef struct {
  uint8_t *rgba;
  uint32_t width;
  uint32_t height;
} InputImage;

typedef struct {
  const char **values;
  int count;
  uint32_t threads;
} Arguments;

typedef struct {
  const char *input;
  char *output;
  uint32_t threads;
  int ok;
} BatchItem;

static uint16_t le16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int dimensions(uint32_t width, uint32_t height, size_t *bytes) {
  uint64_t pixels = (uint64_t)width * height;
  if (!width || !height || pixels > PIXEL_LIMIT ||
      pixels > SIZE_MAX / 4u)
    return 0;
  *bytes = (size_t)pixels * 4u;
  return 1;
}

static void input_free(InputImage *image) {
  free(image->rgba);
  memset(image, 0, sizeof(*image));
}

static int read_file(const char *path, uint8_t **data, size_t *size) {
  FILE *file = fopen(path, "rb");
  if (!file) {
    fprintf(stderr, "error: could not open %s\n", path);
    return 0;
  }
  int ok = fseek(file, 0, SEEK_END) == 0;
  long measured = ok ? ftell(file) : -1;
  if (measured <= 0 || (uint64_t)measured > INPUT_LIMIT ||
      (uint64_t)measured > SIZE_MAX ||
      fseek(file, 0, SEEK_SET) != 0)
    ok = 0;
  uint8_t *bytes =
      ok ? (uint8_t *)malloc((size_t)measured) : NULL;
  if (!bytes)
    ok = 0;
  if (ok && fread(bytes, 1, (size_t)measured, file) !=
                (size_t)measured)
    ok = 0;
  if (fclose(file) != 0)
    ok = 0;
  if (!ok) {
    free(bytes);
    fprintf(stderr, "error: could not read %s\n", path);
    return 0;
  }
  *data = bytes;
  *size = (size_t)measured;
  return 1;
}

static int write_file(const char *path, const uint8_t *data, size_t size) {
  size_t path_size = strlen(path);
  if (path_size > SIZE_MAX - 40u)
    return 0;
  char *temporary = (char *)malloc(path_size + 40u);
  if (!temporary)
    return 0;
  snprintf(temporary, path_size + 40u, "%s.tmp.%ld", path, (long)getpid());
  FILE *file = fopen(temporary, "wb");
  int ok = file != NULL;
  if (ok && size && fwrite(data, 1, size, file) != size)
    ok = 0;
  if (file && fclose(file) != 0)
    ok = 0;
  if (ok && rename(temporary, path) != 0)
    ok = 0;
  if (!ok)
    unlink(temporary);
  free(temporary);
  if (!ok)
    fprintf(stderr, "error: could not write %s\n", path);
  return ok;
}

static int load_png(const char *path, InputImage *out) {
  png_image image;
  memset(&image, 0, sizeof(image));
  image.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&image, path)) {
    fprintf(stderr, "error: invalid PNG: %s\n", image.message);
    return 0;
  }
  if (PNG_IMAGE_SAMPLE_COMPONENT_SIZE(image.format) > 1) {
    png_image_free(&image);
    fprintf(stderr, "error: QLIC currently accepts up to 8 bits per channel\n");
    return 0;
  }
  size_t bytes = 0;
  if (!dimensions(image.width, image.height, &bytes)) {
    png_image_free(&image);
    fprintf(stderr, "error: image dimensions are too large\n");
    return 0;
  }
  uint8_t *rgba = (uint8_t *)malloc(bytes);
  if (!rgba) {
    png_image_free(&image);
    fprintf(stderr, "error: out of memory\n");
    return 0;
  }
  image.format = PNG_FORMAT_RGBA;
  if (!png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
    fprintf(stderr, "error: invalid PNG: %s\n", image.message);
    png_image_free(&image);
    free(rgba);
    return 0;
  }
  uint32_t width = image.width;
  uint32_t height = image.height;
  png_image_free(&image);
  out->rgba = rgba;
  out->width = width;
  out->height = height;
  return 1;
}

static int load_bmp(const uint8_t *data, size_t size, InputImage *out) {
  if (size < 54u || data[0] != 'B' || data[1] != 'M' ||
      le32(data + 14) < 40u || le16(data + 26) != 1u) {
    fprintf(stderr, "error: invalid BMP\n");
    return 0;
  }
  uint32_t width = le32(data + 18);
  int32_t signed_height = (int32_t)le32(data + 22);
  uint32_t height =
      signed_height < 0 ? (uint32_t)(-(int64_t)signed_height)
                        : (uint32_t)signed_height;
  uint16_t depth = le16(data + 28);
  uint32_t compression = le32(data + 30);
  int indexed = depth == 1u || depth == 4u || depth == 8u;
  if (!((compression == 0u &&
         (indexed || depth == 24u || depth == 32u)) ||
        (compression == 1u && depth == 8u) ||
        (compression == 2u && depth == 4u))) {
    fprintf(stderr,
            compression == 4u
                ? "error: lossy JPEG compressed BMP images are not accepted\n"
                : "error: unsupported BMP pixel format\n");
    return 0;
  }
  size_t bytes = 0;
  if (!dimensions(width, height, &bytes)) {
    fprintf(stderr, "error: image dimensions are too large\n");
    return 0;
  }
  uint32_t offset = le32(data + 10);
  if (offset > size)
    return 0;
  uint32_t palette_count = 0;
  const uint8_t *palette = NULL;
  if (indexed) {
    palette_count = le32(data + 46);
    if (!palette_count)
      palette_count = 1u << depth;
    uint64_t palette_offset = 14u + le32(data + 14);
    uint64_t palette_bytes = (uint64_t)palette_count * 4u;
    if (palette_count > 256u || palette_offset > offset ||
        palette_bytes > offset - palette_offset) {
      fprintf(stderr, "error: invalid BMP palette\n");
      return 0;
    }
    palette = data + (size_t)palette_offset;
  }
  uint8_t *rgba = (uint8_t *)calloc(bytes, 1u);
  if (!rgba) {
    fprintf(stderr, "error: out of memory\n");
    return 0;
  }
  size_t row = 0;
  int has_alpha = 0;
  if (indexed) {
    for (size_t i = 0; i < (size_t)width * height; ++i) {
      rgba[i * 4u + 0u] = palette[2];
      rgba[i * 4u + 1u] = palette[1];
      rgba[i * 4u + 2u] = palette[0];
      rgba[i * 4u + 3u] = 255u;
    }
  }
  if (compression == 0u) {
    uint64_t row64 = ((uint64_t)width * depth + 31u) / 32u * 4u;
    if (row64 > SIZE_MAX || row64 > (size - offset) / height) {
      free(rgba);
      fprintf(stderr, "error: truncated BMP\n");
      return 0;
    }
    row = (size_t)row64;
    for (uint32_t y = 0; y < height; ++y) {
      uint32_t source_y = signed_height < 0 ? y : height - 1u - y;
      const uint8_t *source = data + offset + (size_t)source_y * row;
      uint8_t *destination = rgba + (size_t)y * width * 4u;
      for (uint32_t x = 0; x < width; ++x) {
        uint32_t index = 0;
        if (depth == 1u)
          index = (source[x >> 3u] >> (7u - (x & 7u))) & 1u;
        else if (depth == 4u)
          index = (source[x >> 1u] >> ((x & 1u) ? 0u : 4u)) & 15u;
        else if (depth == 8u)
          index = source[x];
        if (indexed) {
          if (index >= palette_count) {
            free(rgba);
            fprintf(stderr, "error: invalid BMP palette index\n");
            return 0;
          }
          destination[x * 4u + 0u] = palette[index * 4u + 2u];
          destination[x * 4u + 1u] = palette[index * 4u + 1u];
          destination[x * 4u + 2u] = palette[index * 4u + 0u];
          destination[x * 4u + 3u] = 255u;
        } else {
          const uint8_t *pixel = source + (size_t)x * (depth / 8u);
          destination[x * 4u + 0u] = pixel[2];
          destination[x * 4u + 1u] = pixel[1];
          destination[x * 4u + 2u] = pixel[0];
          destination[x * 4u + 3u] = depth == 32u ? pixel[3] : 255u;
          has_alpha |= depth == 32u && pixel[3] != 0u;
        }
      }
    }
    if (depth == 32u && !has_alpha)
      for (size_t i = 3u; i < bytes; i += 4u)
        rgba[i] = 255u;
  } else {
    if (signed_height < 0) {
      free(rgba);
      fprintf(stderr, "error: invalid top down RLE BMP\n");
      return 0;
    }
    size_t position = offset;
    uint32_t x = 0;
    uint32_t line = 0;
    int finished = 0;
    while (position + 2u <= size && !finished) {
      uint32_t count = data[position++];
      uint32_t value = data[position++];
      if (count) {
        if (line >= height || count > width - x) {
          free(rgba);
          fprintf(stderr, "error: invalid BMP run\n");
          return 0;
        }
        for (uint32_t i = 0; i < count; ++i) {
          uint32_t index =
              depth == 8u ? value
                          : (value >> ((i & 1u) ? 0u : 4u)) & 15u;
          if (index >= palette_count) {
            free(rgba);
            fprintf(stderr, "error: invalid BMP palette index\n");
            return 0;
          }
          uint32_t y = height - 1u - line;
          uint8_t *pixel =
              rgba + ((size_t)y * width + x + i) * 4u;
          pixel[0] = palette[index * 4u + 2u];
          pixel[1] = palette[index * 4u + 1u];
          pixel[2] = palette[index * 4u + 0u];
          pixel[3] = 255u;
        }
        x += count;
      } else if (value == 0u) {
        if (line >= height) {
          free(rgba);
          fprintf(stderr, "error: invalid BMP line\n");
          return 0;
        }
        x = 0;
        ++line;
      } else if (value == 1u) {
        finished = 1;
      } else if (value == 2u) {
        if (position + 2u > size) {
          free(rgba);
          fprintf(stderr, "error: truncated BMP delta\n");
          return 0;
        }
        uint32_t dx = data[position++];
        uint32_t dy = data[position++];
        if (line >= height || dx > width - x ||
            dy > height - line - 1u) {
          free(rgba);
          fprintf(stderr, "error: invalid BMP delta\n");
          return 0;
        }
        x += dx;
        line += dy;
      } else {
        uint32_t count2 = value;
        size_t packed = depth == 8u ? count2 : (count2 + 1u) / 2u;
        if (line >= height || count2 > width - x ||
            packed > size - position ||
            packed + (packed & 1u) > size - position) {
          free(rgba);
          fprintf(stderr, "error: truncated BMP run\n");
          return 0;
        }
        for (uint32_t i = 0; i < count2; ++i) {
          uint32_t index =
              depth == 8u
                  ? data[position + i]
                  : (data[position + (i >> 1u)] >>
                     ((i & 1u) ? 0u : 4u)) &
                        15u;
          if (index >= palette_count) {
            free(rgba);
            fprintf(stderr, "error: invalid BMP palette index\n");
            return 0;
          }
          uint32_t y = height - 1u - line;
          uint8_t *pixel =
              rgba + ((size_t)y * width + x + i) * 4u;
          pixel[0] = palette[index * 4u + 2u];
          pixel[1] = palette[index * 4u + 1u];
          pixel[2] = palette[index * 4u + 0u];
          pixel[3] = 255u;
        }
        position += packed + (packed & 1u);
        x += count2;
      }
    }
    if (!finished) {
      free(rgba);
      fprintf(stderr, "error: truncated BMP\n");
      return 0;
    }
  }
  out->rgba = rgba;
  out->width = width;
  out->height = height;
  return 1;
}

#ifdef QLIC_HAVE_WEBP
static int load_webp(const uint8_t *data, size_t size, InputImage *out) {
  WebPBitstreamFeatures features;
  VP8StatusCode status = WebPGetFeatures(data, size, &features);
  if (status != VP8_STATUS_OK) {
    fprintf(stderr, "error: invalid WebP\n");
    return 0;
  }
  if (features.format == 1) {
    fprintf(stderr, "error: lossy WebP images are not accepted\n");
    return 0;
  }
  if (features.format != 2 || features.has_animation) {
    fprintf(stderr, "error: unsupported WebP image\n");
    return 0;
  }
  size_t bytes = 0;
  if (!dimensions((uint32_t)features.width, (uint32_t)features.height,
                  &bytes)) {
    fprintf(stderr, "error: image dimensions are too large\n");
    return 0;
  }
  uint8_t *rgba = (uint8_t *)malloc(bytes);
  if (!rgba) {
    fprintf(stderr, "error: out of memory\n");
    return 0;
  }
  if (!WebPDecodeRGBAInto(data, size, rgba, bytes, features.width * 4)) {
    free(rgba);
    fprintf(stderr, "error: invalid lossless WebP\n");
    return 0;
  }
  out->rgba = rgba;
  out->width = (uint32_t)features.width;
  out->height = (uint32_t)features.height;
  return 1;
}
#endif

#ifdef QLIC_HAVE_JXL
static int load_jxl(const uint8_t *data, size_t size, InputImage *out) {
  JxlDecoder *decoder = JxlDecoderCreate(NULL);
  if (!decoder ||
      JxlDecoderSubscribeEvents(
          decoder, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) !=
          JXL_DEC_SUCCESS ||
      JxlDecoderSetInput(decoder, data, size) != JXL_DEC_SUCCESS) {
    JxlDecoderDestroy(decoder);
    fprintf(stderr, "error: invalid JPEG XL\n");
    return 0;
  }
  JxlDecoderCloseInput(decoder);
  JxlBasicInfo info;
  memset(&info, 0, sizeof(info));
  JxlPixelFormat format = {4u, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0u};
  uint8_t *rgba = NULL;
  size_t bytes = 0;
  int ok = 0;
  for (;;) {
    JxlDecoderStatus status = JxlDecoderProcessInput(decoder);
    if (status == JXL_DEC_BASIC_INFO) {
      if (JxlDecoderGetBasicInfo(decoder, &info) != JXL_DEC_SUCCESS)
        break;
      size_t expected = 0;
      if (!dimensions(info.xsize, info.ysize, &expected) ||
          !info.uses_original_profile || info.bits_per_sample > 8u ||
          info.exponent_bits_per_sample || info.alpha_bits > 8u ||
          info.alpha_exponent_bits ||
          (info.num_color_channels != 1u &&
           info.num_color_channels != 3u) ||
          info.num_extra_channels != (info.alpha_bits ? 1u : 0u) ||
          info.alpha_premultiplied || info.have_animation ||
          JxlDecoderImageOutBufferSize(decoder, &format, &bytes) !=
              JXL_DEC_SUCCESS ||
          bytes != expected) {
        fprintf(stderr,
                !info.uses_original_profile
                    ? "error: lossy JPEG XL images are not accepted\n"
                    : "error: unsupported JPEG XL image\n");
        break;
      }
    } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      rgba = (uint8_t *)malloc(bytes);
      if (!rgba ||
          JxlDecoderSetImageOutBuffer(decoder, &format, rgba, bytes) !=
              JXL_DEC_SUCCESS)
        break;
    } else if (status == JXL_DEC_FULL_IMAGE) {
      ok = rgba != NULL;
    } else if (status == JXL_DEC_SUCCESS) {
      break;
    } else if (status == JXL_DEC_ERROR ||
               status == JXL_DEC_NEED_MORE_INPUT) {
      break;
    }
  }
  JxlDecoderDestroy(decoder);
  if (!ok) {
    free(rgba);
    if (!info.xsize)
      fprintf(stderr, "error: invalid JPEG XL\n");
    return 0;
  }
  out->rgba = rgba;
  out->width = info.xsize;
  out->height = info.ysize;
  return 1;
}
#endif

#ifdef QLIC_HAVE_AVIF
static int load_avif(const uint8_t *data, size_t size, InputImage *out) {
  avifDecoder *decoder = avifDecoderCreate();
  avifImage *image = avifImageCreateEmpty();
  if (!decoder || !image ||
      avifDecoderReadMemory(decoder, image, data, size) != AVIF_RESULT_OK) {
    avifImageDestroy(image);
    avifDecoderDestroy(decoder);
    fprintf(stderr, "error: invalid AVIF\n");
    return 0;
  }
  if (image->depth > 8u ||
      (image->yuvFormat != AVIF_PIXEL_FORMAT_YUV444 &&
       image->yuvFormat != AVIF_PIXEL_FORMAT_YUV400) ||
      image->yuvRange != AVIF_RANGE_FULL ||
      (image->yuvFormat != AVIF_PIXEL_FORMAT_YUV400 &&
       image->matrixCoefficients != AVIF_MATRIX_COEFFICIENTS_IDENTITY)) {
    avifImageDestroy(image);
    avifDecoderDestroy(decoder);
    fprintf(stderr, "error: lossy or unsupported AVIF images are not accepted\n");
    return 0;
  }
  size_t bytes = 0;
  if (!dimensions(image->width, image->height, &bytes)) {
    avifImageDestroy(image);
    avifDecoderDestroy(decoder);
    fprintf(stderr, "error: image dimensions are too large\n");
    return 0;
  }
  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, image);
  rgb.format = AVIF_RGB_FORMAT_RGBA;
  rgb.depth = 8u;
  avifRGBImageAllocatePixels(&rgb);
  int ok = rgb.pixels &&
           avifImageYUVToRGB(image, &rgb) == AVIF_RESULT_OK &&
           rgb.rowBytes == (size_t)image->width * 4u;
  uint8_t *rgba = ok ? (uint8_t *)malloc(bytes) : NULL;
  if (rgba)
    memcpy(rgba, rgb.pixels, bytes);
  uint32_t width = rgb.width;
  uint32_t height = rgb.height;
  avifRGBImageFreePixels(&rgb);
  avifImageDestroy(image);
  avifDecoderDestroy(decoder);
  if (!rgba) {
    fprintf(stderr, "error: AVIF conversion failed\n");
    return 0;
  }
  out->rgba = rgba;
  out->width = width;
  out->height = height;
  return 1;
}
#endif

#ifdef QLIC_HAVE_TIFF
static int load_tiff(const char *path, InputImage *out) {
  TIFF *tiff = TIFFOpen(path, "r");
  if (!tiff) {
    fprintf(stderr, "error: invalid TIFF\n");
    return 0;
  }
  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t bits = 8;
  uint16_t sample_format = SAMPLEFORMAT_UINT;
  uint16_t compression = COMPRESSION_NONE;
  int tags = TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) &&
             TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height);
  (void)TIFFGetFieldDefaulted(tiff, TIFFTAG_BITSPERSAMPLE, &bits);
  (void)TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLEFORMAT, &sample_format);
  (void)TIFFGetFieldDefaulted(tiff, TIFFTAG_COMPRESSION, &compression);
  size_t bytes = 0;
  if (!tags || bits > 8u || sample_format != SAMPLEFORMAT_UINT ||
      compression == COMPRESSION_JPEG ||
      !dimensions(width, height, &bytes)) {
    TIFFClose(tiff);
    fprintf(stderr,
            compression == COMPRESSION_JPEG
                ? "error: lossy JPEG compressed TIFF images are not accepted\n"
                : "error: unsupported TIFF image\n");
    return 0;
  }
  if (bytes > (size_t)PTRDIFF_MAX) {
    TIFFClose(tiff);
    fprintf(stderr, "error: TIFF image is too large\n");
    return 0;
  }
  uint32_t *raster = (uint32_t *)_TIFFmalloc((tmsize_t)bytes);
  uint8_t *rgba = (uint8_t *)malloc(bytes);
  if (!raster || !rgba ||
      !TIFFReadRGBAImageOriented(tiff, width, height, raster,
                                 ORIENTATION_TOPLEFT, 0)) {
    _TIFFfree(raster);
    free(rgba);
    TIFFClose(tiff);
    fprintf(stderr, "error: TIFF conversion failed\n");
    return 0;
  }
  for (size_t i = 0; i < (size_t)width * height; ++i) {
    rgba[i * 4u + 0u] = TIFFGetR(raster[i]);
    rgba[i * 4u + 1u] = TIFFGetG(raster[i]);
    rgba[i * 4u + 2u] = TIFFGetB(raster[i]);
    rgba[i * 4u + 3u] = TIFFGetA(raster[i]);
  }
  _TIFFfree(raster);
  TIFFClose(tiff);
  out->rgba = rgba;
  out->width = width;
  out->height = height;
  return 1;
}
#endif

static int load_image(const char *path, InputImage *out) {
  memset(out, 0, sizeof(*out));
  uint8_t *data = NULL;
  size_t size = 0;
  if (!read_file(path, &data, &size))
    return 0;
  int ok = 0;
  static const uint8_t png_signature[8] =
      {137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u};
#ifdef QLIC_HAVE_JXL
  static const uint8_t jxl_container[12] =
      {0u, 0u, 0u, 12u, 'J', 'X', 'L', ' ', 13u, 10u, 135u, 10u};
#endif
  if (size >= 8u && memcmp(data, png_signature, 8u) == 0) {
    free(data);
    return load_png(path, out);
  }
  if (size >= 2u && data[0] == 'B' && data[1] == 'M') {
    ok = load_bmp(data, size, out);
  } else if (size >= 3u && data[0] == 0xffu && data[1] == 0xd8u &&
             data[2] == 0xffu) {
    fprintf(stderr, "error: lossy JPEG images are not accepted\n");
#ifdef QLIC_HAVE_WEBP
  } else if (size >= 12u && memcmp(data, "RIFF", 4u) == 0 &&
             memcmp(data + 8u, "WEBP", 4u) == 0) {
    ok = load_webp(data, size, out);
#endif
#ifdef QLIC_HAVE_JXL
  } else if ((size >= 2u && data[0] == 0xffu && data[1] == 0x0au) ||
             (size >= 12u && memcmp(data, jxl_container, 12u) == 0)) {
    ok = load_jxl(data, size, out);
#endif
#ifdef QLIC_HAVE_AVIF
  } else if (size >= 16u && memcmp(data + 4u, "ftyp", 4u) == 0 &&
             (memcmp(data + 8u, "avif", 4u) == 0 ||
              memcmp(data + 8u, "avis", 4u) == 0)) {
    ok = load_avif(data, size, out);
#endif
#ifdef QLIC_HAVE_TIFF
  } else if (size >= 4u &&
             ((!memcmp(data, "II*\0", 4u)) ||
              (!memcmp(data, "MM\0*", 4u)))) {
    free(data);
    return load_tiff(path, out);
#endif
  } else {
    fprintf(stderr, "error: unsupported image format\n");
  }
  free(data);
  return ok;
}

static int save_png(const char *path, const qlic_image *image) {
  size_t path_size = strlen(path);
  if (path_size > SIZE_MAX - 40u)
    return 0;
  char *temporary = (char *)malloc(path_size + 40u);
  if (!temporary)
    return 0;
  snprintf(temporary, path_size + 40u, "%s.tmp.%ld", path, (long)getpid());
  png_image png;
  memset(&png, 0, sizeof(png));
  png.version = PNG_IMAGE_VERSION;
  png.width = image->width;
  png.height = image->height;
  png.format = PNG_FORMAT_RGBA;
  if (!png_image_write_to_file(&png, temporary, 0, image->rgba,
                               (png_int_32)image->stride, NULL)) {
    fprintf(stderr, "error: could not write PNG: %s\n", png.message);
    unlink(temporary);
    free(temporary);
    return 0;
  }
  int ok = rename(temporary, path) == 0;
  if (!ok) {
    fprintf(stderr, "error: could not write %s\n", path);
    unlink(temporary);
  }
  free(temporary);
  return ok;
}

static int parse_threads(const char *value, uint32_t *threads) {
  if (strcmp(value, "all") == 0) {
    *threads = qlic_hardware_thread_count();
    return 1;
  }
  char *end = NULL;
  errno = 0;
  unsigned long parsed = strtoul(value, &end, 10);
  if (errno || !end || *end || !parsed || parsed > UINT32_MAX)
    return 0;
  uint32_t hardware = qlic_hardware_thread_count();
  *threads = parsed > hardware ? hardware : (uint32_t)parsed;
  return 1;
}

static int arguments(int argc, char **argv, Arguments *out) {
  out->values = (const char **)calloc((size_t)argc, sizeof(*out->values));
  if (!out->values)
    return 0;
  out->threads = 1u;
  for (int i = 2; i < argc; ++i) {
    if (strcmp(argv[i], "--threads") == 0) {
      if (++i >= argc || !parse_threads(argv[i], &out->threads)) {
        fprintf(stderr, "error: invalid --threads option\n");
        free(out->values);
        return 0;
      }
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "error: unknown option %s\n", argv[i]);
      free(out->values);
      return 0;
    } else {
      out->values[out->count++] = argv[i];
    }
  }
  return 1;
}

static int pack_file(const char *input, const char *output,
                     uint32_t threads, int quiet) {
  InputImage image;
  if (!load_image(input, &image))
    return 0;
  qlic_encode_options options;
  qlic_encode_options_default(&options);
  options.threads = threads;
  uint8_t *encoded = NULL;
  size_t encoded_size = 0;
  size_t stride = (size_t)image.width * 4u;
  int status =
      qlic_encode_rgba(image.rgba, stride * image.height,
                       image.width, image.height, stride, &options,
                       &encoded, &encoded_size);
  input_free(&image);
  if (status != QLIC_OK) {
    fprintf(stderr, "error: %s\n", qlic_last_error());
    return 0;
  }
  int ok = write_file(output, encoded, encoded_size);
  qlic_free(encoded);
  if (ok && !quiet)
    printf("%s -> %s, %zu bytes\n", input, output, encoded_size);
  return ok;
}

static int unpack_file(const char *input, const char *output,
                       uint32_t threads) {
  uint8_t *data = NULL;
  size_t size = 0;
  if (!read_file(input, &data, &size))
    return 0;
  qlic_image image;
  memset(&image, 0, sizeof(image));
  qlic_decode_limits limits;
  qlic_decode_limits_default(&limits);
  limits.threads = threads;
  int status = qlic_decode_rgba(data, size, &limits, &image);
  free(data);
  if (status != QLIC_OK) {
    fprintf(stderr, "error: %s\n", qlic_last_error());
    return 0;
  }
  int ok = save_png(output, &image);
  qlic_image_free(&image);
  return ok;
}

static const char *base_name(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static int batch_path(const char *directory, const char *input,
                      char **output) {
  const char *name = base_name(input);
  size_t stem = strlen(name);
  size_t directory_size = strlen(directory);
  if (stem > INT_MAX || directory_size > SIZE_MAX - stem - 8u)
    return 0;
  char *path = (char *)malloc(directory_size + stem + 8u);
  if (!path)
    return 0;
  snprintf(path, directory_size + stem + 8u, "%s/%.*s.qlic",
           directory, (int)stem, name);
  *output = path;
  return 1;
}

static void batch_item(void *context, unsigned index) {
  BatchItem *item = &((BatchItem *)context)[index];
  if (!item->output)
    return;
  item->ok =
      pack_file(item->input, item->output, item->threads, 0);
}

static int command_batch(const Arguments *args) {
  if (args->count < 2) {
    fprintf(stderr, "error: batch expects an output directory and images\n");
    return 2;
  }
  const char *directory = args->values[0];
  if (mkdir(directory, 0775) != 0 && errno != EEXIST) {
    fprintf(stderr, "error: could not create %s\n", directory);
    return 1;
  }
  unsigned count = (unsigned)(args->count - 1);
  BatchItem *items = (BatchItem *)calloc(count, sizeof(*items));
  if (!items) {
    fprintf(stderr, "error: out of memory\n");
    return 1;
  }
  int failures = 0;
  for (unsigned i = 0; i < count; ++i) {
    items[i].input = args->values[i + 1u];
    if (!batch_path(directory, items[i].input, &items[i].output)) {
      ++failures;
      continue;
    }
    for (unsigned previous = 0; previous < i; ++previous) {
      if (items[previous].output &&
          strcmp(items[previous].output, items[i].output) == 0) {
        fprintf(stderr, "error: duplicate batch output %s\n",
                items[i].output);
        ++failures;
        free(items[i].output);
        items[i].output = NULL;
        break;
      }
    }
  }
  unsigned jobs = args->threads < count ? args->threads : count;
  if (!jobs)
    jobs = 1u;
  unsigned inner = args->threads / jobs;
  unsigned extra = args->threads % jobs;
  unsigned active = 0;
  for (unsigned i = 0; i < count; ++i) {
    if (!items[i].output)
      continue;
    items[i].threads = inner + (active++ < extra);
  }
  qlic_parallel_for(count, jobs, batch_item, items);
  for (unsigned i = 0; i < count; ++i) {
    if (items[i].output && !items[i].ok)
      ++failures;
    free(items[i].output);
  }
  free(items);
  if (failures)
    fprintf(stderr, "error: %d batch item%s failed\n", failures,
            failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}

static int command_info(const char *path) {
  uint8_t *data = NULL;
  size_t size = 0;
  if (!read_file(path, &data, &size))
    return 1;
  qlic_info info;
  memset(&info, 0, sizeof(info));
  int status = qlic_get_info(data, size, NULL, &info);
  free(data);
  if (status != QLIC_OK) {
    fprintf(stderr, "error: %s\n", qlic_last_error());
    return 1;
  }
  printf("%ux%u frames=%u animated=%u bytes=%zu\n",
         info.width, info.height, info.frame_count, info.animated, size);
  return 0;
}

static void usage(void) {
  printf("QLIC %s\n", qlic_version());
  puts("qlic pack input output.qlic [--threads N|all]");
  puts("qlic batch output-directory input... [--threads N|all]");
  puts("qlic unpack input.qlic output.png [--threads N|all]");
  puts("qlic info input.qlic");
  puts("qlic version");
}

int main(int argc, char **argv) {
  if (argc < 2 || strcmp(argv[1], "--help") == 0 ||
      strcmp(argv[1], "-h") == 0) {
    usage();
    return 0;
  }
  if (strcmp(argv[1], "version") == 0 ||
      strcmp(argv[1], "--version") == 0) {
    if (argc != 2) {
      fprintf(stderr, "error: version does not accept arguments\n");
      return 2;
    }
    printf("QLIC %s\n", qlic_version());
    return 0;
  }
  Arguments args;
  memset(&args, 0, sizeof(args));
  if (!arguments(argc, argv, &args))
    return 2;
  int result = 2;
  if (strcmp(argv[1], "pack") == 0 && args.count == 2) {
    result = pack_file(args.values[0], args.values[1],
                       args.threads, 0)
                 ? 0
                 : 1;
  } else if (strcmp(argv[1], "batch") == 0) {
    result = command_batch(&args);
  } else if (strcmp(argv[1], "unpack") == 0 && args.count == 2) {
    result = unpack_file(args.values[0], args.values[1],
                         args.threads)
                 ? 0
                 : 1;
  } else if (strcmp(argv[1], "info") == 0 && args.count == 1 &&
             args.threads == 1u) {
    result = command_info(args.values[0]);
  } else {
    fprintf(stderr, "error: invalid command or arguments\n");
  }
  free(args.values);
  return result;
}
