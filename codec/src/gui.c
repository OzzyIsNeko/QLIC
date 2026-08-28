#include "win_util.h"

#ifndef QLIC_STATIC
#define QLIC_STATIC
#endif
#include <qlic/qlic.h>

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <icm.h>
#include <shellapi.h>
#include <strsafe.h>
#include <wincodec.h>

#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define PATH_CAP 32768u
#define WM_QLIC_DONE (WM_APP + 1u)
#define WM_QLIC_VIEW_DONE (WM_APP + 2u)
#define ID_VIEW_TIMER 1u
#define ID_OPTIONS_TIMER 2u
#define ID_BUTTON_TIMER 1u
#define OPTIONS_FRAME_COUNT 12u
#define QLIC_COLOR_BACKGROUND RGB(231, 242, 233)
#define QLIC_COLOR_CARD RGB(252, 253, 252)
#define QLIC_COLOR_FIELD RGB(243, 248, 244)
#define QLIC_COLOR_FIELD_BORDER RGB(216, 230, 220)
#define QLIC_COLOR_BORDER RGB(199, 219, 206)
#define QLIC_COLOR_TEXT RGB(23, 49, 38)
#define QLIC_COLOR_MUTED RGB(82, 112, 100)
#define QLIC_COLOR_DETAILS RGB(73, 102, 90)
#define QLIC_COLOR_NOTE RGB(96, 122, 110)
#define QLIC_COLOR_PLACEHOLDER RGB(138, 160, 149)
#define QLIC_COLOR_DISABLED RGB(158, 179, 168)
#define QLIC_COLOR_LABEL RGB(55, 98, 77)
#define QLIC_COLOR_SECONDARY RGB(40, 84, 62)
#define QLIC_COLOR_ACCENT RGB(79, 126, 101)
#define QLIC_COLOR_ACCENT_DARK RGB(66, 110, 88)
#define QLIC_COLOR_ACCENT_PALE RGB(224, 237, 228)
#define QLIC_COLOR_BUTTON RGB(248, 252, 249)
#define QLIC_COLOR_BUTTON_BORDER RGB(189, 211, 197)
#define QLIC_COLOR_BUTTON_HOVER RGB(127, 166, 142)
#define QLIC_COLOR_PROGRESS RGB(93, 141, 114)
#define QLIC_COLOR_PROGRESS_BACKGROUND RGB(213, 230, 218)
#define QLIC_COLOR_VIEWER RGB(223, 232, 226)
#define QLIC_COLOR_VIEWER_BORDER RGB(202, 219, 208)
#define QLIC_COLOR_WARNING RGB(145, 101, 12)
#define QLIC_LOSSY_STATUS                                                     \
  L"Lossy source \u00b7 QLIC is lossless, so the output will likely be larger."

enum {
  ID_INPUT = 100,
  ID_BROWSE,
  ID_COMPRESS,
  ID_SAVE,
  ID_PROGRESS,
  ID_OPTIONS,
  ID_THREADS,
  ID_COLOR_PROFILE,
  ID_ICC_PATH,
  ID_ICC_BROWSE,
  ID_ALPHA,
  ID_THREADS_LIST,
  ID_COLOR_PROFILE_LIST,
  ID_ALPHA_LIST,
  ID_VIEW_OPEN,
  ID_VIEW_ZOOM_OUT,
  ID_VIEW_FIT,
  ID_VIEW_ZOOM_IN,
  ID_VIEW_ENCODER,
  ID_VIEW_DETAILS,
  ID_VIEW_PREVIOUS,
  ID_VIEW_PLAY,
  ID_VIEW_NEXT,
  ID_VIEW_FRAME,
  ID_VIEW_SAVE_PNG,
  ID_VIEW_PIXEL,
  ID_VIEW_NOTE,
  ID_VIEW_LABEL,
  ID_INPUT_LABEL = 202,
  ID_RESULT_LABEL,
  ID_INPUT_DETAILS,
  ID_STATUS,
  ID_RESULT,
  ID_RESULT_DETAILS,
  ID_THREADS_LABEL,
  ID_COLOR_PROFILE_LABEL,
  ID_ICC_LABEL,
  ID_ALPHA_LABEL,
  ID_OPTIONS_NOTE,
  ID_OUTPUT
};

typedef struct {
  HWND window;
  wchar_t command[PATH_CAP];
  wchar_t verify_command[PATH_CAP];
  wchar_t executable[PATH_CAP];
  wchar_t output[PATH_CAP];
  uint64_t source_size;
} Job;

typedef struct {
  wchar_t output[PATH_CAP];
  uint64_t source_size;
  uint64_t output_size;
  double milliseconds;
  int ok;
  int verified;
  int cancelled;
  int lossy_source;
  wchar_t message[1024];
} Result;

typedef struct {
  uint8_t *rgba;
  uint8_t *display_rgba;
  uint32_t delay_ms;
} ViewFrame;

typedef struct {
  wchar_t path[PATH_CAP];
  wchar_t message[1024];
  uint64_t source_size;
  double milliseconds;
  uint32_t width;
  uint32_t height;
  uint32_t channels;
  uint32_t bits_per_sample;
  uint32_t alpha_mode;
  uint32_t color_authority;
  uint32_t transfer_characteristics;
  uint32_t frame_count;
  int has_alpha;
  int has_icc;
  int has_cicp;
  int hdr_preview;
  int color_managed;
  int ok;
  size_t stride;
  uint8_t *icc;
  size_t icc_size;
  ViewFrame *frames;
} ViewResult;

typedef struct {
  HWND window;
  wchar_t path[PATH_CAP];
} ViewJob;

typedef struct {
  HWND window;
  uint8_t hover;
  uint8_t target;
  int tracking;
} ButtonAnimation;

static HWND g_input;
static HWND g_input_details;
static HWND g_browse;
static HWND g_compress;
static HWND g_save;
static HWND g_progress;
static HWND g_status;
static HWND g_result;
static HWND g_result_details;
static HWND g_input_label;
static HWND g_result_label;
static HWND g_options;
static HWND g_output;
static HWND g_threads;
static HWND g_threads_list;
static HWND g_threads_label;
static HWND g_color_profile;
static HWND g_color_profile_list;
static HWND g_color_profile_label;
static HWND g_icc_path;
static HWND g_icc_label;
static HWND g_icc_browse;
static HWND g_alpha;
static HWND g_alpha_list;
static HWND g_alpha_label;
static HWND g_options_note;
static HWND g_view_open;
static HWND g_view_zoom_out;
static HWND g_view_fit;
static HWND g_view_zoom_in;
static HWND g_view_encoder;
static HWND g_view_details;
static HWND g_view_previous;
static HWND g_view_play;
static HWND g_view_next;
static HWND g_view_frame_label;
static HWND g_view_save_png;
static HWND g_view_pixel;
static HWND g_view_note;
static HWND g_view_label;
static HFONT g_font;
static HFONT g_title_font;
static HFONT g_result_font;
static HFONT g_placeholder_font;
static HFONT g_label_font;
static HFONT g_mono_font;
static HBRUSH g_background;
static HBRUSH g_card;
static HBRUSH g_field;
static wchar_t g_temp_output[PATH_CAP];
static HANDLE volatile g_process;
static int g_busy;
static int g_options_open;
static int g_options_animating;
static unsigned g_options_position;
static unsigned g_options_frame;
static int g_keyboard_navigation;
static int g_result_placeholder = 1;
static int g_open_choice;
static int g_thread_selection;
static int g_profile_selection;
static int g_alpha_selection;
static unsigned g_thread_values[33];
static size_t g_thread_value_count;
static UINT g_dpi = 96u;
static HWND g_main_window;
static ViewResult *g_view;
static HBITMAP g_view_bitmap;
static uint32_t g_view_frame;
static int g_view_mode;
static int g_view_loading;
static int g_view_fit_mode = 1;
static int g_view_playing;
static int g_view_dragging;
static int g_view_pan_x;
static int g_view_pan_y;
static int g_view_drag_x;
static int g_view_drag_y;
static int g_view_drag_pan_x;
static int g_view_drag_pan_y;
static double g_view_zoom = 1.0;
static ButtonAnimation g_button_animations[17];
static size_t g_button_animation_count;

static void close_choice_lists(void);
static void update_options(void);
static void update_controls(void);
static void layout(HWND window);
static void update_view_zoom_label(HWND window);
static void reset_pixel_text(void);
static LRESULT CALLBACK button_proc(HWND window, UINT message, WPARAM wparam,
                                    LPARAM lparam, UINT_PTR subclass,
                                    DWORD_PTR data);

static const wchar_t *const g_profile_names[] = {
    L"No color metadata",   L"sRGB",
    L"Display P3",          L"Rec. 2100 PQ (HDR)",
    L"Rec. 2100 HLG (HDR)", L"Linear Rec. 2020",
    L"Custom ICC profile"};

static const wchar_t *const g_alpha_names[] = {L"Straight alpha",
                                               L"Premultiplied alpha"};

static int scaled(int value) { return MulDiv(value, (int)g_dpi, 96); }

static unsigned options_ease(void) {
  uint64_t position = g_options_position;
  return (unsigned)((position * position * (765u - 2u * position) + 32512u) /
                    65025u);
}

static int options_motion(int distance) {
  return MulDiv(scaled(distance), (int)options_ease(), 255);
}

static unsigned options_frame_position(unsigned frame) {
  return (unsigned)MulDiv((int)frame, 255, OPTIONS_FRAME_COUNT - 1u);
}

static COLORREF blend_color(COLORREF start, COLORREF end, unsigned amount) {
  unsigned inverse = 255u - amount;
  return RGB((GetRValue(start) * inverse + GetRValue(end) * amount + 127u) /
                 255u,
             (GetGValue(start) * inverse + GetGValue(end) * amount + 127u) /
                 255u,
             (GetBValue(start) * inverse + GetBValue(end) * amount + 127u) /
                 255u);
}

static ButtonAnimation *button_animation(HWND window) {
  for (size_t index = 0; index < g_button_animation_count; ++index)
    if (g_button_animations[index].window == window)
      return &g_button_animations[index];
  return NULL;
}

static void animate_button(ButtonAnimation *animation, uint8_t target) {
  animation->target = target;
  if (animation->hover != target)
    SetTimer(animation->window, ID_BUTTON_TIMER, 16u, NULL);
}

static LRESULT CALLBACK button_proc(HWND window, UINT message, WPARAM wparam,
                                    LPARAM lparam, UINT_PTR subclass,
                                    DWORD_PTR data) {
  ButtonAnimation *animation = (ButtonAnimation *)data;
  if (window == g_options &&
      (message == WM_COMMAND || message == WM_DRAWITEM ||
       message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT))
    return SendMessageW(GetParent(window), message, wparam, lparam);
  if (window == g_options && message == WM_NCHITTEST && g_options_position) {
    POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    ScreenToClient(window, &point);
    if (point.y >= scaled(44))
      return HTTRANSPARENT;
  }
  switch (message) {
  case WM_MOUSEMOVE:
    if (!animation->tracking) {
      TRACKMOUSEEVENT tracking = {sizeof(tracking), TME_LEAVE, window, 0};
      animation->tracking = TrackMouseEvent(&tracking) != FALSE;
    }
    if (IsWindowEnabled(window)) {
      int header = window != g_options ||
                   (GET_Y_LPARAM(lparam) >= 0 &&
                    GET_Y_LPARAM(lparam) < scaled(44));
      animate_button(animation, header ? 255u : 0u);
    }
    break;
  case WM_MOUSELEAVE:
    animation->tracking = 0;
    animate_button(animation, 0u);
    break;
  case WM_ENABLE:
    if (!wparam)
      animate_button(animation, 0u);
    break;
  case WM_TIMER:
    if (wparam == ID_BUTTON_TIMER) {
      unsigned value = animation->hover;
      if (value < animation->target) {
        value += 32u;
        if (value > animation->target)
          value = animation->target;
      } else if (value > animation->target) {
        value = value > 32u ? value - 32u : 0u;
        if (value < animation->target)
          value = animation->target;
      }
      animation->hover = (uint8_t)value;
      InvalidateRect(window, NULL, FALSE);
      if (animation->hover == animation->target)
        KillTimer(window, ID_BUTTON_TIMER);
      return 0;
    }
    break;
  case WM_NCDESTROY:
    KillTimer(window, ID_BUTTON_TIMER);
    RemoveWindowSubclass(window, button_proc, subclass);
    break;
  default:
    break;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

static int initialize_button_animations(void) {
  HWND buttons[] = {
      g_browse,        g_compress,       g_save,          g_options,
      g_threads,       g_color_profile,  g_icc_browse,    g_alpha,
      g_view_open,     g_view_zoom_out,  g_view_fit,      g_view_zoom_in,
      g_view_encoder,  g_view_previous,  g_view_play,     g_view_next,
      g_view_save_png};
  g_button_animation_count = _countof(buttons);
  for (size_t index = 0; index < _countof(buttons); ++index) {
    ButtonAnimation *animation = &g_button_animations[index];
    animation->window = buttons[index];
    if (!SetWindowSubclass(buttons[index], button_proc, 1u,
                           (DWORD_PTR)animation))
      return 0;
  }
  return 1;
}

static HFONT make_font(int points, int weight, const wchar_t *face) {
  return CreateFontW(-MulDiv(points, (int)g_dpi, 72), 0, 0, 0, weight, 0, 0, 0,
                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

static void replace_fonts(void) {
  if (g_font)
    DeleteObject(g_font);
  if (g_title_font)
    DeleteObject(g_title_font);
  if (g_result_font)
    DeleteObject(g_result_font);
  if (g_placeholder_font)
    DeleteObject(g_placeholder_font);
  if (g_label_font)
    DeleteObject(g_label_font);
  if (g_mono_font)
    DeleteObject(g_mono_font);
  g_font = make_font(10, FW_NORMAL, L"Segoe UI Variable Text");
  g_title_font = make_font(22, FW_SEMIBOLD, L"Segoe UI Variable Display");
  g_result_font = make_font(28, FW_SEMIBOLD, L"Segoe UI Variable Display");
  g_placeholder_font = make_font(13, FW_MEDIUM, L"Segoe UI Variable Text");
  g_label_font = make_font(9, FW_SEMIBOLD, L"Segoe UI Variable Text");
  g_mono_font = make_font(9, FW_NORMAL, L"Cascadia Mono");
}

static void apply_fonts(void) {
  HWND controls[] = {g_input,          g_input_details, g_browse,
                     g_compress,       g_save,          g_status,
                     g_result_details, g_options,       g_threads,
                     g_threads_list,   g_color_profile, g_color_profile_list,
                     g_icc_path,       g_icc_browse,    g_alpha,
                     g_alpha_list,     g_options_note,  g_view_open,
                     g_view_zoom_out,  g_view_fit,      g_view_zoom_in,
                     g_view_encoder,   g_view_details,  g_view_previous,
                     g_view_play,      g_view_next,     g_view_frame_label,
                     g_view_save_png,  g_view_note};
  for (size_t index = 0; index < sizeof(controls) / sizeof(controls[0]);
       ++index)
    if (controls[index])
      SendMessageW(controls[index], WM_SETFONT, (WPARAM)g_font, TRUE);
  if (g_result)
    SendMessageW(g_result, WM_SETFONT,
                 (WPARAM)(g_result_placeholder ? g_placeholder_font
                                               : g_result_font),
                 TRUE);
  if (g_input_label)
    SendMessageW(g_input_label, WM_SETFONT, (WPARAM)g_label_font, TRUE);
  if (g_result_label)
    SendMessageW(g_result_label, WM_SETFONT, (WPARAM)g_label_font, TRUE);
  if (g_view_label)
    SendMessageW(g_view_label, WM_SETFONT, (WPARAM)g_label_font, TRUE);
  if (g_view_pixel)
    SendMessageW(g_view_pixel, WM_SETFONT, (WPARAM)g_mono_font, TRUE);
  HWND labels[] = {g_threads_label, g_color_profile_label, g_icc_label,
                   g_alpha_label};
  for (size_t index = 0; index < sizeof(labels) / sizeof(labels[0]); ++index)
    if (labels[index])
      SendMessageW(labels[index], WM_SETFONT, (WPARAM)g_label_font, TRUE);
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
    StringCchPrintfW(text, capacity, L"%llu bytes", (unsigned long long)bytes);
  else
    StringCchPrintfW(text, capacity, L"%.2f %ls", value, unit);
}

static double monotonic_milliseconds(void) {
  LARGE_INTEGER counter;
  LARGE_INTEGER frequency;
  if (QueryPerformanceCounter(&counter) &&
      QueryPerformanceFrequency(&frequency))
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
  return (double)GetTickCount64();
}

static void format_duration(double milliseconds, wchar_t *text,
                            size_t capacity) {
  if (milliseconds < 10.0)
    StringCchPrintfW(text, capacity, L"%.2f ms", milliseconds);
  else
    StringCchPrintfW(text, capacity, L"%.2f seconds",
                     milliseconds / 1000.0);
}

static int multiply_size(size_t left, size_t right, size_t *result) {
  if (left && right > SIZE_MAX / left)
    return 0;
  *result = left * right;
  return 1;
}

static uint8_t *read_file_bytes(const wchar_t *path, uint64_t limit,
                                size_t *size) {
  FILE *file = NULL;
  if (_wfopen_s(&file, path, L"rb") || !file)
    return NULL;
  if (_fseeki64(file, 0, SEEK_END)) {
    fclose(file);
    return NULL;
  }
  __int64 length = _ftelli64(file);
  if (length <= 0 || (uint64_t)length > limit || (uint64_t)length > SIZE_MAX ||
      _fseeki64(file, 0, SEEK_SET)) {
    fclose(file);
    return NULL;
  }
  uint8_t *bytes = (uint8_t *)malloc((size_t)length);
  if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
    free(bytes);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *size = (size_t)length;
  return bytes;
}

static void view_error(ViewResult *result, const wchar_t *fallback) {
  const char *error = qlic_last_error();
  if (error && error[0]) {
    int written = MultiByteToWideChar(CP_UTF8, 0, error, -1, result->message,
                                      (int)_countof(result->message));
    if (written)
      return;
  }
  wcscpy_s(result->message, _countof(result->message), fallback);
}

static uint8_t scale_sample(uint32_t value, uint32_t bits) {
  uint32_t maximum = bits == 24u ? 0xFFFFFFu : ((UINT32_C(1) << bits) - 1u);
  return (uint8_t)(((uint64_t)value * 255u + maximum / 2u) / maximum);
}

static uint32_t load_sample(const uint8_t *sample, uint32_t bits) {
  if (bits <= 8u)
    return sample[0];
  if (bits <= 16u) {
    uint16_t value;
    memcpy(&value, sample, sizeof(value));
    return value;
  }
  uint32_t value;
  memcpy(&value, sample, sizeof(value));
  return value;
}

static void make_straight(uint8_t *rgba, size_t pixels, uint32_t alpha_mode) {
  if (alpha_mode != QLIC_ALPHA_PREMULTIPLIED)
    return;
  for (size_t index = 0; index < pixels; ++index) {
    uint8_t *pixel = rgba + index * 4u;
    uint32_t alpha = pixel[3];
    if (!alpha) {
      pixel[0] = pixel[1] = pixel[2] = 0;
      continue;
    }
    for (size_t channel = 0; channel < 3u; ++channel) {
      uint32_t straight = ((uint32_t)pixel[channel] * 255u + alpha / 2u) /
                          alpha;
      pixel[channel] = (uint8_t)(straight > 255u ? 255u : straight);
    }
  }
}

static int apply_icc_profile(uint8_t *rgba, uint32_t width, uint32_t height,
                             size_t stride, const uint8_t *icc,
                             size_t icc_size) {
  if (!rgba || !width || !height || !icc || !icc_size ||
      icc_size > UINT32_MAX)
    return 0;
  PROFILE input_description = {PROFILE_MEMBUFFER, (PVOID)icc, (DWORD)icc_size};
  HPROFILE input = OpenColorProfileW(&input_description, PROFILE_READ,
                                     FILE_SHARE_READ, OPEN_EXISTING);
  if (!input)
    return 0;

  wchar_t display_path[PATH_CAP];
  DWORD display_chars = (DWORD)_countof(display_path);
  HDC screen = GetDC(NULL);
  int found_display = screen && SetICMMode(screen, ICM_ON) != ICM_OFF &&
                      GetICMProfileW(screen, &display_chars, display_path);
  if (screen)
    ReleaseDC(NULL, screen);
  if (!found_display) {
    DWORD display_bytes = sizeof(display_path);
    found_display = GetStandardColorSpaceProfileW(
        NULL, LCS_sRGB, display_path, &display_bytes) != FALSE;
  }
  if (!found_display) {
    wchar_t system_directory[MAX_PATH];
    UINT system_length =
        GetSystemDirectoryW(system_directory, _countof(system_directory));
    found_display =
        system_length && system_length < _countof(system_directory) &&
        SUCCEEDED(StringCchPrintfW(
            display_path, _countof(display_path),
            L"%ls\\spool\\drivers\\color\\sRGB Color Space Profile.icm",
            system_directory)) &&
        GetFileAttributesW(display_path) != INVALID_FILE_ATTRIBUTES;
  }
  if (!found_display) {
    CloseColorProfile(input);
    return 0;
  }

  PROFILE output_description = {
      PROFILE_FILENAME, display_path,
      (DWORD)((wcslen(display_path) + 1u) * sizeof(wchar_t))};
  HPROFILE output = OpenColorProfileW(&output_description, PROFILE_READ,
                                      FILE_SHARE_READ, OPEN_EXISTING);
  if (!output) {
    CloseColorProfile(input);
    return 0;
  }
  HPROFILE profiles[2] = {input, output};
  DWORD intents[2] = {INTENT_PERCEPTUAL, INTENT_PERCEPTUAL};
  HTRANSFORM transform = CreateMultiProfileTransform(
      profiles, 2, intents, 2, BEST_MODE, INDEX_DONT_CARE);
  if (!transform) {
    CloseColorProfile(output);
    CloseColorProfile(input);
    return 0;
  }

  size_t packed_row = 0;
  int ok = multiply_size((size_t)width, 3u, &packed_row);
  size_t color_stride = ok ? (packed_row + 3u) & ~(size_t)3u : 0u;
  const uint32_t rows_per_chunk = 32u;
  size_t chunk_bytes = 0;
  ok = ok && multiply_size(color_stride, rows_per_chunk, &chunk_bytes);
  uint8_t *source = ok ? (uint8_t *)malloc(chunk_bytes) : NULL;
  uint8_t *destination = ok ? (uint8_t *)malloc(chunk_bytes) : NULL;
  ok = source && destination;
  for (uint32_t top = 0; ok && top < height; top += rows_per_chunk) {
    uint32_t rows = height - top;
    if (rows > rows_per_chunk)
      rows = rows_per_chunk;
    memset(source, 0, color_stride * rows);
    for (uint32_t y = 0; y < rows; ++y) {
      const uint8_t *input_row = rgba + (size_t)(top + y) * stride;
      uint8_t *packed = source + (size_t)y * color_stride;
      for (uint32_t x = 0; x < width; ++x) {
        packed[(size_t)x * 3u] = input_row[(size_t)x * 4u + 2u];
        packed[(size_t)x * 3u + 1u] = input_row[(size_t)x * 4u + 1u];
        packed[(size_t)x * 3u + 2u] = input_row[(size_t)x * 4u];
      }
    }
    ok = TranslateBitmapBits(transform, source, BM_BGRTRIPLETS, width, rows,
                             (DWORD)color_stride, destination, BM_BGRTRIPLETS,
                             (DWORD)color_stride, NULL, 0) != FALSE;
    for (uint32_t y = 0; ok && y < rows; ++y) {
      uint8_t *output_row = rgba + (size_t)(top + y) * stride;
      const uint8_t *packed = destination + (size_t)y * color_stride;
      for (uint32_t x = 0; x < width; ++x) {
        output_row[(size_t)x * 4u] = packed[(size_t)x * 3u + 2u];
        output_row[(size_t)x * 4u + 1u] = packed[(size_t)x * 3u + 1u];
        output_row[(size_t)x * 4u + 2u] = packed[(size_t)x * 3u];
      }
    }
  }
  free(destination);
  free(source);
  DeleteColorTransform(transform);
  CloseColorProfile(output);
  CloseColorProfile(input);
  return ok;
}

static int rgba_to_frame(ViewFrame *frame, uint8_t *rgba, uint32_t width,
                         uint32_t height, size_t stride, uint32_t alpha_mode,
                         const uint8_t *icc, size_t icc_size,
                         int *color_managed, int *has_alpha) {
  size_t row = 0;
  size_t pixels = 0;
  size_t bytes = 0;
  if (!frame || !rgba || !color_managed || !has_alpha || !width || !height ||
      !multiply_size((size_t)width, 4u, &row) || stride < row ||
      !multiply_size((size_t)width, height, &pixels) ||
      !multiply_size(row, height, &bytes))
    return 0;
  make_straight(rgba, pixels, alpha_mode);
  frame->rgba = (uint8_t *)malloc(bytes);
  if (!frame->rgba)
    return 0;
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t *source = rgba + (size_t)y * stride;
    uint8_t *destination = frame->rgba + (size_t)y * row;
    memcpy(destination, source, row);
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t *pixel = source + (size_t)x * 4u;
      uint32_t alpha = pixel[3];
      if (alpha != 255u)
        *has_alpha = 1;
    }
  }
  if (icc && icc_size) {
    frame->display_rgba = (uint8_t *)malloc(bytes);
    if (frame->display_rgba) {
      memcpy(frame->display_rgba, frame->rgba, bytes);
      if (apply_icc_profile(frame->display_rgba, width, height, row, icc,
                            icc_size)) {
        *color_managed = 1;
      } else {
        free(frame->display_rgba);
        frame->display_rgba = NULL;
      }
    }
  }
  return 1;
}

static uint8_t *hdr_to_rgba8(const qlic_hdr_image *image, size_t *stride) {
  if (!image || !stride || !image->pixels || !image->width || !image->height ||
      (image->channels != 1u && image->channels != 3u &&
       image->channels != 4u) ||
      image->bits_per_sample < 8u || image->bits_per_sample > 24u)
    return NULL;
  size_t sample_bytes = image->bits_per_sample <= 8u
                            ? 1u
                            : image->bits_per_sample <= 16u ? 2u : 4u;
  size_t samples_per_row = 0;
  size_t input_row = 0;
  size_t row = 0;
  size_t bytes = 0;
  size_t last_row_offset = 0;
  if (!multiply_size((size_t)image->width, image->channels,
                     &samples_per_row) ||
      !multiply_size(samples_per_row, sample_bytes, &input_row) ||
      image->stride < input_row ||
      !multiply_size((size_t)image->height - 1u, image->stride,
                     &last_row_offset) ||
      input_row > SIZE_MAX - last_row_offset ||
      image->pixels_size < last_row_offset + input_row ||
      !multiply_size((size_t)image->width, 4u, &row) ||
      !multiply_size(row, image->height, &bytes))
    return NULL;
  uint8_t *rgba = (uint8_t *)malloc(bytes);
  if (!rgba)
    return NULL;
  for (uint32_t y = 0; y < image->height; ++y) {
    const uint8_t *source = (const uint8_t *)image->pixels +
                            (size_t)y * image->stride;
    uint8_t *destination = rgba + (size_t)y * row;
    for (uint32_t x = 0; x < image->width; ++x) {
      const uint8_t *pixel =
          source + (size_t)x * image->channels * sample_bytes;
      uint8_t red = scale_sample(load_sample(pixel, image->bits_per_sample),
                                 image->bits_per_sample);
      uint8_t green = red;
      uint8_t blue = red;
      uint8_t alpha = 255u;
      if (image->channels >= 3u) {
        green = scale_sample(load_sample(pixel + sample_bytes,
                                         image->bits_per_sample),
                             image->bits_per_sample);
        blue = scale_sample(load_sample(pixel + sample_bytes * 2u,
                                        image->bits_per_sample),
                            image->bits_per_sample);
      }
      if (image->channels == 4u)
        alpha = scale_sample(load_sample(pixel + sample_bytes * 3u,
                                         image->bits_per_sample),
                             image->bits_per_sample);
      destination[(size_t)x * 4u] = red;
      destination[(size_t)x * 4u + 1u] = green;
      destination[(size_t)x * 4u + 2u] = blue;
      destination[(size_t)x * 4u + 3u] = alpha;
    }
  }
  *stride = row;
  return rgba;
}

static void free_view_result(ViewResult *result) {
  if (!result)
    return;
  if (result->frames) {
    for (uint32_t index = 0; index < result->frame_count; ++index) {
      free(result->frames[index].rgba);
      free(result->frames[index].display_rgba);
    }
    free(result->frames);
  }
  free(result->icc);
  free(result);
}

static void post_view_result(ViewJob *job, ViewResult *result) {
  if (!PostMessageW(job->window, WM_QLIC_VIEW_DONE, 0, (LPARAM)result))
    free_view_result(result);
  free(job);
}

static DWORD WINAPI decode_view(LPVOID parameter) {
  ViewJob *job = (ViewJob *)parameter;
  ViewResult *result = (ViewResult *)calloc(1, sizeof(*result));
  if (!result) {
    post_view_result(job, NULL);
    return 0;
  }
  wcscpy_s(result->path, PATH_CAP, job->path);
  double started = monotonic_milliseconds();
  qlic_decode_limits_v2 limits;
  qlic_decode_limits_v2_default(&limits);
  limits.threads = qlic_hardware_thread_count();
  size_t file_bytes = 0;
  uint8_t *data =
      read_file_bytes(job->path, limits.max_file_bytes, &file_bytes);
  result->source_size = file_bytes;
  if (!data) {
    wcscpy_s(result->message, _countof(result->message),
             L"The QLIC file could not be read within the decoder limits.");
    post_view_result(job, result);
    return 0;
  }

  qlic_info_v2 info = {0};
  info.struct_size = sizeof(info);
  int status = qlic_get_info_v2(data, file_bytes, &limits, &info);
  if (status != QLIC_OK) {
    view_error(result, L"The QLIC header is invalid or unsupported.");
    free(data);
    post_view_result(job, result);
    return 0;
  }
  result->width = info.width;
  result->height = info.height;
  result->channels = info.channels;
  result->bits_per_sample = info.bits_per_sample;
  result->alpha_mode = info.alpha_mode;
  result->color_authority = info.color_authority;
  result->has_icc = info.has_icc != 0;
  result->has_cicp = info.has_cicp != 0;
  result->hdr_preview = info.has_cicp &&
                        (info.has_mastering_display || info.has_content_light);

  if (info.animated) {
    qlic_decode_limits animation_limits;
    qlic_decode_limits_default(&animation_limits);
    animation_limits.threads = limits.threads;
    qlic_animation animation = {0};
    status = qlic_decode_animation(data, file_bytes, &animation_limits,
                                   &animation);
    if (status == QLIC_OK && animation.frame_count) {
      result->frame_count = animation.frame_count;
      result->frames =
          (ViewFrame *)calloc(result->frame_count, sizeof(*result->frames));
      if (!result->frames)
        status = QLIC_OUT_OF_MEMORY;
      for (uint32_t index = 0;
           status == QLIC_OK && index < result->frame_count; ++index) {
        qlic_image *image = &animation.frames[index].image;
        result->frames[index].delay_ms = animation.frames[index].delay_ms;
        if (!rgba_to_frame(&result->frames[index], image->rgba, image->width,
                           image->height, image->stride, info.alpha_mode, NULL,
                           0, &result->color_managed, &result->has_alpha))
          status = QLIC_OUT_OF_MEMORY;
      }
    }
    qlic_animation_free(&animation);
  } else {
    result->frame_count = 1u;
    result->frames = (ViewFrame *)calloc(1u, sizeof(*result->frames));
    if (!result->frames)
      status = QLIC_OUT_OF_MEMORY;
    else {
      qlic_hdr_image image = {0};
      image.struct_size = sizeof(image);
      status = qlic_decode_hdr(data, file_bytes, &limits, &image);
      if (status == QLIC_OK) {
        if (image.has_cicp)
          result->transfer_characteristics =
              image.cicp.transfer_characteristics;
        if (image.icc && image.icc_size) {
          result->icc = (uint8_t *)malloc(image.icc_size);
          if (!result->icc) {
            status = QLIC_OUT_OF_MEMORY;
          } else {
            memcpy(result->icc, image.icc, image.icc_size);
            result->icc_size = image.icc_size;
          }
        }
        size_t rgba_stride = 0;
        uint8_t *rgba = status == QLIC_OK ? hdr_to_rgba8(&image, &rgba_stride)
                                          : NULL;
        if (status == QLIC_OK &&
            (!rgba || !rgba_to_frame(
                          &result->frames[0], rgba, image.width, image.height,
                          rgba_stride, image.alpha_mode, image.icc,
                          image.icc_size, &result->color_managed,
                          &result->has_alpha)))
          status = QLIC_OUT_OF_MEMORY;
        free(rgba);
        result->hdr_preview = image.has_cicp &&
                              (image.cicp.transfer_characteristics ==
                                   QLIC_CICP_TRANSFER_PQ ||
                               image.cicp.transfer_characteristics ==
                                   QLIC_CICP_TRANSFER_HLG);
        qlic_hdr_image_free(&image);
      } else if (info.bits_per_sample == 8u) {
        size_t rgba_stride = 0;
        size_t rgba_bytes = 0;
        if (!multiply_size((size_t)info.width, 4u, &rgba_stride) ||
            !multiply_size(rgba_stride, info.height, &rgba_bytes)) {
          status = QLIC_OUT_OF_MEMORY;
        } else {
          uint8_t *rgba = (uint8_t *)malloc(rgba_bytes);
          if (!rgba) {
            status = QLIC_OUT_OF_MEMORY;
          } else {
            qlic_pixel_buffer pixels = {0};
            pixels.struct_size = sizeof(pixels);
            pixels.format = QLIC_PIXELS_RGBA8;
            pixels.pixels = rgba;
            pixels.pixels_size = rgba_bytes;
            pixels.stride = rgba_stride;
            status = qlic_decode_pixels(data, file_bytes, &limits, &pixels);
            if (status == QLIC_OK &&
                !rgba_to_frame(&result->frames[0], rgba, info.width,
                               info.height, rgba_stride, info.alpha_mode, NULL,
                               0, &result->color_managed,
                               &result->has_alpha))
              status = QLIC_OUT_OF_MEMORY;
            free(rgba);
          }
        }
      } else {
        qlic_decode_limits wide_limits;
        qlic_decode_limits_default(&wide_limits);
        wide_limits.threads = limits.threads;
        qlic_wide_image wide = {0};
        status = qlic_decode_wide(data, file_bytes, &wide_limits, &wide);
        if (status == QLIC_OK) {
          qlic_hdr_image adapted = {0};
          adapted.struct_size = sizeof(adapted);
          adapted.width = wide.width;
          adapted.height = wide.height;
          adapted.channels = wide.channels;
          adapted.bits_per_sample = wide.bits_per_sample;
          adapted.sample_type = QLIC_SAMPLE_UINT;
          adapted.alpha_mode = info.alpha_mode;
          adapted.pixels = wide.pixels;
          adapted.pixels_size = wide.pixels_size;
          adapted.stride = wide.stride;
          size_t rgba_stride = 0;
          uint8_t *rgba = hdr_to_rgba8(&adapted, &rgba_stride);
          if (!rgba || !rgba_to_frame(
                           &result->frames[0], rgba, wide.width, wide.height,
                           rgba_stride, info.alpha_mode, NULL, 0,
                           &result->color_managed, &result->has_alpha))
            status = QLIC_OUT_OF_MEMORY;
          free(rgba);
          qlic_wide_image_free(&wide);
        }
      }
    }
  }
  free(data);
  result->stride = (size_t)result->width * 4u;
  result->milliseconds = monotonic_milliseconds() - started;
  if (status == QLIC_OK && result->frames && result->frames[0].rgba) {
    result->ok = 1;
  } else {
    view_error(result, L"The QLIC pixels could not be decoded for display.");
  }
  post_view_result(job, result);
  return 0;
}

static void remove_temporary_output(void) {
  if (g_temp_output[0])
    DeleteFileW(g_temp_output);
  g_temp_output[0] = 0;
}

static void set_result_heading(const wchar_t *text, int placeholder) {
  g_result_placeholder = placeholder;
  SendMessageW(g_result, WM_SETFONT,
               (WPARAM)(placeholder ? g_placeholder_font : g_result_font),
               TRUE);
  SetWindowTextW(g_result, text);
}

static void clear_result(void) {
  remove_temporary_output();
  set_result_heading(L"No result", 1);
  SetWindowTextW(g_result_details, L"");
  EnableWindow(g_save, FALSE);
}

static const wchar_t *file_name_part(const wchar_t *path) {
  const wchar_t *slash = wcsrchr(path, L'\\');
  const wchar_t *forward = wcsrchr(path, L'/');
  if (forward && (!slash || forward > slash))
    slash = forward;
  return slash ? slash + 1 : path;
}

static void destroy_view_bitmap(void) {
  if (g_view_bitmap)
    DeleteObject(g_view_bitmap);
  g_view_bitmap = NULL;
}

static int select_view_frame(uint32_t index) {
  if (!g_view || !g_view->ok || index >= g_view->frame_count ||
      !g_view->frames[index].rgba)
    return 0;
  destroy_view_bitmap();
  BITMAPINFO bitmap = {0};
  bitmap.bmiHeader.biSize = sizeof(bitmap.bmiHeader);
  bitmap.bmiHeader.biWidth = (LONG)g_view->width;
  bitmap.bmiHeader.biHeight = -(LONG)g_view->height;
  bitmap.bmiHeader.biPlanes = 1;
  bitmap.bmiHeader.biBitCount = 32;
  bitmap.bmiHeader.biCompression = BI_RGB;
  void *bits = NULL;
  HDC screen = GetDC(NULL);
  g_view_bitmap =
      CreateDIBSection(screen, &bitmap, DIB_RGB_COLORS, &bits, NULL, 0);
  if (screen)
    ReleaseDC(NULL, screen);
  if (!g_view_bitmap || !bits) {
    destroy_view_bitmap();
    return 0;
  }
  size_t bytes = 0;
  if (!multiply_size(g_view->stride, g_view->height, &bytes)) {
    destroy_view_bitmap();
    return 0;
  }
  const uint8_t *rgba = g_view->frames[index].display_rgba
                            ? g_view->frames[index].display_rgba
                            : g_view->frames[index].rgba;
  uint8_t *bgra = (uint8_t *)bits;
  size_t pixels = bytes / 4u;
  for (size_t pixel = 0; pixel < pixels; ++pixel) {
    const uint8_t *source = rgba + pixel * 4u;
    uint8_t *destination = bgra + pixel * 4u;
    uint32_t alpha = source[3];
    destination[0] =
        (uint8_t)(((uint32_t)source[2] * alpha + 127u) / 255u);
    destination[1] =
        (uint8_t)(((uint32_t)source[1] * alpha + 127u) / 255u);
    destination[2] =
        (uint8_t)(((uint32_t)source[0] * alpha + 127u) / 255u);
    destination[3] = source[3];
  }
  g_view_frame = index;
  return 1;
}

static void show_encoder_controls(int show) {
  HWND controls[] = {
      g_input,         g_input_details, g_browse,           g_save,
      g_input_label,   g_options,       g_output,           g_threads_list,
      g_color_profile_list,             g_alpha_list};
  for (size_t index = 0; index < _countof(controls); ++index)
    ShowWindow(controls[index], show ? SW_SHOW : SW_HIDE);
  if (show)
    update_options();
}

static void show_viewer_controls(int show) {
  HWND controls[] = {
      g_view_open,       g_view_zoom_out, g_view_fit,      g_view_zoom_in,
      g_view_encoder,    g_view_details,  g_view_previous, g_view_play,
      g_view_next,       g_view_frame_label, g_view_save_png,
      g_view_pixel,      g_view_note,     g_view_label};
  for (size_t index = 0; index < _countof(controls); ++index)
    ShowWindow(controls[index], show ? SW_SHOW : SW_HIDE);
}

static void stop_view_playback(HWND window) {
  KillTimer(window, ID_VIEW_TIMER);
  g_view_playing = 0;
  if (g_view_play)
    SetWindowTextW(g_view_play, L"Play");
}

static void update_view_buttons(void) {
  int ready = !g_view_loading && g_view && g_view->ok;
  int animated = ready && g_view->frame_count > 1u;
  EnableWindow(g_view_open, !g_view_loading);
  EnableWindow(g_view_zoom_out, ready);
  EnableWindow(g_view_fit, ready);
  EnableWindow(g_view_zoom_in, ready);
  EnableWindow(g_view_encoder, !g_view_loading);
  EnableWindow(g_view_save_png, ready);
  HWND animation[] = {g_view_previous, g_view_play, g_view_next,
                      g_view_frame_label};
  for (size_t index = 0; index < _countof(animation); ++index)
    if (animation[index])
      ShowWindow(animation[index], animated ? SW_SHOW : SW_HIDE);
  if (g_view_frame_label && animated) {
    wchar_t text[64];
    StringCchPrintfW(text, _countof(text), L"Frame %u / %u", g_view_frame + 1u,
                     g_view->frame_count);
    SetWindowTextW(g_view_frame_label, text);
  }
  update_view_zoom_label(g_main_window);
  if (g_view_zoom_out)
    InvalidateRect(g_view_zoom_out, NULL, TRUE);
  if (g_view_fit)
    InvalidateRect(g_view_fit, NULL, TRUE);
  if (g_view_zoom_in)
    InvalidateRect(g_view_zoom_in, NULL, TRUE);
}

static void enter_viewer_mode(HWND window, const wchar_t *path) {
  g_view_mode = 1;
  g_options_open = 0;
  g_options_animating = 0;
  g_options_position = 0;
  g_options_frame = 0;
  KillTimer(window, ID_OPTIONS_TIMER);
  close_choice_lists();
  SetParent(g_status, window);
  SetParent(g_progress, window);
  show_encoder_controls(0);
  show_viewer_controls(1);
  wchar_t title[PATH_CAP];
  if (FAILED(StringCchPrintfW(title, _countof(title), L"%ls - QLIC",
                              file_name_part(path))))
    wcscpy_s(title, _countof(title), L"QLIC Viewer");
  SetWindowTextW(window, title);
  layout(window);
  InvalidateRect(window, NULL, TRUE);
}

static void leave_viewer_mode(HWND window) {
  stop_view_playback(window);
  g_view_loading = 0;
  destroy_view_bitmap();
  free_view_result(g_view);
  g_view = NULL;
  g_view_mode = 0;
  g_view_frame = 0;
  g_view_fit_mode = 1;
  g_view_zoom = 1.0;
  g_view_pan_x = g_view_pan_y = 0;
  g_view_dragging = 0;
  show_viewer_controls(0);
  SetParent(g_status, g_output);
  SetParent(g_progress, g_output);
  show_encoder_controls(1);
  SetWindowTextW(window, L"QLIC");
  SetWindowTextW(g_status, L"Choose an image to compress, or open a QLIC file.");
  ShowWindow(g_progress, SW_HIDE);
  update_controls();
  layout(window);
  InvalidateRect(window, NULL, TRUE);
}

static void start_viewer(HWND window, const wchar_t *path) {
  uint64_t bytes = 0;
  if (!window || !path || !path[0] || !file_size(path, &bytes)) {
    SetWindowTextW(g_status, L"The selected QLIC file is not available.");
    return;
  }
  if (g_view_loading)
    return;
  stop_view_playback(window);
  destroy_view_bitmap();
  free_view_result(g_view);
  g_view = NULL;
  g_view_loading = 1;
  g_view_frame = 0;
  g_view_fit_mode = 1;
  g_view_zoom = 1.0;
  g_view_pan_x = g_view_pan_y = 0;
  enter_viewer_mode(window, path);
  SetWindowTextW(g_view_details, L"Decoding and verifying QLIC\u2026");
  SetWindowTextW(g_view_note, L"");
  reset_pixel_text();
  SetWindowTextW(g_status, L"Opening QLIC\u2026");
  SendMessageW(g_progress, PBM_SETMARQUEE, TRUE, 35);
  ShowWindow(g_progress, SW_SHOW);
  update_view_buttons();

  ViewJob *job = (ViewJob *)calloc(1, sizeof(*job));
  if (!job || wcscpy_s(job->path, PATH_CAP, path)) {
    free(job);
    g_view_loading = 0;
    ShowWindow(g_progress, SW_HIDE);
    SetWindowTextW(g_view_details, L"The QLIC path is too long.");
    update_view_buttons();
    return;
  }
  job->window = window;
  HANDLE thread = CreateThread(NULL, 0, decode_view, job, 0, NULL);
  if (!thread) {
    free(job);
    g_view_loading = 0;
    ShowWindow(g_progress, SW_HIDE);
    SetWindowTextW(g_view_details, L"The QLIC decoder could not be started.");
    update_view_buttons();
    return;
  }
  CloseHandle(thread);
  /* decode_view owns job after CreateThread succeeds and releases it through
     post_view_result on every exit. */
  // cppcheck-suppress memleak
}

static void redraw_choice_buttons(void) {
  if (g_threads)
    InvalidateRect(g_threads, NULL, TRUE);
  if (g_color_profile)
    InvalidateRect(g_color_profile, NULL, TRUE);
  if (g_alpha)
    InvalidateRect(g_alpha, NULL, TRUE);
}

static void close_choice_lists(void) {
  if (g_threads_list)
    ShowWindow(g_threads_list, SW_HIDE);
  if (g_color_profile_list)
    ShowWindow(g_color_profile_list, SW_HIDE);
  if (g_alpha_list)
    ShowWindow(g_alpha_list, SW_HIDE);
  g_open_choice = 0;
  redraw_choice_buttons();
}

static void toggle_choice_list(int list_id, HWND list) {
  int was_open = g_open_choice == list_id;
  close_choice_lists();
  if (was_open || !g_options_open || g_options_animating || g_busy || !list)
    return;
  g_open_choice = list_id;
  SetWindowPos(list, HWND_TOP, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  SetFocus(list);
  redraw_choice_buttons();
}

static int apply_choice(HWND button, HWND list, int *selection) {
  LRESULT selected = SendMessageW(list, LB_GETCURSEL, 0, 0);
  if (selected == LB_ERR)
    return 0;
  wchar_t text[96];
  if (SendMessageW(list, LB_GETTEXT, (WPARAM)selected, (LPARAM)text) == LB_ERR)
    return 0;
  *selection = (int)selected;
  SetWindowTextW(button, text);
  close_choice_lists();
  SetFocus(button);
  return 1;
}

static void update_options(void) {
  int enabled = (g_options_open || g_options_position) && !g_busy;
  int interactive = g_options_open && !g_options_animating && !g_busy;
  if (!interactive)
    close_choice_lists();
  int icc = g_profile_selection == 6;
  if (g_icc_path)
    EnableWindow(g_icc_path, enabled && icc);
  if (g_icc_browse)
    EnableWindow(g_icc_browse, enabled && icc);
  if (g_threads)
    EnableWindow(g_threads, enabled);
  if (g_color_profile)
    EnableWindow(g_color_profile, enabled);
  if (g_alpha)
    EnableWindow(g_alpha, enabled && g_profile_selection > 0);
  if (g_options_note) {
    const wchar_t *note;
    if (g_profile_selection == 0)
      note = L"No color or alpha metadata.";
    else if (g_profile_selection == 6)
      note = L"Embeds the ICC profile. No tone mapping.";
    else
      note = L"Stores color and alpha metadata. No tone mapping.";
    SetWindowTextW(g_options_note, note);
  }
  if (g_options)
    RedrawWindow(g_options, NULL, NULL,
                 RDW_INVALIDATE | RDW_ALLCHILDREN);
}

static void update_controls(void) {
  wchar_t input[PATH_CAP];
  GetWindowTextW(g_input, input, PATH_CAP);
  EnableWindow(g_input, !g_busy);
  EnableWindow(g_browse, !g_busy);
  EnableWindow(g_compress, g_busy || input[0]);
  EnableWindow(g_options, !g_busy);
  SetWindowTextW(g_compress, g_busy ? L"Cancel" : L"Compress");
  update_options();
}

static void set_input(const wchar_t *path) {
  if (!path || !path[0])
    return;
  if (has_extension(path, L".qlic")) {
    start_viewer(g_main_window, path);
    return;
  }
  if (g_view_mode)
    leave_viewer_mode(g_main_window);
  uint64_t bytes = 0;
  if (!file_size(path, &bytes)) {
    SetWindowTextW(g_status, L"The selected image is not available.");
    return;
  }
  SetWindowTextW(g_input, path);
  wchar_t size_text[64];
  wchar_t details[160];
  format_size(bytes, size_text, 64);
  StringCchPrintfW(details, 160, L"Original: %ls", size_text);
  SetWindowTextW(g_input_details, details);
  int lossy = has_extension(path, L".jpg") ||
              has_extension(path, L".jpeg") ||
              has_extension(path, L".jpe");
  SetWindowTextW(g_status, lossy ? QLIC_LOSSY_STATUS : L"Ready");
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
      L"Supported images\0*.qlic;*.png;*.jpg;*.jpeg;*.jpe;*.webp;*.jxl;"
      L"*.avif;*.bmp;*.tif;*.tiff;*.gif\0"
      L"QLIC images\0*.qlic\0"
      L"All files\0*.*\0";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
  if (GetOpenFileNameW(&dialog))
    set_input(path);
  free(path);
}

static void choose_icc(HWND window) {
  wchar_t *path = path_buffer();
  if (!path)
    return;
  GetWindowTextW(g_icc_path, path, PATH_CAP);
  OPENFILENAMEW dialog = {0};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window;
  dialog.lpstrFile = path;
  dialog.nMaxFile = PATH_CAP;
  dialog.lpstrFilter = L"ICC color profiles\0*.icc;*.icm\0All files\0*.*\0";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
  if (GetOpenFileNameW(&dialog))
    SetWindowTextW(g_icc_path, path);
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
  if (wcslen(temporary) + 1u > capacity) {
    DeleteFileW(temporary);
    return 0;
  }
  return wcscpy_s(output, capacity, temporary) == 0;
}

static void post_result(Job *job, Result *result) {
  if (!PostMessageW(job->window, WM_QLIC_DONE, 0, (LPARAM)result)) {
    DeleteFileW(job->output);
    free(result);
  }
  free(job);
}

static volatile LONG g_cancel_requested;

static void capture_bytes(char *capture, size_t capacity, size_t *used,
                          const char *bytes, size_t count) {
  if (*used >= capacity - 1u)
    return;
  size_t available = capacity - 1u - *used;
  if (count > available)
    count = available;
  memcpy(capture + *used, bytes, count);
  *used += count;
  capture[*used] = 0;
}

static int run_child(const Job *job, const wchar_t *source_command,
                     char *capture, size_t capture_capacity,
                     size_t *capture_used, double *milliseconds) {
  SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
  HANDLE read_pipe = NULL;
  HANDLE write_pipe = NULL;
  if (!CreatePipe(&read_pipe, &write_pipe, &security, 0) ||
      !SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
    if (read_pipe)
      CloseHandle(read_pipe);
    if (write_pipe)
      CloseHandle(write_pipe);
    return 0;
  }

  wchar_t *command = path_buffer();
  if (!command) {
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    return 0;
  }
  wcscpy_s(command, PATH_CAP, source_command);

  STARTUPINFOEXW startup = {0};
  PROCESS_INFORMATION process = {0};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
  startup.StartupInfo.wShowWindow = SW_HIDE;
  startup.StartupInfo.hStdOutput = write_pipe;
  startup.StartupInfo.hStdError = write_pipe;
  SIZE_T attribute_bytes = 0;
  InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_bytes);
  startup.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(
      GetProcessHeap(), 0, attribute_bytes);
  if (!startup.lpAttributeList ||
      !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
                                         &attribute_bytes)) {
    if (startup.lpAttributeList)
      HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
    free(command);
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    return 0;
  }
  if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                 PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &write_pipe,
                                 sizeof(write_pipe), NULL, NULL)) {
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
    free(command);
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    return 0;
  }
  double started = monotonic_milliseconds();
  BOOL created = CreateProcessW(job->executable, command, NULL, NULL, TRUE,
                                CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                NULL, NULL, &startup.StartupInfo, &process);
  DeleteProcThreadAttributeList(startup.lpAttributeList);
  HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
  free(command);
  CloseHandle(write_pipe);
  if (!created) {
    CloseHandle(read_pipe);
    return 0;
  }

  HANDLE cancel_handle = NULL;
  if (DuplicateHandle(GetCurrentProcess(), process.hProcess,
                      GetCurrentProcess(), &cancel_handle,
                      PROCESS_TERMINATE | SYNCHRONIZE |
                          PROCESS_QUERY_LIMITED_INFORMATION,
                      FALSE, 0)) {
    HANDLE old = (HANDLE)InterlockedExchangePointer(
        (PVOID volatile *)&g_process, cancel_handle);
    if (old)
      CloseHandle(old);
  }

  char buffer[4096];
  DWORD count = 0;
  while (ReadFile(read_pipe, buffer, sizeof(buffer), &count, NULL) && count) {
    capture_bytes(capture, capture_capacity, capture_used, buffer, count);
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  if (milliseconds)
    *milliseconds = monotonic_milliseconds() - started;
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  if (cancel_handle) {
    HANDLE current = (HANDLE)InterlockedCompareExchangePointer(
        (PVOID volatile *)&g_process, NULL, cancel_handle);
    if (current == cancel_handle)
      CloseHandle(cancel_handle);
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  CloseHandle(read_pipe);
  return exit_code == 0;
}

static void result_message(Result *result, const char *capture) {
  if (!capture || !capture[0])
    return;
  const char *message = capture;
  const char *candidate = capture;
  while ((candidate = strstr(candidate, "error: ")) != NULL) {
    message = candidate + 7;
    candidate += 7;
  }
  int length = MultiByteToWideChar(
      CP_UTF8, 0, message, -1, result->message,
      (int)(sizeof(result->message) / sizeof(result->message[0])));
  if (!length)
    MultiByteToWideChar(
        CP_ACP, 0, message, -1, result->message,
        (int)(sizeof(result->message) / sizeof(result->message[0])));
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
  char capture[8192] = {0};
  size_t capture_used = 0;
  int packed = run_child(job, job->command, capture, sizeof(capture),
                         &capture_used, &result->milliseconds);
  result->lossy_source =
      strstr(capture, "warning: This source is lossy.") != NULL;
  result->cancelled =
      InterlockedCompareExchange(&g_cancel_requested, 0, 0) != 0;
  if (packed && !result->cancelled &&
      file_size(job->output, &result->output_size) &&
      result->output_size > 0u) {
    result->verified = run_child(job, job->verify_command, capture,
                                 sizeof(capture), &capture_used, NULL);
  }
  result->cancelled = result->cancelled || InterlockedCompareExchange(
                                               &g_cancel_requested, 0, 0) != 0;
  result->ok = packed && result->verified && !result->cancelled;
  result_message(result, capture);

  if (!result->ok)
    DeleteFileW(job->output);
  post_result(job, result);
  return 0;
}

static const wchar_t *color_profile_argument(int selection) {
  static const wchar_t *const profiles[] = {
      NULL,           L"srgb",           L"display-p3", L"rec2100-pq",
      L"rec2100-hlg", L"rec2020-linear", NULL};
  return selection >= 0 &&
                 (size_t)selection < sizeof(profiles) / sizeof(profiles[0])
             ? profiles[selection]
             : NULL;
}

static void cancel_compression(void) {
  if (!g_busy)
    return;
  InterlockedExchange(&g_cancel_requested, 1);
  HANDLE process =
      (HANDLE)InterlockedExchangePointer((PVOID volatile *)&g_process, NULL);
  if (process) {
    TerminateProcess(process, ERROR_CANCELLED);
    CloseHandle(process);
  }
  SetWindowTextW(g_status, L"Cancelling...");
  EnableWindow(g_compress, FALSE);
}

static void start_compression(HWND window) {
  if (g_busy) {
    cancel_compression();
    return;
  }
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
  wchar_t *quoted_icc = path_buffer();
  if (!job || !quoted_executable || !quoted_input || !quoted_output ||
      !quoted_icc) {
    free(job);
    free(quoted_executable);
    free(quoted_input);
    free(quoted_output);
    free(quoted_icc);
    SetWindowTextW(g_status, L"Could not prepare the compression test.");
    return;
  }
  if (!temporary_qlic(job->output, PATH_CAP)) {
    free(job);
    free(quoted_executable);
    free(quoted_input);
    free(quoted_output);
    free(quoted_icc);
    SetWindowTextW(g_status, L"Could not prepare the compression test.");
    return;
  }

  wchar_t threads[32] = L"all";
  if (g_thread_selection > 0 &&
      (size_t)g_thread_selection < g_thread_value_count)
    StringCchPrintfW(threads, 32, L"%u", g_thread_values[g_thread_selection]);
  int profile_selection = g_profile_selection;
  const wchar_t *profile = color_profile_argument(profile_selection);
  wchar_t icc_path[PATH_CAP] = {0};
  if (profile_selection == 6) {
    GetWindowTextW(g_icc_path, icc_path, PATH_CAP);
    uint64_t icc_size = 0;
    if (!icc_path[0] || !file_size(icc_path, &icc_size) || !icc_size) {
      DeleteFileW(job->output);
      free(job);
      free(quoted_executable);
      free(quoted_input);
      free(quoted_output);
      free(quoted_icc);
      SetWindowTextW(g_status, L"Choose a readable ICC profile first.");
      return;
    }
  }
  int valid =
      executable_path(job->executable, PATH_CAP) &&
      wquote(job->executable, quoted_executable, PATH_CAP) &&
      wquote(input, quoted_input, PATH_CAP) &&
      wquote(job->output, quoted_output, PATH_CAP) &&
      (profile_selection != 6 || wquote(icc_path, quoted_icc, PATH_CAP));
  HRESULT formatted = valid
                          ? StringCchPrintfW(job->command, PATH_CAP,
                                             L"%ls pack %ls %ls --threads %ls",
                                             quoted_executable, quoted_input,
                                             quoted_output, threads)
                          : STRSAFE_E_INSUFFICIENT_BUFFER;
  if (SUCCEEDED(formatted) && profile)
    formatted = StringCchPrintfW(job->command + wcslen(job->command),
                                 PATH_CAP - wcslen(job->command),
                                 L" --color-profile %ls", profile);
  if (SUCCEEDED(formatted) && profile_selection == 6)
    formatted = StringCchPrintfW(job->command + wcslen(job->command),
                                 PATH_CAP - wcslen(job->command), L" --icc %ls",
                                 quoted_icc);
  if (SUCCEEDED(formatted) && profile_selection > 0) {
    if (g_alpha_selection == 1)
      formatted = StringCchPrintfW(job->command + wcslen(job->command),
                                   PATH_CAP - wcslen(job->command),
                                   L" --alpha premultiplied");
  }
  if (SUCCEEDED(formatted))
    formatted = StringCchPrintfW(job->verify_command, PATH_CAP,
                                 L"%ls verify %ls --threads %ls",
                                 quoted_executable, quoted_output, threads);
  free(quoted_executable);
  free(quoted_input);
  free(quoted_output);
  free(quoted_icc);
  if (FAILED(formatted)) {
    DeleteFileW(job->output);
    free(job);
    SetWindowTextW(g_status, L"The image path is too long.");
    return;
  }

  job->window = window;
  job->source_size = source_size;
  InterlockedExchange(&g_cancel_requested, 0);
  g_busy = 1;
  SetWindowTextW(g_status, L"Compressing and verifying...");
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
  wchar_t duration[48];
  wchar_t details[320];
  format_size(result->source_size, source_text, 64);
  format_size(result->output_size, output_text, 64);
  format_duration(result->milliseconds, duration, _countof(duration));
  double change = ((double)result->output_size - (double)result->source_size) *
                  100.0 / (double)result->source_size;
  /* container sizes show the saving someone will actually get */
  if (change < -0.005)
    StringCchPrintfW(headline, 96, L"%.1f%% smaller", -change);
  else if (change > 0.005)
    StringCchPrintfW(headline, 96, L"%.1f%% larger", change);
  else
    wcscpy_s(headline, 96, L"Same file size");
  StringCchPrintfW(
      details, 320,
      L"Original file  %ls\r\nQLIC file       %ls\r\nEncode time    %ls"
      L"\r\nVerification   passed",
      source_text, output_text, duration);
  set_result_heading(headline, 0);
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

static int write_view_png(const wchar_t *path) {
  if (!path || !g_view || !g_view->ok || g_view_frame >= g_view->frame_count)
    return 0;
  const uint8_t *rgba = g_view->frames[g_view_frame].rgba;
  size_t row = 0;
  size_t bytes = 0;
  if (!rgba || !multiply_size(g_view->width, 4u, &row) ||
      !multiply_size(row, g_view->height, &bytes) || row > UINT_MAX ||
      bytes > UINT_MAX)
    return 0;

  uint8_t *bgra = (uint8_t *)malloc(bytes);
  if (!bgra)
    return 0;
  for (size_t pixel = 0; pixel < bytes / 4u; ++pixel) {
    const uint8_t *source = rgba + pixel * 4u;
    uint8_t *destination = bgra + pixel * 4u;
    destination[0] = source[2];
    destination[1] = source[1];
    destination[2] = source[0];
    destination[3] = source[3];
  }

  HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
    free(bgra);
    return 0;
  }
  IWICImagingFactory *factory = NULL;
  IWICStream *stream = NULL;
  IWICBitmapEncoder *encoder = NULL;
  IWICBitmapFrameEncode *frame = NULL;
  IWICColorContext *color = NULL;
  IPropertyBag2 *bag = NULL;
  HRESULT result = CoCreateInstance(
      &CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
      &IID_IWICImagingFactory, (void **)&factory);
  if (SUCCEEDED(result))
    result = factory->lpVtbl->CreateStream(factory, &stream);
  if (SUCCEEDED(result))
    result = stream->lpVtbl->InitializeFromFilename(stream, path,
                                                     GENERIC_WRITE);
  if (SUCCEEDED(result))
    result = factory->lpVtbl->CreateEncoder(factory, &GUID_ContainerFormatPng,
                                             NULL, &encoder);
  if (SUCCEEDED(result))
    result = encoder->lpVtbl->Initialize(encoder, (IStream *)stream,
                                         WICBitmapEncoderNoCache);
  if (SUCCEEDED(result))
    result = encoder->lpVtbl->CreateNewFrame(encoder, &frame, &bag);
  if (SUCCEEDED(result))
    result = frame->lpVtbl->Initialize(frame, bag);
  if (SUCCEEDED(result) && g_view->icc && g_view->icc_size &&
      g_view->icc_size <= UINT_MAX && !g_view->hdr_preview) {
    result = factory->lpVtbl->CreateColorContext(factory, &color);
    if (SUCCEEDED(result))
      result = color->lpVtbl->InitializeFromMemory(
          color, g_view->icc, (UINT)g_view->icc_size);
    if (SUCCEEDED(result))
      result = frame->lpVtbl->SetColorContexts(frame, 1u, &color);
  }
  if (SUCCEEDED(result))
    result = frame->lpVtbl->SetSize(frame, g_view->width, g_view->height);
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (SUCCEEDED(result))
    result = frame->lpVtbl->SetPixelFormat(frame, &format);
  if (SUCCEEDED(result) &&
      !IsEqualGUID(&format, &GUID_WICPixelFormat32bppBGRA))
    result = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
  if (SUCCEEDED(result))
    result = frame->lpVtbl->WritePixels(frame, g_view->height, (UINT)row,
                                         (UINT)bytes, bgra);
  if (SUCCEEDED(result))
    result = frame->lpVtbl->Commit(frame);
  if (SUCCEEDED(result))
    result = encoder->lpVtbl->Commit(encoder);
  if (bag)
    bag->lpVtbl->Release(bag);
  if (color)
    color->lpVtbl->Release(color);
  if (frame)
    frame->lpVtbl->Release(frame);
  if (encoder)
    encoder->lpVtbl->Release(encoder);
  if (stream)
    stream->lpVtbl->Release(stream);
  if (factory)
    factory->lpVtbl->Release(factory);
  if (SUCCEEDED(initialized))
    CoUninitialize();
  free(bgra);
  if (FAILED(result))
    DeleteFileW(path);
  return SUCCEEDED(result);
}

static void save_view_png(HWND window) {
  if (!g_view || !g_view->ok)
    return;
  wchar_t *path = path_buffer();
  if (!path)
    return;
  replace_extension(g_view->path, L".png", path, PATH_CAP);
  OPENFILENAMEW dialog = {0};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window;
  dialog.lpstrFile = path;
  dialog.nMaxFile = PATH_CAP;
  dialog.lpstrFilter = L"PNG image\0*.png\0All files\0*.*\0";
  dialog.lpstrDefExt = L"png";
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
  if (GetSaveFileNameW(&dialog)) {
    SetWindowTextW(g_status, L"Saving PNG…");
    SetCursor(LoadCursorW(NULL, IDC_WAIT));
    int saved = write_view_png(path);
    SetCursor(LoadCursorW(NULL, IDC_ARROW));
    SetWindowTextW(g_status,
                   saved ? L"PNG saved." : L"Could not save the PNG file.");
  }
  free(path);
}

static HWND make_control(DWORD extended, const wchar_t *class_name,
                         const wchar_t *text, DWORD style, int id,
                         HWND parent) {
  HWND control = CreateWindowExW(
      extended, class_name, text,
      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, 0, 0, 0, 0, parent,
      (HMENU)(INT_PTR)id, (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE),
      NULL);
  if (control && g_font)
    SendMessageW(control, WM_SETFONT, (WPARAM)g_font, TRUE);
  return control;
}

static HWND make_choice_list(int id, HWND owner) {
  DWORD style = LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY |
                LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP;
  return make_control(0, L"LISTBOX", L"", style, id, owner);
}

static void position_choice_list(HWND field, HWND list, int item_count) {
  if (!field || !list || item_count <= 0)
    return;
  RECT field_rect;
  if (!GetWindowRect(field, &field_rect))
    return;
  int height = scaled(34) * item_count + scaled(4);
  int width = field_rect.right - field_rect.left;
  POINT position = {field_rect.left, field_rect.bottom + scaled(3)};
  ScreenToClient(GetParent(list), &position);
  SetWindowPos(list, HWND_TOP, position.x, position.y, width, height,
               SWP_NOACTIVATE);
}

static RECT page_bounds(const RECT *client) {
  int width = client->right - client->left - scaled(56);
  int maximum = scaled(980);
  if (width > maximum)
    width = maximum;
  if (width < 0)
    width = 0;
  int left = (client->left + client->right - width) / 2;
  RECT page = {left, client->top, left + width, client->bottom};
  return page;
}

static void layout_option_motion(const RECT *page, int redraw) {
  int page_width = page->right - page->left;
  int shift = options_motion(200);
  int output_top = scaled(286) + shift;
  int output_height = page->bottom - output_top;
  if (output_height < 0)
    output_height = 0;
  if (redraw) {
    SetWindowPos(g_output, g_save, page->left, output_top, page_width,
                 output_height, SWP_NOACTIVATE);
    SetWindowPos(g_options, g_output, page->left, scaled(242), page_width,
                 scaled(242), SWP_NOACTIVATE);
  } else {
    SetWindowPos(g_output, NULL, page->left, output_top, page_width,
                 output_height, SWP_NOACTIVATE | SWP_NOZORDER);
  }
}

static void layout(HWND window) {
  RECT client;
  GetClientRect(window, &client);
  RECT page = page_bounds(&client);
  int page_width = page.right - page.left;
  if (g_view_mode) {
    int margin = page.left;
    int content = page.left + scaled(16);
    int right = page.right - scaled(16);
    int card_width = page_width - scaled(32);
    MoveWindow(g_view_open, margin, scaled(112), scaled(112), scaled(40),
               TRUE);
    MoveWindow(g_view_encoder, page.right - scaled(112), scaled(112),
               scaled(112), scaled(40), TRUE);
    MoveWindow(g_status, margin + scaled(128), scaled(120),
               page_width - scaled(256), scaled(24), TRUE);
    MoveWindow(g_progress, margin, scaled(156), page_width, scaled(4),
               TRUE);
    MoveWindow(g_view_label, content, scaled(182), scaled(180), scaled(20),
               TRUE);
    MoveWindow(g_view_details, content, scaled(206), card_width - scaled(260),
               scaled(24), TRUE);
    MoveWindow(g_view_zoom_out, right - scaled(232), scaled(180), scaled(48),
               scaled(40), TRUE);
    MoveWindow(g_view_fit, right - scaled(176), scaled(180), scaled(112),
               scaled(40), TRUE);
    MoveWindow(g_view_zoom_in, right - scaled(56), scaled(180), scaled(48),
               scaled(40), TRUE);
    int footer = client.bottom - scaled(140);
    int save_left = right - scaled(128);
    int frame_left = (content + scaled(250) + save_left - scaled(264)) / 2;
    MoveWindow(g_view_pixel, content, footer - scaled(3), scaled(250),
               scaled(38), TRUE);
    MoveWindow(g_view_previous, frame_left, footer - scaled(6),
               scaled(40), scaled(40), TRUE);
    MoveWindow(g_view_play, frame_left + scaled(48), footer - scaled(6),
               scaled(68), scaled(40), TRUE);
    MoveWindow(g_view_next, frame_left + scaled(124), footer - scaled(6),
               scaled(40), scaled(40), TRUE);
    MoveWindow(g_view_frame_label, frame_left + scaled(172), footer,
               scaled(104), scaled(28), TRUE);
    MoveWindow(g_view_save_png, save_left, footer - scaled(6),
               scaled(128), scaled(40), TRUE);
    MoveWindow(g_view_note, content, client.bottom - scaled(94), card_width,
               scaled(48), TRUE);
    update_view_zoom_label(window);
    return;
  }
  int card_left = page.left + scaled(16);
  int browse_width = scaled(116);
  int card_width = page_width - scaled(32);
  MoveWindow(g_input_label, card_left, scaled(132), card_width, scaled(20),
             TRUE);
  MoveWindow(g_input, card_left + scaled(10), scaled(162),
             card_width - browse_width - scaled(36), scaled(32), TRUE);
  MoveWindow(g_browse, page.right - scaled(16) - browse_width, scaled(158),
             browse_width, scaled(40), TRUE);
  MoveWindow(g_input_details, card_left, scaled(204), card_width, scaled(22),
             TRUE);
  MoveWindow(g_status, 0, scaled(9), page_width - scaled(180), scaled(24), TRUE);
  MoveWindow(g_compress, page_width - scaled(164), scaled(4), scaled(164),
             scaled(42), TRUE);
  MoveWindow(g_progress, 0, scaled(50), page_width, scaled(5), TRUE);
  MoveWindow(g_result_label, scaled(16), scaled(94), card_width, scaled(20),
             TRUE);
  MoveWindow(g_result, scaled(16), scaled(124), card_width, scaled(52), TRUE);
  MoveWindow(g_result_details, scaled(16), scaled(184),
             card_width - scaled(148), scaled(82), TRUE);
  MoveWindow(g_threads_label, scaled(16), scaled(44), scaled(210), scaled(18),
             TRUE);
  MoveWindow(g_threads, scaled(16), scaled(64), scaled(210), scaled(40), TRUE);
  MoveWindow(g_color_profile_label, scaled(248), scaled(44),
             card_width - scaled(232), scaled(18), TRUE);
  MoveWindow(g_color_profile, scaled(248), scaled(64),
             card_width - scaled(232), scaled(40), TRUE);
  SendMessageW(g_threads_list, LB_SETITEMHEIGHT, 0, scaled(34));
  SendMessageW(g_color_profile_list, LB_SETITEMHEIGHT, 0, scaled(34));
  SendMessageW(g_alpha_list, LB_SETITEMHEIGHT, 0, scaled(34));
  int thread_items = (int)SendMessageW(g_threads_list, LB_GETCOUNT, 0, 0);
  int profile_items =
      (int)SendMessageW(g_color_profile_list, LB_GETCOUNT, 0, 0);
  int alpha_items = (int)SendMessageW(g_alpha_list, LB_GETCOUNT, 0, 0);
  position_choice_list(g_threads, g_threads_list, thread_items);
  position_choice_list(g_color_profile, g_color_profile_list, profile_items);
  MoveWindow(g_icc_label, scaled(16), scaled(108), card_width, scaled(18), TRUE);
  MoveWindow(g_icc_path, scaled(16), scaled(128),
             card_width - browse_width - scaled(16), scaled(32), TRUE);
  MoveWindow(g_icc_browse, page_width - scaled(16) - browse_width, scaled(124),
             browse_width, scaled(40), TRUE);
  MoveWindow(g_alpha_label, scaled(16), scaled(164), scaled(210), scaled(18),
              TRUE);
  MoveWindow(g_alpha, scaled(16), scaled(184), scaled(232), scaled(40), TRUE);
  position_choice_list(g_alpha, g_alpha_list, alpha_items);
  MoveWindow(g_options_note, scaled(266), scaled(166),
              card_width - scaled(250), scaled(46), TRUE);
  layout_option_motion(&page, 1);
  MoveWindow(g_save, page.right - scaled(16) - scaled(128),
             client.bottom - scaled(80), scaled(128), scaled(40), TRUE);
  if (g_open_choice == ID_THREADS_LIST)
    SetWindowPos(g_threads_list, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  else if (g_open_choice == ID_COLOR_PROFILE_LIST)
    SetWindowPos(g_color_profile_list, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  else if (g_open_choice == ID_ALPHA_LIST)
    SetWindowPos(g_alpha_list, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void redraw_options_state(HWND window) {
  RECT client;
  GetClientRect(window, &client);
  RECT page = page_bounds(&client);
  layout_option_motion(&page, 0);
  RedrawWindow(g_options, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

static void redraw_options_frame(HWND window) {
  RECT client;
  GetClientRect(window, &client);
  RECT page = page_bounds(&client);
  layout_option_motion(&page, 0);
  RECT header = {0, 0, page.right - page.left, scaled(44)};
  InvalidateRect(g_options, &header, FALSE);
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

static void round_outline(HDC dc, const RECT *rectangle, COLORREF color,
                          int width, int radius) {
  HPEN pen = CreatePen(PS_SOLID, width, color);
  HGDIOBJ old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
  HGDIOBJ old_pen = SelectObject(dc, pen);
  RoundRect(dc, rectangle->left, rectangle->top, rectangle->right,
            rectangle->bottom, radius, radius);
  SelectObject(dc, old_pen);
  SelectObject(dc, old_brush);
  DeleteObject(pen);
}

static RECT view_card(const RECT *client) {
  RECT page = page_bounds(client);
  RECT card = {page.left, scaled(164), page.right,
               client->bottom - scaled(24)};
  return card;
}

static RECT view_port(const RECT *client) {
  RECT page = page_bounds(client);
  RECT viewport = {page.left + scaled(16), scaled(234),
                   page.right - scaled(16),
                   client->bottom - scaled(156)};
  return viewport;
}

static double view_scale(const RECT *client) {
  if (!g_view || !g_view->ok || !g_view->width || !g_view->height)
    return 1.0;
  double value = g_view_zoom;
  if (g_view_fit_mode) {
    RECT viewport = view_port(client);
    double horizontal = (double)(viewport.right - viewport.left) /
                        (double)g_view->width;
    double vertical = (double)(viewport.bottom - viewport.top) /
                      (double)g_view->height;
    value = horizontal < vertical ? horizontal : vertical;
  }
  if (value < 1.0 / 4096.0)
    value = 1.0 / 4096.0;
  if (value > 64.0)
    value = 64.0;
  return value;
}

static void update_view_zoom_label(HWND window) {
  if (!g_view_fit)
    return;
  if (!window || !g_view || !g_view->ok) {
    SetWindowTextW(g_view_fit, L"Fit");
    return;
  }
  RECT client;
  GetClientRect(window, &client);
  unsigned percent = (unsigned)(view_scale(&client) * 100.0 + 0.5);
  if (!percent)
    percent = 1u;
  wchar_t text[32];
  StringCchPrintfW(text, _countof(text), L"Fit %u%%", percent);
  SetWindowTextW(g_view_fit, text);
}

static RECT view_destination(const RECT *client) {
  RECT viewport = view_port(client);
  RECT destination = viewport;
  if (!g_view || !g_view->ok || !g_view->width || !g_view->height)
    return destination;
  int available_width = viewport.right - viewport.left;
  int available_height = viewport.bottom - viewport.top;
  double scale = view_scale(client);
  double scaled_width = (double)g_view->width * scale + 0.5;
  double scaled_height = (double)g_view->height * scale + 0.5;
  int width = scaled_width > INT_MAX ? INT_MAX : (int)scaled_width;
  int height = scaled_height > INT_MAX ? INT_MAX : (int)scaled_height;
  if (width < 1)
    width = 1;
  if (height < 1)
    height = 1;
  if (g_view_fit_mode)
    g_view_pan_x = g_view_pan_y = 0;
  int maximum_x = width > available_width ? (width - available_width) / 2 : 0;
  int maximum_y =
      height > available_height ? (height - available_height) / 2 : 0;
  if (g_view_pan_x < -maximum_x)
    g_view_pan_x = -maximum_x;
  if (g_view_pan_x > maximum_x)
    g_view_pan_x = maximum_x;
  if (g_view_pan_y < -maximum_y)
    g_view_pan_y = -maximum_y;
  if (g_view_pan_y > maximum_y)
    g_view_pan_y = maximum_y;
  int center_x = (viewport.left + viewport.right) / 2 + g_view_pan_x;
  int center_y = (viewport.top + viewport.bottom) / 2 + g_view_pan_y;
  destination.left = center_x - width / 2;
  destination.top = center_y - height / 2;
  destination.right = destination.left + width;
  destination.bottom = destination.top + height;
  return destination;
}

static int rounded_double(double value) {
  return (int)(value < 0.0 ? value - 0.5 : value + 0.5);
}

static void zoom_view(HWND window, double factor, const POINT *anchor) {
  if (!window || !g_view || !g_view->ok || factor <= 0.0)
    return;
  RECT client;
  GetClientRect(window, &client);
  RECT old_destination = view_destination(&client);
  double old_scale = view_scale(&client);
  double source_x = (double)g_view->width / 2.0;
  double source_y = (double)g_view->height / 2.0;
  POINT point = {(client.left + client.right) / 2,
                 (client.top + client.bottom) / 2};
  if (anchor) {
    point = *anchor;
    source_x = ((double)point.x - old_destination.left) / old_scale;
    source_y = ((double)point.y - old_destination.top) / old_scale;
  }
  double next = old_scale * factor;
  if (next < 1.0 / 4096.0)
    next = 1.0 / 4096.0;
  if (next > 64.0)
    next = 64.0;
  g_view_fit_mode = 0;
  g_view_zoom = next;
  RECT viewport = view_port(&client);
  double width = (double)g_view->width * next;
  double height = (double)g_view->height * next;
  g_view_pan_x = rounded_double((double)point.x - source_x * next + width / 2.0 -
                                (viewport.left + viewport.right) / 2.0);
  g_view_pan_y = rounded_double((double)point.y - source_y * next + height / 2.0 -
                                (viewport.top + viewport.bottom) / 2.0);
  update_view_buttons();
  InvalidateRect(window, NULL, FALSE);
}

static void reset_pixel_text(void) {
  if (g_view_pixel)
    SetWindowTextW(g_view_pixel, L"Point at the image to inspect a pixel.");
}

static void inspect_view_pixel(HWND window, int x, int y) {
  if (!window || !g_view || !g_view->ok ||
      g_view_frame >= g_view->frame_count) {
    reset_pixel_text();
    return;
  }
  RECT client;
  GetClientRect(window, &client);
  RECT viewport = view_port(&client);
  RECT destination = view_destination(&client);
  POINT point = {x, y};
  if (!PtInRect(&viewport, point) || !PtInRect(&destination, point)) {
    reset_pixel_text();
    return;
  }
  double scale = view_scale(&client);
  uint32_t pixel_x = (uint32_t)(((double)x - destination.left) / scale);
  uint32_t pixel_y = (uint32_t)(((double)y - destination.top) / scale);
  if (pixel_x >= g_view->width || pixel_y >= g_view->height) {
    reset_pixel_text();
    return;
  }
  const uint8_t *rgba = g_view->frames[g_view_frame].rgba +
                        ((size_t)pixel_y * g_view->width + pixel_x) * 4u;
  wchar_t text[160];
  StringCchPrintfW(text, _countof(text),
                   L"x %u · y %u · RGBA %u, %u, %u, %u · #%02X%02X%02X%02X",
                   pixel_x, pixel_y, rgba[0], rgba[1], rgba[2], rgba[3],
                   rgba[0], rgba[1], rgba[2], rgba[3]);
  SetWindowTextW(g_view_pixel, text);
}

static UINT current_frame_delay(void) {
  if (!g_view || g_view_frame >= g_view->frame_count)
    return 100u;
  uint32_t delay = g_view->frames[g_view_frame].delay_ms;
  if (!delay)
    delay = 100u;
  return delay > INT_MAX ? INT_MAX : (UINT)delay;
}

static void show_view_frame(HWND window, uint32_t index) {
  if (!g_view || !g_view->frame_count || !select_view_frame(index))
    return;
  reset_pixel_text();
  update_view_buttons();
  if (g_view_playing)
    SetTimer(window, ID_VIEW_TIMER, current_frame_delay(), NULL);
  InvalidateRect(window, NULL, FALSE);
}

static void toggle_view_playback(HWND window) {
  if (!g_view || !g_view->ok || g_view->frame_count < 2u)
    return;
  if (g_view_playing) {
    stop_view_playback(window);
  } else {
    g_view_playing = 1;
    SetWindowTextW(g_view_play, L"Pause");
    SetTimer(window, ID_VIEW_TIMER, current_frame_delay(), NULL);
  }
  InvalidateRect(g_view_play, NULL, TRUE);
}

static void paint_checkerboard(HDC dc, const RECT *rectangle) {
  int tile = scaled(8);
  HBRUSH light = CreateSolidBrush(RGB(255, 255, 255));
  HBRUSH dark = CreateSolidBrush(RGB(217, 222, 219));
  for (int y = rectangle->top; y < rectangle->bottom; y += tile) {
    for (int x = rectangle->left; x < rectangle->right; x += tile) {
      RECT square = {x, y, x + tile, y + tile};
      if (square.right > rectangle->right)
        square.right = rectangle->right;
      if (square.bottom > rectangle->bottom)
        square.bottom = rectangle->bottom;
      int column = (x - rectangle->left) / tile;
      int row = (y - rectangle->top) / tile;
      FillRect(dc, &square, (column + row) & 1 ? dark : light);
    }
  }
  DeleteObject(dark);
  DeleteObject(light);
}

static void paint_viewer(HWND window, HDC dc, const RECT *client) {
  (void)window;
  RECT card = view_card(client);
  RECT shadow = card;
  OffsetRect(&shadow, 0, scaled(2));
  round_rect(dc, &shadow, RGB(218, 232, 222), RGB(218, 232, 222),
             scaled(18));
  round_rect(dc, &card, QLIC_COLOR_CARD, QLIC_COLOR_BORDER, scaled(18));
  RECT viewport = view_port(client);
  round_rect(dc, &viewport, QLIC_COLOR_VIEWER, QLIC_COLOR_VIEWER_BORDER,
              scaled(12));
  if (!g_view || !g_view->ok || !g_view_bitmap) {
    HGDIOBJ old_font = SelectObject(dc, g_result_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, QLIC_COLOR_TEXT);
    RECT message = viewport;
    InflateRect(&message, -scaled(32), -scaled(32));
    const wchar_t *text = g_view_loading
                              ? L"Decoding and verifying QLIC\u2026"
                              : g_view && g_view->message[0]
                                    ? g_view->message
                                    : L"Open a QLIC image to view it.";
    DrawTextW(dc, text, -1, &message,
              DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old_font);
    return;
  }

  RECT destination = view_destination(client);
  RECT clipped;
  if (!IntersectRect(&clipped, &destination, &viewport))
    return;
  int saved = SaveDC(dc);
  IntersectClipRect(dc, viewport.left, viewport.top, viewport.right,
                    viewport.bottom);
  if (g_view->has_alpha)
    paint_checkerboard(dc, &clipped);
  HDC memory = CreateCompatibleDC(dc);
  HGDIOBJ old_bitmap = SelectObject(memory, g_view_bitmap);
  int destination_width = destination.right - destination.left;
  int destination_height = destination.bottom - destination.top;
  if (g_view->has_alpha) {
    SetStretchBltMode(dc, COLORONCOLOR);
    SetStretchBltMode(memory, COLORONCOLOR);
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, destination.left, destination.top, destination_width,
               destination_height, memory, 0, 0, (int)g_view->width,
               (int)g_view->height, blend);
  } else {
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchBlt(dc, destination.left, destination.top, destination_width,
               destination_height, memory, 0, 0, (int)g_view->width,
               (int)g_view->height, SRCCOPY);
  }
  SelectObject(memory, old_bitmap);
  DeleteDC(memory);
  double scale = view_scale(client);
  if (scale >= 8.0) {
    int first_x = destination.left < viewport.left
                      ? (int)((viewport.left - destination.left) / scale)
                      : 0;
    int first_y = destination.top < viewport.top
                      ? (int)((viewport.top - destination.top) / scale)
                      : 0;
    int last_x = destination.right > viewport.right
                     ? (int)((viewport.right - destination.left) / scale) + 1
                     : (int)g_view->width;
    int last_y = destination.bottom > viewport.bottom
                     ? (int)((viewport.bottom - destination.top) / scale) + 1
                     : (int)g_view->height;
    if (last_x > (int)g_view->width)
      last_x = (int)g_view->width;
    if (last_y > (int)g_view->height)
      last_y = (int)g_view->height;
    HPEN grid = CreatePen(PS_SOLID, 1, RGB(109, 122, 113));
    HGDIOBJ old_grid = SelectObject(dc, grid);
    for (int x = first_x; x <= last_x; ++x) {
      int line = destination.left + rounded_double((double)x * scale);
      MoveToEx(dc, line, clipped.top, NULL);
      LineTo(dc, line, clipped.bottom);
    }
    for (int y = first_y; y <= last_y; ++y) {
      int line = destination.top + rounded_double((double)y * scale);
      MoveToEx(dc, clipped.left, line, NULL);
      LineTo(dc, clipped.right, line);
    }
    SelectObject(dc, old_grid);
    DeleteObject(grid);
  }
  HPEN border = CreatePen(PS_SOLID, 1, QLIC_COLOR_BORDER);
  HGDIOBJ old_pen = SelectObject(dc, border);
  HGDIOBJ old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
  Rectangle(dc, destination.left, destination.top, destination.right,
            destination.bottom);
  SelectObject(dc, old_brush);
  SelectObject(dc, old_pen);
  DeleteObject(border);
  RestoreDC(dc, saved);
  round_outline(dc, &viewport, QLIC_COLOR_VIEWER_BORDER, 1, scaled(12));
}

static void paint_window(HWND window) {
  PAINTSTRUCT paint;
  HDC dc = BeginPaint(window, &paint);
  RECT client;
  GetClientRect(window, &client);
  RECT page = page_bounds(&client);
  FillRect(dc, &client, g_background);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, QLIC_COLOR_TEXT);
  HGDIOBJ old_font = SelectObject(dc, g_title_font);
  RECT title = {page.left, scaled(22), page.right, scaled(62)};
  DrawTextW(dc, L"QLIC", -1, &title, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  SelectObject(dc, g_font);
  SetTextColor(dc, QLIC_COLOR_MUTED);
  RECT subtitle = {page.left, scaled(66), page.right, scaled(100)};
  DrawTextW(dc, L"Quick Lossless Image Codec", -1, &subtitle,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  SelectObject(dc, old_font);
  if (g_view_mode) {
    paint_viewer(window, dc, &client);
    EndPaint(window, &paint);
    return;
  }
  RECT input_card = {page.left, scaled(116), page.right, scaled(234)};
  RECT input_shadow = input_card;
  OffsetRect(&input_shadow, 0, scaled(2));
  round_rect(dc, &input_shadow, RGB(218, 232, 222), RGB(218, 232, 222),
             scaled(18));
  round_rect(dc, &input_card, QLIC_COLOR_CARD, QLIC_COLOR_BORDER, scaled(18));

  RECT input_field = {page.left + scaled(16), scaled(158),
                      page.right - scaled(148), scaled(198)};
  round_rect(dc, &input_field, QLIC_COLOR_FIELD, QLIC_COLOR_FIELD_BORDER,
             scaled(10));
  EndPaint(window, &paint);
}

static void draw_button(const DRAWITEMSTRUCT *item) {
  RECT outer = item->rcItem;
  int options = item->CtlID == ID_OPTIONS;
  int selector = item->CtlID == ID_THREADS || item->CtlID == ID_COLOR_PROFILE ||
                 item->CtlID == ID_ALPHA;
  int outside_card = item->CtlID == ID_COMPRESS || options ||
                      item->CtlID == ID_VIEW_OPEN ||
                      item->CtlID == ID_VIEW_ENCODER;
  HBRUSH parent = outside_card ? g_background : g_card;
  FillRect(item->hDC, &outer, parent);
  RECT button = outer;
  InflateRect(&button, -1, -1);
  RECT surface = button;

  int primary = item->CtlID == ID_COMPRESS;
  int options_panel = options && (g_options_open || g_options_position);
  unsigned option_motion = options ? options_ease() : 0u;
  int disabled = (item->itemState & ODS_DISABLED) != 0;
  int pressed = (item->itemState & ODS_SELECTED) != 0;
  int selector_open =
      (item->CtlID == ID_THREADS && g_open_choice == ID_THREADS_LIST) ||
      (item->CtlID == ID_COLOR_PROFILE &&
       g_open_choice == ID_COLOR_PROFILE_LIST) ||
      (item->CtlID == ID_ALPHA && g_open_choice == ID_ALPHA_LIST);
  ButtonAnimation *animation = button_animation(item->hwndItem);
  unsigned hover = animation && !disabled ? animation->hover : 0u;
  if (pressed)
    hover = 255u;
  COLORREF fill = primary
                      ? blend_color(QLIC_COLOR_ACCENT, QLIC_COLOR_ACCENT_DARK,
                                    hover)
                      : blend_color(QLIC_COLOR_BUTTON, RGB(255, 255, 255), hover);
  COLORREF border =
      primary ? fill
              : blend_color(QLIC_COLOR_BUTTON_BORDER, QLIC_COLOR_BUTTON_HOVER,
                            hover);
  COLORREF text = primary ? RGB(255, 255, 255) : QLIC_COLOR_SECONDARY;
  if (selector_open) {
    fill = QLIC_COLOR_ACCENT_PALE;
    border = QLIC_COLOR_BUTTON_HOVER;
  }
  if (options_panel) {
    fill = QLIC_COLOR_CARD;
    border = QLIC_COLOR_BORDER;
  }
  if (disabled) {
    fill = primary ? RGB(158, 186, 170) : RGB(250, 252, 251);
    border = primary ? fill : RGB(222, 233, 226);
    text = primary ? QLIC_COLOR_FIELD : RGB(150, 172, 161);
  }
  if (options_panel) {
    button.bottom -= scaled(2);
    RECT shadow = button;
    OffsetRect(&shadow, 0, scaled(2));
    round_rect(item->hDC, &shadow, RGB(218, 232, 222), RGB(218, 232, 222),
               scaled(16));
    round_rect(item->hDC, &button, fill, border, scaled(16));
    RECT icc_field = {scaled(16), scaled(124), outer.right - scaled(148),
                      scaled(164)};
    round_rect(item->hDC, &icc_field, QLIC_COLOR_FIELD,
               QLIC_COLOR_FIELD_BORDER, scaled(10));
    button.bottom = button.top + scaled(42);
    surface = button;
    if (pressed)
      OffsetRect(&button, 0, scaled(1));
  } else {
    if (options)
      button.bottom = button.top + scaled(42);
    if (pressed)
      OffsetRect(&button, 0, scaled(1));
    surface = button;
    round_rect(item->hDC, &button, fill, border, scaled(11));
  }

  wchar_t text_buffer[96];
  GetWindowTextW(item->hwndItem, text_buffer, 96);
  HGDIOBJ old_font = SelectObject(item->hDC, g_font);
  SetBkMode(item->hDC, TRANSPARENT);
  SetTextColor(item->hDC, text);
  UINT format = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
  if (options || selector) {
    int center_x =
        selector ? button.right - scaled(17) : button.left + scaled(12);
    int center_y = (button.top + button.bottom) / 2;
    POINT chevron[3];
    if (options) {
      POINT collapsed[3] = {{-2, -4}, {3, 0}, {-2, 4}};
      POINT expanded[3] = {{-4, -2}, {4, -2}, {0, 3}};
      for (size_t index = 0; index < _countof(chevron); ++index) {
        chevron[index].x =
            center_x + scaled(collapsed[index].x) +
            MulDiv(scaled(expanded[index].x - collapsed[index].x),
                   (int)option_motion, 255);
        chevron[index].y =
            center_y + scaled(collapsed[index].y) +
            MulDiv(scaled(expanded[index].y - collapsed[index].y),
                   (int)option_motion, 255);
      }
    } else if (selector_open) {
      chevron[0].x = center_x - scaled(4);
      chevron[0].y = center_y + scaled(2);
      chevron[1].x = center_x + scaled(4);
      chevron[1].y = center_y + scaled(2);
      chevron[2].x = center_x;
      chevron[2].y = center_y - scaled(3);
    } else {
      chevron[0].x = center_x - scaled(4);
      chevron[0].y = center_y - scaled(2);
      chevron[1].x = center_x + scaled(4);
      chevron[1].y = center_y - scaled(2);
      chevron[2].x = center_x;
      chevron[2].y = center_y + scaled(3);
    }
    HBRUSH chevron_brush = CreateSolidBrush(text);
    HPEN chevron_pen = CreatePen(PS_SOLID, 1, text);
    HGDIOBJ old_brush = SelectObject(item->hDC, chevron_brush);
    HGDIOBJ old_pen = SelectObject(item->hDC, chevron_pen);
    Polygon(item->hDC, chevron, 3);
    SelectObject(item->hDC, old_pen);
    SelectObject(item->hDC, old_brush);
    DeleteObject(chevron_pen);
    DeleteObject(chevron_brush);
    if (selector) {
      button.left += scaled(12);
      button.right -= scaled(34);
    } else {
      button.left += scaled(23);
    }
    format |= DT_LEFT;
  } else {
    format |= DT_CENTER;
  }
  DrawTextW(item->hDC, text_buffer, -1, &button, format);
  SelectObject(item->hDC, old_font);

  if ((item->itemState & ODS_FOCUS) && g_keyboard_navigation && !disabled) {
    RECT focus = surface;
    InflateRect(&focus, -scaled(2), -scaled(2));
    round_outline(item->hDC, &focus, primary ? QLIC_COLOR_FIELD
                                             : QLIC_COLOR_BUTTON_HOVER,
                   scaled(2), scaled(9));
  }
}

static void draw_choice_item(const DRAWITEMSTRUCT *item) {
  if (item->itemID == (UINT)-1)
    return;
  RECT row = item->rcItem;
  FillRect(item->hDC, &row, g_card);
  RECT surface = row;
  InflateRect(&surface, -scaled(3), -scaled(2));
  int highlighted = (item->itemState & ODS_SELECTED) != 0;
  round_rect(item->hDC, &surface,
             highlighted ? QLIC_COLOR_ACCENT_PALE : QLIC_COLOR_FIELD,
             highlighted ? QLIC_COLOR_ACCENT : QLIC_COLOR_BORDER, scaled(9));

  wchar_t text_buffer[96];
  if (SendMessageW(item->hwndItem, LB_GETTEXT, item->itemID,
                   (LPARAM)text_buffer) == LB_ERR)
    text_buffer[0] = 0;
  HGDIOBJ old_font = SelectObject(item->hDC, g_font);
  SetBkMode(item->hDC, TRANSPARENT);
  SetTextColor(item->hDC,
               highlighted ? QLIC_COLOR_SECONDARY : QLIC_COLOR_TEXT);
  RECT text_rect = surface;
  text_rect.left += scaled(34);
  text_rect.right -= scaled(10);
  DrawTextW(item->hDC, text_buffer, -1, &text_rect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  int center_x = surface.left + scaled(17);
  int center_y = (surface.top + surface.bottom) / 2;
  HBRUSH dot =
      CreateSolidBrush(highlighted ? QLIC_COLOR_ACCENT : QLIC_COLOR_BORDER);
  HPEN dot_pen = CreatePen(PS_SOLID, 1,
                           highlighted ? QLIC_COLOR_ACCENT : QLIC_COLOR_BORDER);
  HGDIOBJ old_brush = SelectObject(item->hDC, dot);
  HGDIOBJ old_pen = SelectObject(item->hDC, dot_pen);
  Ellipse(item->hDC, center_x - scaled(4), center_y - scaled(4),
          center_x + scaled(4), center_y + scaled(4));
  SelectObject(item->hDC, old_pen);
  SelectObject(item->hDC, old_brush);
  DeleteObject(dot_pen);
  DeleteObject(dot);
  SelectObject(item->hDC, old_font);

  if (item->itemState & ODS_FOCUS) {
    RECT focus = surface;
    InflateRect(&focus, -scaled(2), -scaled(2));
    round_outline(item->hDC, &focus, QLIC_COLOR_ACCENT_DARK, scaled(1),
                  scaled(8));
  }
}

static LRESULT CALLBACK output_proc(HWND window, UINT message, WPARAM wparam,
                                    LPARAM lparam, UINT_PTR subclass,
                                    DWORD_PTR data) {
  (void)data;
  if (message == WM_COMMAND || message == WM_DRAWITEM ||
      message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT ||
      message == WM_NOTIFY)
    return SendMessageW(GetParent(window), message, wparam, lparam);
  switch (message) {
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT: {
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(window, &paint);
    RECT client;
    GetClientRect(window, &client);
    FillRect(dc, &client, g_background);
    RECT card = {0, scaled(76), client.right, client.bottom - scaled(24)};
    if (card.bottom > card.top) {
      RECT shadow = card;
      OffsetRect(&shadow, 0, scaled(2));
      round_rect(dc, &shadow, RGB(218, 232, 222), RGB(218, 232, 222),
                 scaled(18));
      round_rect(dc, &card, QLIC_COLOR_CARD, QLIC_COLOR_BORDER, scaled(18));
    }
    EndPaint(window, &paint);
    return 0;
  }
  case WM_NCDESTROY:
    RemoveWindowSubclass(window, output_proc, subclass);
    break;
  default:
    break;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
  switch (message) {
  case WM_CREATE: {
    g_main_window = window;
    g_dpi = GetDpiForWindow(window);
    g_background = CreateSolidBrush(QLIC_COLOR_BACKGROUND);
    g_card = CreateSolidBrush(QLIC_COLOR_CARD);
    g_field = CreateSolidBrush(QLIC_COLOR_FIELD);
    replace_fonts();
    g_output = make_control(WS_EX_CONTROLPARENT, L"STATIC", L"",
                            WS_CLIPCHILDREN, ID_OUTPUT, window);
    if (!g_output || !SetWindowSubclass(g_output, output_proc, 1u, 0))
      return -1;
    g_input_label =
        make_control(0, L"STATIC", L"FILE", SS_LEFT, ID_INPUT_LABEL, window);
    g_result_label = make_control(0, L"STATIC", L"QLIC RESULT", SS_LEFT,
                                  ID_RESULT_LABEL, g_output);
    g_input =
        make_control(0, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP,
                     ID_INPUT, window);
    SendMessageW(g_input, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(scaled(4), scaled(4)));
    g_browse = make_control(0, L"BUTTON", L"Choose file",
                            BS_OWNERDRAW | WS_TABSTOP, ID_BROWSE, window);
    g_input_details = make_control(0, L"STATIC", L"Drop an image or QLIC file",
                                   SS_LEFT, ID_INPUT_DETAILS, window);
    g_options = make_control(WS_EX_CONTROLPARENT, L"BUTTON",
                             L"Advanced settings",
                             BS_OWNERDRAW | WS_TABSTOP | WS_CLIPCHILDREN,
                             ID_OPTIONS, window);
    g_threads_label = make_control(0, L"STATIC", L"CPU USE", SS_LEFT,
                                   ID_THREADS_LABEL, g_options);
    g_threads = make_control(0, L"BUTTON", L"All CPUs",
                             BS_OWNERDRAW | WS_TABSTOP, ID_THREADS, g_options);
    unsigned available = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (!available)
      available = 1u;
    g_thread_values[0] = 0u;
    g_thread_value_count = 1u;
    for (unsigned count = 1u; count <= available; count *= 2u) {
      if (g_thread_value_count <
          sizeof(g_thread_values) / sizeof(g_thread_values[0]))
        g_thread_values[g_thread_value_count++] = count;
      if (count > available / 2u)
        break;
    }
    g_color_profile_label =
        make_control(0, L"STATIC", L"COLOR METADATA", SS_LEFT,
                     ID_COLOR_PROFILE_LABEL, g_options);
    g_color_profile =
        make_control(0, L"BUTTON", g_profile_names[0],
                     BS_OWNERDRAW | WS_TABSTOP, ID_COLOR_PROFILE, g_options);
    g_icc_label = make_control(0, L"STATIC", L"ICC PROFILE", SS_LEFT,
                               ID_ICC_LABEL, g_options);
    g_icc_path =
        make_control(0, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP,
                     ID_ICC_PATH, g_options);
    g_icc_browse =
        make_control(0, L"BUTTON", L"Choose profile", BS_OWNERDRAW | WS_TABSTOP,
                     ID_ICC_BROWSE, g_options);
    g_alpha_label = make_control(0, L"STATIC", L"ALPHA ASSOCIATION", SS_LEFT,
                                 ID_ALPHA_LABEL, g_options);
    g_alpha = make_control(0, L"BUTTON", g_alpha_names[0],
                           BS_OWNERDRAW | WS_TABSTOP, ID_ALPHA, g_options);
    g_options_note = make_control(
        0, L"STATIC",
        L"Stores 8-24-bit integer samples and metadata. No tone mapping.",
        SS_LEFT, ID_OPTIONS_NOTE, g_options);
    g_compress = make_control(0, L"BUTTON", L"Compress",
                              BS_OWNERDRAW | WS_TABSTOP, ID_COMPRESS, g_output);
    g_status = make_control(0, L"STATIC",
                            L"Choose an image or open a QLIC file.", SS_LEFT,
                            ID_STATUS, g_output);
    g_progress = make_control(0, PROGRESS_CLASSW, L"", PBS_MARQUEE | PBS_SMOOTH,
                              ID_PROGRESS, g_output);
    g_result = make_control(0, L"STATIC", L"No result", SS_LEFT, ID_RESULT,
                            g_output);
    g_result_details = make_control(0, L"STATIC", L"", SS_LEFT,
                                    ID_RESULT_DETAILS, g_output);
    g_save = make_control(0, L"BUTTON", L"Save QLIC",
                          BS_OWNERDRAW | WS_TABSTOP, ID_SAVE, window);
    g_view_open = make_control(0, L"BUTTON", L"Choose file",
                               BS_OWNERDRAW | WS_TABSTOP, ID_VIEW_OPEN, window);
    g_view_zoom_out = make_control(0, L"BUTTON", L"−",
                                   BS_OWNERDRAW | WS_TABSTOP, ID_VIEW_ZOOM_OUT,
                                   window);
    g_view_fit = make_control(0, L"BUTTON", L"Fit",
                              BS_OWNERDRAW | WS_TABSTOP, ID_VIEW_FIT, window);
    g_view_zoom_in = make_control(0, L"BUTTON", L"+",
                                  BS_OWNERDRAW | WS_TABSTOP, ID_VIEW_ZOOM_IN,
                                  window);
    g_view_encoder = make_control(0, L"BUTTON", L"Encoder",
                                  BS_OWNERDRAW | WS_TABSTOP, ID_VIEW_ENCODER,
                                  window);
    g_view_details = make_control(0, L"STATIC", L"", SS_LEFT,
                                  ID_VIEW_DETAILS, window);
    g_view_previous = make_control(0, L"BUTTON", L"‹",
                                   BS_OWNERDRAW | WS_TABSTOP, ID_VIEW_PREVIOUS,
                                   window);
    g_view_play = make_control(0, L"BUTTON", L"Play",
                               BS_OWNERDRAW | WS_TABSTOP, ID_VIEW_PLAY, window);
    g_view_next = make_control(0, L"BUTTON", L"›",
                               BS_OWNERDRAW | WS_TABSTOP, ID_VIEW_NEXT, window);
    g_view_frame_label = make_control(0, L"STATIC", L"", SS_LEFT,
                                      ID_VIEW_FRAME, window);
    g_view_save_png = make_control(0, L"BUTTON", L"Save PNG",
                                   BS_OWNERDRAW | WS_TABSTOP, ID_VIEW_SAVE_PNG,
                                   window);
    g_view_pixel = make_control(
        0, L"STATIC", L"Point at the image to inspect a pixel.",
        SS_LEFT | SS_NOPREFIX, ID_VIEW_PIXEL, window);
    g_view_note = make_control(0, L"STATIC", L"", SS_LEFT | SS_NOPREFIX,
                               ID_VIEW_NOTE, window);
    g_view_label = make_control(0, L"STATIC", L"QLIC VIEWER", SS_LEFT,
                                ID_VIEW_LABEL, window);
    g_threads_list = make_choice_list(ID_THREADS_LIST, window);
    g_color_profile_list = make_choice_list(ID_COLOR_PROFILE_LIST, window);
    g_alpha_list = make_choice_list(ID_ALPHA_LIST, window);
    if (!g_output || !g_input_label || !g_result_label || !g_input || !g_browse ||
        !g_input_details || !g_compress || !g_status || !g_progress ||
        !g_result || !g_result_details || !g_save || !g_options ||
        !g_threads || !g_threads_list || !g_threads_label || !g_color_profile ||
        !g_color_profile_list || !g_color_profile_label || !g_icc_path ||
        !g_icc_label || !g_icc_browse || !g_alpha || !g_alpha_list ||
        !g_alpha_label || !g_options_note || !g_view_open ||
        !g_view_zoom_out || !g_view_fit || !g_view_zoom_in ||
        !g_view_encoder || !g_view_details || !g_view_previous ||
        !g_view_play || !g_view_next || !g_view_frame_label ||
        !g_view_save_png || !g_view_pixel || !g_view_note || !g_view_label ||
        !initialize_button_animations())
      return -1;
    SendMessageW(g_threads_list, LB_ADDSTRING, 0,
                 (LPARAM)L"All CPUs");
    for (size_t index = 1; index < g_thread_value_count; ++index) {
      wchar_t value[40];
      unsigned count = g_thread_values[index];
      StringCchPrintfW(value, 40, count == 1u ? L"1 CPU" : L"%u CPUs", count);
      SendMessageW(g_threads_list, LB_ADDSTRING, 0, (LPARAM)value);
    }
    for (size_t index = 0;
         index < sizeof(g_profile_names) / sizeof(g_profile_names[0]); ++index)
      SendMessageW(g_color_profile_list, LB_ADDSTRING, 0,
                   (LPARAM)g_profile_names[index]);
    for (size_t index = 0;
         index < sizeof(g_alpha_names) / sizeof(g_alpha_names[0]); ++index)
      SendMessageW(g_alpha_list, LB_ADDSTRING, 0, (LPARAM)g_alpha_names[index]);
    SendMessageW(g_threads_list, LB_SETCURSEL, 0, 0);
    SendMessageW(g_color_profile_list, LB_SETCURSEL, 0, 0);
    SendMessageW(g_alpha_list, LB_SETCURSEL, 0, 0);
    apply_fonts();
    SendMessageW(g_progress, PBM_SETBKCOLOR, 0,
                 QLIC_COLOR_PROGRESS_BACKGROUND);
    SendMessageW(g_progress, PBM_SETBARCOLOR, 0, QLIC_COLOR_PROGRESS);
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
    show_viewer_controls(0);
    EnableWindow(g_save, FALSE);
    DragAcceptFiles(window, TRUE);
    update_controls();
    update_options();
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
    if (((DRAWITEMSTRUCT *)lparam)->CtlType == ODT_LISTBOX) {
      draw_choice_item((DRAWITEMSTRUCT *)lparam);
      return TRUE;
    }
    break;
  case WM_CTLCOLORSTATIC: {
    int id = GetDlgCtrlID((HWND)lparam);
    int label = id == ID_INPUT_LABEL || id == ID_RESULT_LABEL ||
                id == ID_THREADS_LABEL || id == ID_COLOR_PROFILE_LABEL ||
                id == ID_ICC_LABEL || id == ID_ALPHA_LABEL ||
                id == ID_VIEW_LABEL;
    int disabled_label =
        (id == ID_ICC_LABEL && g_profile_selection != 6) ||
        (id == ID_ALPHA_LABEL && g_profile_selection == 0);
    COLORREF color = QLIC_COLOR_MUTED;
    if (id == ID_RESULT)
      color = g_result_placeholder ? QLIC_COLOR_PLACEHOLDER : QLIC_COLOR_TEXT;
    else if (id == ID_VIEW_PIXEL)
      color = QLIC_COLOR_SECONDARY;
    else if (id == ID_RESULT_DETAILS || id == ID_VIEW_DETAILS ||
             id == ID_VIEW_FRAME)
      color = QLIC_COLOR_DETAILS;
    else if (id == ID_INPUT_DETAILS || id == ID_OPTIONS_NOTE ||
             id == ID_VIEW_NOTE)
      color = QLIC_COLOR_NOTE;
    else if (id == ID_STATUS) {
      wchar_t text[96];
      GetWindowTextW((HWND)lparam, text, _countof(text));
      if (wcsstr(text, L"Lossy source"))
        color = QLIC_COLOR_WARNING;
    } else if (label)
      color = disabled_label ? QLIC_COLOR_DISABLED : QLIC_COLOR_LABEL;
    if (id == ID_ICC_PATH) {
      SetBkColor((HDC)wparam, QLIC_COLOR_FIELD);
      SetTextColor((HDC)wparam, QLIC_COLOR_MUTED);
      return (LRESULT)g_field;
    }
    SetBkMode((HDC)wparam, TRANSPARENT);
    SetTextColor((HDC)wparam, color);
    return (LRESULT)(id == ID_STATUS ? g_background : g_card);
  }
  case WM_CTLCOLOREDIT:
    SetBkColor((HDC)wparam, QLIC_COLOR_FIELD);
    SetTextColor((HDC)wparam, QLIC_COLOR_TEXT);
    return (LRESULT)g_field;
  case WM_CTLCOLORLISTBOX:
    SetBkColor((HDC)wparam, QLIC_COLOR_CARD);
    SetTextColor((HDC)wparam, QLIC_COLOR_TEXT);
    return (LRESULT)g_card;
  case WM_SIZE:
    layout(window);
    InvalidateRect(window, NULL, TRUE);
    return 0;
  case WM_LBUTTONDOWN:
    if (g_view_mode && g_view && g_view->ok) {
      RECT client;
      GetClientRect(window, &client);
      POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      RECT viewport = view_port(&client);
      if (PtInRect(&viewport, point)) {
        if (g_view_fit_mode) {
          g_view_zoom = view_scale(&client);
          g_view_fit_mode = 0;
        }
        g_view_dragging = 1;
        g_view_drag_x = point.x;
        g_view_drag_y = point.y;
        g_view_drag_pan_x = g_view_pan_x;
        g_view_drag_pan_y = g_view_pan_y;
        SetCapture(window);
        SetFocus(window);
        return 0;
      }
    }
    break;
  case WM_MOUSEMOVE:
    if (g_view_mode) {
      int x = GET_X_LPARAM(lparam);
      int y = GET_Y_LPARAM(lparam);
      if (g_view_dragging && (wparam & MK_LBUTTON)) {
        g_view_pan_x = g_view_drag_pan_x + x - g_view_drag_x;
        g_view_pan_y = g_view_drag_pan_y + y - g_view_drag_y;
        InvalidateRect(window, NULL, FALSE);
      } else if (g_view_dragging) {
        g_view_dragging = 0;
        ReleaseCapture();
      }
      inspect_view_pixel(window, x, y);
      return 0;
    }
    break;
  case WM_LBUTTONUP:
    if (g_view_dragging) {
      g_view_dragging = 0;
      ReleaseCapture();
      return 0;
    }
    break;
  case WM_CAPTURECHANGED:
    g_view_dragging = 0;
    break;
  case WM_MOUSEWHEEL:
    if (g_view_mode && g_view && g_view->ok) {
      POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(window, &point);
      RECT client;
      GetClientRect(window, &client);
      RECT viewport = view_port(&client);
      if (PtInRect(&viewport, point)) {
        zoom_view(window, GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 1.25 : 0.8,
                  &point);
        inspect_view_pixel(window, point.x, point.y);
        return 0;
      }
    }
    break;
  case WM_KEYDOWN:
    if (g_view_mode && g_view && g_view->ok) {
      if (wparam == VK_ADD || wparam == VK_OEM_PLUS) {
        zoom_view(window, 2.0, NULL);
        return 0;
      }
      if (wparam == VK_SUBTRACT || wparam == VK_OEM_MINUS) {
        zoom_view(window, 0.5, NULL);
        return 0;
      }
      if (wparam == VK_SPACE && g_view->frame_count > 1u) {
        toggle_view_playback(window);
        return 0;
      }
    }
    break;
  case WM_SETCURSOR:
    if (g_view_mode && LOWORD(lparam) == HTCLIENT && g_view && g_view->ok) {
      POINT point;
      GetCursorPos(&point);
      ScreenToClient(window, &point);
      RECT client;
      GetClientRect(window, &client);
      RECT viewport = view_port(&client);
      if (PtInRect(&viewport, point)) {
        SetCursor(LoadCursorW(NULL, g_view_dragging ? IDC_SIZEALL : IDC_HAND));
        return TRUE;
      }
    }
    break;
  case WM_ACTIVATEAPP:
    if (!wparam)
      close_choice_lists();
    break;
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
    update_options();
    InvalidateRect(window, NULL, TRUE);
    return 0;
  }
  case WM_GETMINMAXINFO: {
    MINMAXINFO *limits = (MINMAXINFO *)lparam;
    limits->ptMinTrackSize.x = scaled(720);
    limits->ptMinTrackSize.y = scaled(780);
    return 0;
  }
  case WM_COMMAND:
    switch (LOWORD(wparam)) {
    case ID_OPTIONS:
      if (!g_busy) {
        close_choice_lists();
        g_options_open = !g_options_open;
        unsigned target = g_options_open ? OPTIONS_FRAME_COUNT - 1u : 0u;
        g_options_animating = g_options_frame != target;
        update_options();
        if (g_options_animating) {
          if (!SetTimer(window, ID_OPTIONS_TIMER, 16u, NULL)) {
            g_options_frame = target;
            g_options_position = options_frame_position(target);
            g_options_animating = 0;
            update_options();
          }
        } else {
          KillTimer(window, ID_OPTIONS_TIMER);
          update_options();
        }
        redraw_options_state(window);
      }
      return 0;
    case ID_THREADS:
      if (HIWORD(wparam) == BN_CLICKED)
        toggle_choice_list(ID_THREADS_LIST, g_threads_list);
      return 0;
    case ID_THREADS_LIST:
      if (HIWORD(wparam) == LBN_SELCHANGE)
        apply_choice(g_threads, g_threads_list, &g_thread_selection);
      return 0;
    case ID_COLOR_PROFILE:
      if (HIWORD(wparam) == BN_CLICKED)
        toggle_choice_list(ID_COLOR_PROFILE_LIST, g_color_profile_list);
      return 0;
    case ID_COLOR_PROFILE_LIST:
      if (HIWORD(wparam) == LBN_SELCHANGE) {
        if (apply_choice(g_color_profile, g_color_profile_list,
                         &g_profile_selection))
          update_options();
      }
      return 0;
    case ID_ALPHA:
      if (HIWORD(wparam) == BN_CLICKED)
        toggle_choice_list(ID_ALPHA_LIST, g_alpha_list);
      return 0;
    case ID_ALPHA_LIST:
      if (HIWORD(wparam) == LBN_SELCHANGE)
        apply_choice(g_alpha, g_alpha_list, &g_alpha_selection);
      return 0;
    case ID_ICC_BROWSE:
      if (g_options_open && !g_options_animating) {
        close_choice_lists();
        choose_icc(window);
      }
      return 0;
    case ID_BROWSE:
      close_choice_lists();
      choose_input(window);
      return 0;
    case ID_COMPRESS:
      close_choice_lists();
      start_compression(window);
      return 0;
    case ID_SAVE:
      close_choice_lists();
      save_result(window);
      return 0;
    case ID_VIEW_OPEN:
      choose_input(window);
      return 0;
    case ID_VIEW_ZOOM_OUT:
      zoom_view(window, 0.5, NULL);
      return 0;
    case ID_VIEW_FIT:
      if (g_view && g_view->ok) {
        g_view_fit_mode = 1;
        g_view_pan_x = g_view_pan_y = 0;
        reset_pixel_text();
        update_view_buttons();
        InvalidateRect(window, NULL, TRUE);
      }
      return 0;
    case ID_VIEW_ZOOM_IN:
      zoom_view(window, 2.0, NULL);
      return 0;
    case ID_VIEW_ENCODER:
      leave_viewer_mode(window);
      return 0;
    case ID_VIEW_PREVIOUS:
      if (g_view && g_view->frame_count > 1u) {
        stop_view_playback(window);
        show_view_frame(window, g_view_frame ? g_view_frame - 1u
                                            : g_view->frame_count - 1u);
      }
      return 0;
    case ID_VIEW_PLAY:
      toggle_view_playback(window);
      return 0;
    case ID_VIEW_NEXT:
      if (g_view && g_view->frame_count > 1u) {
        stop_view_playback(window);
        show_view_frame(window, (g_view_frame + 1u) % g_view->frame_count);
      }
      return 0;
    case ID_VIEW_SAVE_PNG:
      save_view_png(window);
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
  case WM_TIMER:
    if (wparam == ID_OPTIONS_TIMER && g_options_animating) {
      unsigned target = g_options_open ? OPTIONS_FRAME_COUNT - 1u : 0u;
      if (g_options_frame < target)
        ++g_options_frame;
      else
        --g_options_frame;
      g_options_position = options_frame_position(g_options_frame);
      g_options_animating = g_options_frame != target;
      if (!g_options_animating) {
        KillTimer(window, ID_OPTIONS_TIMER);
        update_options();
      }
      redraw_options_frame(window);
      return 0;
    }
    if (wparam == ID_VIEW_TIMER && g_view_playing && g_view && g_view->ok &&
        g_view->frame_count > 1u) {
      uint32_t next = (g_view_frame + 1u) % g_view->frame_count;
      if (select_view_frame(next)) {
        reset_pixel_text();
        update_view_buttons();
        SetTimer(window, ID_VIEW_TIMER, current_frame_delay(), NULL);
        InvalidateRect(window, NULL, FALSE);
      } else {
        stop_view_playback(window);
      }
      return 0;
    }
    break;
  case WM_QLIC_VIEW_DONE: {
    ViewResult *result = (ViewResult *)lparam;
    g_view_loading = 0;
    SendMessageW(g_progress, PBM_SETMARQUEE, FALSE, 0);
    ShowWindow(g_progress, SW_HIDE);
    free_view_result(g_view);
    g_view = result;
    if (g_view && g_view->ok && select_view_frame(0u)) {
      wchar_t size_text[64];
      wchar_t details[256];
      wchar_t format[96];
      wchar_t note[1024];
      format_size(g_view->source_size, size_text, _countof(size_text));
      const wchar_t *channels = g_view->channels == 1u
                                    ? L"Gray"
                                    : g_view->channels == 3u ? L"RGB" : L"RGBA";
      const wchar_t *transfer =
          g_view->transfer_characteristics == QLIC_CICP_TRANSFER_PQ
              ? L"PQ "
              : g_view->transfer_characteristics == QLIC_CICP_TRANSFER_HLG
                    ? L"HLG "
                    : L"";
      StringCchPrintfW(format, _countof(format), L"%ls%ls %u-bit", transfer,
                       channels, g_view->bits_per_sample);
      const wchar_t *metadata = g_view->has_icc && g_view->has_cicp
                                    ? L" \u00b7 ICC + CICP"
                                    : g_view->has_icc ? L" \u00b7 ICC"
                                      : g_view->has_cicp ? L" \u00b7 CICP"
                                                         : L"";
      wchar_t frames[48] = L"";
      if (g_view->frame_count > 1u)
        StringCchPrintfW(frames, _countof(frames), L" \u00b7 %u frames",
                         g_view->frame_count);
      StringCchPrintfW(details, _countof(details),
                       L"%u \u00d7 %u \u00b7 %ls%ls%ls", g_view->width,
                       g_view->height, format, frames, metadata);
      SetWindowTextW(g_view_details, details);
      StringCchCopyW(note, _countof(note),
                     L"Wheel or +/\u2212 zooms. Drag to pan. ");
      if (g_view->hdr_preview)
        StringCchCatW(
            note, _countof(note),
            g_view->transfer_characteristics == QLIC_CICP_TRANSFER_HLG
                ? L"HLG samples are linearly scaled into an 8-bit SDR preview; "
                  L"no tone mapping is applied. PNG saves this preview."
                : L"PQ samples are linearly scaled into an 8-bit SDR preview; "
                  L"no tone mapping is applied. PNG saves this preview.");
      else if (g_view->bits_per_sample > 8u)
        StringCchCatW(note, _countof(note),
                      L"Wide samples are linearly scaled into an 8-bit preview. "
                      L"PNG saves this preview.");
      else if (g_view->frame_count > 1u)
        StringCchCatW(
            note, _countof(note),
            L"PNG saves the current frame with its exact decoded RGBA8 pixels.");
      else if (g_view->alpha_mode == QLIC_ALPHA_PREMULTIPLIED)
        StringCchCatW(note, _countof(note),
                      L"PNG saves the decoded image with straight RGBA8 pixels.");
      else
        StringCchCatW(
            note, _countof(note),
            L"PNG preserves exact decoded RGBA8 pixels, including RGB under zero alpha.");
      if (g_view->has_icc && !g_view->hdr_preview)
        StringCchCatW(note, _countof(note),
                      g_view->color_managed
                          ? L" ICC is color managed here and retained in PNG."
                          : L" ICC is retained in PNG; this preview is uncorrected.");
      else if (g_view->has_cicp && !g_view->hdr_preview)
        StringCchCatW(note, _countof(note),
                      L" PNG does not carry QLIC CICP metadata.");
      SetWindowTextW(g_view_note, note);
      wchar_t duration[48];
      format_duration(g_view->milliseconds, duration, _countof(duration));
      wchar_t status[160];
      StringCchPrintfW(status, _countof(status),
                       g_view->hdr_preview
                           ? L"Ready \u00b7 SDR preview \u00b7 %ls \u00b7 %ls"
                           : L"Ready \u00b7 %ls \u00b7 %ls",
                       size_text, duration);
      SetWindowTextW(g_status, status);
      reset_pixel_text();
    } else {
      destroy_view_bitmap();
      SetWindowTextW(g_view_details,
                     g_view && g_view->message[0]
                         ? g_view->message
                         : L"The QLIC image could not be decoded.");
      SetWindowTextW(g_status, L"Could not open QLIC");
      SetWindowTextW(g_view_note, L"");
      reset_pixel_text();
    }
    update_view_buttons();
    InvalidateRect(window, NULL, TRUE);
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
      SetWindowTextW(g_status, result->lossy_source
                                   ? QLIC_LOSSY_STATUS
                                   : L"Compression complete.");
      EnableWindow(g_save, TRUE);
    } else if (result && result->cancelled) {
      set_result_heading(L"Cancelled", 0);
      SetWindowTextW(g_result_details, L"");
      SetWindowTextW(g_status, L"Compression cancelled.");
    } else {
      set_result_heading(L"Compression failed", 0);
      SetWindowTextW(
          g_result_details,
          result && result->message[0]
              ? result->message
              : L"The QLIC command did not produce a verified file.");
      SetWindowTextW(g_status, L"Compression or verification failed.");
    }
    free(result);
    update_controls();
    return 0;
  }
  case WM_CLOSE:
    if (g_busy) {
      cancel_compression();
      return 0;
    }
    DestroyWindow(window);
    return 0;
  case WM_DESTROY:
    cancel_compression();
    remove_temporary_output();
    KillTimer(window, ID_VIEW_TIMER);
    KillTimer(window, ID_OPTIONS_TIMER);
    destroy_view_bitmap();
    free_view_result(g_view);
    g_view = NULL;
    if (g_font)
      DeleteObject(g_font);
    if (g_title_font)
      DeleteObject(g_title_font);
    if (g_result_font)
      DeleteObject(g_result_font);
    if (g_placeholder_font)
      DeleteObject(g_placeholder_font);
    if (g_label_font)
      DeleteObject(g_label_font);
    if (g_mono_font)
      DeleteObject(g_mono_font);
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

  HWND window = CreateWindowExW(
      WS_EX_COMPOSITED, window_class.lpszClassName, L"QLIC",
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
      MulDiv(780, (int)GetDpiForSystem(), 96),
      MulDiv(820, (int)GetDpiForSystem(), 96), NULL, NULL, instance, NULL);
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
    if (message.message == WM_KEYDOWN)
      g_keyboard_navigation = 1;
    else if (message.message == WM_LBUTTONDOWN ||
             message.message == WM_MBUTTONDOWN ||
             message.message == WM_RBUTTONDOWN)
      g_keyboard_navigation = 0;
    if (g_open_choice && message.message == WM_KEYDOWN &&
        message.wParam == VK_ESCAPE) {
      HWND field = g_open_choice == ID_THREADS_LIST         ? g_threads
                   : g_open_choice == ID_COLOR_PROFILE_LIST ? g_color_profile
                                                            : g_alpha;
      close_choice_lists();
      SetFocus(field);
      continue;
    }
    if (g_open_choice && message.message == WM_KEYDOWN &&
        message.wParam == VK_RETURN) {
      if (g_open_choice == ID_THREADS_LIST)
        apply_choice(g_threads, g_threads_list, &g_thread_selection);
      else if (g_open_choice == ID_COLOR_PROFILE_LIST) {
        if (apply_choice(g_color_profile, g_color_profile_list,
                         &g_profile_selection))
          update_options();
      } else
        apply_choice(g_alpha, g_alpha_list, &g_alpha_selection);
      continue;
    }
    if (IsDialogMessageW(window, &message))
      continue;
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return result < 0 ? 1 : (int)message.wParam;
}
