// SPDX-License-Identifier: BSD-2-Clause-Patent
// CMIO publisher for the two Piano sensors and a separate moving-pattern control.
#import <CoreMediaIO/CoreMediaIO.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <os/log.h>
#import <IOKit/IOKitLib.h>
#import <CoreGraphics/CoreGraphics.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include "../../../package/PianoCamera/PianoCameraABI.h"
#include "../../../package/PianoCamera/CameraIsp.h"
#import "Devices.h"

static const int32_t kFps = 15;
static os_log_t gLog;

// ------------------------------------------------------------------------------------------------------------------ the stream
@interface PianoStreamSource : NSObject <CMIOExtensionStreamSource>
@property(nonatomic, strong) CMIOExtensionStream *stream;
@property(nonatomic, strong) CMIOExtensionStreamFormat *format;
- (instancetype)initWithWidth:(int32_t)width height:(int32_t)height sensor:(uint32_t)sensor;
@end

@implementation PianoStreamSource {
  CVPixelBufferPoolRef _pool;
  CMFormatDescriptionRef _fd;
  dispatch_source_t _timer;
  dispatch_queue_t _queue;
  uint64_t _frame;
  int32_t _width, _height;
  uint32_t _sensor;
  atomic_bool _running;
  io_connect_t _connection;
  mach_vm_address_t _raw;
  mach_vm_size_t _rawSize;
  float *_work;
  uint8_t *_rgb;
  uint64_t _dropped, _lastSequence, _lastDropCount, _lastCaptureNs, _ispTotal, _ispMax;
}

- (instancetype)initWithWidth:(int32_t)width height:(int32_t)height sensor:(uint32_t)sensor
{
  if (!(self = [super init])) return nil;
  _width = width;
  _height = height;
  _sensor = sensor;
  atomic_init(&_running, false);
  NSDictionary *color = @{
      (__bridge NSString *)kCMFormatDescriptionExtension_ColorPrimaries:
          (__bridge NSString *)kCMFormatDescriptionColorPrimaries_ITU_R_709_2,
      (__bridge NSString *)kCMFormatDescriptionExtension_TransferFunction:
          (__bridge NSString *)kCMFormatDescriptionTransferFunction_sRGB,
  };
  if (CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCVPixelFormatType_32BGRA, width, height,
                                      (__bridge CFDictionaryRef)color, &_fd)) return nil;
  CMTime frame = CMTimeMake(1, kFps);
  _format = [CMIOExtensionStreamFormat streamFormatWithFormatDescription:_fd maxFrameDuration:frame minFrameDuration:frame
                                                     validFrameDurations:nil];
  NSDictionary *attrs = @{
    (id)kCVPixelBufferWidthKey : @(_width),
    (id)kCVPixelBufferHeightKey : @(_height),
    (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
    (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
  };
  if (CVPixelBufferPoolCreate(kCFAllocatorDefault, NULL, (__bridge CFDictionaryRef)attrs, &_pool)) return nil;
  _queue = dispatch_queue_create("com.unixnu.PianoCamera.frames", DISPATCH_QUEUE_SERIAL);
  return self;
}

- (void)dealloc
{
  if (_fd) CFRelease(_fd);
  if (_pool) CVPixelBufferPoolRelease(_pool);
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
  NSNumber *index = streamProperties.activeFormatIndex;
  NSDictionary *duration = streamProperties.frameDuration;
  if ((!index || [index isEqualToNumber:@0]) &&
      (!duration || CMTimeCompare(CMTimeMakeFromDictionary((__bridge CFDictionaryRef)duration), CMTimeMake(1, kFps)) == 0))
    return YES;
  if (outError) *outError = [NSError errorWithDomain:NSPOSIXErrorDomain code:EINVAL userInfo:nil];
  return NO;
}

- (BOOL)authorizedToStartStreamForClient:(CMIOExtensionClient *)client { return YES; }

// The frame: eight vertical colour bars scrolling left, a white line sweeping down, the frame number's bits as squares at the top.
- (void)fill:(CVPixelBufferRef)pb
{
  uint8_t *base = CVPixelBufferGetBaseAddress(pb);
  const size_t stride = CVPixelBufferGetBytesPerRow(pb);
  static const uint8_t bars[8][3] = {{255, 255, 255}, {0, 255, 255}, {255, 255, 0}, {0, 255, 0},
                                     {255, 0, 255}, {0, 0, 255},   {255, 0, 0},   {16, 16, 16}};   // B, G, R
  const uint32_t shift = (uint32_t)(_frame * 8 % _width), sweep = (uint32_t)(_frame * 6 % _height);
  for (int32_t y = 0; y < _height; ++y) {
    uint8_t *row = base + (size_t)y * stride;
    for (int32_t x = 0; x < _width; ++x) {
      const uint8_t *c = bars[((x + shift) % _width) * 8 / _width];
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
}

static IOReturn liveCall(io_connect_t connection, uint32_t op, const void *in, size_t inSize, void *out, size_t size)
{
  IOReturn status = IOConnectCallStructMethod(connection, op, in, inSize, out, &size);
  return status;
}

static BOOL ioResult(IOReturn status, NSError **outError)
{
  if (status && outError) *outError = [NSError errorWithDomain:NSMachErrorDomain code:status userInfo:nil];
  return status == kIOReturnSuccess;
}

- (void)publish:(CVPixelBufferRef)pb ticks:(uint64_t)ticks sequence:(uint64_t)sequence drops:(uint64_t)drops
{
  CMTime pts = CMClockMakeHostTimeFromSystemUnits(ticks);
  CMTime ns = CMTimeConvertScale(pts, NSEC_PER_SEC, kCMTimeRoundingMethod_RoundHalfAwayFromZero);
  if (!CMTIME_IS_NUMERIC(ns) || ns.value <= 0) { ++_dropped; return; }
  CMSampleTimingInfo timing = {CMTimeMake(1, kFps), pts, kCMTimeInvalid};
  CMSampleBufferRef sample = NULL;
  CVBufferSetAttachment(pb, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2,
                        kCVAttachmentMode_ShouldPropagate);
  CVBufferSetAttachment(pb, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_sRGB,
                        kCVAttachmentMode_ShouldPropagate);
  CMVideoFormatDescriptionRef description = NULL;
  OSStatus error = CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pb, &description);
  if (!error) {
    error = CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, pb, description, &timing, &sample);
    CFRelease(description);
  }
  if (error) { ++_dropped; return; }
  const BOOL gap = _lastCaptureNs && (drops != _lastDropCount || (uint64_t)ns.value - _lastCaptureNs > 100000000);
  if (gap) os_log(gLog, "camera cadence gap sensor=%u seq=%llu delta_ns=%llu kernel_drops=%llu",
                  _sensor, sequence, (uint64_t)ns.value - _lastCaptureNs, drops);
  _lastCaptureNs = ns.value;
  _lastDropCount = drops;
  _lastSequence = sequence;
  [_stream sendSampleBuffer:sample
              discontinuity:gap ? CMIOExtensionStreamDiscontinuityFlagSampleDropped : CMIOExtensionStreamDiscontinuityFlagNone
       hostTimeInNanoseconds:ns.value];
  CFRelease(sample);
  if (++_frame == 1)
    os_log(gLog, "camera first sample sensor=%u seq=%llu pts_ns=%lld isp_ns=%llu", _sensor, sequence, ns.value, _ispMax);
}

- (CVPixelBufferRef)borrowPixel
{
  CVPixelBufferRef pb = NULL;
  NSDictionary *limits = @{(__bridge NSString *)kCVPixelBufferPoolAllocationThresholdKey: @8};
  CVReturn status = CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(kCFAllocatorDefault, _pool,
                                           (__bridge CFDictionaryRef)limits, &pb);
  if (status) ++_dropped;
  return pb;
}

- (void)tick
{
  if (!atomic_load(&_running)) return;
  CVPixelBufferRef pb = [self borrowPixel];
  if (!pb) return;
  if (!CVPixelBufferLockBaseAddress(pb, 0)) {
    [self fill:pb];
    CVPixelBufferUnlockBaseAddress(pb, 0);
    [self publish:pb ticks:mach_absolute_time() sequence:_frame + 1 drops:_dropped];
  } else ++_dropped;
  CVPixelBufferRelease(pb);
}

- (BOOL)convert:(const PianoCameraLiveFrame *)frame pixel:(CVPixelBufferRef)pb
{
  const BOOL rear = _sensor == kPianoCameraRear;
  const uint32_t rawWidth = rear ? 4080 : 3264, rawHeight = rear ? 3072 : 1836, stride = rear ? 5104 : 4080;
  if (frame->version != kPianoCameraLiveVersion || frame->sensor != _sensor || frame->slot >= kPianoCameraSlots ||
      frame->width != rawWidth || frame->height != rawHeight || frame->stride != stride ||
      frame->bayer != (rear ? kPianoCameraGRBG : kPianoCameraBGGR) ||
      frame->black >= 1023 || frame->bytes != stride * rawHeight || frame->bytes > kPianoCameraSlotBytes ||
      !frame->sofTicks || frame->sequence <= _lastSequence ||
      (uint64_t)frame->slot * kPianoCameraSlotBytes + frame->bytes > _rawSize) return NO;
  uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  PianoIspStats stats;
  const uint8_t *raw = (const uint8_t *)(uintptr_t)_raw + (uint64_t)frame->slot * kPianoCameraSlotBytes;
  if (pianoIsp(raw, rawWidth, rawHeight, stride, frame->bayer, frame->black, 2, _work, _rgb, &stats)) return NO;
  if (CVPixelBufferLockBaseAddress(pb, 0)) return NO;
  uint8_t *base = CVPixelBufferGetBaseAddress(pb);
  const size_t rowBytes = CVPixelBufferGetBytesPerRow(pb);
  for (int32_t y = 0; y < _height; ++y) {
    uint8_t *row = base + y * rowBytes;
    for (int32_t x = 0; x < _width; ++x) {
      // The rear sensor is mounted upside down. Leave selfie mirroring to clients.
      size_t source = rear ? (size_t)(_height - 1 - y) * _width + _width - 1 - x : (size_t)y * _width + x;
      row[x * 4 + 0] = _rgb[source * 3 + 2];
      row[x * 4 + 1] = _rgb[source * 3 + 1];
      row[x * 4 + 2] = _rgb[source * 3 + 0];
      row[x * 4 + 3] = 255;
    }
  }
  CVPixelBufferUnlockBaseAddress(pb, 0);
  uint64_t elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
  _ispTotal += elapsed;
  _ispMax = MAX(_ispMax, elapsed);
  return YES;
}

- (void)readLive
{
  uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 8 * NSEC_PER_SEC;
  while (atomic_load(&_running)) {
    @autoreleasepool {
      PianoCameraLiveFrame frame;
      IOReturn result = liveCall(_connection, kPianoCameraLiveWait, NULL, 0, &frame, sizeof frame);
      if (result == kIOReturnTimeout && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < deadline) continue;
      if (result) {
        if (atomic_load(&_running)) {
          os_log_error(gLog, "camera RAW wait failed sensor=%u result=%#x", _sensor, result);
          PianoCameraLiveStatus state;
          liveCall(_connection, kPianoCameraLiveStop, NULL, 0, &state, sizeof state);
        }
        break;
      }
      deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + NSEC_PER_SEC;
      CVPixelBufferRef pb = [self borrowPixel];
      BOOL valid = pb && [self convert:&frame pixel:pb];
      PianoCameraLiveReleaseArgs release = {kPianoCameraLiveVersion, frame.slot, frame.sequence};
      result = liveCall(_connection, kPianoCameraLiveRelease, &release, sizeof release, NULL, 0);
      if (valid && !result && atomic_load(&_running))
        [self publish:pb ticks:frame.sofTicks sequence:frame.sequence drops:frame.drops + _dropped];
      else if (pb) ++_dropped;
      if (pb) CVPixelBufferRelease(pb);
      if (result) {
        os_log_error(gLog, "camera RAW lease release failed sensor=%u result=%#x", _sensor, result);
        PianoCameraLiveStatus state;
        liveCall(_connection, kPianoCameraLiveStop, NULL, 0, &state, sizeof state);
        break;
      }
    }
  }
}

- (BOOL)startStreamAndReturnError:(NSError **)outError
{
  if (atomic_load(&_running)) return YES;
  _frame = _dropped = _lastSequence = _lastDropCount = _lastCaptureNs = _ispTotal = _ispMax = 0;
  if (_sensor) {
    _work = malloc((size_t)_width * _height * 3 * sizeof(float));
    _rgb = malloc((size_t)_width * _height * 3);
    IOReturn result = _work && _rgb ? kIOReturnNotFound : kIOReturnNoMemory;
    if (_work && _rgb) {
      io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PianoCamera"));
      if (service) {
        result = IOServiceOpen(service, mach_task_self(), 0, &_connection);
        IOObjectRelease(service);
      }
      os_log(gLog, "camera role open sensor=%u result=%#x", _sensor, result);
    }
    if (!result) {
      PianoCameraLiveStartArgs req = {kPianoCameraLiveVersion, _sensor};
      result = liveCall(_connection, kPianoCameraLiveStart, &req, sizeof req, NULL, 0);
    }
    if (!result) {
      result = IOConnectMapMemory64(_connection, kPianoCameraMemoryRing, mach_task_self(), &_raw, &_rawSize,
                                    kIOMapAnywhere | kIOMapReadOnly);
      if (!result && _rawSize != kPianoCameraSlots * (uint64_t)kPianoCameraSlotBytes) result = kIOReturnBadArgument;
    }
    if (result) {
      os_log_error(gLog, "camera stream start failed sensor=%u result=%#x", _sensor, result);
      [self closeLive];
      return ioResult(result, outError);
    }
    atomic_store(&_running, true);
    dispatch_async(_queue, ^{ [self readLive]; });
  } else {
    _timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _queue);
    dispatch_source_set_timer(_timer, DISPATCH_TIME_NOW, NSEC_PER_SEC / kFps, NSEC_PER_MSEC);
    __weak PianoStreamSource *weakSelf = self;
    dispatch_source_set_event_handler(_timer, ^{ [weakSelf tick]; });
    atomic_store(&_running, true);
    dispatch_resume(_timer);
  }
  os_log(gLog, "camera stream start sensor=%u", _sensor);
  return YES;
}

- (void)closeLive
{
  if (_raw) IOConnectUnmapMemory64(_connection, kPianoCameraMemoryRing, mach_task_self(), _raw);
  if (_connection) IOServiceClose(_connection);
  _raw = _rawSize = _connection = 0;
  free(_rgb);
  free(_work);
  _rgb = NULL;
  _work = NULL;
}

- (BOOL)stopStreamAndReturnError:(NSError **)outError
{
  if (!atomic_exchange(&_running, false)) return YES;
  if (_timer) dispatch_source_cancel(_timer);
  _timer = nil;
  PianoCameraLiveStatus state = {0};
  IOReturn result = _connection ? liveCall(_connection, kPianoCameraLiveStop, NULL, 0, &state, sizeof state) : kIOReturnSuccess;
  dispatch_sync(_queue, ^{});
  [self closeLive];
  os_log(gLog, "camera stream stop sensor=%u result=%#x state=%u clean=%u guard=%u smmu=%#x sent=%llu dropped=%llu kernel_drops=%llu isp_avg_ns=%llu isp_max_ns=%llu",
         _sensor, result, state.state, state.cleanOff, state.guardFault, state.smmuFault, _frame, _dropped, state.dropped,
         _frame ? _ispTotal / _frame : 0, _ispMax);
  if (!result && _sensor && state.state != kPianoCameraStopped) result = kIOReturnIOError;
  return ioResult(result, outError);
}

@end

// ------------------------------------------------------------------------------------------------------------------ the device
@interface PianoDeviceSource : NSObject <CMIOExtensionDeviceSource>
@property(nonatomic, strong) CMIOExtensionDevice *device;
@property(nonatomic, strong) PianoStreamSource *streamSource;
- (instancetype)initWithName:(NSString *)name identifier:(NSString *)identifier sensor:(uint32_t)sensor
                        width:(int32_t)width height:(int32_t)height;
@end

@implementation PianoDeviceSource
- (instancetype)initWithName:(NSString *)name identifier:(NSString *)identifier sensor:(uint32_t)sensor
                        width:(int32_t)width height:(int32_t)height
{
  if (!(self = [super init])) return nil;
  NSUUID *deviceID = [[NSUUID alloc] initWithUUIDString:identifier];
  NSString *streamIdentifier = [identifier stringByReplacingOccurrencesOfString:@"50494E4F00" withString:@"50494E4F01"];
  NSUUID *streamID = [[NSUUID alloc] initWithUUIDString:streamIdentifier];
  _device = [CMIOExtensionDevice deviceWithLocalizedName:name deviceID:deviceID legacyDeviceID:identifier source:self];
  _streamSource = [[PianoStreamSource alloc] initWithWidth:width height:height sensor:sensor];
  if (!_streamSource) return nil;
  _streamSource.stream = [CMIOExtensionStream streamWithLocalizedName:name streamID:streamID
                                  direction:CMIOExtensionStreamDirectionSource clockType:CMIOExtensionStreamClockTypeHostTime
                                     source:_streamSource];
  NSError *error = nil;
  if (![_device addStream:_streamSource.stream error:&error]) {
    os_log_error(gLog, "camera addStream failed sensor=%u code=%ld", sensor, (long)error.code);
    return nil;
  }
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
@property(nonatomic, strong) NSArray<PianoDeviceSource *> *deviceSources;
@end

@implementation PianoProviderSource
- (instancetype)initWithClientQueue:(dispatch_queue_t)queue
{
  if (!(self = [super init])) return nil;
  _provider = [CMIOExtensionProvider providerWithSource:self clientQueue:queue];
  PianoDeviceSource *pattern = [[PianoDeviceSource alloc] initWithName:PIANO_PATTERN_NAME
                                     identifier:PIANO_PATTERN_ID sensor:0 width:1280 height:720];
  PianoDeviceSource *front = [[PianoDeviceSource alloc] initWithName:PIANO_FRONT_NAME
                                     identifier:PIANO_FRONT_ID sensor:kPianoCameraFront width:816 height:459];
  PianoDeviceSource *rear = [[PianoDeviceSource alloc] initWithName:PIANO_REAR_NAME
                                     identifier:PIANO_REAR_ID sensor:kPianoCameraRear width:1020 height:768];
  if (!pattern || !front || !rear) return nil;
  _deviceSources = @[pattern, front, rear];
  for (PianoDeviceSource *source in _deviceSources) {
    NSError *error = nil;
    if (![_provider addDevice:source.device error:&error]) {
      os_log_error(gLog, "camera addDevice failed code=%ld", (long)error.code);
      return nil;
    }
    os_log(gLog, "camera added %{public}@", source.device.localizedName);
  }
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
    os_log(gLog, "PianoCamera extension starting at %d fps", kFps);
    dispatch_queue_t control = dispatch_queue_create("com.unixnu.PianoCamera.client", DISPATCH_QUEUE_SERIAL);
    __attribute__((objc_precise_lifetime)) PianoProviderSource *source = [[PianoProviderSource alloc] initWithClientQueue:control];
    if (!source) return 1;
    [CMIOExtensionProvider startServiceWithProvider:source.provider];
    CFRunLoopRun();
  }
  return 0;
}
