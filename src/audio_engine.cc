#include "audio_engine.h"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
#include <atomic>
#include <cstring>
#include <future>
#include <mutex>
#include <span>
#include <thread>
#include <string>
#include <utility>
#include <vector>

#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio/echo_canceller3_config.h"
#include "api/audio/neural_residual_echo_estimator_creator.h"
#include "api/environment/environment_factory.h"
#include "echo_model.h"
#include "rnnoise.h"
#include "third_party/tflite/src/tensorflow/lite/kernels/register.h"
#include "third_party/tflite/src/tensorflow/lite/model_builder.h"

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kSampleRate = 48000;
constexpr size_t kFrameSamples = 480;
constexpr size_t kRenderPrerollSamples = kFrameSamples;  // 10 ms.
constexpr size_t kMaxOutputQueueSamples = kFrameSamples * 3;  // 30 ms.

WAVEFORMATEX ProcessingFormat(WORD channels) {
  WAVEFORMATEX format = {};
  format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
  format.nChannels = channels;
  format.nSamplesPerSec = kSampleRate;
  format.wBitsPerSample = 32;
  format.nBlockAlign =
      static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
  format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
  return format;
}

// A 48 kHz IEEE-float format with the channel count and speaker layout of an
// endpoint's mix format. Streams opened with AUTOCONVERTPCM let WASAPI convert
// between it and whatever the endpoint itself runs at.
WAVEFORMATEXTENSIBLE FloatFormatMatching(const WAVEFORMATEX& mix_format) {
  WAVEFORMATEXTENSIBLE format = {};
  format.Format = ProcessingFormat(mix_format.nChannels);
  if (mix_format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
      mix_format.cbSize >=
          sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask =
        reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&mix_format)
            ->dwChannelMask;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  }
  return format;
}

constexpr float kSilenceDbfs = -120.0f;
// AEC3's capture saturation threshold (32700 on the PCM16 scale).
constexpr float kClippingThreshold = 32700.0f / 32768.0f;
constexpr ULONGLONG kClippingHoldMs = 1000;
// Meter fall-off per 10 ms frame: 10^(-0.3 dB / 20), i.e. 30 dB per second.
constexpr float kPeakDecayPerFrame = 0.96605f;

float FramePeak(const float* samples, size_t count) {
  float peak = 0.0f;
  for (size_t i = 0; i < count; ++i) {
    peak = std::max(peak, std::fabs(samples[i]));
  }
  return peak;
}

float ToDbfs(float peak) {
  constexpr float kSilencePeak = 1e-6f;  // -120 dBFS.
  if (!(peak > kSilencePeak)) {
    return kSilenceDbfs;
  }
  return 20.0f * std::log10(peak);
}

// Decaying peak meter: written by one audio thread once per 10 ms frame and
// read by any thread.
class PeakMeter {
 public:
  void Reset() { peak_.store(0.0f, std::memory_order_relaxed); }

  void Update(float frame_peak) {
    const float decayed =
        peak_.load(std::memory_order_relaxed) * kPeakDecayPerFrame;
    peak_.store(std::max(frame_peak, decayed), std::memory_order_relaxed);
  }

  float dbfs() const { return ToDbfs(peak_.load(std::memory_order_relaxed)); }

 private:
  std::atomic<float> peak_ = 0.0f;
};

std::wstring HResultText(const wchar_t* operation, HRESULT result) {
  wchar_t* system_message = nullptr;
  const DWORD size = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(result), 0,
      reinterpret_cast<wchar_t*>(&system_message), 0, nullptr);

  wchar_t code[9] = {};
  swprintf_s(code, L"%08X", static_cast<unsigned int>(result));
  std::wstring message(operation);
  message.append(L" failed (0x").append(code).append(L")");
  if (size != 0 && system_message != nullptr) {
    std::wstring detail(system_message, size);
    while (!detail.empty() &&
           (detail.back() == L'\r' || detail.back() == L'\n')) {
      detail.pop_back();
    }
    message.append(L": ").append(detail);
  }
  if (system_message != nullptr) {
    LocalFree(system_message);
  }
  return message;
}

class AudioRingBuffer {
 public:
  AudioRingBuffer() : samples_(kMaxOutputQueueSamples) {}

  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    size_ = 0;
  }

  void Push(const float* input, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count >= samples_.size()) {
      input += count - samples_.size();
      count = samples_.size();
      head_ = 0;
      size_ = 0;
    }
    if (size_ + count > samples_.size()) {
      const size_t discarded = size_ + count - samples_.size();
      head_ = (head_ + discarded) % samples_.size();
      size_ -= discarded;
    }
    size_t tail = (head_ + size_) % samples_.size();
    const size_t first = std::min(count, samples_.size() - tail);
    std::memcpy(samples_.data() + tail, input, first * sizeof(float));
    if (first < count) {
      std::memcpy(samples_.data(), input + first,
                  (count - first) * sizeof(float));
    }
    size_ += count;
  }

  size_t PopAndPad(float* output, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t popped = std::min(count, size_);
    const size_t first = std::min(popped, samples_.size() - head_);
    std::memcpy(output, samples_.data() + head_, first * sizeof(float));
    if (first < popped) {
      std::memcpy(output + first, samples_.data(),
                  (popped - first) * sizeof(float));
    }
    head_ = (head_ + popped) % samples_.size();
    size_ -= popped;
    std::fill(output + popped, output + count, 0.0f);
    return popped;
  }

  size_t Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<float> samples_;
  size_t head_ = 0;
  size_t size_ = 0;
};

class FrameAssembler {
 public:
  explicit FrameAssembler(size_t channels)
      : frames_(channels), channels_(channels) {
    for (size_t ch = 0; ch < channels; ++ch) {
      channels_[ch] = frames_[ch].data();
    }
  }

  template <typename Callback>
  bool Add(const float* samples,
           size_t count,
           bool silent,
           Callback&& on_frame) {
    while (count != 0) {
      const size_t copied = std::min(count, kFrameSamples - used_);
      for (size_t ch = 0; ch < channels_.size(); ++ch) {
        float* dest = channels_[ch] + used_;
        if (silent) {
          std::fill_n(dest, copied, 0.0f);
        } else if (channels_.size() == 1) {
          std::memcpy(dest, samples, copied * sizeof(float));
        } else {
          for (size_t i = 0; i < copied; ++i) {
            dest[i] = samples[i * channels_.size() + ch];
          }
        }
      }
      if (!silent) {
        samples += copied * channels_.size();
      }
      used_ += copied;
      count -= copied;
      if (used_ == kFrameSamples) {
        if (!on_frame(channels_.data())) {
          return false;
        }
        used_ = 0;
      }
    }
    return true;
  }

 private:
  std::vector<std::array<float, kFrameSamples>> frames_;
  std::vector<float*> channels_;
  size_t used_ = 0;
};

struct CaptureSession {
  ComPtr<IAudioClient> client;
  ComPtr<IAudioCaptureClient> capture;
  WORD channels = 1;
  HANDLE event = nullptr;
  // Loopback only: a silent render stream on the same endpoint. WASAPI stops
  // delivering loopback packets while nothing plays, which makes AEC3 reset
  // its delay estimate and re-converge (suppressing speech) on every sound.
  ComPtr<IAudioClient> keepalive_client;
  ComPtr<IAudioRenderClient> keepalive_render;
  UINT32 keepalive_frames = 0;

  ~CaptureSession() {
    if (event != nullptr) {
      CloseHandle(event);
    }
  }
};

struct RenderSession {
  ComPtr<IAudioClient> client;
  ComPtr<IAudioRenderClient> render;
  UINT32 buffer_frames = 0;
  WORD channels = 2;
  HANDLE event = nullptr;

  ~RenderSession() {
    if (event != nullptr) {
      CloseHandle(event);
    }
  }
};

std::wstring InitializeLoopbackKeepAlive(IMMDevice* device,
                                         CaptureSession* session) {
  HRESULT result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                    nullptr, &session->keepalive_client);
  if (FAILED(result)) {
    return HResultText(L"Activating the echo-reference keep-alive stream",
                       result);
  }
  WAVEFORMATEX* mix_format = nullptr;
  result = session->keepalive_client->GetMixFormat(&mix_format);
  if (FAILED(result)) {
    return HResultText(L"Reading the echo-reference keep-alive format",
                       result);
  }
  constexpr REFERENCE_TIME kKeepAliveBuffer = 1000000;  // 100 ms.
  result = session->keepalive_client->Initialize(
      AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_NOPERSIST,
      kKeepAliveBuffer, 0, mix_format, nullptr);
  CoTaskMemFree(mix_format);
  if (FAILED(result)) {
    return HResultText(L"Starting the echo-reference keep-alive stream",
                       result);
  }
  result = session->keepalive_client->GetBufferSize(&session->keepalive_frames);
  if (FAILED(result)) {
    return HResultText(L"Reading the keep-alive buffer size", result);
  }
  result = session->keepalive_client->GetService(
      IID_PPV_ARGS(&session->keepalive_render));
  if (FAILED(result)) {
    return HResultText(L"Opening the keep-alive render buffer", result);
  }
  BYTE* buffer = nullptr;
  result = session->keepalive_render->GetBuffer(session->keepalive_frames,
                                                &buffer);
  if (SUCCEEDED(result)) {
    result = session->keepalive_render->ReleaseBuffer(
        session->keepalive_frames, AUDCLNT_BUFFERFLAGS_SILENT);
  }
  if (FAILED(result)) {
    return HResultText(L"Priming the keep-alive stream", result);
  }
  return {};
}

// Tops the keep-alive stream back up with silence so it never starves.
HRESULT RefillLoopbackKeepAlive(CaptureSession* session) {
  UINT32 padding = 0;
  HRESULT result = session->keepalive_client->GetCurrentPadding(&padding);
  if (FAILED(result)) {
    return result;
  }
  const UINT32 available = session->keepalive_frames - padding;
  if (available == 0) {
    return S_OK;
  }
  BYTE* buffer = nullptr;
  result = session->keepalive_render->GetBuffer(available, &buffer);
  if (FAILED(result)) {
    return result;
  }
  return session->keepalive_render->ReleaseBuffer(available,
                                                  AUDCLNT_BUFFERFLAGS_SILENT);
}

std::wstring InitializeCaptureSession(const std::wstring& device_id,
                                      bool loopback,
                                      CaptureSession* session) {
  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                    CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  if (FAILED(result)) {
    return HResultText(L"Creating the Windows audio enumerator", result);
  }

  ComPtr<IMMDevice> device;
  result = enumerator->GetDevice(device_id.c_str(), &device);
  if (FAILED(result)) {
    return HResultText(loopback ? L"Opening the echo-reference output"
                                : L"Opening the microphone",
                       result);
  }

  const auto activate = [&]() {
    session->client.Reset();
    return device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            &session->client);
  };
  result = activate();
  if (FAILED(result)) {
    return HResultText(L"Activating the WASAPI capture client", result);
  }

  WAVEFORMATEXTENSIBLE format = {};
  format.Format = ProcessingFormat(1);
  if (loopback) {
    WAVEFORMATEX* mix_format = nullptr;
    result = session->client->GetMixFormat(&mix_format);
    if (FAILED(result)) {
      return HResultText(L"Reading the speaker channel layout", result);
    }
    // Preserve every speaker channel instead of downmixing the AEC reference.
    format = FloatFormatMatching(*mix_format);
    CoTaskMemFree(mix_format);
  }
  session->channels = format.Format.nChannels;
  DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                AUDCLNT_STREAMFLAGS_NOPERSIST |
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  if (loopback) {
    flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
  }
  bool initialized = false;
  if (!loopback) {
    ComPtr<IAudioClient2> client2;
    if (SUCCEEDED(session->client.As(&client2))) {
      AudioClientProperties properties = {};
      properties.cbSize = sizeof(properties);
      properties.eCategory = AudioCategory_Other;
      properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
      result = client2->SetClientProperties(&properties);
      if (SUCCEEDED(result)) {
        result = session->client->Initialize(
            AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &format.Format, nullptr);
      }
      initialized = SUCCEEDED(result);
      if (!initialized) {
        // A driver may reject RAW properties or the format in RAW mode.
        // Retry ordinary capture on a fresh client, without stale properties.
        client2.Reset();
        result = activate();
        if (FAILED(result)) {
          return HResultText(L"Reactivating the microphone WASAPI client",
                             result);
        }
      }
    }
  }
  if (!initialized) {
    result = session->client->Initialize(
        AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &format.Format, nullptr);
  }
  if (FAILED(result)) {
    return HResultText(loopback ? L"Starting 48 kHz speaker loopback"
                                : L"Starting 48 kHz microphone capture",
                       result);
  }

  session->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (session->event == nullptr) {
    return HResultText(L"Creating the WASAPI capture event",
                       HRESULT_FROM_WIN32(GetLastError()));
  }
  result = session->client->SetEventHandle(session->event);
  if (FAILED(result)) {
    return HResultText(L"Connecting the WASAPI capture event", result);
  }

  result = session->client->GetService(IID_PPV_ARGS(&session->capture));
  if (FAILED(result)) {
    return HResultText(L"Opening the WASAPI capture buffer", result);
  }
  if (loopback) {
    return InitializeLoopbackKeepAlive(device.Get(), session);
  }
  return {};
}

std::wstring InitializeRenderSession(const std::wstring& device_id,
                                     RenderSession* session) {
  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                    CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  if (FAILED(result)) {
    return HResultText(L"Creating the Windows audio enumerator", result);
  }

  ComPtr<IMMDevice> device;
  result = enumerator->GetDevice(device_id.c_str(), &device);
  if (FAILED(result)) {
    return HResultText(L"Opening the virtual cable playback endpoint", result);
  }

  const auto activate = [&]() {
    session->client.Reset();
    return device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            &session->client);
  };
  result = activate();
  if (FAILED(result)) {
    return HResultText(L"Activating the virtual cable WASAPI client", result);
  }

  // Any cable format works: play 48 kHz float with the endpoint's channel
  // count and let WASAPI convert to the cable's own sample rate and format.
  WAVEFORMATEX* mix_format = nullptr;
  result = session->client->GetMixFormat(&mix_format);
  if (FAILED(result)) {
    return HResultText(L"Reading the virtual cable's channel layout", result);
  }
  WAVEFORMATEXTENSIBLE format = FloatFormatMatching(*mix_format);
  CoTaskMemFree(mix_format);
  const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                      AUDCLNT_STREAMFLAGS_NOPERSIST |
                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

  bool initialized = false;
  ComPtr<IAudioClient3> client3;
  if (SUCCEEDED(session->client.As(&client3))) {
    UINT32 default_period = 0;
    UINT32 fundamental_period = 0;
    UINT32 minimum_period = 0;
    UINT32 maximum_period = 0;
    result = client3->GetSharedModeEnginePeriod(
        &format.Format, &default_period, &fundamental_period, &minimum_period,
        &maximum_period);
    if (SUCCEEDED(result)) {
      // AEC3 emits one 10 ms block at a time. Shorter advertised periods
      // starve between those bursts, so use the stable default period.
      result = client3->InitializeSharedAudioStream(flags, default_period,
                                                    &format.Format, nullptr);
      initialized = SUCCEEDED(result);
    }
  }

  client3.Reset();
  if (!initialized) {
    result = activate();
    if (FAILED(result)) {
      return HResultText(L"Reactivating the virtual cable WASAPI client",
                         result);
    }
    result = session->client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0,
                                         &format.Format, nullptr);
    if (FAILED(result)) {
      return HResultText(L"Starting 48 kHz playback into the virtual cable",
                         result);
    }
  }
  session->channels = format.Format.nChannels;

  session->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (session->event == nullptr) {
    return HResultText(L"Creating the virtual cable playback event",
                       HRESULT_FROM_WIN32(GetLastError()));
  }
  result = session->client->SetEventHandle(session->event);
  if (FAILED(result)) {
    return HResultText(L"Connecting the virtual cable playback event", result);
  }
  result = session->client->GetBufferSize(&session->buffer_frames);
  if (FAILED(result)) {
    return HResultText(L"Reading the virtual cable buffer size", result);
  }
  result = session->client->GetService(IID_PPV_ARGS(&session->render));
  if (FAILED(result)) {
    return HResultText(L"Opening the virtual cable render buffer", result);
  }
  return {};
}

bool ReadFileBytes(const std::wstring& path, std::string* bytes) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  LARGE_INTEGER size = {};
  bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
            size.QuadPart <= MAXDWORD;
  if (ok) {
    bytes->resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ok = ReadFile(file, bytes->data(), static_cast<DWORD>(size.QuadPart),
                  &read, nullptr) &&
         read == size.QuadPart;
  }
  CloseHandle(file);
  return ok;
}

bool UsesWebRtcNoiseSuppression(NoiseSuppressionMode mode) {
  return mode == NoiseSuppressionMode::kWebRtcModerate ||
         mode == NoiseSuppressionMode::kWebRtcHigh;
}

webrtc::AudioProcessing::Config::NoiseSuppression::Level
WebRtcNoiseSuppressionLevel(NoiseSuppressionMode mode) {
  // kHigh is Chrome's level (media/webrtc/helpers.cc).
  return mode == NoiseSuppressionMode::kWebRtcModerate
             ? webrtc::AudioProcessing::Config::NoiseSuppression::kModerate
             : webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
}

bool UsesRnnoise(NoiseSuppressionMode mode) {
  return mode == NoiseSuppressionMode::kRnnoiseGentle ||
         mode == NoiseSuppressionMode::kRnnoiseMedium ||
         mode == NoiseSuppressionMode::kRnnoiseFull;
}

// Minimum per-band gain applied after RNNoise (amplitude): 10^(-dB/20).
float RnnoiseGainFloor(NoiseSuppressionMode mode) {
  switch (mode) {
    case NoiseSuppressionMode::kRnnoiseGentle:
      return 0.2512f;  // -12 dB.
    case NoiseSuppressionMode::kRnnoiseMedium:
      return 0.1f;  // -20 dB.
    default:
      return 0.0f;  // Unlimited.
  }
}

// RNNoise output lags its input by two frames (960 samples, 20 ms): one frame
// of overlap-add synthesis plus one frame of lookahead (`delayed_X`).
constexpr size_t kRnnoiseDelaySamples = 2 * kFrameSamples;

}  // namespace

class AudioEngine::Impl {
 public:
  bool Start(const EngineConfig& engine_config,
             ErrorCallback error_callback,
             std::wstring* error) {
    if (running_.load()) {
      if (error != nullptr) {
        *error = L"Audio processing is already running.";
      }
      return false;
    }
    if (error != nullptr) {
      error->clear();
    }

    if (rnnoise_get_frame_size() != kFrameSamples) {
      if (error != nullptr) {
        *error = L"RNNoise does not support the required 10 ms audio frames.";
      }
      return false;
    }
    rnnoise_.reset(rnnoise_create(nullptr));
    if (!rnnoise_) {
      if (error != nullptr) {
        *error = L"RNNoise could not initialize its built-in model.";
      }
      return false;
    }
    noise_mode_.store(engine_config.noise_mode, std::memory_order_relaxed);
    echo_cancellation_.store(engine_config.echo_cancellation,
                             std::memory_order_relaxed);
    active_noise_mode_ = engine_config.noise_mode;
    active_echo_cancellation_ = engine_config.echo_cancellation;
    rnnoise_dry_history_.fill(0.0f);
    output_gain_ = muted_.load(std::memory_order_relaxed) ? 0.0f : 1.0f;

    webrtc::AudioProcessing::Config config;
    config.pipeline.maximum_internal_processing_rate = kSampleRate;
    config.pipeline.multi_channel_render = true;
    config.echo_canceller.enabled = active_echo_cancellation_;
    config.echo_canceller.enforce_high_pass_filtering = true;
    config.high_pass_filter.enabled = true;
    config.noise_suppression.enabled =
        UsesWebRtcNoiseSuppression(active_noise_mode_);
    config.noise_suppression.level =
        WebRtcNoiseSuppressionLevel(active_noise_mode_);
    webrtc::EchoCanceller3Config aec3_config;
    aec3_config.delay.fixed_capture_delay_samples = 160;  // 10 ms at 16 kHz.
    auto multichannel_config =
        webrtc::EchoCanceller3Config::CreateDefaultMultichannelConfig();
    multichannel_config.delay.fixed_capture_delay_samples =
        aec3_config.delay.fixed_capture_delay_samples;

    // Chrome's neural residual echo estimator replaces AEC3's hand-tuned
    // residual echo estimate that drives suppression during double-talk.
    // WebRTC validates the model interface and returns null on a mismatch,
    // in which case classic AEC3 runs. The estimator lives inside `apm_`
    // even while echo cancellation is toggled off, so toggling it back on
    // live keeps using the model.
    apm_ = nullptr;  // The previous estimator must not outlive its model.
    std::unique_ptr<webrtc::NeuralResidualEchoEstimator> echo_estimator;
    echo_model_.reset();
    echo_model_bytes_.clear();
    if (engine_config.use_neural_echo_model) {
      if (!EchoModelInstalled()) {
        // First run: copy the model from the local Chrome profile. On failure
        // classic AEC3 runs; the UI reports why via ImportEchoModelFromChrome.
        ImportEchoModelFromChrome(nullptr);
      }
      const std::wstring model_path = EchoModelPath();
      if (!model_path.empty() &&
          ReadFileBytes(model_path, &echo_model_bytes_)) {
        echo_model_ = tflite::FlatBufferModel::BuildFromBuffer(
            echo_model_bytes_.data(), echo_model_bytes_.size());
        if (echo_model_) {
          tflite::ops::builtin::BuiltinOpResolver op_resolver;
          echo_estimator = webrtc::CreateNeuralResidualEchoEstimator(
              echo_model_.get(), &op_resolver);
        }
      }
    }
    const bool neural_active = echo_estimator != nullptr;
    neural_echo_estimator_active_.store(neural_active);
    if (!neural_active) {
      echo_model_.reset();
      echo_model_bytes_.clear();
    }

    apm_ = webrtc::BuiltinAudioProcessingBuilder(config)
               .SetEchoCancellerConfig(aec3_config, multichannel_config)
               .SetNeuralResidualEchoEstimator(std::move(echo_estimator))
               .Build(webrtc::CreateEnvironment());
    if (!apm_) {
      if (error != nullptr) {
        *error = L"WebRTC could not create the AEC3 processor.";
      }
      return false;
    }
    const int initialize_result = apm_->Initialize();
    if (initialize_result != webrtc::AudioProcessing::kNoError) {
      if (error != nullptr) {
        *error = L"WebRTC could not initialize the AEC3 processor.";
      }
      apm_ = nullptr;
      return false;
    }

    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event_ == nullptr) {
      if (error != nullptr) {
        *error = HResultText(L"Creating the audio stop event",
                             HRESULT_FROM_WIN32(GetLastError()));
      }
      apm_ = nullptr;
      return false;
    }

    output_.Clear();
    microphone_meter_.Reset();
    reference_meter_.Reset();
    output_meter_.Reset();
    microphone_last_clip_ms_.store(0, std::memory_order_relaxed);
    callback_ = std::move(error_callback);
    error_reported_.store(false);
    running_.store(true);

    std::promise<std::wstring> microphone_promise;
    std::promise<std::wstring> loopback_promise;
    std::promise<std::wstring> cable_promise;
    std::future<std::wstring> microphone_ready = microphone_promise.get_future();
    std::future<std::wstring> loopback_ready = loopback_promise.get_future();
    std::future<std::wstring> cable_ready = cable_promise.get_future();

    microphone_thread_ =
        std::thread([this, microphone_id = engine_config.microphone_id,
                     ready = std::move(microphone_promise)]() mutable {
          RunCapture(microphone_id, false, std::move(ready));
        });
    loopback_thread_ =
        std::thread([this, echo_output_id = engine_config.echo_output_id,
                     ready = std::move(loopback_promise)]() mutable {
          RunCapture(echo_output_id, true, std::move(ready));
        });
    cable_thread_ =
        std::thread([this, cable_render_id = engine_config.cable_render_id,
                     ready = std::move(cable_promise)]() mutable {
          RunRender(cable_render_id, std::move(ready));
        });

    std::wstring startup_error = microphone_ready.get();
    if (startup_error.empty()) {
      startup_error = loopback_ready.get();
    } else {
      loopback_ready.wait();
    }
    if (startup_error.empty()) {
      startup_error = cable_ready.get();
    } else {
      cable_ready.wait();
    }

    if (!startup_error.empty()) {
      Stop();
      if (error != nullptr) {
        *error = std::move(startup_error);
      }
      return false;
    }
    return true;
  }

  void Stop() {
    if (stop_event_ != nullptr) {
      SetEvent(stop_event_);
    }
    if (microphone_thread_.joinable()) {
      microphone_thread_.join();
    }
    if (loopback_thread_.joinable()) {
      loopback_thread_.join();
    }
    if (cable_thread_.joinable()) {
      cable_thread_.join();
    }
    if (stop_event_ != nullptr) {
      CloseHandle(stop_event_);
      stop_event_ = nullptr;
    }
    output_.Clear();
    apm_ = nullptr;
    echo_model_.reset();
    echo_model_bytes_.clear();
    neural_echo_estimator_active_.store(false);
    rnnoise_.reset();
    callback_ = nullptr;
    running_.store(false);
  }

  void SetNoiseSuppressionMode(NoiseSuppressionMode mode) {
    noise_mode_.store(mode, std::memory_order_relaxed);
  }

  void SetEchoCancellation(bool enabled) {
    echo_cancellation_.store(enabled, std::memory_order_relaxed);
  }

  void SetMuted(bool muted) { muted_.store(muted, std::memory_order_relaxed); }

  bool running() const { return running_.load(); }
  bool neural_echo_estimator_active() const {
    return neural_echo_estimator_active_.load();
  }

  EngineLevels levels() const {
    EngineLevels levels;
    if (!running_.load()) {
      return levels;
    }
    levels.microphone_peak_dbfs = microphone_meter_.dbfs();
    levels.reference_peak_dbfs = reference_meter_.dbfs();
    levels.output_peak_dbfs = output_meter_.dbfs();
    const ULONGLONG last_clip =
        microphone_last_clip_ms_.load(std::memory_order_relaxed);
    levels.microphone_clipping =
        last_clip != 0 && GetTickCount64() - last_clip < kClippingHoldMs;
    return levels;
  }

 private:
  void ReportError(std::wstring message) {
    bool expected = false;
    if (!error_reported_.compare_exchange_strong(expected, true)) {
      return;
    }
    if (stop_event_ != nullptr) {
      SetEvent(stop_event_);
    }
    if (callback_) {
      callback_(std::move(message));
    }
  }

  void RunCapture(const std::wstring& device_id,
                  bool loopback,
                  std::promise<std::wstring> ready) {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_result)) {
      ready.set_value(HResultText(L"Initializing COM for audio capture",
                                  com_result));
      return;
    }

    CaptureSession session;
    std::wstring error =
        InitializeCaptureSession(device_id, loopback, &session);
    if (!error.empty()) {
      ready.set_value(std::move(error));
      CoUninitialize();
      return;
    }

    HRESULT result = session.client->Start();
    if (SUCCEEDED(result) && loopback) {
      result = session.keepalive_client->Start();
    }
    if (FAILED(result)) {
      ready.set_value(HResultText(L"Starting WASAPI capture", result));
      CoUninitialize();
      return;
    }
    ready.set_value({});

    DWORD task_index = 0;
    HANDLE task =
        AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    FrameAssembler assembler(session.channels);
    const webrtc::StreamConfig stream_config(kSampleRate, session.channels);
    HANDLE events[] = {stop_event_, session.event};
    bool keep_running = true;

    while (keep_running &&
           WaitForMultipleObjects(2, events, FALSE, INFINITE) ==
               WAIT_OBJECT_0 + 1) {
      UINT32 packet_frames = 0;
      result = session.capture->GetNextPacketSize(&packet_frames);
      while (SUCCEEDED(result) && packet_frames != 0) {
        BYTE* packet = nullptr;
        DWORD flags = 0;
        result = session.capture->GetBuffer(&packet, &packet_frames, &flags,
                                            nullptr, nullptr);
        if (FAILED(result)) {
          break;
        }

        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        const float* samples = silent ? nullptr
                                      : reinterpret_cast<const float*>(packet);
        keep_running = assembler.Add(
            samples, packet_frames, silent,
            [this, loopback, &stream_config](float* const* channels) {
              int processing_result = webrtc::AudioProcessing::kNoError;
              if (loopback) {
                float reference_peak = 0.0f;
                for (size_t ch = 0; ch < stream_config.num_channels(); ++ch) {
                  reference_peak = std::max(
                      reference_peak, FramePeak(channels[ch], kFrameSamples));
                }
                reference_meter_.Update(reference_peak);
                processing_result = apm_->ProcessReverseStream(
                    channels, stream_config, stream_config, channels);
              } else {
                const float microphone_peak =
                    FramePeak(channels[0], kFrameSamples);
                microphone_meter_.Update(microphone_peak);
                if (microphone_peak >= kClippingThreshold) {
                  microphone_last_clip_ms_.store(GetTickCount64(),
                                                 std::memory_order_relaxed);
                }
                // Apply setting changes on the capture thread so denoisers
                // never overlap and RNNoise state is never touched by the UI
                // thread. Speaker loopback keeps feeding AEC3 regardless.
                const auto mode = noise_mode_.load(std::memory_order_relaxed);
                const bool echo_cancellation =
                    echo_cancellation_.load(std::memory_order_relaxed);
                if (mode != active_noise_mode_ ||
                    echo_cancellation != active_echo_cancellation_) {
                  auto config = apm_->GetConfig();
                  config.echo_canceller.enabled = echo_cancellation;
                  config.noise_suppression.enabled =
                      UsesWebRtcNoiseSuppression(mode);
                  config.noise_suppression.level =
                      WebRtcNoiseSuppressionLevel(mode);
                  apm_->ApplyConfig(config);
                  if (UsesRnnoise(mode) && !UsesRnnoise(active_noise_mode_)) {
                    rnnoise_init(rnnoise_.get(), nullptr);
                    rnnoise_dry_history_.fill(0.0f);
                  }
                  active_noise_mode_ = mode;
                  active_echo_cancellation_ = echo_cancellation;
                }
                apm_->set_stream_delay_ms(0);
                processing_result = apm_->ProcessStream(
                    channels, stream_config, stream_config, channels);
                if (processing_result == webrtc::AudioProcessing::kNoError) {
                  if (UsesRnnoise(mode)) {
                    // RNNoise uses PCM16-scale floats, not normalized floats.
                    std::span<float, kFrameSamples> frame(channels[0],
                                                          kFrameSamples);
                    std::array<float, kFrameSamples> dry;
                    for (size_t i = 0; i < kFrameSamples; ++i) {
                      frame[i] *= 32768.0f;
                      dry[i] = frame[i];
                    }
                    rnnoise_process_frame(rnnoise_.get(), frame.data(),
                                           frame.data());
                    // Cap the attenuation by mixing in the dry signal, delayed
                    // to match RNNoise's output: each band's gain then stays
                    // within [floor, 1] instead of [0, 1].
                    const float floor = RnnoiseGainFloor(mode);
                    for (size_t i = 0; i < kFrameSamples; ++i) {
                      frame[i] = ((1.0f - floor) * frame[i] +
                                  floor * rnnoise_dry_history_[i]) *
                                 (1.0f / 32768.0f);
                    }
                    std::copy(rnnoise_dry_history_.begin() + kFrameSamples,
                              rnnoise_dry_history_.end(),
                              rnnoise_dry_history_.begin());
                    std::copy(dry.begin(), dry.end(),
                              rnnoise_dry_history_.begin() + kFrameSamples);
                  }
                  ApplyOutputGain(channels[0]);
                  output_meter_.Update(FramePeak(channels[0], kFrameSamples));
                  output_.Push(channels[0], kFrameSamples);
                }
              }
              if (processing_result != webrtc::AudioProcessing::kNoError) {
                ReportError(loopback
                                ? L"WebRTC AEC3 rejected a speaker frame."
                                : L"WebRTC AEC3 rejected a microphone frame.");
                return false;
              }
              return true;
            });

        const HRESULT release_result =
            session.capture->ReleaseBuffer(packet_frames);
        if (FAILED(release_result)) {
          result = release_result;
          break;
        }
        if (!keep_running) {
          break;
        }
        result = session.capture->GetNextPacketSize(&packet_frames);
      }
      if (SUCCEEDED(result) && loopback) {
        result = RefillLoopbackKeepAlive(&session);
        if (FAILED(result)) {
          ReportError(HResultText(L"Refilling the echo-reference keep-alive",
                                  result));
          break;
        }
      }
      if (FAILED(result)) {
        ReportError(HResultText(loopback ? L"Reading speaker loopback"
                                         : L"Reading the microphone",
                                    result));
        break;
      }
    }

    session.client->Stop();
    if (session.keepalive_client) {
      session.keepalive_client->Stop();
    }
    if (task != nullptr) {
      AvRevertMmThreadCharacteristics(task);
    }
    CoUninitialize();
  }

  void RunRender(const std::wstring& device_id,
                 std::promise<std::wstring> ready) {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_result)) {
      ready.set_value(
          HResultText(L"Initializing COM for virtual cable playback",
                      com_result));
      return;
    }

    RenderSession session;
    std::wstring error = InitializeRenderSession(device_id, &session);
    if (!error.empty()) {
      ready.set_value(std::move(error));
      CoUninitialize();
      return;
    }

    BYTE* initial_buffer = nullptr;
    HRESULT result =
        session.render->GetBuffer(session.buffer_frames, &initial_buffer);
    if (SUCCEEDED(result)) {
      result = session.render->ReleaseBuffer(session.buffer_frames,
                                             AUDCLNT_BUFFERFLAGS_SILENT);
    }
    if (FAILED(result)) {
      ready.set_value(HResultText(L"Priming virtual cable playback", result));
      CoUninitialize();
      return;
    }

    result = session.client->Start();
    if (FAILED(result)) {
      ready.set_value(HResultText(L"Starting virtual cable playback", result));
      CoUninitialize();
      return;
    }
    ready.set_value({});

    DWORD task_index = 0;
    HANDLE task =
        AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    std::vector<float> mono_buffer(session.buffer_frames);
    bool output_started = false;
    HANDLE events[] = {stop_event_, session.event};
    while (WaitForMultipleObjects(2, events, FALSE, INFINITE) ==
           WAIT_OBJECT_0 + 1) {
      UINT32 padding = 0;
      result = session.client->GetCurrentPadding(&padding);
      if (FAILED(result)) {
        ReportError(HResultText(L"Reading virtual cable playback padding",
                                result));
        break;
      }
      const UINT32 available = session.buffer_frames - padding;
      if (available == 0) {
        continue;
      }

      BYTE* raw_buffer = nullptr;
      result = session.render->GetBuffer(available, &raw_buffer);
      if (FAILED(result)) {
        ReportError(
            HResultText(L"Opening the virtual cable playback buffer", result));
        break;
      }
      const size_t queued = output_.Size();
      if ((!output_started && queued < kRenderPrerollSamples) ||
          (output_started && queued < available)) {
        output_started = false;
        result = session.render->ReleaseBuffer(
            available, AUDCLNT_BUFFERFLAGS_SILENT);
        if (FAILED(result)) {
          ReportError(
              HResultText(L"Writing silence to the virtual cable", result));
          break;
        }
        continue;
      }
      output_started = true;
      auto* buffer = reinterpret_cast<float*>(raw_buffer);
      const size_t popped =
          output_.PopAndPad(mono_buffer.data(), available);
      for (UINT32 frame = 0; frame < available; ++frame) {
        const float sample = std::clamp(mono_buffer[frame], -1.0f, 1.0f);
        for (WORD channel = 0; channel < session.channels; ++channel) {
          buffer[static_cast<size_t>(frame) * session.channels + channel] =
              sample;
        }
      }
      const DWORD flags =
          popped == 0 ? AUDCLNT_BUFFERFLAGS_SILENT : 0;
      result = session.render->ReleaseBuffer(available, flags);
      if (FAILED(result)) {
        ReportError(HResultText(
            L"Writing processed audio to the virtual cable", result));
        break;
      }
    }

    session.client->Stop();
    if (task != nullptr) {
      AvRevertMmThreadCharacteristics(task);
    }
    CoUninitialize();
  }

  // Mute: fades the processed voice to/from silence over one 10 ms frame
  // (no click). Capture thread only; processing keeps running underneath so
  // AEC3 stays converged and unmuting is clean immediately.
  void ApplyOutputGain(float* frame) {
    const float target = muted_.load(std::memory_order_relaxed) ? 0.0f : 1.0f;
    if (output_gain_ == target) {
      if (target == 0.0f) {
        std::fill_n(frame, kFrameSamples, 0.0f);
      }
      return;
    }
    const float step =
        (target - output_gain_) / static_cast<float>(kFrameSamples);
    for (size_t i = 0; i < kFrameSamples; ++i) {
      frame[i] *= output_gain_ + step * static_cast<float>(i + 1);
    }
    output_gain_ = target;
  }

  std::atomic<NoiseSuppressionMode> noise_mode_ = NoiseSuppressionMode::kOff;
  std::atomic<bool> echo_cancellation_ = true;
  std::atomic<bool> muted_ = false;
  float output_gain_ = 1.0f;  // Capture thread; see ApplyOutputGain.
  NoiseSuppressionMode active_noise_mode_ = NoiseSuppressionMode::kOff;
  bool active_echo_cancellation_ = true;
  std::unique_ptr<DenoiseState, decltype(&rnnoise_destroy)> rnnoise_{
      nullptr, rnnoise_destroy};
  // Dry input over the last kRnnoiseDelaySamples; index i of the oldest frame
  // lines up with sample i of the current RNNoise output.
  static_assert(kRnnoiseDelaySamples == 2 * kFrameSamples);
  std::array<float, kRnnoiseDelaySamples> rnnoise_dry_history_{};
  std::atomic<bool> running_ = false;
  std::atomic<bool> error_reported_ = false;
  HANDLE stop_event_ = nullptr;
  AudioRingBuffer output_;
  // Written by the audio threads, read by levels() on any thread.
  PeakMeter microphone_meter_;
  PeakMeter reference_meter_;
  PeakMeter output_meter_;
  std::atomic<ULONGLONG> microphone_last_clip_ms_ = 0;
  // The model bytes and FlatBufferModel must outlive the estimator owned by
  // `apm_`; members are destroyed in reverse order, so keep these above it.
  std::string echo_model_bytes_;
  std::unique_ptr<tflite::FlatBufferModel> echo_model_;
  std::atomic<bool> neural_echo_estimator_active_ = false;
  webrtc::scoped_refptr<webrtc::AudioProcessing> apm_;
  ErrorCallback callback_;
  std::thread microphone_thread_;
  std::thread loopback_thread_;
  std::thread cable_thread_;
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() { impl_->Stop(); }

bool AudioEngine::Start(const EngineConfig& config,
                        ErrorCallback error_callback,
                        std::wstring* error) {
  return impl_->Start(config, std::move(error_callback), error);
}

void AudioEngine::Stop() { impl_->Stop(); }
void AudioEngine::SetNoiseSuppressionMode(NoiseSuppressionMode mode) {
  impl_->SetNoiseSuppressionMode(mode);
}
void AudioEngine::SetEchoCancellation(bool enabled) {
  impl_->SetEchoCancellation(enabled);
}
void AudioEngine::SetMuted(bool muted) { impl_->SetMuted(muted); }
bool AudioEngine::running() const { return impl_->running(); }
bool AudioEngine::neural_echo_estimator_active() const {
  return impl_->neural_echo_estimator_active();
}
EngineLevels AudioEngine::levels() const { return impl_->levels(); }
