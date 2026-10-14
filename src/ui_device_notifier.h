#ifndef AEC3_VIRTUAL_MIC_UI_DEVICE_NOTIFIER_H_
#define AEC3_VIRTUAL_MIC_UI_DEVICE_NOTIFIER_H_

#include <windows.h>

#include <mmdeviceapi.h>

// Watches audio endpoints (added, removed, enabled/disabled, default changed)
// and posts `message` to `window` for each change. Notifications arrive on
// MMDevice worker threads, so the UI thread does all the real work.
class DeviceChangeNotifier {
 public:
  DeviceChangeNotifier() = default;
  ~DeviceChangeNotifier();

  DeviceChangeNotifier(const DeviceChangeNotifier&) = delete;
  DeviceChangeNotifier& operator=(const DeviceChangeNotifier&) = delete;

  // Call on a COM-initialized thread.
  bool Start(HWND window, UINT message);
  void Stop();

 private:
  class Client;
  IMMDeviceEnumerator* enumerator_ = nullptr;
  Client* client_ = nullptr;
};

#endif  // AEC3_VIRTUAL_MIC_UI_DEVICE_NOTIFIER_H_
