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

// One entry per detected cable kind, in preference order (VB-CABLE first,
// then VAC). `capture` is used to report each cable's exact capture-side name.
std::vector<VirtualCable> FindVirtualCables(
    const std::vector<AudioDeviceInfo>& render,
    const std::vector<AudioDeviceInfo>& capture);

#endif  // AEC3_VIRTUAL_MIC_AUDIO_DEVICES_H_
