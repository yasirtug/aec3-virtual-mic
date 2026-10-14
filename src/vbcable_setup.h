#ifndef AEC3_VIRTUAL_MIC_VBCABLE_SETUP_H_
#define AEC3_VIRTUAL_MIC_VBCABLE_SETUP_H_

// Installer helpers: run without UI and return a process exit code.
// SnapshotAudioDefaults() and ConfigureFreshVbCable() run elevated and ONLY
// when the installer freshly installs VB-CABLE (never for a pre-existing one).

// `--detect-cables`: exit code is a bitmask of the active virtual cables
// (kDetectedVbCable | kDetectedVacLite), detected exactly like the app does.
constexpr int kDetectedVbCable = 1;
constexpr int kDetectedVacLite = 2;
int DetectVirtualCables();

// `--snapshot-audio-defaults`: runs right before the VB-CABLE setup. Records
// the default playback/recording endpoint of every role so that
// ConfigureFreshVbCable() can undo Windows switching its defaults to the new
// cable.
int SnapshotAudioDefaults();

// `--configure-vbcable`: runs right after the VB-CABLE setup. Applies the
// recommended VB-CABLE settings (48 kHz internal rate, reduced pipe latency,
// 48 kHz endpoint formats), restores the snapshotted default endpoints if a
// CABLE endpoint took them over, and deletes the snapshot.
int ConfigureFreshVbCable();

#endif  // AEC3_VIRTUAL_MIC_VBCABLE_SETUP_H_
