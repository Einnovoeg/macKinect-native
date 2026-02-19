#include "../backends/backend.h"

#include <CoreFoundation/CFPlugIn.h>
#include <CoreMediaIO/CMIOHardwarePlugIn.h>
#include <CoreMediaIO/CMIOHardwareSystem.h>
#include <CoreMediaIO/CMIOHardwareDevice.h>
#include <CoreMediaIO/CMIOHardwareStream.h>
#include <CoreMedia/CMFormatDescription.h>
#include <CoreMedia/CMSampleBuffer.h>
#include <CoreMedia/CMSimpleQueue.h>
#include <CoreVideo/CVPixelBuffer.h>
#include <CoreAudio/AudioHardwareBase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr AudioObjectID kObjectIDDevice = 2;
constexpr AudioObjectID kObjectIDStream = 3;

constexpr const char* kPluginName = "macKinect Camera DAL";
constexpr const char* kManufacturerName = "macKinect";
constexpr const char* kPluginBundleID = "com.mackinect.cameradal";
constexpr const char* kDeviceUID = "com.mackinect.cameradal.device";
constexpr const char* kModelUID = "com.mackinect.cameradal.model";

constexpr int kOutputWidth = 640;
constexpr int kOutputHeight = 480;
constexpr int kOutputFPS = 30;
constexpr std::size_t kQueueCapacity = 8;

std::atomic<ULONG> gRefCount{1};
CMIOHardwarePlugInRef gPlugInRef = nullptr;
CMIOObjectID gPlugInObjectID = kCMIOObjectUnknown;
CMIODeviceID gDeviceObjectID = kCMIODeviceUnknown;
CMIOStreamID gStreamObjectID = kCMIOStreamUnknown;

std::mutex gStateMutex;
CMSimpleQueueRef gSampleQueue = nullptr;
CMIODeviceStreamQueueAlteredProc gQueueAlteredProc = nullptr;
void* gQueueAlteredRefCon = nullptr;
CMFormatDescriptionRef gFormatDescription = nullptr;
std::thread gProducerThread;
std::atomic<bool> gProducerRunning{false};
std::atomic<UInt32> gRunningClients{0};
std::atomic<uint64_t> gFrameCounter{0};

class KinectFrameSource {
 public:
  bool start() {
    stop();

    auto try_backend = [&](std::unique_ptr<KinectBackend> backend) -> bool {
      if (!backend) {
        return false;
      }
      if (!backend->probe().available) {
        return false;
      }

      auto devices = backend->listDevices();
      if (devices.empty()) {
        return false;
      }

      auto device = backend->openDevice(devices.front().serial);
      if (!device) {
        return false;
      }

      device->setStreamKind(StreamKind::kRgb);
      if (!device->start()) {
        return false;
      }

      backend_ = std::move(backend);
      device_ = std::move(device);
      return true;
    };

    if (try_backend(CreateKinectV2Backend())) {
      return true;
    }
    if (try_backend(CreateKinectV1Backend())) {
      return true;
    }

    return false;
  }

  void stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (device_) {
      device_->stop();
      device_.reset();
    }
    backend_.reset();
  }

  bool nextRGB(std::vector<uint8_t>& rgb, int& width, int& height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_) {
      return false;
    }

    device_->setStreamKind(StreamKind::kRgb);
    if (!device_->update()) {
      return false;
    }

    FrameData frame;
    if (!device_->getFrame(frame) || frame.rgb.empty() || frame.width <= 0 || frame.height <= 0) {
      return false;
    }

    rgb = std::move(frame.rgb);
    width = frame.width;
    height = frame.height;
    return true;
  }

 private:
  std::mutex mutex_;
  std::unique_ptr<KinectBackend> backend_;
  std::unique_ptr<KinectDevice> device_;
};

KinectFrameSource gKinectSource;

CFStringRef CopyCFString(const char* value) {
  return CFStringCreateWithCString(nullptr, value, kCFStringEncodingUTF8);
}

bool IsInputScope(const CMIOObjectPropertyAddress* address) {
  if (address == nullptr) {
    return false;
  }
  return address->mScope == kCMIODevicePropertyScopeInput || address->mScope == kCMIOObjectPropertyScopeGlobal;
}

bool EnsureFormatDescriptionLocked() {
  if (gFormatDescription != nullptr) {
    return true;
  }

  CMFormatDescriptionRef description = nullptr;
  const OSStatus rc =
      CMVideoFormatDescriptionCreate(nullptr, kCVPixelFormatType_32BGRA, kOutputWidth, kOutputHeight, nullptr, &description);
  if (rc != noErr || description == nullptr) {
    return false;
  }
  gFormatDescription = description;
  return true;
}

void FillFallbackPattern(std::uint8_t* base, std::size_t bytes_per_row, uint64_t frame_index) {
  for (int y = 0; y < kOutputHeight; ++y) {
    auto* row = base + y * bytes_per_row;
    for (int x = 0; x < kOutputWidth; ++x) {
      const std::uint8_t r = static_cast<std::uint8_t>((x + frame_index) % 256);
      const std::uint8_t g = static_cast<std::uint8_t>((y + frame_index * 2) % 256);
      const std::uint8_t b = static_cast<std::uint8_t>((x + y + frame_index * 3) % 256);
      row[x * 4 + 0] = b;
      row[x * 4 + 1] = g;
      row[x * 4 + 2] = r;
      row[x * 4 + 3] = 255;
    }
  }
}

void FillFromRGB(
    const std::vector<uint8_t>& rgb,
    int src_width,
    int src_height,
    std::uint8_t* base,
    std::size_t bytes_per_row) {
  if (src_width <= 0 || src_height <= 0 || rgb.size() < static_cast<std::size_t>(src_width * src_height * 3)) {
    FillFallbackPattern(base, bytes_per_row, gFrameCounter.load());
    return;
  }

  for (int y = 0; y < kOutputHeight; ++y) {
    const int sy = (y * src_height) / kOutputHeight;
    auto* row = base + y * bytes_per_row;
    for (int x = 0; x < kOutputWidth; ++x) {
      const int sx = (x * src_width) / kOutputWidth;
      const std::size_t si = static_cast<std::size_t>((sy * src_width + sx) * 3);
      row[x * 4 + 0] = rgb[si + 2];
      row[x * 4 + 1] = rgb[si + 1];
      row[x * 4 + 2] = rgb[si + 0];
      row[x * 4 + 3] = 255;
    }
  }
}

CMSampleBufferRef CreateSampleBuffer(uint64_t frame_index) {
  CVPixelBufferRef pixel_buffer = nullptr;
  const OSStatus pixel_rc =
      CVPixelBufferCreate(kCFAllocatorDefault, kOutputWidth, kOutputHeight, kCVPixelFormatType_32BGRA, nullptr, &pixel_buffer);
  if (pixel_rc != kCVReturnSuccess || pixel_buffer == nullptr) {
    return nullptr;
  }

  if (CVPixelBufferLockBaseAddress(pixel_buffer, 0) != kCVReturnSuccess) {
    CFRelease(pixel_buffer);
    return nullptr;
  }

  auto* base = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(pixel_buffer));
  const std::size_t bytes_per_row = static_cast<std::size_t>(CVPixelBufferGetBytesPerRow(pixel_buffer));

  std::vector<std::uint8_t> rgb;
  int src_width = 0;
  int src_height = 0;
  if (gKinectSource.nextRGB(rgb, src_width, src_height)) {
    FillFromRGB(rgb, src_width, src_height, base, bytes_per_row);
  } else {
    FillFallbackPattern(base, bytes_per_row, frame_index);
  }

  CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);

  std::lock_guard<std::mutex> lock(gStateMutex);
  if (!EnsureFormatDescriptionLocked()) {
    CFRelease(pixel_buffer);
    return nullptr;
  }

  CMSampleTimingInfo timing{};
  timing.duration = CMTimeMake(1, kOutputFPS);
  timing.presentationTimeStamp = CMTimeMake(static_cast<int64_t>(frame_index), kOutputFPS);
  timing.decodeTimeStamp = kCMTimeInvalid;

  CMSampleBufferRef sample_buffer = nullptr;
  const OSStatus sample_rc = CMSampleBufferCreateForImageBuffer(
      kCFAllocatorDefault, pixel_buffer, true, nullptr, nullptr, gFormatDescription, &timing, &sample_buffer);
  CFRelease(pixel_buffer);

  if (sample_rc != noErr) {
    return nullptr;
  }
  return sample_buffer;
}

void FlushQueueLocked() {
  if (gSampleQueue == nullptr) {
    return;
  }
  while (CMSimpleQueueGetCount(gSampleQueue) > 0) {
    const void* raw_token = CMSimpleQueueDequeue(gSampleQueue);
    auto* token = reinterpret_cast<CMSampleBufferRef>(const_cast<void*>(raw_token));
    if (token != nullptr) {
      CFRelease(token);
    }
  }
}

void NotifyDeviceRunningChanged() {
  if (gPlugInRef == nullptr || gDeviceObjectID == kCMIODeviceUnknown) {
    return;
  }
  const CMIOObjectPropertyAddress addresses[] = {
      {kCMIODevicePropertyDeviceIsRunning, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMain},
      {kCMIODevicePropertyDeviceIsRunningSomewhere, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMain},
  };
  CMIOObjectPropertiesChanged(gPlugInRef, gDeviceObjectID, 2, addresses);
}

void ProducerLoop() {
  using clock = std::chrono::steady_clock;
  const auto frame_interval = std::chrono::milliseconds(1000 / kOutputFPS);
  auto next_frame_time = clock::now();

  while (gProducerRunning.load(std::memory_order_acquire)) {
    const uint64_t frame_index = gFrameCounter.fetch_add(1, std::memory_order_relaxed);
    CMSampleBufferRef sample = CreateSampleBuffer(frame_index);
    if (sample != nullptr) {
      std::lock_guard<std::mutex> lock(gStateMutex);
      if (gSampleQueue != nullptr) {
        while (CMSimpleQueueGetCount(gSampleQueue) >= static_cast<int32_t>(kQueueCapacity)) {
          const void* raw_old_sample = CMSimpleQueueDequeue(gSampleQueue);
          auto* old_sample = reinterpret_cast<CMSampleBufferRef>(const_cast<void*>(raw_old_sample));
          if (old_sample != nullptr) {
            CFRelease(old_sample);
          }
        }

        const OSStatus enqueue_rc = CMSimpleQueueEnqueue(gSampleQueue, sample);
        if (enqueue_rc == noErr) {
          if (gQueueAlteredProc != nullptr) {
            gQueueAlteredProc(gStreamObjectID, sample, gQueueAlteredRefCon);
          }
          sample = nullptr;
        }
      }
    }
    if (sample != nullptr) {
      CFRelease(sample);
    }

    next_frame_time += frame_interval;
    std::this_thread::sleep_until(next_frame_time);
  }
}

void StartProducingIfNeeded() {
  const UInt32 previous = gRunningClients.fetch_add(1, std::memory_order_acq_rel);
  if (previous != 0) {
    NotifyDeviceRunningChanged();
    return;
  }

  gFrameCounter.store(0, std::memory_order_release);
  gKinectSource.start();
  gProducerRunning.store(true, std::memory_order_release);
  gProducerThread = std::thread(ProducerLoop);
  NotifyDeviceRunningChanged();
}

void StopProducingIfNeeded() {
  const UInt32 previous = gRunningClients.fetch_sub(1, std::memory_order_acq_rel);
  if (previous > 1) {
    NotifyDeviceRunningChanged();
    return;
  }

  gProducerRunning.store(false, std::memory_order_release);
  if (gProducerThread.joinable()) {
    gProducerThread.join();
  }
  gKinectSource.stop();

  std::lock_guard<std::mutex> lock(gStateMutex);
  FlushQueueLocked();
  NotifyDeviceRunningChanged();
}

void TeardownObjects() {
  gProducerRunning.store(false, std::memory_order_release);
  if (gProducerThread.joinable()) {
    gProducerThread.join();
  }
  gRunningClients.store(0, std::memory_order_release);
  gKinectSource.stop();

  std::lock_guard<std::mutex> lock(gStateMutex);
  FlushQueueLocked();
  if (gSampleQueue != nullptr) {
    CFRelease(gSampleQueue);
    gSampleQueue = nullptr;
  }
  gQueueAlteredProc = nullptr;
  gQueueAlteredRefCon = nullptr;
  if (gFormatDescription != nullptr) {
    CFRelease(gFormatDescription);
    gFormatDescription = nullptr;
  }
}

Boolean HasPropertyForPlugIn(const CMIOObjectPropertyAddress* address) {
  switch (address->mSelector) {
    case kCMIOObjectPropertyClass:
    case kCMIOObjectPropertyOwner:
    case kCMIOObjectPropertyCreator:
    case kCMIOObjectPropertyName:
    case kCMIOObjectPropertyManufacturer:
    case kCMIOObjectPropertyOwnedObjects:
    case kCMIOPlugInPropertyBundleID:
    case kCMIOPlugInPropertyIsExtension:
      return true;
    default:
      return false;
  }
}

Boolean HasPropertyForDevice(const CMIOObjectPropertyAddress* address) {
  switch (address->mSelector) {
    case kCMIOObjectPropertyClass:
    case kCMIOObjectPropertyOwner:
    case kCMIOObjectPropertyCreator:
    case kCMIOObjectPropertyName:
    case kCMIOObjectPropertyManufacturer:
    case kCMIOObjectPropertyOwnedObjects:
    case kCMIODevicePropertyDeviceUID:
    case kCMIODevicePropertyModelUID:
    case kCMIODevicePropertyTransportType:
    case kCMIODevicePropertyDeviceIsAlive:
    case kCMIODevicePropertyDeviceIsRunning:
    case kCMIODevicePropertyDeviceIsRunningSomewhere:
    case kCMIODevicePropertySuspendedByUser:
    case kCMIODevicePropertyHogMode:
    case kCMIODevicePropertyLatency:
    case kCMIODevicePropertyStreams:
    case kCMIODevicePropertyStreamConfiguration:
    case kCMIODevicePropertyCanProcessAVCCommand:
    case kCMIODevicePropertyCanProcessRS422Command:
    case kCMIODevicePropertyExcludeNonDALAccess:
      return true;
    default:
      return false;
  }
}

Boolean HasPropertyForStream(const CMIOObjectPropertyAddress* address) {
  switch (address->mSelector) {
    case kCMIOObjectPropertyClass:
    case kCMIOObjectPropertyOwner:
    case kCMIOObjectPropertyCreator:
    case kCMIOObjectPropertyName:
    case kCMIOObjectPropertyManufacturer:
    case kCMIOObjectPropertyOwnedObjects:
    case kCMIOStreamPropertyDirection:
    case kCMIOStreamPropertyTerminalType:
    case kCMIOStreamPropertyStartingChannel:
    case kCMIOStreamPropertyLatency:
    case kCMIOStreamPropertyFormatDescription:
    case kCMIOStreamPropertyFormatDescriptions:
    case kCMIOStreamPropertyFrameRate:
    case kCMIOStreamPropertyFrameRates:
    case kCMIOStreamPropertyFrameRateRanges:
    case kCMIOStreamPropertyNoDataEventCount:
    case kCMIOStreamPropertyNoDataTimeoutInMSec:
    case kCMIOStreamPropertyCanProcessDeckCommand:
    case kCMIOStreamPropertyEndOfData:
      return true;
    default:
      return false;
  }
}

HRESULT STDMETHODCALLTYPE PlugInQueryInterface(void*, REFIID, LPVOID* out_interface);
ULONG STDMETHODCALLTYPE PlugInAddRef(void*);
ULONG STDMETHODCALLTYPE PlugInRelease(void*);
OSStatus PlugInInitialize(CMIOHardwarePlugInRef self);
OSStatus PlugInInitializeWithObjectID(CMIOHardwarePlugInRef self, CMIOObjectID object_id);
OSStatus PlugInTeardown(CMIOHardwarePlugInRef self);
void PlugInObjectShow(CMIOHardwarePlugInRef self, CMIOObjectID object_id);
Boolean PlugInObjectHasProperty(CMIOHardwarePlugInRef self, CMIOObjectID object_id, const CMIOObjectPropertyAddress* address);
OSStatus PlugInObjectIsPropertySettable(
    CMIOHardwarePlugInRef self,
    CMIOObjectID object_id,
    const CMIOObjectPropertyAddress* address,
    Boolean* is_settable);
OSStatus PlugInObjectGetPropertyDataSize(
    CMIOHardwarePlugInRef self,
    CMIOObjectID object_id,
    const CMIOObjectPropertyAddress* address,
    UInt32 qualifier_data_size,
    const void* qualifier_data,
    UInt32* data_size);
OSStatus PlugInObjectGetPropertyData(
    CMIOHardwarePlugInRef self,
    CMIOObjectID object_id,
    const CMIOObjectPropertyAddress* address,
    UInt32 qualifier_data_size,
    const void* qualifier_data,
    UInt32 data_size,
    UInt32* data_used,
    void* data);
OSStatus PlugInObjectSetPropertyData(
    CMIOHardwarePlugInRef self,
    CMIOObjectID object_id,
    const CMIOObjectPropertyAddress* address,
    UInt32 qualifier_data_size,
    const void* qualifier_data,
    UInt32 data_size,
    const void* data);
OSStatus PlugInDeviceSuspend(CMIOHardwarePlugInRef self, CMIODeviceID device_id);
OSStatus PlugInDeviceResume(CMIOHardwarePlugInRef self, CMIODeviceID device_id);
OSStatus PlugInDeviceStartStream(CMIOHardwarePlugInRef self, CMIODeviceID device_id, CMIOStreamID stream_id);
OSStatus PlugInDeviceStopStream(CMIOHardwarePlugInRef self, CMIODeviceID device_id, CMIOStreamID stream_id);
OSStatus PlugInDeviceProcessAVCCommand(CMIOHardwarePlugInRef self, CMIODeviceID device_id, CMIODeviceAVCCommand* io_avc_command);
OSStatus PlugInDeviceProcessRS422Command(
    CMIOHardwarePlugInRef self,
    CMIODeviceID device_id,
    CMIODeviceRS422Command* io_rs422_command);
OSStatus PlugInStreamCopyBufferQueue(
    CMIOHardwarePlugInRef self,
    CMIOStreamID stream_id,
    CMIODeviceStreamQueueAlteredProc queue_altered_proc,
    void* queue_altered_ref_con,
    CMSimpleQueueRef* queue);
OSStatus PlugInStreamDeckPlay(CMIOHardwarePlugInRef self, CMIOStreamID stream_id);
OSStatus PlugInStreamDeckStop(CMIOHardwarePlugInRef self, CMIOStreamID stream_id);
OSStatus PlugInStreamDeckJog(CMIOHardwarePlugInRef self, CMIOStreamID stream_id, SInt32 speed);
OSStatus PlugInStreamDeckCueTo(CMIOHardwarePlugInRef self, CMIOStreamID stream_id, Float64 frame_number, Boolean play_on_cue);

CMIOHardwarePlugInInterface gPlugInInterface = {
    nullptr,
    PlugInQueryInterface,
    PlugInAddRef,
    PlugInRelease,
    PlugInInitialize,
    PlugInInitializeWithObjectID,
    PlugInTeardown,
    PlugInObjectShow,
    PlugInObjectHasProperty,
    PlugInObjectIsPropertySettable,
    PlugInObjectGetPropertyDataSize,
    PlugInObjectGetPropertyData,
    PlugInObjectSetPropertyData,
    PlugInDeviceSuspend,
    PlugInDeviceResume,
    PlugInDeviceStartStream,
    PlugInDeviceStopStream,
    PlugInDeviceProcessAVCCommand,
    PlugInDeviceProcessRS422Command,
    PlugInStreamCopyBufferQueue,
    PlugInStreamDeckPlay,
    PlugInStreamDeckStop,
    PlugInStreamDeckJog,
    PlugInStreamDeckCueTo,
};

CMIOHardwarePlugInInterface* gPlugInInterfacePtr = &gPlugInInterface;

HRESULT STDMETHODCALLTYPE PlugInQueryInterface(void*, REFIID, LPVOID* out_interface) {
  if (out_interface == nullptr) {
    return E_POINTER;
  }
  *out_interface = &gPlugInInterfacePtr;
  PlugInAddRef(nullptr);
  return S_OK;
}

ULONG STDMETHODCALLTYPE PlugInAddRef(void*) {
  return ++gRefCount;
}

ULONG STDMETHODCALLTYPE PlugInRelease(void*) {
  return --gRefCount;
}

OSStatus PlugInInitialize(CMIOHardwarePlugInRef self) {
  gPlugInRef = self;
  return noErr;
}

OSStatus PlugInInitializeWithObjectID(CMIOHardwarePlugInRef self, CMIOObjectID object_id) {
  gPlugInRef = self;
  gPlugInObjectID = object_id;

  if (gDeviceObjectID != kCMIODeviceUnknown && gStreamObjectID != kCMIOStreamUnknown) {
    return noErr;
  }

  OSStatus rc = CMIOObjectCreate(self, gPlugInObjectID, kCMIODeviceClassID, &gDeviceObjectID);
  if (rc != noErr) {
    gDeviceObjectID = kCMIODeviceUnknown;
    return rc;
  }

  rc = CMIOObjectCreate(self, gDeviceObjectID, kCMIOStreamClassID, &gStreamObjectID);
  if (rc != noErr) {
    gStreamObjectID = kCMIOStreamUnknown;
    return rc;
  }

  CMIOObjectID published_device = gDeviceObjectID;
  rc = CMIOObjectsPublishedAndDied(self, gPlugInObjectID, 1, &published_device, 0, nullptr);
  if (rc != noErr) {
    return rc;
  }

  CMIOObjectID published_stream = gStreamObjectID;
  return CMIOObjectsPublishedAndDied(self, gDeviceObjectID, 1, &published_stream, 0, nullptr);
}

OSStatus PlugInTeardown(CMIOHardwarePlugInRef self) {
  TeardownObjects();

  if (gStreamObjectID != kCMIOStreamUnknown) {
    CMIOObjectID dead_stream = gStreamObjectID;
    CMIOObjectsPublishedAndDied(self, gDeviceObjectID, 0, nullptr, 1, &dead_stream);
  }
  if (gDeviceObjectID != kCMIODeviceUnknown) {
    CMIOObjectID dead_device = gDeviceObjectID;
    CMIOObjectsPublishedAndDied(self, gPlugInObjectID, 0, nullptr, 1, &dead_device);
  }

  gStreamObjectID = kCMIOStreamUnknown;
  gDeviceObjectID = kCMIODeviceUnknown;
  return noErr;
}

void PlugInObjectShow(CMIOHardwarePlugInRef, CMIOObjectID) {}

Boolean PlugInObjectHasProperty(CMIOHardwarePlugInRef, CMIOObjectID object_id, const CMIOObjectPropertyAddress* address) {
  if (address == nullptr) {
    return false;
  }
  if (object_id == gPlugInObjectID) {
    return HasPropertyForPlugIn(address);
  }
  if (object_id == gDeviceObjectID) {
    return HasPropertyForDevice(address);
  }
  if (object_id == gStreamObjectID) {
    return HasPropertyForStream(address);
  }
  return false;
}

OSStatus PlugInObjectIsPropertySettable(
    CMIOHardwarePlugInRef,
    CMIOObjectID object_id,
    const CMIOObjectPropertyAddress* address,
    Boolean* is_settable) {
  if (address == nullptr || is_settable == nullptr) {
    return kCMIOHardwareIllegalOperationError;
  }
  if (!PlugInObjectHasProperty(nullptr, object_id, address)) {
    return kCMIOHardwareUnknownPropertyError;
  }
  *is_settable = false;
  return noErr;
}

OSStatus PlugInObjectGetPropertyDataSize(
    CMIOHardwarePlugInRef,
    CMIOObjectID object_id,
    const CMIOObjectPropertyAddress* address,
    UInt32,
    const void*,
    UInt32* data_size) {
  if (address == nullptr || data_size == nullptr) {
    return kCMIOHardwareIllegalOperationError;
  }

  if (object_id == gPlugInObjectID) {
    switch (address->mSelector) {
      case kCMIOObjectPropertyClass:
      case kCMIOObjectPropertyOwner:
        *data_size = sizeof(CMIOObjectID);
        return noErr;
      case kCMIOObjectPropertyCreator:
      case kCMIOObjectPropertyName:
      case kCMIOObjectPropertyManufacturer:
      case kCMIOPlugInPropertyBundleID:
        *data_size = sizeof(CFStringRef);
        return noErr;
      case kCMIOObjectPropertyOwnedObjects:
        *data_size = gDeviceObjectID == kCMIODeviceUnknown ? 0 : sizeof(CMIOObjectID);
        return noErr;
      case kCMIOPlugInPropertyIsExtension:
        *data_size = sizeof(UInt32);
        return noErr;
      default:
        return kCMIOHardwareUnknownPropertyError;
    }
  }

  if (object_id == gDeviceObjectID) {
    switch (address->mSelector) {
      case kCMIOObjectPropertyClass:
      case kCMIOObjectPropertyOwner:
      case kCMIODevicePropertyTransportType:
      case kCMIODevicePropertyDeviceIsAlive:
      case kCMIODevicePropertyDeviceIsRunning:
      case kCMIODevicePropertyDeviceIsRunningSomewhere:
      case kCMIODevicePropertySuspendedByUser:
      case kCMIODevicePropertyHogMode:
      case kCMIODevicePropertyLatency:
      case kCMIODevicePropertyCanProcessAVCCommand:
      case kCMIODevicePropertyCanProcessRS422Command:
      case kCMIODevicePropertyExcludeNonDALAccess:
        *data_size = sizeof(UInt32);
        return noErr;
      case kCMIOObjectPropertyCreator:
      case kCMIOObjectPropertyName:
      case kCMIOObjectPropertyManufacturer:
      case kCMIODevicePropertyDeviceUID:
      case kCMIODevicePropertyModelUID:
        *data_size = sizeof(CFStringRef);
        return noErr;
      case kCMIOObjectPropertyOwnedObjects:
        *data_size = gStreamObjectID == kCMIOStreamUnknown ? 0 : sizeof(CMIOObjectID);
        return noErr;
      case kCMIODevicePropertyStreams:
        *data_size = IsInputScope(address) && gStreamObjectID != kCMIOStreamUnknown ? sizeof(CMIOObjectID) : 0;
        return noErr;
      case kCMIODevicePropertyStreamConfiguration:
        *data_size = IsInputScope(address) ? sizeof(CMIODeviceStreamConfiguration) + sizeof(UInt32) : sizeof(CMIODeviceStreamConfiguration);
        return noErr;
      default:
        return kCMIOHardwareUnknownPropertyError;
    }
  }

  if (object_id == gStreamObjectID) {
    switch (address->mSelector) {
      case kCMIOObjectPropertyClass:
      case kCMIOObjectPropertyOwner:
      case kCMIOStreamPropertyDirection:
      case kCMIOStreamPropertyTerminalType:
      case kCMIOStreamPropertyStartingChannel:
      case kCMIOStreamPropertyLatency:
      case kCMIOStreamPropertyNoDataEventCount:
      case kCMIOStreamPropertyNoDataTimeoutInMSec:
      case kCMIOStreamPropertyCanProcessDeckCommand:
      case kCMIOStreamPropertyEndOfData:
        *data_size = sizeof(UInt32);
        return noErr;
      case kCMIOObjectPropertyCreator:
      case kCMIOObjectPropertyName:
      case kCMIOObjectPropertyManufacturer:
        *data_size = sizeof(CFStringRef);
        return noErr;
      case kCMIOObjectPropertyOwnedObjects:
        *data_size = 0;
        return noErr;
      case kCMIOStreamPropertyFormatDescription:
        *data_size = sizeof(CMFormatDescriptionRef);
        return noErr;
      case kCMIOStreamPropertyFormatDescriptions:
        *data_size = sizeof(CFArrayRef);
        return noErr;
      case kCMIOStreamPropertyFrameRate:
        *data_size = sizeof(Float64);
        return noErr;
      case kCMIOStreamPropertyFrameRates:
        *data_size = sizeof(Float64);
        return noErr;
      case kCMIOStreamPropertyFrameRateRanges:
        *data_size = sizeof(AudioValueRange);
        return noErr;
      default:
        return kCMIOHardwareUnknownPropertyError;
    }
  }

  return kCMIOHardwareBadObjectError;
}

template <typename T>
OSStatus WriteScalar(UInt32 in_data_size, UInt32* out_data_used, void* out_data, const T& value) {
  if (in_data_size < sizeof(T)) {
    return kCMIOHardwareBadPropertySizeError;
  }
  *reinterpret_cast<T*>(out_data) = value;
  if (out_data_used != nullptr) {
    *out_data_used = sizeof(T);
  }
  return noErr;
}

OSStatus PlugInObjectGetPropertyData(
    CMIOHardwarePlugInRef,
    CMIOObjectID object_id,
    const CMIOObjectPropertyAddress* address,
    UInt32,
    const void*,
    UInt32 data_size,
    UInt32* data_used,
    void* data) {
  if (address == nullptr || data == nullptr) {
    return kCMIOHardwareIllegalOperationError;
  }

  if (object_id == gPlugInObjectID) {
    switch (address->mSelector) {
      case kCMIOObjectPropertyClass:
        return WriteScalar(data_size, data_used, data, static_cast<CMIOClassID>(kCMIOPlugInClassID));
      case kCMIOObjectPropertyOwner:
        return WriteScalar(data_size, data_used, data, static_cast<CMIOObjectID>(kCMIOObjectSystemObject));
      case kCMIOObjectPropertyCreator:
      case kCMIOPlugInPropertyBundleID: {
        if (data_size < sizeof(CFStringRef)) {
          return kCMIOHardwareBadPropertySizeError;
        }
        *reinterpret_cast<CFStringRef*>(data) = CopyCFString(kPluginBundleID);
        if (data_used != nullptr) {
          *data_used = sizeof(CFStringRef);
        }
        return noErr;
      }
      case kCMIOObjectPropertyName: {
        if (data_size < sizeof(CFStringRef)) {
          return kCMIOHardwareBadPropertySizeError;
        }
        *reinterpret_cast<CFStringRef*>(data) = CopyCFString(kPluginName);
        if (data_used != nullptr) {
          *data_used = sizeof(CFStringRef);
        }
        return noErr;
      }
      case kCMIOObjectPropertyManufacturer: {
        if (data_size < sizeof(CFStringRef)) {
          return kCMIOHardwareBadPropertySizeError;
        }
        *reinterpret_cast<CFStringRef*>(data) = CopyCFString(kManufacturerName);
        if (data_used != nullptr) {
          *data_used = sizeof(CFStringRef);
        }
        return noErr;
      }
      case kCMIOObjectPropertyOwnedObjects: {
        if (gDeviceObjectID == kCMIODeviceUnknown) {
          if (data_used != nullptr) {
            *data_used = 0;
          }
          return noErr;
        }
        return WriteScalar(data_size, data_used, data, gDeviceObjectID);
      }
      case kCMIOPlugInPropertyIsExtension:
        return WriteScalar(data_size, data_used, data, static_cast<UInt32>(0));
      default:
        return kCMIOHardwareUnknownPropertyError;
    }
  }

  if (object_id == gDeviceObjectID) {
    switch (address->mSelector) {
      case kCMIOObjectPropertyClass:
        return WriteScalar(data_size, data_used, data, static_cast<CMIOClassID>(kCMIODeviceClassID));
      case kCMIOObjectPropertyOwner:
        return WriteScalar(data_size, data_used, data, gPlugInObjectID);
      case kCMIOObjectPropertyCreator: {
        if (data_size < sizeof(CFStringRef)) {
          return kCMIOHardwareBadPropertySizeError;
        }
        *reinterpret_cast<CFStringRef*>(data) = CopyCFString(kPluginBundleID);
        if (data_used != nullptr) {
          *data_used = sizeof(CFStringRef);
        }
        return noErr;
      }
      case kCMIOObjectPropertyName: {
        if (data_size < sizeof(CFStringRef)) {
          return kCMIOHardwareBadPropertySizeError;
        }
        *reinterpret_cast<CFStringRef*>(data) = CopyCFString("Kinect Camera");
        if (data_used != nullptr) {
          *data_used = sizeof(CFStringRef);
        }
        return noErr;
      }
      case kCMIOObjectPropertyManufacturer: {
        if (data_size < sizeof(CFStringRef)) {
          return kCMIOHardwareBadPropertySizeError;
        }
        *reinterpret_cast<CFStringRef*>(data) = CopyCFString(kManufacturerName);
        if (data_used != nullptr) {
          *data_used = sizeof(CFStringRef);
        }
        return noErr;
      }
      case kCMIOObjectPropertyOwnedObjects: {
        if (gStreamObjectID == kCMIOStreamUnknown) {
          if (data_used != nullptr) {
            *data_used = 0;
          }
          return noErr;
        }
        return WriteScalar(data_size, data_used, data, gStreamObjectID);
      }
      case kCMIODevicePropertyDeviceUID: {
        if (data_size < sizeof(CFStringRef)) {
          return kCMIOHardwareBadPropertySizeError;
        }
        *reinterpret_cast<CFStringRef*>(data) = CopyCFString(kDeviceUID);
        if (data_used != nullptr) {
          *data_used = sizeof(CFStringRef);
        }
        return noErr;
      }
      case kCMIODevicePropertyModelUID: {
        if (data_size < sizeof(CFStringRef)) {
          return kCMIOHardwareBadPropertySizeError;
        }
        *reinterpret_cast<CFStringRef*>(data) = CopyCFString(kModelUID);
        if (data_used != nullptr) {
          *data_used = sizeof(CFStringRef);
        }
        return noErr;
      }
      case kCMIODevicePropertyTransportType:
        return WriteScalar(data_size, data_used, data, static_cast<UInt32>('virt'));
      case kCMIODevicePropertyDeviceIsAlive:
        return WriteScalar(data_size, data_used, data, static_cast<UInt32>(1));
      case kCMIODevicePropertyDeviceIsRunning:
      case kCMIODevicePropertyDeviceIsRunningSomewhere:
        return WriteScalar(data_size, data_used, data, gRunningClients.load() > 0 ? static_cast<UInt32>(1) : static_cast<UInt32>(0));
      case kCMIODevicePropertySuspendedByUser:
      case kCMIODevicePropertyHogMode:
      case kCMIODevicePropertyLatency:
      case kCMIODevicePropertyCanProcessAVCCommand:
      case kCMIODevicePropertyCanProcessRS422Command:
      case kCMIODevicePropertyExcludeNonDALAccess:
        return WriteScalar(data_size, data_used, data, static_cast<UInt32>(0));
      case kCMIODevicePropertyStreams: {
        if (!IsInputScope(address) || gStreamObjectID == kCMIOStreamUnknown) {
          if (data_used != nullptr) {
            *data_used = 0;
          }
          return noErr;
        }
        return WriteScalar(data_size, data_used, data, gStreamObjectID);
      }
      case kCMIODevicePropertyStreamConfiguration: {
        if (!IsInputScope(address)) {
          if (data_size < sizeof(CMIODeviceStreamConfiguration)) {
            return kCMIOHardwareBadPropertySizeError;
          }
          auto* config = reinterpret_cast<CMIODeviceStreamConfiguration*>(data);
          config->mNumberStreams = 0;
          if (data_used != nullptr) {
            *data_used = sizeof(CMIODeviceStreamConfiguration);
          }
          return noErr;
        }
        if (data_size < sizeof(CMIODeviceStreamConfiguration) + sizeof(UInt32)) {
          return kCMIOHardwareBadPropertySizeError;
        }
        auto* config = reinterpret_cast<CMIODeviceStreamConfiguration*>(data);
        config->mNumberStreams = 1;
        config->mNumberChannels[0] = 1;
        if (data_used != nullptr) {
          *data_used = sizeof(CMIODeviceStreamConfiguration) + sizeof(UInt32);
        }
        return noErr;
      }
      default:
        return kCMIOHardwareUnknownPropertyError;
    }
  }

  if (object_id == gStreamObjectID) {
    switch (address->mSelector) {
      case kCMIOObjectPropertyClass:
        return WriteScalar(data_size, data_used, data, static_cast<CMIOClassID>(kCMIOStreamClassID));
      case kCMIOObjectPropertyOwner:
        return WriteScalar(data_size, data_used, data, gDeviceObjectID);
      case kCMIOObjectPropertyCreator: {
        if (data_size < sizeof(CFStringRef)) {
          return kCMIOHardwareBadPropertySizeError;
        }
        *reinterpret_cast<CFStringRef*>(data) = CopyCFString(kPluginBundleID);
        if (data_used != nullptr) {
          *data_used = sizeof(CFStringRef);
        }
        return noErr;
      }
      case kCMIOObjectPropertyName: {
        if (data_size < sizeof(CFStringRef)) {
          return kCMIOHardwareBadPropertySizeError;
        }
        *reinterpret_cast<CFStringRef*>(data) = CopyCFString("Kinect RGB Stream");
        if (data_used != nullptr) {
          *data_used = sizeof(CFStringRef);
        }
        return noErr;
      }
      case kCMIOObjectPropertyManufacturer: {
        if (data_size < sizeof(CFStringRef)) {
          return kCMIOHardwareBadPropertySizeError;
        }
        *reinterpret_cast<CFStringRef*>(data) = CopyCFString(kManufacturerName);
        if (data_used != nullptr) {
          *data_used = sizeof(CFStringRef);
        }
        return noErr;
      }
      case kCMIOObjectPropertyOwnedObjects:
        if (data_used != nullptr) {
          *data_used = 0;
        }
        return noErr;
      case kCMIOStreamPropertyDirection:
        return WriteScalar(data_size, data_used, data, static_cast<UInt32>(1));
      case kCMIOStreamPropertyTerminalType:
        return WriteScalar(data_size, data_used, data, static_cast<UInt32>(0));
      case kCMIOStreamPropertyStartingChannel:
        return WriteScalar(data_size, data_used, data, static_cast<UInt32>(1));
      case kCMIOStreamPropertyLatency:
      case kCMIOStreamPropertyNoDataEventCount:
        return WriteScalar(data_size, data_used, data, static_cast<UInt32>(0));
      case kCMIOStreamPropertyNoDataTimeoutInMSec:
        return WriteScalar(data_size, data_used, data, static_cast<UInt32>(2000));
      case kCMIOStreamPropertyCanProcessDeckCommand:
      case kCMIOStreamPropertyEndOfData:
        return WriteScalar(data_size, data_used, data, static_cast<UInt32>(0));
      case kCMIOStreamPropertyFormatDescription: {
        if (data_size < sizeof(CMFormatDescriptionRef)) {
          return kCMIOHardwareBadPropertySizeError;
        }
        std::lock_guard<std::mutex> lock(gStateMutex);
        if (!EnsureFormatDescriptionLocked()) {
          return kCMIOHardwareUnspecifiedError;
        }
        CFRetain(gFormatDescription);
        *reinterpret_cast<CMFormatDescriptionRef*>(data) = gFormatDescription;
        if (data_used != nullptr) {
          *data_used = sizeof(CMFormatDescriptionRef);
        }
        return noErr;
      }
      case kCMIOStreamPropertyFormatDescriptions: {
        if (data_size < sizeof(CFArrayRef)) {
          return kCMIOHardwareBadPropertySizeError;
        }
        std::lock_guard<std::mutex> lock(gStateMutex);
        if (!EnsureFormatDescriptionLocked()) {
          return kCMIOHardwareUnspecifiedError;
        }
        const void* values[] = {gFormatDescription};
        CFArrayRef array = CFArrayCreate(kCFAllocatorDefault, values, 1, &kCFTypeArrayCallBacks);
        *reinterpret_cast<CFArrayRef*>(data) = array;
        if (data_used != nullptr) {
          *data_used = sizeof(CFArrayRef);
        }
        return noErr;
      }
      case kCMIOStreamPropertyFrameRate:
        return WriteScalar(data_size, data_used, data, static_cast<Float64>(kOutputFPS));
      case kCMIOStreamPropertyFrameRates:
        return WriteScalar(data_size, data_used, data, static_cast<Float64>(kOutputFPS));
      case kCMIOStreamPropertyFrameRateRanges: {
        if (data_size < sizeof(AudioValueRange)) {
          return kCMIOHardwareBadPropertySizeError;
        }
        auto* range = reinterpret_cast<AudioValueRange*>(data);
        range->mMinimum = static_cast<Float64>(kOutputFPS);
        range->mMaximum = static_cast<Float64>(kOutputFPS);
        if (data_used != nullptr) {
          *data_used = sizeof(AudioValueRange);
        }
        return noErr;
      }
      default:
        return kCMIOHardwareUnknownPropertyError;
    }
  }

  return kCMIOHardwareBadObjectError;
}

OSStatus PlugInObjectSetPropertyData(
    CMIOHardwarePlugInRef,
    CMIOObjectID object_id,
    const CMIOObjectPropertyAddress* address,
    UInt32,
    const void*,
    UInt32,
    const void*) {
  if (address == nullptr) {
    return kCMIOHardwareIllegalOperationError;
  }
  if (object_id != gPlugInObjectID && object_id != gDeviceObjectID && object_id != gStreamObjectID) {
    return kCMIOHardwareBadObjectError;
  }
  if (address->mSelector == kCMIOObjectPropertyListenerAdded || address->mSelector == kCMIOObjectPropertyListenerRemoved) {
    return noErr;
  }
  return kCMIOHardwareUnsupportedOperationError;
}

OSStatus PlugInDeviceSuspend(CMIOHardwarePlugInRef, CMIODeviceID device_id) {
  if (device_id != gDeviceObjectID) {
    return kCMIOHardwareBadDeviceError;
  }
  while (gRunningClients.load() > 0) {
    StopProducingIfNeeded();
  }
  return noErr;
}

OSStatus PlugInDeviceResume(CMIOHardwarePlugInRef, CMIODeviceID device_id) {
  if (device_id != gDeviceObjectID) {
    return kCMIOHardwareBadDeviceError;
  }
  return noErr;
}

OSStatus PlugInDeviceStartStream(CMIOHardwarePlugInRef, CMIODeviceID device_id, CMIOStreamID stream_id) {
  if (device_id != gDeviceObjectID) {
    return kCMIOHardwareBadDeviceError;
  }
  if (stream_id != gStreamObjectID) {
    return kCMIOHardwareBadStreamError;
  }
  StartProducingIfNeeded();
  return noErr;
}

OSStatus PlugInDeviceStopStream(CMIOHardwarePlugInRef, CMIODeviceID device_id, CMIOStreamID stream_id) {
  if (device_id != gDeviceObjectID) {
    return kCMIOHardwareBadDeviceError;
  }
  if (stream_id != gStreamObjectID) {
    return kCMIOHardwareBadStreamError;
  }
  if (gRunningClients.load() > 0) {
    StopProducingIfNeeded();
  }
  return noErr;
}

OSStatus PlugInDeviceProcessAVCCommand(CMIOHardwarePlugInRef, CMIODeviceID, CMIODeviceAVCCommand*) {
  return kCMIOHardwareUnsupportedOperationError;
}

OSStatus PlugInDeviceProcessRS422Command(CMIOHardwarePlugInRef, CMIODeviceID, CMIODeviceRS422Command*) {
  return kCMIOHardwareUnsupportedOperationError;
}

OSStatus PlugInStreamCopyBufferQueue(
    CMIOHardwarePlugInRef,
    CMIOStreamID stream_id,
    CMIODeviceStreamQueueAlteredProc queue_altered_proc,
    void* queue_altered_ref_con,
    CMSimpleQueueRef* queue) {
  if (queue == nullptr) {
    return kCMIOHardwareIllegalOperationError;
  }
  if (stream_id != gStreamObjectID) {
    return kCMIOHardwareBadStreamError;
  }

  std::lock_guard<std::mutex> lock(gStateMutex);
  if (gSampleQueue == nullptr) {
    const OSStatus rc = CMSimpleQueueCreate(kCFAllocatorDefault, static_cast<int32_t>(kQueueCapacity), &gSampleQueue);
    if (rc != noErr || gSampleQueue == nullptr) {
      return kCMIOHardwareUnspecifiedError;
    }
  }

  gQueueAlteredProc = queue_altered_proc;
  gQueueAlteredRefCon = queue_altered_ref_con;

  *queue = gSampleQueue;
  CFRetain(*queue);
  return noErr;
}

OSStatus PlugInStreamDeckPlay(CMIOHardwarePlugInRef, CMIOStreamID) {
  return kCMIOHardwareUnsupportedOperationError;
}

OSStatus PlugInStreamDeckStop(CMIOHardwarePlugInRef, CMIOStreamID) {
  return kCMIOHardwareUnsupportedOperationError;
}

OSStatus PlugInStreamDeckJog(CMIOHardwarePlugInRef, CMIOStreamID, SInt32) {
  return kCMIOHardwareUnsupportedOperationError;
}

OSStatus PlugInStreamDeckCueTo(CMIOHardwarePlugInRef, CMIOStreamID, Float64, Boolean) {
  return kCMIOHardwareUnsupportedOperationError;
}

}  // namespace

extern "C" void* KinectCameraDALPluginMain(CFAllocatorRef, CFUUIDRef requested_type_uuid) {
  if (requested_type_uuid == nullptr) {
    return nullptr;
  }
  if (!CFEqual(requested_type_uuid, kCMIOHardwarePlugInTypeID)) {
    return nullptr;
  }
  PlugInAddRef(nullptr);
  return &gPlugInInterfacePtr;
}
