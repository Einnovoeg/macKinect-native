#include "backends/backend.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if KINECT_HAVE_LIBFREENECT
#include <libfreenect.h>
#include <libfreenect_audio.h>
#endif

namespace {

#if KINECT_HAVE_LIBFREENECT

constexpr int kWidth = 640;
constexpr int kHeight = 480;
constexpr int kPixelCount = kWidth * kHeight;

bool IsSyntheticIndexSerial(const std::string &serial) {
  return serial.rfind("DeviceIndex-", 0) == 0;
}

bool FileExists(const std::string &path) {
  if (path.empty()) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

bool FirmwareExistsInDir(const std::string &dir) {
  if (dir.empty()) {
    return false;
  }
  std::error_code ec;
  const auto full = std::filesystem::path(dir) / "audios.bin";
  return std::filesystem::exists(full, ec);
}

std::string BundleResourcesDir() {
#if defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string exe;
  exe.resize(size + 1);
  if (_NSGetExecutablePath(exe.data(), &size) != 0) {
    return "";
  }
  exe.resize(std::strlen(exe.c_str()));
  std::error_code ec;
  auto path = std::filesystem::weakly_canonical(std::filesystem::path(exe), ec);
  if (ec || path.empty()) {
    path = std::filesystem::path(exe);
  }
  const auto resources = path.parent_path().parent_path() / "Resources";
  return resources.string();
#else
  return "";
#endif
}

std::string FindFirmwareDirectory() {
  const char *env = std::getenv("LIBFREENECT_FIRMWARE_PATH");
  if (env != nullptr && FirmwareExistsInDir(env)) {
    return std::string(env);
  }

  std::vector<std::string> candidates;
  const auto resources = BundleResourcesDir();
  if (!resources.empty()) {
    candidates.push_back((std::filesystem::path(resources) / "libfreenect").string());
    candidates.push_back(resources);
  }

  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (!ec) {
    candidates.push_back((cwd / ".libfreenect").string());
    candidates.push_back((cwd / "libfreenect").string());
    candidates.push_back((cwd / "../libfreenect/src").string());
    candidates.push_back((cwd / "../libfreenect").string());
  }

  candidates.push_back("/usr/local/share/libfreenect");
  candidates.push_back("/usr/share/libfreenect");
  candidates.push_back("/opt/homebrew/share/libfreenect");

  for (const auto &dir : candidates) {
    if (FirmwareExistsInDir(dir)) {
      return dir;
    }
  }
  return "";
}

class FreenectV1Device final : public KinectDevice {
 public:
  FreenectV1Device(freenect_context *ctx, int index, std::string serial, bool audio_supported)
      : ctx_(ctx), index_(index), serial_(std::move(serial)), audio_supported_(audio_supported) {}

  ~FreenectV1Device() override {
    stop();
    if (dev_ != nullptr) {
      freenect_close_device(dev_);
      dev_ = nullptr;
    }
  }

  bool open() {
    if (ctx_ == nullptr) {
      return false;
    }

    int rc = -1;
    if (!serial_.empty() && !IsSyntheticIndexSerial(serial_)) {
      rc = freenect_open_device_by_camera_serial(ctx_, &dev_, serial_.c_str());
      if (rc < 0 || dev_ == nullptr) {
        std::cerr << "[kinect-v1] freenect_open_device_by_camera_serial failed for serial " << serial_
                  << " (rc=" << rc << ")\n";
      }
    }

    if (dev_ == nullptr && index_ >= 0) {
      rc = freenect_open_device(ctx_, &dev_, index_);
      if (rc < 0 || dev_ == nullptr) {
        std::cerr << "[kinect-v1] freenect_open_device failed for index " << index_ << " (rc=" << rc << ")\n";
        return false;
      }
    }

    if (dev_ == nullptr) {
      std::cerr << "[kinect-v1] device open failed (serial=" << serial_ << ", index=" << index_ << ")\n";
      return false;
    }

    freenect_set_user(dev_, this);
    freenect_set_depth_callback(dev_, &FreenectV1Device::OnDepthFrame);
    freenect_set_video_callback(dev_, &FreenectV1Device::OnVideoFrame);
    if (audio_supported_) {
      freenect_set_audio_in_callback(dev_, &FreenectV1Device::OnAudioFrame);
    }
    return true;
  }

  bool start() override {
    if (dev_ == nullptr || running_) {
      return dev_ != nullptr;
    }

    const freenect_frame_mode depth_mode =
        freenect_find_depth_mode(FREENECT_RESOLUTION_MEDIUM, FREENECT_DEPTH_MM);
    if (!depth_mode.is_valid || freenect_set_depth_mode(dev_, depth_mode) < 0) {
      std::cerr << "[kinect-v1] failed to set depth mode (DEPTH_MM @ 640x480)\n";
      return false;
    }

    if (!ApplyVideoMode()) {
      std::cerr << "[kinect-v1] failed to apply video mode\n";
      return false;
    }

    if (freenect_start_depth(dev_) < 0) {
      std::cerr << "[kinect-v1] freenect_start_depth failed\n";
      return false;
    }
    depth_started_ = true;

    // ApplyVideoMode() starts video the first time. Only start explicitly if it
    // wasn't started there.
    if (!video_started_) {
      if (freenect_start_video(dev_) < 0) {
        freenect_stop_depth(dev_);
        depth_started_ = false;
        std::cerr << "[kinect-v1] freenect_start_video failed\n";
        return false;
      }
      video_started_ = true;
    }

    running_ = true;

    if (audio_enabled_) {
      setAudioEnabled(true);
    }

    return true;
  }

  bool stop() override {
    if (dev_ == nullptr) {
      return false;
    }
    if (!running_) {
      return true;
    }

    if (audio_started_) {
      freenect_stop_audio(dev_);
      audio_started_ = false;
    }
    if (video_started_) {
      freenect_stop_video(dev_);
      video_started_ = false;
    }
    if (depth_started_) {
      freenect_stop_depth(dev_);
      depth_started_ = false;
    }
    running_ = false;
    return true;
  }

  bool update() override {
    if (!running_) {
      return false;
    }

    if (requested_stream_ != active_stream_) {
      ApplyVideoMode();
    }

    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 2000;
    if (freenect_process_events_timeout(ctx_, &timeout) < 0) {
      return false;
    }

    std::lock_guard<std::mutex> lock(frame_mutex_);
    return has_new_frame_;
  }

  bool getFrame(FrameData &out_frame) override {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!has_new_frame_) {
      return false;
    }

    out_frame = frame_;
    has_new_frame_ = false;
    return true;
  }

  void setTilt(int angle) override {
    if (dev_ == nullptr) {
      return;
    }
    const int clamped = std::max(-30, std::min(30, angle));
    freenect_set_tilt_degs(dev_, clamped);
  }

  void setLed(int mode) override {
    if (dev_ == nullptr) {
      return;
    }
    const int clamped = std::max(0, std::min(6, mode));
    freenect_set_led(dev_, static_cast<freenect_led_options>(clamped));
  }

  void setStreamKind(StreamKind kind) override {
    requested_stream_ = kind;
  }

  StreamKind streamKind() const override {
    return requested_stream_;
  }

  void setMirror(bool enabled) override {
    if (dev_ == nullptr) {
      return;
    }
    const freenect_flag_value value = enabled ? FREENECT_ON : FREENECT_OFF;
    freenect_set_flag(dev_, FREENECT_MIRROR_DEPTH, value);
    freenect_set_flag(dev_, FREENECT_MIRROR_VIDEO, value);
  }

  void setAutoExposure(bool enabled) override {
    if (dev_ == nullptr) {
      return;
    }
    const freenect_flag_value value = enabled ? FREENECT_ON : FREENECT_OFF;
    freenect_set_flag(dev_, FREENECT_AUTO_EXPOSURE, value);
    freenect_set_flag(dev_, FREENECT_AUTO_FLICKER, value);
  }

  void setAutoWhiteBalance(bool enabled) override {
    if (dev_ == nullptr) {
      return;
    }
    freenect_set_flag(dev_, FREENECT_AUTO_WHITE_BALANCE, enabled ? FREENECT_ON : FREENECT_OFF);
  }

  void setNearMode(bool enabled) override {
    if (dev_ == nullptr) {
      return;
    }
    freenect_set_flag(dev_, FREENECT_NEAR_MODE, enabled ? FREENECT_ON : FREENECT_OFF);
  }

  void setManualExposureUs(int value) override {
    if (dev_ == nullptr) {
      return;
    }
    const int clamped = std::max(1000, std::min(200000, value));
    freenect_set_exposure(dev_, clamped);
  }

  void setIrBrightness(int value) override {
    if (dev_ == nullptr) {
      return;
    }
    const int clamped = std::max(1, std::min(50, value));
    freenect_set_ir_brightness(dev_, static_cast<uint16_t>(clamped));
  }

  bool setAudioEnabled(bool enabled) override {
    if (!audio_supported_) {
      audio_enabled_ = false;
      return false;
    }
    audio_enabled_ = enabled;
    if (dev_ == nullptr || !running_) {
      return false;
    }
    if (enabled) {
      if (!audio_started_ && freenect_start_audio(dev_) == 0) {
        audio_started_ = true;
      }
    } else if (audio_started_) {
      freenect_stop_audio(dev_);
      audio_started_ = false;
    }
    return audio_started_;
  }

  bool audioEnabled() const override {
    return audio_enabled_ && audio_started_;
  }

  float audioLevel() const override {
    return audio_level_;
  }

  bool supportsMotor() const override {
    return true;
  }

  bool supportsLed() const override {
    return true;
  }

  bool supportsAudioInput() const override {
    return audio_supported_;
  }

  bool supportsIr() const override {
    return true;
  }

 private:
  bool ApplyVideoMode() {
    if (dev_ == nullptr) {
      return false;
    }

    const freenect_video_format format =
        requested_stream_ == StreamKind::kIr ? FREENECT_VIDEO_IR_8BIT : FREENECT_VIDEO_RGB;
    const freenect_frame_mode mode = freenect_find_video_mode(FREENECT_RESOLUTION_MEDIUM, format);
    if (!mode.is_valid) {
      std::cerr << "[kinect-v1] requested video mode is invalid (format=" << static_cast<int>(format) << ")\n";
      return false;
    }

    if (video_started_) {
      freenect_stop_video(dev_);
      video_started_ = false;
    }

    if (freenect_set_video_mode(dev_, mode) < 0) {
      std::cerr << "[kinect-v1] freenect_set_video_mode failed (format=" << static_cast<int>(format) << ")\n";
      return false;
    }

    if (freenect_start_video(dev_) < 0) {
      std::cerr << "[kinect-v1] freenect_start_video failed while applying video mode\n";
      return false;
    }
    video_started_ = true;
    active_stream_ = requested_stream_;
    return true;
  }

  static void OnDepthFrame(freenect_device *dev, void *depth, uint32_t timestamp) {
    auto *self = static_cast<FreenectV1Device *>(freenect_get_user(dev));
    if (self == nullptr || depth == nullptr) {
      return;
    }

    std::lock_guard<std::mutex> lock(self->frame_mutex_);
    self->frame_.width = kWidth;
    self->frame_.height = kHeight;
    self->frame_.timestamp = timestamp;
    self->frame_.depth.resize(kPixelCount);
    std::memcpy(self->frame_.depth.data(), depth, kPixelCount * sizeof(uint16_t));
    self->has_new_frame_ = true;
  }

  static void OnVideoFrame(freenect_device *dev, void *video, uint32_t timestamp) {
    auto *self = static_cast<FreenectV1Device *>(freenect_get_user(dev));
    if (self == nullptr || video == nullptr) {
      return;
    }

    std::lock_guard<std::mutex> lock(self->frame_mutex_);
    self->frame_.width = kWidth;
    self->frame_.height = kHeight;
    self->frame_.timestamp = timestamp;

    if (self->active_stream_ == StreamKind::kIr) {
      self->frame_.ir.resize(kPixelCount);
      std::memcpy(self->frame_.ir.data(), video, kPixelCount);
    } else {
      self->frame_.rgb.resize(kPixelCount * 3);
      std::memcpy(self->frame_.rgb.data(), video, kPixelCount * 3);
    }

    self->has_new_frame_ = true;
  }

  static void OnAudioFrame(
      freenect_device *dev,
      int num_samples,
      int32_t *,
      int32_t *,
      int32_t *,
      int32_t *,
      int16_t *cancelled,
      void *) {
    auto *self = static_cast<FreenectV1Device *>(freenect_get_user(dev));
    if (self == nullptr || cancelled == nullptr || num_samples <= 0) {
      return;
    }

    double energy = 0.0;
    for (int i = 0; i < num_samples; ++i) {
      const double sample = static_cast<double>(cancelled[i]);
      energy += sample * sample;
    }
    energy /= static_cast<double>(num_samples);
    const double rms = std::sqrt(energy) / 32768.0;
    self->audio_level_ = static_cast<float>(rms);
  }

  freenect_context *ctx_ = nullptr;
  freenect_device *dev_ = nullptr;
  int index_ = 0;
  std::string serial_;
  bool audio_supported_ = false;

  bool running_ = false;
  bool depth_started_ = false;
  bool video_started_ = false;
  bool audio_started_ = false;
  bool audio_enabled_ = false;

  StreamKind requested_stream_ = StreamKind::kRgb;
  StreamKind active_stream_ = StreamKind::kRgb;

  mutable std::mutex frame_mutex_;
  FrameData frame_;
  bool has_new_frame_ = false;
  float audio_level_ = 0.0f;
};

class FreenectV1Backend final : public KinectBackend {
 public:
  FreenectV1Backend() {
    if (freenect_init(&ctx_, nullptr) < 0) {
      ctx_ = nullptr;
      return;
    }
    freenect_set_log_level(ctx_, FREENECT_LOG_WARNING);

    firmware_dir_ = FindFirmwareDirectory();
    has_audio_firmware_ = !firmware_dir_.empty();
    if (has_audio_firmware_) {
      setenv("LIBFREENECT_FIRMWARE_PATH", firmware_dir_.c_str(), 1);
    }

    freenect_device_flags selected = static_cast<freenect_device_flags>(FREENECT_DEVICE_CAMERA | FREENECT_DEVICE_MOTOR);
    if (has_audio_firmware_) {
      selected = static_cast<freenect_device_flags>(selected | FREENECT_DEVICE_AUDIO);
    }
    freenect_select_subdevices(ctx_, selected);
  }

  ~FreenectV1Backend() override {
    if (ctx_ != nullptr) {
      freenect_shutdown(ctx_);
      ctx_ = nullptr;
    }
  }

  std::string name() const override {
    return "libfreenect (Kinect v1)";
  }

  KinectGeneration generation() const override {
    return KinectGeneration::kV1;
  }

  ProbeResult probe() override {
    if (ctx_ == nullptr) {
      return {false, "libfreenect initialization failed."};
    }
    const int count = EnumerateCountWithRetries(4, 80);
    if (count < 0) {
      return {false, "Kinect v1 enumeration failed."};
    }
    if (count == 0) {
      if (has_audio_firmware_) {
        return {true, "Backend ready. No Kinect v1 devices are currently attached."};
      }
      return {true, "Backend ready (camera/depth only). No Kinect v1 devices are currently attached."};
    }

    if (!has_audio_firmware_) {
      return {true,
              std::to_string(count) +
                  " Kinect v1 device(s) detected. Audio disabled because audios.bin firmware was not found."};
    }
    return {true, std::to_string(count) + " Kinect v1 device(s) detected."};
  }

  std::vector<DeviceInfo> listDevices() override {
    std::vector<DeviceInfo> devices;
    if (ctx_ == nullptr) {
      return devices;
    }

    for (int attempt = 0; attempt < 4; ++attempt) {
      freenect_device_attributes *attrs = nullptr;
      const int count = freenect_list_device_attributes(ctx_, &attrs);
      if (count > 0 && attrs != nullptr) {
        int index = 0;
        for (auto *cur = attrs; cur != nullptr; cur = cur->next) {
          DeviceInfo info;
          info.generation = KinectGeneration::kV1;
          info.serial = cur->camera_serial != nullptr ? cur->camera_serial : ("DeviceIndex-" + std::to_string(index));
          info.name = "Kinect v1";
          devices.push_back(std::move(info));
          ++index;
        }
        freenect_free_device_attributes(attrs);
        return devices;
      }
      if (attrs != nullptr) {
        freenect_free_device_attributes(attrs);
      }
      if (attempt + 1 < 4) {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
      }
    }

    // Fallback by index only.
    const int fallback_count = std::max(0, EnumerateCountWithRetries(4, 80));
    for (int i = 0; i < fallback_count; ++i) {
      DeviceInfo info;
      info.generation = KinectGeneration::kV1;
      info.serial = "DeviceIndex-" + std::to_string(i);
      info.name = "Kinect v1";
      devices.push_back(std::move(info));
    }
    return devices;
  }

  std::unique_ptr<KinectDevice> openDevice(const std::string &serial) override {
    if (ctx_ == nullptr) {
      return nullptr;
    }

    auto try_open = [&](int index, const std::string &candidate_serial) -> std::unique_ptr<KinectDevice> {
      auto device = std::make_unique<FreenectV1Device>(ctx_, index, candidate_serial, has_audio_firmware_);
      if (!device->open()) {
        return nullptr;
      }
      return device;
    };

    constexpr int kMaxRetries = 6;
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
      if (!serial.empty() && !IsSyntheticIndexSerial(serial)) {
        if (auto device = try_open(-1, serial)) {
          return device;
        }
      }

      const int count = std::max(0, EnumerateCountWithRetries(2, 80));
      std::vector<int> candidates;
      std::unordered_set<int> seen;

      if (serial.rfind("DeviceIndex-", 0) == 0) {
        try {
          const int parsed = std::stoi(serial.substr(12));
          candidates.push_back(parsed);
          seen.insert(parsed);
        } catch (...) {
        }
      } else if (!serial.empty()) {
        const auto devices = listDevices();
        for (std::size_t i = 0; i < devices.size(); ++i) {
          if (devices[i].serial == serial) {
            const int idx = static_cast<int>(i);
            candidates.push_back(idx);
            seen.insert(idx);
            break;
          }
        }
      }

      for (int i = 0; i < count; ++i) {
        if (seen.insert(i).second) {
          candidates.push_back(i);
        }
      }

      for (int index : candidates) {
        if (auto device = try_open(index, serial)) {
          return device;
        }
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    return nullptr;
  }

  PreviewResult preview(std::chrono::seconds duration) override {
    PreviewResult result;
    const auto devices = listDevices();
    if (devices.empty()) {
      result.detail = "No Kinect v1 device available for preview.";
      return result;
    }

    auto device = openDevice(devices[0].serial);
    if (!device || !device->start()) {
      result.detail = "Could not start Kinect v1 preview.";
      return result;
    }

    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) {
      device->update();
      FrameData frame;
      if (device->getFrame(frame)) {
        if (!frame.rgb.empty()) {
          ++result.color_frames;
        }
        if (!frame.depth.empty()) {
          ++result.depth_frames;
        }
      }
    }
    device->stop();

    result.success = (result.color_frames + result.depth_frames) > 0;
    result.detail = result.success ? "Preview captured." : "No frames captured.";
    return result;
  }

 private:
  int EnumerateCountWithRetries(int attempts, int delay_ms) const {
    if (ctx_ == nullptr) {
      return -1;
    }

    const int clamped_attempts = std::max(1, attempts);
    for (int attempt = 0; attempt < clamped_attempts; ++attempt) {
      const int count = freenect_num_devices(ctx_);
      if (count != 0) {
        return count;
      }
      if (attempt + 1 < clamped_attempts) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(0, delay_ms)));
      }
    }
    return 0;
  }

  freenect_context *ctx_ = nullptr;
  bool has_audio_firmware_ = false;
  std::string firmware_dir_;
};

#else

class FreenectV1Backend final : public KinectBackend {
 public:
  std::string name() const override {
    return "libfreenect (Kinect v1)";
  }

  KinectGeneration generation() const override {
    return KinectGeneration::kV1;
  }

  ProbeResult probe() override {
    return {false, "libfreenect is not available in this build."};
  }

  std::vector<DeviceInfo> listDevices() override {
    return {};
  }

  std::unique_ptr<KinectDevice> openDevice(const std::string &) override {
    return nullptr;
  }

  PreviewResult preview(std::chrono::seconds) override {
    return {false, "Kinect v1 preview unavailable.", 0, 0};
  }
};

#endif

}  // namespace

std::unique_ptr<KinectBackend> CreateKinectV1Backend() {
  return std::make_unique<FreenectV1Backend>();
}
