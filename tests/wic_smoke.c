#include <windows.h>
#include <wincodec.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wincodecsdk.h>

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
static const uint8_t fixture_pixels[4] = {11, 22, 33, 44};

typedef HRESULT(__stdcall *DllGetClassObjectFn)(REFCLSID, REFIID, void **);
typedef HRESULT(__stdcall *DllCanUnloadNowFn)(void);

typedef struct {
  IStream iface;
  volatile LONG refs;
  const uint8_t *data;
  size_t size;
  uint64_t reported_size;
  uint64_t position;
  ULONG max_read;
  unsigned reads;
  int seekable;
} HostileStream;

static HRESULT STDMETHODCALLTYPE hostile_qi(IStream *iface, REFIID riid,
                                            void **result) {
  if (!result)
    return E_POINTER;
  *result = NULL;
  if (!IsEqualIID(riid, &IID_IUnknown) && !IsEqualIID(riid, &IID_ISequentialStream) &&
      !IsEqualIID(riid, &IID_IStream))
    return E_NOINTERFACE;
  *result = iface;
  iface->lpVtbl->AddRef(iface);
  return S_OK;
}

static ULONG STDMETHODCALLTYPE hostile_add(IStream *iface) {
  HostileStream *stream = (HostileStream *)iface;
  return (ULONG)InterlockedIncrement(&stream->refs);
}

static ULONG STDMETHODCALLTYPE hostile_release(IStream *iface) {
  HostileStream *stream = (HostileStream *)iface;
  return (ULONG)InterlockedDecrement(&stream->refs);
}

static HRESULT STDMETHODCALLTYPE hostile_read(IStream *iface, void *buffer,
                                              ULONG requested, ULONG *read) {
  HostileStream *stream = (HostileStream *)iface;
  if (!buffer && requested)
    return STG_E_INVALIDPOINTER;
  if (read)
    *read = 0u;
  ++stream->reads;
  if (stream->position >= stream->size)
    return S_FALSE;
  size_t available = stream->size - (size_t)stream->position;
  size_t amount = requested < available ? requested : available;
  if (stream->max_read && amount > stream->max_read)
    amount = stream->max_read;
  if (amount)
    memcpy(buffer, stream->data + (size_t)stream->position, amount);
  stream->position += amount;
  if (read)
    *read = (ULONG)amount;
  return amount == requested ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE hostile_write(IStream *iface,
                                               const void *buffer, ULONG size,
                                               ULONG *written) {
  (void)iface;
  (void)buffer;
  (void)size;
  if (written)
    *written = 0u;
  return STG_E_ACCESSDENIED;
}

static HRESULT STDMETHODCALLTYPE hostile_seek(IStream *iface,
                                              LARGE_INTEGER move,
                                              DWORD origin,
                                              ULARGE_INTEGER *position) {
  HostileStream *stream = (HostileStream *)iface;
  if (!stream->seekable)
    return STG_E_INVALIDFUNCTION;
  int64_t base = origin == STREAM_SEEK_SET
                     ? 0
                     : origin == STREAM_SEEK_CUR
                           ? (int64_t)stream->position
                           : origin == STREAM_SEEK_END
                                 ? (int64_t)stream->reported_size
                                 : INT64_MIN;
  if (base == INT64_MIN || (move.QuadPart < 0 && base < -move.QuadPart) ||
      (move.QuadPart > 0 && base > INT64_MAX - move.QuadPart))
    return STG_E_INVALIDFUNCTION;
  int64_t next = base + move.QuadPart;
  if (next < 0)
    return STG_E_INVALIDFUNCTION;
  stream->position = (uint64_t)next;
  if (position)
    position->QuadPart = stream->position;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE hostile_set_size(IStream *iface,
                                                  ULARGE_INTEGER size) {
  (void)iface;
  (void)size;
  return STG_E_ACCESSDENIED;
}

static HRESULT STDMETHODCALLTYPE hostile_copy(IStream *iface, IStream *target,
                                              ULARGE_INTEGER size,
                                              ULARGE_INTEGER *read,
                                              ULARGE_INTEGER *written) {
  (void)iface;
  (void)target;
  (void)size;
  if (read)
    read->QuadPart = 0u;
  if (written)
    written->QuadPart = 0u;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE hostile_commit(IStream *iface, DWORD flags) {
  (void)iface;
  (void)flags;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE hostile_revert(IStream *iface) {
  (void)iface;
  return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE hostile_lock(IStream *iface,
                                              ULARGE_INTEGER offset,
                                              ULARGE_INTEGER size,
                                              DWORD type) {
  (void)iface;
  (void)offset;
  (void)size;
  (void)type;
  return STG_E_INVALIDFUNCTION;
}

static HRESULT STDMETHODCALLTYPE hostile_unlock(IStream *iface,
                                                ULARGE_INTEGER offset,
                                                ULARGE_INTEGER size,
                                                DWORD type) {
  (void)iface;
  (void)offset;
  (void)size;
  (void)type;
  return STG_E_INVALIDFUNCTION;
}

static HRESULT STDMETHODCALLTYPE hostile_stat(IStream *iface, STATSTG *stat,
                                              DWORD flags) {
  HostileStream *stream = (HostileStream *)iface;
  (void)flags;
  if (!stat)
    return STG_E_INVALIDPOINTER;
  memset(stat, 0, sizeof(*stat));
  stat->type = STGTY_STREAM;
  stat->cbSize.QuadPart = stream->reported_size;
  stat->grfMode = STGM_READ;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE hostile_clone(IStream *iface,
                                               IStream **clone) {
  (void)iface;
  if (!clone)
    return E_POINTER;
  *clone = NULL;
  return E_NOTIMPL;
}

static IStreamVtbl hostile_vtbl = {
    hostile_qi,     hostile_add,    hostile_release, hostile_read,
    hostile_write,  hostile_seek,   hostile_set_size, hostile_copy,
    hostile_commit, hostile_revert, hostile_lock,    hostile_unlock,
    hostile_stat,   hostile_clone};

static void hostile_init(HostileStream *stream, const uint8_t *data,
                         size_t size, uint64_t reported_size, ULONG max_read,
                         int seekable, uint64_t position) {
  memset(stream, 0, sizeof(*stream));
  stream->iface.lpVtbl = &hostile_vtbl;
  stream->refs = 1;
  stream->data = data;
  stream->size = size;
  stream->reported_size = reported_size;
  stream->max_read = max_read;
  stream->seekable = seekable;
  stream->position = position;
}

static HRESULT validate_metadata(IWICBitmapFrameDecode *frame,
                                 UINT minimum_count, int require_standard) {
  IWICMetadataBlockReader *blocks = NULL;
  IWICMetadataQueryReader *query = NULL;
  IEnumUnknown *enumerator = NULL;
  IUnknown *frame_identity = NULL;
  IUnknown *block_identity = NULL;
  HRESULT hr = frame->lpVtbl->QueryInterface(
      frame, &IID_IWICMetadataBlockReader, (void **)&blocks);
  GUID container = {0};
  UINT count = 0;
  int found_xmp = 0;
  int found_exif = 0;
  if (SUCCEEDED(hr))
    hr = blocks->lpVtbl->GetContainerFormat(blocks, &container);
  if (SUCCEEDED(hr) &&
      !IsEqualGUID(&container, &GUID_ContainerFormatQlic))
    hr = E_FAIL;
  if (SUCCEEDED(hr))
    hr = blocks->lpVtbl->GetCount(blocks, &count);
  if (SUCCEEDED(hr) && count < minimum_count)
    hr = E_FAIL;
  if (SUCCEEDED(hr))
    hr = frame->lpVtbl->QueryInterface(frame, &IID_IUnknown,
                                       (void **)&frame_identity);
  if (SUCCEEDED(hr))
    hr = blocks->lpVtbl->QueryInterface(blocks, &IID_IUnknown,
                                         (void **)&block_identity);
  if (SUCCEEDED(hr) && frame_identity != block_identity)
    hr = E_FAIL;

  for (UINT index = 0; index < count && SUCCEEDED(hr); ++index) {
    IWICMetadataReader *reader = NULL;
    GUID format = {0};
    UINT entries = 0;
    hr = blocks->lpVtbl->GetReaderByIndex(blocks, index, &reader);
    if (SUCCEEDED(hr))
      hr = reader->lpVtbl->GetMetadataFormat(reader, &format);
    if (SUCCEEDED(hr) && IsEqualGUID(&format, &GUID_MetadataFormatXMP))
      found_xmp = 1;
    if (SUCCEEDED(hr) &&
        (IsEqualGUID(&format, &GUID_MetadataFormatIfd) ||
         IsEqualGUID(&format, &GUID_MetadataFormatExif)))
      found_exif = 1;
    if (SUCCEEDED(hr))
      hr = reader->lpVtbl->GetCount(reader, &entries);
    if (SUCCEEDED(hr) &&
        (IsEqualGUID(&format, &GUID_MetadataFormatXMP) ||
         IsEqualGUID(&format, &GUID_MetadataFormatIfd) ||
         IsEqualGUID(&format, &GUID_MetadataFormatExif)) &&
        !entries)
      hr = E_FAIL;
    if (reader)
      reader->lpVtbl->Release(reader);
  }
  if (SUCCEEDED(hr) && require_standard && (!found_xmp || !found_exif))
    hr = E_FAIL;

  UINT enumerated = 0;
  if (SUCCEEDED(hr))
    hr = blocks->lpVtbl->GetEnumerator(blocks, &enumerator);
  while (SUCCEEDED(hr)) {
    IUnknown *item = NULL;
    ULONG fetched = 0;
    HRESULT next = enumerator->lpVtbl->Next(enumerator, 1u, &item, &fetched);
    if (next == S_FALSE) {
      if (item || fetched)
        hr = E_FAIL;
      break;
    }
    if (next != S_OK || !item || fetched != 1u) {
      if (item)
        item->lpVtbl->Release(item);
      hr = E_FAIL;
      break;
    }
    item->lpVtbl->Release(item);
    ++enumerated;
  }
  if (SUCCEEDED(hr) && enumerated != count)
    hr = E_FAIL;

  if (SUCCEEDED(hr))
    hr = frame->lpVtbl->GetMetadataQueryReader(frame, &query);
  if (SUCCEEDED(hr))
    hr = query->lpVtbl->GetContainerFormat(query, &container);
  if (SUCCEEDED(hr) &&
      !IsEqualGUID(&container, &GUID_ContainerFormatQlic))
    hr = E_FAIL;
  if (SUCCEEDED(hr) && require_standard) {
    PROPVARIANT orientation;
    PropVariantInit(&orientation);
    hr = query->lpVtbl->GetMetadataByName(
        query, L"/ifd/{ushort=274}", &orientation);
    if (SUCCEEDED(hr) &&
        (orientation.vt != VT_UI2 || orientation.uiVal != 6u))
      hr = E_FAIL;
    PropVariantClear(&orientation);
  }
  if (SUCCEEDED(hr) && require_standard) {
    PROPVARIANT creator;
    PropVariantInit(&creator);
    hr = query->lpVtbl->GetMetadataByName(
        query, L"/xmp/xmp:CreatorTool", &creator);
    if (SUCCEEDED(hr) &&
        (creator.vt != VT_LPWSTR || !creator.pwszVal ||
         wcscmp(creator.pwszVal, L"QLIC WIC Test")))
      hr = E_FAIL;
    PropVariantClear(&creator);
  }

  if (query)
    query->lpVtbl->Release(query);
  if (enumerator)
    enumerator->lpVtbl->Release(enumerator);
  if (block_identity)
    block_identity->lpVtbl->Release(block_identity);
  if (frame_identity)
    frame_identity->lpVtbl->Release(frame_identity);
  if (blocks)
    blocks->lpVtbl->Release(blocks);
  return hr;
}

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

static void make_fixture_bytes(uint8_t file[36]) {
  memset(file, 0, 36u);
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
}

static int write_fixture(wchar_t *path) {
  wchar_t dir[MAX_PATH];
  if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"qlc", 0, path))
    return 0;
  uint8_t file[36] = {0};
  make_fixture_bytes(file);
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

static HRESULT validate_hostile_streams(IWICBitmapDecoder *decoder) {
  uint8_t fixture[36];
  make_fixture_bytes(fixture);
  HostileStream fragmented;
  hostile_init(&fragmented, fixture, sizeof(fixture), sizeof(fixture), 1u, 1,
               7u);
  DWORD capability = 0u;
  HRESULT hr = decoder->lpVtbl->QueryCapability(
      decoder, &fragmented.iface, &capability);
  if (FAILED(hr) ||
      !(capability & WICBitmapDecoderCapabilityCanDecodeAllImages) ||
      fragmented.position != 7u || fragmented.reads != sizeof(fixture))
    return E_FAIL;

  HostileStream truncated;
  hostile_init(&truncated, fixture, sizeof(fixture), sizeof(fixture) + 64u,
               0u, 1, 5u);
  capability = 1u;
  hr = decoder->lpVtbl->QueryCapability(decoder, &truncated.iface,
                                        &capability);
  if (FAILED(hr) || capability || truncated.position != 5u)
    return E_FAIL;

  uint8_t budget_data[5000] = {0};
  memcpy(budget_data, fixture, sizeof(fixture));
  HostileStream budget;
  hostile_init(&budget, budget_data, sizeof(budget_data), sizeof(budget_data),
               1u, 1, 3u);
  capability = 1u;
  hr = decoder->lpVtbl->QueryCapability(decoder, &budget.iface, &capability);
  if (FAILED(hr) || capability || budget.position != 3u ||
      budget.reads != 4096u)
    return E_FAIL;

  HostileStream nonseekable;
  hostile_init(&nonseekable, fixture, sizeof(fixture), sizeof(fixture), 0u, 0,
               0u);
  capability = 1u;
  hr = decoder->lpVtbl->QueryCapability(decoder, &nonseekable.iface,
                                        &capability);
  if (FAILED(hr) || capability || nonseekable.reads)
    return E_FAIL;
  return S_OK;
}

typedef struct {
  IWICBitmapDecoder *decoder;
  HostileStream stream;
  HANDLE start;
  HRESULT result;
} InitializeThread;

static DWORD WINAPI initialize_thread(void *parameter) {
  InitializeThread *thread = (InitializeThread *)parameter;
  if (WaitForSingleObject(thread->start, 10000u) != WAIT_OBJECT_0) {
    thread->result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    return 1u;
  }
  thread->result = thread->decoder->lpVtbl->Initialize(
      thread->decoder, &thread->stream.iface, WICDecodeMetadataCacheOnLoad);
  return 0u;
}

static HRESULT validate_concurrent_initialize(void) {
  IWICBitmapDecoder *decoder = NULL;
  HMODULE module = NULL;
  HRESULT hr = create_adjacent_decoder(&decoder, &module);
  if (FAILED(hr))
    return hr;
  uint8_t fixture[36];
  make_fixture_bytes(fixture);
  HANDLE start = CreateEventW(NULL, TRUE, FALSE, NULL);
  HANDLE handles[8] = {0};
  InitializeThread threads[8];
  memset(threads, 0, sizeof(threads));
  if (!start)
    hr = HRESULT_FROM_WIN32(GetLastError());
  for (unsigned index = 0; index < 8u && SUCCEEDED(hr); ++index) {
    threads[index].decoder = decoder;
    threads[index].start = start;
    threads[index].result = E_UNEXPECTED;
    hostile_init(&threads[index].stream, fixture, sizeof(fixture),
                 sizeof(fixture), 0u, 1, 0u);
    handles[index] =
        CreateThread(NULL, 0u, initialize_thread, threads + index, 0u, NULL);
    if (!handles[index])
      hr = HRESULT_FROM_WIN32(GetLastError());
  }
  if (SUCCEEDED(hr) && !SetEvent(start))
    hr = HRESULT_FROM_WIN32(GetLastError());
  if (SUCCEEDED(hr) &&
      WaitForMultipleObjects(8u, handles, TRUE, 30000u) != WAIT_OBJECT_0)
    hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  unsigned successes = 0u, wrong_state = 0u;
  for (unsigned index = 0; index < 8u; ++index) {
    if (handles[index]) {
      if (WaitForSingleObject(handles[index], 30000u) != WAIT_OBJECT_0)
        hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
      CloseHandle(handles[index]);
    }
    successes += threads[index].result == S_OK;
    wrong_state += threads[index].result == WINCODEC_ERR_WRONGSTATE;
  }
  if (start)
    CloseHandle(start);
  if (SUCCEEDED(hr) && (successes != 1u || wrong_state != 7u))
    hr = E_FAIL;
  decoder->lpVtbl->Release(decoder);
  FreeLibrary(module);
  return hr;
}

static HRESULT probe_registered_catalog(IWICImagingFactory *fac,
                                        const wchar_t *path) {
  IWICComponentInfo *component = NULL;
  IWICBitmapDecoderInfo *decoder_info = NULL;
  IWICBitmapDecoder *registered_decoder = NULL;
  IWICStream *stream = NULL;
  IEnumUnknown *components = NULL;
  IUnknown *unknown = NULL;
  HRESULT hr = fac->lpVtbl->CreateComponentInfo(
      fac, &CLSID_QlicWicDecoder, &component);
  fwprintf(stderr, L"CreateComponentInfo: 0x%08lx\n", (unsigned long)hr);
  if (SUCCEEDED(hr)) {
    DWORD signing = 0;
    HRESULT signing_hr = component->lpVtbl->GetSigningStatus(component,
                                                              &signing);
    fwprintf(stderr, L"GetSigningStatus: 0x%08lx flags=0x%08lx\n",
             (unsigned long)signing_hr, (unsigned long)signing);
  }
  if (SUCCEEDED(hr)) {
    hr = component->lpVtbl->QueryInterface(
        component, &IID_IWICBitmapDecoderInfo, (void **)&decoder_info);
    fwprintf(stderr, L"QI(IWICBitmapDecoderInfo): 0x%08lx\n",
             (unsigned long)hr);
  }
  if (SUCCEEDED(hr)) {
    hr = fac->lpVtbl->CreateStream(fac, &stream);
    if (SUCCEEDED(hr))
      hr = stream->lpVtbl->InitializeFromFilename(stream, path, GENERIC_READ);
  }
  if (SUCCEEDED(hr)) {
    BOOL matches = FALSE;
    hr = decoder_info->lpVtbl->MatchesPattern(
        decoder_info, (IStream *)stream, &matches);
    fwprintf(stderr, L"MatchesPattern: 0x%08lx match=%d\n",
             (unsigned long)hr, matches);
    if (SUCCEEDED(hr) && !matches)
      hr = WINCODEC_ERR_COMPONENTNOTFOUND;
  }
  if (SUCCEEDED(hr)) {
    IWICBitmapDecoder *com_decoder = NULL;
    HRESULT com_hr = CoCreateInstance(
        &CLSID_QlicWicDecoder, NULL, CLSCTX_INPROC_SERVER,
        &IID_IWICBitmapDecoder, (void **)&com_decoder);
    fwprintf(stderr, L"CoCreateInstance(QLIC): 0x%08lx\n",
             (unsigned long)com_hr);
    if (com_decoder)
      com_decoder->lpVtbl->Release(com_decoder);
    hr = decoder_info->lpVtbl->CreateInstance(decoder_info,
                                               &registered_decoder);
    fwprintf(stderr, L"DecoderInfo.CreateInstance: 0x%08lx\n",
             (unsigned long)hr);
  }
  if (SUCCEEDED(hr)) {
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    hr = stream->lpVtbl->Seek(stream, zero, STREAM_SEEK_SET, NULL);
    if (SUCCEEDED(hr))
      hr = registered_decoder->lpVtbl->Initialize(
          registered_decoder, (IStream *)stream,
          WICDecodeMetadataCacheOnLoad);
    fwprintf(stderr, L"RegisteredDecoder.Initialize: 0x%08lx\n",
             (unsigned long)hr);
  }
  if (SUCCEEDED(hr)) {
    hr = fac->lpVtbl->CreateComponentEnumerator(
        fac, WICDecoder, WICComponentEnumerateDefault, &components);
  }
  if (SUCCEEDED(hr)) {
    BOOL found = FALSE;
    ULONG fetched = 0;
    while (components->lpVtbl->Next(components, 1, &unknown, &fetched) == S_OK) {
      IWICComponentInfo *candidate = NULL;
      CLSID clsid = {0};
      HRESULT candidate_hr = unknown->lpVtbl->QueryInterface(
          unknown, &IID_IWICComponentInfo, (void **)&candidate);
      if (SUCCEEDED(candidate_hr))
        candidate_hr = candidate->lpVtbl->GetCLSID(candidate, &clsid);
      if (SUCCEEDED(candidate_hr) &&
          IsEqualCLSID(&clsid, &CLSID_QlicWicDecoder))
        found = TRUE;
      if (candidate)
        candidate->lpVtbl->Release(candidate);
      unknown->lpVtbl->Release(unknown);
      unknown = NULL;
      if (found)
        break;
    }
    fwprintf(stderr, L"CreateComponentEnumerator found QLIC: %d\n", found);
    if (!found)
      hr = WINCODEC_ERR_COMPONENTNOTFOUND;
  }
  if (unknown)
    unknown->lpVtbl->Release(unknown);
  if (components)
    components->lpVtbl->Release(components);
  if (stream)
    stream->lpVtbl->Release(stream);
  if (registered_decoder)
    registered_decoder->lpVtbl->Release(registered_decoder);
  if (decoder_info)
    decoder_info->lpVtbl->Release(decoder_info);
  if (component)
    component->lpVtbl->Release(component);
  return hr;
}

int wmain(int argc, wchar_t **argv) {
  int self_test = argc == 2 && !_wcsicmp(argv[1], L"--self-test");
  int wide_test = argc >= 3 && !_wcsicmp(argv[1], L"--direct-wide");
  int hdr8_test = argc >= 3 && !_wcsicmp(argv[1], L"--direct-hdr8");
  int hdr10_test = argc >= 3 && !_wcsicmp(argv[1], L"--direct-hdr10");
  int hdr16_test = argc >= 3 && !_wcsicmp(argv[1], L"--direct-hdr16");
  int premultiplied16_test =
      argc >= 3 && !_wcsicmp(argv[1], L"--direct-hdr16-premultiplied");
  int metadata_test =
      argc >= 3 && !_wcsicmp(argv[1], L"--direct-metadata");
  int resolution_test =
      argc >= 3 && !_wcsicmp(argv[1], L"--direct-resolution");
  int reject_test =
      argc >= 3 && !_wcsicmp(argv[1], L"--direct-reject");
  int catalog_test =
      argc >= 3 && !_wcsicmp(argv[1], L"--catalog");
  int direct = self_test || wide_test || hdr8_test || hdr10_test ||
               hdr16_test ||
               premultiplied16_test || metadata_test || resolution_test ||
               reject_test ||
               (argc >= 3 && !_wcsicmp(argv[1], L"--direct"));
  wchar_t fixture[MAX_PATH] = {0};
  const wchar_t *path =
      (direct && !self_test) || catalog_test
          ? argv[2]
          : (argc >= 2 ? argv[1] : NULL);
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
    if (catalog_test) {
      stage = L"ProbeRegisteredCatalog";
      hr = probe_registered_catalog(fac, path);
      if (SUCCEEDED(hr)) {
        printf("registered WIC catalog ok\n");
        rc = 0;
        goto cleanup;
      }
    } else if (direct) {
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
        LARGE_INTEGER zero;
        ULARGE_INTEGER before = {0}, after = {0};
        DWORD capability = 0;
        zero.QuadPart = 0;
        stage = L"QueryCapability.PositionBefore";
        hr = stream->lpVtbl->Seek(stream, zero, STREAM_SEEK_CUR, &before);
        if (SUCCEEDED(hr)) {
          stage = L"QueryCapability";
          hr = dec->lpVtbl->QueryCapability(dec, (IStream *)stream,
                                            &capability);
        }
        if (SUCCEEDED(hr)) {
          stage = L"QueryCapability.PositionAfter";
          hr = stream->lpVtbl->Seek(stream, zero, STREAM_SEEK_CUR, &after);
        }
        if (SUCCEEDED(hr) &&
            (before.QuadPart != after.QuadPart ||
             (reject_test ? capability != 0u
                          : !(capability &
                              WICBitmapDecoderCapabilityCanDecodeAllImages)) ||
             (!reject_test && metadata_test &&
              !(capability &
                WICBitmapDecoderCapabilityCanEnumerateMetadata)))) {
          stage = L"ValidateQueryCapability";
          hr = E_FAIL;
        }
      }
      if (SUCCEEDED(hr) && reject_test) {
        HRESULT rejected = dec->lpVtbl->Initialize(
            dec, (IStream *)stream, WICDecodeMetadataCacheOnLoad);
        if (rejected != WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT) {
          stage = L"RejectUnsupported.Initialize";
          hr = E_FAIL;
          goto cleanup;
        }
        printf("wic correctly rejected unsupported native image\n");
        rc = 0;
        goto cleanup;
      }
      if (SUCCEEDED(hr) && self_test) {
        BYTE invalid[] = {'Q', 'L', 'I', 'C'};
        IWICStream *invalid_stream = NULL;
        DWORD capability = 1u;
        stage = L"CreateInvalidStream";
        hr = fac->lpVtbl->CreateStream(fac, &invalid_stream);
        if (SUCCEEDED(hr)) {
          stage = L"InitializeInvalidStream";
          hr = invalid_stream->lpVtbl->InitializeFromMemory(
              invalid_stream, invalid, (DWORD)sizeof(invalid));
        }
        if (SUCCEEDED(hr)) {
          stage = L"QueryInvalidCapability";
          hr = dec->lpVtbl->QueryCapability(
              dec, (IStream *)invalid_stream, &capability);
        }
        if (invalid_stream)
          invalid_stream->lpVtbl->Release(invalid_stream);
        if (SUCCEEDED(hr) && capability) {
          stage = L"ValidateInvalidCapability";
          hr = E_FAIL;
        }
      }
      if (SUCCEEDED(hr) && self_test) {
        stage = L"HostileStreams";
        hr = validate_hostile_streams(dec);
      }
      if (SUCCEEDED(hr) && self_test) {
        stage = L"ConcurrentInitialize";
        hr = validate_concurrent_initialize();
      }
      if (SUCCEEDED(hr)) {
        stage = L"Decoder.Initialize";
        hr = dec->lpVtbl->Initialize(dec, (IStream *)stream,
                                     WICDecodeMetadataCacheOnLoad);
      }
      if (SUCCEEDED(hr)) {
        HRESULT second = dec->lpVtbl->Initialize(
            dec, (IStream *)stream, WICDecodeMetadataCacheOnLoad);
        if (second != WINCODEC_ERR_WRONGSTATE) {
          stage = L"Decoder.InitializeTwice";
          hr = E_FAIL;
        }
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
    if (SUCCEEDED(hr)) {
      IWICBitmapSource *thumbnail = NULL;
      HRESULT probe = frame->lpVtbl->GetThumbnail(frame, &thumbnail);
      if (thumbnail)
        thumbnail->lpVtbl->Release(thumbnail);
      if (probe != WINCODEC_ERR_CODECNOTHUMBNAIL || thumbnail) {
        stage = L"Frame.GetThumbnail";
        hr = E_FAIL;
      }
    }
    if (SUCCEEDED(hr)) {
      IWICMetadataBlockReader *blocks = NULL;
      UINT metadata_count = 1u;
      stage = L"Frame.EmptyMetadataBlocks";
      hr = frame->lpVtbl->QueryInterface(
          frame, &IID_IWICMetadataBlockReader, (void **)&blocks);
      if (SUCCEEDED(hr))
        hr = blocks->lpVtbl->GetCount(blocks, &metadata_count);
      if (blocks)
        blocks->lpVtbl->Release(blocks);
      if (SUCCEEDED(hr) && metadata_count != 0u)
        hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) {
      IWICMetadataQueryReader *query = NULL;
      HRESULT probe = frame->lpVtbl->GetMetadataQueryReader(frame, &query);
      if (query)
        query->lpVtbl->Release(query);
      if (probe != WINCODEC_ERR_UNSUPPORTEDOPERATION || query) {
        stage = L"Frame.EmptyMetadataQueryReader";
        hr = E_FAIL;
      }
    }
    if (SUCCEEDED(hr)) {
      double x = 0.0, y = 0.0;
      stage = L"Frame.DefaultResolution";
      hr = frame->lpVtbl->GetResolution(frame, &x, &y);
      if (SUCCEEDED(hr) && (x != 96.0 || y != 96.0))
        hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) {
      UINT decoder_contexts = 1u, frame_contexts = 1u;
      stage = L"Decoder.EmptyColorContexts";
      hr = dec->lpVtbl->GetColorContexts(dec, 0u, NULL, &decoder_contexts);
      if (SUCCEEDED(hr)) {
        stage = L"Frame.EmptyColorContexts";
        hr = frame->lpVtbl->GetColorContexts(frame, 0u, NULL,
                                              &frame_contexts);
      }
      if (SUCCEEDED(hr) && (decoder_contexts || frame_contexts))
        hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) {
      IWICBitmapSource *thumbnail = NULL;
      HRESULT probe = dec->lpVtbl->GetThumbnail(dec, &thumbnail);
      if (thumbnail)
        thumbnail->lpVtbl->Release(thumbnail);
      if (probe != WINCODEC_ERR_CODECNOTHUMBNAIL || thumbnail) {
        stage = L"Decoder.GetThumbnail";
        hr = E_FAIL;
      }
    }
    if (SUCCEEDED(hr)) {
      IWICBitmapSource *preview = NULL;
      HRESULT probe = dec->lpVtbl->GetPreview(dec, &preview);
      if (preview)
        preview->lpVtbl->Release(preview);
      if (probe != WINCODEC_ERR_UNSUPPORTEDOPERATION || preview) {
        stage = L"Decoder.GetPreview";
        hr = E_FAIL;
      }
    }
  }
  if (SUCCEEDED(hr) && metadata_test) {
    stage = L"ValidateMetadata";
    hr = validate_metadata(frame, 2u, 1);
  }
  if (SUCCEEDED(hr) && resolution_test) {
    double x = 0.0, y = 0.0;
    stage = L"ValidateResolution";
    hr = frame->lpVtbl->GetResolution(frame, &x, &y);
    if (SUCCEEDED(hr) &&
        (fabs(x - 11811.0 * 0.0254) > 0.0001 ||
         fabs(y - 5906.0 * 0.0254) > 0.0001))
      hr = E_FAIL;
  }
  if (SUCCEEDED(hr) && wide_test) {
    WICPixelFormatGUID pixel_format = {0};
    uint16_t direct_pixels[17u * 9u] = {0};
    UINT width = 0, height = 0;
    stage = L"WideFrame.GetSize";
    hr = frame->lpVtbl->GetSize(frame, &width, &height);
    if (SUCCEEDED(hr) && (width != 17u || height != 9u))
      hr = WINCODEC_ERR_BADIMAGE;
    if (SUCCEEDED(hr)) {
      stage = L"WideFrame.GetPixelFormat";
      hr = frame->lpVtbl->GetPixelFormat(frame, &pixel_format);
    }
    if (SUCCEEDED(hr) &&
        !IsEqualGUID(&pixel_format, &GUID_WICPixelFormat16bppGray))
      hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    if (SUCCEEDED(hr)) {
      stage = L"WideFrame.CopyPixels";
      hr = frame->lpVtbl->CopyPixels(
          frame, NULL, 17u * sizeof(uint16_t), sizeof(direct_pixels),
          (BYTE *)direct_pixels);
    }
    for (UINT y = 0; y < 9u && SUCCEEDED(hr); ++y) {
      for (UINT x = 0; x < 17u; ++x) {
        uint16_t expected =
            (uint16_t)(x * 3001u + y * 7919u + 123u);
        if (direct_pixels[(size_t)y * 17u + x] != expected) {
          stage = L"ValidateWidePixels";
          hr = WINCODEC_ERR_BADIMAGE;
          break;
        }
      }
    }
  }
  if (SUCCEEDED(hr) && hdr8_test) {
    static const uint8_t expected[12] = {
        0, 1, 255, 17, 127, 254, 255, 128, 2, 33, 66, 99};
    WICPixelFormatGUID pixel_format = {0};
    uint8_t direct_pixels[12] = {0};
    UINT width = 0, height = 0;
    stage = L"Hdr8Frame.GetSize";
    hr = frame->lpVtbl->GetSize(frame, &width, &height);
    if (SUCCEEDED(hr) && (width != 2u || height != 2u))
      hr = WINCODEC_ERR_BADIMAGE;
    if (SUCCEEDED(hr)) {
      stage = L"Hdr8Frame.GetPixelFormat";
      hr = frame->lpVtbl->GetPixelFormat(frame, &pixel_format);
    }
    if (SUCCEEDED(hr) &&
        !IsEqualGUID(&pixel_format, &GUID_WICPixelFormat24bppRGB))
      hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    if (SUCCEEDED(hr)) {
      stage = L"Hdr8Frame.CopyPixels";
      hr = frame->lpVtbl->CopyPixels(frame, NULL, 6u, sizeof(direct_pixels),
                                     direct_pixels);
    }
    if (SUCCEEDED(hr) && memcmp(direct_pixels, expected, sizeof(expected)))
      hr = WINCODEC_ERR_BADIMAGE;
  }
  if (SUCCEEDED(hr) && hdr10_test) {
    static const uint16_t samples[12] = {
        0u, 1u, 1023u, 1023u, 512u, 256u,
        17u, 511u, 1000u, 64u, 900u, 333u};
    WICPixelFormatGUID pixel_format = {0};
    uint32_t direct_pixels[4] = {0};
    UINT width = 0, height = 0;
    stage = L"Hdr10Frame.GetSize";
    hr = frame->lpVtbl->GetSize(frame, &width, &height);
    if (SUCCEEDED(hr) && (width != 2u || height != 2u))
      hr = WINCODEC_ERR_BADIMAGE;
    if (SUCCEEDED(hr)) {
      stage = L"Hdr10Frame.GetPixelFormat";
      hr = frame->lpVtbl->GetPixelFormat(frame, &pixel_format);
    }
    if (SUCCEEDED(hr) && !IsEqualGUID(
                              &pixel_format,
                              &GUID_WICPixelFormat32bppR10G10B10A2HDR10))
      hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    if (SUCCEEDED(hr)) {
      stage = L"Hdr10Frame.CopyPixels";
      hr = frame->lpVtbl->CopyPixels(frame, NULL, 2u * sizeof(uint32_t),
                                     sizeof(direct_pixels),
                                     (BYTE *)direct_pixels);
    }
    for (UINT pixel = 0; pixel < 4u && SUCCEEDED(hr); ++pixel) {
      uint32_t expected = ((uint32_t)samples[(size_t)pixel * 3u] << 22) |
                          ((uint32_t)samples[(size_t)pixel * 3u + 1u] << 12) |
                          ((uint32_t)samples[(size_t)pixel * 3u + 2u] << 2) |
                          3u;
      if (direct_pixels[pixel] != expected) {
        stage = L"ValidateHdr10Pixels";
        hr = WINCODEC_ERR_BADIMAGE;
      }
    }
  }
  if (SUCCEEDED(hr) && (hdr16_test || premultiplied16_test)) {
    WICPixelFormatGUID pixel_format = {0};
    uint16_t direct_pixels[11u * 7u * 4u] = {0};
    UINT width = 0, height = 0;
    stage = L"Hdr16Frame.GetSize";
    hr = frame->lpVtbl->GetSize(frame, &width, &height);
    if (SUCCEEDED(hr) && (width != 11u || height != 7u))
      hr = WINCODEC_ERR_BADIMAGE;
    if (SUCCEEDED(hr)) {
      stage = L"Hdr16Frame.GetPixelFormat";
      hr = frame->lpVtbl->GetPixelFormat(frame, &pixel_format);
    }
    const WICPixelFormatGUID *expected_format =
        premultiplied16_test ? &GUID_WICPixelFormat64bppPRGBA
                             : &GUID_WICPixelFormat64bppRGBA;
    if (SUCCEEDED(hr) && !IsEqualGUID(&pixel_format, expected_format))
      hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    if (SUCCEEDED(hr)) {
      stage = L"Hdr16Frame.CopyPixels";
      hr = frame->lpVtbl->CopyPixels(
          frame, NULL, 11u * 4u * sizeof(uint16_t), sizeof(direct_pixels),
          (BYTE *)direct_pixels);
    }
    for (UINT y = 0; y < 7u && SUCCEEDED(hr); ++y) {
      for (UINT x = 0; x < 11u; ++x) {
        for (UINT channel = 0; channel < 4u; ++channel) {
          uint16_t expected =
              (uint16_t)(x * 3001u + y * 7919u + channel * 11003u + 123u);
          size_t index = ((size_t)y * 11u + x) * 4u + channel;
          if (direct_pixels[index] != expected) {
            stage = L"ValidateHdr16Pixels";
            hr = WINCODEC_ERR_BADIMAGE;
            break;
          }
        }
      }
    }
  }
  if (SUCCEEDED(hr) && direct) {
    if (self_test && !GetProcAddress(qlicDll, "OpenInPhotos")) {
      stage = L"OpenInPhotos export";
      hr = E_FAIL;
    }
  }
  if (SUCCEEDED(hr) && direct) {
    DllCanUnloadNowFn can_unload =
        (DllCanUnloadNowFn)GetProcAddress(qlicDll, "DllCanUnloadNow");
    if (!can_unload || can_unload() != S_FALSE) {
      stage = L"DllCanUnloadNow.Active";
      hr = E_FAIL;
    }
  }
  if (SUCCEEDED(hr)) {
    stage = L"CreateFormatConverter";
    hr = fac->lpVtbl->CreateFormatConverter(fac, &conv);
  }
  if (SUCCEEDED(hr)) {
    stage = L"FormatConverter.Initialize";
    const WICPixelFormatGUID *format =
        wide_test ? &GUID_WICPixelFormat16bppGray
        : hdr8_test ? &GUID_WICPixelFormat24bppRGB
        : hdr10_test ? &GUID_WICPixelFormat32bppR10G10B10A2HDR10
        : premultiplied16_test ? &GUID_WICPixelFormat64bppPRGBA
        : hdr16_test ? &GUID_WICPixelFormat64bppRGBA
                     : &GUID_WICPixelFormat32bppRGBA;
    hr = conv->lpVtbl->Initialize(
        conv, (IWICBitmapSource *)frame, format,
        WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
  }

  UINT w = 0, h = 0;
  if (SUCCEEDED(hr)) {
    stage = L"GetSize";
    hr = conv->lpVtbl->GetSize(conv, &w, &h);
  }
  size_t stride =
      (size_t)w * (wide_test ? 2u : hdr8_test ? 3u
                            : hdr10_test ? 4u
                            : (hdr16_test || premultiplied16_test) ? 8u
                                                                 : 4u);
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
  if (SUCCEEDED(hr) && wide_test) {
    const uint16_t *wide = (const uint16_t *)pix;
    if (w != 17u || h != 9u || bytes != 17u * 9u * sizeof(uint16_t)) {
      stage = L"ValidateConvertedWideLayout";
      hr = WINCODEC_ERR_BADIMAGE;
    }
    for (UINT y = 0; y < h && SUCCEEDED(hr); ++y) {
      for (UINT x = 0; x < w; ++x) {
        uint16_t expected =
            (uint16_t)(x * 3001u + y * 7919u + 123u);
        if (wide[(size_t)y * w + x] != expected) {
          stage = L"ValidateConvertedWidePixels";
          hr = WINCODEC_ERR_BADIMAGE;
          break;
        }
      }
    }
  }
  if (SUCCEEDED(hr) && hdr8_test) {
    static const uint8_t expected[12] = {
        0, 1, 255, 17, 127, 254, 255, 128, 2, 33, 66, 99};
    if (w != 2u || h != 2u || bytes != sizeof(expected) ||
        memcmp(pix, expected, sizeof(expected))) {
      stage = L"ValidateConvertedHdr8Pixels";
      hr = WINCODEC_ERR_BADIMAGE;
    }
  }
  if (SUCCEEDED(hr) && hdr10_test) {
    static const uint16_t samples[12] = {
        0u, 1u, 1023u, 1023u, 512u, 256u,
        17u, 511u, 1000u, 64u, 900u, 333u};
    const uint32_t *hdr10 = (const uint32_t *)pix;
    if (w != 2u || h != 2u || bytes != 4u * sizeof(uint32_t)) {
      stage = L"ValidateConvertedHdr10Layout";
      hr = WINCODEC_ERR_BADIMAGE;
    }
    for (UINT pixel = 0; pixel < 4u && SUCCEEDED(hr); ++pixel) {
      uint32_t expected = ((uint32_t)samples[(size_t)pixel * 3u] << 22) |
                          ((uint32_t)samples[(size_t)pixel * 3u + 1u] << 12) |
                          ((uint32_t)samples[(size_t)pixel * 3u + 2u] << 2) |
                          3u;
      if (hdr10[pixel] != expected) {
        stage = L"ValidateConvertedHdr10Pixels";
        hr = WINCODEC_ERR_BADIMAGE;
      }
    }
  }
  if (SUCCEEDED(hr) && (hdr16_test || premultiplied16_test)) {
    const uint16_t *hdr16 = (const uint16_t *)pix;
    if (w != 11u || h != 7u || bytes != 11u * 7u * 4u * sizeof(uint16_t)) {
      stage = L"ValidateConvertedHdr16Layout";
      hr = WINCODEC_ERR_BADIMAGE;
    }
    for (UINT y = 0; y < h && SUCCEEDED(hr); ++y) {
      for (UINT x = 0; x < w; ++x) {
        for (UINT channel = 0; channel < 4u; ++channel) {
          uint16_t expected =
              (uint16_t)(x * 3001u + y * 7919u + channel * 11003u + 123u);
          size_t index = ((size_t)y * w + x) * 4u + channel;
          if (hdr16[index] != expected) {
            stage = L"ValidateConvertedHdr16Pixels";
            hr = WINCODEC_ERR_BADIMAGE;
            break;
          }
        }
      }
    }
  }
  if (SUCCEEDED(hr)) {
    printf("wic decode ok: %ux%u rgba crc32=%08x\n", w, h, crc32(pix, bytes));
    rc = 0;
  } else {
    fwprintf(stderr, L"WIC decode failed at %ls: 0x%08lx\n", stage,
             (unsigned long)hr);
  }

cleanup:
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
  if (qlicDll) {
    if (!rc && direct) {
      DllCanUnloadNowFn can_unload =
          (DllCanUnloadNowFn)GetProcAddress(qlicDll, "DllCanUnloadNow");
      if (!can_unload || can_unload() != S_OK) {
        fwprintf(stderr, L"WIC DLL retained COM references after release\n");
        rc = 1;
      }
    }
    FreeLibrary(qlicDll);
  }
  if (co)
    CoUninitialize();
  if (self_test)
    DeleteFileW(fixture);
  return rc;
}
