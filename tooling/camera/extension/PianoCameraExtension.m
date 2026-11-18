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
#include "CameraOutput.h"

static os_log_t gLog;

// ------------------------------------------------------------------------------------------------------------------ the stream
@interface PianoStreamSource : NSObject <CMIOExtensionStreamSource>
@property(nonatomic, strong) CMIOExtensionStream *stream;
- (instancetype)initWithWidth:(int32_t)width height:(int32_t)height sensor:(uint32_t)sensor;
@end

@implementation PianoStreamSource {
  CVPixelBufferPoolRef _outputPools[2], _sourcePool;
  NSArray<CMIOExtensionStreamFormat *> *_formats;
  dispatch_source_t _timer;
  dispatch_queue_t _outputQueue, _captureQueue;
  int32_t _nativeWidth, _nativeHeight;
  uint32_t _sensor;
  atomic_bool _running;
  atomic_uint_fast64_t _captureDropped;
  // Output queue owns all published state and the immutable latest source reference.
  NSUInteger _activeFormat;
  CMTime _duration, _maxDuration;
  CVPixelBufferRef _latest;
  uint64_t _latestSequence, _latestDrops, _latestSourceNs;
  uint64_t _frame, _dropped, _repeated, _sourceFrames, _staleTicks;
  uint64_t _lastPublishedSequence, _lastPresentationTicks, _lastPresentationNs, _lastDropCount;
  BOOL _configurationChanged, _stalled;
  // Capture queue owns the RAW lease, ISP buffers and these capture counters.
  io_connect_t _connection;
  mach_vm_address_t _raw;
  mach_vm_size_t _rawSize;
  float *_work;
  uint8_t *_rgb;
  uint64_t _lastReadSequence, _ispTotal, _ispMax, _converted;
}

static CVReturn makePool(uint32_t width, uint32_t height, CVPixelBufferPoolRef *pool)
{
  NSDictionary *attrs = @{
    (id)kCVPixelBufferWidthKey : @(width), (id)kCVPixelBufferHeightKey : @(height),
    (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
    (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
  };
  return CVPixelBufferPoolCreate(kCFAllocatorDefault, NULL, (__bridge CFDictionaryRef)attrs, pool);
}

static NSDictionary *timeDictionary(CMTime time)
{
  return CFBridgingRelease(CMTimeCopyAsDictionary(time, kCFAllocatorDefault));
}

static BOOL validDuration(NSUInteger index, CMTime time)
{
  return CMTIME_IS_NUMERIC(time) && time.epoch == 0 && pianoOutputDurationValid((unsigned)index, time.value, time.timescale);
}

- (instancetype)initWithWidth:(int32_t)width height:(int32_t)height sensor:(uint32_t)sensor
{
  if (!(self = [super init])) return nil;
  _nativeWidth = width;
  _nativeHeight = height;
  _sensor = sensor;
  _duration = _maxDuration = CMTimeMake(1, kPianoNativeFps);
  atomic_init(&_running, false);
  atomic_init(&_captureDropped, 0);
  _outputQueue = dispatch_queue_create("com.unixnu.PianoCamera.output", DISPATCH_QUEUE_SERIAL);
  _captureQueue = dispatch_queue_create("com.unixnu.PianoCamera.capture", DISPATCH_QUEUE_SERIAL);
  NSDictionary *color = @{
      (__bridge NSString *)kCMFormatDescriptionExtension_ColorPrimaries:
          (__bridge NSString *)kCMFormatDescriptionColorPrimaries_ITU_R_709_2,
      (__bridge NSString *)kCMFormatDescriptionExtension_TransferFunction:
          (__bridge NSString *)kCMFormatDescriptionTransferFunction_sRGB,
  };
  NSMutableArray *formats = [NSMutableArray array];
  for (unsigned index = 0; index < 2; ++index) {
    PianoOutputFormat f = pianoOutputFormat(width, height, index);
    CMFormatDescriptionRef description = NULL;
    OSStatus result = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCVPixelFormatType_32BGRA, f.width, f.height,
                                                    (__bridge CFDictionaryRef)color, &description);
    if (!result) {
      [formats addObject:[CMIOExtensionStreamFormat streamFormatWithFormatDescription:description
                           maxFrameDuration:CMTimeMake(1, f.minFps) minFrameDuration:CMTimeMake(1, f.maxFps)
                           validFrameDurations:nil]];
      CFRelease(description);
      result = makePool(f.width, f.height, &_outputPools[index]);
    }
    if (result) {
      os_log_error(gLog, "camera init failed sensor=%u format=%u result=%d", _sensor, index, (int)result);
      return nil;
    }
  }
  _formats = [formats copy];
  if (sensor && makePool(width, height, &_sourcePool)) return nil;
  return self;
}

- (void)dealloc
{
  if (_latest) CVPixelBufferRelease(_latest);
  if (_sourcePool) CVPixelBufferPoolRelease(_sourcePool);
  for (unsigned i = 0; i < 2; ++i) if (_outputPools[i]) CVPixelBufferPoolRelease(_outputPools[i]);
}

- (NSArray<CMIOExtensionStreamFormat *> *)formats { return _formats; }

- (NSSet<CMIOExtensionProperty> *)availableProperties
{
  return [NSSet setWithObjects:CMIOExtensionPropertyStreamActiveFormatIndex, CMIOExtensionPropertyStreamFrameDuration,
                              CMIOExtensionPropertyStreamMaxFrameDuration, nil];
}

- (CMIOExtensionStreamProperties *)streamPropertiesForProperties:(NSSet<CMIOExtensionProperty> *)properties error:(NSError **)outError
{
  CMIOExtensionStreamProperties *p = [CMIOExtensionStreamProperties streamPropertiesWithDictionary:@{}];
  dispatch_sync(_outputQueue, ^{
    if ([properties containsObject:CMIOExtensionPropertyStreamActiveFormatIndex]) p.activeFormatIndex = @(self->_activeFormat);
    if ([properties containsObject:CMIOExtensionPropertyStreamFrameDuration]) p.frameDuration = timeDictionary(self->_duration);
    if ([properties containsObject:CMIOExtensionPropertyStreamMaxFrameDuration]) p.maxFrameDuration = timeDictionary(self->_maxDuration);
    os_log(gLog, "camera format query sensor=%u index=%lu duration=%lld/%d max=%lld/%d", self->_sensor,
           (unsigned long)self->_activeFormat, self->_duration.value, self->_duration.timescale,
           self->_maxDuration.value, self->_maxDuration.timescale);
  });
  return p;
}

- (void)retimeOutput
{
  if (_timer) {
    uint64_t interval = (uint64_t)llround(CMTimeGetSeconds(_duration) * NSEC_PER_SEC);
    dispatch_source_set_timer(_timer, DISPATCH_TIME_NOW, interval, NSEC_PER_MSEC);
  }
}

- (BOOL)setStreamProperties:(CMIOExtensionStreamProperties *)properties error:(NSError **)outError
{
  NSNumber *index = properties.activeFormatIndex;
  NSDictionary *requested = properties.frameDuration, *requestedMax = properties.maxFrameDuration;
  __block BOOL accepted = NO;
  __block NSDictionary *changed;
  dispatch_sync(_outputQueue, ^{
    NSUInteger next = index ? index.unsignedIntegerValue : self->_activeFormat;
    CMTime duration = requested ? CMTimeMakeFromDictionary((__bridge CFDictionaryRef)requested) : self->_duration;
    CMTime maximum = requestedMax ? CMTimeMakeFromDictionary((__bridge CFDictionaryRef)requestedMax) : self->_maxDuration;
    if (next > 1 || (index && index.doubleValue != (double)next)) return;
    // A format-only transition chooses a valid duration before notifying the client.
    if (!requested && !validDuration(next, duration)) duration = CMTimeMake(1, kPianoNativeFps);
    if (!requestedMax && !validDuration(next, maximum)) maximum = CMTimeMake(1, kPianoNativeFps);
    if (!validDuration(next, duration) || !validDuration(next, maximum)) return;
    if (CMTimeCompare(duration, maximum) > 0) {
      // AVCapture clients may set min/max separately, in either order.
      if (requested && !requestedMax) maximum = duration;
      else if (requestedMax && !requested) duration = maximum;
      else return;
    }
    const BOOL different = next != self->_activeFormat || CMTimeCompare(duration, self->_duration) ||
                           CMTimeCompare(maximum, self->_maxDuration);
    self->_activeFormat = next;
    self->_duration = duration;
    self->_maxDuration = maximum;
    if (different) {
      self->_configurationChanged = YES;
      [self retimeOutput];
      changed = @{
        CMIOExtensionPropertyStreamActiveFormatIndex: [CMIOExtensionPropertyState propertyStateWithValue:@(next)],
        CMIOExtensionPropertyStreamFrameDuration: [CMIOExtensionPropertyState propertyStateWithValue:timeDictionary(duration)],
        CMIOExtensionPropertyStreamMaxFrameDuration: [CMIOExtensionPropertyState propertyStateWithValue:timeDictionary(maximum)],
      };
    }
    accepted = YES;
    os_log(gLog, "camera format set sensor=%u index=%lu duration=%lld/%d max=%lld/%d running=%u", self->_sensor,
           (unsigned long)next, duration.value, duration.timescale, maximum.value, maximum.timescale,
           atomic_load(&self->_running));
  });
  if (changed) [_stream notifyPropertiesChanged:changed];
  if (!accepted) {
    os_log_error(gLog, "camera format rejected sensor=%u index=%{public}@ duration=%{public}@ max=%{public}@",
                 _sensor, index, requested, requestedMax);
    if (outError) *outError = [NSError errorWithDomain:NSPOSIXErrorDomain code:EINVAL
                     userInfo:@{NSLocalizedDescriptionKey: @"Expected native 15 fps or 640x480 BGRA at 15..30 fps."}];
  }
  return accepted;
}

- (BOOL)authorizedToStartStreamForClient:(CMIOExtensionClient *)client { return YES; }

// Eight moving bars, a sweeping line and 24 clipped counter squares.
- (void)fill:(CVPixelBufferRef)pb
{
  uint8_t *base = CVPixelBufferGetBaseAddress(pb);
  const size_t stride = CVPixelBufferGetBytesPerRow(pb);
  const int32_t width = (int32_t)CVPixelBufferGetWidth(pb), height = (int32_t)CVPixelBufferGetHeight(pb);
  static const uint8_t bars[8][3] = {{255, 255, 255}, {0, 255, 255}, {255, 255, 0}, {0, 255, 0},
                                     {255, 0, 255}, {0, 0, 255},   {255, 0, 0},   {16, 16, 16}};
  const uint32_t shift = (uint32_t)(_frame * 8 % width), sweep = (uint32_t)(_frame * 6 % height);
  for (int32_t y = 0; y < height; ++y) {
    uint8_t *row = base + (size_t)y * stride;
    for (int32_t x = 0; x < width; ++x) {
      const uint8_t *c = bars[((x + shift) % width) * 8 / width];
      uint8_t *px = row + (size_t)x * 4;
      const int line = (uint32_t)y >= sweep && (uint32_t)y < sweep + 4;
      px[0] = line ? 255 : c[0]; px[1] = line ? 255 : c[1]; px[2] = line ? 255 : c[2]; px[3] = 255;
    }
  }
  const int32_t cell = MAX(1, MIN(48, (width - 32) / 24)), square = MIN(32, cell);
  for (int bit = 0; bit < 24; ++bit) {
    const uint8_t v = (_frame >> (23 - bit)) & 1 ? 255 : 0;
    for (int32_t y = 16; y < MIN(48, height); ++y)
      for (int32_t x = 16 + bit * cell; x < MIN(16 + bit * cell + square, width); ++x) {
        uint8_t *px = base + (size_t)y * stride + (size_t)x * 4;
        px[0] = px[1] = px[2] = v;
      }
  }
}

static IOReturn liveCall(io_connect_t connection, uint32_t op, const void *in, size_t inSize, void *out, size_t size)
{
  return IOConnectCallStructMethod(connection, op, in, inSize, out, &size);
}

static BOOL ioResult(IOReturn status, NSError **outError)
{
  if (status && outError) *outError = [NSError errorWithDomain:NSMachErrorDomain code:status userInfo:nil];
  return status == kIOReturnSuccess;
}

- (void)publish:(CVPixelBufferRef)pb ticks:(uint64_t)ticks sequence:(uint64_t)sequence drops:(uint64_t)drops
{
  if (ticks <= _lastPresentationTicks) { ++_dropped; return; }
  CMTime pts = CMClockMakeHostTimeFromSystemUnits(ticks);
  CMTime ns = CMTimeConvertScale(pts, NSEC_PER_SEC, kCMTimeRoundingMethod_RoundHalfAwayFromZero);
  if (!CMTIME_IS_NUMERIC(ns) || ns.value <= 0 || (uint64_t)ns.value <= _lastPresentationNs) { ++_dropped; return; }
  CMSampleTimingInfo timing = {_duration, pts, kCMTimeInvalid};
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
  const uint64_t interval = (uint64_t)llround(CMTimeGetSeconds(_duration) * NSEC_PER_SEC);
  const BOOL gap = _configurationChanged || (_lastPresentationNs &&
                   (drops != _lastDropCount || (uint64_t)ns.value - _lastPresentationNs > interval * 2));
  if (gap) os_log(gLog, "camera cadence gap sensor=%u seq=%llu delta_ns=%llu source_drops=%llu format_change=%u",
                  _sensor, sequence, _lastPresentationNs ? (uint64_t)ns.value - _lastPresentationNs : 0, drops,
                  _configurationChanged);
  _lastPresentationTicks = ticks;
  _lastPresentationNs = ns.value;
  _lastDropCount = drops;
  _configurationChanged = NO;
  const BOOL repeated = _sensor && sequence == _lastPublishedSequence;
  _lastPublishedSequence = sequence;
  if (repeated) ++_repeated;
  [_stream sendSampleBuffer:sample
              discontinuity:gap ? CMIOExtensionStreamDiscontinuityFlagSampleDropped : CMIOExtensionStreamDiscontinuityFlagNone
       hostTimeInNanoseconds:ns.value];
  CFRelease(sample);
  if (++_frame == 1)
    os_log(gLog, "camera first sample sensor=%u index=%lu seq=%llu pts_ns=%lld", _sensor,
           (unsigned long)_activeFormat, sequence, ns.value);
  if (_frame % 30 == 0)
    os_log(gLog, "camera output sensor=%u index=%lu sent=%llu sources=%llu repeated=%llu dropped=%llu stale_ticks=%llu duration=%lld/%d",
           _sensor, (unsigned long)_activeFormat, _frame, _sourceFrames, _repeated, _dropped, _staleTicks,
           _duration.value, _duration.timescale);
}

static CVPixelBufferRef borrowPixel(CVPixelBufferPoolRef pool, unsigned threshold)
{
  CVPixelBufferRef pb = NULL;
  NSDictionary *limits = @{(__bridge NSString *)kCVPixelBufferPoolAllocationThresholdKey: @(threshold)};
  CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(kCFAllocatorDefault, pool, (__bridge CFDictionaryRef)limits, &pb);
  return pb;
}

- (void)tick
{
  if (!atomic_load(&_running)) return;
  const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  if (_sensor && (!_latest || !pianoOutputSourceFresh(now, _latestSourceNs))) {
    ++_staleTicks;
    if (_latest && !_stalled) {
      os_log_error(gLog, "camera source stalled sensor=%u seq=%llu age_ns=%llu repeated=%llu", _sensor,
                   _latestSequence, now >= _latestSourceNs ? now - _latestSourceNs : 0, _repeated);
      _stalled = YES;
      _configurationChanged = YES;
    }
    return;
  }
  // Two fixed pools, four buffers apiece: format changes cannot grow the output allocation beyond eight.
  CVPixelBufferRef pb = borrowPixel(_outputPools[_activeFormat], 4);
  if (!pb) { ++_dropped; return; }
  if (!CVPixelBufferLockBaseAddress(pb, 0)) {
    BOOL initialized = YES;
    if (!_sensor) [self fill:pb];
    else if (CVPixelBufferLockBaseAddress(_latest, kCVPixelBufferLock_ReadOnly)) initialized = NO;
    else {
      pianoOutputScaleBGRA(CVPixelBufferGetBaseAddress(_latest), _nativeWidth, _nativeHeight,
                           CVPixelBufferGetBytesPerRow(_latest), CVPixelBufferGetBaseAddress(pb),
                           (uint32_t)CVPixelBufferGetWidth(pb), (uint32_t)CVPixelBufferGetHeight(pb),
                           CVPixelBufferGetBytesPerRow(pb));
      CVPixelBufferUnlockBaseAddress(_latest, kCVPixelBufferLock_ReadOnly);
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    if (initialized && atomic_load(&_running))
      [self publish:pb ticks:mach_absolute_time() sequence:_sensor ? _latestSequence : _frame + 1
              drops:_latestDrops + atomic_load(&_captureDropped)];
    else ++_dropped;
  } else ++_dropped;
  CVPixelBufferRelease(pb);
}

- (BOOL)convert:(const PianoCameraLiveFrame *)frame pixel:(CVPixelBufferRef)pb
{
  const BOOL rear = _sensor == kPianoCameraRear;
  const uint32_t rawWidth = rear ? 4080 : 3264, rawHeight = rear ? 3072 : 1836, stride = rear ? 5104 : 4080;
  if (frame->version != kPianoCameraLiveVersion || frame->sensor != _sensor || frame->slot >= kPianoCameraSlots ||
      frame->width != rawWidth || frame->height != rawHeight || frame->stride != stride ||
      frame->bayer != (rear ? kPianoCameraGRBG : kPianoCameraBGGR) || frame->black >= 1023 ||
      frame->bytes != stride * rawHeight || frame->bytes > kPianoCameraSlotBytes || !frame->sofTicks ||
      frame->sequence <= _lastReadSequence ||
      (uint64_t)frame->slot * kPianoCameraSlotBytes + frame->bytes > _rawSize) return NO;
  uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  PianoIspStats stats;
  const uint8_t *raw = (const uint8_t *)(uintptr_t)_raw + (uint64_t)frame->slot * kPianoCameraSlotBytes;
  if (pianoIsp(raw, rawWidth, rawHeight, stride, frame->bayer, frame->black, 2, _work, _rgb, &stats) ||
      stats.width != (uint32_t)_nativeWidth || stats.height != (uint32_t)_nativeHeight) return NO;
  if (CVPixelBufferLockBaseAddress(pb, 0)) return NO;
  uint8_t *base = CVPixelBufferGetBaseAddress(pb);
  const size_t rowBytes = CVPixelBufferGetBytesPerRow(pb);
  for (int32_t y = 0; y < _nativeHeight; ++y) {
    uint8_t *row = base + y * rowBytes;
    for (int32_t x = 0; x < _nativeWidth; ++x) {
      // The rear sensor is mounted upside down. Leave selfie mirroring to clients.
      size_t source = rear ? (size_t)(_nativeHeight - 1 - y) * _nativeWidth + _nativeWidth - 1 - x :
                             (size_t)y * _nativeWidth + x;
      row[x * 4 + 0] = _rgb[source * 3 + 2]; row[x * 4 + 1] = _rgb[source * 3 + 1];
      row[x * 4 + 2] = _rgb[source * 3 + 0]; row[x * 4 + 3] = 255;
    }
  }
  CVPixelBufferUnlockBaseAddress(pb, 0);
  const uint64_t elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
  _ispTotal += elapsed;
  _ispMax = MAX(_ispMax, elapsed);
  ++_converted;
  return YES;
}

- (void)readLive
{
  uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 8 * NSEC_PER_SEC;
  while (atomic_load(&_running)) {
    @autoreleasepool {
      PianoCameraLiveFrame frame = {0};
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
      CVPixelBufferRef pb = borrowPixel(_sourcePool, 3);
      const BOOL valid = pb && [self convert:&frame pixel:pb];
      PianoCameraLiveReleaseArgs release = {kPianoCameraLiveVersion, frame.slot, frame.sequence};
      result = liveCall(_connection, kPianoCameraLiveRelease, &release, sizeof release, NULL, 0);
      if (valid && !result && atomic_load(&_running)) {
        _lastReadSequence = frame.sequence;
        const CMTime source = CMTimeConvertScale(CMClockMakeHostTimeFromSystemUnits(frame.sofTicks), NSEC_PER_SEC,
                                                kCMTimeRoundingMethod_RoundHalfAwayFromZero);
        const uint64_t sourceNs = CMTIME_IS_NUMERIC(source) && source.value > 0 ? (uint64_t)source.value : 0;
        // Backpressure here bounds the handoff to one frame; never retain an accumulating async queue of sources.
        dispatch_sync(_outputQueue, ^{
          if (!atomic_load(&self->_running)) return;
          if (self->_latest) CVPixelBufferRelease(self->_latest);
          self->_latest = CVPixelBufferRetain(pb);
          self->_latestSequence = frame.sequence;
          self->_latestDrops = frame.drops;
          self->_latestSourceNs = sourceNs;
          ++self->_sourceFrames;
          if (self->_stalled) os_log(gLog, "camera source resumed sensor=%u seq=%llu", self->_sensor, frame.sequence);
          self->_stalled = NO;
        });
      } else atomic_fetch_add(&_captureDropped, 1);
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
  _lastReadSequence = _ispTotal = _ispMax = _converted = 0;
  atomic_store(&_captureDropped, 0);
  dispatch_sync(_outputQueue, ^{
    self->_frame = self->_dropped = self->_repeated = self->_sourceFrames = self->_staleTicks = 0;
    self->_lastPublishedSequence = self->_lastPresentationTicks = self->_lastPresentationNs = self->_lastDropCount = 0;
    self->_latestSequence = self->_latestDrops = self->_latestSourceNs = 0;
    self->_stalled = self->_configurationChanged = NO;
  });
  if (_sensor) {
    _work = malloc((size_t)_nativeWidth * _nativeHeight * 3 * sizeof(float));
    _rgb = malloc((size_t)_nativeWidth * _nativeHeight * 3);
    const char *step = !_work ? "isp-work" : !_rgb ? "isp-rgb" : "service-open";
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
      step = "abi2-start";
      PianoCameraLiveStartArgs req = {kPianoCameraLiveVersion, _sensor};
      result = liveCall(_connection, kPianoCameraLiveStart, &req, sizeof req, NULL, 0);
      os_log(gLog, "camera ABI2 start sensor=%u result=%#x", _sensor, result);
    }
    if (!result) {
      step = "ring-map";
      result = IOConnectMapMemory64(_connection, kPianoCameraMemoryRing, mach_task_self(), &_raw, &_rawSize,
                                    kIOMapAnywhere | kIOMapReadOnly);
      os_log(gLog, "camera ring map sensor=%u bytes=%llu result=%#x", _sensor, _rawSize, result);
      if (!result && _rawSize != kPianoCameraSlots * (uint64_t)kPianoCameraSlotBytes) {
        step = "ring-length";
        result = kIOReturnBadArgument;
      }
    }
    if (result) {
      os_log_error(gLog, "camera stream start failed sensor=%u step=%{public}s result=%#x", _sensor, step, result);
      [self closeLive];
      return ioResult(result, outError);
    }
  }
  atomic_store(&_running, true);
  dispatch_sync(_outputQueue, ^{
    self->_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self->_outputQueue);
    __weak PianoStreamSource *weakSelf = self;
    dispatch_source_set_event_handler(self->_timer, ^{ [weakSelf tick]; });
    [self retimeOutput];
    dispatch_resume(self->_timer);
    os_log(gLog, "camera stream start sensor=%u index=%lu duration=%lld/%d source_fps=%d", self->_sensor,
           (unsigned long)self->_activeFormat, self->_duration.value, self->_duration.timescale, kPianoNativeFps);
  });
  if (_sensor) dispatch_async(_captureQueue, ^{ [self readLive]; });
  return YES;
}

- (void)closeLive
{
  if (_raw) IOConnectUnmapMemory64(_connection, kPianoCameraMemoryRing, mach_task_self(), _raw);
  if (_connection) IOServiceClose(_connection);
  _raw = _rawSize = _connection = 0;
  free(_rgb); free(_work);
  _rgb = NULL; _work = NULL;
}

- (BOOL)stopStreamAndReturnError:(NSError **)outError
{
  if (!atomic_exchange(&_running, false)) return YES;
  // Cancel on the output queue, then stop/wake the RAW waiter, drain both queues, and only then free mappings/ISP memory.
  dispatch_sync(_outputQueue, ^{
    if (self->_timer) dispatch_source_cancel(self->_timer);
    self->_timer = nil;
  });
  PianoCameraLiveStatus state = {0};
  IOReturn result = _connection ? liveCall(_connection, kPianoCameraLiveStop, NULL, 0, &state, sizeof state) : kIOReturnSuccess;
  dispatch_sync(_captureQueue, ^{});
  dispatch_sync(_outputQueue, ^{
    if (self->_latest) CVPixelBufferRelease(self->_latest);
    self->_latest = NULL;
    os_log(gLog, "camera output stop sensor=%u sent=%llu sources=%llu repeated=%llu dropped=%llu stale_ticks=%llu",
           self->_sensor, self->_frame, self->_sourceFrames, self->_repeated, self->_dropped, self->_staleTicks);
  });
  [self closeLive];
  os_log(gLog, "camera stream stop sensor=%u result=%#x state=%u clean=%u guard=%u smmu=%#x capture_dropped=%llu kernel_drops=%llu converted=%llu isp_avg_ns=%llu isp_max_ns=%llu",
         _sensor, result, state.state, state.cleanOff, state.guardFault, state.smmuFault, (uint64_t)atomic_load(&_captureDropped),
         state.dropped, _converted, _converted ? _ispTotal / _converted : 0, _ispMax);
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
    os_log(gLog, "PianoCamera extension starting: sensor=%d fps compat=15..30 fps", kPianoNativeFps);
    dispatch_queue_t control = dispatch_queue_create("com.unixnu.PianoCamera.client", DISPATCH_QUEUE_SERIAL);
    __attribute__((objc_precise_lifetime)) PianoProviderSource *source = [[PianoProviderSource alloc] initWithClientQueue:control];
    if (!source) return 1;
    [CMIOExtensionProvider startServiceWithProvider:source.provider];
    CFRunLoopRun();
  }
  return 0;
}
