#include <qlic/qlic.h>
#include "qlic_version.h"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strsafe.h>
#include <wchar.h>
#include <wincodec.h>

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

typedef struct QlicDecoder QlicDecoder;
typedef struct QlicFrame QlicFrame;

struct QlicDecoder {
  IWICBitmapDecoder iface;
  volatile LONG refs;
  qlic_animation anim;
  int ready;
};

struct QlicFrame {
  IWICBitmapFrameDecode iface;
  volatile LONG refs;
  QlicDecoder *dec;
  UINT index;
};

typedef struct {
  IClassFactory iface;
  volatile LONG refs;
} QlicFactory;

static HMODULE g_mod;
static volatile LONG g_refs;
static volatile LONG g_locks;
/* WIC can decode during shell browsing, keep its limits below the library defaults */
static const qlic_decode_limits g_wic_decode_limits = {
    sizeof(qlic_decode_limits),
    0,
    UINT64_C(268435456),
    UINT64_C(268435456),
    UINT64_C(33554432),
    UINT64_C(268435456),
    512u,
    0};

static IWICBitmapDecoderVtbl g_decoder_vtbl;
static IWICBitmapFrameDecodeVtbl g_frame_vtbl;
static IClassFactoryVtbl g_factory_vtbl;
static QlicFactory g_factory = {{&g_factory_vtbl}, 1};

static HRESULT q_hr(int status) {
  if (status == QLIC_OUT_OF_MEMORY)
    return E_OUTOFMEMORY;
  if (status == QLIC_LIMIT_EXCEEDED)
    return WINCODEC_ERR_VALUEOUTOFRANGE;
  if (status == QLIC_BAD_DATA || status == QLIC_ERROR)
    return WINCODEC_ERR_BADIMAGE;
  return WINCODEC_ERR_COMPONENTINITIALIZEFAILURE;
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
  if (end.QuadPart > g_wic_decode_limits.max_file_bytes)
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
  while (pos < n) {
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

static int stream_magic(IStream *s) {
  if (!s)
    return 0;
  LARGE_INTEGER z;
  ULARGE_INTEGER old;
  uint8_t head[4] = {0};
  ULONG got = 0;
  z.QuadPart = 0;
  if (FAILED(s->lpVtbl->Seek(s, z, STREAM_SEEK_CUR, &old)))
    old.QuadPart = 0;
  /* WIC may probe more than one decoder, do not consume the shared stream */
  if (FAILED(s->lpVtbl->Seek(s, z, STREAM_SEEK_SET, NULL)))
    return 0;
  HRESULT hr = s->lpVtbl->Read(s, head, sizeof(head), &got);
  LARGE_INTEGER back;
  back.QuadPart = (LONGLONG)old.QuadPart;
  s->lpVtbl->Seek(s, back, STREAM_SEEK_SET, NULL);
  return SUCCEEDED(hr) && got == sizeof(head) &&
         memcmp(head, "QLIC", sizeof(head)) == 0;
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
  qlic_animation anim = {0};
  int result = qlic_decode_animation(data, n, &g_wic_decode_limits, &anim);
  free(data);
  if (result != QLIC_OK) {
    char line[1200];
    snprintf(line, sizeof(line), "dec_qlic failed: bytes=%zu err=%s", n,
             qlic_last_error());
    wic_log(line);
    return q_hr(result);
  }
  d->anim = anim;
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
  *cap = stream_magic(s) ? WICBitmapDecoderCapabilityCanDecodeAllImages : 0;
  wic_log(*cap ? "QueryCapability match" : "QueryCapability no match");
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE dec_init(IWICBitmapDecoder *This, IStream *s,
                                          WICDecodeOptions opt) {
  (void)opt;
  if (!s)
    return E_INVALIDARG;
  return dec_load((QlicDecoder *)This, s);
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
  wic_log("GetDecoderInfo unsupported");
  return WINCODEC_ERR_UNSUPPORTEDOPERATION;
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
  if (!src)
    return E_POINTER;
  *src = NULL;
  IWICBitmapFrameDecode *fr = NULL;
  HRESULT hr = This->lpVtbl->GetFrame(This, 0, &fr);
  if (FAILED(hr))
    return hr;
  *src = (IWICBitmapSource *)fr;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE dec_contexts(IWICBitmapDecoder *This, UINT c,
                                              IWICColorContext **ctx,
                                              UINT *actual) {
  (void)This;
  (void)c;
  (void)ctx;
  if (!actual)
    return E_POINTER;
  *actual = 0;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE dec_thumb(IWICBitmapDecoder *This,
                                           IWICBitmapSource **src) {
  (void)This;
  if (!src)
    return E_POINTER;
  *src = NULL;
  return WINCODEC_ERR_UNSUPPORTEDOPERATION;
}

static HRESULT STDMETHODCALLTYPE dec_count(IWICBitmapDecoder *This,
                                           UINT *count) {
  QlicDecoder *d = (QlicDecoder *)This;
  if (!count)
    return E_POINTER;
  if (!d->ready)
    return WINCODEC_ERR_NOTINITIALIZED;
  *count = d->anim.frame_count;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE dec_frame(IWICBitmapDecoder *This, UINT index,
                                           IWICBitmapFrameDecode **frame) {
  QlicDecoder *d = (QlicDecoder *)This;
  if (!frame)
    return E_POINTER;
  *frame = NULL;
  if (!d->ready)
    return WINCODEC_ERR_NOTINITIALIZED;
  if (index >= d->anim.frame_count)
    return WINCODEC_ERR_FRAMEMISSING;
  QlicFrame *f = (QlicFrame *)calloc(1, sizeof(*f));
  if (!f)
    return E_OUTOFMEMORY;
  f->iface.lpVtbl = &g_frame_vtbl;
  f->refs = 1;
  f->dec = d;
  f->index = index;
  This->lpVtbl->AddRef(This);
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
    This->lpVtbl->AddRef(This);
    return S_OK;
  }
  return E_NOINTERFACE;
}

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
  qlic_image *im = &f->dec->anim.frames[f->index].image;
  *w = im->width;
  *h = im->height;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE fr_pf(IWICBitmapFrameDecode *This,
                                       WICPixelFormatGUID *pf) {
  (void)This;
  if (!pf)
    return E_POINTER;
  *pf = GUID_WICPixelFormat32bppRGBA;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE fr_res(IWICBitmapFrameDecode *This, double *x,
                                        double *y) {
  (void)This;
  if (!x || !y)
    return E_POINTER;
  *x = 96.0;
  *y = 96.0;
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
  qlic_image *im = &f->dec->anim.frames[f->index].image;
  UINT iw = im->width;
  UINT ih = im->height;
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
  uint64_t row = (uint64_t)(UINT)w * 4u;
  if (row > UINT_MAX || stride < row)
    return E_INVALIDARG;
  uint64_t required =
      h ? (uint64_t)stride * ((UINT)h - 1u) + row : 0u;
  if (required > UINT_MAX || required > bufn)
    return E_INVALIDARG;
  uint64_t source_stride = im->stride;
  uint64_t source_offset =
      (uint64_t)(UINT)y * source_stride + (uint64_t)(UINT)x * 4u;
  uint64_t source_required =
      h ? source_offset + (uint64_t)((UINT)h - 1u) * source_stride + row
        : source_offset;
  if (source_stride < (uint64_t)iw * 4u || source_offset > SIZE_MAX ||
      source_stride > SIZE_MAX || source_required < source_offset ||
      source_required > im->rgba_size)
    return WINCODEC_ERR_BADIMAGE;
  const uint8_t *src = im->rgba + (size_t)source_offset;
  for (UINT yy = 0; yy < (UINT)h; ++yy) {
    memcpy(buf + (size_t)yy * stride,
           src + (size_t)yy * (size_t)source_stride, (size_t)row);
  }
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE fr_meta(IWICBitmapFrameDecode *This,
                                         IWICMetadataQueryReader **r) {
  (void)This;
  if (!r)
    return E_POINTER;
  *r = NULL;
  return WINCODEC_ERR_UNSUPPORTEDOPERATION;
}

static HRESULT STDMETHODCALLTYPE fr_contexts(IWICBitmapFrameDecode *This,
                                             UINT c, IWICColorContext **ctx,
                                             UINT *actual) {
  (void)This;
  (void)c;
  (void)ctx;
  if (!actual)
    return E_POINTER;
  *actual = 0;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE fr_thumb(IWICBitmapFrameDecode *This,
                                          IWICBitmapSource **src) {
  (void)This;
  if (!src)
    return E_POINTER;
  *src = NULL;
  return WINCODEC_ERR_UNSUPPORTEDOPERATION;
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
  return (ULONG)InterlockedIncrement(&f->refs);
}

static ULONG STDMETHODCALLTYPE fac_rel(IClassFactory *This) {
  QlicFactory *f = (QlicFactory *)This;
  return (ULONG)InterlockedDecrement(&f->refs);
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
  InterlockedIncrement(&g_refs);
  HRESULT hr = d->iface.lpVtbl->QueryInterface(&d->iface, riid, ppv);
  d->iface.lpVtbl->Release(&d->iface);
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

static HRESULT reg_key(HKEY root, const wchar_t *path, HKEY *out) {
  DWORD disp = 0;
  return hwin(
      RegCreateKeyExW(root, path, 0, NULL, 0, KEY_WRITE, NULL, out, &disp));
}

static HRESULT reg_sz(HKEY k, const wchar_t *name, const wchar_t *v) {
  DWORD n = (DWORD)((wcslen(v) + 1u) * sizeof(wchar_t));
  return hwin(RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v, n));
}

static HRESULT reg_dw(HKEY k, const wchar_t *name, DWORD v) {
  return hwin(
      RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v)));
}

static HRESULT reg_bin(HKEY k, const wchar_t *name, const BYTE *v, DWORD n) {
  return hwin(RegSetValueExW(k, name, 0, REG_BINARY, v, n));
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
  hr = delete_tree(root, L"Software\\Classes\\QLIC.Image");
  if (FAILED(hr) && SUCCEEDED(result))
    result = hr;
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
  REG_SET(reg_sz(k, L"Author", L"QLIC Project"));
  REG_SET(reg_sz(k, L"Vendor", QLIC_VENDOR_TEXT));
  REG_SET(reg_sz(k, L"Version", QLIC_VERSION_W));
  REG_SET(reg_sz(k, L"SpecVersion", L"1.0.0"));
  REG_SET(reg_sz(k, L"ColorManagementVersion", L"1.0.0.0"));
  REG_SET(reg_sz(k, L"ContainerFormat", QLIC_CONTAINER_TEXT));
  REG_SET(reg_sz(k, L"FileExtensions", L".qlic"));
  REG_SET(reg_sz(k, L"MimeTypes", L"image/qlic"));
  REG_SET(reg_dw(k, L"Flags", 1));
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
  RegCloseKey(k);
  k = NULL;

  hr = reg_key(root, L"Software\\Classes\\QLIC.Image", &k);
  if (FAILED(hr))
    goto fail;
  REG_SET(reg_sz(k, NULL, L"QLIC Image"));
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
  return reg_write(HKEY_CURRENT_USER);
}

HRESULT __stdcall DllUnregisterServer(void) {
  return reg_remove(HKEY_CURRENT_USER);
}

HRESULT __stdcall DllInstall(BOOL install, LPCWSTR command_line) {
  HKEY root =
      command_line && _wcsicmp(command_line, L"machine") == 0
          ? HKEY_LOCAL_MACHINE
          : HKEY_CURRENT_USER;
  return install ? reg_write(root) : reg_remove(root);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, void *reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    g_mod = h;
    DisableThreadLibraryCalls(h);
  }
  return TRUE;
}
