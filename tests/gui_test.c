#include <windows.h>
#include <strsafe.h>

#include <stdio.h>
#include <wchar.h>

enum {
  ID_BROWSE = 101,
  ID_COMPRESS = 102,
  ID_OPTIONS = 105,
  ID_THREADS = 106,
  ID_COLOR_PROFILE = 107,
  ID_ICC_BROWSE = 109,
  ID_ALPHA = 110,
  ID_THREADS_LIST = 111,
  ID_COLOR_PROFILE_LIST = 112,
  ID_ALPHA_LIST = 113,
  ID_VIEW_OPEN = 114,
  ID_VIEW_ZOOM_OUT = 115,
  ID_VIEW_FIT = 116,
  ID_VIEW_ZOOM_IN = 117,
  ID_VIEW_ENCODER = 118,
  ID_VIEW_DETAILS = 119,
  ID_VIEW_PREVIOUS = 120,
  ID_VIEW_PLAY = 121,
  ID_VIEW_NEXT = 122,
  ID_VIEW_FRAME = 123,
  ID_VIEW_SAVE_PNG = 124,
  ID_VIEW_PIXEL = 125,
  ID_VIEW_NOTE = 126,
  ID_VIEW_LABEL = 127,
  OLD_TITLE_CONTROL = 200,
  OLD_SUBTITLE_CONTROL = 201,
  ID_INPUT_LABEL = 202,
  ID_INPUT_DETAILS = 204,
  ID_STATUS = 205,
  ID_OUTPUT = 213
};

typedef struct {
  DWORD process_id;
  HWND window;
} WindowSearch;

static BOOL CALLBACK find_window(HWND window, LPARAM parameter) {
  WindowSearch *search = (WindowSearch *)parameter;
  DWORD process_id = 0;
  GetWindowThreadProcessId(window, &process_id);
  if (process_id != search->process_id)
    return TRUE;
  wchar_t class_name[32];
  if (!GetClassNameW(window, class_name, 32) ||
      wcscmp(class_name, L"QLIC") != 0)
    return TRUE;
  search->window = window;
  return FALSE;
}

static HWND wait_for_window(DWORD process_id) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    WindowSearch search = {process_id, NULL};
    EnumWindows(find_window, (LPARAM)&search);
    if (search.window)
      return search.window;
    Sleep(20);
  }
  return NULL;
}

static int has_visible_style(HWND window) {
  return window && (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0;
}

static int expect(int condition, const char *message) {
  if (condition)
    return 1;
  fprintf(stderr, "QLIC GUI test failed: %s\n", message);
  return 0;
}

static int select_item(HWND owner, HWND list, int list_id, int index) {
  if (SendMessageW(list, LB_SETCURSEL, (WPARAM)index, 0) == LB_ERR)
    return 0;
  SendMessageW(owner, WM_COMMAND, MAKEWPARAM(list_id, LBN_SELCHANGE),
               (LPARAM)list);
  return 1;
}

static int wait_for_text(HWND control, const wchar_t *needle) {
  for (int attempt = 0; attempt < 500; ++attempt) {
    wchar_t text[1024];
    GetWindowTextW(control, text, (int)(sizeof(text) / sizeof(text[0])));
    if (wcsstr(text, needle))
      return 1;
    Sleep(20);
  }
  return 0;
}

static int wait_for_top(HWND window, int target, int increasing) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    RECT rectangle;
    if (GetWindowRect(window, &rectangle) &&
        ((increasing && rectangle.top >= target) ||
         (!increasing && rectangle.top <= target)))
      return 1;
    Sleep(10);
  }
  return 0;
}

int wmain(int argc, wchar_t **argv) {
  if (argc != 2 && argc != 3 && argc != 4) {
    fprintf(stderr,
            "usage: qlic-gui-test <qlic-gui.exe> "
            "[lossy-image | image.qlic expected-size]\n");
    return 2;
  }

  STARTUPINFOW startup = {0};
  PROCESS_INFORMATION process = {0};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  wchar_t executable[32768];
  DWORD executable_length = GetFullPathNameW(argv[1], 32768, executable, NULL);
  if (!executable_length || executable_length >= 32768) {
    fprintf(stderr, "QLIC GUI test executable path is invalid\n");
    return 2;
  }
  wchar_t fixture[32768] = {0};
  if (argc >= 3) {
    DWORD fixture_length = GetFullPathNameW(argv[2], 32768, fixture, NULL);
    if (!fixture_length || fixture_length >= 32768) {
      fprintf(stderr, "QLIC GUI test fixture path is invalid\n");
      return 2;
    }
  }
  wchar_t command[32768] = {0};
  if (argc >= 3 && FAILED(StringCchPrintfW(command, 32768, L"\"%ls\" \"%ls\"",
                                           executable, fixture))) {
    fprintf(stderr, "QLIC GUI test command is too long\n");
    return 2;
  }
  if (!CreateProcessW(executable, argc >= 3 ? command : NULL, NULL, NULL, FALSE, 0,
                      NULL, NULL, &startup, &process)) {
    fprintf(stderr, "QLIC GUI test could not start the GUI: %lu\n",
            GetLastError());
    return 1;
  }
  CloseHandle(process.hThread);
  WaitForInputIdle(process.hProcess, 5000);

  int ok = 1;
  HWND window = wait_for_window(process.dwProcessId);
  ok &= expect(window != NULL, "main window was not created");
  if (window && argc == 4) {
    HWND open = GetDlgItem(window, ID_VIEW_OPEN);
    HWND zoom_out = GetDlgItem(window, ID_VIEW_ZOOM_OUT);
    HWND fit = GetDlgItem(window, ID_VIEW_FIT);
    HWND zoom_in = GetDlgItem(window, ID_VIEW_ZOOM_IN);
    HWND encoder = GetDlgItem(window, ID_VIEW_ENCODER);
    HWND details = GetDlgItem(window, ID_VIEW_DETAILS);
    HWND previous = GetDlgItem(window, ID_VIEW_PREVIOUS);
    HWND play = GetDlgItem(window, ID_VIEW_PLAY);
    HWND next = GetDlgItem(window, ID_VIEW_NEXT);
    HWND frame = GetDlgItem(window, ID_VIEW_FRAME);
    HWND save_png = GetDlgItem(window, ID_VIEW_SAVE_PNG);
    HWND pixel = GetDlgItem(window, ID_VIEW_PIXEL);
    HWND note = GetDlgItem(window, ID_VIEW_NOTE);
    HWND label = GetDlgItem(window, ID_VIEW_LABEL);
    ok &= expect(open && zoom_out && fit && zoom_in && encoder && details &&
                     previous && play && next && frame && save_png && pixel &&
                     note && label,
                  "a viewer control is missing");
    ok &= expect((GetWindowLongPtrW(open, GWL_STYLE) & WS_TABSTOP) &&
                     (GetWindowLongPtrW(zoom_out, GWL_STYLE) & WS_TABSTOP) &&
                     (GetWindowLongPtrW(fit, GWL_STYLE) & WS_TABSTOP) &&
                     (GetWindowLongPtrW(zoom_in, GWL_STYLE) & WS_TABSTOP) &&
                     (GetWindowLongPtrW(encoder, GWL_STYLE) & WS_TABSTOP) &&
                     (GetWindowLongPtrW(save_png, GWL_STYLE) & WS_TABSTOP),
                 "viewer buttons are missing keyboard navigation");
    ok &= expect(has_visible_style(open) && has_visible_style(zoom_out) &&
                     has_visible_style(fit) && has_visible_style(zoom_in) &&
                     has_visible_style(encoder) && has_visible_style(details) &&
                     has_visible_style(save_png) && has_visible_style(pixel) &&
                     has_visible_style(note) && has_visible_style(label),
                  "viewer controls are not visible");
    wchar_t text[1024];
    GetWindowTextW(open, text, 1024);
    ok &= expect(wcscmp(text, L"Choose file") == 0,
                 "viewer file chooser does not match the web interface");
    ok &= expect(!has_visible_style(GetDlgItem(window, ID_OPTIONS)),
                 "encoder controls remain visible in viewer mode");
    ok &= expect(wait_for_text(details, argv[3]),
                  "viewer did not finish decoding the expected dimensions");
    GetWindowTextW(details, text, 1024);
    int animated = wcsstr(text, L"frames") != NULL;
    ok &= expect(IsWindowEnabled(save_png), "PNG export did not enable");
    ok &= expect(wait_for_text(note, L"PNG"),
                  "viewer did not explain its PNG preview behavior");
    GetWindowTextW(label, text, 1024);
    ok &= expect(wcscmp(text, L"QLIC VIEWER") == 0,
                  "viewer label does not match the web interface");
    GetWindowTextW(pixel, text, 1024);
    ok &= expect(wcsstr(text, L"inspect a pixel") != NULL,
                  "pixel inspector did not initialize");
    ok &= expect(has_visible_style(previous) == animated &&
                     has_visible_style(play) == animated &&
                     has_visible_style(next) == animated &&
                     has_visible_style(frame) == animated,
                  "animation controls have the wrong visibility");
    wchar_t title[32768];
    GetWindowTextW(window, title, 32768);
    const wchar_t *name = wcsrchr(fixture, L'\\');
    name = name ? name + 1 : fixture;
    ok &= expect(wcsstr(title, name) != NULL && wcsstr(title, L" - QLIC") != NULL,
                 "viewer title does not preserve the original filename");
    SendMessageW(zoom_in, BM_CLICK, 0, 0);
    SendMessageW(zoom_out, BM_CLICK, 0, 0);
    SendMessageW(fit, BM_CLICK, 0, 0);
    GetWindowTextW(fit, text, 1024);
    ok &= expect(wcsncmp(text, L"Fit ", 4) == 0,
                  "fit control did not report the zoom level");
    wchar_t fit_text[96];
    wcscpy_s(fit_text, 96, text);
    RECT client;
    GetClientRect(window, &client);
    POINT wheel_point = {client.right / 2, client.bottom / 2};
    ClientToScreen(window, &wheel_point);
    SendMessageW(window, WM_MOUSEWHEEL,
                 MAKEWPARAM(0, (WORD)(SHORT)(-WHEEL_DELTA)),
                 MAKELPARAM((short)wheel_point.x, (short)wheel_point.y));
    GetWindowTextW(fit, text, 1024);
    ok &= expect(wcscmp(text, fit_text) != 0,
                 "mouse-wheel zoom did not update the zoom level");
    SendMessageW(fit, BM_CLICK, 0, 0);
    if (animated) {
      SendMessageW(play, BM_CLICK, 0, 0);
      GetWindowTextW(play, text, 1024);
      ok &= expect(wcscmp(text, L"Pause") == 0,
                    "animation playback did not start");
      SendMessageW(play, BM_CLICK, 0, 0);
      GetWindowTextW(play, text, 1024);
      ok &= expect(wcscmp(text, L"Play") == 0,
                    "animation playback did not pause");
    }
    PostMessageW(window, WM_CLOSE, 0, 0);
  } else if (window && argc == 3) {
    HWND output = GetDlgItem(window, ID_OUTPUT);
    HWND status = output ? GetDlgItem(output, ID_STATUS) : NULL;
    int warned = status && wait_for_text(status, L"Lossy source");
    if (status && !warned) {
      wchar_t text[256];
      GetWindowTextW(status, text, 256);
      fwprintf(stderr, L"QLIC GUI status: %ls\n", text);
    }
    ok &= expect(warned, "lossy input warning did not appear");
    PostMessageW(window, WM_CLOSE, 0, 0);
  } else if (window) {
    HWND output = GetDlgItem(window, ID_OUTPUT);
    HWND browse = GetDlgItem(window, ID_BROWSE);
    HWND compress = GetDlgItem(output, ID_COMPRESS);
    HWND options = GetDlgItem(window, ID_OPTIONS);
    HWND threads = GetDlgItem(options, ID_THREADS);
    HWND profiles = GetDlgItem(options, ID_COLOR_PROFILE);
    HWND icc_browse = GetDlgItem(options, ID_ICC_BROWSE);
    HWND alpha = GetDlgItem(options, ID_ALPHA);
    HWND thread_list = GetDlgItem(window, ID_THREADS_LIST);
    HWND profile_list = GetDlgItem(window, ID_COLOR_PROFILE_LIST);
    HWND alpha_list = GetDlgItem(window, ID_ALPHA_LIST);
    HWND input_label = GetDlgItem(window, ID_INPUT_LABEL);
    HWND input_details = GetDlgItem(window, ID_INPUT_DETAILS);
    HWND status = GetDlgItem(output, ID_STATUS);
    ok &= expect(output && input_label && input_details && browse && compress &&
                     options && threads && profiles && icc_browse && alpha &&
                     thread_list && profile_list && alpha_list && status,
                  "an option control is missing");
    ok &= expect((GetWindowLongPtrW(browse, GWL_STYLE) & WS_TABSTOP) &&
                     (GetWindowLongPtrW(options, GWL_STYLE) & WS_TABSTOP) &&
                     (GetWindowLongPtrW(threads, GWL_STYLE) & WS_TABSTOP) &&
                     (GetWindowLongPtrW(profiles, GWL_STYLE) & WS_TABSTOP) &&
                     (GetWindowLongPtrW(icc_browse, GWL_STYLE) & WS_TABSTOP) &&
                     (GetWindowLongPtrW(alpha, GWL_STYLE) & WS_TABSTOP),
                 "encoder controls are missing keyboard navigation");
    wchar_t text[96];
    GetWindowTextW(input_label, text, 96);
    ok &= expect(wcscmp(text, L"FILE") == 0,
                  "input label does not match the web interface");
    GetWindowTextW(browse, text, 96);
    ok &= expect(wcscmp(text, L"Choose file") == 0,
                  "file chooser does not match the web interface");
    GetWindowTextW(input_details, text, 96);
    ok &= expect(wcscmp(text, L"Drop an image or QLIC file") == 0,
                 "drop hint does not match the web interface");
    GetWindowTextW(options, text, 96);
    ok &= expect(wcscmp(text, L"Advanced settings") == 0,
                  "advanced settings wording changed");
    ok &= expect((GetWindowLongPtrW(window, GWL_STYLE) & WS_CLIPCHILDREN) != 0,
                 "main window does not clip child painting");
    ok &= expect((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_COMPOSITED) != 0,
                 "main window does not composite child painting");
    LONG_PTR window_style = GetWindowLongPtrW(window, GWL_STYLE);
    LONG_PTR desktop_controls =
        WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    ok &= expect((window_style & desktop_controls) == desktop_controls,
                 "resize, minimize, or maximize window controls are missing");
    ok &= expect(!GetDlgItem(window, OLD_TITLE_CONTROL) &&
                     !GetDlgItem(window, OLD_SUBTITLE_CONTROL),
                 "centered header is not painted on the main surface");

    RECT original_window;
    RECT narrow_profile;
    RECT wide_profile;
    GetWindowRect(window, &original_window);
    GetWindowRect(profiles, &narrow_profile);
    int original_width = original_window.right - original_window.left;
    int original_height = original_window.bottom - original_window.top;
    SetWindowPos(window, NULL, original_window.left, original_window.top,
                 original_width + 800, original_height + 200,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    RedrawWindow(window, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    GetWindowRect(profiles, &wide_profile);
    ok &= expect(wide_profile.right - wide_profile.left >
                     narrow_profile.right - narrow_profile.left,
                 "maximized layout did not resize the controls");
    SetWindowPos(window, NULL, original_window.left, original_window.top,
                 original_width, original_height,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    RedrawWindow(window, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);

    RECT options_rect;
    RECT closed_output;
    GetWindowRect(options, &options_rect);
    GetWindowRect(output, &closed_output);
    SendMessageW(options, BM_CLICK, 0, 0);
    ok &= expect(wait_for_top(output, options_rect.bottom, 1),
                 "advanced settings animation did not finish opening");
    ok &= expect(has_visible_style(threads) && has_visible_style(profiles) &&
                     has_visible_style(alpha),
                 "options did not open");
    ok &= expect(!IsWindowEnabled(alpha),
                   "alpha setting is active without color metadata");
    RECT alpha_rect;
    RECT compress_rect;
    RECT status_rect;
    GetWindowRect(alpha, &alpha_rect);
    GetWindowRect(compress, &compress_rect);
    GetWindowRect(status, &status_rect);
    GetWindowRect(options, &options_rect);
    ok &= expect(options_rect.bottom - options_rect.top >
                     (closed_output.top - options_rect.top) * 4,
                 "advanced settings did not expand as one surface");
    ok &= expect(compress_rect.top > options_rect.bottom,
                 "compress button is too close to advanced settings");
    ok &= expect(status_rect.top - alpha_rect.bottom >= 16,
                  "alpha setting has no bottom padding");

    SendMessageW(threads, BM_CLICK, 0, 0);
    ok &= expect(has_visible_style(thread_list), "CPU choices did not open");
    ok &= expect(SendMessageW(thread_list, LB_GETCOUNT, 0, 0) >= 2,
                 "CPU choices are incomplete");
    ok &= expect(GetWindow(thread_list, GW_HWNDPREV) == NULL,
                 "CPU choices are not topmost");
    ok &= expect(select_item(window, thread_list, ID_THREADS_LIST, 1),
                 "CPU choice could not be selected");
    GetWindowTextW(threads, text, 96);
    ok &= expect(wcscmp(text, L"1 CPU") == 0,
                 "CPU choice did not update its label");
    ok &= expect(!has_visible_style(thread_list),
                 "CPU choices did not close after selection");

    SendMessageW(profiles, BM_CLICK, 0, 0);
    ok &= expect(has_visible_style(profile_list),
                 "color-profile choices did not open");
    ok &= expect(SendMessageW(profile_list, LB_GETCOUNT, 0, 0) == 7,
                 "color-profile choices are incomplete");
    ok &= expect(GetWindow(profile_list, GW_HWNDPREV) == NULL,
                 "color-profile choices are not topmost");
    ok &= expect(select_item(window, profile_list, ID_COLOR_PROFILE_LIST, 1),
                 "color profile could not be selected");
    ok &= expect(IsWindowEnabled(alpha),
                 "alpha setting did not enable with color metadata");

    SendMessageW(profiles, BM_CLICK, 0, 0);
    ok &= expect(select_item(window, profile_list, ID_COLOR_PROFILE_LIST, 6),
                 "ICC profile choice could not be selected");
    ok &=
        expect(IsWindowEnabled(icc_browse), "ICC file chooser did not enable");

    SendMessageW(alpha, BM_CLICK, 0, 0);
    ok &= expect(has_visible_style(alpha_list), "alpha choices did not open");
    ok &= expect(SendMessageW(alpha_list, LB_GETCOUNT, 0, 0) == 2,
                 "alpha choices are incomplete");
    ok &= expect(GetWindow(alpha_list, GW_HWNDPREV) == NULL,
                 "alpha choices are not topmost");
    ok &= expect(select_item(window, alpha_list, ID_ALPHA_LIST, 1),
                 "alpha choice could not be selected");
    GetWindowTextW(alpha, text, 96);
    ok &= expect(wcscmp(text, L"Premultiplied alpha") == 0,
                 "alpha choice did not update its label");

    SendMessageW(options, BM_CLICK, 0, 0);
    ok &= expect(wait_for_top(output, closed_output.top, 0),
                 "advanced settings animation did not finish closing");
    ok &= expect(
        !IsWindowEnabled(threads) && !has_visible_style(thread_list) &&
            !has_visible_style(profile_list) && !has_visible_style(alpha_list),
        "options did not close cleanly");
    for (int pass = 0; pass < 10; ++pass) {
      SendMessageW(options, BM_CLICK, 0, 0);
      UpdateWindow(window);
    }
    ok &= expect(wait_for_top(output, closed_output.top, 0),
                 "rapid option toggles did not settle");
    ok &= expect(!IsWindowEnabled(threads) && !IsWindowEnabled(profiles) &&
                     !IsWindowEnabled(alpha),
                 "rapid option toggles left a partial state");
    PostMessageW(window, WM_CLOSE, 0, 0);
  }

  if (WaitForSingleObject(process.hProcess, 3000) == WAIT_TIMEOUT) {
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, 3000);
    ok &= expect(0, "GUI did not exit cleanly");
  }
  CloseHandle(process.hProcess);
  if (!ok)
    return 1;
  puts(argc == 4 ? "QLIC GUI viewer test passed"
                 : argc == 3 ? "QLIC GUI lossy warning test passed"
                             : "QLIC GUI resize and selector test passed");
  return 0;
}
