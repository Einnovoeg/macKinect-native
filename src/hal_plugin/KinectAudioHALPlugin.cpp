#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/HostTime.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace {

constexpr AudioObjectID kObjectIDDevice = 2;
constexpr AudioObjectID kObjectIDStreamInput = 3;

constexpr const char *kPlugInName = "macKinect Audio HAL";
constexpr const char *kManufacturerName = "macKinect";
constexpr const char *kDeviceUID = "com.mackinect.audiohal.device";
constexpr const char *kModelUID = "com.mackinect.audiohal.model";

std::atomic<ULONG> gRefCount{1};
AudioServerPlugInHostRef gHost = nullptr;
std::mutex gStateMutex;
Float64 gSampleRate = 48000.0;
UInt32 gBufferFrameSize = 480;
UInt32 gRunningIOClients = 0;
UInt64 gZeroTimeStampSeed = 1;
UInt64 gZeroHostTime = 0;
Float64 gZeroSampleTime = 0.0;

AudioStreamBasicDescription MakeFormat(Float64 sample_rate) {
  AudioStreamBasicDescription asbd{};
  asbd.mSampleRate = sample_rate;
  asbd.mFormatID = kAudioFormatLinearPCM;
  asbd.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
  asbd.mBytesPerPacket = sizeof(Float32);
  asbd.mFramesPerPacket = 1;
  asbd.mBytesPerFrame = sizeof(Float32);
  asbd.mChannelsPerFrame = 1;
  asbd.mBitsPerChannel = 32;
  return asbd;
}

OSStatus UnknownProperty() {
  return kAudioHardwareUnknownPropertyError;
}

CFStringRef CopyCFString(const char *text) {
  return CFStringCreateWithCString(nullptr, text, kCFStringEncodingUTF8);
}

bool IsInputScope(const AudioObjectPropertyAddress *address) {
  if (address == nullptr) {
    return false;
  }
  return address->mScope == kAudioObjectPropertyScopeInput || address->mScope == kAudioObjectPropertyScopeGlobal;
}

HRESULT STDMETHODCALLTYPE DriverQueryInterface(void *in_driver, REFIID, LPVOID *out_interface);
ULONG STDMETHODCALLTYPE DriverAddRef(void *in_driver);
ULONG STDMETHODCALLTYPE DriverRelease(void *in_driver);
OSStatus STDMETHODCALLTYPE DriverInitialize(AudioServerPlugInDriverRef in_driver, AudioServerPlugInHostRef in_host);
OSStatus STDMETHODCALLTYPE DriverCreateDevice(AudioServerPlugInDriverRef in_driver, CFDictionaryRef in_description,
                                              const AudioServerPlugInClientInfo *in_client_info,
                                              AudioObjectID *out_device_object_id);
OSStatus STDMETHODCALLTYPE DriverDestroyDevice(AudioServerPlugInDriverRef in_driver, AudioObjectID in_device_object_id);
OSStatus STDMETHODCALLTYPE DriverAddDeviceClient(AudioServerPlugInDriverRef in_driver, AudioObjectID in_device_object_id,
                                                 const AudioServerPlugInClientInfo *in_client_info);
OSStatus STDMETHODCALLTYPE DriverRemoveDeviceClient(AudioServerPlugInDriverRef in_driver, AudioObjectID in_device_object_id,
                                                    const AudioServerPlugInClientInfo *in_client_info);
OSStatus STDMETHODCALLTYPE DriverPerformDeviceConfigurationChange(AudioServerPlugInDriverRef in_driver,
                                                                  AudioObjectID in_device_object_id,
                                                                  UInt64 in_change_action, void *in_change_info);
OSStatus STDMETHODCALLTYPE DriverAbortDeviceConfigurationChange(AudioServerPlugInDriverRef in_driver,
                                                                AudioObjectID in_device_object_id, UInt64 in_change_action,
                                                                void *in_change_info);
Boolean STDMETHODCALLTYPE DriverHasProperty(AudioServerPlugInDriverRef in_driver, AudioObjectID in_object_id,
                                            pid_t in_client_process_id,
                                            const AudioObjectPropertyAddress *in_address);
OSStatus STDMETHODCALLTYPE DriverIsPropertySettable(AudioServerPlugInDriverRef in_driver, AudioObjectID in_object_id,
                                                    pid_t in_client_process_id,
                                                    const AudioObjectPropertyAddress *in_address, Boolean *out_is_settable);
OSStatus STDMETHODCALLTYPE DriverGetPropertyDataSize(AudioServerPlugInDriverRef in_driver, AudioObjectID in_object_id,
                                                     pid_t in_client_process_id,
                                                     const AudioObjectPropertyAddress *in_address,
                                                     UInt32 in_qualifier_data_size, const void *in_qualifier_data,
                                                     UInt32 *out_data_size);
OSStatus STDMETHODCALLTYPE DriverGetPropertyData(AudioServerPlugInDriverRef in_driver, AudioObjectID in_object_id,
                                                 pid_t in_client_process_id,
                                                 const AudioObjectPropertyAddress *in_address,
                                                 UInt32 in_qualifier_data_size, const void *in_qualifier_data,
                                                 UInt32 in_data_size, UInt32 *out_data_size, void *out_data);
OSStatus STDMETHODCALLTYPE DriverSetPropertyData(AudioServerPlugInDriverRef in_driver, AudioObjectID in_object_id,
                                                 pid_t in_client_process_id,
                                                 const AudioObjectPropertyAddress *in_address,
                                                 UInt32 in_qualifier_data_size, const void *in_qualifier_data,
                                                 UInt32 in_data_size, const void *in_data);
OSStatus STDMETHODCALLTYPE DriverStartIO(AudioServerPlugInDriverRef in_driver, AudioObjectID in_device_object_id,
                                         UInt32 in_client_id);
OSStatus STDMETHODCALLTYPE DriverStopIO(AudioServerPlugInDriverRef in_driver, AudioObjectID in_device_object_id,
                                        UInt32 in_client_id);
OSStatus STDMETHODCALLTYPE DriverGetZeroTimeStamp(AudioServerPlugInDriverRef in_driver, AudioObjectID in_device_object_id,
                                                  UInt32 in_client_id, Float64 *out_sample_time, UInt64 *out_host_time,
                                                  UInt64 *out_seed);
OSStatus STDMETHODCALLTYPE DriverWillDoIOOperation(AudioServerPlugInDriverRef in_driver, AudioObjectID in_device_object_id,
                                                   UInt32 in_client_id, UInt32 in_operation_id, Boolean *out_will_do,
                                                   Boolean *out_will_do_in_place);
OSStatus STDMETHODCALLTYPE DriverBeginIOOperation(AudioServerPlugInDriverRef in_driver, AudioObjectID in_device_object_id,
                                                  UInt32 in_client_id, UInt32 in_operation_id,
                                                  UInt32 in_io_buffer_frame_size,
                                                  const AudioServerPlugInIOCycleInfo *in_io_cycle_info);
OSStatus STDMETHODCALLTYPE DriverDoIOOperation(AudioServerPlugInDriverRef in_driver, AudioObjectID in_device_object_id,
                                               AudioObjectID in_stream_object_id, UInt32 in_client_id,
                                               UInt32 in_operation_id, UInt32 in_io_buffer_frame_size,
                                               const AudioServerPlugInIOCycleInfo *in_io_cycle_info, void *io_main_buffer,
                                               void *io_secondary_buffer);
OSStatus STDMETHODCALLTYPE DriverEndIOOperation(AudioServerPlugInDriverRef in_driver, AudioObjectID in_device_object_id,
                                                UInt32 in_client_id, UInt32 in_operation_id,
                                                UInt32 in_io_buffer_frame_size,
                                                const AudioServerPlugInIOCycleInfo *in_io_cycle_info);

AudioServerPlugInDriverInterface gDriverInterface = {
    nullptr,
    DriverQueryInterface,
    DriverAddRef,
    DriverRelease,
    DriverInitialize,
    DriverCreateDevice,
    DriverDestroyDevice,
    DriverAddDeviceClient,
    DriverRemoveDeviceClient,
    DriverPerformDeviceConfigurationChange,
    DriverAbortDeviceConfigurationChange,
    DriverHasProperty,
    DriverIsPropertySettable,
    DriverGetPropertyDataSize,
    DriverGetPropertyData,
    DriverSetPropertyData,
    DriverStartIO,
    DriverStopIO,
    DriverGetZeroTimeStamp,
    DriverWillDoIOOperation,
    DriverBeginIOOperation,
    DriverDoIOOperation,
    DriverEndIOOperation};

AudioServerPlugInDriverInterface *gDriverInterfacePtr = &gDriverInterface;
AudioServerPlugInDriverRef gDriverRef = &gDriverInterfacePtr;

HRESULT STDMETHODCALLTYPE DriverQueryInterface(void *, REFIID, LPVOID *out_interface) {
  if (out_interface == nullptr) {
    return E_POINTER;
  }
  *out_interface = gDriverRef;
  DriverAddRef(nullptr);
  return S_OK;
}

ULONG STDMETHODCALLTYPE DriverAddRef(void *) {
  return ++gRefCount;
}

ULONG STDMETHODCALLTYPE DriverRelease(void *) {
  const ULONG value = --gRefCount;
  return value;
}

OSStatus STDMETHODCALLTYPE DriverInitialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef in_host) {
  std::lock_guard<std::mutex> lock(gStateMutex);
  gHost = in_host;
  gZeroHostTime = AudioGetCurrentHostTime();
  gZeroSampleTime = 0.0;
  gZeroTimeStampSeed = 1;
  return noErr;
}

OSStatus STDMETHODCALLTYPE DriverCreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef,
                                              const AudioServerPlugInClientInfo *, AudioObjectID *) {
  return kAudioHardwareUnsupportedOperationError;
}

OSStatus STDMETHODCALLTYPE DriverDestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) {
  return kAudioHardwareUnsupportedOperationError;
}

OSStatus STDMETHODCALLTYPE DriverAddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *) {
  return noErr;
}

OSStatus STDMETHODCALLTYPE DriverRemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID,
                                                    const AudioServerPlugInClientInfo *) {
  return noErr;
}

OSStatus STDMETHODCALLTYPE DriverPerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *) {
  return noErr;
}

OSStatus STDMETHODCALLTYPE DriverAbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *) {
  return noErr;
}

Boolean STDMETHODCALLTYPE DriverHasProperty(AudioServerPlugInDriverRef, AudioObjectID in_object_id, pid_t,
                                            const AudioObjectPropertyAddress *in_address) {
  if (in_address == nullptr) {
    return false;
  }

  switch (in_object_id) {
    case kAudioObjectPlugInObject:
      switch (in_address->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
          return true;
        default:
          return false;
      }
    case kObjectIDDevice:
      switch (in_address->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyStreams:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyBufferFrameSize:
        case kAudioDevicePropertyBufferFrameSizeRange:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyLatency:
          return true;
        default:
          return false;
      }
    case kObjectIDStreamInput:
      switch (in_address->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailablePhysicalFormats:
          return true;
        default:
          return false;
      }
    default:
      return false;
  }
}

OSStatus STDMETHODCALLTYPE DriverIsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID in_object_id, pid_t,
                                                    const AudioObjectPropertyAddress *in_address, Boolean *out_is_settable) {
  if (in_address == nullptr || out_is_settable == nullptr) {
    return kAudioHardwareIllegalOperationError;
  }

  *out_is_settable = false;
  if (in_object_id == kObjectIDDevice &&
      (in_address->mSelector == kAudioDevicePropertyNominalSampleRate ||
       in_address->mSelector == kAudioDevicePropertyBufferFrameSize)) {
    *out_is_settable = true;
  } else if (in_object_id == kObjectIDStreamInput &&
             (in_address->mSelector == kAudioStreamPropertyVirtualFormat ||
              in_address->mSelector == kAudioStreamPropertyPhysicalFormat)) {
    *out_is_settable = true;
  }
  return noErr;
}

OSStatus STDMETHODCALLTYPE DriverGetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID in_object_id, pid_t,
                                                     const AudioObjectPropertyAddress *in_address, UInt32,
                                                     const void *, UInt32 *out_data_size) {
  if (in_address == nullptr || out_data_size == nullptr) {
    return kAudioHardwareIllegalOperationError;
  }

  switch (in_object_id) {
    case kAudioObjectPlugInObject:
      switch (in_address->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
          *out_data_size = sizeof(AudioClassID);
          return noErr;
        case kAudioObjectPropertyManufacturer:
        case kAudioPlugInPropertyResourceBundle:
          *out_data_size = sizeof(CFStringRef);
          return noErr;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
          *out_data_size = sizeof(AudioObjectID);
          return noErr;
        case kAudioPlugInPropertyTranslateUIDToDevice:
          *out_data_size = sizeof(AudioObjectID);
          return noErr;
        default:
          return UnknownProperty();
      }
    case kObjectIDDevice:
      switch (in_address->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyBufferFrameSize:
          *out_data_size = sizeof(UInt32);
          return noErr;
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
          *out_data_size = sizeof(CFStringRef);
          return noErr;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
          *out_data_size = sizeof(AudioObjectID);
          return noErr;
        case kAudioDevicePropertyNominalSampleRate:
          *out_data_size = sizeof(Float64);
          return noErr;
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyBufferFrameSizeRange:
          *out_data_size = sizeof(AudioValueRange);
          return noErr;
        default:
          return UnknownProperty();
      }
    case kObjectIDStreamInput:
      switch (in_address->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
          *out_data_size = sizeof(UInt32);
          return noErr;
        case kAudioObjectPropertyName:
          *out_data_size = sizeof(CFStringRef);
          return noErr;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
          *out_data_size = sizeof(AudioStreamBasicDescription);
          return noErr;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
          *out_data_size = sizeof(AudioStreamRangedDescription);
          return noErr;
        default:
          return UnknownProperty();
      }
    default:
      return UnknownProperty();
  }
}

OSStatus STDMETHODCALLTYPE DriverGetPropertyData(AudioServerPlugInDriverRef, AudioObjectID in_object_id, pid_t,
                                                 const AudioObjectPropertyAddress *in_address, UInt32 in_qualifier_data_size,
                                                 const void *in_qualifier_data, UInt32 in_data_size, UInt32 *out_data_size,
                                                 void *out_data) {
  if (in_address == nullptr || out_data_size == nullptr || out_data == nullptr) {
    return kAudioHardwareIllegalOperationError;
  }

  switch (in_object_id) {
    case kAudioObjectPlugInObject:
      switch (in_address->mSelector) {
        case kAudioObjectPropertyBaseClass:
          if (in_data_size < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<AudioClassID *>(out_data) = kAudioObjectClassID;
          *out_data_size = sizeof(AudioClassID);
          return noErr;
        case kAudioObjectPropertyClass:
          if (in_data_size < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<AudioClassID *>(out_data) = kAudioPlugInClassID;
          *out_data_size = sizeof(AudioClassID);
          return noErr;
        case kAudioObjectPropertyManufacturer:
          if (in_data_size < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<CFStringRef *>(out_data) = CopyCFString(kManufacturerName);
          *out_data_size = sizeof(CFStringRef);
          return noErr;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
          if (in_data_size < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<AudioObjectID *>(out_data) = kObjectIDDevice;
          *out_data_size = sizeof(AudioObjectID);
          return noErr;
        case kAudioPlugInPropertyTranslateUIDToDevice: {
          if (in_data_size < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
          AudioObjectID translated = kAudioObjectUnknown;
          if (in_qualifier_data_size == sizeof(CFStringRef) && in_qualifier_data != nullptr) {
            CFStringRef uid = *reinterpret_cast<CFStringRef const *>(in_qualifier_data);
            CFStringRef expected = CopyCFString(kDeviceUID);
            if (uid != nullptr && expected != nullptr && CFEqual(uid, expected)) {
              translated = kObjectIDDevice;
            }
            if (expected != nullptr) CFRelease(expected);
          }
          *reinterpret_cast<AudioObjectID *>(out_data) = translated;
          *out_data_size = sizeof(AudioObjectID);
          return noErr;
        }
        case kAudioPlugInPropertyResourceBundle:
          if (in_data_size < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<CFStringRef *>(out_data) = nullptr;
          *out_data_size = sizeof(CFStringRef);
          return noErr;
        default:
          return UnknownProperty();
      }

    case kObjectIDDevice:
      switch (in_address->mSelector) {
        case kAudioObjectPropertyBaseClass:
          if (in_data_size < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<AudioClassID *>(out_data) = kAudioObjectClassID;
          *out_data_size = sizeof(AudioClassID);
          return noErr;
        case kAudioObjectPropertyClass:
          if (in_data_size < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<AudioClassID *>(out_data) = kAudioDeviceClassID;
          *out_data_size = sizeof(AudioClassID);
          return noErr;
        case kAudioObjectPropertyOwner:
          if (in_data_size < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<AudioObjectID *>(out_data) = kAudioObjectPlugInObject;
          *out_data_size = sizeof(AudioObjectID);
          return noErr;
        case kAudioObjectPropertyName:
          if (in_data_size < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<CFStringRef *>(out_data) = CopyCFString(kPlugInName);
          *out_data_size = sizeof(CFStringRef);
          return noErr;
        case kAudioObjectPropertyManufacturer:
          if (in_data_size < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<CFStringRef *>(out_data) = CopyCFString(kManufacturerName);
          *out_data_size = sizeof(CFStringRef);
          return noErr;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
          if (!IsInputScope(in_address)) {
            *out_data_size = 0;
            return noErr;
          }
          if (in_data_size < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<AudioObjectID *>(out_data) = kObjectIDStreamInput;
          *out_data_size = sizeof(AudioObjectID);
          return noErr;
        case kAudioDevicePropertyDeviceUID:
          if (in_data_size < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<CFStringRef *>(out_data) = CopyCFString(kDeviceUID);
          *out_data_size = sizeof(CFStringRef);
          return noErr;
        case kAudioDevicePropertyModelUID:
          if (in_data_size < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<CFStringRef *>(out_data) = CopyCFString(kModelUID);
          *out_data_size = sizeof(CFStringRef);
          return noErr;
        case kAudioDevicePropertyTransportType:
          if (in_data_size < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<UInt32 *>(out_data) = kAudioDeviceTransportTypeVirtual;
          *out_data_size = sizeof(UInt32);
          return noErr;
        case kAudioDevicePropertyNominalSampleRate: {
          if (in_data_size < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
          std::lock_guard<std::mutex> lock(gStateMutex);
          *reinterpret_cast<Float64 *>(out_data) = gSampleRate;
          *out_data_size = sizeof(Float64);
          return noErr;
        }
        case kAudioDevicePropertyAvailableNominalSampleRates: {
          if (in_data_size < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError;
          auto *range = reinterpret_cast<AudioValueRange *>(out_data);
          range->mMinimum = 16000.0;
          range->mMaximum = 48000.0;
          *out_data_size = sizeof(AudioValueRange);
          return noErr;
        }
        case kAudioDevicePropertyBufferFrameSize: {
          if (in_data_size < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
          std::lock_guard<std::mutex> lock(gStateMutex);
          *reinterpret_cast<UInt32 *>(out_data) = gBufferFrameSize;
          *out_data_size = sizeof(UInt32);
          return noErr;
        }
        case kAudioDevicePropertyBufferFrameSizeRange: {
          if (in_data_size < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError;
          auto *range = reinterpret_cast<AudioValueRange *>(out_data);
          range->mMinimum = 64;
          range->mMaximum = 4096;
          *out_data_size = sizeof(AudioValueRange);
          return noErr;
        }
        case kAudioDevicePropertyDeviceIsAlive:
          if (in_data_size < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<UInt32 *>(out_data) = 1;
          *out_data_size = sizeof(UInt32);
          return noErr;
        case kAudioDevicePropertyDeviceIsRunning: {
          if (in_data_size < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
          std::lock_guard<std::mutex> lock(gStateMutex);
          *reinterpret_cast<UInt32 *>(out_data) = gRunningIOClients > 0 ? 1U : 0U;
          *out_data_size = sizeof(UInt32);
          return noErr;
        }
        case kAudioDevicePropertyLatency:
          if (in_data_size < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<UInt32 *>(out_data) = 0;
          *out_data_size = sizeof(UInt32);
          return noErr;
        default:
          return UnknownProperty();
      }

    case kObjectIDStreamInput:
      switch (in_address->mSelector) {
        case kAudioObjectPropertyBaseClass:
          if (in_data_size < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<AudioClassID *>(out_data) = kAudioObjectClassID;
          *out_data_size = sizeof(AudioClassID);
          return noErr;
        case kAudioObjectPropertyClass:
          if (in_data_size < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<AudioClassID *>(out_data) = kAudioStreamClassID;
          *out_data_size = sizeof(AudioClassID);
          return noErr;
        case kAudioObjectPropertyOwner:
          if (in_data_size < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<AudioObjectID *>(out_data) = kObjectIDDevice;
          *out_data_size = sizeof(AudioObjectID);
          return noErr;
        case kAudioObjectPropertyName:
          if (in_data_size < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<CFStringRef *>(out_data) = CopyCFString("Kinect Mic Stream");
          *out_data_size = sizeof(CFStringRef);
          return noErr;
        case kAudioStreamPropertyDirection:
          if (in_data_size < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<UInt32 *>(out_data) = 1;
          *out_data_size = sizeof(UInt32);
          return noErr;
        case kAudioStreamPropertyTerminalType:
          if (in_data_size < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<UInt32 *>(out_data) = kAudioStreamTerminalTypeMicrophone;
          *out_data_size = sizeof(UInt32);
          return noErr;
        case kAudioStreamPropertyStartingChannel:
          if (in_data_size < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
          *reinterpret_cast<UInt32 *>(out_data) = 1;
          *out_data_size = sizeof(UInt32);
          return noErr;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: {
          if (in_data_size < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
          std::lock_guard<std::mutex> lock(gStateMutex);
          *reinterpret_cast<AudioStreamBasicDescription *>(out_data) = MakeFormat(gSampleRate);
          *out_data_size = sizeof(AudioStreamBasicDescription);
          return noErr;
        }
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
          if (in_data_size < sizeof(AudioStreamRangedDescription)) return kAudioHardwareBadPropertySizeError;
          auto *range = reinterpret_cast<AudioStreamRangedDescription *>(out_data);
          range->mFormat = MakeFormat(48000.0);
          range->mSampleRateRange.mMinimum = 16000.0;
          range->mSampleRateRange.mMaximum = 48000.0;
          *out_data_size = sizeof(AudioStreamRangedDescription);
          return noErr;
        }
        default:
          return UnknownProperty();
      }

    default:
      return UnknownProperty();
  }
}

OSStatus STDMETHODCALLTYPE DriverSetPropertyData(AudioServerPlugInDriverRef, AudioObjectID in_object_id, pid_t,
                                                 const AudioObjectPropertyAddress *in_address, UInt32, const void *,
                                                 UInt32 in_data_size, const void *in_data) {
  if (in_address == nullptr || in_data == nullptr) {
    return kAudioHardwareIllegalOperationError;
  }

  if (in_object_id == kObjectIDDevice && in_address->mSelector == kAudioDevicePropertyNominalSampleRate) {
    if (in_data_size < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
    std::lock_guard<std::mutex> lock(gStateMutex);
    gSampleRate = *reinterpret_cast<const Float64 *>(in_data);
    ++gZeroTimeStampSeed;
    return noErr;
  }
  if (in_object_id == kObjectIDDevice && in_address->mSelector == kAudioDevicePropertyBufferFrameSize) {
    if (in_data_size < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
    std::lock_guard<std::mutex> lock(gStateMutex);
    gBufferFrameSize = *reinterpret_cast<const UInt32 *>(in_data);
    return noErr;
  }
  if (in_object_id == kObjectIDStreamInput &&
      (in_address->mSelector == kAudioStreamPropertyVirtualFormat ||
       in_address->mSelector == kAudioStreamPropertyPhysicalFormat)) {
    if (in_data_size < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
    const auto *asbd = reinterpret_cast<const AudioStreamBasicDescription *>(in_data);
    std::lock_guard<std::mutex> lock(gStateMutex);
    gSampleRate = asbd->mSampleRate;
    ++gZeroTimeStampSeed;
    return noErr;
  }

  return kAudioHardwareUnsupportedOperationError;
}

OSStatus STDMETHODCALLTYPE DriverStartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32) {
  std::lock_guard<std::mutex> lock(gStateMutex);
  ++gRunningIOClients;
  return noErr;
}

OSStatus STDMETHODCALLTYPE DriverStopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32) {
  std::lock_guard<std::mutex> lock(gStateMutex);
  if (gRunningIOClients > 0) {
    --gRunningIOClients;
  }
  return noErr;
}

OSStatus STDMETHODCALLTYPE DriverGetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *out_sample_time,
                                                  UInt64 *out_host_time, UInt64 *out_seed) {
  if (out_sample_time == nullptr || out_host_time == nullptr || out_seed == nullptr) {
    return kAudioHardwareIllegalOperationError;
  }

  std::lock_guard<std::mutex> lock(gStateMutex);
  const UInt64 now = AudioGetCurrentHostTime();
  if (gZeroHostTime == 0) {
    gZeroHostTime = now;
  }

  const Float64 elapsed = AudioConvertHostTimeToNanos(now - gZeroHostTime) / 1.0e9;
  gZeroSampleTime = elapsed * gSampleRate;

  *out_sample_time = gZeroSampleTime;
  *out_host_time = now;
  *out_seed = gZeroTimeStampSeed;
  return noErr;
}

OSStatus STDMETHODCALLTYPE DriverWillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32 in_operation_id,
                                                   Boolean *out_will_do, Boolean *out_will_do_in_place) {
  if (out_will_do == nullptr || out_will_do_in_place == nullptr) {
    return kAudioHardwareIllegalOperationError;
  }
  const bool read_input = in_operation_id == kAudioServerPlugInIOOperationReadInput;
  *out_will_do = read_input ? 1 : 0;
  *out_will_do_in_place = 1;
  return noErr;
}

OSStatus STDMETHODCALLTYPE DriverBeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
                                                  const AudioServerPlugInIOCycleInfo *) {
  return noErr;
}

OSStatus STDMETHODCALLTYPE DriverDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32,
                                               UInt32 in_operation_id, UInt32 in_io_buffer_frame_size,
                                               const AudioServerPlugInIOCycleInfo *, void *io_main_buffer, void *) {
  if (in_operation_id == kAudioServerPlugInIOOperationReadInput && io_main_buffer != nullptr) {
    std::memset(io_main_buffer, 0, static_cast<size_t>(in_io_buffer_frame_size) * sizeof(Float32));
  }
  return noErr;
}

OSStatus STDMETHODCALLTYPE DriverEndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
                                                const AudioServerPlugInIOCycleInfo *) {
  return noErr;
}

}  // namespace

extern "C" void *KinectAudioHALPlugInFactory(CFAllocatorRef, CFUUIDRef requested_type_uuid) {
  if (requested_type_uuid == nullptr) {
    return nullptr;
  }
  CFUUIDRef expected = kAudioServerPlugInTypeUUID;
  if (!CFEqual(requested_type_uuid, expected)) {
    return nullptr;
  }
  DriverAddRef(nullptr);
  return gDriverRef;
}
