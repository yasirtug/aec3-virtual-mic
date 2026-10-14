#include "cable_setup.h"

#include "audio_devices.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <cwchar>
#include <string>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

// Exit codes (logged by the installer).
constexpr int kExitOk = 0;
constexpr int kExitComFailed = 1;
constexpr int kExitSnapshotFailed = 3;

// HKLM key of our installer; holds the transient snapshot (and the installer's
// `InstalledVacLite` marker).
constexpr wchar_t kInstallerKey[] = L"Software\\AEC3VirtualMic";

constexpr ERole kRoles[] = {eConsole, eMultimedia, eCommunications};

// Undocumented but long-stable interface used by the Windows Sound control
// panel (Windows 7 and later) to change default endpoints. Only
// SetDefaultEndpoint is called; the vtable order must still match.
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

std::wstring EndpointId(IMMDevice* device) {
  LPWSTR id = nullptr;
  if (FAILED(device->GetId(&id)) || id == nullptr) {
    return {};
  }
  std::wstring result(id);
  CoTaskMemFree(id);
  return result;
}

// Ids of the active virtual cable endpoints (VAC or VB-CABLE) for `flow`.
std::vector<std::wstring> CableEndpointIds(EDataFlow flow) {
  std::vector<std::wstring> ids;
  for (const AudioDeviceInfo& device : EnumerateAudioDevices(flow, nullptr)) {
    if (IsVirtualCableEndpointName(device.name)) {
      ids.push_back(device.id);
    }
  }
  return ids;
}

bool Contains(const std::vector<std::wstring>& ids, const std::wstring& id) {
  for (const std::wstring& candidate : ids) {
    if (candidate == id) {
      return true;
    }
  }
  return false;
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

// Gives every role whose default is now a cable endpoint back to the endpoint
// recorded before the cable was installed (when that endpoint is still
// active and is not a cable itself).
void RestoreDefaults(IMMDeviceEnumerator* enumerator,
                     IPolicyConfig* policy,
                     HKEY snapshot_key) {
  for (EDataFlow flow : {eRender, eCapture}) {
    const std::vector<std::wstring> cables = CableEndpointIds(flow);
    if (cables.empty()) {
      continue;
    }
    for (ERole role : kRoles) {
      ComPtr<IMMDevice> current;
      if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, role, &current)) ||
          !Contains(cables, EndpointId(current.Get()))) {
        continue;
      }
      const std::wstring previous = ReadSnapshot(snapshot_key, flow, role);
      if (previous.empty() || Contains(cables, previous) ||
          !IsActiveEndpoint(enumerator, previous)) {
        continue;
      }
      policy->SetDefaultEndpoint(previous.c_str(), role);
    }
  }
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

int RestoreAudioDefaults() {
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
  HKEY snapshot_key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kInstallerKey, 0,
                    KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY,
                    &snapshot_key) != ERROR_SUCCESS) {
    return kExitOk;  // No snapshot: nothing to restore.
  }

  // The cable's endpoints appear asynchronously after the driver install;
  // Windows may make them the default devices as they arrive.
  constexpr DWORD kPollMs = 250;
  constexpr int kMaxPolls = 60;  // 15 s
  bool cable_present = false;
  for (int i = 0; i < kMaxPolls && !cable_present; ++i) {
    cable_present = !CableEndpointIds(eRender).empty() &&
                    !CableEndpointIds(eCapture).empty();
    if (!cable_present) {
      Sleep(kPollMs);
    }
  }
  if (cable_present) {
    // Default switching can lag behind endpoint arrival: restore twice.
    Sleep(2000);
    RestoreDefaults(enumerator.Get(), policy.Get(), snapshot_key);
    Sleep(3000);
    RestoreDefaults(enumerator.Get(), policy.Get(), snapshot_key);
  }

  for (EDataFlow flow : {eRender, eCapture}) {
    for (ERole role : kRoles) {
      RegDeleteValueW(snapshot_key, SnapshotValueName(flow, role).c_str());
    }
  }
  RegCloseKey(snapshot_key);
  return kExitOk;
}
