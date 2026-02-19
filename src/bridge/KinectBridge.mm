#import "KinectBridge.h"

#include "../backends/backend.h"

#include <memory>
#include <string>
#include <vector>

@implementation KinectFrame

- (instancetype)initWithRgb:(NSData *)rgb
                      depth:(NSData *)depth
                         ir:(NSData *)ir
                      width:(NSInteger)width
                     height:(NSInteger)height
                  timestamp:(NSTimeInterval)timestamp {
  self = [super init];
  if (self) {
    _rgbData = rgb;
    _depthData = depth;
    _irData = ir;
    _width = width;
    _height = height;
    _timestamp = timestamp;
  }
  return self;
}

@end

@interface KinectBridge () {
  std::unique_ptr<KinectBackend> _backend;
  std::unique_ptr<KinectDevice> _device;
  FrameData _frame;
  NSInteger _selectedGeneration;
  NSInteger _streamType;
  BOOL _streaming;
  NSString *_lastError;
}
@end

@implementation KinectBridge

+ (instancetype)sharedInstance {
  static KinectBridge *shared = nil;
  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^{
    shared = [[self alloc] init];
  });
  return shared;
}

- (instancetype)init {
  self = [super init];
  if (self) {
    _selectedGeneration = 1;
    _streamType = 0;
    _streaming = NO;
    _lastError = @"";
  }
  return self;
}

- (BOOL)initializeBackend:(NSInteger)backendType {
  _device.reset();
  _backend.reset();
  _streaming = NO;

  _selectedGeneration = backendType;
  if (backendType == 2) {
    _backend = CreateKinectV2Backend();
  } else {
    _backend = CreateKinectV1Backend();
    _selectedGeneration = 1;
  }
  if (!_backend) {
    _lastError = @"No backend instance available.";
    return NO;
  }
  const ProbeResult probe = _backend->probe();
  _lastError = probe.detail.empty() ? @"" : [NSString stringWithUTF8String:probe.detail.c_str()];
  // Backend initialization succeeded. Even if probe currently reports
  // unavailable, let openDevice() attempt explicit serial/index opening.
  return YES;
}

- (NSArray<NSDictionary *> *)discoverDevices {
  NSMutableArray<NSDictionary *> *result = [NSMutableArray array];

  {
    auto backend = CreateKinectV1Backend();
    if (backend) {
      (void)backend->probe();
      for (const auto &dev : backend->listDevices()) {
        NSString *serial = [NSString stringWithUTF8String:dev.serial.c_str()];
        NSString *name = dev.name.empty() ? @"Kinect v1" : [NSString stringWithUTF8String:dev.name.c_str()];
        [result addObject:@{
          @"generation": @1,
          @"serial": serial ?: @"",
          @"name": name ?: @"Kinect v1"
        }];
      }
    }
  }

  {
    auto backend = CreateKinectV2Backend();
    if (backend) {
      (void)backend->probe();
      for (const auto &dev : backend->listDevices()) {
        NSString *serial = [NSString stringWithUTF8String:dev.serial.c_str()];
        NSString *name = dev.name.empty() ? @"Kinect v2" : [NSString stringWithUTF8String:dev.name.c_str()];
        [result addObject:@{
          @"generation": @2,
          @"serial": serial ?: @"",
          @"name": name ?: @"Kinect v2"
        }];
      }
    }
  }

  return result;
}

- (NSArray<NSString *> *)listDevices {
  if (!_backend) {
    return @[];
  }
  auto devices = _backend->listDevices();
  NSMutableArray<NSString *> *result = [NSMutableArray arrayWithCapacity:devices.size()];
  for (const auto &dev : devices) {
    NSString *serial = [NSString stringWithUTF8String:dev.serial.c_str()];
    [result addObject:(serial ?: @"")];
  }
  return result;
}

- (BOOL)openDeviceWithGeneration:(NSInteger)generation serial:(NSString *)serial {
  if (![self initializeBackend:generation]) {
    return NO;
  }
  return [self openDevice:serial];
}

- (BOOL)openDevice:(nullable NSString *)serial {
  if (!_backend) {
    _lastError = @"Backend is not initialized.";
    return NO;
  }

  std::string serialValue;
  if (serial != nil) {
    serialValue = [serial UTF8String];
  } else {
    const auto devices = _backend->listDevices();
    if (devices.empty()) {
      return NO;
    }
    serialValue = devices[0].serial;
  }

  _device = _backend->openDevice(serialValue);
  if (_device == nullptr) {
    NSString *failureHint = _selectedGeneration == 1
                                ? @"Failed to open device. For Kinect v1, ensure audios.bin firmware is available."
                                : @"Failed to open device.";
    if (_lastError != nil && _lastError.length > 0) {
      _lastError = [_lastError stringByAppendingFormat:@" %@", failureHint];
    } else {
      _lastError = failureHint;
    }
  } else {
    _lastError = @"";
  }
  return _device != nullptr;
}

- (void)startStream {
  if (!_device) {
    return;
  }
  _device->setStreamKind(static_cast<StreamKind>(_streamType));
  _streaming = _device->start();
}

- (void)stopStream {
  if (!_device) {
    _streaming = NO;
    return;
  }
  _device->stop();
  _streaming = NO;
}

- (nullable KinectFrame *)pollFrame {
  if (!_device || !_streaming) {
    return nil;
  }

  _device->update();
  if (!_device->getFrame(_frame)) {
    return nil;
  }

  NSData *rgb = _frame.rgb.empty() ? [NSData data] : [NSData dataWithBytes:_frame.rgb.data() length:_frame.rgb.size()];
  NSData *depth = _frame.depth.empty() ? [NSData data]
                                       : [NSData dataWithBytes:_frame.depth.data()
                                                      length:_frame.depth.size() * sizeof(uint16_t)];
  NSData *ir = _frame.ir.empty() ? [NSData data] : [NSData dataWithBytes:_frame.ir.data() length:_frame.ir.size()];

  return [[KinectFrame alloc] initWithRgb:rgb
                                    depth:depth
                                       ir:ir
                                    width:_frame.width
                                   height:_frame.height
                                timestamp:(NSTimeInterval)_frame.timestamp / 1000.0];
}

- (BOOL)isStreaming {
  return _streaming;
}

- (void)setStreamType:(NSInteger)streamType {
  _streamType = MAX(0, MIN(2, streamType));
  if (_device) {
    _device->setStreamKind(static_cast<StreamKind>(_streamType));
  }
}

- (NSInteger)streamType {
  return _streamType;
}

- (void)setTilt:(NSInteger)angle {
  if (_device) {
    _device->setTilt((int)angle);
  }
}

- (void)setLed:(NSInteger)mode {
  if (_device) {
    _device->setLed((int)mode);
  }
}

- (void)setMirror:(BOOL)enabled {
  if (_device) {
    _device->setMirror(enabled);
  }
}

- (void)setAutoExposure:(BOOL)enabled {
  if (_device) {
    _device->setAutoExposure(enabled);
  }
}

- (void)setAutoWhiteBalance:(BOOL)enabled {
  if (_device) {
    _device->setAutoWhiteBalance(enabled);
  }
}

- (void)setNearMode:(BOOL)enabled {
  if (_device) {
    _device->setNearMode(enabled);
  }
}

- (void)setManualExposureUs:(NSInteger)value {
  if (_device) {
    _device->setManualExposureUs((int)value);
  }
}

- (void)setIrBrightness:(NSInteger)value {
  if (_device) {
    _device->setIrBrightness((int)value);
  }
}

- (BOOL)setAudioEnabled:(BOOL)enabled {
  if (!_device) {
    return NO;
  }
  return _device->setAudioEnabled(enabled);
}

- (BOOL)audioEnabled {
  if (!_device) {
    return NO;
  }
  return _device->audioEnabled();
}

- (float)audioLevel {
  if (!_device) {
    return 0.0f;
  }
  return _device->audioLevel();
}

- (NSDictionary *)deviceCapabilities {
  if (!_device) {
    return @{
      @"supportsMotor": @NO,
      @"supportsLed": @NO,
      @"supportsAudioInput": @NO,
      @"supportsDepth": @NO,
      @"supportsIr": @NO
    };
  }

  return @{
    @"supportsMotor": @(_device->supportsMotor()),
    @"supportsLed": @(_device->supportsLed()),
    @"supportsAudioInput": @(_device->supportsAudioInput()),
    @"supportsDepth": @(_device->supportsDepth()),
    @"supportsIr": @(_device->supportsIr())
  };
}

- (NSString *)lastError {
  return _lastError ?: @"";
}

@end
