#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

enum class KinectGeneration {
  kV1,
  kV2,
};

inline const char *KinectGenerationLabel(KinectGeneration gen) {
  switch (gen) {
    case KinectGeneration::kV1:
      return "Kinect v1";
    case KinectGeneration::kV2:
      return "Kinect v2";
  }
  return "Unknown";
}

struct ProbeResult {
  bool available;
  std::string detail;
};

struct PreviewResult {
  bool success = false;
  std::string detail;
  std::uint64_t color_frames = 0;
  std::uint64_t depth_frames = 0;
};

struct DeviceInfo {
  KinectGeneration generation;
  std::string serial;
  std::string name;
};

enum class StreamKind {
    kRgb = 0,
    kIr = 1,
    kDepth = 2,
};

struct FrameData {
    std::vector<uint8_t> rgb;
    std::vector<uint16_t> depth;
    std::vector<uint8_t> ir;
    int width = 0;
    int height = 0;
    uint32_t timestamp = 0;
};

class KinectDevice {
public:
    virtual ~KinectDevice() = default;
    
    // Control
    virtual bool start() = 0;
    virtual bool stop() = 0;
    
    // Updates
    virtual bool update() = 0;
    virtual bool getFrame(FrameData& out_frame) = 0;
    
    // Hardware control
    virtual void setTilt(int angle) = 0;
    virtual void setLed(int mode) = 0;

    // Camera/stream settings
    virtual void setStreamKind(StreamKind) {}
    virtual StreamKind streamKind() const { return StreamKind::kRgb; }
    virtual void setMirror(bool) {}
    virtual void setAutoExposure(bool) {}
    virtual void setAutoWhiteBalance(bool) {}
    virtual void setNearMode(bool) {}
    virtual void setManualExposureUs(int) {}
    virtual void setIrBrightness(int) {}

    // Audio controls
    virtual bool setAudioEnabled(bool) { return false; }
    virtual bool audioEnabled() const { return false; }
    virtual float audioLevel() const { return 0.0f; }

    // Capability flags
    virtual bool supportsMotor() const { return false; }
    virtual bool supportsLed() const { return false; }
    virtual bool supportsAudioInput() const { return false; }
    virtual bool supportsDepth() const { return true; }
    virtual bool supportsIr() const { return false; }
};

class KinectBackend {
 public:
  virtual ~KinectBackend() = default;

  virtual std::string name() const = 0;

  virtual KinectGeneration generation() const = 0;

  // Checks if the backend is viable (library loaded, USB devices present).
  virtual ProbeResult probe() = 0;

  // Returns a list of attached devices.
  virtual std::vector<DeviceInfo> listDevices() = 0;

  // Runs a short preview stream on the first available device (blocking).
  virtual PreviewResult preview(std::chrono::seconds duration) = 0;
  
  // Opens a device for live usage
  virtual std::unique_ptr<KinectDevice> openDevice(const std::string& serial) = 0;
};

std::unique_ptr<KinectBackend> CreateKinectV1Backend();
std::unique_ptr<KinectBackend> CreateKinectV2Backend();
