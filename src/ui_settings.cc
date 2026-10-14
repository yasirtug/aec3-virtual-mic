#include "ui_settings.h"

#include <windows.h>

#include <cwchar>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kSettingsKey[] = L"Software\\AEC3VirtualMic";
constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"AEC3VirtualMic";

bool ReadString(const wchar_t* key_path,
                const wchar_t* name,
                std::wstring* value) {
  DWORD bytes = 0;
  if (RegGetValueW(HKEY_CURRENT_USER, key_path, name, RRF_RT_REG_SZ, nullptr,
                   nullptr, &bytes) != ERROR_SUCCESS ||
      bytes < sizeof(wchar_t)) {
    return false;
  }
  std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
  if (RegGetValueW(HKEY_CURRENT_USER, key_path, name, RRF_RT_REG_SZ, nullptr,
                   buffer.data(), &bytes) != ERROR_SUCCESS) {
    return false;
  }
  // RegGetValueW guarantees termination; drop the terminator and anything
  // after it.
  buffer.resize(wcsnlen(buffer.c_str(), buffer.size()));
  *value = std::move(buffer);
  return true;
}

bool WriteString(const wchar_t* key_path,
                 const wchar_t* name,
                 const std::wstring& value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, key_path, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const DWORD bytes =
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  const LSTATUS status =
      RegSetValueExW(key, name, 0, REG_SZ,
                     reinterpret_cast<const BYTE*>(value.c_str()), bytes);
  RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

std::wstring AutostartCommandLine() {
  std::vector<wchar_t> path(MAX_PATH);
  for (;;) {
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) {
      return {};
    }
    if (length < path.size()) {
      std::wstring command_line(L"\"");
      command_line.append(path.data(), length).append(L"\" --startup");
      return command_line;
    }
    if (path.size() >= 32768) {
      return {};
    }
    path.resize(path.size() * 2);
  }
}

}  // namespace

std::wstring ReadSetting(const wchar_t* name) {
  std::wstring value;
  ReadString(kSettingsKey, name, &value);
  return value;
}

void WriteSetting(const wchar_t* name, const std::wstring& value) {
  WriteString(kSettingsKey, name, value);
}

bool ReadBoolSetting(const wchar_t* name, bool default_value) {
  const std::wstring value = ReadSetting(name);
  if (value == L"1") {
    return true;
  }
  if (value == L"0") {
    return false;
  }
  return default_value;
}

void WriteBoolSetting(const wchar_t* name, bool value) {
  WriteSetting(name, value ? L"1" : L"0");
}

bool IsAutostartEnabled() {
  std::wstring value;
  return ReadString(kRunKey, kRunValue, &value) && !value.empty();
}

bool SetAutostartEnabled(bool enabled) {
  if (enabled) {
    const std::wstring command_line = AutostartCommandLine();
    return !command_line.empty() &&
           WriteString(kRunKey, kRunValue, command_line);
  }
  const LSTATUS status =
      RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
  return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}
