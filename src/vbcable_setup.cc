#include "vbcable_setup.h"

#include "audio_devices.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <cwchar>
#include <string>
#include <utility>
#include <vector>

// How the values below were chosen is documented (with sources) in
// installer/README.md, section "Recommended VB-CABLE settings".

namespace {

using Microsoft::WRL::ComPtr;

// Exit codes (logged by the installer).
constexpr int kExitOk = 0;
constexpr int kExitComFailed = 1;
constexpr int kExitDriverSettingsFailed = 2;
constexpr int kExitSnapshotFailed = 3;

// HKLM key of our installer (also holds the `InstalledVbCable` marker).
constexpr wchar_t kInstallerKey[] = L"Software\\AEC3VirtualMic";

// Parameters the VB-CABLE driver (vbaudio_cable64_win10.sys, pack 45) reads
// once when the driver starts: REG_DWORD values under
// HKLM\Software\VB-Audio\Cable. They take effect on the next driver start
// (the reboot VB-CABLE requires after installation).
constexpr wchar_t kVbCableDriverKey[] = L"Software\\VB-Audio\\Cable";
// Internal sample rate; 48 kHz equals the cable's endpoint formats and the
// app's processing rate, so audio passes the cable without resampling.
constexpr wchar_t kVbCableSampleRateValue[] = L"VBAudioCableWDM_SR";
constexpr DWORD kVbCableSampleRate = 48000;
// Pipe latency in samples (driver default 7168 = 149 ms at 48 kHz). The
// Windows audio engine feeds and drains the cable with 10 ms (480 sample)
// periods; 3 x 1024 keeps VB-Audio's "3 x largest buffer" rule with headroom
// for clients using up to 1024-sample buffers.
constexpr wchar_t kVbCableLatencyValue[] = L"VBAudioCableWDM_Latency";
constexpr DWORD kVbCableLatency = 3 * 1024;

// Adapter name of the VB-CABLE endpoints ("CABLE Input", "CABLE In 16ch",
// "CABLE Output"), from the driver INF.
constexpr wchar_t kVbCableAdapterName[] = L"VB-Audio Virtual Cable";
// Endpoint names whose default format must be 48 kHz.
constexpr wchar_t kCableInputName[] = L"CABLE Input";
constexpr wchar_t kCableOutputName[] = L"CABLE Output";
constexpr DWORD kTargetSampleRate = 48000;
constexpr DWORD kStereoMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

// Defined locally (values from functiondiscoverykeys_devpkey.h) so no extra
// GUID library is needed.
// PKEY_DeviceInterface_FriendlyName: adapter name, "VB-Audio Virtual Cable".
constexpr PROPERTYKEY kAdapterNameKey = {
    {0x026e516e,
     0xb814,
     0x414b,
     {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}},
    2};
// PKEY_Device_DeviceDesc: endpoint name, e.g. "CABLE Input".
constexpr PROPERTYKEY kEndpointNameKey = {
    {0xa45c254e,
     0xdf1c,
     0x4efd,
     {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    2};

constexpr ERole kRoles[] = {eConsole, eMultimedia, eCommunications};

// Undocumented but long-stable interface used by the Windows Sound control
// panel (Windows 7 and later) to change default endpoints and device formats.
// Only the methods called here matter; the vtable order must match.
MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown {
 public:
  virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR device_id,
                                                 WAVEFORMATEX** format) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR device_id,
                                                    INT default_format,
                                                    WAVEFORMATEX** format) = 0;
  virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR device_id) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(
      PCWSTR device_id,
      WAVEFORMATEX* endpoint_format,
      WAVEFORMATEX* mix_format) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR device_id,
                                                        INT default_period,
                                                        PINT64 default_value,
                                                        PINT64 minimum) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR device_id,
                                                        PINT64 period) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR device_id,
                                                 void* mode) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR device_id,
                                                 void* mode) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR device_id,
                                                     const PROPERTYKEY& key,
                                                     PROPVARIANT* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR device_id,
                                                     const PROPERTYKEY& key,
                                                     PROPVARIANT* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR device_id,
                                                       ERole role) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR device_id,
                                                          INT visible) = 0;
};

// CPolicyConfigClient
constexpr CLSID kPolicyConfigClsid = {
    0x870af99c,
    0x171d,
    0x4f9e,
    {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

constexpr GUID kSubtypePcm = {
    0x00000001,
    0x0000,
    0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kSubtypeFloat = {
    0x00000003,
    0x0000,
    0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

class ComScope {
 public:
  ComScope() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
  ~ComScope() {
    if (SUCCEEDED(result_)) {
      CoUninitialize();
    }
  }
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;

  // RPC_E_CHANGED_MODE still leaves COM usable on this thread.
  bool usable() const {
    return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
  }

 private:
  HRESULT result_;
};

std::wstring SnapshotValueName(EDataFlow flow, ERole role) {
  return std::wstring(flow == eRender ? L"SnapshotDefaultRender"
                                      : L"SnapshotDefaultCapture") +
         std::to_wstring(static_cast<int>(role));
}

bool WriteDword(HKEY key, const wchar_t* name, DWORD value) {
  return RegSetValueExW(key, name, 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&value),
                        sizeof(value)) == ERROR_SUCCESS;
}

bool ApplyDriverSettings() {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kVbCableDriverKey, 0, nullptr,
                      REG_OPTION_NON_VOLATILE,
                      KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key,
                      nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const bool ok =
      WriteDword(key, kVbCableSampleRateValue, kVbCableSampleRate) &&
      WriteDword(key, kVbCableLatencyValue, kVbCableLatency);
  RegCloseKey(key);
  return ok;
}

std::wstring EndpointId(IMMDevice* device) {
  LPWSTR id = nullptr;
  if (FAILED(device->GetId(&id)) || id == nullptr) {
    return {};
  }
  std::wstring result(id);
  CoTaskMemFree(id);
  return result;
}

std::wstring StringProperty(IMMDevice* device, const PROPERTYKEY& key) {
  ComPtr<IPropertyStore> store;
  if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) {
    return {};
  }
  PROPVARIANT value;
  PropVariantInit(&value);
  std::wstring result;
  if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR &&
      value.pwszVal != nullptr) {
    result = value.pwszVal;
  }
  PropVariantClear(&value);
  return result;
}

struct CableEndpoint {
  std::wstring id;
  std::wstring endpoint_name;  // e.g. "CABLE Input"
};

// Active endpoints of the plain VB-CABLE driver for `flow`.
std::vector<CableEndpoint> FindCableEndpoints(IMMDeviceEnumerator* enumerator,
                                              EDataFlow flow) {
  std::vector<CableEndpoint> result;
  ComPtr<IMMDeviceCollection> collection;
  if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE,
                                            &collection))) {
    return result;
  }
  UINT count = 0;
  if (FAILED(collection->GetCount(&count))) {
    return result;
  }
  for (UINT i = 0; i < count; ++i) {
    ComPtr<IMMDevice> device;
    if (FAILED(collection->Item(i, &device))) {
      continue;
    }
    const std::wstring adapter =
        StringProperty(device.Get(), kAdapterNameKey);
    if (CompareStringOrdinal(adapter.c_str(), -1, kVbCableAdapterName, -1,
                             TRUE) != CSTR_EQUAL) {
      continue;
    }
    CableEndpoint endpoint;
    endpoint.id = EndpointId(device.Get());
    endpoint.endpoint_name =
        StringProperty(device.Get(), kEndpointNameKey);
    if (!endpoint.id.empty()) {
      result.push_back(std::move(endpoint));
    }
  }
  return result;
}

bool IsActiveEndpoint(IMMDeviceEnumerator* enumerator,
                      const std::wstring& id) {
  ComPtr<IMMDevice> device;
  DWORD state = 0;
  return SUCCEEDED(enumerator->GetDevice(id.c_str(), &device)) &&
         SUCCEEDED(device->GetState(&state)) && state == DEVICE_STATE_ACTIVE;
}

std::wstring ReadSnapshot(HKEY key, EDataFlow flow, ERole role) {
  const std::wstring name = SnapshotValueName(flow, role);
  DWORD bytes = 0;
  if (RegGetValueW(key, nullptr, name.c_str(), RRF_RT_REG_SZ, nullptr,
                   nullptr, &bytes) != ERROR_SUCCESS ||
      bytes < sizeof(wchar_t)) {
    return {};
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  if (RegGetValueW(key, nullptr, name.c_str(), RRF_RT_REG_SZ, nullptr,
                   value.data(), &bytes) != ERROR_SUCCESS) {
    return {};
  }
  value.resize(wcsnlen(value.c_str(), value.size()));
  return value;
}

bool ContainsId(const std::vector<CableEndpoint>& endpoints,
                const std::wstring& id) {
  for (const CableEndpoint& endpoint : endpoints) {
    if (endpoint.id == id) {
      return true;
    }
  }
  return false;
}

// Gives every role whose default is now a CABLE endpoint back to the endpoint
// recorded before VB-CABLE was installed (when that endpoint is still active).
void RestoreDefaults(IMMDeviceEnumerator* enumerator,
                     IPolicyConfig* policy,
                     HKEY snapshot_key) {
  for (EDataFlow flow : {eRender, eCapture}) {
    const std::vector<CableEndpoint> cables =
        FindCableEndpoints(enumerator, flow);
    if (cables.empty()) {
      continue;
    }
    for (ERole role : kRoles) {
      ComPtr<IMMDevice> current;
      if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, role, &current))) {
        continue;
      }
      if (!ContainsId(cables, EndpointId(current.Get()))) {
        continue;
      }
      const std::wstring previous = ReadSnapshot(snapshot_key, flow, role);
      if (previous.empty() || ContainsId(cables, previous) ||
          !IsActiveEndpoint(enumerator, previous)) {
        continue;
      }
      policy->SetDefaultEndpoint(previous.c_str(), role);
    }
  }
}

// Makes the endpoint's shared-mode device format 48 kHz when it is not.
// Keeps the channel count; uses 24-bit PCM like the driver's OEM default.
void EnsureEndpointRate(IPolicyConfig* policy, const std::wstring& id) {
  WAVEFORMATEX* current = nullptr;
  if (FAILED(policy->GetDeviceFormat(id.c_str(), FALSE, &current)) ||
      current == nullptr) {
    return;
  }
  const bool already_ok = current->nSamplesPerSec == kTargetSampleRate;
  WORD channels = current->nChannels;
  DWORD channel_mask = kStereoMask;
  if (current->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
      current->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
    channel_mask =
        reinterpret_cast<WAVEFORMATEXTENSIBLE*>(current)->dwChannelMask;
  }
  CoTaskMemFree(current);
  if (already_ok) {
    return;
  }
  if (channels == 0) {
    channels = 2;
    channel_mask = kStereoMask;
  }

  WAVEFORMATEXTENSIBLE device_format = {};
  device_format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  device_format.Format.nChannels = channels;
  device_format.Format.nSamplesPerSec = kTargetSampleRate;
  device_format.Format.wBitsPerSample = 24;
  device_format.Format.nBlockAlign = static_cast<WORD>(channels * 3);
  device_format.Format.nAvgBytesPerSec =
      kTargetSampleRate * device_format.Format.nBlockAlign;
  device_format.Format.cbSize =
      sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  device_format.Samples.wValidBitsPerSample = 24;
  device_format.dwChannelMask = channel_mask;
  device_format.SubFormat = kSubtypePcm;

  WAVEFORMATEXTENSIBLE mix_format = device_format;
  mix_format.Format.wBitsPerSample = 32;
  mix_format.Format.nBlockAlign = static_cast<WORD>(channels * 4);
  mix_format.Format.nAvgBytesPerSec =
      kTargetSampleRate * mix_format.Format.nBlockAlign;
  mix_format.Samples.wValidBitsPerSample = 32;
  mix_format.SubFormat = kSubtypeFloat;

  policy->SetDeviceFormat(id.c_str(), &device_format.Format,
                          &mix_format.Format);
}

}  // namespace

int DetectVirtualCables() {
  ComScope com;
  if (!com.usable()) {
    return 0;
  }
  const std::vector<AudioDeviceInfo> render =
      EnumerateAudioDevices(eRender, nullptr);
  const std::vector<AudioDeviceInfo> capture =
      EnumerateAudioDevices(eCapture, nullptr);
  int detected = 0;
  for (const VirtualCable& cable : FindVirtualCables(render, capture)) {
    if (cable.kind == VirtualCableKind::kVbCable) {
      detected |= kDetectedVbCable;
    } else if (cable.kind == VirtualCableKind::kVacLite) {
      detected |= kDetectedVacLite;
    }
  }
  return detected;
}

int SnapshotAudioDefaults() {
  ComScope com;
  if (!com.usable()) {
    return kExitComFailed;
  }
  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                              CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
    return kExitComFailed;
  }
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kInstallerKey, 0, nullptr,
                      REG_OPTION_NON_VOLATILE,
                      KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key,
                      nullptr) != ERROR_SUCCESS) {
    return kExitSnapshotFailed;
  }
  bool ok = true;
  for (EDataFlow flow : {eRender, eCapture}) {
    for (ERole role : kRoles) {
      const std::wstring name = SnapshotValueName(flow, role);
      ComPtr<IMMDevice> device;
      std::wstring id;
      if (SUCCEEDED(
              enumerator->GetDefaultAudioEndpoint(flow, role, &device))) {
        id = EndpointId(device.Get());
      }
      if (id.empty()) {
        RegDeleteValueW(key, name.c_str());
        continue;
      }
      ok = RegSetValueExW(key, name.c_str(), 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(id.c_str()),
                          static_cast<DWORD>((id.size() + 1) *
                                             sizeof(wchar_t))) ==
               ERROR_SUCCESS &&
           ok;
    }
  }
  RegCloseKey(key);
  return ok ? kExitOk : kExitSnapshotFailed;
}

int ConfigureFreshVbCable() {
  const bool driver_ok = ApplyDriverSettings();

  ComScope com;
  if (!com.usable()) {
    return kExitComFailed;
  }
  ComPtr<IMMDeviceEnumerator> enumerator;
  ComPtr<IPolicyConfig> policy;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                              CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
      FAILED(CoCreateInstance(kPolicyConfigClsid, nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&policy)))) {
    return kExitComFailed;
  }

  // The cable's endpoints appear asynchronously after the driver install;
  // Windows may make them the default devices as they arrive.
  constexpr DWORD kPollMs = 250;
  constexpr int kMaxPolls = 60;  // 15 s
  bool cable_present = false;
  for (int i = 0; i < kMaxPolls; ++i) {
    if (!FindCableEndpoints(enumerator.Get(), eRender).empty() &&
        !FindCableEndpoints(enumerator.Get(), eCapture).empty()) {
      cable_present = true;
      break;
    }
    Sleep(kPollMs);
  }

  HKEY snapshot_key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kInstallerKey, 0,
                    KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY,
                    &snapshot_key) != ERROR_SUCCESS) {
    snapshot_key = nullptr;
  }

  if (cable_present) {
    // Default switching can lag behind endpoint arrival: restore twice.
    Sleep(2000);
    if (snapshot_key != nullptr) {
      RestoreDefaults(enumerator.Get(), policy.Get(), snapshot_key);
    }
    for (EDataFlow flow : {eRender, eCapture}) {
      for (const CableEndpoint& endpoint :
           FindCableEndpoints(enumerator.Get(), flow)) {
        if (CompareStringOrdinal(endpoint.endpoint_name.c_str(), -1,
                                 kCableInputName, -1, TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(endpoint.endpoint_name.c_str(), -1,
                                 kCableOutputName, -1, TRUE) == CSTR_EQUAL) {
          EnsureEndpointRate(policy.Get(), endpoint.id);
        }
      }
    }
    Sleep(3000);
    if (snapshot_key != nullptr) {
      RestoreDefaults(enumerator.Get(), policy.Get(), snapshot_key);
    }
  }

  if (snapshot_key != nullptr) {
    for (EDataFlow flow : {eRender, eCapture}) {
      for (ERole role : kRoles) {
        RegDeleteValueW(snapshot_key, SnapshotValueName(flow, role).c_str());
      }
    }
    RegCloseKey(snapshot_key);
  }
  return driver_ok ? kExitOk : kExitDriverSettingsFailed;
}
