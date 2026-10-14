#ifndef AEC3_VIRTUAL_MIC_UI_SETTINGS_H_
#define AEC3_VIRTUAL_MIC_UI_SETTINGS_H_

#include <string>

// Per-user settings in HKCU\Software\AEC3VirtualMic (all values are REG_SZ).
std::wstring ReadSetting(const wchar_t* name);
void WriteSetting(const wchar_t* name, const std::wstring& value);
// "1"/"0" values; anything else (including missing) yields `default_value`.
bool ReadBoolSetting(const wchar_t* name, bool default_value);
void WriteBoolSetting(const wchar_t* name, bool value);

// Autostart: HKCU\...\CurrentVersion\Run value "AEC3VirtualMic" =
// "<exe path>" --startup.
bool IsAutostartEnabled();
bool SetAutostartEnabled(bool enabled);

#endif  // AEC3_VIRTUAL_MIC_UI_SETTINGS_H_
