#ifndef AEC3_VIRTUAL_MIC_UI_LEVEL_METER_H_
#define AEC3_VIRTUAL_MIC_UI_LEVEL_METER_H_

#include <windows.h>

// Horizontal peak meter control: a bar from -60 dBFS to 0 dBFS (green, then
// amber above -12 dBFS, red above -3 dBFS) followed by the numeric level.
// Honors WM_SETFONT for the numeric label.
inline constexpr wchar_t kLevelMeterClass[] = L"Aec3VirtualMicLevelMeter";

bool RegisterLevelMeterClass(HINSTANCE instance);

// `dbfs` is a peak level in dBFS; -120 (or anything below the meter floor)
// renders as silence. Repaints only when the displayed value changes.
void SetLevelMeterValue(HWND meter, float dbfs);

#endif  // AEC3_VIRTUAL_MIC_UI_LEVEL_METER_H_
