#ifndef AEC3_VIRTUAL_MIC_ECHO_MODEL_H_
#define AEC3_VIRTUAL_MIC_ECHO_MODEL_H_

#include <string>

// The neural echo model is Google's unreleased ML residual echo estimator that
// Chrome downloads (optimization guide target 71) for its own echo
// cancellation. It is not part of this project and not covered by its license:
// this app only copies the file from the user's local Chrome profile, without
// any network access. It may be absent if Chrome has not downloaded it yet.

// Per-user copy: %LOCALAPPDATA%\AEC3VirtualMic\residual_echo_estimator.tflite
// Empty if the local application data folder cannot be resolved.
std::wstring EchoModelPath();

bool EchoModelInstalled();

// Copies the newest model.tflite from local Chrome profiles (Stable, Beta, Dev,
// Canary; optimization_guide_model_store\71\*\*\model.tflite) to
// EchoModelPath(), replacing any existing copy.
// Returns false with a user-readable *error when Chrome or the model is not
// found, or when the copy fails.
bool ImportEchoModelFromChrome(std::wstring* error);

#endif  // AEC3_VIRTUAL_MIC_ECHO_MODEL_H_
