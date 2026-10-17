#include "ui_mute.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kMutedEventName[] = L"Local\\AEC3VirtualMic.Muted";
constexpr wchar_t kMuteChangedEventName[] = L"Local\\AEC3VirtualMic.MuteChanged";

// Draws color (r, g, b) with coverage `alpha` over a straight-alpha BGRA
// pixel.
void Blend(uint8_t* pixel, uint8_t r, uint8_t g, uint8_t b, float alpha) {
  if (alpha <= 0.0f) {
    return;
  }
  alpha = std::min(alpha, 1.0f);
  const float below = pixel[3] / 255.0f * (1.0f - alpha);
  const float out_alpha = alpha + below;
  const auto mix = [&](uint8_t source, uint8_t destination) {
    return static_cast<uint8_t>(
        std::lround((source * alpha + destination * below) / out_alpha));
  };
  pixel[0] = mix(b, pixel[0]);
  pixel[1] = mix(g, pixel[1]);
  pixel[2] = mix(r, pixel[2]);
  pixel[3] = static_cast<uint8_t>(std::lround(out_alpha * 255.0f));
}

// Antialiased coverage of a disk of `radius` at distance `distance`.
float DiskCoverage(float distance, float radius) {
  return std::clamp(radius - distance + 0.5f, 0.0f, 1.0f);
}

bool ReadBitmap32(HDC dc, HBITMAP bitmap, int width, int height, void* bits) {
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // Top-down.
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  return GetDIBits(dc, bitmap, 0, static_cast<UINT>(height), bits, &info,
                   DIB_RGB_COLORS) == height;
}

}  // namespace

MuteStatePublisher::MuteStatePublisher()
    : muted_(CreateEventW(nullptr, TRUE, FALSE, kMutedEventName)),
      changed_(CreateEventW(nullptr, FALSE, FALSE, kMuteChangedEventName)) {
  // A reader may still hold the objects from an earlier run; start unmuted.
  Publish(false);
}

MuteStatePublisher::~MuteStatePublisher() {
  Publish(false);  // Readers holding the handles must not stay "muted".
  if (muted_ != nullptr) {
    CloseHandle(muted_);
  }
  if (changed_ != nullptr) {
    CloseHandle(changed_);
  }
}

void MuteStatePublisher::Publish(bool muted) {
  if (muted_ != nullptr) {
    if (muted) {
      SetEvent(muted_);
    } else {
      ResetEvent(muted_);
    }
  }
  if (changed_ != nullptr) {
    SetEvent(changed_);
  }
}

std::wstring HotkeyName(WORD hotkey) {
  const UINT key = LOBYTE(hotkey);
  const BYTE modifiers = HIBYTE(hotkey);
  if (key == 0) {
    return {};
  }
  std::wstring name;
  if ((modifiers & HOTKEYF_CONTROL) != 0) {
    name += L"Ctrl + ";
  }
  if ((modifiers & HOTKEYF_ALT) != 0) {
    name += L"Alt + ";
  }
  if ((modifiers & HOTKEYF_SHIFT) != 0) {
    name += L"Shift + ";
  }
  LONG key_lparam = static_cast<LONG>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC))
                    << 16;
  if ((modifiers & HOTKEYF_EXT) != 0) {
    key_lparam |= 1 << 24;
  }
  wchar_t key_name[64] = {};
  if (GetKeyNameTextW(key_lparam, key_name,
                      static_cast<int>(std::size(key_name))) > 0) {
    name += key_name;
  } else {
    name += L"Key " + std::to_wstring(key);
  }
  return name;
}

UINT HotkeyRegisterModifiers(WORD hotkey) {
  const BYTE modifiers = HIBYTE(hotkey);
  UINT result = MOD_NOREPEAT;
  if ((modifiers & HOTKEYF_CONTROL) != 0) {
    result |= MOD_CONTROL;
  }
  if ((modifiers & HOTKEYF_ALT) != 0) {
    result |= MOD_ALT;
  }
  if ((modifiers & HOTKEYF_SHIFT) != 0) {
    result |= MOD_SHIFT;
  }
  return result;
}

HICON CreateMutedIcon(HICON base) {
  ICONINFO info = {};
  if (!GetIconInfo(base, &info)) {
    return nullptr;
  }
  HICON result = nullptr;
  BITMAP bitmap = {};
  if (info.hbmColor != nullptr &&
      GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) != 0) {
    const int width = bitmap.bmWidth;
    const int height = bitmap.bmHeight;
    const size_t count = static_cast<size_t>(width) * height;
    std::vector<uint32_t> pixels(count);
    HDC dc = GetDC(nullptr);
    bool ok = ReadBitmap32(dc, info.hbmColor, width, height, pixels.data());
    // Icons without an alpha channel: take transparency from the mask.
    if (ok && std::all_of(pixels.begin(), pixels.end(),
                          [](uint32_t p) { return (p >> 24) == 0; })) {
      std::vector<uint32_t> mask(count);
      ok = ReadBitmap32(dc, info.hbmMask, width, height, mask.data());
      for (size_t i = 0; ok && i < count; ++i) {
        const bool transparent = (mask[i] & 0x00FFFFFF) != 0;
        pixels[i] = (pixels[i] & 0x00FFFFFF) | (transparent ? 0u : 0xFF000000u);
      }
    }
    ReleaseDC(nullptr, dc);

    if (ok) {
      // Red disk with a white bar and a thin white rim for contrast on any
      // taskbar color.
      const float radius = width * 0.3f;
      const float center_x = width - radius - 0.5f;
      const float center_y = height - radius - 0.5f;
      const float bar_half_width = radius * 0.62f;
      const float bar_half_height = std::max(radius * 0.2f, 0.75f);
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          const float dx = x + 0.5f - center_x;
          const float dy = y + 0.5f - center_y;
          const float distance = std::sqrt(dx * dx + dy * dy);
          if (distance > radius + 1.5f) {
            continue;
          }
          auto* pixel = reinterpret_cast<uint8_t*>(
              &pixels[static_cast<size_t>(y) * width + x]);
          Blend(pixel, 255, 255, 255, DiskCoverage(distance, radius + 1.0f));
          Blend(pixel, 196, 43, 28, DiskCoverage(distance, radius));
          const float bar =
              std::clamp(bar_half_width - std::fabs(dx) + 0.5f, 0.0f, 1.0f) *
              std::clamp(bar_half_height - std::fabs(dy) + 0.5f, 0.0f, 1.0f);
          Blend(pixel, 255, 255, 255, bar);
        }
      }

      BITMAPINFO color_info = {};
      color_info.bmiHeader.biSize = sizeof(color_info.bmiHeader);
      color_info.bmiHeader.biWidth = width;
      color_info.bmiHeader.biHeight = -height;
      color_info.bmiHeader.biPlanes = 1;
      color_info.bmiHeader.biBitCount = 32;
      color_info.bmiHeader.biCompression = BI_RGB;
      void* color_bits = nullptr;
      HBITMAP color = CreateDIBSection(nullptr, &color_info, DIB_RGB_COLORS,
                                       &color_bits, nullptr, 0);
      // All-zero AND mask: the alpha channel defines transparency.
      const std::vector<uint8_t> mask_bits(
          static_cast<size_t>((width + 15) / 16) * 2 * height, 0);
      HBITMAP mask = CreateBitmap(width, height, 1, 1, mask_bits.data());
      if (color != nullptr && mask != nullptr) {
        std::copy(pixels.begin(), pixels.end(),
                  static_cast<uint32_t*>(color_bits));
        ICONINFO muted = {};
        muted.fIcon = TRUE;
        muted.hbmMask = mask;
        muted.hbmColor = color;
        result = CreateIconIndirect(&muted);
      }
      if (color != nullptr) {
        DeleteObject(color);
      }
      if (mask != nullptr) {
        DeleteObject(mask);
      }
    }
  }
  if (info.hbmColor != nullptr) {
    DeleteObject(info.hbmColor);
  }
  if (info.hbmMask != nullptr) {
    DeleteObject(info.hbmMask);
  }
  return result;
}
