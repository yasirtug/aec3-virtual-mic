#include "audio_devices.h"

#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <cwctype>

namespace {

using Microsoft::WRL::ComPtr;

std::wstring HResultText(const wchar_t* operation, HRESULT result) {
  wchar_t* system_message = nullptr;
  const DWORD size = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(result), 0,
      reinterpret_cast<wchar_t*>(&system_message), 0, nullptr);

  std::wstring message(operation);
  message.append(L" failed (0x");
  wchar_t code[9] = {};
  swprintf_s(code, L"%08X", static_cast<unsigned int>(result));
  message.append(code).append(L")");
  if (size != 0 && system_message != nullptr) {
    std::wstring detail(system_message, size);
    while (!detail.empty() &&
           (detail.back() == L'\r' || detail.back() == L'\n')) {
      detail.pop_back();
    }
    message.append(L": ").append(detail);
  }
  if (system_message != nullptr) {
    LocalFree(system_message);
  }
  return message;
}

std::wstring Lowercase(std::wstring value) {
  for (wchar_t& character : value) {
    character = static_cast<wchar_t>(std::towlower(character));
  }
  return value;
}

bool Contains(const std::wstring& haystack, const wchar_t* needle) {
  return haystack.find(needle) != std::wstring::npos;
}

// Plain VB-CABLE: "CABLE Input (VB-Audio Virtual Cable)". The optional
// "CABLE In 16ch" endpoint of the same driver is intentionally not matched.
bool IsVbCableRender(const std::wstring& lowercase_name) {
  return Contains(lowercase_name, L"vb-audio virtual cable") &&
         Contains(lowercase_name, L"cable input");
}

// VB-CABLE A+B / C+D packs: "CABLE-A Input (VB-Audio Cable A)".
bool IsVbCableVariantRender(const std::wstring& lowercase_name) {
  return Contains(lowercase_name, L"vb-audio cable") &&
         Contains(lowercase_name, L"input");
}

bool IsVacLiteRender(const std::wstring& lowercase_name) {
  return lowercase_name == L"line 1 (virtual audio cable)";
}

bool IsOtherVacRender(const std::wstring& lowercase_name) {
  return Contains(lowercase_name, L"virtual audio cable") &&
         !Contains(lowercase_name, L"vb-audio");
}

// Splits a friendly name "Endpoint (Device description)" into its parts.
void SplitFriendlyName(const std::wstring& name,
                       std::wstring* endpoint,
                       std::wstring* description) {
  const size_t open = name.rfind(L" (");
  if (open == std::wstring::npos || name.back() != L')') {
    *endpoint = name;
    description->clear();
    return;
  }
  *endpoint = name.substr(0, open);
  *description = name.substr(open + 2, name.size() - open - 3);
}

// Returns the friendly name of the recording endpoint that carries what is
// played into `render_name`: "CABLE Input (X)" pairs with "CABLE Output (X)";
// VAC lines keep the same name on both sides. When the capture endpoint is not
// active, the expected name is derived from the render name.
std::wstring CaptureNameFor(const std::wstring& render_name,
                            const std::vector<AudioDeviceInfo>& capture) {
  std::wstring endpoint;
  std::wstring description;
  SplitFriendlyName(render_name, &endpoint, &description);
  const size_t input = Lowercase(endpoint).find(L"input");
  if (input != std::wstring::npos) {
    endpoint.replace(input, 5, L"Output");
  }
  const std::wstring expected =
      description.empty() ? endpoint : endpoint + L" (" + description + L")";

  const std::wstring lowercase_expected = Lowercase(expected);
  for (const AudioDeviceInfo& device : capture) {
    if (Lowercase(device.name) == lowercase_expected) {
      return device.name;
    }
  }
  if (!description.empty()) {
    const std::wstring lowercase_endpoint = Lowercase(endpoint);
    const std::wstring lowercase_description =
        L"(" + Lowercase(description) + L")";
    for (const AudioDeviceInfo& device : capture) {
      const std::wstring name = Lowercase(device.name);
      if (Contains(name, lowercase_description.c_str()) &&
          Contains(name, lowercase_endpoint.c_str())) {
        return device.name;
      }
    }
  }
  return expected;
}

}  // namespace

std::vector<AudioDeviceInfo> EnumerateAudioDevices(EDataFlow flow,
                                                   std::wstring* error) {
  std::vector<AudioDeviceInfo> devices;
  if (error != nullptr) {
    error->clear();
  }

  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                    CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  if (FAILED(result)) {
    if (error != nullptr) {
      *error = HResultText(L"Creating the Windows audio enumerator", result);
    }
    return devices;
  }

  ComPtr<IMMDeviceCollection> collection;
  result = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
  if (FAILED(result)) {
    if (error != nullptr) {
      *error = HResultText(L"Enumerating active audio devices", result);
    }
    return devices;
  }

  UINT count = 0;
  result = collection->GetCount(&count);
  if (FAILED(result)) {
    if (error != nullptr) {
      *error = HResultText(L"Reading the audio device count", result);
    }
    return devices;
  }

  devices.reserve(count);
  for (UINT index = 0; index < count; ++index) {
    ComPtr<IMMDevice> device;
    if (FAILED(collection->Item(index, &device))) {
      continue;
    }

    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id))) {
      continue;
    }

    ComPtr<IPropertyStore> properties;
    result = device->OpenPropertyStore(STGM_READ, &properties);
    if (FAILED(result)) {
      CoTaskMemFree(id);
      continue;
    }

    PROPVARIANT name;
    PropVariantInit(&name);
    result = properties->GetValue(PKEY_Device_FriendlyName, &name);
    if (SUCCEEDED(result) && name.vt == VT_LPWSTR && name.pwszVal != nullptr) {
      devices.push_back({id, name.pwszVal});
    }
    PropVariantClear(&name);
    CoTaskMemFree(id);
  }

  return devices;
}

std::wstring GetDefaultAudioDeviceId(EDataFlow flow) {
  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&enumerator)))) {
    return {};
  }

  ComPtr<IMMDevice> device;
  if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, eCommunications,
                                                 &device)) &&
      FAILED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device))) {
    return {};
  }

  LPWSTR raw_id = nullptr;
  if (FAILED(device->GetId(&raw_id))) {
    return {};
  }
  std::wstring id(raw_id);
  CoTaskMemFree(raw_id);
  return id;
}

std::vector<VirtualCable> FindVirtualCables(
    const std::vector<AudioDeviceInfo>& render,
    const std::vector<AudioDeviceInfo>& capture) {
  const AudioDeviceInfo* vb_cable = nullptr;
  const AudioDeviceInfo* vb_cable_variant = nullptr;
  std::wstring vb_cable_variant_name;
  const AudioDeviceInfo* vac_lite = nullptr;
  const AudioDeviceInfo* other_vac = nullptr;
  for (const AudioDeviceInfo& device : render) {
    const std::wstring name = Lowercase(device.name);
    if (IsVbCableRender(name)) {
      if (vb_cable == nullptr) {
        vb_cable = &device;
      }
    } else if (IsVbCableVariantRender(name)) {
      // Several A+B/C+D cables may exist; pick "CABLE-A" before "CABLE-B".
      if (vb_cable_variant == nullptr || name < vb_cable_variant_name) {
        vb_cable_variant = &device;
        vb_cable_variant_name = name;
      }
    } else if (IsVacLiteRender(name)) {
      if (vac_lite == nullptr) {
        vac_lite = &device;
      }
    } else if (IsOtherVacRender(name)) {
      if (other_vac == nullptr) {
        other_vac = &device;
      }
    }
  }

  std::vector<VirtualCable> cables;
  const auto add = [&](VirtualCableKind kind, const AudioDeviceInfo* device) {
    VirtualCable cable;
    cable.kind = kind;
    cable.render_id = device->id;
    cable.render_name = device->name;
    cable.capture_name = CaptureNameFor(device->name, capture);
    cables.push_back(std::move(cable));
  };
  if (vb_cable != nullptr || vb_cable_variant != nullptr) {
    add(VirtualCableKind::kVbCable,
        vb_cable != nullptr ? vb_cable : vb_cable_variant);
  }
  if (vac_lite != nullptr || other_vac != nullptr) {
    add(VirtualCableKind::kVacLite, vac_lite != nullptr ? vac_lite : other_vac);
  }
  return cables;
}
