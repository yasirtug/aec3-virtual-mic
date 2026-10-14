#ifndef AEC3_VIRTUAL_MIC_VBCABLE_SETUP_H_
#define AEC3_VIRTUAL_MIC_VBCABLE_SETUP_H_

// Both functions run elevated, without UI, and are invoked ONLY by the
// installer when it freshly installs VB-CABLE (never for a pre-existing one).
// They return a process exit code (0 = success).

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
