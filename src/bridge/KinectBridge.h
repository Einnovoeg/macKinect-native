#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface KinectFrame : NSObject
@property (nonatomic, readonly) NSData *rgbData;
@property (nonatomic, readonly) NSData *depthData;
@property (nonatomic, readonly) NSData *irData;
@property (nonatomic, readonly) NSInteger width;
@property (nonatomic, readonly) NSInteger height;
@property (nonatomic, readonly) NSTimeInterval timestamp;

- (instancetype)initWithRgb:(NSData *)rgb
                      depth:(NSData *)depth
                         ir:(NSData *)ir
                      width:(NSInteger)width
                     height:(NSInteger)height
                  timestamp:(NSTimeInterval)timestamp;
@end

@interface KinectBridge : NSObject

+ (instancetype)sharedInstance;

// Legacy API: 1 = V1, 2 = V2
- (BOOL)initializeBackend:(NSInteger)backendType;

// Modern API: discover and select devices across both generations.
- (NSArray<NSDictionary *> *)discoverDevices;
- (BOOL)openDeviceWithGeneration:(NSInteger)generation serial:(NSString *)serial;

- (NSArray<NSString *> *)listDevices;

// Open the first available device if serial is nil
- (BOOL)openDevice:(nullable NSString *)serial;

- (void)startStream;
- (void)stopStream;

// Poll for the latest frame. Returns nil if no new frame.
- (nullable KinectFrame *)pollFrame;

- (BOOL)isStreaming;

// Stream selection: 0=RGB, 1=IR, 2=Depth
- (void)setStreamType:(NSInteger)streamType;
- (NSInteger)streamType;

// Controls
- (void)setTilt:(NSInteger)angle;
- (void)setLed:(NSInteger)mode;
- (void)setMirror:(BOOL)enabled;
- (void)setAutoExposure:(BOOL)enabled;
- (void)setAutoWhiteBalance:(BOOL)enabled;
- (void)setNearMode:(BOOL)enabled;
- (void)setManualExposureUs:(NSInteger)value;
- (void)setIrBrightness:(NSInteger)value;

// Audio
- (BOOL)setAudioEnabled:(BOOL)enabled;
- (BOOL)audioEnabled;
- (float)audioLevel;

// Status/capabilities
- (NSDictionary *)deviceCapabilities;
- (NSString *)lastError;

@end

NS_ASSUME_NONNULL_END
