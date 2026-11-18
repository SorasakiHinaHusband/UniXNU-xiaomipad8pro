// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoCamera's camera extension (docs/drivers/camera.md section 4.5.2, step 2: the test pattern): a CoreMediaIO system extension
// with one device, "Piano Test Camera", one source stream of BGRA frames at 1280x720 and 30 fps: moving colour bars, a frame
// counter and a sweeping line, so a client shows that frames arrive and move. Written from Apple's public CoreMediaIO headers and
// documentation ("Creating a camera extension with Core Media I/O"); no Apple sample code is copied. The next version feeds the
// same device from PianoCamera's kernel stream (section 4.5.2, step 4).
//   Built by tooling/camera/extension/build-camera-app.sh into /Applications/PianoCamera.app/Contents/Library/SystemExtensions/
//   com.unixnu.PianoCamera.Extension.systemextension; activated by the app (PianoCamera activate). Its log: os_log subsystem
//   com.unixnu.PianoCamera (log stream --predicate 'subsystem == "com.unixnu.PianoCamera"').
#import <CoreMediaIO/CoreMediaIO.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <os/log.h>
#include <mach/mach_time.h>

static const int32_t kWidth = 1280, kHeight = 720, kFps = 30;
static os_log_t gLog;

// ------------------------------------------------------------------------------------------------------------------ the stream
@interface PianoStreamSource : NSObject <CMIOExtensionStreamSource>
@property(nonatomic, strong) CMIOExtensionStream *stream;
@property(nonatomic, strong) CMIOExtensionStreamFormat *format;
- (instancetype)initWithFormatDescription:(CMFormatDescriptionRef)fd;
@end

@implementation PianoStreamSource {
  CVPixelBufferPoolRef _pool;
  CMFormatDescriptionRef _fd;
  dispatch_source_t _timer;
  dispatch_queue_t _queue;
  uint64_t _frame;
  BOOL _running;
}

- (instancetype)initWithFormatDescription:(CMFormatDescriptionRef)fd
{
  if (!(self = [super init])) return nil;
  _fd = (CMFormatDescriptionRef)CFRetain(fd);
  CMTime frame = CMTimeMake(1, kFps);
  _format = [CMIOExtensionStreamFormat streamFormatWithFormatDescription:fd maxFrameDuration:frame minFrameDuration:frame
                                                     validFrameDurations:nil];
  NSDictionary *attrs = @{
    (id)kCVPixelBufferWidthKey : @(kWidth),
    (id)kCVPixelBufferHeightKey : @(kHeight),
    (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
    (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
  };
  CVPixelBufferPoolCreate(kCFAllocatorDefault, NULL, (__bridge CFDictionaryRef)attrs, &_pool);
  _queue = dispatch_queue_create("com.unixnu.PianoCamera.frames", DISPATCH_QUEUE_SERIAL);
  return self;
}

- (NSArray<CMIOExtensionStreamFormat *> *)formats { return @[ _format ]; }

- (NSSet<CMIOExtensionProperty> *)availableProperties
{
  return [NSSet setWithObjects:CMIOExtensionPropertyStreamActiveFormatIndex, CMIOExtensionPropertyStreamFrameDuration, nil];
}

- (CMIOExtensionStreamProperties *)streamPropertiesForProperties:(NSSet<CMIOExtensionProperty> *)properties error:(NSError **)outError
{
  CMIOExtensionStreamProperties *p = [CMIOExtensionStreamProperties streamPropertiesWithDictionary:@{}];
  if ([properties containsObject:CMIOExtensionPropertyStreamActiveFormatIndex]) p.activeFormatIndex = @0;
  if ([properties containsObject:CMIOExtensionPropertyStreamFrameDuration]) {
    CMTime frame = CMTimeMake(1, kFps);
    p.frameDuration = CFBridgingRelease(CMTimeCopyAsDictionary(frame, kCFAllocatorDefault));
  }
  return p;
}

- (BOOL)setStreamProperties:(CMIOExtensionStreamProperties *)streamProperties error:(NSError **)outError
{
  return YES;   // one format, one rate
}

- (BOOL)authorizedToStartStreamForClient:(CMIOExtensionClient *)client { return YES; }

// The frame: eight vertical colour bars scrolling left, a white line sweeping down, the frame number's bits as squares at the top.
- (void)fill:(CVPixelBufferRef)pb
{
  CVPixelBufferLockBaseAddress(pb, 0);
  uint8_t *base = CVPixelBufferGetBaseAddress(pb);
  const size_t stride = CVPixelBufferGetBytesPerRow(pb);
  static const uint8_t bars[8][3] = {{255, 255, 255}, {0, 255, 255}, {255, 255, 0}, {0, 255, 0},
                                     {255, 0, 255}, {0, 0, 255},   {255, 0, 0},   {16, 16, 16}};   // B, G, R
  const uint32_t shift = (uint32_t)(_frame * 8 % kWidth), sweep = (uint32_t)(_frame * 6 % kHeight);
  for (int32_t y = 0; y < kHeight; ++y) {
    uint8_t *row = base + (size_t)y * stride;
    for (int32_t x = 0; x < kWidth; ++x) {
      const uint8_t *c = bars[((x + shift) % kWidth) * 8 / kWidth];
      uint8_t *px = row + (size_t)x * 4;
      const int line = (uint32_t)y >= sweep && (uint32_t)y < sweep + 4;
      px[0] = line ? 255 : c[0];
      px[1] = line ? 255 : c[1];
      px[2] = line ? 255 : c[2];
      px[3] = 255;
    }
  }
  for (int bit = 0; bit < 24; ++bit) {   // the frame counter: 24 squares of 32 px, white for 1
    const uint8_t v = (_frame >> (23 - bit)) & 1 ? 255 : 0;
    for (int32_t y = 16; y < 48; ++y)
      for (int32_t x = 16 + bit * 48; x < 48 + bit * 48; ++x) {
        uint8_t *px = base + (size_t)y * stride + (size_t)x * 4;
        px[0] = px[1] = px[2] = v;
      }
  }
  CVPixelBufferUnlockBaseAddress(pb, 0);
}

- (void)tick
{
  if (!_running) return;
  CVPixelBufferRef pb = NULL;
  if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, _pool, &pb) != kCVReturnSuccess) {
    os_log_error(gLog, "no pixel buffer from the pool");
    return;
  }
  [self fill:pb];
  uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  CMSampleTimingInfo timing = {CMTimeMake(1, kFps), CMClockMakeHostTimeFromSystemUnits(mach_absolute_time()), kCMTimeInvalid};
  CMSampleBufferRef sb = NULL;
  OSStatus st = CMSampleBufferCreateForImageBuffer(kCFAllocatorDefault, pb, true, NULL, NULL, _fd, &timing, &sb);
  CVPixelBufferRelease(pb);
  if (st != noErr || !sb) {
    os_log_error(gLog, "CMSampleBufferCreateForImageBuffer: %d", (int)st);
    return;
  }
  [_stream sendSampleBuffer:sb discontinuity:CMIOExtensionStreamDiscontinuityFlagNone hostTimeInNanoseconds:now];
  CFRelease(sb);
  if (++_frame % 300 == 1) os_log(gLog, "test pattern: frame %llu sent", (unsigned long long)_frame);
}

- (BOOL)startStreamAndReturnError:(NSError **)outError
{
  os_log(gLog, "stream start");
  _running = YES;
  _timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _queue);
  dispatch_source_set_timer(_timer, DISPATCH_TIME_NOW, NSEC_PER_SEC / kFps, NSEC_PER_MSEC);
  __weak PianoStreamSource *weakSelf = self;
  dispatch_source_set_event_handler(_timer, ^{ [weakSelf tick]; });
  dispatch_resume(_timer);
  return YES;
}

- (BOOL)stopStreamAndReturnError:(NSError **)outError
{
  os_log(gLog, "stream stop after %llu frames", (unsigned long long)_frame);
  _running = NO;
  if (_timer) dispatch_source_cancel(_timer);
  _timer = nil;
  return YES;
}
@end

// ------------------------------------------------------------------------------------------------------------------ the device
@interface PianoDeviceSource : NSObject <CMIOExtensionDeviceSource>
@property(nonatomic, strong) CMIOExtensionDevice *device;
@property(nonatomic, strong) PianoStreamSource *streamSource;
@end

@implementation PianoDeviceSource
- (instancetype)init
{
  if (!(self = [super init])) return nil;
  // Fixed identities: a client keeps its choice of camera across restarts.
  NSUUID *deviceID = [[NSUUID alloc] initWithUUIDString:@"6A1F2A8E-4C2B-4E8F-9B57-50494E4F0001"];
  NSUUID *streamID = [[NSUUID alloc] initWithUUIDString:@"6A1F2A8E-4C2B-4E8F-9B57-50494E4F0101"];
  _device = [CMIOExtensionDevice deviceWithLocalizedName:@"Piano Test Camera" deviceID:deviceID
                                          legacyDeviceID:@"com.unixnu.PianoCamera.test" source:self];
  CMFormatDescriptionRef fd = NULL;
  CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCVPixelFormatType_32BGRA, kWidth, kHeight, NULL, &fd);
  _streamSource = [[PianoStreamSource alloc] initWithFormatDescription:fd];
  CFRelease(fd);
  CMIOExtensionStream *stream = [CMIOExtensionStream streamWithLocalizedName:@"Piano Test Camera"
                                                                     streamID:streamID
                                                                    direction:CMIOExtensionStreamDirectionSource
                                                                    clockType:CMIOExtensionStreamClockTypeHostTime
                                                                       source:_streamSource];
  _streamSource.stream = stream;
  NSError *error = nil;
  if (![_device addStream:stream error:&error]) os_log_error(gLog, "addStream: %{public}@", error);
  return self;
}

- (NSSet<CMIOExtensionProperty> *)availableProperties
{
  return [NSSet setWithObjects:CMIOExtensionPropertyDeviceTransportType, CMIOExtensionPropertyDeviceModel, nil];
}

- (CMIOExtensionDeviceProperties *)devicePropertiesForProperties:(NSSet<CMIOExtensionProperty> *)properties error:(NSError **)outError
{
  CMIOExtensionDeviceProperties *p = [CMIOExtensionDeviceProperties devicePropertiesWithDictionary:@{}];
  if ([properties containsObject:CMIOExtensionPropertyDeviceTransportType]) p.transportType = @((uint32_t)'bltn');   // built in (kIOAudioDeviceTransportTypeBuiltIn)
  if ([properties containsObject:CMIOExtensionPropertyDeviceModel]) p.model = @"Xiaomi Pad 8 Pro";
  return p;
}

- (BOOL)setDeviceProperties:(CMIOExtensionDeviceProperties *)deviceProperties error:(NSError **)outError { return YES; }
@end

// ---------------------------------------------------------------------------------------------------------------- the provider
@interface PianoProviderSource : NSObject <CMIOExtensionProviderSource>
@property(nonatomic, strong) CMIOExtensionProvider *provider;
@property(nonatomic, strong) PianoDeviceSource *deviceSource;
@end

@implementation PianoProviderSource
- (instancetype)initWithClientQueue:(dispatch_queue_t)queue
{
  if (!(self = [super init])) return nil;
  _provider = [CMIOExtensionProvider providerWithSource:self clientQueue:queue];
  _deviceSource = [[PianoDeviceSource alloc] init];
  NSError *error = nil;
  if (![_provider addDevice:_deviceSource.device error:&error]) os_log_error(gLog, "addDevice: %{public}@", error);
  return self;
}

- (BOOL)connectClient:(CMIOExtensionClient *)client error:(NSError **)outError
{
  os_log(gLog, "client %{public}@ connected", client.clientID);
  return YES;
}

- (void)disconnectClient:(CMIOExtensionClient *)client { os_log(gLog, "client %{public}@ gone", client.clientID); }

- (NSSet<CMIOExtensionProperty> *)availableProperties
{
  return [NSSet setWithObjects:CMIOExtensionPropertyProviderManufacturer, CMIOExtensionPropertyProviderName, nil];
}

- (CMIOExtensionProviderProperties *)providerPropertiesForProperties:(NSSet<CMIOExtensionProperty> *)properties error:(NSError **)outError
{
  CMIOExtensionProviderProperties *p = [CMIOExtensionProviderProperties providerPropertiesWithDictionary:@{}];
  if ([properties containsObject:CMIOExtensionPropertyProviderManufacturer]) p.manufacturer = @"UniXNU-piano";
  if ([properties containsObject:CMIOExtensionPropertyProviderName]) p.name = @"PianoCamera";
  return p;
}

- (BOOL)setProviderProperties:(CMIOExtensionProviderProperties *)providerProperties error:(NSError **)outError { return YES; }
@end

int main(void)
{
  @autoreleasepool {
    gLog = os_log_create("com.unixnu.PianoCamera", "extension");
    os_log(gLog, "PianoCamera extension starting (test pattern %dx%d at %d fps)", kWidth, kHeight, kFps);
    PianoProviderSource *source = [[PianoProviderSource alloc] initWithClientQueue:nil];
    [CMIOExtensionProvider startServiceWithProvider:source.provider];
    CFRunLoopRun();
  }
  return 0;
}
