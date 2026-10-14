#ifndef AEC3_VIRTUAL_MIC_AUDIO_DEVICES_H_
#define AEC3_VIRTUAL_MIC_AUDIO_DEVICES_H_

#include <mmdeviceapi.h>

#include <string>
#include <vector>

struct AudioDeviceInfo {
  std::wstring id;
  std::wstring name;
};

std::vector<AudioDeviceInfo> EnumerateAudioDevices(EDataFlow flow,
                                                   std::wstring* error);
std::wstring GetDefaultAudioDeviceId(EDataFlow flow);

enum class VirtualCableKind { kNone, kVbCable, kVacLite };

struct VirtualCable {
  VirtualCableKind kind = VirtualCableKind::kNone;
  // Endpoint the app plays into ("CABLE Input" / VAC "Line 1" playback).
  std::wstring render_id;
  // Its friendly name.
  std::wstring render_name;
  // Friendly name users must pick as microphone in Discord etc.
  std::wstring capture_name;
};

// Prefers VB-CABLE over VAC Lite. `capture` is used to report the exact
// capture-side name.
VirtualCable FindVirtualCable(const std::vector<AudioDeviceInfo>& render,
                              const std::vector<AudioDeviceInfo>& capture);

#endif  // AEC3_VIRTUAL_MIC_AUDIO_DEVICES_H_
