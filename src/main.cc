#include <windows.h>

#include <commctrl.h>
#include <mmdeviceapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "audio_devices.h"
#include "audio_engine.h"
#include "echo_model.h"
#include "resource.h"
#include "ui_device_notifier.h"
#include "ui_level_meter.h"
#include "ui_settings.h"
#include "vbcable_setup.h"

namespace {

constexpr wchar_t kAppName[] = L"AEC3 Virtual Mic";
constexpr wchar_t kAppVersion[] = L"" AEC3VM_VERSION_STR;
constexpr wchar_t kWindowClass[] = L"Aec3VirtualMicrophoneWindow";
constexpr wchar_t kInstanceMutex[] = L"AEC3VirtualMic.Running";
constexpr wchar_t kVbCableUrl[] = L"https://vb-audio.com/Cable/";

constexpr UINT kEngineErrorMessage = WM_APP + 1;  // wparam: engine session
constexpr UINT kTrayMessage = WM_APP + 2;
constexpr UINT kDevicesChangedMessage = WM_APP + 3;
constexpr UINT kActivateMessage = WM_APP + 4;  // Sent by a second launch.

constexpr UINT_PTR kLevelTimer = 1;
constexpr UINT kLevelTimerMs = 66;  // ~15 Hz.
constexpr UINT_PTR kDeviceRefreshTimer = 2;
constexpr UINT kDeviceRefreshDelayMs = 400;  // Coalesces bursts of changes.
constexpr UINT_PTR kCopyFeedbackTimer = 3;
constexpr UINT kCopyFeedbackMs = 1500;
constexpr ULONGLONG kClipWarningHoldMs = 3000;

constexpr UINT kTrayIconId = 1;

// Settings value names (HKCU\Software\AEC3VirtualMic).
constexpr wchar_t kMicrophoneSetting[] = L"MicrophoneId";
constexpr wchar_t kSpeakersSetting[] = L"SpeakerOutputId";
constexpr wchar_t kNoiseSetting[] = L"NoiseSuppression";
constexpr wchar_t kEchoCancellationSetting[] = L"EchoCancellation";
constexpr wchar_t kNeuralModelSetting[] = L"NeuralEchoModel";

enum ControlId : int {
  kStartStopButton = 1001,
  kStatusText,
  kStatusDetail,
  kDiscordName,
  kCopyNameButton,
  kMicrophoneCombo,
  kSpeakersCombo,
  kCableText,
  kCableLink,
  kEchoCancellationCheck,
  kEchoModeCombo,
  kModelStatus,
  kImportModelButton,
  kNoiseCombo,
  kMicrophoneMeter,
  kReferenceMeter,
  kOutputMeter,
  kClipText,
  kClipSettingsButton,
  kAutostartCheck,
  kAboutButton,
  kTrayShow = 2001,
  kTrayStartStop,
  kTrayExit,
};

// Dropdown order; the registry stores the stable NoiseSuppressionMode value.
struct NoiseModeOption {
  const wchar_t* label;
  NoiseSuppressionMode mode;
};
constexpr NoiseModeOption kNoiseModeOptions[] = {
    {L"Off", NoiseSuppressionMode::kOff},
    {L"WebRTC (moderate)", NoiseSuppressionMode::kWebRtcModerate},
    {L"WebRTC (high)", NoiseSuppressionMode::kWebRtcHigh},
    {L"RNNoise (gentle, max \u221212 dB)", NoiseSuppressionMode::kRnnoiseGentle},
    {L"RNNoise (medium, max \u221220 dB)", NoiseSuppressionMode::kRnnoiseMedium},
    {L"RNNoise (full)", NoiseSuppressionMode::kRnnoiseFull},
};
constexpr NoiseSuppressionMode kDefaultNoiseMode =
    NoiseSuppressionMode::kWebRtcHigh;

// Echo suppression dropdown order.
constexpr const wchar_t* kEchoModeLabels[] = {
    L"Neural (Chrome model)",
    L"Classic WebRTC AEC3",
};
constexpr int kEchoModeNeuralIndex = 0;
constexpr int kEchoModeClassicIndex = 1;

// ---------------------------------------------------------------------------
// Tooltip texts.

constexpr wchar_t kTipStartStop[] =
    L"Start or stop processing. While running, your microphone is cleaned up "
    L"and sent to the virtual cable. Every setting can be changed while it "
    L"runs.";
constexpr wchar_t kTipStatus[] =
    L"Current state. If something goes wrong (for example a device is "
    L"unplugged), processing stops and the reason is shown here.";
constexpr wchar_t kTipDiscordName[] =
    L"Other apps hear this app through the virtual cable. In Discord (User "
    L"Settings \u2192 Voice & Video \u2192 Input Device), Zoom, Teams, OBS, "
    L"etc., select exactly this device as your microphone.\n\nDo not select "
    L"your real microphone there, or echo cancellation is bypassed.";
constexpr wchar_t kTipCopyName[] =
    L"Copy the device name to the clipboard.";
constexpr wchar_t kTipMicrophone[] =
    L"Your real, physical microphone. The app captures it, removes echo and "
    L"noise, and sends the result to the virtual cable.";
constexpr wchar_t kTipSpeakers[] =
    L"The speakers or headphones you actually listen on.\n\nThe app records "
    L"what plays on this device and uses it as the echo reference: whatever "
    L"comes out of these speakers is removed from your microphone, so people "
    L"in the call don't hear themselves. If this is the wrong device, echo "
    L"cancellation has nothing to work with.";
constexpr wchar_t kTipCable[] =
    L"The app plays your processed voice into a virtual audio cable; other "
    L"apps use the cable's other end as their microphone.\n\nThe AEC3 Virtual "
    L"Mic installer sets up VB-CABLE (VB-Audio donationware) for you. VAC "
    L"Lite (\"Line 1\") also works.";
constexpr wchar_t kTipCableLink[] = L"Opens the VB-Audio website.";
constexpr wchar_t kTipEchoCancellation[] =
    L"Removes the sound of your speakers from your microphone.\n\nLeave this "
    L"on unless you use headphones: with headphones there is no echo to "
    L"remove, and turning it off avoids any processing artifacts. Applies "
    L"instantly.";
constexpr wchar_t kTipEchoMode[] =
    L"Neural (Chrome model): adds the machine-learning residual echo "
    L"estimator that Chrome uses for its own echo cancellation; it removes "
    L"leftover echo better.\n\nClassic WebRTC AEC3: the standard open-source "
    L"echo canceller, always available.\n\nIf Neural is selected but the "
    L"model is missing or can't be loaded, the app automatically uses "
    L"Classic. Changing this restarts processing.";
constexpr wchar_t kTipModelStatus[] =
    L"The neural model is Google's unreleased machine-learning residual echo "
    L"estimator, which Chrome downloads for its own echo cancellation.\n\n"
    L"This app only copies that file from your local Chrome profile: nothing "
    L"is downloaded or uploaded, no network is used. It is not bundled "
    L"because Google hasn't released it, and it is not covered by this app's "
    L"MIT license. It may be missing if Chrome hasn't downloaded it yet.";
constexpr wchar_t kTipImportModel[] =
    L"Copy the neural echo model from your local Chrome (Stable, Beta, Dev or "
    L"Canary) into this app's data folder. Nothing is downloaded. If "
    L"processing is running with Neural selected, it restarts to load the "
    L"model.";
constexpr wchar_t kTipNoise[] =
    L"Off: no noise suppression.\n"
    L"WebRTC (moderate): light and very natural.\n"
    L"WebRTC (high): the level Chrome uses in calls.\n"
    L"RNNoise (gentle / medium): neural noise removal capped at 12 or 20 dB "
    L"of attenuation, so your voice stays natural.\n"
    L"RNNoise (full): strongest; removes the most noise but can eat parts of "
    L"your voice in noisy rooms.\n\nApplies instantly.";
constexpr wchar_t kTipMicrophoneMeter[] =
    L"Peak level of your raw microphone. When you speak normally, peaks "
    L"around \u221220 to \u22126 dB are ideal. Red means it is close to "
    L"clipping.";
constexpr wchar_t kTipReferenceMeter[] =
    L"Peak level of what plays on your speakers (the echo reference). It "
    L"moves when the other side talks or any other audio plays.";
constexpr wchar_t kTipOutputMeter[] =
    L"Peak level of your processed voice sent to the virtual cable \u2014 "
    L"what others hear.";
constexpr wchar_t kTipClip[] =
    L"Your microphone signal hits the maximum level (clipping). Clipped audio "
    L"is distorted and makes echo cancellation much worse. Lower the "
    L"microphone's input volume in Windows to about 70%.";
constexpr wchar_t kTipClipSettings[] =
    L"Opens Windows Settings \u2192 Sound. Select your microphone under "
    L"Input and lower its volume to about 70%.";
constexpr wchar_t kTipAutostart[] =
    L"Start the app automatically when you sign in to Windows. It starts in "
    L"the notification area and begins processing right away. Off unless you "
    L"turn it on.";
constexpr wchar_t kTipAbout[] = L"Version, license and third-party notices.";

// ---------------------------------------------------------------------------

enum class UiStatus { kStopped, kRunning, kRestarting, kError };

struct AppState {
  HINSTANCE instance = nullptr;
  HWND window = nullptr;
  HWND tooltip = nullptr;

  // Status.
  HWND status_group = nullptr;
  HWND status_text = nullptr;
  HWND status_detail = nullptr;
  HWND start_stop_button = nullptr;
  HWND discord_label = nullptr;
  HWND discord_name = nullptr;
  HWND copy_name_button = nullptr;
  // Devices.
  HWND devices_group = nullptr;
  HWND microphone_label = nullptr;
  HWND microphone_combo = nullptr;
  HWND speakers_label = nullptr;
  HWND speakers_combo = nullptr;
  HWND cable_text = nullptr;
  HWND cable_link = nullptr;
  // Processing.
  HWND processing_group = nullptr;
  HWND echo_cancellation_check = nullptr;
  HWND echo_mode_label = nullptr;
  HWND echo_mode_combo = nullptr;
  HWND model_status = nullptr;
  HWND import_model_button = nullptr;
  HWND noise_label = nullptr;
  HWND noise_combo = nullptr;
  // Levels.
  HWND levels_group = nullptr;
  HWND microphone_meter_label = nullptr;
  HWND microphone_meter = nullptr;
  HWND reference_meter_label = nullptr;
  HWND reference_meter = nullptr;
  HWND output_meter_label = nullptr;
  HWND output_meter = nullptr;
  HWND clip_text = nullptr;
  HWND clip_settings_button = nullptr;
  // App.
  HWND app_group = nullptr;
  HWND autostart_check = nullptr;
  HWND about_button = nullptr;

  UINT dpi = USER_DEFAULT_SCREEN_DPI;
  HFONT font = nullptr;
  HFONT status_font = nullptr;
  int text_height = 16;

  // Devices shown in the combos (the virtual cable's own endpoints are
  // excluded: they can be neither the microphone nor the echo reference).
  std::vector<AudioDeviceInfo> microphones;
  std::vector<AudioDeviceInfo> speakers;
  VirtualCable cable;
  std::wstring device_error;
  // User choices from the registry; empty = follow the Windows default.
  std::wstring preferred_microphone_id;
  std::wstring preferred_speakers_id;

  AudioEngine engine;
  EngineConfig running_config;  // Valid while the engine runs.
  std::wstring running_microphone_name;
  std::wstring running_speakers_name;
  WPARAM engine_session = 0;  // Filters errors posted by earlier sessions.

  UiStatus status = UiStatus::kStopped;
  bool level_timer_active = false;
  bool clip_warning_visible = false;
  ULONGLONG last_clip_tick = 0;

  DeviceChangeNotifier device_notifier;
  UINT taskbar_created_message = 0;
  bool tray_icon_added = false;
  bool exit_requested = false;
};

int Scale(const AppState* state, int value) {
  return MulDiv(value, static_cast<int>(state->dpi), USER_DEFAULT_SCREEN_DPI);
}

bool IsHighContrast() {
  HIGHCONTRASTW contrast = {sizeof(contrast)};
  return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast),
                               &contrast, 0) &&
         (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

std::wstring ExecutableDirectory() {
  std::vector<wchar_t> path(MAX_PATH);
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0) {
      return {};
    }
    if (length < path.size()) {
      std::wstring directory(path.data(), length);
      const size_t slash = directory.find_last_of(L"\\/");
      directory.resize(slash == std::wstring::npos ? 0 : slash);
      return directory;
    }
    if (path.size() >= 32768) {
      return {};
    }
    path.resize(path.size() * 2);
  }
}

bool FileExists(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Opens a web link, or "file:<name>" (a text file next to the executable) in
// Notepad, which works regardless of file associations.
void OpenLink(HWND owner, const wchar_t* href) {
  if (href == nullptr || *href == L'\0') {
    return;
  }
  constexpr wchar_t kFilePrefix[] = L"file:";
  constexpr size_t kFilePrefixLength = std::size(kFilePrefix) - 1;
  if (wcsncmp(href, kFilePrefix, kFilePrefixLength) == 0) {
    const std::wstring path = L"\"" + ExecutableDirectory() + L"\\" +
                              (href + kFilePrefixLength) + L"\"";
    ShellExecuteW(owner, L"open", L"notepad.exe", path.c_str(), nullptr,
                  SW_SHOWNORMAL);
    return;
  }
  ShellExecuteW(owner, L"open", href, nullptr, nullptr, SW_SHOWNORMAL);
}

bool CopyToClipboard(HWND owner, const std::wstring& text) {
  if (!OpenClipboard(owner)) {
    return false;
  }
  EmptyClipboard();
  bool copied = false;
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (memory != nullptr) {
    void* destination = GlobalLock(memory);
    if (destination != nullptr) {
      std::memcpy(destination, text.c_str(), bytes);
      GlobalUnlock(memory);
      copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    }
    if (!copied) {
      GlobalFree(memory);
    }
  }
  CloseClipboard();
  return copied;
}

// ---------------------------------------------------------------------------
// Settings.

NoiseSuppressionMode LoadNoiseMode() {
  const std::wstring saved = ReadSetting(kNoiseSetting);
  for (const NoiseModeOption& option : kNoiseModeOptions) {
    if (saved == std::to_wstring(static_cast<int>(option.mode))) {
      return option.mode;
    }
  }
  return kDefaultNoiseMode;
}

NoiseSuppressionMode SelectedNoiseMode(const AppState* state) {
  const LRESULT index = SendMessageW(state->noise_combo, CB_GETCURSEL, 0, 0);
  if (index >= 0 && static_cast<size_t>(index) < std::size(kNoiseModeOptions)) {
    return kNoiseModeOptions[static_cast<size_t>(index)].mode;
  }
  return kDefaultNoiseMode;
}

bool EchoCancellationChecked(const AppState* state) {
  return SendMessageW(state->echo_cancellation_check, BM_GETCHECK, 0, 0) ==
         BST_CHECKED;
}

bool NeuralModelSelected(const AppState* state) {
  return SendMessageW(state->echo_mode_combo, CB_GETCURSEL, 0, 0) !=
         kEchoModeClassicIndex;
}

const AudioDeviceInfo* SelectedDevice(
    HWND combo,
    const std::vector<AudioDeviceInfo>& devices) {
  const LRESULT index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
  if (index == CB_ERR || static_cast<size_t>(index) >= devices.size()) {
    return nullptr;
  }
  return &devices[static_cast<size_t>(index)];
}

const AudioDeviceInfo* FindDevice(const std::vector<AudioDeviceInfo>& devices,
                                  const std::wstring& id) {
  for (const AudioDeviceInfo& device : devices) {
    if (device.id == id) {
      return &device;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Tray icon.

std::wstring TrayTip(const AppState* state) {
  switch (state->status) {
    case UiStatus::kRunning:
      return std::wstring(kAppName) + L" \u2014 Running";
    case UiStatus::kRestarting:
      return std::wstring(kAppName) + L" \u2014 Restarting\u2026";
    case UiStatus::kError:
      return std::wstring(kAppName) + L" \u2014 Stopped (error)";
    case UiStatus::kStopped:
      break;
  }
  return std::wstring(kAppName) + L" \u2014 Stopped";
}

void AddTrayIcon(AppState* state) {
  if (state->tray_icon_added) {
    return;
  }
  NOTIFYICONDATAW icon = {};
  icon.cbSize = sizeof(icon);
  icon.hWnd = state->window;
  icon.uID = kTrayIconId;
  icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  icon.uCallbackMessage = kTrayMessage;
  icon.hIcon =
      reinterpret_cast<HICON>(GetClassLongPtrW(state->window, GCLP_HICONSM));
  wcsncpy_s(icon.szTip, TrayTip(state).c_str(), _TRUNCATE);
  state->tray_icon_added = Shell_NotifyIconW(NIM_ADD, &icon) == TRUE;
}

void UpdateTrayTip(AppState* state) {
  if (!state->tray_icon_added) {
    return;
  }
  NOTIFYICONDATAW icon = {};
  icon.cbSize = sizeof(icon);
  icon.hWnd = state->window;
  icon.uID = kTrayIconId;
  icon.uFlags = NIF_TIP;
  wcsncpy_s(icon.szTip, TrayTip(state).c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &icon);
}

void ShowTrayBalloon(AppState* state,
                     const wchar_t* title,
                     const std::wstring& text) {
  if (!state->tray_icon_added) {
    return;
  }
  NOTIFYICONDATAW icon = {};
  icon.cbSize = sizeof(icon);
  icon.hWnd = state->window;
  icon.uID = kTrayIconId;
  icon.uFlags = NIF_INFO;
  icon.dwInfoFlags = NIIF_ERROR;
  wcsncpy_s(icon.szInfoTitle, title, _TRUNCATE);
  wcsncpy_s(icon.szInfo, text.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &icon);
}

void RemoveTrayIcon(AppState* state) {
  if (!state->tray_icon_added) {
    return;
  }
  NOTIFYICONDATAW icon = {};
  icon.cbSize = sizeof(icon);
  icon.hWnd = state->window;
  icon.uID = kTrayIconId;
  Shell_NotifyIconW(NIM_DELETE, &icon);
  state->tray_icon_added = false;
}

// ---------------------------------------------------------------------------
// Levels.

bool WindowShowing(const AppState* state) {
  return IsWindowVisible(state->window) && !IsIconic(state->window);
}

void ShowClipWarning(AppState* state, bool visible) {
  if (state->clip_warning_visible == visible) {
    return;
  }
  state->clip_warning_visible = visible;
  ShowWindow(state->clip_text, visible ? SW_SHOWNA : SW_HIDE);
  ShowWindow(state->clip_settings_button, visible ? SW_SHOWNA : SW_HIDE);
}

void ResetLevels(AppState* state) {
  SetLevelMeterValue(state->microphone_meter, -120.0f);
  SetLevelMeterValue(state->reference_meter, -120.0f);
  SetLevelMeterValue(state->output_meter, -120.0f);
  state->last_clip_tick = 0;
  ShowClipWarning(state, false);
}

void PollLevels(AppState* state) {
  const EngineLevels levels = state->engine.levels();
  SetLevelMeterValue(state->microphone_meter, levels.microphone_peak_dbfs);
  SetLevelMeterValue(state->reference_meter, levels.reference_peak_dbfs);
  SetLevelMeterValue(state->output_meter, levels.output_peak_dbfs);
  const ULONGLONG now = GetTickCount64();
  if (levels.microphone_clipping) {
    state->last_clip_tick = now;
  }
  ShowClipWarning(state, state->last_clip_tick != 0 &&
                             now - state->last_clip_tick < kClipWarningHoldMs);
}

// Meters update only while the window is on screen and the engine runs.
void UpdateLevelTimer(AppState* state, bool window_showing) {
  const bool wanted = window_showing && state->engine.running();
  if (wanted && !state->level_timer_active) {
    state->level_timer_active =
        SetTimer(state->window, kLevelTimer, kLevelTimerMs, nullptr) != 0;
    PollLevels(state);
  } else if (!wanted && state->level_timer_active) {
    KillTimer(state->window, kLevelTimer);
    state->level_timer_active = false;
  }
  if (!state->engine.running()) {
    ResetLevels(state);
  }
}

void ShowFromTray(AppState* state) {
  ShowWindow(state->window, IsIconic(state->window) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(state->window);
  UpdateLevelTimer(state, true);
}

void HideToTray(AppState* state) {
  ShowWindow(state->window, SW_HIDE);
  UpdateLevelTimer(state, false);
}

// ---------------------------------------------------------------------------
// Status texts.

std::wstring ModelStatusText(const AppState* state) {
  if (!NeuralModelSelected(state)) {
    return L"Classic WebRTC AEC3 selected \u2014 the Chrome model is not "
           L"used.";
  }
  if (state->engine.running()) {
    if (!EchoCancellationChecked(state)) {
      return L"Neural selected \u2014 not in use while echo cancellation is "
             L"off.";
    }
    if (state->engine.neural_echo_estimator_active()) {
      return L"Neural active (Chrome model).";
    }
    if (EchoModelInstalled()) {
      return L"Neural selected, but the Chrome model could not be loaded "
             L"\u2014 using Classic. Try Import from Chrome.";
    }
    return L"Neural selected but model not found \u2014 using Classic "
           L"(Import from Chrome).";
  }
  if (EchoModelInstalled()) {
    return L"Neural selected \u2014 Chrome model imported; it loads when "
           L"processing starts.";
  }
  return L"Neural selected \u2014 model not imported yet. Start tries to "
         L"import it from Chrome; otherwise Classic is used.";
}

void UpdateModelStatus(AppState* state) {
  SetWindowTextW(state->model_status, ModelStatusText(state).c_str());
}

std::wstring StoppedDetail(const AppState* state) {
  if (!state->device_error.empty()) {
    return state->device_error;
  }
  if (state->cable.kind == VirtualCableKind::kNone) {
    return L"No virtual cable found \u2014 see Devices. Processing needs one "
           L"to pass your voice to other apps.";
  }
  if (state->microphones.empty()) {
    return L"No microphone found. Connect or enable one in Windows.";
  }
  if (state->speakers.empty()) {
    return L"No speakers found. Connect or enable an output device in "
           L"Windows.";
  }
  return L"Click Start to send your echo-free voice to " +
         state->cable.render_name + L".";
}

std::wstring RunningDetail(const AppState* state) {
  return L"Your echo-free voice is playing into " + state->cable.render_name +
         L". Select the device below as your microphone in other apps.";
}

void SetStatus(AppState* state, UiStatus status, const std::wstring& detail) {
  state->status = status;
  const wchar_t* title = L"Stopped";
  switch (status) {
    case UiStatus::kRunning:
      title = L"Running";
      break;
    case UiStatus::kRestarting:
      title = L"Restarting\u2026";
      break;
    case UiStatus::kError:
      title = L"Error";
      break;
    case UiStatus::kStopped:
      break;
  }
  SetWindowTextW(state->status_text, title);
  SetWindowTextW(state->status_detail, detail.c_str());
  const bool active =
      status == UiStatus::kRunning || status == UiStatus::kRestarting;
  SetWindowTextW(state->start_stop_button, active ? L"Stop" : L"Start");
  InvalidateRect(state->status_text, nullptr, TRUE);
  UpdateModelStatus(state);
  UpdateTrayTip(state);
}

// Refreshes the idle status line (e.g. after device changes) without hiding
// an error the user hasn't acted on yet.
void UpdateIdleStatus(AppState* state) {
  if (state->status == UiStatus::kStopped) {
    SetStatus(state, UiStatus::kStopped, StoppedDetail(state));
  }
}

void UpdateCableUi(AppState* state) {
  std::wstring text;
  std::wstring link;
  switch (state->cable.kind) {
    case VirtualCableKind::kVbCable:
      text = L"VB-CABLE detected. Your voice is played into " +
             state->cable.render_name + L".";
      link = L"VB-CABLE is VB-Audio donationware \u2014 <a "
             L"href=\"https://www.vb-cable.com\">www.vb-cable.com</a>";
      break;
    case VirtualCableKind::kVacLite:
      text = L"VAC Lite detected. Your voice is played into " +
             state->cable.render_name + L".";
      break;
    case VirtualCableKind::kNone:
      text = L"No virtual cable found. Other apps hear this app through one; "
             L"the installer sets up VB-CABLE, or get it from VB-Audio:";
      link = std::wstring(L"<a href=\"") + kVbCableUrl +
             L"\">Download VB-CABLE (vb-audio.com/Cable)</a>";
      break;
  }
  SetWindowTextW(state->cable_text, text.c_str());
  InvalidateRect(state->cable_text, nullptr, TRUE);
  SetWindowTextW(state->cable_link, link.c_str());
  ShowWindow(state->cable_link, link.empty() ? SW_HIDE : SW_SHOWNA);

  const std::wstring discord_name =
      state->cable.kind == VirtualCableKind::kNone
          ? std::wstring(L"(no virtual cable installed)")
          : state->cable.capture_name;
  SetWindowTextW(state->discord_name, discord_name.c_str());
}

// ---------------------------------------------------------------------------
// Engine control.

bool StartAudio(AppState* state) {
  if (!state->device_error.empty()) {
    SetStatus(state, UiStatus::kError, state->device_error);
    return false;
  }
  if (state->cable.kind == VirtualCableKind::kNone) {
    SetStatus(state, UiStatus::kError,
              L"No virtual cable found. Install VB-CABLE (see Devices), then "
              L"click Start.");
    return false;
  }
  const AudioDeviceInfo* microphone =
      SelectedDevice(state->microphone_combo, state->microphones);
  if (microphone == nullptr) {
    SetStatus(state, UiStatus::kError,
              L"Select your microphone under Devices.");
    return false;
  }
  const AudioDeviceInfo* speakers =
      SelectedDevice(state->speakers_combo, state->speakers);
  if (speakers == nullptr) {
    SetStatus(state, UiStatus::kError,
              L"Select the speakers you listen on under Devices.");
    return false;
  }

  EngineConfig config;
  config.microphone_id = microphone->id;
  config.echo_output_id = speakers->id;
  config.cable_render_id = state->cable.render_id;
  config.noise_mode = SelectedNoiseMode(state);
  config.echo_cancellation = EchoCancellationChecked(state);
  config.use_neural_echo_model = NeuralModelSelected(state);

  const WPARAM session = ++state->engine_session;
  std::wstring error;
  HCURSOR previous_cursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
  const bool started = state->engine.Start(
      config,
      [window = state->window, session](std::wstring message) {
        auto* owned_message = new std::wstring(std::move(message));
        if (!PostMessageW(window, kEngineErrorMessage, session,
                          reinterpret_cast<LPARAM>(owned_message))) {
          delete owned_message;
        }
      },
      &error);
  SetCursor(previous_cursor);
  if (!started) {
    state->engine.Stop();
    SetStatus(state, UiStatus::kError,
              error.empty() ? std::wstring(L"Audio processing failed to start.")
                            : error);
    UpdateLevelTimer(state, WindowShowing(state));
    return false;
  }

  state->running_config = std::move(config);
  state->running_microphone_name = microphone->name;
  state->running_speakers_name = speakers->name;
  SetStatus(state, UiStatus::kRunning, RunningDetail(state));
  UpdateLevelTimer(state, WindowShowing(state));
  return true;
}

void StopAudio(AppState* state) {
  ++state->engine_session;
  state->engine.Stop();
  SetStatus(state, UiStatus::kStopped, StoppedDetail(state));
  UpdateLevelTimer(state, WindowShowing(state));
}

void StopWithError(AppState* state, const std::wstring& message) {
  ++state->engine_session;
  state->engine.Stop();
  SetStatus(state, UiStatus::kError, message);
  UpdateLevelTimer(state, WindowShowing(state));
  MessageBeep(MB_ICONERROR);
  if (!IsWindowVisible(state->window)) {
    ShowTrayBalloon(state, L"AEC3 Virtual Mic stopped", message);
  }
}

// Applies settings that only take effect at Start. No-op while stopped.
void RestartAudio(AppState* state, const wchar_t* reason) {
  if (!state->engine.running()) {
    return;
  }
  SetStatus(state, UiStatus::kRestarting, reason);
  RedrawWindow(state->window, nullptr, nullptr,
               RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
  ++state->engine_session;
  state->engine.Stop();
  StartAudio(state);
}

void ToggleAudio(AppState* state) {
  if (state->engine.running()) {
    StopAudio(state);
  } else {
    StartAudio(state);
  }
}

// ---------------------------------------------------------------------------
// Devices.

// Selects `wanted_id`, else `fallback_id`, else the first entry.
void PopulateDeviceCombo(HWND combo,
                         const std::vector<AudioDeviceInfo>& devices,
                         const std::wstring& wanted_id,
                         const std::wstring& fallback_id) {
  SendMessageW(combo, WM_SETREDRAW, FALSE, 0);
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  int selection = CB_ERR;
  int fallback = CB_ERR;
  for (size_t index = 0; index < devices.size(); ++index) {
    const AudioDeviceInfo& device = devices[index];
    SendMessageW(combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(device.name.c_str()));
    if (selection == CB_ERR && !wanted_id.empty() && device.id == wanted_id) {
      selection = static_cast<int>(index);
    }
    if (fallback == CB_ERR && !fallback_id.empty() &&
        device.id == fallback_id) {
      fallback = static_cast<int>(index);
    }
  }
  if (selection == CB_ERR) {
    selection = fallback != CB_ERR ? fallback : (devices.empty() ? CB_ERR : 0);
  }
  if (selection != CB_ERR) {
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selection), 0);
  }
  SendMessageW(combo, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(combo, nullptr, TRUE);
}

void RefreshDevices(AppState* state) {
  std::wstring capture_error;
  std::vector<AudioDeviceInfo> capture =
      EnumerateAudioDevices(eCapture, &capture_error);
  std::wstring render_error;
  std::vector<AudioDeviceInfo> render =
      EnumerateAudioDevices(eRender, &render_error);
  state->device_error = !capture_error.empty() ? capture_error : render_error;
  state->cable = FindVirtualCable(render, capture);

  state->microphones.clear();
  for (AudioDeviceInfo& device : capture) {
    if (state->cable.kind != VirtualCableKind::kNone &&
        device.name == state->cable.capture_name) {
      continue;  // The cable's own capture side would feed back into itself.
    }
    state->microphones.push_back(std::move(device));
  }
  state->speakers.clear();
  for (AudioDeviceInfo& device : render) {
    if (state->cable.kind != VirtualCableKind::kNone &&
        device.id == state->cable.render_id) {
      continue;  // The cable is where the app plays, not what the user hears.
    }
    state->speakers.push_back(std::move(device));
  }

  const bool running = state->engine.running();
  PopulateDeviceCombo(state->microphone_combo, state->microphones,
                      running ? state->running_config.microphone_id
                              : state->preferred_microphone_id,
                      GetDefaultAudioDeviceId(eCapture));
  PopulateDeviceCombo(state->speakers_combo, state->speakers,
                      running ? state->running_config.echo_output_id
                              : state->preferred_speakers_id,
                      GetDefaultAudioDeviceId(eRender));
  UpdateCableUi(state);

  if (!running) {
    UpdateIdleStatus(state);
    return;
  }
  if (!state->device_error.empty()) {
    StopWithError(state, state->device_error);
    return;
  }
  if (FindDevice(state->microphones, state->running_config.microphone_id) ==
      nullptr) {
    StopWithError(state, L"The microphone \"" +
                             state->running_microphone_name +
                             L"\" was disconnected or disabled.");
    return;
  }
  if (FindDevice(state->speakers, state->running_config.echo_output_id) ==
      nullptr) {
    StopWithError(state, L"The speakers \"" + state->running_speakers_name +
                             L"\" were disconnected or disabled.");
    return;
  }
  if (state->cable.kind == VirtualCableKind::kNone) {
    StopWithError(state,
                  L"The virtual cable was removed or disabled. Your voice can "
                  L"no longer reach other apps.");
    return;
  }
  if (state->cable.render_id != state->running_config.cable_render_id) {
    // A preferred cable appeared (e.g. VB-CABLE next to VAC Lite) or the old
    // one vanished: follow what the UI shows.
    RestartAudio(state, L"Switching to the detected virtual cable\u2026");
  }
}

void OnDeviceSelectionChanged(AppState* state, bool microphone) {
  HWND combo = microphone ? state->microphone_combo : state->speakers_combo;
  const AudioDeviceInfo* device = SelectedDevice(
      combo, microphone ? state->microphones : state->speakers);
  if (device == nullptr) {
    return;
  }
  if (microphone) {
    state->preferred_microphone_id = device->id;
    WriteSetting(kMicrophoneSetting, device->id);
  } else {
    state->preferred_speakers_id = device->id;
    WriteSetting(kSpeakersSetting, device->id);
  }
  if (!state->engine.running()) {
    return;
  }
  const std::wstring& running_id = microphone
                                       ? state->running_config.microphone_id
                                       : state->running_config.echo_output_id;
  if (device->id != running_id) {
    RestartAudio(state, microphone ? L"Switching microphone\u2026"
                                   : L"Switching speakers\u2026");
  }
}

// ---------------------------------------------------------------------------
// Dialogs.

void ShowMessage(AppState* state,
                 PCWSTR icon,
                 const wchar_t* instruction,
                 const std::wstring& content) {
  TaskDialog(state->window, state->instance, kAppName, instruction,
             content.c_str(), TDCBF_OK_BUTTON, icon, nullptr);
}

void ImportModel(AppState* state) {
  std::wstring error;
  HCURSOR previous_cursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
  const bool imported = ImportEchoModelFromChrome(&error);
  SetCursor(previous_cursor);
  if (!imported) {
    UpdateModelStatus(state);
    ShowMessage(
        state, TD_WARNING_ICON, L"The Chrome model could not be imported",
        (error.empty() ? std::wstring(L"Chrome's model was not found.")
                       : error) +
            L"\n\nChrome downloads this model by itself for its own echo "
            L"cancellation; it may be missing if Chrome isn't installed or "
            L"hasn't downloaded it yet. Until then the app uses Classic "
            L"WebRTC AEC3, which works without it.");
    return;
  }

  if (!NeuralModelSelected(state)) {
    UpdateModelStatus(state);
    ShowMessage(state, TD_INFORMATION_ICON, L"Chrome model imported",
                L"Select \"Neural (Chrome model)\" under Echo suppression to "
                L"use it.");
    return;
  }
  if (!state->engine.running()) {
    UpdateModelStatus(state);
    ShowMessage(state, TD_INFORMATION_ICON, L"Chrome model imported",
                L"It will be used when processing starts.");
    return;
  }
  RestartAudio(state, L"Loading the imported model\u2026");
  if (!state->engine.running()) {
    return;  // The status line shows why the restart failed.
  }
  if (state->engine.neural_echo_estimator_active()) {
    ShowMessage(state, TD_INFORMATION_ICON, L"Chrome model imported",
                L"Processing restarted and the neural echo model is active.");
  } else {
    ShowMessage(state, TD_WARNING_ICON,
                L"Chrome model imported, but it could not be loaded",
                L"The copied file may come from an incompatible Chrome "
                L"version. Processing continues with Classic WebRTC AEC3.");
  }
}

HRESULT CALLBACK AboutCallback(HWND dialog,
                               UINT notification,
                               WPARAM,
                               LPARAM lparam,
                               LONG_PTR) {
  if (notification == TDN_HYPERLINK_CLICKED) {
    OpenLink(dialog, reinterpret_cast<const wchar_t*>(lparam));
  }
  return S_OK;
}

// Returns an "<a href="file:NAME">label</a>" link for the first existing
// candidate next to the executable, or the plain label.
std::wstring LocalFileLink(std::initializer_list<const wchar_t*> candidates,
                           const wchar_t* label) {
  const std::wstring directory = ExecutableDirectory();
  for (const wchar_t* name : candidates) {
    if (FileExists(directory + L"\\" + name)) {
      return std::wstring(L"<a href=\"file:") + name + L"\">" + label +
             L"</a>";
    }
  }
  return label;
}

void ShowAbout(AppState* state) {
  const std::wstring instruction =
      std::wstring(kAppName) + L" " + kAppVersion;
  const std::wstring content =
      L"Echo-cancelled virtual microphone for Windows.\n\n"
      L"Copyright (c) 2026 AEC3VirtualMic contributors.\nReleased under the " +
      LocalFileLink({L"LICENSE.txt", L"LICENSE"}, L"MIT License") +
      L".\n\nUses WebRTC (BSD-3-Clause), RNNoise (BSD-3-Clause) and "
      L"TensorFlow Lite (Apache-2.0) \u2014 see " +
      LocalFileLink({L"THIRD_PARTY_NOTICES.txt", L"THIRD_PARTY_NOTICES.md"},
                    L"third-party notices") +
      L".\n\nVB-CABLE is VB-Audio donationware \u2014 "
      L"<a href=\"https://www.vb-cable.com\">www.vb-cable.com</a> \u2014 all "
      L"participations are welcome.";
  const wchar_t footer[] =
      L"Neural echo model: Google's unreleased machine-learning residual echo "
      L"estimator, which Chrome downloads for its own echo cancellation. This "
      L"app only copies that file from your local Chrome profile \u2014 "
      L"nothing is downloaded or uploaded. The model is not part of this "
      L"project, not covered by its MIT license, and may be missing until "
      L"Chrome has downloaded it; Classic WebRTC AEC3 works without it.";

  TASKDIALOGCONFIG config = {};
  config.cbSize = sizeof(config);
  config.hwndParent = state->window;
  config.hInstance = state->instance;
  config.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_POSITION_RELATIVE_TO_WINDOW |
                   TDF_ALLOW_DIALOG_CANCELLATION;
  config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
  config.pszWindowTitle = L"About AEC3 Virtual Mic";
  config.pszMainIcon = MAKEINTRESOURCEW(IDI_APP_ICON);
  config.pszMainInstruction = instruction.c_str();
  config.pszContent = content.c_str();
  config.pszFooterIcon = TD_INFORMATION_ICON;
  config.pszFooter = footer;
  config.pfCallback = AboutCallback;
  TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
}

void ShowTrayMenu(AppState* state) {
  POINT point = {};
  GetCursorPos(&point);
  HMENU menu = CreatePopupMenu();
  if (menu == nullptr) {
    return;
  }
  AppendMenuW(menu, MF_STRING, kTrayShow, L"Show");
  AppendMenuW(menu, MF_STRING, kTrayStartStop,
              state->engine.running() ? L"Stop processing"
                                      : L"Start processing");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kTrayExit, L"Exit");
  SetMenuDefaultItem(menu, kTrayShow, FALSE);
  SetForegroundWindow(state->window);
  const UINT command = TrackPopupMenu(
      menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y,
      0, state->window, nullptr);
  DestroyMenu(menu);
  if (command == kTrayShow) {
    ShowFromTray(state);
  } else if (command == kTrayStartStop) {
    ToggleAudio(state);
    if (!state->engine.running() && state->status == UiStatus::kError) {
      ShowFromTray(state);  // Show why it didn't start.
    }
  } else if (command == kTrayExit) {
    state->exit_requested = true;
    PostMessageW(state->window, WM_CLOSE, 0, 0);
  }
  PostMessageW(state->window, WM_NULL, 0, 0);
}

// ---------------------------------------------------------------------------
// Controls, fonts and layout.

HWND CreateChild(AppState* state,
                 const wchar_t* class_name,
                 const wchar_t* text,
                 DWORD style,
                 int id = 0,
                 DWORD ex_style = 0) {
  return CreateWindowExW(ex_style, class_name, text,
                         WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                         state->window,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                         state->instance, nullptr);
}

HWND CreateGroup(AppState* state, const wchar_t* text) {
  return CreateChild(state, WC_BUTTONW, text, BS_GROUPBOX);
}

HWND CreateLabel(AppState* state, const wchar_t* text, int id = 0) {
  return CreateChild(state, WC_STATICW, text, SS_LEFT | SS_NOTIFY, id);
}

HWND CreateCombo(AppState* state, int id) {
  return CreateChild(state, WC_COMBOBOXW, nullptr,
                     WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, id);
}

HWND CreateButton(AppState* state, const wchar_t* text, int id) {
  return CreateChild(state, WC_BUTTONW, text, WS_TABSTOP | BS_PUSHBUTTON, id);
}

HWND CreateCheckBox(AppState* state, const wchar_t* text, int id) {
  return CreateChild(state, WC_BUTTONW, text, WS_TABSTOP | BS_AUTOCHECKBOX,
                     id);
}

HWND CreateMeter(AppState* state, int id) {
  return CreateChild(state, kLevelMeterClass, nullptr, 0, id);
}

void CreateControls(AppState* state) {
  // Creation order is the tab order.
  state->status_group = CreateGroup(state, L"Status");
  state->status_text = CreateLabel(state, L"Stopped", kStatusText);
  state->start_stop_button = CreateButton(state, L"Start", kStartStopButton);
  state->status_detail = CreateLabel(state, L"", kStatusDetail);
  state->discord_label = CreateLabel(
      state, L"In Discord and other apps, select this microphone:");
  state->discord_name = CreateChild(
      state, WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
      kDiscordName, WS_EX_CLIENTEDGE);
  state->copy_name_button =
      CreateButton(state, L"Copy name", kCopyNameButton);

  state->devices_group = CreateGroup(state, L"Devices");
  state->microphone_label = CreateLabel(state, L"Microphone");
  state->microphone_combo = CreateCombo(state, kMicrophoneCombo);
  state->speakers_label = CreateLabel(state, L"Speakers (echo reference)");
  state->speakers_combo = CreateCombo(state, kSpeakersCombo);
  state->cable_text = CreateLabel(state, L"", kCableText);
  state->cable_link =
      CreateChild(state, WC_LINK, L"", WS_TABSTOP, kCableLink);

  state->processing_group = CreateGroup(state, L"Processing");
  state->echo_cancellation_check =
      CreateCheckBox(state, L"Echo cancellation", kEchoCancellationCheck);
  state->echo_mode_label = CreateLabel(state, L"Echo suppression");
  state->echo_mode_combo = CreateCombo(state, kEchoModeCombo);
  state->model_status = CreateLabel(state, L"", kModelStatus);
  state->import_model_button =
      CreateButton(state, L"Import from Chrome", kImportModelButton);
  state->noise_label = CreateLabel(state, L"Noise suppression");
  state->noise_combo = CreateCombo(state, kNoiseCombo);

  state->levels_group = CreateGroup(state, L"Levels");
  state->microphone_meter_label = CreateLabel(state, L"Microphone");
  state->microphone_meter = CreateMeter(state, kMicrophoneMeter);
  state->reference_meter_label = CreateLabel(state, L"Speakers (reference)");
  state->reference_meter = CreateMeter(state, kReferenceMeter);
  state->output_meter_label = CreateLabel(state, L"Output");
  state->output_meter = CreateMeter(state, kOutputMeter);
  state->clip_text = CreateLabel(
      state,
      L"Mic is clipping \u2014 lower its level in Windows to ~70%", kClipText);
  state->clip_settings_button =
      CreateButton(state, L"Open sound settings", kClipSettingsButton);
  ShowWindow(state->clip_text, SW_HIDE);
  ShowWindow(state->clip_settings_button, SW_HIDE);

  state->app_group = CreateGroup(state, L"App");
  state->autostart_check =
      CreateCheckBox(state, L"Start with Windows", kAutostartCheck);
  state->about_button = CreateButton(state, L"About", kAboutButton);

  for (const wchar_t* label : kEchoModeLabels) {
    SendMessageW(state->echo_mode_combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label));
  }
  for (const NoiseModeOption& option : kNoiseModeOptions) {
    SendMessageW(state->noise_combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(option.label));
  }
}

void AddTooltip(AppState* state, HWND control, const wchar_t* text) {
  TTTOOLINFOW tool = {};
  tool.cbSize = sizeof(tool);
  tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
  tool.hwnd = state->window;
  tool.uId = reinterpret_cast<UINT_PTR>(control);
  tool.lpszText = const_cast<wchar_t*>(text);
  SendMessageW(state->tooltip, TTM_ADDTOOLW, 0,
               reinterpret_cast<LPARAM>(&tool));
}

void CreateTooltips(AppState* state) {
  state->tooltip = CreateWindowExW(
      WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
      WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
      CW_USEDEFAULT, CW_USEDEFAULT, state->window, nullptr, state->instance,
      nullptr);
  if (state->tooltip == nullptr) {
    return;
  }
  SendMessageW(state->tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP,
               MAKELPARAM(30000, 0));
  const struct {
    HWND control;
    const wchar_t* text;
  } tools[] = {
      {state->status_text, kTipStatus},
      {state->status_detail, kTipStatus},
      {state->start_stop_button, kTipStartStop},
      {state->discord_label, kTipDiscordName},
      {state->discord_name, kTipDiscordName},
      {state->copy_name_button, kTipCopyName},
      {state->microphone_label, kTipMicrophone},
      {state->microphone_combo, kTipMicrophone},
      {state->speakers_label, kTipSpeakers},
      {state->speakers_combo, kTipSpeakers},
      {state->cable_text, kTipCable},
      {state->cable_link, kTipCableLink},
      {state->echo_cancellation_check, kTipEchoCancellation},
      {state->echo_mode_label, kTipEchoMode},
      {state->echo_mode_combo, kTipEchoMode},
      {state->model_status, kTipModelStatus},
      {state->import_model_button, kTipImportModel},
      {state->noise_label, kTipNoise},
      {state->noise_combo, kTipNoise},
      {state->microphone_meter_label, kTipMicrophoneMeter},
      {state->microphone_meter, kTipMicrophoneMeter},
      {state->reference_meter_label, kTipReferenceMeter},
      {state->reference_meter, kTipReferenceMeter},
      {state->output_meter_label, kTipOutputMeter},
      {state->output_meter, kTipOutputMeter},
      {state->clip_text, kTipClip},
      {state->clip_settings_button, kTipClipSettings},
      {state->autostart_check, kTipAutostart},
      {state->about_button, kTipAbout},
  };
  for (const auto& tool : tools) {
    AddTooltip(state, tool.control, tool.text);
  }
}

BOOL CALLBACK SetChildFont(HWND child, LPARAM font) {
  SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), FALSE);
  return TRUE;
}

// (Re)creates the fonts for the current DPI and applies them.
void ApplyFonts(AppState* state) {
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                  &metrics, 0, state->dpi)) {
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics,
                          0);
    metrics.lfMessageFont.lfHeight =
        MulDiv(metrics.lfMessageFont.lfHeight, static_cast<int>(state->dpi),
               static_cast<int>(GetDpiForSystem()));
  }
  LOGFONTW base = metrics.lfMessageFont;
  HFONT font = CreateFontIndirectW(&base);
  LOGFONTW large = base;
  large.lfHeight = MulDiv(base.lfHeight, 8, 5);
  large.lfWeight = FW_SEMIBOLD;
  HFONT status_font = CreateFontIndirectW(&large);

  EnumChildWindows(state->window, SetChildFont, reinterpret_cast<LPARAM>(font));
  SendMessageW(state->status_text, WM_SETFONT,
               reinterpret_cast<WPARAM>(status_font), FALSE);
  if (state->tooltip != nullptr) {
    SendMessageW(state->tooltip, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                 FALSE);
    SendMessageW(state->tooltip, TTM_SETMAXTIPWIDTH, 0, Scale(state, 380));
  }

  if (state->font != nullptr) {
    DeleteObject(state->font);
  }
  if (state->status_font != nullptr) {
    DeleteObject(state->status_font);
  }
  state->font = font;
  state->status_font = status_font;

  HDC dc = GetDC(state->window);
  HGDIOBJ old_font = SelectObject(dc, font);
  TEXTMETRICW text_metrics = {};
  GetTextMetricsW(dc, &text_metrics);
  state->text_height = text_metrics.tmHeight;
  SelectObject(dc, old_font);
  ReleaseDC(state->window, dc);
}

void Place(HWND control, int x, int y, int width, int height) {
  SetWindowPos(control, nullptr, x, y, width, height,
               SWP_NOZORDER | SWP_NOACTIVATE);
}

// Two columns: Status + Devices on the left, Processing + Levels + App on the
// right. Returns the client size.
SIZE Layout(AppState* state) {
  const auto s = [state](int value) { return Scale(state, value); };
  const int margin = s(12);
  const int padding = s(12);
  const int gap = s(8);
  const int small_gap = s(4);
  const int group_gap = s(10);
  const int column_width = s(372);
  const int text = state->text_height;
  const int control = std::max(s(25), text + s(10));
  const int label_width = s(128);
  const int combo_drop = s(220);

  struct Column {
    int x;
    int y;
    HWND last_group;
    int last_group_top;
  };
  int group_top = 0;
  const auto begin_group = [&](Column& column) {
    group_top = column.y;
    column.y += text + s(6);
  };
  const auto end_group = [&](Column& column, HWND group) {
    column.y += padding;
    Place(group, column.x, group_top, column_width, column.y - group_top);
    column.last_group = group;
    column.last_group_top = group_top;
    column.y += group_gap;
  };
  const auto label_row = [&](HWND label, int x, int y, int row_height) {
    Place(label, x, y + (row_height - text) / 2, label_width - gap, text);
  };

  // Left column.
  Column left = {margin, margin, nullptr, 0};
  {
    const int x = left.x + padding;
    const int w = column_width - 2 * padding;
    begin_group(left);
    HDC dc = GetDC(state->window);
    HGDIOBJ old_font = SelectObject(dc, state->status_font);
    TEXTMETRICW metrics = {};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, old_font);
    ReleaseDC(state->window, dc);
    const int status_height = metrics.tmHeight;
    const int button_width = s(112);
    const int button_height = control + s(6);
    const int row = std::max(status_height, button_height);
    Place(state->status_text, x, left.y + (row - status_height) / 2,
          w - button_width - gap, status_height);
    Place(state->start_stop_button, x + w - button_width,
          left.y + (row - button_height) / 2, button_width, button_height);
    left.y += row + small_gap;
    Place(state->status_detail, x, left.y, w, 3 * text);
    left.y += 3 * text + gap;
    Place(state->discord_label, x, left.y, w, text);
    left.y += text + small_gap;
    const int copy_width = s(96);
    Place(state->discord_name, x, left.y, w - copy_width - gap, control);
    Place(state->copy_name_button, x + w - copy_width, left.y, copy_width,
          control);
    left.y += control;
    end_group(left, state->status_group);

    begin_group(left);
    Place(state->microphone_label, x, left.y, w, text);
    left.y += text + s(3);
    Place(state->microphone_combo, x, left.y, w, combo_drop);
    left.y += control + gap;
    Place(state->speakers_label, x, left.y, w, text);
    left.y += text + s(3);
    Place(state->speakers_combo, x, left.y, w, combo_drop);
    left.y += control + gap;
    Place(state->cable_text, x, left.y, w, 2 * text);
    left.y += 2 * text + s(2);
    Place(state->cable_link, x, left.y, w, text + s(2));
    left.y += text + s(2);
    end_group(left, state->devices_group);
  }

  // Right column.
  Column right = {margin + column_width + margin, margin, nullptr, 0};
  {
    const int x = right.x + padding;
    const int w = column_width - 2 * padding;
    begin_group(right);
    Place(state->echo_cancellation_check, x, right.y, w, control);
    right.y += control + small_gap;
    label_row(state->echo_mode_label, x, right.y, control);
    Place(state->echo_mode_combo, x + label_width, right.y, w - label_width,
          combo_drop);
    right.y += control + gap;
    Place(state->model_status, x, right.y, w, 2 * text);
    right.y += 2 * text + small_gap;
    Place(state->import_model_button, x, right.y, s(150), control);
    right.y += control + gap;
    label_row(state->noise_label, x, right.y, control);
    Place(state->noise_combo, x + label_width, right.y, w - label_width,
          combo_drop);
    right.y += control;
    end_group(right, state->processing_group);

    begin_group(right);
    const int meter_height = s(22);
    const struct {
      HWND label;
      HWND meter;
    } meters[] = {
        {state->microphone_meter_label, state->microphone_meter},
        {state->reference_meter_label, state->reference_meter},
        {state->output_meter_label, state->output_meter},
    };
    for (const auto& meter : meters) {
      label_row(meter.label, x, right.y, meter_height);
      Place(meter.meter, x + label_width, right.y, w - label_width,
            meter_height);
      right.y += meter_height + small_gap;
    }
    right.y += small_gap;
    Place(state->clip_text, x, right.y, w, 2 * text);
    right.y += 2 * text;
    Place(state->clip_settings_button, x, right.y, s(160), control);
    right.y += control;
    end_group(right, state->levels_group);

    begin_group(right);
    const int about_width = s(96);
    Place(state->autostart_check, x, right.y, w - about_width - gap, control);
    Place(state->about_button, x + w - about_width, right.y, about_width,
          control);
    right.y += control;
    end_group(right, state->app_group);
  }

  // Stretch the shorter column's last group so both columns end together.
  const int bottom = std::max(left.y, right.y) - group_gap;
  Column& shorter = left.y < right.y ? left : right;
  Place(shorter.last_group, shorter.x, shorter.last_group_top, column_width,
        bottom - shorter.last_group_top);

  InvalidateRect(state->window, nullptr, TRUE);
  return SIZE{right.x + column_width + margin, bottom + margin};
}

void ResizeWindowToLayout(AppState* state, const RECT* suggested) {
  const SIZE client = Layout(state);
  RECT frame = {0, 0, client.cx, client.cy};
  AdjustWindowRectExForDpi(
      &frame, static_cast<DWORD>(GetWindowLongPtrW(state->window, GWL_STYLE)),
      FALSE,
      static_cast<DWORD>(GetWindowLongPtrW(state->window, GWL_EXSTYLE)),
      state->dpi);
  const int width = frame.right - frame.left;
  const int height = frame.bottom - frame.top;
  if (suggested != nullptr) {
    SetWindowPos(state->window, nullptr, suggested->left, suggested->top,
                 width, height, SWP_NOZORDER | SWP_NOACTIVATE);
  } else {
    SetWindowPos(state->window, nullptr, 0, 0, width, height,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

void LoadSettingsIntoControls(AppState* state) {
  state->preferred_microphone_id = ReadSetting(kMicrophoneSetting);
  state->preferred_speakers_id = ReadSetting(kSpeakersSetting);

  const bool echo_cancellation =
      ReadBoolSetting(kEchoCancellationSetting, true);
  SendMessageW(state->echo_cancellation_check, BM_SETCHECK,
               echo_cancellation ? BST_CHECKED : BST_UNCHECKED, 0);
  state->engine.SetEchoCancellation(echo_cancellation);

  SendMessageW(state->echo_mode_combo, CB_SETCURSEL,
               static_cast<WPARAM>(ReadBoolSetting(kNeuralModelSetting, true)
                                       ? kEchoModeNeuralIndex
                                       : kEchoModeClassicIndex),
               0);

  const NoiseSuppressionMode noise_mode = LoadNoiseMode();
  for (size_t index = 0; index < std::size(kNoiseModeOptions); ++index) {
    if (kNoiseModeOptions[index].mode == noise_mode) {
      SendMessageW(state->noise_combo, CB_SETCURSEL, index, 0);
    }
  }
  state->engine.SetNoiseSuppressionMode(noise_mode);

  SendMessageW(state->autostart_check, BM_SETCHECK,
               IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
}

COLORREF StatusColor(const AppState* state) {
  switch (state->status) {
    case UiStatus::kRunning:
      return RGB(16, 124, 16);
    case UiStatus::kRestarting:
      return RGB(157, 93, 0);
    case UiStatus::kError:
      return RGB(196, 43, 28);
    case UiStatus::kStopped:
      break;
  }
  return GetSysColor(COLOR_WINDOWTEXT);
}

LRESULT OnControlColorStatic(AppState* state, HDC dc, HWND control) {
  COLORREF color = GetSysColor(COLOR_WINDOWTEXT);
  if (!IsHighContrast()) {
    if (control == state->status_text) {
      color = StatusColor(state);
    } else if (control == state->clip_text ||
               (control == state->cable_text &&
                state->cable.kind == VirtualCableKind::kNone)) {
      color = RGB(196, 43, 28);
    } else if (control == state->status_detail ||
               control == state->model_status) {
      color = RGB(80, 80, 80);
    }
  }
  SetTextColor(dc, color);
  SetBkColor(dc, GetSysColor(COLOR_WINDOW));
  return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
}

// ---------------------------------------------------------------------------
// Command handling.

void OnCommand(AppState* state, int id, int code) {
  switch (id) {
    case kStartStopButton:
    case kTrayStartStop:
      if (code == BN_CLICKED) {
        ToggleAudio(state);
      }
      return;
    case kCopyNameButton:
      if (code == BN_CLICKED) {
        if (state->cable.kind != VirtualCableKind::kNone &&
            CopyToClipboard(state->window, state->cable.capture_name)) {
          SetWindowTextW(state->copy_name_button, L"Copied");
          SetTimer(state->window, kCopyFeedbackTimer, kCopyFeedbackMs,
                   nullptr);
        } else {
          MessageBeep(MB_ICONWARNING);
        }
      }
      return;
    case kMicrophoneCombo:
    case kSpeakersCombo:
      if (code == CBN_SELCHANGE) {
        OnDeviceSelectionChanged(state, id == kMicrophoneCombo);
      }
      return;
    case kEchoCancellationCheck:
      if (code == BN_CLICKED) {
        const bool enabled = EchoCancellationChecked(state);
        state->engine.SetEchoCancellation(enabled);
        WriteBoolSetting(kEchoCancellationSetting, enabled);
        UpdateModelStatus(state);
      }
      return;
    case kEchoModeCombo:
      if (code == CBN_SELCHANGE) {
        const bool neural = NeuralModelSelected(state);
        WriteBoolSetting(kNeuralModelSetting, neural);
        if (state->engine.running() &&
            neural != state->running_config.use_neural_echo_model) {
          RestartAudio(state, L"Switching echo suppression\u2026");
        } else {
          UpdateModelStatus(state);
        }
      }
      return;
    case kImportModelButton:
      if (code == BN_CLICKED) {
        ImportModel(state);
      }
      return;
    case kNoiseCombo:
      if (code == CBN_SELCHANGE) {
        const NoiseSuppressionMode mode = SelectedNoiseMode(state);
        state->engine.SetNoiseSuppressionMode(mode);
        WriteSetting(kNoiseSetting, std::to_wstring(static_cast<int>(mode)));
      }
      return;
    case kClipSettingsButton:
      if (code == BN_CLICKED) {
        OpenLink(state->window, L"ms-settings:sound");
      }
      return;
    case kAutostartCheck:
      if (code == BN_CLICKED) {
        const bool enabled =
            SendMessageW(state->autostart_check, BM_GETCHECK, 0, 0) ==
            BST_CHECKED;
        if (!SetAutostartEnabled(enabled)) {
          SendMessageW(state->autostart_check, BM_SETCHECK,
                       IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
          ShowMessage(state, TD_ERROR_ICON,
                      L"Could not change \"Start with Windows\"",
                      L"Windows did not allow updating the startup entry for "
                      L"your account.");
        }
      }
      return;
    case kAboutButton:
      if (code == BN_CLICKED) {
        ShowAbout(state);
      }
      return;
    case kTrayShow:
      ShowFromTray(state);
      return;
    case kTrayExit:
      state->exit_requested = true;
      PostMessageW(state->window, WM_CLOSE, 0, 0);
      return;
    case IDCANCEL:  // Esc (via IsDialogMessage) hides to the tray.
      HideToTray(state);
      return;
  }
}

LRESULT CALLBACK WindowProcedure(HWND window,
                                 UINT message,
                                 WPARAM wparam,
                                 LPARAM lparam) {
  auto* state =
      reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    state = static_cast<AppState*>(create->lpCreateParams);
    state->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(state));
  }
  if (state == nullptr) {
    return DefWindowProcW(window, message, wparam, lparam);
  }
  if (state->taskbar_created_message != 0 &&
      message == state->taskbar_created_message) {
    // Explorer restarted: the tray icon must be added again.
    state->tray_icon_added = false;
    AddTrayIcon(state);
    return 0;
  }

  switch (message) {
    case WM_CREATE:
      state->dpi = GetDpiForWindow(window);
      CreateControls(state);
      CreateTooltips(state);
      ApplyFonts(state);
      LoadSettingsIntoControls(state);
      ResizeWindowToLayout(state, nullptr);
      RefreshDevices(state);
      state->device_notifier.Start(window, kDevicesChangedMessage);
      return 0;
    case WM_DPICHANGED:
      state->dpi = HIWORD(wparam);
      ApplyFonts(state);
      ResizeWindowToLayout(state, reinterpret_cast<const RECT*>(lparam));
      return 0;
    case WM_SETTINGCHANGE:
      if (wparam == SPI_SETNONCLIENTMETRICS) {
        ApplyFonts(state);
        ResizeWindowToLayout(state, nullptr);
      }
      break;
    case WM_CTLCOLORSTATIC:
      return OnControlColorStatic(state, reinterpret_cast<HDC>(wparam),
                                  reinterpret_cast<HWND>(lparam));
    case WM_COMMAND:
      OnCommand(state, LOWORD(wparam), HIWORD(wparam));
      return 0;
    case WM_NOTIFY: {
      const auto* header = reinterpret_cast<const NMHDR*>(lparam);
      if (header->idFrom == kCableLink &&
          (header->code == NM_CLICK || header->code == NM_RETURN)) {
        const auto* link = reinterpret_cast<const NMLINK*>(lparam);
        OpenLink(window, link->item.szUrl);
        return 0;
      }
      break;
    }
    case WM_TIMER:
      if (wparam == kLevelTimer) {
        PollLevels(state);
      } else if (wparam == kDeviceRefreshTimer) {
        KillTimer(window, kDeviceRefreshTimer);
        RefreshDevices(state);
      } else if (wparam == kCopyFeedbackTimer) {
        KillTimer(window, kCopyFeedbackTimer);
        SetWindowTextW(state->copy_name_button, L"Copy name");
      }
      return 0;
    case WM_SHOWWINDOW:
      UpdateLevelTimer(state, wparam != FALSE && !IsIconic(window));
      break;
    case WM_SIZE:
      UpdateLevelTimer(state,
                       wparam != SIZE_MINIMIZED && IsWindowVisible(window));
      return 0;
    case kDevicesChangedMessage:
      // Restarting the timer coalesces a burst of endpoint notifications.
      SetTimer(window, kDeviceRefreshTimer, kDeviceRefreshDelayMs, nullptr);
      return 0;
    case kActivateMessage:
      ShowFromTray(state);
      return 0;
    case kTrayMessage:
      if (lparam == WM_LBUTTONDBLCLK) {
        ShowFromTray(state);
      } else if (lparam == WM_RBUTTONUP) {
        ShowTrayMenu(state);
      }
      return 0;
    case kEngineErrorMessage: {
      std::unique_ptr<std::wstring> error(
          reinterpret_cast<std::wstring*>(lparam));
      if (wparam == state->engine_session) {
        StopWithError(state, *error);
      }
      return 0;
    }
    case WM_ENDSESSION:
      // Sign-out, shutdown, or Restart Manager (installer upgrade): exit.
      if (wparam != FALSE) {
        state->exit_requested = true;
        DestroyWindow(window);
      }
      return 0;
    case WM_CLOSE:
      if (!state->exit_requested) {
        HideToTray(state);
        return 0;
      }
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      state->device_notifier.Stop();
      KillTimer(window, kLevelTimer);
      KillTimer(window, kDeviceRefreshTimer);
      KillTimer(window, kCopyFeedbackTimer);
      ++state->engine_session;
      state->engine.Stop();
      RemoveTrayIcon(state);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

// A second normal launch brings the running instance to the front.
void ActivateExistingInstance() {
  for (int attempt = 0; attempt < 30; ++attempt) {
    HWND existing = FindWindowW(kWindowClass, nullptr);
    if (existing != nullptr) {
      DWORD process_id = 0;
      GetWindowThreadProcessId(existing, &process_id);
      AllowSetForegroundWindow(process_id);
      PostMessageW(existing, kActivateMessage, 0, 0);
      return;
    }
    Sleep(100);  // The first instance may still be creating its window.
  }
}

struct CommandLine {
  bool startup = false;
  bool enable_autostart = false;
  bool configure_vbcable = false;
  bool snapshot_audio_defaults = false;
};

CommandLine ParseCommandLine() {
  CommandLine result;
  int count = 0;
  LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  if (arguments == nullptr) {
    return result;
  }
  for (int index = 1; index < count; ++index) {
    if (_wcsicmp(arguments[index], L"--startup") == 0) {
      result.startup = true;
    } else if (_wcsicmp(arguments[index], L"--enable-autostart") == 0) {
      result.enable_autostart = true;
    } else if (_wcsicmp(arguments[index], L"--configure-vbcable") == 0) {
      result.configure_vbcable = true;
    } else if (_wcsicmp(arguments[index], L"--snapshot-audio-defaults") ==
               0) {
      result.snapshot_audio_defaults = true;
    }
  }
  LocalFree(arguments);
  return result;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  const CommandLine command_line = ParseCommandLine();
  // Installer helpers: no UI, no single-instance mutex, no window.
  if (command_line.configure_vbcable) {
    return ConfigureFreshVbCable();
  }
  if (command_line.snapshot_audio_defaults) {
    return SnapshotAudioDefaults();
  }
  if (command_line.enable_autostart) {
    return SetAutostartEnabled(true) ? 0 : 1;
  }

  HANDLE instance_mutex = CreateMutexW(nullptr, FALSE, kInstanceMutex);
  if (instance_mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (!command_line.startup) {
      ActivateExistingInstance();
    }
    CloseHandle(instance_mutex);
    return 0;
  }

  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com_result)) {
    MessageBoxW(nullptr, L"COM initialization failed.", kAppName,
                MB_OK | MB_ICONERROR);
    if (instance_mutex != nullptr) {
      CloseHandle(instance_mutex);
    }
    return 1;
  }

  INITCOMMONCONTROLSEX controls = {sizeof(controls),
                                   ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES |
                                       ICC_LINK_CLASS};
  InitCommonControlsEx(&controls);
  RegisterLevelMeterClass(instance);

  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = WindowProcedure;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hIcon = static_cast<HICON>(LoadImageW(
      instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
  window_class.hIconSm = static_cast<HICON>(LoadImageW(
      instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kWindowClass;
  if (RegisterClassExW(&window_class) == 0) {
    CoUninitialize();
    if (instance_mutex != nullptr) {
      CloseHandle(instance_mutex);
    }
    return 1;
  }

  auto state = std::make_unique<AppState>();
  state->instance = instance;
  state->taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
  // The final size is computed from the DPI-scaled layout in WM_CREATE.
  HWND window = CreateWindowExW(
      0, kWindowClass, kAppName,
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
      CW_USEDEFAULT, 800, 480, nullptr, nullptr, instance, state.get());
  if (window == nullptr) {
    CoUninitialize();
    if (instance_mutex != nullptr) {
      CloseHandle(instance_mutex);
    }
    return 1;
  }
  AddTrayIcon(state.get());

  if (command_line.startup) {
    // Autostart: process in the background; only surface the window if
    // processing could not start.
    if (!StartAudio(state.get())) {
      ShowWindow(window, SW_SHOWNORMAL);
    }
  } else {
    ShowWindow(window, show_command);
  }
  UpdateWindow(window);

  MSG message = {};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (IsDialogMessageW(window, &message)) {
      continue;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  state.reset();
  CoUninitialize();
  if (instance_mutex != nullptr) {
    CloseHandle(instance_mutex);
  }
  return static_cast<int>(message.wParam);
}
