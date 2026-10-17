#ifndef AEC3_VIRTUAL_MIC_UI_MUTE_H_
#define AEC3_VIRTUAL_MIC_UI_MUTE_H_

#include <windows.h>

#include <string>

// Publishes the mute state to other processes (e.g. a keyboard LED helper)
// through two named events in the session namespace:
//   Local\AEC3VirtualMic.Muted        manual-reset, signaled while muted
//   Local\AEC3VirtualMic.MuteChanged  auto-reset, set on every change
// Readers open them by name (SYNCHRONIZE) and test `Muted` with a zero
// timeout. Both disappear when the app exits, so a reader that reopens them
// periodically sees "not muted" once the app is gone, even after a crash.
class MuteStatePublisher {
 public:
  MuteStatePublisher();
  ~MuteStatePublisher();

  MuteStatePublisher(const MuteStatePublisher&) = delete;
  MuteStatePublisher& operator=(const MuteStatePublisher&) = delete;

  void Publish(bool muted);

 private:
  HANDLE muted_ = nullptr;
  HANDLE changed_ = nullptr;
};

// Hotkeys use the hotkey control's HKM_GETHOTKEY format: LOBYTE = virtual
// key, HIBYTE = HOTKEYF_* modifiers. 0 = none.
std::wstring HotkeyName(WORD hotkey);
UINT HotkeyRegisterModifiers(WORD hotkey);

// Copy of `base` with a red "muted" badge in the lower right corner, or
// nullptr. The caller owns the returned icon (DestroyIcon).
HICON CreateMutedIcon(HICON base);

#endif  // AEC3_VIRTUAL_MIC_UI_MUTE_H_
