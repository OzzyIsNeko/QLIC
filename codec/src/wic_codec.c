#include <qlic/qlic.h>
#include "qlic_core.h"
#include "qlic_version.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <softpub.h>
#include <shlobj.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strsafe.h>
#include <wchar.h>
#include <wincodec.h>
#include <wincodecsdk.h>
#include <wintrust.h>

static const GUID CLSID_QlicWicDecoder = {
    0x5ce9f7d8,
    0x140b,
    0x43fc,
    {0x87, 0x62, 0xb8, 0xe7, 0x2f, 0xf6, 0xb7, 0x65}};
static const GUID GUID_ContainerFormatQlic = {
    0x1f87e8dd,
    0x31bf,
    0x4ba7,
    {0xab, 0xdd, 0xf1, 0x32, 0xb3, 0xfb, 0x97, 0xb2}};

#define QLIC_CLSID_TEXT L"{5CE9F7D8-140B-43FC-8762-B8E72FF6B765}"
#define QLIC_CONTAINER_TEXT L"{1F87E8DD-31BF-4BA7-ABDD-F132B3FB97B2}"
#define QLIC_VENDOR_TEXT L"{D33A40F2-32BB-469A-9C8B-99615E7B34E4}"
#define WIC_DECODER_CAT_TEXT L"{7ED96837-96F0-4812-B211-F13C24117ED3}"
#define WIC_THUMBNAIL_IID_TEXT L"{E357FCCD-A995-4576-B01F-234630154E96}"
#define WIC_THUMBNAIL_PROVIDER_TEXT L"{C7657C4A-9F68-40FA-A4DF-96BC08EB3551}"
#define SHELL_IMAGE_PREVIEW_TEXT L"{FFE2A43C-56B9-4BF5-9A79-CC6D4285608A}"
#define PHOTOS_APP_PROGID L"AppX4mntx4h978m1v9gtzv0ewksfd6pmwsre"

typedef struct QlicDecoder QlicDecoder;
typedef struct QlicFrame QlicFrame;

struct QlicDecoder {
  IWICBitmapDecoder iface;
  volatile LONG refs;
  SRWLOCK lock;
  qlic_animation anim;
  qlic_wide_image wide;
  qlic_hdr_image hdr;
  int is_wide;
  int is_hdr;
  int ready;
};

struct QlicFrame {
  IWICBitmapFrameDecode iface;
  IWICMetadataBlockReader metadata_iface;
  volatile LONG refs;
  QlicDecoder *dec;
  UINT index;
};

typedef struct {
  IEnumUnknown iface;
  volatile LONG refs;
  QlicFrame *frame;
  UINT current;
} QlicMetadataEnum;

typedef struct {
  IClassFactory iface;
  volatile LONG refs;
} QlicFactory;

static HMODULE g_mod;
static volatile LONG g_refs;
static volatile LONG g_locks;
/* WIC can decode during shell browsing, keep its limits below the library defaults */
static const qlic_decode_limits g_limits = {
    sizeof(qlic_decode_limits),
    0,
    UINT64_C(268435456),
    UINT64_C(268435456),
    UINT64_C(33554432),
    UINT64_C(268435456),
    512u,
    0};
static const qlic_decode_limits_v2 g_limits_v2 = {
    sizeof(qlic_decode_limits_v2),
    0,
    UINT64_C(268435456),
    UINT64_C(268435456),
    UINT64_C(33554432),
    UINT64_C(268435456),
    UINT64_C(268435456),
    UINT64_C(16777216),
    512u,
    256u,
    {0, 0}};

static int hdr10_compatible(const qlic_hdr_image *image) {
  return image && image->bits_per_sample == 10u && image->channels == 3u &&
         image->has_cicp &&
         (image->color_authority == QLIC_COLOR_CICP ||
          image->color_authority == QLIC_COLOR_CICP_PREFERRED) &&
         image->cicp.color_primaries == QLIC_CICP_PRIMARIES_BT2020 &&
         image->cicp.transfer_characteristics == QLIC_CICP_TRANSFER_PQ &&
         image->cicp.matrix_coefficients == QLIC_CICP_MATRIX_RGB &&
         image->cicp.full_range == 1u;
}

static int hdr10_info_compatible(const HdrInfo *info) {
  qlic_hdr_image image = {0};
  image.bits_per_sample = info->bits_per_sample;
  image.channels = info->channels;
  image.color_authority = info->color_authority;
  image.has_cicp = info->has_cicp;
  image.cicp.color_primaries = info->color_primaries;
  image.cicp.transfer_characteristics = info->transfer_characteristics;
  image.cicp.matrix_coefficients = info->matrix_coefficients;
  image.cicp.full_range = info->full_range;
  return hdr10_compatible(&image);
}

static IWICBitmapDecoderVtbl g_decoder_vtbl;
static IWICBitmapFrameDecodeVtbl g_frame_vtbl;
static IWICMetadataBlockReaderVtbl g_metadata_block_vtbl;
static IEnumUnknownVtbl g_metadata_enum_vtbl;
static IClassFactoryVtbl g_factory_vtbl;
static QlicFactory g_factory = {{&g_factory_vtbl}, 1};

static HRESULT q_hr(int status) {
  if (status == QLIC_OUT_OF_MEMORY)
    return E_OUTOFMEMORY;
  if (status == QLIC_LIMIT_EXCEEDED)
    return WINCODEC_ERR_VALUEOUTOFRANGE;
  if (status == QLIC_BAD_DATA || status == QLIC_ERROR)
    return WINCODEC_ERR_BADIMAGE;
  if (status == QLIC_UNSUPPORTED_FORMAT)
    return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
  return WINCODEC_ERR_COMPONENTINITIALIZEFAILURE;
}

static HRESULT component_factory(IWICComponentFactory **factory) {
  if (!factory)
    return E_POINTER;
  *factory = NULL;
  return CoCreateInstance(&CLSID_WICImagingFactory, NULL,
                          CLSCTX_INPROC_SERVER, &IID_IWICComponentFactory,
                          (void **)factory);
}

static HRESULT memory_stream(const uint8_t *data, size_t size,
                             IStream **stream) {
  if (!stream)
    return E_POINTER;
  *stream = NULL;
  if ((!data && size) || size > ULONG_MAX)
    return E_INVALIDARG;
  HRESULT hr = CreateStreamOnHGlobal(NULL, TRUE, stream);
  if (FAILED(hr))
    return hr;
  if (size) {
    ULONG written = 0;
    hr = (*stream)->lpVtbl->Write(*stream, data, (ULONG)size, &written);
    if (SUCCEEDED(hr) && written != size)
      hr = STG_E_WRITEFAULT;
  }
  if (SUCCEEDED(hr)) {
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    hr = (*stream)->lpVtbl->Seek(*stream, zero, STREAM_SEEK_SET, NULL);
  }
  if (FAILED(hr)) {
    (*stream)->lpVtbl->Release(*stream);
    *stream = NULL;
  }
  return hr;
}

static int tiff_first_ifd(const uint8_t *data, size_t size, size_t *offset,
                          DWORD *persist_options) {
  if (size < 8u || !offset || !persist_options)
    return 0;
  uint32_t value = 0;
  if (!memcmp(data, "II\x2a\0", 4u)) {
    value = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
            ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
    *persist_options = (DWORD)WICPersistOptionLittleEndian;
  } else if (!memcmp(data, "MM\0\x2a", 4u)) {
    value = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
            ((uint32_t)data[6] << 8) | (uint32_t)data[7];
    *persist_options = (DWORD)WICPersistOptionBigEndian;
  } else {
    return 0;
  }
  if (value >= size)
    return 0;
  *offset = (size_t)value;
  return 1;
}

static HRESULT metadata_reader_for_block(const qlic_metadata_block *block,
                                         IWICMetadataReader **reader) {
  if (!reader)
    return E_POINTER;
  *reader = NULL;
  if (!block || (!block->data && block->size))
    return E_INVALIDARG;

  IWICComponentFactory *factory = NULL;
  IStream *stream = NULL;
  HRESULT hr = component_factory(&factory);
  const GUID *format = &GUID_MetadataFormatUnknown;
  const uint8_t *payload = block->data;
  size_t payload_size = block->size;
  size_t stream_offset = 0;
  DWORD persist_options = (DWORD)WICPersistOptionDefault;
  if (!memcmp(block->tag, "XMP_", 4u)) {
    format = &GUID_MetadataFormatXMP;
  } else if (!memcmp(block->tag, "IPTC", 4u)) {
    format = &GUID_MetadataFormatIPTC;
  } else if (!memcmp(block->tag, "EXIF", 4u)) {
    format = &GUID_MetadataFormatExif;
    if (payload_size >= 6u && !memcmp(payload, "Exif\0\0", 6u)) {
      payload += 6u;
      payload_size -= 6u;
    }
    if (tiff_first_ifd(payload, payload_size, &stream_offset,
                       &persist_options))
      format = &GUID_MetadataFormatIfd;
  }
  if (SUCCEEDED(hr))
    hr = memory_stream(payload, payload_size, &stream);
  if (SUCCEEDED(hr) && stream_offset) {
    LARGE_INTEGER position;
    position.QuadPart = (LONGLONG)stream_offset;
    hr = stream->lpVtbl->Seek(stream, position, STREAM_SEEK_SET, NULL);
  }
  if (SUCCEEDED(hr)) {
    DWORD options = persist_options | (DWORD)WICMetadataCreationFailUnknown;
    hr = factory->lpVtbl->CreateMetadataReader(
        factory, format, NULL, options, stream, reader);
    if (SUCCEEDED(hr)) {
      UINT count = 0;
      hr = (*reader)->lpVtbl->GetCount(*reader, &count);
      if (FAILED(hr)) {
        (*reader)->lpVtbl->Release(*reader);
        *reader = NULL;
      }
    }
  }
  if (stream)
    stream->lpVtbl->Release(stream);

  if (FAILED(hr) && factory) {
    stream = NULL;
    hr = memory_stream(block->data, block->size, &stream);
    if (SUCCEEDED(hr))
      hr = factory->lpVtbl->CreateMetadataReader(
          factory, &GUID_MetadataFormatUnknown, NULL,
          (DWORD)WICPersistOptionDefault |
              (DWORD)WICMetadataCreationDefault,
          stream,
          reader);
    if (stream)
      stream->lpVtbl->Release(stream);
  }
  if (factory)
    factory->lpVtbl->Release(factory);
  return hr;
}

static void wic_log(const char *msg) {
  wchar_t path[MAX_PATH];
  if (!GetEnvironmentVariableW(L"QLIC_WIC_LOG", path, MAX_PATH))
    return;
  HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE)
    return;
  DWORD wr = 0;
  WriteFile(f, msg, (DWORD)strlen(msg), &wr, NULL);
  WriteFile(f, "\r\n", 2, &wr, NULL);
  CloseHandle(f);
}

static HRESULT read_stream(IStream *s, uint8_t **out, size_t *outn) {
  *out = NULL;
  *outn = 0;
  if (!s)
    return E_INVALIDARG;

  LARGE_INTEGER z;
  ULARGE_INTEGER end;
  z.QuadPart = 0;
  HRESULT hr = s->lpVtbl->Seek(s, z, STREAM_SEEK_SET, NULL);
  if (FAILED(hr)) {
    char line[128];
    snprintf(line, sizeof(line), "seek start failed: 0x%08lx",
             (unsigned long)hr);
    wic_log(line);
    return hr;
  }
  hr = s->lpVtbl->Seek(s, z, STREAM_SEEK_END, &end);
  if (FAILED(hr)) {
    char line[128];
    snprintf(line, sizeof(line), "seek end failed: 0x%08lx", (unsigned long)hr);
    wic_log(line);
    return hr;
  }
  if (end.QuadPart > g_limits.max_file_bytes)
    return WINCODEC_ERR_VALUEOUTOFRANGE;
  if (end.QuadPart > SIZE_MAX)
    return E_OUTOFMEMORY;
  hr = s->lpVtbl->Seek(s, z, STREAM_SEEK_SET, NULL);
  if (FAILED(hr)) {
    char line[128];
    snprintf(line, sizeof(line), "seek rewind failed: 0x%08lx",
             (unsigned long)hr);
    wic_log(line);
    return hr;
  }

  size_t n = (size_t)end.QuadPart;
  uint8_t *p = (uint8_t *)malloc(n ? n : 1);
  if (!p)
    return E_OUTOFMEMORY;
  size_t pos = 0;
  unsigned read_calls = 0;
  while (pos < n) {
    if (++read_calls > 4096u) {
      wic_log("stream read work budget exceeded");
      free(p);
      return WINCODEC_ERR_BADIMAGE;
    }
    ULONG want = (ULONG)((n - pos) > 0x100000u ? 0x100000u : (n - pos));
    ULONG got = 0;
    hr = s->lpVtbl->Read(s, p + pos, want, &got);
    if (FAILED(hr)) {
      char line[128];
      snprintf(line, sizeof(line), "stream read failed: 0x%08lx",
               (unsigned long)hr);
      wic_log(line);
      free(p);
      return hr;
    }
    if (!got) {
      wic_log("stream read truncated");
      free(p);
      return WINCODEC_ERR_BADIMAGE;
    }
    if (got > want) {
      wic_log("stream read exceeded request");
      free(p);
      return WINCODEC_ERR_BADIMAGE;
    }
    pos += got;
  }
  *out = p;
  *outn = n;
  return S_OK;
}

static HRESULT dec_load(QlicDecoder *d, IStream *s) {
  if (d->ready)
    return WINCODEC_ERR_WRONGSTATE;
  wic_log("Initialize entered");
  uint8_t *data = NULL;
  size_t n = 0;
  HRESULT hr = read_stream(s, &data, &n);
  if (FAILED(hr))
    return hr;
  qlic_info_v2 info = {0};
  info.struct_size = sizeof(info);
  int result = qlic_get_info_v2(data, n, &g_limits_v2, &info);
  int is_hdr = result == QLIC_OK && n > 12u && data[12] == 20u;
  if (is_hdr) {
    if (info.bits_per_sample != 8u && info.bits_per_sample != 16u &&
        !(info.bits_per_sample == 10u && info.channels == 3u &&
          info.has_cicp)) {
      result = QLIC_UNSUPPORTED_FORMAT;
    } else {
      d->hdr.struct_size = sizeof(d->hdr);
      result = qlic_decode_hdr(data, n, &g_limits_v2, &d->hdr);
      if (result == QLIC_OK && info.bits_per_sample == 10u &&
          !hdr10_compatible(&d->hdr)) {
        qlic_hdr_image_free(&d->hdr);
        result = QLIC_UNSUPPORTED_FORMAT;
      } else if (result == QLIC_OK) {
        d->is_hdr = 1;
      }
    }
  } else if (result == QLIC_OK && info.bits_per_sample > 8u) {
    if (info.bits_per_sample != 16u) {
      result = QLIC_UNSUPPORTED_FORMAT;
    } else {
      result = qlic_decode_wide(data, n, &g_limits, &d->wide);
      if (result == QLIC_OK)
        d->is_wide = 1;
    }
  } else if (result == QLIC_OK) {
    result =
        qlic_decode_animation(data, n, &g_limits, &d->anim);
  }
  free(data);
  if (result != QLIC_OK) {
    char line[1200];
    snprintf(line, sizeof(line), "dec_qlic failed: bytes=%zu err=%s", n,
             qlic_last_error());
    wic_log(line);
    return q_hr(result);
  }
  d->ready = 1;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE dec_qi(IWICBitmapDecoder *This, REFIID riid,
                                        void **ppv) {
  if (!ppv)
    return E_POINTER;
  *ppv = NULL;
  if (IsEqualIID(riid, &IID_IUnknown) ||
      IsEqualIID(riid, &IID_IWICBitmapDecoder)) {
    *ppv = This;
    This->lpVtbl->AddRef(This);
    return S_OK;
  }
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dec_add(IWICBitmapDecoder *This) {
  QlicDecoder *d = (QlicDecoder *)This;
  return (ULONG)InterlockedIncrement(&d->refs);
}

static ULONG STDMETHODCALLTYPE dec_rel(IWICBitmapDecoder *This) {
  QlicDecoder *d = (QlicDecoder *)This;
  LONG r = InterlockedDecrement(&d->refs);
  if (!r) {
    qlic_animation_free(&d->anim);
    qlic_wide_image_free(&d->wide);
    qlic_hdr_image_free(&d->hdr);
    InterlockedDecrement(&g_refs);
    free(d);
  }
  return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE dec_cap(IWICBitmapDecoder *This, IStream *s,
                                         DWORD *cap) {
  (void)This;
  if (!cap)
    return E_POINTER;
  *cap = 0;
  if (!s)
    return E_INVALIDARG;

  LARGE_INTEGER zero;
  ULARGE_INTEGER old;
  zero.QuadPart = 0;
  if (FAILED(s->lpVtbl->Seek(s, zero, STREAM_SEEK_CUR, &old))) {
    wic_log("QueryCapability stream is not seekable");
    return S_OK;
  }

  uint8_t *data = NULL;
  size_t size = 0;
  HRESULT hr = read_stream(s, &data, &size);
  if (SUCCEEDED(hr)) {
    qlic_info_v2 info = {0};
    info.struct_size = sizeof(info);
    int status = qlic_get_info_v2(data, size, &g_limits_v2, &info);
    int supported = status == QLIC_OK;
    if (supported && size > 12u && data[12] == 20u) {
      supported = (info.bits_per_sample == 8u ||
                   info.bits_per_sample == 16u) &&
                  (info.channels == 1u || info.channels == 3u ||
                   info.channels == 4u);
      if (!supported && info.bits_per_sample == 10u && info.channels == 3u) {
        HdrInfo hdr = {0};
        supported = hdr_qlic_info_limited(data, size, &hdr, NULL, NULL) &&
                    hdr10_info_compatible(&hdr);
      }
    } else if (supported && info.bits_per_sample > 8u) {
      supported = info.bits_per_sample == 16u &&
                  (info.channels == 1u || info.channels == 3u ||
                   info.channels == 4u);
    }
    if (supported) {
      *cap = WICBitmapDecoderCapabilityCanDecodeAllImages;
      if (info.metadata_count)
        *cap |= WICBitmapDecoderCapabilityCanEnumerateMetadata;
    }
  }
  free(data);

  LARGE_INTEGER back;
  back.QuadPart = (LONGLONG)old.QuadPart;
  if (FAILED(s->lpVtbl->Seek(s, back, STREAM_SEEK_SET, NULL)))
    *cap = 0;
  wic_log(*cap ? "QueryCapability match" : "QueryCapability no match");
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE dec_init(IWICBitmapDecoder *This, IStream *s,
                                          WICDecodeOptions opt) {
  if (!s)
    return E_INVALIDARG;
  if (opt != WICDecodeMetadataCacheOnDemand &&
      opt != WICDecodeMetadataCacheOnLoad)
    return E_INVALIDARG;
  QlicDecoder *d = (QlicDecoder *)This;
  AcquireSRWLockExclusive(&d->lock);
  HRESULT hr = dec_load(d, s);
  ReleaseSRWLockExclusive(&d->lock);
  return hr;
}

static HRESULT STDMETHODCALLTYPE dec_container(IWICBitmapDecoder *This,
                                               GUID *fmt) {
  (void)This;
  if (!fmt)
    return E_POINTER;
  *fmt = GUID_ContainerFormatQlic;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE dec_info(IWICBitmapDecoder *This,
                                          IWICBitmapDecoderInfo **info) {
  (void)This;
  if (!info)
    return E_POINTER;
  *info = NULL;
  IWICImagingFactory *factory = NULL;
  IWICComponentInfo *component = NULL;
  HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL,
                                CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void **)&factory);
  if (SUCCEEDED(hr))
    hr = factory->lpVtbl->CreateComponentInfo(
        factory, &CLSID_QlicWicDecoder, &component);
  if (SUCCEEDED(hr))
    hr = component->lpVtbl->QueryInterface(
        component, &IID_IWICBitmapDecoderInfo, (void **)info);
  if (component)
    component->lpVtbl->Release(component);
  if (factory)
    factory->lpVtbl->Release(factory);
  return hr;
}

static HRESULT STDMETHODCALLTYPE dec_palette(IWICBitmapDecoder *This,
                                             IWICPalette *pal) {
  (void)This;
  (void)pal;
  return WINCODEC_ERR_PALETTEUNAVAILABLE;
}

static HRESULT STDMETHODCALLTYPE dec_meta(IWICBitmapDecoder *This,
                                          IWICMetadataQueryReader **r) {
  (void)This;
  if (!r)
    return E_POINTER;
  *r = NULL;
  return WINCODEC_ERR_UNSUPPORTEDOPERATION;
}

static HRESULT STDMETHODCALLTYPE dec_preview(IWICBitmapDecoder *This,
                                             IWICBitmapSource **src) {
  (void)This;
  if (!src)
    return E_POINTER;
  *src = NULL;
  return WINCODEC_ERR_UNSUPPORTEDOPERATION;
}

static HRESULT STDMETHODCALLTYPE dec_contexts(IWICBitmapDecoder *This, UINT c,
                                              IWICColorContext **ctx,
                                              UINT *actual) {
  if (!actual)
    return E_POINTER;
  *actual = 0;
  IWICBitmapFrameDecode *frame = NULL;
  HRESULT hr = This->lpVtbl->GetFrame(This, 0, &frame);
  if (SUCCEEDED(hr))
    hr = frame->lpVtbl->GetColorContexts(frame, c, ctx, actual);
  if (frame)
    frame->lpVtbl->Release(frame);
  return hr;
}

static HRESULT STDMETHODCALLTYPE dec_thumb(IWICBitmapDecoder *This,
                                           IWICBitmapSource **src) {
  (void)This;
  if (!src)
    return E_POINTER;
  *src = NULL;
  return WINCODEC_ERR_CODECNOTHUMBNAIL;
}

static HRESULT STDMETHODCALLTYPE dec_count(IWICBitmapDecoder *This,
                                           UINT *count) {
  QlicDecoder *d = (QlicDecoder *)This;
  if (!count)
    return E_POINTER;
  *count = 0;
  AcquireSRWLockShared(&d->lock);
  HRESULT hr = S_OK;
  if (!d->ready)
    hr = WINCODEC_ERR_NOTINITIALIZED;
  else
    *count = d->is_wide || d->is_hdr ? 1u : d->anim.frame_count;
  ReleaseSRWLockShared(&d->lock);
  return hr;
}

static HRESULT STDMETHODCALLTYPE dec_frame(IWICBitmapDecoder *This, UINT index,
                                           IWICBitmapFrameDecode **frame) {
  QlicDecoder *d = (QlicDecoder *)This;
  if (!frame)
    return E_POINTER;
  *frame = NULL;
  AcquireSRWLockShared(&d->lock);
  if (!d->ready) {
    ReleaseSRWLockShared(&d->lock);
    return WINCODEC_ERR_NOTINITIALIZED;
  }
  UINT count = d->is_wide || d->is_hdr ? 1u : d->anim.frame_count;
  if (index >= count) {
    ReleaseSRWLockShared(&d->lock);
    return WINCODEC_ERR_FRAMEMISSING;
  }
  QlicFrame *f = (QlicFrame *)calloc(1, sizeof(*f));
  if (!f) {
    ReleaseSRWLockShared(&d->lock);
    return E_OUTOFMEMORY;
  }
  f->iface.lpVtbl = &g_frame_vtbl;
  f->metadata_iface.lpVtbl = &g_metadata_block_vtbl;
  f->refs = 1;
  f->dec = d;
  f->index = index;
  This->lpVtbl->AddRef(This);
  ReleaseSRWLockShared(&d->lock);
  InterlockedIncrement(&g_refs);
  *frame = &f->iface;
  return S_OK;
}

static IWICBitmapDecoderVtbl g_decoder_vtbl = {
    dec_qi,        dec_add,   dec_rel,     dec_cap,  dec_init,
    dec_container, dec_info,  dec_palette, dec_meta, dec_preview,
    dec_contexts,  dec_thumb, dec_count,   dec_frame};

static HRESULT STDMETHODCALLTYPE fr_qi(IWICBitmapFrameDecode *This, REFIID riid,
                                       void **ppv) {
  if (!ppv)
    return E_POINTER;
  *ppv = NULL;
  if (IsEqualIID(riid, &IID_IUnknown) ||
      IsEqualIID(riid, &IID_IWICBitmapSource) ||
      IsEqualIID(riid, &IID_IWICBitmapFrameDecode)) {
    *ppv = This;
  } else if (IsEqualIID(riid, &IID_IWICMetadataBlockReader)) {
    QlicFrame *f = (QlicFrame *)This;
    *ppv = &f->metadata_iface;
  } else {
    return E_NOINTERFACE;
  }
  This->lpVtbl->AddRef(This);
  return S_OK;
}

static QlicFrame *frame_from_metadata(IWICMetadataBlockReader *This) {
  return CONTAINING_RECORD(This, QlicFrame, metadata_iface);
}

static HRESULT STDMETHODCALLTYPE mbr_qi(IWICMetadataBlockReader *This,
                                        REFIID riid, void **ppv) {
  QlicFrame *f = frame_from_metadata(This);
  return f->iface.lpVtbl->QueryInterface(&f->iface, riid, ppv);
}

static ULONG STDMETHODCALLTYPE mbr_add(IWICMetadataBlockReader *This) {
  QlicFrame *f = frame_from_metadata(This);
  return f->iface.lpVtbl->AddRef(&f->iface);
}

static ULONG STDMETHODCALLTYPE mbr_rel(IWICMetadataBlockReader *This) {
  QlicFrame *f = frame_from_metadata(This);
  return f->iface.lpVtbl->Release(&f->iface);
}

static UINT frame_metadata_count(const QlicFrame *f) {
  return f->dec->is_hdr ? f->dec->hdr.metadata_count : 0u;
}

static HRESULT STDMETHODCALLTYPE mbr_container(IWICMetadataBlockReader *This,
                                               GUID *format) {
  (void)This;
  if (!format)
    return E_POINTER;
  *format = GUID_ContainerFormatQlic;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE mbr_count(IWICMetadataBlockReader *This,
                                           UINT *count) {
  if (!count)
    return E_POINTER;
  QlicFrame *f = frame_from_metadata(This);
  *count = frame_metadata_count(f);
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE mbr_reader(IWICMetadataBlockReader *This,
                                            UINT index,
                                            IWICMetadataReader **reader) {
  if (!reader)
    return E_POINTER;
  *reader = NULL;
  QlicFrame *f = frame_from_metadata(This);
  if (index >= frame_metadata_count(f))
    return E_INVALIDARG;
  return metadata_reader_for_block(f->dec->hdr.metadata + index, reader);
}

static HRESULT STDMETHODCALLTYPE menum_qi(IEnumUnknown *This, REFIID riid,
                                          void **ppv) {
  if (!ppv)
    return E_POINTER;
  *ppv = NULL;
  if (!IsEqualIID(riid, &IID_IUnknown) &&
      !IsEqualIID(riid, &IID_IEnumUnknown))
    return E_NOINTERFACE;
  *ppv = This;
  This->lpVtbl->AddRef(This);
  return S_OK;
}

static ULONG STDMETHODCALLTYPE menum_add(IEnumUnknown *This) {
  QlicMetadataEnum *e = (QlicMetadataEnum *)This;
  return (ULONG)InterlockedIncrement(&e->refs);
}

static ULONG STDMETHODCALLTYPE menum_rel(IEnumUnknown *This) {
  QlicMetadataEnum *e = (QlicMetadataEnum *)This;
  LONG refs = InterlockedDecrement(&e->refs);
  if (!refs) {
    e->frame->iface.lpVtbl->Release(&e->frame->iface);
    InterlockedDecrement(&g_refs);
    free(e);
  }
  return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE menum_next(IEnumUnknown *This, ULONG count,
                                            IUnknown **items,
                                            ULONG *fetched) {
  if (!items || (count != 1u && !fetched))
    return E_POINTER;
  if (fetched)
    *fetched = 0;
  for (ULONG index = 0; index < count; ++index)
    items[index] = NULL;
  QlicMetadataEnum *e = (QlicMetadataEnum *)This;
  ULONG done = 0;
  UINT total = frame_metadata_count(e->frame);
  while (done < count && e->current < total) {
    IWICMetadataReader *reader = NULL;
    HRESULT hr = e->frame->metadata_iface.lpVtbl->GetReaderByIndex(
        &e->frame->metadata_iface, e->current, &reader);
    if (FAILED(hr)) {
      for (ULONG index = 0; index < done; ++index) {
        items[index]->lpVtbl->Release(items[index]);
        items[index] = NULL;
      }
      return hr;
    }
    items[done++] = (IUnknown *)reader;
    ++e->current;
  }
  if (fetched)
    *fetched = done;
  return done == count ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE menum_skip(IEnumUnknown *This, ULONG count) {
  QlicMetadataEnum *e = (QlicMetadataEnum *)This;
  UINT total = frame_metadata_count(e->frame);
  uint64_t next = (uint64_t)e->current + count;
  if (next > total) {
    e->current = total;
    return S_FALSE;
  }
  e->current = (UINT)next;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE menum_reset(IEnumUnknown *This) {
  QlicMetadataEnum *e = (QlicMetadataEnum *)This;
  e->current = 0;
  return S_OK;
}

static HRESULT metadata_enum_new(QlicFrame *frame, UINT current,
                                 IEnumUnknown **result) {
  if (!result)
    return E_POINTER;
  *result = NULL;
  QlicMetadataEnum *e = (QlicMetadataEnum *)calloc(1u, sizeof(*e));
  if (!e)
    return E_OUTOFMEMORY;
  e->iface.lpVtbl = &g_metadata_enum_vtbl;
  e->refs = 1;
  e->frame = frame;
  e->current = current;
  frame->iface.lpVtbl->AddRef(&frame->iface);
  InterlockedIncrement(&g_refs);
  *result = &e->iface;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE menum_clone(IEnumUnknown *This,
                                             IEnumUnknown **clone) {
  QlicMetadataEnum *e = (QlicMetadataEnum *)This;
  return metadata_enum_new(e->frame, e->current, clone);
}

static HRESULT STDMETHODCALLTYPE mbr_enum(IWICMetadataBlockReader *This,
                                          IEnumUnknown **result) {
  return metadata_enum_new(frame_from_metadata(This), 0u, result);
}

static IWICMetadataBlockReaderVtbl g_metadata_block_vtbl = {
    mbr_qi, mbr_add, mbr_rel, mbr_container, mbr_count, mbr_reader, mbr_enum};

static IEnumUnknownVtbl g_metadata_enum_vtbl = {
    menum_qi, menum_add, menum_rel, menum_next,
    menum_skip, menum_reset, menum_clone};

static ULONG STDMETHODCALLTYPE fr_add(IWICBitmapFrameDecode *This) {
  QlicFrame *f = (QlicFrame *)This;
  return (ULONG)InterlockedIncrement(&f->refs);
}

static ULONG STDMETHODCALLTYPE fr_rel(IWICBitmapFrameDecode *This) {
  QlicFrame *f = (QlicFrame *)This;
  LONG r = InterlockedDecrement(&f->refs);
  if (!r) {
    f->dec->iface.lpVtbl->Release(&f->dec->iface);
    InterlockedDecrement(&g_refs);
    free(f);
  }
  return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE fr_size(IWICBitmapFrameDecode *This, UINT *w,
                                         UINT *h) {
  QlicFrame *f = (QlicFrame *)This;
  if (!w || !h)
    return E_POINTER;
  if (f->dec->is_hdr) {
    *w = f->dec->hdr.width;
    *h = f->dec->hdr.height;
  } else if (f->dec->is_wide) {
    *w = f->dec->wide.width;
    *h = f->dec->wide.height;
  } else {
    qlic_image *im = &f->dec->anim.frames[f->index].image;
    *w = im->width;
    *h = im->height;
  }
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE fr_pf(IWICBitmapFrameDecode *This,
                                       WICPixelFormatGUID *pf) {
  if (!pf)
    return E_POINTER;
  QlicFrame *f = (QlicFrame *)This;
  if (f->dec->is_hdr) {
    qlic_hdr_image *im = &f->dec->hdr;
    if (im->bits_per_sample == 8u) {
      if (im->channels == 1u)
        *pf = GUID_WICPixelFormat8bppGray;
      else if (im->channels == 3u)
        *pf = GUID_WICPixelFormat24bppRGB;
      else if (im->channels == 4u)
        *pf = im->alpha_mode == QLIC_ALPHA_PREMULTIPLIED
                  ? GUID_WICPixelFormat32bppPRGBA
                  : GUID_WICPixelFormat32bppRGBA;
      else
        return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    } else if (im->bits_per_sample == 16u) {
      if (im->channels == 1u)
        *pf = GUID_WICPixelFormat16bppGray;
      else if (im->channels == 3u)
        *pf = GUID_WICPixelFormat48bppRGB;
      else if (im->channels == 4u)
        *pf = im->alpha_mode == QLIC_ALPHA_PREMULTIPLIED
                  ? GUID_WICPixelFormat64bppPRGBA
                  : GUID_WICPixelFormat64bppRGBA;
      else
        return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    } else if (hdr10_compatible(im)) {
      *pf = GUID_WICPixelFormat32bppR10G10B10A2HDR10;
    } else {
      return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    }
  } else if (!f->dec->is_wide) {
    *pf = GUID_WICPixelFormat32bppRGBA;
  } else if (f->dec->wide.channels == 1u) {
    *pf = GUID_WICPixelFormat16bppGray;
  } else if (f->dec->wide.channels == 3u) {
    *pf = GUID_WICPixelFormat48bppRGB;
  } else if (f->dec->wide.channels == 4u) {
    *pf = GUID_WICPixelFormat64bppRGBA;
  } else {
    return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
  }
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE fr_res(IWICBitmapFrameDecode *This, double *x,
                                        double *y) {
  if (!x || !y)
    return E_POINTER;
  *x = 96.0;
  *y = 96.0;
  QlicFrame *f = (QlicFrame *)This;
  if (f->dec->is_hdr) {
    for (uint32_t index = 0; index < f->dec->hdr.metadata_count; ++index) {
      const qlic_metadata_block *block = f->dec->hdr.metadata + index;
      if (!memcmp(block->tag, "pHYs", 4u) && block->size == 9u &&
          block->data[8] == 1u) {
        uint32_t px = ((uint32_t)block->data[0] << 24) |
                      ((uint32_t)block->data[1] << 16) |
                      ((uint32_t)block->data[2] << 8) | block->data[3];
        uint32_t py = ((uint32_t)block->data[4] << 24) |
                      ((uint32_t)block->data[5] << 16) |
                      ((uint32_t)block->data[6] << 8) | block->data[7];
        if (px)
          *x = (double)px * 0.0254;
        if (py)
          *y = (double)py * 0.0254;
        break;
      }
    }
  }
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE fr_palette(IWICBitmapFrameDecode *This,
                                            IWICPalette *pal) {
  (void)This;
  (void)pal;
  return WINCODEC_ERR_PALETTEUNAVAILABLE;
}

static HRESULT STDMETHODCALLTYPE fr_pixels(IWICBitmapFrameDecode *This,
                                           const WICRect *rc, UINT stride,
                                           UINT bufn, BYTE *buf) {
  QlicFrame *f = (QlicFrame *)This;
  if (!buf)
    return E_POINTER;
  const uint8_t *pixels;
  size_t pixels_size;
  size_t source_stride;
  size_t pixel_bytes;
  UINT iw;
  UINT ih;
  int hdr8 = 0;
  int hdr10 = 0;
  if (f->dec->is_hdr) {
    qlic_hdr_image *im = &f->dec->hdr;
    if ((im->bits_per_sample != 8u && im->bits_per_sample != 16u &&
         !hdr10_compatible(im)) ||
        (im->channels != 1u && im->channels != 3u && im->channels != 4u))
      return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    pixels = (const uint8_t *)im->pixels;
    pixels_size = im->pixels_size;
    source_stride = im->stride;
    hdr8 = im->bits_per_sample == 8u;
    hdr10 = hdr10_compatible(im);
    pixel_bytes = hdr10 ? sizeof(uint32_t)
                        : (size_t)im->channels *
                              (hdr8 ? 1u : sizeof(uint16_t));
    iw = im->width;
    ih = im->height;
  } else if (f->dec->is_wide) {
    qlic_wide_image *im = &f->dec->wide;
    if (im->bits_per_sample != 16u ||
        (im->channels != 1u && im->channels != 3u && im->channels != 4u))
      return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    pixels = (const uint8_t *)im->pixels;
    pixels_size = im->pixels_size;
    source_stride = im->stride;
    pixel_bytes = (size_t)im->channels * sizeof(uint16_t);
    iw = im->width;
    ih = im->height;
  } else {
    qlic_image *im = &f->dec->anim.frames[f->index].image;
    pixels = im->rgba;
    pixels_size = im->rgba_size;
    source_stride = im->stride;
    pixel_bytes = 4u;
    iw = im->width;
    ih = im->height;
  }
  if (iw > INT_MAX || ih > INT_MAX)
    return WINCODEC_ERR_VALUEOUTOFRANGE;
  INT x = 0, y = 0, w = (INT)iw, h = (INT)ih;
  if (rc) {
    x = rc->X;
    y = rc->Y;
    w = rc->Width;
    h = rc->Height;
  }
  if (x < 0 || y < 0 || w < 0 || h < 0 || (UINT)x > iw || (UINT)y > ih ||
      (UINT)w > iw - (UINT)x || (UINT)h > ih - (UINT)y)
    return E_INVALIDARG;
  /* WIC uses UINT at the edge but these intermediate products can be larger */
  uint64_t row = (uint64_t)(UINT)w * pixel_bytes;
  if (row > UINT_MAX || stride < row)
    return E_INVALIDARG;
  uint64_t required =
      h ? (uint64_t)stride * ((UINT)h - 1u) + row : 0u;
  if (required > UINT_MAX || required > bufn)
    return E_INVALIDARG;
  size_t source_pixel_bytes =
      hdr8 ? pixel_bytes * sizeof(uint16_t)
            : hdr10 ? 3u * sizeof(uint16_t) : pixel_bytes;
  uint64_t source_offset = (uint64_t)(UINT)y * source_stride +
                           (uint64_t)(UINT)x * source_pixel_bytes;
  uint64_t source_required =
      h ? source_offset + (uint64_t)((UINT)h - 1u) * source_stride +
              (uint64_t)(UINT)w * source_pixel_bytes
        : source_offset;
  if (source_stride < (uint64_t)iw * source_pixel_bytes ||
      source_offset > SIZE_MAX ||
      source_stride > SIZE_MAX || source_required < source_offset ||
      source_required > pixels_size)
    return WINCODEC_ERR_BADIMAGE;
  const uint8_t *src = pixels + (size_t)source_offset;
  for (UINT yy = 0; yy < (UINT)h; ++yy) {
    const uint8_t *source_row = src + (size_t)yy * (size_t)source_stride;
    uint8_t *destination_row = buf + (size_t)yy * stride;
    if (hdr8) {
      const uint16_t *samples = (const uint16_t *)source_row;
      size_t count = (size_t)(UINT)w * f->dec->hdr.channels;
      for (size_t sample = 0; sample < count; ++sample)
        destination_row[sample] = (uint8_t)samples[sample];
    } else if (hdr10) {
      const uint16_t *samples = (const uint16_t *)source_row;
      for (UINT xx = 0; xx < (UINT)w; ++xx) {
        uint32_t packed = ((uint32_t)samples[(size_t)xx * 3u] << 22) |
                          ((uint32_t)samples[(size_t)xx * 3u + 1u] << 12) |
                          ((uint32_t)samples[(size_t)xx * 3u + 2u] << 2) | 3u;
        memcpy(destination_row + (size_t)xx * sizeof(packed), &packed,
               sizeof(packed));
      }
    } else {
      memcpy(destination_row, source_row, (size_t)row);
    }
  }
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE fr_meta(IWICBitmapFrameDecode *This,
                                         IWICMetadataQueryReader **r) {
  if (!r)
    return E_POINTER;
  *r = NULL;
  QlicFrame *f = (QlicFrame *)This;
  if (!frame_metadata_count(f))
    return WINCODEC_ERR_UNSUPPORTEDOPERATION;
  IWICComponentFactory *factory = NULL;
  HRESULT hr = component_factory(&factory);
  if (SUCCEEDED(hr))
    hr = factory->lpVtbl->CreateQueryReaderFromBlockReader(
        factory, &f->metadata_iface, r);
  if (factory)
    factory->lpVtbl->Release(factory);
  return hr;
}

static HRESULT STDMETHODCALLTYPE fr_contexts(IWICBitmapFrameDecode *This,
                                             UINT c, IWICColorContext **ctx,
                                             UINT *actual) {
  if (!actual)
    return E_POINTER;
  QlicFrame *f = (QlicFrame *)This;
  if (!f->dec->is_hdr || !f->dec->hdr.icc_size) {
    *actual = 0;
    return S_OK;
  }
  *actual = 1;
  if (!c)
    return S_OK;
  if (!ctx)
    return E_POINTER;
  ctx[0] = NULL;
  IWICImagingFactory *factory = NULL;
  IWICColorContext *color = NULL;
  HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL,
                                CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void **)&factory);
  if (SUCCEEDED(hr))
    hr = factory->lpVtbl->CreateColorContext(factory, &color);
  if (SUCCEEDED(hr))
    hr = color->lpVtbl->InitializeFromMemory(color, f->dec->hdr.icc,
                                             (UINT)f->dec->hdr.icc_size);
  if (SUCCEEDED(hr))
    ctx[0] = color;
  else if (color)
    color->lpVtbl->Release(color);
  if (factory)
    factory->lpVtbl->Release(factory);
  return hr;
}

static HRESULT STDMETHODCALLTYPE fr_thumb(IWICBitmapFrameDecode *This,
                                          IWICBitmapSource **src) {
  (void)This;
  if (!src)
    return E_POINTER;
  *src = NULL;
  return WINCODEC_ERR_CODECNOTHUMBNAIL;
}

static IWICBitmapFrameDecodeVtbl g_frame_vtbl = {
    fr_qi,      fr_add,    fr_rel,  fr_size,     fr_pf,   fr_res,
    fr_palette, fr_pixels, fr_meta, fr_contexts, fr_thumb};

static HRESULT STDMETHODCALLTYPE fac_qi(IClassFactory *This, REFIID riid,
                                        void **ppv) {
  if (!ppv)
    return E_POINTER;
  *ppv = NULL;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) {
    *ppv = This;
    This->lpVtbl->AddRef(This);
    return S_OK;
  }
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE fac_add(IClassFactory *This) {
  QlicFactory *f = (QlicFactory *)This;
  InterlockedIncrement(&g_refs);
  return (ULONG)InterlockedIncrement(&f->refs);
}

static ULONG STDMETHODCALLTYPE fac_rel(IClassFactory *This) {
  QlicFactory *f = (QlicFactory *)This;
  LONG refs = InterlockedDecrement(&f->refs);
  InterlockedDecrement(&g_refs);
  return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE fac_create(IClassFactory *This,
                                            IUnknown *outer, REFIID riid,
                                            void **ppv) {
  (void)This;
  if (!ppv)
    return E_POINTER;
  *ppv = NULL;
  if (outer)
    return CLASS_E_NOAGGREGATION;
  QlicDecoder *d = (QlicDecoder *)calloc(1, sizeof(*d));
  if (!d)
    return E_OUTOFMEMORY;
  d->iface.lpVtbl = &g_decoder_vtbl;
  d->refs = 1;
  InitializeSRWLock(&d->lock);
  InterlockedIncrement(&g_refs);
  HRESULT hr = d->iface.lpVtbl->QueryInterface(&d->iface, riid, ppv);
  d->iface.lpVtbl->Release(&d->iface);
  /* QueryInterface's retained COM reference is returned through ppv. */
  // cppcheck-suppress memleak
  return hr;
}

static HRESULT STDMETHODCALLTYPE fac_lock(IClassFactory *This, BOOL lock) {
  (void)This;
  if (lock)
    InterlockedIncrement(&g_locks);
  else
    InterlockedDecrement(&g_locks);
  return S_OK;
}

static IClassFactoryVtbl g_factory_vtbl = {fac_qi, fac_add, fac_rel, fac_create,
                                           fac_lock};

_Check_return_ STDAPI DllGetClassObject(_In_ REFCLSID clsid, _In_ REFIID riid,
                                        _Outptr_ LPVOID *ppv) {
  if (!IsEqualCLSID(clsid, &CLSID_QlicWicDecoder))
    return CLASS_E_CLASSNOTAVAILABLE;
  return g_factory.iface.lpVtbl->QueryInterface(&g_factory.iface, riid, ppv);
}

__control_entrypoint(DllExport) STDAPI DllCanUnloadNow(void) {
  return (!g_refs && !g_locks) ? S_OK : S_FALSE;
}

static HRESULT hwin(LSTATUS s) {
  return s == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32((DWORD)s);
}

static HRESULT photos_source_path(const wchar_t *command_line, wchar_t *path,
                                  size_t path_count) {
  if (!command_line || !path || path_count < 2u)
    return E_INVALIDARG;
  const wchar_t *begin = command_line;
  while (*begin == L' ' || *begin == L'\t')
    ++begin;
  const wchar_t *end = NULL;
  if (*begin == L'"') {
    ++begin;
    end = wcschr(begin, L'"');
    if (!end)
      return E_INVALIDARG;
  } else {
    end = begin + wcslen(begin);
    while (end > begin && (end[-1] == L' ' || end[-1] == L'\t'))
      --end;
  }
  size_t length = (size_t)(end - begin);
  if (!length || length >= path_count)
    return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
  HRESULT hr = StringCchCopyNW(path, path_count, begin, length);
  if (FAILED(hr))
    return hr;
  wchar_t full[32768];
  DWORD full_length = GetFullPathNameW(path, (DWORD)_countof(full), full, NULL);
  if (!full_length)
    return HRESULT_FROM_WIN32(GetLastError());
  if (full_length >= _countof(full))
    return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
  DWORD attributes = GetFileAttributesW(full);
  if (attributes == INVALID_FILE_ATTRIBUTES)
    return HRESULT_FROM_WIN32(GetLastError());
  if (attributes & FILE_ATTRIBUTE_DIRECTORY)
    return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
  const wchar_t *base = full;
  for (const wchar_t *scan = full; *scan; ++scan) {
    if (*scan == L'\\' || *scan == L'/')
      base = scan + 1;
  }
  const wchar_t *extension = wcsrchr(base, L'.');
  if (!extension || _wcsicmp(extension, L".qlic") != 0)
    return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
  return StringCchCopyW(path, path_count, full);
}

static ULONGLONG photos_alias_hash(const wchar_t *path,
                                   const WIN32_FILE_ATTRIBUTE_DATA *data) {
  ULONGLONG hash = UINT64_C(1469598103934665603);
  while (*path) {
    unsigned value = (unsigned)*path++;
    hash ^= (BYTE)(value & 255u);
    hash *= UINT64_C(1099511628211);
    hash ^= (BYTE)((value >> 8u) & 255u);
    hash *= UINT64_C(1099511628211);
  }
  const DWORD values[] = {
      data->nFileSizeHigh,       data->nFileSizeLow,
      data->ftLastWriteTime.dwHighDateTime,
      data->ftLastWriteTime.dwLowDateTime};
  for (size_t i = 0; i < _countof(values); ++i) {
    DWORD value = values[i];
    for (unsigned shift = 0; shift < 32u; shift += 8u) {
      hash ^= (BYTE)(value >> shift);
      hash *= UINT64_C(1099511628211);
    }
  }
  return hash;
}

static HRESULT photos_alias_path(const wchar_t *source, wchar_t *alias,
                                 size_t alias_count) {
  WIN32_FILE_ATTRIBUTE_DATA source_data;
  if (!GetFileAttributesExW(source, GetFileExInfoStandard, &source_data))
    return HRESULT_FROM_WIN32(GetLastError());
  wchar_t temporary[32768];
  DWORD temporary_length =
      GetTempPathW((DWORD)_countof(temporary), temporary);
  if (!temporary_length)
    return HRESULT_FROM_WIN32(GetLastError());
  if (temporary_length >= _countof(temporary))
    return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
  wchar_t root[32768];
  HRESULT hr = StringCchPrintfW(root, _countof(root), L"%sQLIC-Photos",
                                temporary);
  if (FAILED(hr))
    return hr;
  if (!CreateDirectoryW(root, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
    return HRESULT_FROM_WIN32(GetLastError());
  wchar_t directory[32768];
  hr = StringCchPrintfW(directory, _countof(directory), L"%s\\%016llx",
                        root, photos_alias_hash(source, &source_data));
  if (FAILED(hr))
    return hr;
  if (!CreateDirectoryW(directory, NULL) &&
      GetLastError() != ERROR_ALREADY_EXISTS)
    return HRESULT_FROM_WIN32(GetLastError());

  const wchar_t *base = source;
  for (const wchar_t *scan = source; *scan; ++scan) {
    if (*scan == L'\\' || *scan == L'/')
      base = scan + 1;
  }
  wchar_t safe_name[192];
  size_t safe_length = 0;
  while (base[safe_length] && safe_length + 1u < _countof(safe_name)) {
    wchar_t value = base[safe_length];
    if (value < 32 || wcschr(L"<>:\"/\\|?*", value))
      value = L'_';
    safe_name[safe_length++] = value;
  }
  while (safe_length &&
         (safe_name[safe_length - 1u] == L' ' ||
          safe_name[safe_length - 1u] == L'.'))
    --safe_length;
  if (!safe_length) {
    hr = StringCchCopyW(safe_name, _countof(safe_name), L"image.qlic");
    if (FAILED(hr))
      return hr;
  } else {
    safe_name[safe_length] = L'\0';
  }
  hr = StringCchPrintfW(alias, alias_count, L"%s\\%s.png", directory,
                        safe_name);
  if (FAILED(hr))
    return hr;

  WIN32_FILE_ATTRIBUTE_DATA alias_data;
  int current = GetFileAttributesExW(alias, GetFileExInfoStandard, &alias_data) &&
                alias_data.nFileSizeHigh == source_data.nFileSizeHigh &&
                alias_data.nFileSizeLow == source_data.nFileSizeLow &&
                CompareFileTime(&alias_data.ftLastWriteTime,
                                &source_data.ftLastWriteTime) == 0;
  if (!current && !CopyFileW(source, alias, FALSE))
    return HRESULT_FROM_WIN32(GetLastError());
  return S_OK;
}

static HRESULT open_in_photos(const wchar_t *command_line) {
  wchar_t source[32768];
  HRESULT hr = photos_source_path(command_line, source, _countof(source));
  if (FAILED(hr))
    return hr;
  wchar_t alias[32768];
  hr = photos_alias_path(source, alias, _countof(alias));
  if (FAILED(hr))
    return hr;
  wchar_t uri[32768];
  hr = StringCchPrintfW(uri, _countof(uri),
                        L"ms-photos:viewer?fileName=%s", alias);
  if (FAILED(hr))
    return hr;
  HINSTANCE result = ShellExecuteW(NULL, L"open", uri, NULL, NULL,
                                   SW_SHOWNORMAL);
  if ((INT_PTR)result <= 32)
    return HRESULT_FROM_WIN32((DWORD)(INT_PTR)result);
  return S_OK;
}

void CALLBACK OpenInPhotos(HWND window, HINSTANCE instance,
                           LPSTR command_line, int show) {
  (void)instance;
  (void)command_line;
  (void)show;
  int argument_count = 0;
  LPWSTR *arguments =
      CommandLineToArgvW(GetCommandLineW(), &argument_count);
  HRESULT hr = arguments && argument_count >= 3
                   ? open_in_photos(arguments[argument_count - 1])
                   : E_INVALIDARG;
  if (arguments)
    LocalFree(arguments);
  if (FAILED(hr)) {
    wchar_t message[256];
    StringCchPrintfW(message, _countof(message),
                     L"QLIC could not open this image in Microsoft Photos "
                     L"(0x%08lX).",
                     (unsigned long)hr);
    MessageBoxW(window, message, L"QLIC Photos compatibility",
                MB_OK | MB_ICONERROR);
  }
}

static HRESULT reg_key(HKEY root, const wchar_t *path, HKEY *out) {
  DWORD disp = 0;
  return hwin(
      RegCreateKeyExW(root, path, 0, NULL, 0, KEY_WRITE, NULL, out, &disp));
}

static HRESULT reg_sz(HKEY k, const wchar_t *name, const wchar_t *v) {
  DWORD n = (DWORD)((wcslen(v) + 1u) * sizeof(wchar_t));
  return hwin(RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v, n));
}

static HRESULT reg_expand(HKEY k, const wchar_t *name, const wchar_t *v) {
  DWORD n = (DWORD)((wcslen(v) + 1u) * sizeof(wchar_t));
  return hwin(
      RegSetValueExW(k, name, 0, REG_EXPAND_SZ, (const BYTE *)v, n));
}

static HRESULT reg_none(HKEY k, const wchar_t *name) {
  return hwin(RegSetValueExW(k, name, 0, REG_NONE, NULL, 0));
}

static HRESULT reg_dw(HKEY k, const wchar_t *name, DWORD v) {
  return hwin(
      RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v)));
}

static HRESULT reg_bin(HKEY k, const wchar_t *name, const BYTE *v, DWORD n) {
  return hwin(RegSetValueExW(k, name, 0, REG_BINARY, v, n));
}

static DWORD component_signing_flags(const wchar_t *dll) {
  WINTRUST_FILE_INFO file;
  WINTRUST_DATA trust;
  GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  ZeroMemory(&file, sizeof(file));
  file.cbStruct = sizeof(file);
  file.pcwszFilePath = dll;
  ZeroMemory(&trust, sizeof(trust));
  trust.cbStruct = sizeof(trust);
  trust.dwUIChoice = WTD_UI_NONE;
  trust.fdwRevocationChecks = WTD_REVOKE_NONE;
  trust.dwUnionChoice = WTD_CHOICE_FILE;
  trust.pFile = &file;
  trust.dwStateAction = WTD_STATEACTION_VERIFY;
  trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
  trust.dwUIContext = WTD_UICONTEXT_EXECUTE;
  LONG status = WinVerifyTrust(NULL, &policy, &trust);
  trust.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(NULL, &policy, &trust);
  return status == ERROR_SUCCESS ? WICComponentSigned : WICComponentUnsigned;
}

static HRESULT delete_tree(HKEY root, const wchar_t *path) {
  LSTATUS status = RegDeleteTreeW(root, path);
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
    return S_OK;
  return hwin(status);
}

static HRESULT reg_remove(HKEY root) {
  wchar_t path[512];
  HRESULT result = S_OK;
  HRESULT hr =
      StringCchPrintfW(path, 512, L"Software\\Classes\\CLSID\\%s",
                       QLIC_CLSID_TEXT);
  if (SUCCEEDED(hr))
    hr = delete_tree(root, path);
  if (FAILED(hr))
    result = hr;
  hr = StringCchPrintfW(path, 512,
                        L"Software\\Classes\\CLSID\\%s\\Instance\\%s",
                        WIC_DECODER_CAT_TEXT, QLIC_CLSID_TEXT);
  if (SUCCEEDED(hr))
    hr = delete_tree(root, path);
  if (FAILED(hr) && SUCCEEDED(result))
    result = hr;
  hr = delete_tree(root, L"Software\\Classes\\.qlic");
  if (FAILED(hr) && SUCCEEDED(result))
    result = hr;
  hr = delete_tree(root, L"Software\\Classes\\SystemFileAssociations\\.qlic");
  if (FAILED(hr) && SUCCEEDED(result))
    result = hr;
  hr = delete_tree(root, L"Software\\Classes\\QLIC.Image");
  if (FAILED(hr) && SUCCEEDED(result))
    result = hr;
  HKEY kind_map = NULL;
  LSTATUS kind_status = RegOpenKeyExW(
      root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\KindMap",
      0, KEY_SET_VALUE, &kind_map);
  if (kind_status == ERROR_SUCCESS) {
    kind_status = RegDeleteValueW(kind_map, L".qlic");
    RegCloseKey(kind_map);
  }
  if (kind_status != ERROR_SUCCESS && kind_status != ERROR_FILE_NOT_FOUND &&
      kind_status != ERROR_PATH_NOT_FOUND && SUCCEEDED(result))
    result = hwin(kind_status);
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
  return result;
}

static HRESULT reg_write(HKEY root) {
  wchar_t dll[MAX_PATH];
  DWORD dll_length = GetModuleFileNameW(g_mod, dll, MAX_PATH);
  if (!dll_length)
    return HRESULT_FROM_WIN32(GetLastError());
  if (dll_length >= MAX_PATH)
    return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

  HKEY k = NULL, sub = NULL;
  wchar_t path[512];
  HRESULT hr = S_OK;

  hr = delete_tree(root,
                   L"Software\\Classes\\.qlic\\OpenWithList\\PhotoViewer.dll");
  if (FAILED(hr))
    goto fail;
  hr = delete_tree(
      root,
      L"Software\\Classes\\SystemFileAssociations\\.qlic\\OpenWithList\\PhotoViewer.dll");
  if (FAILED(hr))
    goto fail;
  hr = delete_tree(root,
                   L"Software\\Classes\\QLIC.Image\\shell\\open\\DropTarget");
  if (FAILED(hr))
    goto fail;

  hr = StringCchPrintfW(path, 512, L"Software\\Classes\\CLSID\\%s",
                        QLIC_CLSID_TEXT);
  if (FAILED(hr))
    goto fail;
  hr = reg_key(root, path, &k);
  if (FAILED(hr))
    goto fail;
#define REG_SET(expression)                                                    \
  do {                                                                         \
    hr = (expression);                                                         \
    if (FAILED(hr))                                                            \
      goto fail;                                                               \
  } while (0)
  REG_SET(reg_sz(k, NULL, L"QLIC WIC Decoder"));
  REG_SET(reg_sz(k, L"FriendlyName", L"QLIC Decoder"));
  REG_SET(reg_sz(k, L"Description", L"Quick Lossless Image Codec Decoder"));
  REG_SET(reg_sz(k, L"Author", L"Ozzy M."));
  REG_SET(reg_sz(k, L"Vendor", QLIC_VENDOR_TEXT));
  REG_SET(reg_sz(k, L"Version", QLIC_VERSION_W L".0"));
  REG_SET(reg_sz(k, L"SpecVersion", L"1.0.0.0"));
  REG_SET(reg_sz(k, L"ColorManagementVersion", L"1.0.0.0"));
  REG_SET(reg_sz(k, L"ContainerFormat", QLIC_CONTAINER_TEXT));
  REG_SET(reg_sz(k, L"FileExtensions", L".qlic"));
  REG_SET(reg_sz(k, L"MimeTypes", L"image/qlic"));
  REG_SET(reg_dw(k, L"Flags", component_signing_flags(dll)));
  REG_SET(reg_dw(k, L"ArbitrationPriority", 10));
  REG_SET(reg_dw(k, L"SupportAnimation", 1));
  REG_SET(reg_dw(k, L"SupportChromaKey", 0));
  REG_SET(reg_dw(k, L"SupportLossless", 1));
  REG_SET(reg_dw(k, L"SupportMultiframe", 1));

  hr = reg_key(k, L"InProcServer32", &sub);
  if (FAILED(hr))
    goto fail;
  REG_SET(reg_sz(sub, NULL, dll));
  REG_SET(reg_sz(sub, L"ThreadingModel", L"Both"));
  RegCloseKey(sub);
  sub = NULL;
  hr = reg_key(k, L"Formats\\{F5C7AD2D-6A8D-43DD-A7A8-A29935261AE9}", &sub);
  if (FAILED(hr))
    goto fail;
  RegCloseKey(sub);
  sub = NULL;
  hr = reg_key(k, L"Formats\\{3CC4A650-A527-4D37-A916-3142C7EBEDBA}", &sub);
  if (FAILED(hr))
    goto fail;
  RegCloseKey(sub);
  sub = NULL;
  hr = reg_key(k, L"Formats\\{6FDDC324-4E03-4BFE-B185-3D77768DC908}", &sub);
  if (FAILED(hr))
    goto fail;
  RegCloseKey(sub);
  sub = NULL;
  hr = reg_key(k, L"Formats\\{6FDDC324-4E03-4BFE-B185-3D77768DC90B}", &sub);
  if (FAILED(hr))
    goto fail;
  RegCloseKey(sub);
  sub = NULL;
  hr = reg_key(k, L"Formats\\{6FDDC324-4E03-4BFE-B185-3D77768DC90D}", &sub);
  if (FAILED(hr))
    goto fail;
  RegCloseKey(sub);
  sub = NULL;
  hr = reg_key(k, L"Formats\\{6FDDC324-4E03-4BFE-B185-3D77768DC915}", &sub);
  if (FAILED(hr))
    goto fail;
  RegCloseKey(sub);
  sub = NULL;
  hr = reg_key(k, L"Formats\\{6FDDC324-4E03-4BFE-B185-3D77768DC916}", &sub);
  if (FAILED(hr))
    goto fail;
  RegCloseKey(sub);
  sub = NULL;
  hr = reg_key(k, L"Formats\\{6FDDC324-4E03-4BFE-B185-3D77768DC917}", &sub);
  if (FAILED(hr))
    goto fail;
  RegCloseKey(sub);
  sub = NULL;
  hr = reg_key(k, L"Formats\\{9C215C5D-1ACC-4F0E-A4BC-70FB3AE8FD28}", &sub);
  if (FAILED(hr))
    goto fail;
  RegCloseKey(sub);
  sub = NULL;
  hr = reg_key(k, L"Patterns\\0", &sub);
  if (FAILED(hr))
    goto fail;
  BYTE mask[4] = {255, 255, 255, 255};
  BYTE pattern[4] = {'Q', 'L', 'I', 'C'};
  REG_SET(reg_dw(sub, L"Position", 0));
  REG_SET(reg_dw(sub, L"Length", 4));
  REG_SET(reg_bin(sub, L"Mask", mask, sizeof(mask)));
  REG_SET(reg_bin(sub, L"Pattern", pattern, sizeof(pattern)));
  RegCloseKey(sub);
  sub = NULL;
  RegCloseKey(k);
  k = NULL;

  hr = StringCchPrintfW(path, 512,
                        L"Software\\Classes\\CLSID\\%s\\Instance\\%s",
                        WIC_DECODER_CAT_TEXT, QLIC_CLSID_TEXT);
  if (FAILED(hr))
    goto fail;
  hr = reg_key(root, path, &k);
  if (FAILED(hr))
    goto fail;
  REG_SET(reg_sz(k, L"CLSID", QLIC_CLSID_TEXT));
  REG_SET(reg_sz(k, L"FriendlyName", L"QLIC Decoder"));
  RegCloseKey(k);
  k = NULL;

  hr = reg_key(root, L"Software\\Classes\\.qlic", &k);
  if (FAILED(hr))
    goto fail;
  REG_SET(reg_sz(k, NULL, L"QLIC.Image"));
  REG_SET(reg_sz(k, L"Content Type", L"image/qlic"));
  REG_SET(reg_sz(k, L"PerceivedType", L"image"));
  hr = reg_key(k, L"OpenWithProgids", &sub);
  if (FAILED(hr))
    goto fail;
  REG_SET(reg_none(sub, L"QLIC.Image"));
  LSTATUS direct_photos = RegDeleteValueW(sub, PHOTOS_APP_PROGID);
  if (direct_photos != ERROR_SUCCESS && direct_photos != ERROR_FILE_NOT_FOUND)
    REG_SET(hwin(direct_photos));
  RegCloseKey(sub);
  sub = NULL;
  hr = reg_key(k, L"ShellEx\\" WIC_THUMBNAIL_IID_TEXT, &sub);
  if (FAILED(hr))
    goto fail;
  REG_SET(reg_sz(sub, NULL, WIC_THUMBNAIL_PROVIDER_TEXT));
  RegCloseKey(sub);
  sub = NULL;
  hr = reg_key(k, L"ShellEx\\ContextMenuHandlers\\ShellImagePreview", &sub);
  if (FAILED(hr))
    goto fail;
  REG_SET(reg_sz(sub, NULL, SHELL_IMAGE_PREVIEW_TEXT));
  RegCloseKey(sub);
  sub = NULL;
  RegCloseKey(k);
  k = NULL;

  hr = reg_key(root, L"Software\\Classes\\QLIC.Image", &k);
  if (FAILED(hr))
    goto fail;
  REG_SET(reg_sz(k, NULL, L"QLIC Image"));
  REG_SET(reg_sz(k, L"FriendlyTypeName", L"QLIC Image"));
  REG_SET(reg_dw(k, L"ImageOptionFlags", 1));
  hr = reg_key(k, L"Application", &sub);
  if (FAILED(hr))
    goto fail;
  REG_SET(reg_sz(sub, L"ApplicationName", L"QLIC"));
  REG_SET(reg_sz(sub, L"ApplicationDescription",
                 L"Views original QLIC files and compresses source images"));
  RegCloseKey(sub);
  sub = NULL;
  hr = reg_key(k, L"DefaultIcon", &sub);
  if (FAILED(hr))
    goto fail;
  REG_SET(reg_expand(sub, NULL,
                     L"%SystemRoot%\\System32\\imageres.dll,-72"));
  RegCloseKey(sub);
  sub = NULL;
  hr = reg_key(k, L"shell\\open", &sub);
  if (FAILED(hr))
    goto fail;
  REG_SET(reg_sz(sub, L"MuiVerb", L"Open QLIC image"));
  RegCloseKey(sub);
  sub = NULL;
  hr = reg_key(k, L"shell\\open\\command", &sub);
  if (FAILED(hr))
    goto fail;
  wchar_t viewer_command[MAX_PATH * 3u];
  wchar_t program_files[MAX_PATH];
  wchar_t photo_viewer[MAX_PATH];
  wchar_t qlic_viewer[MAX_PATH * 2u];
  wchar_t system_directory[MAX_PATH];
  UINT system_length =
      GetSystemDirectoryW(system_directory, _countof(system_directory));
  HRESULT program_files_status =
      SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, SHGFP_TYPE_CURRENT,
                       program_files);
  HRESULT photo_viewer_status = FAILED(program_files_status)
                                    ? program_files_status
                                    : StringCchPrintfW(
                                          photo_viewer,
                                          _countof(photo_viewer),
                                          L"%s\\Windows Photo Viewer\\PhotoViewer.dll",
                                          program_files);
  const wchar_t *dll_slash = wcsrchr(dll, L'\\');
  HRESULT qlic_viewer_status =
      dll_slash
          ? StringCchPrintfW(qlic_viewer, _countof(qlic_viewer),
                             L"%.*s\\qlic-gui.exe", (int)(dll_slash - dll), dll)
          : STRSAFE_E_INVALID_PARAMETER;
  int has_qlic_viewer =
      SUCCEEDED(qlic_viewer_status) &&
      GetFileAttributesW(qlic_viewer) != INVALID_FILE_ATTRIBUTES;
  int has_wic_viewer =
      system_length && system_length < _countof(system_directory) &&
      SUCCEEDED(photo_viewer_status) &&
      GetFileAttributesW(photo_viewer) != INVALID_FILE_ATTRIBUTES;
  if (has_qlic_viewer) {
    REG_SET(StringCchPrintfW(viewer_command, _countof(viewer_command),
                             L"\"%s\" \"%%1\"", qlic_viewer));
    REG_SET(reg_sz(sub, NULL, viewer_command));
  } else if (has_wic_viewer) {
    REG_SET(StringCchPrintfW(
        viewer_command, _countof(viewer_command),
        L"\"%s\\rundll32.exe\" \"%s\", ImageView_Fullscreen %%1",
        system_directory, photo_viewer));
    REG_SET(reg_sz(sub, NULL, viewer_command));
  } else {
    REG_SET(StringCchPrintfW(
        viewer_command, _countof(viewer_command),
        L"%%SystemRoot%%\\System32\\rundll32.exe \"%s\",OpenInPhotos \"%%1\"",
        dll));
    REG_SET(reg_expand(sub, NULL, viewer_command));
  }
  RegCloseKey(sub);
  sub = NULL;
  hr = reg_key(k, L"shell\\photos", &sub);
  if (FAILED(hr))
    goto fail;
  REG_SET(reg_sz(sub, L"MuiVerb",
                 L"Open in Microsoft Photos (compatibility)"));
  RegCloseKey(sub);
  sub = NULL;
  hr = reg_key(k, L"shell\\photos\\command", &sub);
  if (FAILED(hr))
    goto fail;
  wchar_t photos_command[MAX_PATH * 2u];
  REG_SET(StringCchPrintfW(
      photos_command, _countof(photos_command),
      L"%%SystemRoot%%\\System32\\rundll32.exe \"%s\",OpenInPhotos \"%%1\"",
      dll));
  REG_SET(reg_expand(sub, NULL, photos_command));
  RegCloseKey(sub);
  sub = NULL;
  RegCloseKey(k);
  k = NULL;

  hr = reg_key(root,
               L"Software\\Classes\\SystemFileAssociations\\.qlic", &k);
  if (FAILED(hr))
    goto fail;
  hr = reg_key(k, L"ShellEx\\" WIC_THUMBNAIL_IID_TEXT, &sub);
  if (FAILED(hr))
    goto fail;
  REG_SET(reg_sz(sub, NULL, WIC_THUMBNAIL_PROVIDER_TEXT));
  RegCloseKey(sub);
  sub = NULL;
  hr = reg_key(k, L"ShellEx\\ContextMenuHandlers\\ShellImagePreview", &sub);
  if (FAILED(hr))
    goto fail;
  REG_SET(reg_sz(sub, NULL, SHELL_IMAGE_PREVIEW_TEXT));
  RegCloseKey(sub);
  sub = NULL;
  RegCloseKey(k);
  k = NULL;

  hr = reg_key(root,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\KindMap",
               &k);
  if (FAILED(hr))
    goto fail;
  REG_SET(reg_sz(k, L".qlic", L"Picture"));
  RegCloseKey(k);
  k = NULL;
#undef REG_SET
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
  return S_OK;

fail:
  if (sub)
    RegCloseKey(sub);
  if (k)
    RegCloseKey(k);
#undef REG_SET
  reg_remove(root);
  return hr;
}

HRESULT __stdcall DllRegisterServer(void) {
  return reg_write(HKEY_LOCAL_MACHINE);
}

HRESULT __stdcall DllUnregisterServer(void) {
  return reg_remove(HKEY_LOCAL_MACHINE);
}

HRESULT __stdcall DllInstall(BOOL install, LPCWSTR command_line) {
  if (command_line && command_line[0] &&
      _wcsicmp(command_line, L"machine") != 0)
    return E_INVALIDARG;
  return install ? reg_write(HKEY_LOCAL_MACHINE)
                 : reg_remove(HKEY_LOCAL_MACHINE);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, void *reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    g_mod = h;
    DisableThreadLibraryCalls(h);
  }
  return TRUE;
}
