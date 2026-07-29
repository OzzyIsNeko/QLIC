#include <windows.h>
#include <wincodec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const GUID CLSID_QlicWicDecoder = {
    0x5ce9f7d8,
    0x140b,
    0x43fc,
    {0x87, 0x62, 0xb8, 0xe7, 0x2f, 0xf6, 0xb7, 0x65}};
static const uint8_t fixture_pixels[4] = {11, 22, 33, 44};

typedef HRESULT(__stdcall *DllGetClassObjectFn)(REFCLSID, REFIID, void **);

static HRESULT create_adjacent_decoder(IWICBitmapDecoder **dec, HMODULE *dll) {
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
  if (!n || n >= MAX_PATH) {
    DWORD e = GetLastError();
    return HRESULT_FROM_WIN32(e ? e : ERROR_INSUFFICIENT_BUFFER);
  }
  wchar_t *slash = wcsrchr(path, L'\\');
  if (!slash)
    return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
  slash[1] = 0;
  if (wcslen(path) + wcslen(L"qlic-wic.dll") + 1 > MAX_PATH)
    return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
  wcscat_s(path, MAX_PATH, L"qlic-wic.dll");

  HMODULE mod = LoadLibraryW(path);
  if (!mod)
    return HRESULT_FROM_WIN32(GetLastError());

  DllGetClassObjectFn get_class =
      (DllGetClassObjectFn)GetProcAddress(mod, "DllGetClassObject");
  if (!get_class) {
    HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
    FreeLibrary(mod);
    return hr;
  }

  IClassFactory *factory = NULL;
  HRESULT hr =
      get_class(&CLSID_QlicWicDecoder, &IID_IClassFactory, (void **)&factory);
  if (SUCCEEDED(hr)) {
    hr = factory->lpVtbl->CreateInstance(factory, NULL, &IID_IWICBitmapDecoder,
                                         (void **)dec);
    factory->lpVtbl->Release(factory);
  }
  if (FAILED(hr)) {
    FreeLibrary(mod);
    return hr;
  }
  *dll = mod;
  return S_OK;
}

static uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t c = 0xffffffffu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k)
      c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
  }
  return c ^ 0xffffffffu;
}

static void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static int write_fixture(wchar_t *path) {
  wchar_t dir[MAX_PATH];
  if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"qlc", 0, path))
    return 0;
  uint8_t file[36] = {0};
  memcpy(file, "QLIC", 4);
  wr32(file + 4, 1);
  wr32(file + 8, 1);
  file[12] = 4;
  file[13] = 2;
  file[15] = 0x80;
  wr32(file + 20, 4);
  file[28] = 11;
  file[29] = 22;
  file[30] = 33;
  file[31] = 44;
  wr32(file + 32, crc32(file, 32));
  FILE *out = NULL;
  if (_wfopen_s(&out, path, L"wb") || !out) {
    DeleteFileW(path);
    return 0;
  }
  size_t written = fwrite(file, 1, sizeof(file), out);
  int closed = fclose(out) == 0;
  int ok = written == sizeof(file) && closed;
  if (!ok)
    DeleteFileW(path);
  return ok;
}

int wmain(int argc, wchar_t **argv) {
  int self_test = argc == 2 && !_wcsicmp(argv[1], L"--self-test");
  int direct = self_test || (argc >= 3 && !_wcsicmp(argv[1], L"--direct"));
  wchar_t fixture[MAX_PATH] = {0};
  const wchar_t *path =
      direct && !self_test ? argv[2] : (argc >= 2 ? argv[1] : NULL);
  if (self_test) {
    if (!write_fixture(fixture)) {
      fwprintf(stderr, L"could not create WIC fixture\n");
      return 1;
    }
    path = fixture;
  }
  if (!path) {
    fwprintf(stderr, L"usage: qlic-wic-smoke [--direct] image.qlic\n");
    return 2;
  }

  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  int co = SUCCEEDED(hr);
  if (hr == RPC_E_CHANGED_MODE)
    co = 0;
  else if (FAILED(hr)) {
    fwprintf(stderr, L"CoInitializeEx failed: 0x%08lx\n", (unsigned long)hr);
    if (self_test)
      DeleteFileW(fixture);
    return 1;
  }

  IWICImagingFactory *fac = NULL;
  IWICBitmapDecoder *dec = NULL;
  IWICBitmapFrameDecode *frame = NULL;
  IWICFormatConverter *conv = NULL;
  IWICStream *stream = NULL;
  HMODULE qlicDll = NULL;
  uint8_t *pix = NULL;
  int rc = 1;

  const wchar_t *stage = L"CoCreateInstance";
  hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                        &IID_IWICImagingFactory, (void **)&fac);
  if (SUCCEEDED(hr)) {
    if (direct) {
      stage = L"Create QLIC decoder";
      hr = create_adjacent_decoder(&dec, &qlicDll);
      if (SUCCEEDED(hr)) {
        stage = L"CreateStream";
        hr = fac->lpVtbl->CreateStream(fac, &stream);
      }
      if (SUCCEEDED(hr)) {
        stage = L"InitializeFromFilename";
        hr = stream->lpVtbl->InitializeFromFilename(stream, path, GENERIC_READ);
      }
      if (SUCCEEDED(hr)) {
        stage = L"Decoder.Initialize";
        hr = dec->lpVtbl->Initialize(dec, (IStream *)stream,
                                     WICDecodeMetadataCacheOnLoad);
      }
    } else {
      stage = L"CreateDecoderFromFilename";
      hr = fac->lpVtbl->CreateDecoderFromFilename(
          fac, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec);
    }
  }
  if (SUCCEEDED(hr)) {
    UINT count = 0;
    hr = dec->lpVtbl->GetFrameCount(dec, &count);
    if (SUCCEEDED(hr))
      printf("wic frame count: %u\n", count);
    if (SUCCEEDED(hr) && self_test && count != 1u) {
      stage = L"ValidateFrameCount";
      hr = WINCODEC_ERR_BADIMAGE;
    }
  }
  if (SUCCEEDED(hr)) {
    stage = L"GetFrame";
    hr = dec->lpVtbl->GetFrame(dec, 0, &frame);
  }
  if (SUCCEEDED(hr) && self_test) {
    WICPixelFormatGUID pixel_format = {0};
    uint8_t direct_pixels[4] = {0};
    stage = L"Frame.GetPixelFormat";
    hr = frame->lpVtbl->GetPixelFormat(frame, &pixel_format);
    if (SUCCEEDED(hr) &&
        !IsEqualGUID(&pixel_format, &GUID_WICPixelFormat32bppRGBA))
      hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    if (SUCCEEDED(hr)) {
      stage = L"Frame.CopyPixels";
      hr = frame->lpVtbl->CopyPixels(frame, NULL, 4u, 4u, direct_pixels);
    }
    if (SUCCEEDED(hr) && memcmp(direct_pixels, fixture_pixels, 4u))
      hr = WINCODEC_ERR_BADIMAGE;
    if (SUCCEEDED(hr)) {
      HRESULT probe =
          frame->lpVtbl->CopyPixels(frame, NULL, 4u, 3u, direct_pixels);
      if (probe != E_INVALIDARG) {
        stage = L"Frame.CopyPixelsBufferCheck";
        hr = E_FAIL;
      }
    }
    if (SUCCEEDED(hr)) {
      WICRect outside = {1, 0, 1, 1};
      HRESULT probe =
          frame->lpVtbl->CopyPixels(frame, &outside, 4u, 4u, direct_pixels);
      if (probe != E_INVALIDARG) {
        stage = L"Frame.CopyPixelsRectCheck";
        hr = E_FAIL;
      }
    }
    if (SUCCEEDED(hr)) {
      IWICBitmapFrameDecode *missing = NULL;
      HRESULT probe = dec->lpVtbl->GetFrame(dec, 1u, &missing);
      if (missing)
        missing->lpVtbl->Release(missing);
      if (probe != WINCODEC_ERR_FRAMEMISSING || missing) {
        stage = L"GetMissingFrame";
        hr = E_FAIL;
      }
    }
  }
  if (SUCCEEDED(hr)) {
    stage = L"CreateFormatConverter";
    hr = fac->lpVtbl->CreateFormatConverter(fac, &conv);
  }
  if (SUCCEEDED(hr)) {
    stage = L"FormatConverter.Initialize";
    hr = conv->lpVtbl->Initialize(
        conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
  }

  UINT w = 0, h = 0;
  if (SUCCEEDED(hr)) {
    stage = L"GetSize";
    hr = conv->lpVtbl->GetSize(conv, &w, &h);
  }
  size_t stride = (size_t)w * 4u;
  size_t bytes = 0;
  if (h && stride > SIZE_MAX / (size_t)h)
    hr = WINCODEC_ERR_VALUEOVERFLOW;
  if (SUCCEEDED(hr))
    bytes = stride * (size_t)h;
  if (SUCCEEDED(hr) && (stride > UINT32_MAX || bytes > UINT32_MAX))
    hr = WINCODEC_ERR_VALUEOVERFLOW;
  if (SUCCEEDED(hr)) {
    pix = (uint8_t *)malloc(bytes ? bytes : 1);
    if (!pix)
      hr = E_OUTOFMEMORY;
  }
  if (SUCCEEDED(hr)) {
    stage = L"CopyPixels";
    hr = conv->lpVtbl->CopyPixels(conv, NULL, (UINT)stride, (UINT)bytes, pix);
  }
  if (SUCCEEDED(hr) && self_test &&
      (w != 1 || h != 1 || bytes != 4 || memcmp(pix, fixture_pixels, 4))) {
    stage = L"ValidatePixels";
    hr = WINCODEC_ERR_BADIMAGE;
  }
  if (SUCCEEDED(hr)) {
    printf("wic decode ok: %ux%u rgba crc32=%08x\n", w, h, crc32(pix, bytes));
    rc = 0;
  } else {
    fwprintf(stderr, L"WIC decode failed at %ls: 0x%08lx\n", stage,
             (unsigned long)hr);
  }

  free(pix);
  if (conv)
    conv->lpVtbl->Release(conv);
  if (frame)
    frame->lpVtbl->Release(frame);
  if (dec)
    dec->lpVtbl->Release(dec);
  if (stream)
    stream->lpVtbl->Release(stream);
  if (fac)
    fac->lpVtbl->Release(fac);
  if (qlicDll)
    FreeLibrary(qlicDll);
  if (co)
    CoUninitialize();
  if (self_test)
    DeleteFileW(fixture);
  return rc;
}
