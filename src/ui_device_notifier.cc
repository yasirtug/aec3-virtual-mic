#include "ui_device_notifier.h"

#include <atomic>

class DeviceChangeNotifier::Client final : public IMMNotificationClient {
 public:
  Client(HWND window, UINT message) : window_(window), message_(message) {}

  // IUnknown
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --references_;
    if (remaining == 0) {
      delete this;
    }
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
      *object = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }

  // IMMNotificationClient
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
    return Notify();
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return Notify(); }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
    return Notify();
  }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow,
                                                   ERole,
                                                   LPCWSTR) override {
    return Notify();
  }
  // Property changes (volume, format, names) fire constantly; ignore them.
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR,
                                                   const PROPERTYKEY) override {
    return S_OK;
  }

 private:
  ~Client() = default;

  HRESULT Notify() {
    PostMessageW(window_, message_, 0, 0);
    return S_OK;
  }

  const HWND window_;
  const UINT message_;
  std::atomic<ULONG> references_{1};
};

DeviceChangeNotifier::~DeviceChangeNotifier() {
  Stop();
}

bool DeviceChangeNotifier::Start(HWND window, UINT message) {
  Stop();
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                              CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
    return false;
  }
  auto* client = new Client(window, message);
  if (FAILED(enumerator->RegisterEndpointNotificationCallback(client))) {
    client->Release();
    enumerator->Release();
    return false;
  }
  enumerator_ = enumerator;
  client_ = client;
  return true;
}

void DeviceChangeNotifier::Stop() {
  if (enumerator_ != nullptr) {
    enumerator_->UnregisterEndpointNotificationCallback(client_);
    enumerator_->Release();
    enumerator_ = nullptr;
  }
  if (client_ != nullptr) {
    client_->Release();
    client_ = nullptr;
  }
}
