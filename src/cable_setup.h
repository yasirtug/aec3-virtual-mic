#ifndef AEC3_VIRTUAL_MIC_CABLE_SETUP_H_
#define AEC3_VIRTUAL_MIC_CABLE_SETUP_H_

// Installer helpers: run without UI and return a process exit code.

// `--detect-cables`: exit code is a bitmask of the active virtual cables
// (kDetectedVbCable | kDetectedVacLite), detected exactly like the app does.
constexpr int kDetectedVbCable = 1;
constexpr int kDetectedVacLite = 2;
int DetectVirtualCables();

// `--snapshot-audio-defaults` (elevated): runs right before the installer
// installs a virtual cable. Records the default playback/recording endpoint
// of every role, because Windows may make the new cable's endpoints the
// defaults.
int SnapshotAudioDefaults();

// `--restore-audio-defaults` (elevated): runs right after the cable is
// installed. Gives every role whose default is now a virtual cable endpoint
// back to the snapshotted endpoint, then deletes the snapshot.
int RestoreAudioDefaults();

#endif  // AEC3_VIRTUAL_MIC_CABLE_SETUP_H_
