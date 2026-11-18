// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Exercise the actual CMIO metadata/setter and pattern timer without opening camera hardware.
#define main pianoCameraProviderMain
#include "../PianoCameraExtension.m"
#undef main
#include <assert.h>
#include <unistd.h>

@interface OutputProbe : PianoStreamSource
@property(nonatomic, strong) NSMutableArray<NSDictionary *> *observations;
@end
@implementation OutputProbe
- (void)publish:(CVPixelBufferRef)pb ticks:(uint64_t)ticks sequence:(uint64_t)sequence drops:(uint64_t)drops
{
  [super publish:pb ticks:ticks sequence:sequence drops:drops];
  assert(!CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly));
  const size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb);
  if (CVPixelBufferGetPixelFormatType(pb) == kCVPixelFormatType_32BGRA) {
    const uint8_t *bytes = CVPixelBufferGetBaseAddress(pb);
    const size_t stride = CVPixelBufferGetBytesPerRow(pb);
    for (size_t y = 0; y < h; ++y)
      for (size_t x = 0; x < w; ++x) assert(bytes[y * stride + x * 4 + 3] == 255);
  } else {
    assert(CVPixelBufferGetPlaneCount(pb) == 2);
    assert(CVPixelBufferGetWidthOfPlane(pb, 0) == w && CVPixelBufferGetHeightOfPlane(pb, 0) == h);
    assert(CVPixelBufferGetWidthOfPlane(pb, 1) == w / 2 && CVPixelBufferGetHeightOfPlane(pb, 1) == h / 2);
    for (unsigned plane = 0; plane < 2; ++plane) {
      const uint8_t *bytes = CVPixelBufferGetBaseAddressOfPlane(pb, plane);
      const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pb, plane);
      for (size_t y = 0; y < (h >> plane); ++y) for (size_t x = 0; x < w; ++x)
        assert(bytes[y * stride + x] >= 16 && bytes[y * stride + x] <= (plane ? 240 : 235));
    }
  }
  CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
  @synchronized (self) {
    [self.observations addObject:@{@"width": @(w), @"height": @(h), @"ticks": @(ticks), @"sequence": @(sequence)}];
  }
}
@end

// Supply exactly three native source frames at 15 fps, then stall without stopping output.
@interface SyntheticSource : OutputProbe
@end
@implementation SyntheticSource
- (BOOL)openLiveAndReturnError:(NSError **)outError { return YES; }
- (IOReturn)stopLive:(PianoCameraLiveStatus *)state
{
  state->state = kPianoCameraStopped;
  state->cleanOff = 1;
  return kIOReturnSuccess;
}
- (void)readLive
{
  usleep(50000);
  for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
    CVPixelBufferRef pb = NULL;
    assert(!CVPixelBufferCreate(kCFAllocatorDefault, 816, 459, kCVPixelFormatType_32BGRA,
                               (__bridge CFDictionaryRef)@{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}}, &pb));
    assert(!CVPixelBufferLockBaseAddress(pb, 0));
    uint8_t *bytes = CVPixelBufferGetBaseAddress(pb);
    const size_t stride = CVPixelBufferGetBytesPerRow(pb);
    for (unsigned y = 0; y < 459; ++y)
      for (unsigned x = 0; x < 816; ++x) {
        uint8_t *pixel = bytes + y * stride + x * 4;
        pixel[0] = (uint8_t)sequence; pixel[1] = pixel[2] = 37; pixel[3] = 255;
      }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    CMTime timestamp = CMTimeConvertScale(CMClockMakeHostTimeFromSystemUnits(mach_absolute_time()), NSEC_PER_SEC,
                                         kCMTimeRoundingMethod_RoundHalfAwayFromZero);
    [self acceptSource:pb sequence:sequence drops:0 sourceNs:(uint64_t)timestamp.value];
    CVPixelBufferRelease(pb);
    usleep(66667);
  }
}
@end

static BOOL set(PianoStreamSource *source, NSNumber *index, CMTime duration, CMTime max)
{
  CMIOExtensionStreamProperties *p = [CMIOExtensionStreamProperties streamPropertiesWithDictionary:@{}];
  p.activeFormatIndex = index;
  if (CMTIME_IS_VALID(duration)) p.frameDuration = timeDictionary(duration);
  if (CMTIME_IS_VALID(max)) p.maxFrameDuration = timeDictionary(max);
  NSError *error = nil;
  BOOL result = [source setStreamProperties:p error:&error];
  assert(result || error.code == EINVAL);
  return result;
}

static void state(PianoStreamSource *source, unsigned index, int fps, int maximumFps)
{
  CMIOExtensionStreamProperties *p = [source streamPropertiesForProperties:source.availableProperties error:NULL];
  assert(p.activeFormatIndex.unsignedIntValue == index);
  assert(!CMTimeCompare(CMTimeMakeFromDictionary((__bridge CFDictionaryRef)p.frameDuration), CMTimeMake(1, fps)));
  assert(!CMTimeCompare(CMTimeMakeFromDictionary((__bridge CFDictionaryRef)p.maxFrameDuration), CMTimeMake(1, maximumFps)));
}

static void metadata(void)
{
  const int widths[] = {1280, 816, 1020}, heights[] = {720, 459, 768};
  for (unsigned i = 0; i < 3; ++i) {
    PianoStreamSource *source = [[PianoStreamSource alloc] initWithWidth:widths[i] height:heights[i] sensor:i];
    assert(source && source.formats.count == 2);
    for (unsigned j = 0; j < 2; ++j) {
      CMIOExtensionStreamFormat *f = source.formats[j];
      CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(f.formatDescription);
      assert(d.width == (j ? 640 : widths[i]) && d.height == (j ? 480 : heights[i]));
      assert(CMFormatDescriptionGetMediaSubType(f.formatDescription) == (j ? kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange : kCVPixelFormatType_32BGRA));
      assert(!CMTimeCompare(f.minFrameDuration, CMTimeMake(1, j ? 30 : 15)));
      assert(!CMTimeCompare(f.maxFrameDuration, CMTimeMake(1, 15)));
      assert(!f.validFrameDurations.count);
    }
    state(source, 0, 15, 15);
    assert(!set(source, @0, CMTimeMake(1, 30), kCMTimeInvalid));
    assert(!set(source, @2, kCMTimeInvalid, kCMTimeInvalid));
    assert(!set(source, @(-1), kCMTimeInvalid, kCMTimeInvalid));
    assert(!set(source, @0.5, kCMTimeInvalid, kCMTimeInvalid));
    assert(set(source, @1, CMTimeMake(1, 30), CMTimeMake(1, 30)));
    state(source, 1, 30, 30);
    assert(!set(source, nil, CMTimeMake(1, 31), kCMTimeInvalid));
    assert(!set(source, nil, CMTimeMake(1, 14), kCMTimeInvalid));
    assert(!set(source, nil, kCMTimePositiveInfinity, kCMTimeInvalid));
    assert(!set(source, nil, CMTimeMake(1, 15), CMTimeMake(1, 30)));
    state(source, 1, 30, 30);
    // Thermal downshift and normal upshift, with the two properties arriving in either order.
    assert(set(source, nil, CMTimeMake(1, 15), kCMTimeInvalid));
    state(source, 1, 15, 15);
    assert(set(source, nil, kCMTimeInvalid, CMTimeMake(1, 30)));
    state(source, 1, 30, 30);
    assert(set(source, nil, kCMTimeInvalid, CMTimeMake(1, 15)));
    state(source, 1, 30, 15);
    assert(set(source, nil, CMTimeMake(1, 24), kCMTimeInvalid));
    state(source, 1, 24, 15);
    assert(set(source, @0, kCMTimeInvalid, kCMTimeInvalid));
    state(source, 0, 15, 15);
  }
}

static void boundedPools(void)
{
  CVPixelBufferPoolRef pools[2] = {NULL, NULL};
  CVPixelBufferRef held[8] = {NULL};
  assert(!makePool(816, 459, &pools[0]) && !makePool(640, 480, &pools[1]));
  for (unsigned i = 0; i < 8; ++i) { held[i] = borrowPixel(pools[i / 4], 4); assert(held[i]); }
  assert(!borrowPixel(pools[0], 4) && !borrowPixel(pools[1], 4));
  CVPixelBufferRelease(held[0]);
  held[0] = borrowPixel(pools[0], 4);
  assert(held[0]);
  for (unsigned i = 0; i < 8; ++i) CVPixelBufferRelease(held[i]);
  CVPixelBufferPoolRelease(pools[0]); CVPixelBufferPoolRelease(pools[1]);
}

static void timer(void)
{
  OutputProbe *source = [[OutputProbe alloc] initWithWidth:1280 height:720 sensor:0];
  source.observations = [NSMutableArray array];
  for (unsigned round = 0; round < 3; ++round) {
    assert(set(source, @1, CMTimeMake(1, 30), CMTimeMake(1, 30)));
    assert([source startStreamAndReturnError:NULL]);
    usleep(250000);
    @synchronized (source) { assert(source.observations.count >= 6 && source.observations.count <= 10); }
    assert(set(source, @0, kCMTimeInvalid, kCMTimeInvalid));
    usleep(200000);
    assert(set(source, @1, CMTimeMake(1, 24), CMTimeMake(1, 24)));
    usleep(200000);
    assert([source stopStreamAndReturnError:NULL]);
    NSUInteger stoppedCount = source.observations.count;
    usleep(100000);
    assert(source.observations.count == stoppedCount);
    unsigned compat = 0, native = 0;
    uint64_t last = 0;
    for (NSDictionary *frame in source.observations) {
      uint64_t ticks = [frame[@"ticks"] unsignedLongLongValue];
      assert(ticks > last);
      last = ticks;
      if ([frame[@"width"] unsignedIntValue] == 640) { ++compat; assert([frame[@"height"] unsignedIntValue] == 480); }
      else { ++native; assert([frame[@"width"] unsignedIntValue] == 1280 && [frame[@"height"] unsignedIntValue] == 720); }
    }
    assert(compat >= 8 && native >= 2);
    [source.observations removeAllObjects];
  }
}

static void repeatedAndStalled(void)
{
  SyntheticSource *source = [[SyntheticSource alloc] initWithWidth:816 height:459 sensor:kPianoCameraFront];
  source.observations = [NSMutableArray array];
  assert(set(source, @1, CMTimeMake(1, 30), CMTimeMake(1, 30)));
  assert([source startStreamAndReturnError:NULL]);
  usleep(20000);
  @synchronized (source) { assert(!source.observations.count); } // no source, no uninitialized output
  usleep(500000);
  NSUInteger count;
  @synchronized (source) { count = source.observations.count; }
  usleep(180000);
  @synchronized (source) { assert(source.observations.count == count); } // timer keeps running, stale source does not repeat
  assert([source stopStreamAndReturnError:NULL]);
  assert(count >= 8 && count <= 15);
  uint64_t previous = 0, ticks = 0;
  unsigned repeated = 0, unique = 0;
  for (NSDictionary *frame in source.observations) {
    uint64_t sequence = [frame[@"sequence"] unsignedLongLongValue];
    uint64_t current = [frame[@"ticks"] unsignedLongLongValue];
    assert(sequence >= 1 && sequence <= 3 && current > ticks);
    assert([frame[@"width"] unsignedIntValue] == 640 && [frame[@"height"] unsignedIntValue] == 480);
    if (sequence == previous) ++repeated;
    else ++unique;
    previous = sequence;
    ticks = current;
  }
  assert(repeated >= 4 && unique == 3);
}

int main(void)
{
  @autoreleasepool {
    gLog = os_log_create("com.unixnu.PianoCamera", "host-tests");
    metadata();
    boundedPools();
    timer();
    repeatedAndStalled();
    puts("camera provider metadata/transitions/pattern/cancellation/repeats/stall/pool-bounds PASS");
  }
  return 0;
}
