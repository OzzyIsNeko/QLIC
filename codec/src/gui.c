#include "win_util.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <strsafe.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define PATH_CAP 32768u
#define WM_QLIC_DONE (WM_APP + 1u)
#define QLIC_COLOR_BACKGROUND RGB(232, 243, 235)
#define QLIC_COLOR_CARD RGB(250, 252, 250)
#define QLIC_COLOR_FIELD RGB(241, 247, 243)
#define QLIC_COLOR_BORDER RGB(207, 224, 212)
#define QLIC_COLOR_TEXT RGB(34, 54, 42)
#define QLIC_COLOR_MUTED RGB(91, 110, 98)
#define QLIC_COLOR_ACCENT RGB(87, 132, 102)
#define QLIC_COLOR_ACCENT_DARK RGB(65, 105, 78)
#define QLIC_COLOR_ACCENT_PALE RGB(224, 237, 228)

enum {
  ID_INPUT = 100,
  ID_BROWSE,
  ID_COMPRESS,
  ID_SAVE,
  ID_PROGRESS,
  ID_TITLE = 200,
  ID_SUBTITLE,
  ID_INPUT_LABEL,
  ID_RESULT_LABEL,
  ID_INPUT_DETAILS,
  ID_STATUS,
  ID_RESULT,
  ID_RESULT_DETAILS
};

typedef struct {
  HWND window;
  wchar_t command[PATH_CAP];
  wchar_t executable[PATH_CAP];
  wchar_t output[PATH_CAP];
  uint64_t source_size;
} Job;

typedef struct {
  wchar_t output[PATH_CAP];
  uint64_t source_size;
  uint64_t output_size;
  uint64_t milliseconds;
  int ok;
} Result;

static HWND g_input;
static HWND g_input_details;
static HWND g_browse;
static HWND g_compress;
static HWND g_save;
static HWND g_progress;
static HWND g_status;
static HWND g_result;
static HWND g_result_details;
static HWND g_title;
static HWND g_subtitle;
static HWND g_input_label;
static HWND g_result_label;
static HFONT g_font;
static HFONT g_title_font;
static HFONT g_result_font;
static HFONT g_label_font;
static HBRUSH g_background;
static HBRUSH g_card;
static HBRUSH g_field;
static wchar_t g_temp_output[PATH_CAP];
static int g_busy;
static UINT g_dpi = 96u;

static int scaled(int value) {
  return MulDiv(value, (int)g_dpi, 96);
}

static HFONT make_font(int points, int weight, const wchar_t *face) {
  return CreateFontW(-MulDiv(points, (int)g_dpi, 72), 0, 0, 0, weight, 0, 0,
                     0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                     face);
}

static void replace_fonts(void) {
  if (g_font)
    DeleteObject(g_font);
  if (g_title_font)
    DeleteObject(g_title_font);
  if (g_result_font)
    DeleteObject(g_result_font);
  if (g_label_font)
    DeleteObject(g_label_font);
  g_font = make_font(10, FW_NORMAL, L"Segoe UI Variable Text");
  g_title_font = make_font(22, FW_SEMIBOLD, L"Segoe UI Variable Display");
  g_result_font = make_font(25, FW_SEMIBOLD, L"Segoe UI Variable Display");
  g_label_font = make_font(9, FW_SEMIBOLD, L"Segoe UI Variable Text");
}

static void apply_fonts(void) {
  HWND controls[] = {g_input,         g_input_details, g_browse,
                     g_compress,      g_save,          g_status,
                     g_result_details, g_subtitle};
  for (size_t index = 0; index < sizeof(controls) / sizeof(controls[0]);
       ++index)
    if (controls[index])
      SendMessageW(controls[index], WM_SETFONT, (WPARAM)g_font, TRUE);
  if (g_title)
    SendMessageW(g_title, WM_SETFONT, (WPARAM)g_title_font, TRUE);
  if (g_result)
    SendMessageW(g_result, WM_SETFONT, (WPARAM)g_result_font, TRUE);
  if (g_input_label)
    SendMessageW(g_input_label, WM_SETFONT, (WPARAM)g_label_font, TRUE);
  if (g_result_label)
    SendMessageW(g_result_label, WM_SETFONT, (WPARAM)g_label_font, TRUE);
}

static wchar_t *path_buffer(void) {
  return (wchar_t *)malloc((size_t)PATH_CAP * sizeof(wchar_t));
}

static int file_size(const wchar_t *path, uint64_t *size) {
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data) ||
      (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    return 0;
  *size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
  return 1;
}

static int has_extension(const wchar_t *path, const wchar_t *extension) {
  const wchar_t *slash = wcsrchr(path, L'\\');
  const wchar_t *dot = wcsrchr(path, L'.');
  return dot && (!slash || dot > slash) && _wcsicmp(dot, extension) == 0;
}

static void replace_extension(const wchar_t *path, const wchar_t *extension,
                              wchar_t *output, size_t capacity) {
  if (wcscpy_s(output, capacity, path))
    return;
  wchar_t *slash = wcsrchr(output, L'\\');
  wchar_t *dot = wcsrchr(output, L'.');
  if (!dot || (slash && dot < slash))
    dot = output + wcslen(output);
  size_t remaining = capacity - (size_t)(dot - output);
  if (wcscpy_s(dot, remaining, extension))
    output[0] = 0;
}

static void format_size(uint64_t bytes, wchar_t *text, size_t capacity) {
  double value = (double)bytes;
  const wchar_t *unit = L"bytes";
  if (bytes >= UINT64_C(1073741824)) {
    value /= 1073741824.0;
    unit = L"GB";
  } else if (bytes >= UINT64_C(1048576)) {
    value /= 1048576.0;
    unit = L"MB";
  } else if (bytes >= UINT64_C(1024)) {
    value /= 1024.0;
    unit = L"KB";
  }
  if (unit[0] == L'b')
    StringCchPrintfW(text, capacity, L"%llu bytes",
                     (unsigned long long)bytes);
  else
    StringCchPrintfW(text, capacity, L"%.2f %ls", value, unit);
}

static void remove_temporary_output(void) {
  if (g_temp_output[0])
    DeleteFileW(g_temp_output);
  g_temp_output[0] = 0;
}

static void clear_result(void) {
  remove_temporary_output();
  SetWindowTextW(g_result, L"No result yet");
  SetWindowTextW(g_result_details, L"");
  EnableWindow(g_save, FALSE);
}

static void update_controls(void) {
  wchar_t input[PATH_CAP];
  GetWindowTextW(g_input, input, PATH_CAP);
  EnableWindow(g_input, !g_busy);
  EnableWindow(g_browse, !g_busy);
  EnableWindow(g_compress, !g_busy && input[0]);
  SetWindowTextW(g_compress,
                 g_busy ? L"Compressing..." : L"Compress with QLIC");
}

static void set_input(const wchar_t *path) {
  if (!path || !path[0])
    return;
  if (has_extension(path, L".qlic")) {
    SetWindowTextW(g_status, L"Choose an image rather than a QLIC file.");
    return;
  }
  uint64_t bytes = 0;
  if (!file_size(path, &bytes)) {
    SetWindowTextW(g_status, L"The selected image is not available.");
    return;
  }
  SetWindowTextW(g_input, path);
  wchar_t size_text[64];
  wchar_t details[160];
  format_size(bytes, size_text, 64);
  StringCchPrintfW(details, 160, L"Original file size: %ls", size_text);
  SetWindowTextW(g_input_details, details);
  SetWindowTextW(g_status, L"Ready");
  clear_result();
  update_controls();
}

static void choose_input(HWND window) {
  wchar_t *path = path_buffer();
  if (!path)
    return;
  GetWindowTextW(g_input, path, PATH_CAP);
  OPENFILENAMEW dialog = {0};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window;
  dialog.lpstrFile = path;
  dialog.nMaxFile = PATH_CAP;
  dialog.lpstrFilter =
      L"Supported images\0*.png;*.webp;*.jxl;*.avif;*.bmp;*.tif;*.tiff;*.gif\0"
      L"All files\0*.*\0";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
  if (GetOpenFileNameW(&dialog))
    set_input(path);
  free(path);
}

static int executable_path(wchar_t *output, size_t capacity) {
  DWORD length = GetModuleFileNameW(NULL, output, (DWORD)capacity);
  if (!length || length >= capacity)
    return 0;
  wchar_t *slash = wcsrchr(output, L'\\');
  if (!slash)
    return wcscpy_s(output, capacity, L"qlic.exe") == 0;
  slash[1] = 0;
  return wcscat_s(output, capacity, L"qlic.exe") == 0;
}

static int temporary_qlic(wchar_t *output, size_t capacity) {
  wchar_t directory[MAX_PATH];
  wchar_t temporary[MAX_PATH];
  DWORD length = GetTempPathW(MAX_PATH, directory);
  if (!length || length >= MAX_PATH ||
      !GetTempFileNameW(directory, L"qli", 0, temporary))
    return 0;
  DeleteFileW(temporary);
  if (wcslen(temporary) + 6u > capacity)
    return 0;
  return wcscpy_s(output, capacity, temporary) == 0 &&
         wcscat_s(output, capacity, L".qlic") == 0;
}

static void post_result(Job *job, Result *result) {
  if (!PostMessageW(job->window, WM_QLIC_DONE, 0, (LPARAM)result)) {
    DeleteFileW(job->output);
    free(result);
  }
  free(job);
}

static DWORD WINAPI run_process(LPVOID parameter) {
  Job *job = (Job *)parameter;
  Result *result = (Result *)calloc(1, sizeof(*result));
  if (!result) {
    DeleteFileW(job->output);
    post_result(job, NULL);
    return 0;
  }
  result->source_size = job->source_size;
  wcscpy_s(result->output, PATH_CAP, job->output);

  SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
  HANDLE read_pipe = NULL;
  HANDLE write_pipe = NULL;
  if (!CreatePipe(&read_pipe, &write_pipe, &security, 0) ||
      !SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
    if (read_pipe)
      CloseHandle(read_pipe);
    if (write_pipe)
      CloseHandle(write_pipe);
    DeleteFileW(job->output);
    post_result(job, result);
    return 0;
  }

  wchar_t *command = path_buffer();
  if (!command) {
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    DeleteFileW(job->output);
    post_result(job, result);
    return 0;
  }
  wcscpy_s(command, PATH_CAP, job->command);

  STARTUPINFOW startup = {0};
  PROCESS_INFORMATION process = {0};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
  startup.wShowWindow = SW_HIDE;
  startup.hStdOutput = write_pipe;
  startup.hStdError = write_pipe;
  uint64_t started = GetTickCount64();
  BOOL created = CreateProcessW(job->executable, command, NULL, NULL, TRUE,
                                CREATE_NO_WINDOW, NULL, NULL, &startup,
                                &process);
  free(command);
  CloseHandle(write_pipe);
  if (!created) {
    CloseHandle(read_pipe);
    DeleteFileW(job->output);
    post_result(job, result);
    return 0;
  }

  char buffer[4096];
  DWORD count = 0;
  while (ReadFile(read_pipe, buffer, sizeof(buffer), &count, NULL) && count) {
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  result->milliseconds = GetTickCount64() - started;
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  CloseHandle(read_pipe);

  result->ok = exit_code == 0 &&
               file_size(job->output, &result->output_size) &&
               result->output_size > 0u;
  if (!result->ok)
    DeleteFileW(job->output);
  post_result(job, result);
  return 0;
}

static void start_compression(HWND window) {
  if (g_busy)
    return;
  wchar_t input[PATH_CAP];
  GetWindowTextW(g_input, input, PATH_CAP);
  uint64_t source_size = 0;
  if (!input[0] || !file_size(input, &source_size) || !source_size) {
    SetWindowTextW(g_status, L"Choose a readable image first.");
    return;
  }

  clear_result();
  Job *job = (Job *)calloc(1, sizeof(*job));
  wchar_t *quoted_executable = path_buffer();
  wchar_t *quoted_input = path_buffer();
  wchar_t *quoted_output = path_buffer();
  if (!job || !quoted_executable || !quoted_input || !quoted_output) {
    free(job);
    free(quoted_executable);
    free(quoted_input);
    free(quoted_output);
    SetWindowTextW(g_status, L"Could not prepare the compression test.");
    return;
  }
  if (!temporary_qlic(job->output, PATH_CAP)) {
    free(job);
    free(quoted_executable);
    free(quoted_input);
    free(quoted_output);
    SetWindowTextW(g_status, L"Could not prepare the compression test.");
    return;
  }

  int valid = executable_path(job->executable, PATH_CAP) &&
              wquote(job->executable, quoted_executable, PATH_CAP) &&
              wquote(input, quoted_input, PATH_CAP) &&
              wquote(job->output, quoted_output, PATH_CAP);
  HRESULT formatted =
      valid ? StringCchPrintfW(job->command, PATH_CAP,
                               L"%ls pack %ls %ls --threads all",
                               quoted_executable, quoted_input, quoted_output)
            : STRSAFE_E_INSUFFICIENT_BUFFER;
  free(quoted_executable);
  free(quoted_input);
  free(quoted_output);
  if (FAILED(formatted)) {
    DeleteFileW(job->output);
    free(job);
    SetWindowTextW(g_status, L"The image path is too long.");
    return;
  }

  job->window = window;
  job->source_size = source_size;
  g_busy = 1;
  SetWindowTextW(g_status, L"Compressing with all available threads...");
  SendMessageW(g_progress, PBM_SETMARQUEE, TRUE, 35);
  ShowWindow(g_progress, SW_SHOW);
  update_controls();
  HANDLE thread = CreateThread(NULL, 0, run_process, job, 0, NULL);
  if (!thread) {
    g_busy = 0;
    DeleteFileW(job->output);
    free(job);
    SendMessageW(g_progress, PBM_SETMARQUEE, FALSE, 0);
    ShowWindow(g_progress, SW_HIDE);
    SetWindowTextW(g_status, L"Could not start the compression test.");
    update_controls();
    return;
  }
  CloseHandle(thread);
}

static void show_result(const Result *result) {
  wchar_t headline[96];
  wchar_t source_text[64];
  wchar_t output_text[64];
  wchar_t details[320];
  format_size(result->source_size, source_text, 64);
  format_size(result->output_size, output_text, 64);
  double change =
      ((double)result->output_size - (double)result->source_size) * 100.0 /
      (double)result->source_size;
  /* container sizes show the saving someone will actually get */
  if (change < -0.005)
    StringCchPrintfW(headline, 96, L"%.1f%% smaller", -change);
  else if (change > 0.005)
    StringCchPrintfW(headline, 96, L"%.1f%% larger", change);
  else
    wcscpy_s(headline, 96, L"Same file size");
  StringCchPrintfW(
      details, 320,
      L"Original file  %ls\r\nQLIC file       %ls\r\nEncode time    %.2f "
      L"seconds",
      source_text, output_text, (double)result->milliseconds / 1000.0);
  SetWindowTextW(g_result, headline);
  SetWindowTextW(g_result_details, details);
}

static void save_result(HWND window) {
  if (!g_temp_output[0])
    return;
  wchar_t input[PATH_CAP];
  wchar_t *path = path_buffer();
  if (!path)
    return;
  GetWindowTextW(g_input, input, PATH_CAP);
  replace_extension(input, L".qlic", path, PATH_CAP);
  OPENFILENAMEW dialog = {0};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window;
  dialog.lpstrFile = path;
  dialog.nMaxFile = PATH_CAP;
  dialog.lpstrFilter = L"QLIC file\0*.qlic\0All files\0*.*\0";
  dialog.lpstrDefExt = L"qlic";
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
  if (GetSaveFileNameW(&dialog)) {
    if (CopyFileW(g_temp_output, path, FALSE))
      SetWindowTextW(g_status, L"QLIC file saved.");
    else
      SetWindowTextW(g_status, L"Could not save the QLIC file.");
  }
  free(path);
}

static HWND make_control(DWORD extended, const wchar_t *class_name,
                         const wchar_t *text, DWORD style, int id,
                         HWND parent) {
  HWND control =
      CreateWindowExW(extended, class_name, text, WS_CHILD | WS_VISIBLE | style,
                      0, 0, 0, 0, parent, (HMENU)(INT_PTR)id,
                      (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), NULL);
  if (control && g_font)
    SendMessageW(control, WM_SETFONT, (WPARAM)g_font, TRUE);
  return control;
}

static void layout(HWND window) {
  RECT client;
  GetClientRect(window, &client);
  int width = client.right;
  int margin = scaled(28);
  int card_left = scaled(44);
  int browse_width = scaled(116);
  int card_width = width - card_left * 2;
  MoveWindow(g_title, margin, scaled(22), width - margin * 2, scaled(40), TRUE);
  MoveWindow(g_subtitle, margin, scaled(66), width - margin * 2, scaled(34),
             TRUE);
  MoveWindow(g_input_label, card_left, scaled(132), card_width, scaled(20),
             TRUE);
  MoveWindow(g_input, card_left + scaled(10), scaled(162),
             card_width - browse_width - scaled(36), scaled(32), TRUE);
  MoveWindow(g_browse, width - card_left - browse_width, scaled(158),
             browse_width, scaled(40), TRUE);
  MoveWindow(g_input_details, card_left, scaled(204), card_width, scaled(22),
             TRUE);
  MoveWindow(g_status, margin, scaled(265), width - margin * 2 - scaled(180),
             scaled(24), TRUE);
  MoveWindow(g_compress, width - margin - scaled(164), scaled(252),
             scaled(164), scaled(42), TRUE);
  MoveWindow(g_progress, margin, scaled(306), width - margin * 2, scaled(5),
             TRUE);
  MoveWindow(g_result_label, card_left, scaled(350), card_width, scaled(20),
             TRUE);
  MoveWindow(g_result, card_left, scaled(380), card_width, scaled(52), TRUE);
  MoveWindow(g_result_details, card_left, scaled(440),
             card_width - scaled(148), scaled(62), TRUE);
  MoveWindow(g_save, width - card_left - scaled(128),
             client.bottom - scaled(80), scaled(128), scaled(40), TRUE);
}

static void round_rect(HDC dc, const RECT *rectangle, COLORREF fill,
                       COLORREF border, int radius) {
  HBRUSH brush = CreateSolidBrush(fill);
  HPEN pen = CreatePen(PS_SOLID, 1, border);
  HGDIOBJ old_brush = SelectObject(dc, brush);
  HGDIOBJ old_pen = SelectObject(dc, pen);
  RoundRect(dc, rectangle->left, rectangle->top, rectangle->right,
            rectangle->bottom, radius, radius);
  SelectObject(dc, old_pen);
  SelectObject(dc, old_brush);
  DeleteObject(pen);
  DeleteObject(brush);
}

static void paint_window(HWND window) {
  PAINTSTRUCT paint;
  HDC dc = BeginPaint(window, &paint);
  RECT client;
  GetClientRect(window, &client);
  FillRect(dc, &client, g_background);

  RECT input_card = {scaled(28), scaled(116), client.right - scaled(28),
                     scaled(238)};
  RECT result_card = {scaled(28), scaled(332), client.right - scaled(28),
                      client.bottom - scaled(24)};
  RECT input_shadow = input_card;
  RECT result_shadow = result_card;
  OffsetRect(&input_shadow, 0, scaled(2));
  OffsetRect(&result_shadow, 0, scaled(2));
  round_rect(dc, &input_shadow, RGB(218, 232, 222), RGB(218, 232, 222),
             scaled(18));
  round_rect(dc, &result_shadow, RGB(218, 232, 222), RGB(218, 232, 222),
             scaled(18));
  round_rect(dc, &input_card, QLIC_COLOR_CARD, QLIC_COLOR_BORDER, scaled(18));
  round_rect(dc, &result_card, QLIC_COLOR_CARD, QLIC_COLOR_BORDER, scaled(18));

  RECT input_field = {scaled(44), scaled(158),
                      client.right - scaled(176), scaled(198)};
  round_rect(dc, &input_field, QLIC_COLOR_FIELD, QLIC_COLOR_FIELD, scaled(10));
  EndPaint(window, &paint);
}

static void draw_button(const DRAWITEMSTRUCT *item) {
  RECT outer = item->rcItem;
  HBRUSH parent = item->CtlID == ID_COMPRESS ? g_background : g_card;
  FillRect(item->hDC, &outer, parent);
  RECT button = outer;
  InflateRect(&button, -1, -1);

  int primary = item->CtlID == ID_COMPRESS;
  int disabled = (item->itemState & ODS_DISABLED) != 0;
  int pressed = (item->itemState & ODS_SELECTED) != 0;
  COLORREF fill = primary ? QLIC_COLOR_ACCENT : QLIC_COLOR_FIELD;
  COLORREF border = primary ? QLIC_COLOR_ACCENT : QLIC_COLOR_BORDER;
  COLORREF text =
      primary ? RGB(255, 255, 255) : QLIC_COLOR_ACCENT_DARK;
  if (pressed)
    fill = primary ? QLIC_COLOR_ACCENT_DARK : QLIC_COLOR_ACCENT_PALE;
  if (disabled) {
    fill = primary ? RGB(181, 205, 188) : RGB(238, 243, 239);
    border = RGB(214, 225, 217);
    text = RGB(139, 156, 145);
  }
  round_rect(item->hDC, &button, fill, border, scaled(11));

  wchar_t text_buffer[64];
  GetWindowTextW(item->hwndItem, text_buffer, 64);
  HGDIOBJ old_font = SelectObject(item->hDC, g_font);
  SetBkMode(item->hDC, TRANSPARENT);
  SetTextColor(item->hDC, text);
  DrawTextW(item->hDC, text_buffer, -1, &button,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  SelectObject(item->hDC, old_font);

  if (item->itemState & ODS_FOCUS) {
    RECT focus = button;
    InflateRect(&focus, -scaled(4), -scaled(4));
    DrawFocusRect(item->hDC, &focus);
  }
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
  switch (message) {
  case WM_CREATE: {
    g_dpi = GetDpiForWindow(window);
    g_background = CreateSolidBrush(QLIC_COLOR_BACKGROUND);
    g_card = CreateSolidBrush(QLIC_COLOR_CARD);
    g_field = CreateSolidBrush(QLIC_COLOR_FIELD);
    replace_fonts();
    g_title = make_control(0, L"STATIC", L"QLIC Demo", SS_CENTER, ID_TITLE,
                           window);
    g_subtitle = make_control(
        0, L"STATIC", L"QLIC Image Compression Demo",
        SS_CENTER, ID_SUBTITLE, window);
    g_input_label = make_control(0, L"STATIC", L"IMAGE", SS_LEFT,
                                 ID_INPUT_LABEL, window);
    g_result_label = make_control(0, L"STATIC", L"QLIC RESULT", SS_LEFT,
                                  ID_RESULT_LABEL, window);
    g_input = make_control(0, L"EDIT", L"",
                           ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP, ID_INPUT,
                           window);
    SendMessageW(g_input, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(scaled(4), scaled(4)));
    g_browse = make_control(0, L"BUTTON", L"Choose image", BS_OWNERDRAW,
                            ID_BROWSE, window);
    g_input_details =
        make_control(0, L"STATIC", L"Drop an image here or choose one",
                     SS_LEFT, ID_INPUT_DETAILS, window);
    g_compress = make_control(0, L"BUTTON", L"Compress with QLIC",
                              BS_OWNERDRAW, ID_COMPRESS, window);
    g_status =
        make_control(0, L"STATIC", L"", SS_LEFT, ID_STATUS, window);
    g_progress = make_control(0, PROGRESS_CLASSW, L"",
                              PBS_MARQUEE | PBS_SMOOTH, ID_PROGRESS, window);
    g_result =
        make_control(0, L"STATIC", L"No result yet", SS_LEFT, ID_RESULT,
                     window);
    g_result_details =
        make_control(0, L"STATIC", L"", SS_LEFT, ID_RESULT_DETAILS, window);
    g_save = make_control(0, L"BUTTON", L"Save QLIC", BS_OWNERDRAW,
                          ID_SAVE, window);
    if (!g_title || !g_subtitle || !g_input_label || !g_result_label ||
        !g_input || !g_browse || !g_input_details || !g_compress ||
        !g_status || !g_progress || !g_result || !g_result_details || !g_save)
      return -1;
    apply_fonts();
    SendMessageW(g_progress, PBM_SETBKCOLOR, 0, QLIC_COLOR_ACCENT_PALE);
    SendMessageW(g_progress, PBM_SETBARCOLOR, 0, QLIC_COLOR_ACCENT);
    DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners,
                          sizeof(corners));
    COLORREF caption = QLIC_COLOR_BACKGROUND;
    COLORREF caption_text = QLIC_COLOR_TEXT;
    DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &caption,
                          sizeof(caption));
    DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &caption_text,
                          sizeof(caption_text));
    ShowWindow(g_progress, SW_HIDE);
    EnableWindow(g_save, FALSE);
    DragAcceptFiles(window, TRUE);
    update_controls();
    layout(window);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT:
    paint_window(window);
    return 0;
  case WM_DRAWITEM:
    if (((DRAWITEMSTRUCT *)lparam)->CtlType == ODT_BUTTON) {
      draw_button((DRAWITEMSTRUCT *)lparam);
      return TRUE;
    }
    break;
  case WM_CTLCOLORSTATIC: {
    int id = GetDlgCtrlID((HWND)lparam);
    SetBkMode((HDC)wparam, TRANSPARENT);
    if (id == ID_TITLE || id == ID_RESULT)
      SetTextColor((HDC)wparam, QLIC_COLOR_TEXT);
    else if (id == ID_INPUT_LABEL || id == ID_RESULT_LABEL)
      SetTextColor((HDC)wparam, QLIC_COLOR_ACCENT_DARK);
    else
      SetTextColor((HDC)wparam, QLIC_COLOR_MUTED);
    return (LRESULT)((id == ID_TITLE || id == ID_SUBTITLE || id == ID_STATUS)
                         ? g_background
                         : g_card);
  }
  case WM_CTLCOLOREDIT:
    SetBkColor((HDC)wparam, QLIC_COLOR_FIELD);
    SetTextColor((HDC)wparam, QLIC_COLOR_TEXT);
    return (LRESULT)g_field;
  case WM_SIZE:
    layout(window);
    InvalidateRect(window, NULL, TRUE);
    return 0;
  case WM_DPICHANGED: {
    g_dpi = HIWORD(wparam);
    replace_fonts();
    apply_fonts();
    RECT *suggested = (RECT *)lparam;
    SetWindowPos(window, NULL, suggested->left, suggested->top,
                 suggested->right - suggested->left,
                 suggested->bottom - suggested->top,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    layout(window);
    InvalidateRect(window, NULL, TRUE);
    return 0;
  }
  case WM_GETMINMAXINFO: {
    MINMAXINFO *limits = (MINMAXINFO *)lparam;
    limits->ptMinTrackSize.x = scaled(720);
    limits->ptMinTrackSize.y = scaled(600);
    return 0;
  }
  case WM_COMMAND:
    switch (LOWORD(wparam)) {
    case ID_BROWSE:
      choose_input(window);
      return 0;
    case ID_COMPRESS:
      start_compression(window);
      return 0;
    case ID_SAVE:
      save_result(window);
      return 0;
    default:
      break;
    }
    break;
  case WM_DROPFILES: {
    HDROP drop = (HDROP)wparam;
    wchar_t *path = path_buffer();
    if (path && DragQueryFileW(drop, 0, path, PATH_CAP))
      set_input(path);
    free(path);
    DragFinish(drop);
    return 0;
  }
  case WM_QLIC_DONE: {
    Result *result = (Result *)lparam;
    g_busy = 0;
    SendMessageW(g_progress, PBM_SETMARQUEE, FALSE, 0);
    ShowWindow(g_progress, SW_HIDE);
    if (result && result->ok) {
      wcscpy_s(g_temp_output, PATH_CAP, result->output);
      show_result(result);
      SetWindowTextW(g_status, L"Compression finished.");
      EnableWindow(g_save, TRUE);
    } else {
      SetWindowTextW(g_result, L"Something went wrong");
      SetWindowTextW(g_result_details, L"");
      SetWindowTextW(g_status, L"Compression failed.");
    }
    free(result);
    update_controls();
    return 0;
  }
  case WM_CLOSE:
    if (g_busy) {
      MessageBoxW(window, L"Wait for compression to finish.", L"QLIC",
                  MB_OK | MB_ICONINFORMATION);
      return 0;
    }
    DestroyWindow(window);
    return 0;
  case WM_DESTROY:
    remove_temporary_output();
    if (g_font)
      DeleteObject(g_font);
    if (g_title_font)
      DeleteObject(g_title_font);
    if (g_result_font)
      DeleteObject(g_result_font);
    if (g_label_font)
      DeleteObject(g_label_font);
    if (g_background)
      DeleteObject(g_background);
    if (g_card)
      DeleteObject(g_card);
    if (g_field)
      DeleteObject(g_field);
    PostQuitMessage(0);
    return 0;
  default:
    break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE previous,
                    _In_ PWSTR command_line, _In_ int show) {
  (void)previous;
  (void)command_line;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&controls);
  WNDCLASSW window_class = {0};
  window_class.hInstance = instance;
  window_class.lpszClassName = L"QLIC";
  window_class.lpfnWndProc = window_proc;
  window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
  window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  if (!RegisterClassW(&window_class))
    return 1;

  HWND window =
      CreateWindowExW(0, window_class.lpszClassName, L"QLIC Compression Demo",
                      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                      MulDiv(780, (int)GetDpiForSystem(), 96),
                      MulDiv(620, (int)GetDpiForSystem(), 96), NULL, NULL,
                      instance, NULL);
  if (!window)
    return 1;

  int argc = 0;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv && argc > 1)
    set_input(argv[1]);
  if (argv)
    LocalFree(argv);
  ShowWindow(window, show);
  UpdateWindow(window);

  MSG message;
  int result;
  while ((result = GetMessageW(&message, NULL, 0, 0)) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return result < 0 ? 1 : (int)message.wParam;
}
