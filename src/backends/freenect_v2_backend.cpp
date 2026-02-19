#include "backends/backend.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if KINECT_HAVE_LIBFREENECT2
#include <libfreenect2/frame_listener_impl.h>
#include <libfreenect2/libfreenect2.hpp>
#include <libfreenect2/logger.h>
#endif

namespace {

#if KINECT_HAVE_LIBFREENECT2

class FreenectV2Device final : public KinectDevice {
 public:
  FreenectV2Device(libfreenect2::Freenect2 *ctx, std::string serial)
      : ctx_(ctx), serial_(std::move(serial)), listener_(libfreenect2::Frame::Color | libfreenect2::Frame::Ir |
                                                         libfreenect2::Frame::Depth) {}

  ~FreenectV2Device() override {
    stop();
    if (dev_ != nullptr) {
      dev_->close();
      dev_ = nullptr;
    }
  }

  bool open() {
    if (ctx_ == nullptr) {
      return false;
    }
    dev_ = ctx_->openDevice(serial_);
    if (dev_ == nullptr) {
      return false;
    }
    dev_->setColorFrameListener(&listener_);
    dev_->setIrAndDepthFrameListener(&listener_);
    return true;
  }

  bool start() override {
    if (dev_ == nullptr || running_) {
      return dev_ != nullptr;
    }
    if (!dev_->start()) {
      return false;
    }
    running_ = true;
    return true;
  }

  bool stop() override {
    if (dev_ == nullptr) {
      return false;
    }
    if (running_) {
      dev_->stop();
      running_ = false;
    }
    return true;
  }

  bool update() override {
    if (!running_) {
      return false;
    }

    libfreenect2::FrameMap frames;
    if (!listener_.waitForNewFrame(frames, 1)) {
      return false;
    }

    std::vector<std::uint8_t> rgb_data;
    std::vector<std::uint16_t> depth_data;
    std::vector<std::uint8_t> ir_data;
    int rgb_w = 0;
    int rgb_h = 0;
    int depth_w = 0;
    int depth_h = 0;
    int ir_w = 0;
    int ir_h = 0;
    std::uint32_t rgb_ts = 0;
    std::uint32_t depth_ts = 0;
    std::uint32_t ir_ts = 0;

    if (frames.count(libfreenect2::Frame::Color) > 0) {
      auto *color = frames[libfreenect2::Frame::Color];
      rgb_w = color->width;
      rgb_h = color->height;
      rgb_ts = color->timestamp;

      // libfreenect2 color frame is typically BGRA. Convert to RGB for UI.
      rgb_data.resize(static_cast<std::size_t>(color->width) * color->height * 3);
      const std::uint8_t *src = color->data;
      for (int i = 0; i < color->width * color->height; ++i) {
        rgb_data[i * 3 + 0] = src[i * 4 + 2];
        rgb_data[i * 3 + 1] = src[i * 4 + 1];
        rgb_data[i * 3 + 2] = src[i * 4 + 0];
      }
    }

    if (frames.count(libfreenect2::Frame::Depth) > 0) {
      auto *depth = frames[libfreenect2::Frame::Depth];
      depth_w = depth->width;
      depth_h = depth->height;
      depth_ts = depth->timestamp;
      depth_data.resize(static_cast<std::size_t>(depth->width) * depth->height);
      const float *src = reinterpret_cast<const float *>(depth->data);
      for (int i = 0; i < depth->width * depth->height; ++i) {
        const float mm = src[i];
        depth_data[i] = static_cast<std::uint16_t>(std::max(0.0f, std::min(65535.0f, mm)));
      }
    }

    if (frames.count(libfreenect2::Frame::Ir) > 0) {
      auto *ir = frames[libfreenect2::Frame::Ir];
      ir_w = ir->width;
      ir_h = ir->height;
      ir_ts = ir->timestamp;
      ir_data.resize(static_cast<std::size_t>(ir->width) * ir->height);
      const float *src = reinterpret_cast<const float *>(ir->data);
      for (int i = 0; i < ir->width * ir->height; ++i) {
        const float v = src[i] / 65535.0f;
        ir_data[i] = static_cast<std::uint8_t>(std::max(0.0f, std::min(255.0f, v * 255.0f)));
      }
    }

    FrameData next_frame;
    auto assign_rgb = [&]() -> bool {
      if (rgb_data.empty() || rgb_w <= 0 || rgb_h <= 0) {
        return false;
      }
      next_frame.rgb = std::move(rgb_data);
      next_frame.width = rgb_w;
      next_frame.height = rgb_h;
      next_frame.timestamp = rgb_ts;
      return true;
    };
    auto assign_depth = [&]() -> bool {
      if (depth_data.empty() || depth_w <= 0 || depth_h <= 0) {
        return false;
      }
      next_frame.depth = std::move(depth_data);
      next_frame.width = depth_w;
      next_frame.height = depth_h;
      next_frame.timestamp = depth_ts;
      return true;
    };
    auto assign_ir = [&]() -> bool {
      if (ir_data.empty() || ir_w <= 0 || ir_h <= 0) {
        return false;
      }
      next_frame.ir = std::move(ir_data);
      next_frame.width = ir_w;
      next_frame.height = ir_h;
      next_frame.timestamp = ir_ts;
      return true;
    };

    bool assigned = false;
    if (selected_stream_ == StreamKind::kRgb) {
      assigned = assign_rgb();
    } else if (selected_stream_ == StreamKind::kIr) {
      assigned = assign_ir();
    } else if (selected_stream_ == StreamKind::kDepth) {
      assigned = assign_depth();
    }
    if (!assigned) {
      assigned = assign_rgb() || assign_ir() || assign_depth();
    }

    if (assigned) {
      frame_ = std::move(next_frame);
      has_new_frame_ = true;
    }

    listener_.release(frames);
    return assigned;
  }

  bool getFrame(FrameData &out_frame) override {
    if (!has_new_frame_) {
      return false;
    }
    out_frame = frame_;
    has_new_frame_ = false;
    return true;
  }

  bool supportsDepth() const override {
    return true;
  }

  bool supportsIr() const override {
    return true;
  }

  void setStreamKind(StreamKind kind) override {
    selected_stream_ = kind;
  }

  StreamKind streamKind() const override {
    return selected_stream_;
  }

  void setTilt(int) override {}
  void setLed(int) override {}

 private:
  libfreenect2::Freenect2 *ctx_ = nullptr;
  std::string serial_;
  libfreenect2::Freenect2Device *dev_ = nullptr;
  libfreenect2::SyncMultiFrameListener listener_;

  FrameData frame_;
  bool has_new_frame_ = false;
  bool running_ = false;
  StreamKind selected_stream_ = StreamKind::kRgb;
};

class FreenectV2Backend final : public KinectBackend {
 public:
  FreenectV2Backend() {
    libfreenect2::setGlobalLogger(libfreenect2::createConsoleLogger(libfreenect2::Logger::Warning));
  }

  std::string name() const override {
    return "libfreenect2 (Kinect v2)";
  }

  KinectGeneration generation() const override {
    return KinectGeneration::kV2;
  }

  ProbeResult probe() override {
    const int count = ctx_.enumerateDevices();
    if (count <= 0) {
      return {false, "No Kinect v2 devices found."};
    }
    return {true, std::to_string(count) + " Kinect v2 device(s) detected."};
  }

  std::vector<DeviceInfo> listDevices() override {
    std::vector<DeviceInfo> devices;
    const int count = ctx_.enumerateDevices();
    for (int i = 0; i < count; ++i) {
      DeviceInfo info;
      info.generation = KinectGeneration::kV2;
      info.serial = ctx_.getDeviceSerialNumber(i);
      info.name = "Kinect v2";
      devices.push_back(std::move(info));
    }
    return devices;
  }

  std::unique_ptr<KinectDevice> openDevice(const std::string &serial) override {
    if (serial.empty()) {
      return nullptr;
    }
    auto device = std::make_unique<FreenectV2Device>(&ctx_, serial);
    if (!device->open()) {
      return nullptr;
    }
    return device;
  }

  PreviewResult preview(std::chrono::seconds duration) override {
    PreviewResult result;
    const auto devices = listDevices();
    if (devices.empty()) {
      result.detail = "No Kinect v2 device available for preview.";
      return result;
    }
    auto device = openDevice(devices[0].serial);
    if (!device || !device->start()) {
      result.detail = "Could not start Kinect v2 preview.";
      return result;
    }
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) {
      if (device->update()) {
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
    }
    device->stop();
    result.success = (result.color_frames + result.depth_frames) > 0;
    result.detail = result.success ? "Preview captured." : "No frames captured.";
    return result;
  }

 private:
  libfreenect2::Freenect2 ctx_;
};

#else

class FreenectV2Backend final : public KinectBackend {
 public:
  std::string name() const override {
    return "libfreenect2 (Kinect v2)";
  }

  KinectGeneration generation() const override {
    return KinectGeneration::kV2;
  }

  ProbeResult probe() override {
    return {false, "libfreenect2 is not available in this build."};
  }

  std::vector<DeviceInfo> listDevices() override {
    return {};
  }

  std::unique_ptr<KinectDevice> openDevice(const std::string &) override {
    return nullptr;
  }

  PreviewResult preview(std::chrono::seconds) override {
    return {false, "Kinect v2 preview unavailable.", 0, 0};
  }
};

#endif

}  // namespace

std::unique_ptr<KinectBackend> CreateKinectV2Backend() {
  return std::make_unique<FreenectV2Backend>();
}
