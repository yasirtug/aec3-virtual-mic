#include "ui_level_meter.h"

#include <cmath>
#include <cwchar>
#include <iterator>

namespace {

constexpr float kFloorDbfs = -60.0f;
constexpr float kWarnDbfs = -12.0f;
constexpr float kHotDbfs = -3.0f;
// Displayed level in tenths of a dB; kSilent means "below the floor".
constexpr LONG_PTR kSilent = -10000;
constexpr int kLevelSlot = 0;
constexpr int kFontSlot = sizeof(LONG_PTR);

LONG_PTR QuantizeLevel(float dbfs) {
  if (!(dbfs > kFloorDbfs)) {  // Also catches NaN.
    return kSilent;
  }
  if (dbfs > 0.0f) {
    dbfs = 0.0f;
  }
  return static_cast<LONG_PTR>(std::lround(dbfs * 10.0f));
}

// Maps a dBFS value to an x coordinate within [left, right].
int LevelToX(float dbfs, int left, int right) {
  const float fraction = (dbfs - kFloorDbfs) / (0.0f - kFloorDbfs);
  const float clamped = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
  return left + static_cast<int>(std::lround(clamped * (right - left)));
}

void FillSolid(HDC dc, const RECT& rect, COLORREF color) {
  if (rect.right <= rect.left || rect.bottom <= rect.top) {
    return;
  }
  SetBkColor(dc, color);
  ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &rect, nullptr, 0, nullptr);
}

void Paint(HWND window, HDC target) {
  RECT client = {};
  GetClientRect(window, &client);
  const int width = client.right - client.left;
  const int height = client.bottom - client.top;
  if (width <= 0 || height <= 0) {
    return;
  }

  HDC dc = CreateCompatibleDC(target);
  HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
  HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
  auto font = reinterpret_cast<HFONT>(GetWindowLongPtrW(window, kFontSlot));
  HGDIOBJ old_font =
      SelectObject(dc, font != nullptr ? static_cast<HGDIOBJ>(font)
                                       : GetStockObject(DEFAULT_GUI_FONT));

  const bool high_contrast = [] {
    HIGHCONTRASTW contrast = {sizeof(contrast)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast),
                                 &contrast, 0) &&
           (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
  }();

  FillSolid(dc, client, GetSysColor(COLOR_WINDOW));

  // Numeric label area on the right, wide enough for "-60.0 dB".
  SIZE label_size = {};
  GetTextExtentPoint32W(dc, L"-00.0 dB", 8, &label_size);
  const int gap = label_size.cy / 2;
  RECT label = client;
  label.left = client.right - label_size.cx;
  RECT bar = client;
  bar.right = label.left - gap;
  const int bar_inset = height / 5;
  bar.top += bar_inset;
  bar.bottom -= bar_inset;

  const LONG_PTR level = GetWindowLongPtrW(window, kLevelSlot);
  if (bar.right > bar.left + 2) {
    const COLORREF border =
        high_contrast ? GetSysColor(COLOR_WINDOWTEXT) : RGB(190, 190, 190);
    const COLORREF track =
        high_contrast ? GetSysColor(COLOR_WINDOW) : RGB(234, 234, 234);
    FillSolid(dc, bar, border);
    RECT inner = {bar.left + 1, bar.top + 1, bar.right - 1, bar.bottom - 1};
    FillSolid(dc, inner, track);

    if (level != kSilent) {
      const float dbfs = static_cast<float>(level) / 10.0f;
      const int level_x = LevelToX(dbfs, inner.left, inner.right);
      const int warn_x = LevelToX(kWarnDbfs, inner.left, inner.right);
      const int hot_x = LevelToX(kHotDbfs, inner.left, inner.right);
      const COLORREF green =
          high_contrast ? GetSysColor(COLOR_HIGHLIGHT) : RGB(16, 137, 62);
      const COLORREF amber =
          high_contrast ? GetSysColor(COLOR_HIGHLIGHT) : RGB(232, 160, 0);
      const COLORREF red =
          high_contrast ? GetSysColor(COLOR_HIGHLIGHT) : RGB(209, 52, 56);
      RECT segment = inner;
      segment.right = level_x < warn_x ? level_x : warn_x;
      FillSolid(dc, segment, green);
      if (level_x > warn_x) {
        segment.left = warn_x;
        segment.right = level_x < hot_x ? level_x : hot_x;
        FillSolid(dc, segment, amber);
      }
      if (level_x > hot_x) {
        segment.left = hot_x;
        segment.right = level_x;
        FillSolid(dc, segment, red);
      }
    }
  }

  wchar_t text[32] = L"\u2014";  // Em dash for silence.
  if (level != kSilent) {
    swprintf_s(text, std::size(text), L"%.1f dB",
               static_cast<double>(level) / 10.0);
  }
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
  DrawTextW(dc, text, -1, &label,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
  SelectObject(dc, old_font);
  SelectObject(dc, old_bitmap);
  DeleteObject(bitmap);
  DeleteDC(dc);
}

LRESULT CALLBACK LevelMeterProcedure(HWND window,
                                     UINT message,
                                     WPARAM wparam,
                                     LPARAM lparam) {
  switch (message) {
    case WM_NCCREATE:
      SetWindowLongPtrW(window, kLevelSlot, kSilent);
      SetWindowLongPtrW(window, kFontSlot, 0);
      break;
    case WM_SETFONT:
      SetWindowLongPtrW(window, kFontSlot, static_cast<LONG_PTR>(wparam));
      if (LOWORD(lparam) != 0) {
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    case WM_GETFONT:
      return GetWindowLongPtrW(window, kFontSlot);
    case WM_ERASEBKGND:
      return 1;
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT paint = {};
      HDC dc = BeginPaint(window, &paint);
      Paint(window, dc);
      EndPaint(window, &paint);
      return 0;
    }
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

bool RegisterLevelMeterClass(HINSTANCE instance) {
  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = LevelMeterProcedure;
  window_class.cbWndExtra = 2 * sizeof(LONG_PTR);
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kLevelMeterClass;
  return RegisterClassExW(&window_class) != 0;
}

void SetLevelMeterValue(HWND meter, float dbfs) {
  const LONG_PTR level = QuantizeLevel(dbfs);
  if (GetWindowLongPtrW(meter, kLevelSlot) == level) {
    return;
  }
  SetWindowLongPtrW(meter, kLevelSlot, level);
  InvalidateRect(meter, nullptr, FALSE);
}
