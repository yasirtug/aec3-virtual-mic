#ifndef AEC3_VIRTUAL_MIC_AUDIO_ENGINE_H_
#define AEC3_VIRTUAL_MIC_AUDIO_ENGINE_H_

#include <functional>
#include <memory>
#include <string>

// Values are persisted in the registry; never renumber existing modes.
enum class NoiseSuppressionMode {
  kOff = 0,
  kWebRtcHigh = 1,
  kRnnoiseFull = 2,
  kWebRtcModerate = 3,
  kRnnoiseGentle = 4,  // Attenuates noise by at most 12 dB.
  kRnnoiseMedium = 5,  // Attenuates noise by at most 20 dB.
};

struct EngineConfig {
  std::wstring microphone_id;
  // Speakers whose loopback is the echo reference.
  std::wstring echo_output_id;
  // VirtualCable::render_id.
  std::wstring cable_render_id;
  NoiseSuppressionMode noise_mode = NoiseSuppressionMode::kOff;
  bool echo_cancellation = true;
  // Neural (Chrome model) residual echo estimation when the model loads;
  // false = classic WebRTC AEC3. Takes effect at Start (the UI restarts the
  // engine when it changes).
  bool use_neural_echo_model = true;
};

// Peak levels over a short recent window, dBFS (-120 = silence).
struct EngineLevels {
  float microphone_peak_dbfs = -120.0f;  // Raw mic input.
  float reference_peak_dbfs = -120.0f;   // Speaker loopback.
  float output_peak_dbfs = -120.0f;      // Processed voice sent to the cable.
  // Mic hit AEC3's saturation threshold (|x| >= 32700/32768) within ~1 s.
  bool microphone_clipping = false;
};

class AudioEngine {
 public:
  // May be called from audio threads.
  using ErrorCallback = std::function<void(std::wstring)>;

  AudioEngine();
  ~AudioEngine();

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  bool Start(const EngineConfig& config,
             ErrorCallback error_callback,
             std::wstring* error);
  void Stop();
  // Live; also remembered for next Start.
  void SetNoiseSuppressionMode(NoiseSuppressionMode mode);
  // Live; also remembered for next Start.
  void SetEchoCancellation(bool enabled);
  // Live; also remembered for next Start. While muted the cable receives
  // silence (10 ms fade); processing keeps running.
  void SetMuted(bool muted);
  bool running() const;
  // True while running with Chrome's neural residual echo estimator model.
  bool neural_echo_estimator_active() const;
  // Thread-safe, cheap; UI polls ~15 Hz.
  EngineLevels levels() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

#endif  // AEC3_VIRTUAL_MIC_AUDIO_ENGINE_H_
