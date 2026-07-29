#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"

#define DEMO_HTML 101
#define DEMO_MODULE 102
#define DEMO_WORKER 103
#define DEMO_WASM 104
#define DEMO_JXL 201
#define DEMO_BROTLI_COMMON 202
#define DEMO_BROTLI_DEC 203
#define IDLE_LIMIT_MS (10ull * 60ull * 1000ull)
#define MAX_FILE_BYTES UINT64_C(268435456)
#define MAX_PIXELS UINT64_C(33554432)

typedef struct {
  const void *data;
  DWORD size;
  const char *type;
} Asset;

typedef struct {
  wchar_t directory[32768];
  wchar_t jxl[32768];
  wchar_t brotli_common[32768];
  wchar_t brotli_dec[32768];
} CodecFiles;

static int send_all(SOCKET socket, const void *data, size_t size) {
  const char *bytes = (const char *)data;
  while (size) {
    int chunk = size > INT_MAX ? INT_MAX : (int)size;
    int sent = send(socket, bytes, chunk, 0);
    if (sent <= 0)
      return 0;
    bytes += sent;
    size -= (size_t)sent;
  }
  return 1;
}

static int resource_asset(HINSTANCE instance, int id, const char *type,
                          Asset *asset) {
  HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
  if (!resource)
    return 0;
  HGLOBAL loaded = LoadResource(instance, resource);
  if (!loaded)
    return 0;
  asset->data = LockResource(loaded);
  asset->size = SizeofResource(instance, resource);
  asset->type = type;
  return asset->data && asset->size;
}

static int write_resource(HINSTANCE instance, int id, const wchar_t *path) {
  Asset asset = {0};
  if (!resource_asset(instance, id, "", &asset))
    return 0;
  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, 0);
  if (file == INVALID_HANDLE_VALUE)
    return 0;
  const uint8_t *data = (const uint8_t *)asset.data;
  size_t remaining = asset.size;
  int ok = 1;
  while (remaining) {
    DWORD chunk = remaining > UINT32_MAX ? UINT32_MAX : (DWORD)remaining;
    DWORD written = 0;
    if (!WriteFile(file, data, chunk, &written, 0) || !written) {
      ok = 0;
      break;
    }
    data += written;
    remaining -= written;
  }
  if (!CloseHandle(file))
    ok = 0;
  if (!ok)
    DeleteFileW(path);
  return ok;
}

static int codec_path(const wchar_t *directory, const wchar_t *name,
                      wchar_t *path, size_t capacity) {
  return swprintf(path, capacity, L"%ls\\%ls", directory, name) >= 0;
}

static void cleanup_codecs(CodecFiles *files) {
  qlic_input_set_runtime_directory(NULL);
  if (files->jxl[0])
    DeleteFileW(files->jxl);
  if (files->brotli_dec[0])
    DeleteFileW(files->brotli_dec);
  if (files->brotli_common[0])
    DeleteFileW(files->brotli_common);
  if (files->directory[0])
    RemoveDirectoryW(files->directory);
  memset(files, 0, sizeof(*files));
}

static int prepare_codecs(HINSTANCE instance, CodecFiles *files) {
  memset(files, 0, sizeof(*files));
  wchar_t temporary[32768];
  DWORD length =
      GetTempPathW((DWORD)(sizeof(temporary) / sizeof(temporary[0])),
                   temporary);
  if (!length || length >= sizeof(temporary) / sizeof(temporary[0]))
    return 0;
  int created = 0;
  for (unsigned attempt = 0; attempt < 64; ++attempt) {
    if (swprintf(files->directory,
                 sizeof(files->directory) / sizeof(files->directory[0]),
                 L"%lsQLIC Demo %lu %08lx %u", temporary,
                 (unsigned long)GetCurrentProcessId(),
                 (unsigned long)GetTickCount(), attempt) < 0)
      return 0;
    if (CreateDirectoryW(files->directory, 0)) {
      created = 1;
      break;
    }
  }
  if (!created ||
      !codec_path(files->directory, L"jxl_dec.dll", files->jxl,
                  sizeof(files->jxl) / sizeof(files->jxl[0])) ||
      !codec_path(files->directory, L"brotlicommon.dll",
                  files->brotli_common,
                  sizeof(files->brotli_common) /
                      sizeof(files->brotli_common[0])) ||
      !codec_path(files->directory, L"brotlidec.dll", files->brotli_dec,
                  sizeof(files->brotli_dec) /
                      sizeof(files->brotli_dec[0])) ||
      !write_resource(instance, DEMO_BROTLI_COMMON, files->brotli_common) ||
      !write_resource(instance, DEMO_BROTLI_DEC, files->brotli_dec) ||
      !write_resource(instance, DEMO_JXL, files->jxl) ||
      !qlic_input_set_runtime_directory(files->directory)) {
    cleanup_codecs(files);
    return 0;
  }
  return 1;
}

static int route_asset(HINSTANCE instance, const char *path, Asset *asset) {
  if (!strcmp(path, "/") || !strcmp(path, "/demo.html"))
    return resource_asset(instance, DEMO_HTML, "text/html; charset=utf-8",
                          asset);
  if (!strcmp(path, "/qlic-web.js"))
    return resource_asset(instance, DEMO_MODULE,
                          "text/javascript; charset=utf-8", asset);
  if (!strcmp(path, "/qlic-worker.js"))
    return resource_asset(instance, DEMO_WORKER,
                          "text/javascript; charset=utf-8", asset);
  if (!strcmp(path, "/qlic-web.wasm"))
    return resource_asset(instance, DEMO_WASM, "application/wasm", asset);
  return 0;
}

static void send_status(SOCKET client, int status, const char *reason) {
  char header[256];
  int length = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Length: 0\r\n"
                        "Cache-Control: no-store\r\n"
                        "Connection: close\r\n\r\n",
                        status, reason);
  if (length > 0 && (size_t)length < sizeof(header))
    send_all(client, header, (size_t)length);
}

static void send_text(SOCKET client, int status, const char *reason,
                      const char *message) {
  size_t size = strlen(message);
  char header[384];
  int length = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: text/plain; charset=utf-8\r\n"
                        "Content-Length: %zu\r\n"
                        "Cache-Control: no-store\r\n"
                        "X-Content-Type-Options: nosniff\r\n"
                        "Connection: close\r\n\r\n",
                        status, reason, size);
  if (length <= 0 || (size_t)length >= sizeof(header) ||
      !send_all(client, header, (size_t)length))
    return;
  send_all(client, message, size);
}

static int content_length(const char *request, const char *header_end,
                          size_t *result) {
  const char *line = strstr(request, "\r\n");
  if (!line)
    return 0;
  line += 2;
  while (line < header_end - 2) {
    const char *end = strstr(line, "\r\n");
    if (!end || end > header_end)
      return 0;
    static const char name[] = "Content-Length:";
    size_t name_size = sizeof(name) - 1u;
    if ((size_t)(end - line) >= name_size &&
        !_strnicmp(line, name, name_size)) {
      const char *value = line + name_size;
      while (value < end && (*value == ' ' || *value == '\t'))
        ++value;
      char number[32];
      size_t digits = (size_t)(end - value);
      if (!digits || digits >= sizeof(number))
        return 0;
      memcpy(number, value, digits);
      number[digits] = 0;
      char *tail = NULL;
      unsigned __int64 parsed = _strtoui64(number, &tail, 10);
      if (!tail || *tail || parsed > SIZE_MAX)
        return 0;
      *result = (size_t)parsed;
      return 1;
    }
    line = end + 2;
  }
  return 0;
}

static uint8_t *read_body(SOCKET client, const char *request,
                          size_t request_size, const char *header_end,
                          size_t *body_size) {
  size_t size = 0;
  if (!content_length(request, header_end, &size) || !size ||
      (uint64_t)size > MAX_FILE_BYTES)
    return NULL;
  uint8_t *body = (uint8_t *)malloc(size);
  if (!body)
    return NULL;
  size_t header_size = (size_t)(header_end - request);
  size_t available = request_size - header_size;
  if (available > size) {
    free(body);
    return NULL;
  }
  memcpy(body, request + header_size, available);
  size_t used = available;
  while (used < size) {
    size_t remaining = size - used;
    int chunk = remaining > INT_MAX ? INT_MAX : (int)remaining;
    int got = recv(client, (char *)body + used, chunk, 0);
    if (got <= 0) {
      free(body);
      return NULL;
    }
    used += (size_t)got;
  }
  *body_size = size;
  return body;
}

static void serve_decode(SOCKET client, const uint8_t *data, size_t size) {
  char error[256] = {0};
  QlicInput input = {0};
  QlicInputImage image = {0};
  if (!qlic_input_open_memory(data, size, MAX_FILE_BYTES, MAX_PIXELS, &input,
                              error, sizeof(error))) {
    send_text(client, 422, "Unprocessable Content",
              error[0] ? error : "The source image was not accepted.");
    return;
  }
  int ok = qlic_input_decode(&input, MAX_PIXELS, &image, error, sizeof(error));
  qlic_input_close(&input);
  if (!ok) {
    send_text(client, 415, "Unsupported Media Type",
              error[0] ? error : "The source image could not be decoded.");
    return;
  }
  uint64_t expected = (uint64_t)image.width * image.height * 4u;
  if (!image.width || !image.height || expected > SIZE_MAX) {
    free(image.rgba);
    send_text(client, 422, "Unprocessable Content",
              "The decoded image dimensions are invalid.");
    return;
  }
  char header[512];
  int length = snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/octet-stream\r\n"
                        "Content-Length: %zu\r\n"
                        "X-QLIC-Width: %lu\r\n"
                        "X-QLIC-Height: %lu\r\n"
                        "Cache-Control: no-store\r\n"
                        "X-Content-Type-Options: nosniff\r\n"
                        "Connection: close\r\n\r\n",
                        (size_t)expected, (unsigned long)image.width,
                        (unsigned long)image.height);
  if (length > 0 && (size_t)length < sizeof(header) &&
      send_all(client, header, (size_t)length))
    send_all(client, image.rgba, (size_t)expected);
  free(image.rgba);
}

static void serve_client(HINSTANCE instance, SOCKET client) {
  char request[4096];
  size_t used = 0;
  while (used + 1u < sizeof(request)) {
    int got = recv(client, request + used, (int)(sizeof(request) - used - 1u),
                   0);
    if (got <= 0)
      return;
    used += (size_t)got;
    request[used] = 0;
    if (strstr(request, "\r\n\r\n"))
      break;
  }
  char *header_end = strstr(request, "\r\n\r\n");
  if (!header_end) {
    send_status(client, 431, "Request Header Fields Too Large");
    return;
  }
  header_end += 4;

  char method[8] = {0};
  char path[512] = {0};
  if (sscanf(request, "%7s %511s", method, path) != 2) {
    send_status(client, 400, "Bad Request");
    return;
  }
  char *query = strchr(path, '?');
  if (query)
    *query = 0;
  if (!strcmp(path, "/heartbeat")) {
    send_status(client, 204, "No Content");
    return;
  }
  if (!strcmp(path, "/favicon.ico")) {
    send_status(client, 204, "No Content");
    return;
  }
  if (!strcmp(path, "/decode")) {
    if (strcmp(method, "POST")) {
      send_status(client, 405, "Method Not Allowed");
      return;
    }
    size_t body_size = 0;
    uint8_t *body =
        read_body(client, request, used, header_end, &body_size);
    if (!body) {
      send_text(client, 400, "Bad Request",
                "The source image upload was invalid.");
      return;
    }
    serve_decode(client, body, body_size);
    free(body);
    return;
  }
  int head = !strcmp(method, "HEAD");
  if (strcmp(method, "GET") && !head) {
    send_status(client, 405, "Method Not Allowed");
    return;
  }

  Asset asset = {0};
  if (!route_asset(instance, path, &asset)) {
    send_status(client, 404, "Not Found");
    return;
  }
  char header[512];
  int length = snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %lu\r\n"
                        "Cache-Control: no-store\r\n"
                        "X-Content-Type-Options: nosniff\r\n"
                        "Connection: close\r\n\r\n",
                        asset.type, (unsigned long)asset.size);
  if (length <= 0 || (size_t)length >= sizeof(header) ||
      !send_all(client, header, (size_t)length))
    return;
  if (!head)
    send_all(client, asset.data, asset.size);
}

static int server_loop(HINSTANCE instance, SOCKET listener) {
  uint64_t active = GetTickCount64();
  for (;;) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listener, &readable);
    struct timeval timeout = {1, 0};
    int ready = select(0, &readable, 0, 0, &timeout);
    if (ready == SOCKET_ERROR)
      return 0;
    if (!ready) {
      if (GetTickCount64() - active >= IDLE_LIMIT_MS)
        return 1;
      continue;
    }
    SOCKET client = accept(listener, 0, 0);
    if (client == INVALID_SOCKET)
      continue;
    DWORD receive_timeout = 30000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
               (const char *)&receive_timeout, sizeof(receive_timeout));
    active = GetTickCount64();
    serve_client(instance, client);
    shutdown(client, SD_BOTH);
    closesocket(client);
  }
}

static void show_error(const wchar_t *message) {
  MessageBoxW(0, message, L"QLIC Demo", MB_OK | MB_ICONERROR);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command,
                    int show) {
  (void)previous;
  (void)command;
  (void)show;
  WSADATA winsock;
  if (WSAStartup(MAKEWORD(2, 2), &winsock)) {
    show_error(L"QLIC Demo could not start its local browser service.");
    return 1;
  }

  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    WSACleanup();
    show_error(L"QLIC Demo could not create its local browser service.");
    return 1;
  }
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  /* only loopback is exposed, and every launch gets its own port */
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(listener, (const struct sockaddr *)&address, sizeof(address)) ==
          SOCKET_ERROR ||
      listen(listener, SOMAXCONN) == SOCKET_ERROR) {
    closesocket(listener);
    WSACleanup();
    show_error(L"QLIC Demo could not start its local browser service.");
    return 1;
  }

  int address_size = sizeof(address);
  if (getsockname(listener, (struct sockaddr *)&address, &address_size) ==
      SOCKET_ERROR) {
    closesocket(listener);
    WSACleanup();
    show_error(L"QLIC Demo could not read its local address.");
    return 1;
  }
  wchar_t url[96];
  if (swprintf(url, sizeof(url) / sizeof(url[0]), L"http://127.0.0.1:%u/",
               (unsigned)ntohs(address.sin_port)) < 0) {
    closesocket(listener);
    WSACleanup();
    show_error(L"QLIC Demo could not create its browser address.");
    return 1;
  }
  CodecFiles codecs;
  if (!prepare_codecs(instance, &codecs)) {
    closesocket(listener);
    WSACleanup();
    show_error(L"QLIC Demo could not prepare its image decoders.");
    return 1;
  }
  HINSTANCE opened = ShellExecuteW(0, L"open", url, 0, 0, SW_SHOWNORMAL);
  if ((INT_PTR)opened <= 32) {
    cleanup_codecs(&codecs);
    closesocket(listener);
    WSACleanup();
    show_error(L"QLIC Demo could not open the default browser.");
    return 1;
  }

  int ok = server_loop(instance, listener);
  cleanup_codecs(&codecs);
  closesocket(listener);
  WSACleanup();
  if (!ok)
    show_error(L"QLIC Demo stopped because its local browser service failed.");
  return ok ? 0 : 1;
}
