// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Exercise the actual CMIO metadata/setter and pattern timer without opening camera hardware.
#ifdef PIANO_CAMERA_HOST_HEAP
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

// The CI host may deny IOSurface's IOKit service. Exercise the same provider and CoreVideo operations with local buffers.
static CVReturn heapPoolCreate(CFAllocatorRef a, CFDictionaryRef options, CFDictionaryRef attributes, CVPixelBufferPoolRef *pool)
{
  NSMutableDictionary *local = [(__bridge NSDictionary *)attributes mutableCopy];
  [local removeObjectForKey:(id)kCVPixelBufferIOSurfacePropertiesKey];
  [local removeObjectForKey:(id)kCVPixelBufferMetalCompatibilityKey];
  return CVPixelBufferPoolCreate(a, options, (__bridge CFDictionaryRef)local, pool);
}
#define CVPixelBufferPoolCreate heapPoolCreate
#endif
#define main pianoCameraProviderMain
#include "../PianoCameraExtension.m"
#undef main
#include <assert.h>
#include <unistd.h>

static NSDictionary *testPixelAttributes(void)
{
#ifdef PIANO_CAMERA_HOST_HEAP
  return nil;
#else
  return @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
#endif
}

@interface OutputProbe : PianoStreamSource
@property(nonatomic, strong) NSMutableArray<NSDictionary *> *observations;
@property(nonatomic, strong) NSMutableArray<NSNumber *> *flags;
@end
@implementation OutputProbe
- (void)deliverSample:(CMSampleBufferRef)sample discontinuity:(CMIOExtensionStreamDiscontinuityFlags)flags hostTime:(uint64_t)ns
{
  @synchronized (self) { [self.flags addObject:@(flags)]; }
  [super deliverSample:sample discontinuity:flags hostTime:ns];
}
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
                               (__bridge CFDictionaryRef)testPixelAttributes(), &pb));
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

// The front source at 15 fps whose kernel drop counter changes with every frame (the rear drops 8-10 % on the tablet), then a
// 450 ms stall (past the 250 ms source age), then four more frames.
@interface DroppingSource : SyntheticSource
@end
@implementation DroppingSource
- (void)readLive
{
  usleep(50000);
  for (uint64_t sequence = 1; sequence <= 10; ++sequence) {
    if (sequence == 7) usleep(450000);
    CVPixelBufferRef pb = NULL;
    assert(!CVPixelBufferCreate(kCFAllocatorDefault, 816, 459, kCVPixelFormatType_32BGRA,
                               (__bridge CFDictionaryRef)testPixelAttributes(), &pb));
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
    [self acceptSource:pb sequence:sequence drops:sequence * 3 sourceNs:(uint64_t)timestamp.value];
    CVPixelBufferRelease(pb);
    usleep(66667);
  }
}
@end

// A client that keeps the last HOLD delivered buffers, as VideoToolbox's software H.264 encoder keeps ~15 inputs before its first
// output (verify-1: with a 4-buffer pool it got 4 frames, then nothing).
@interface HoldingProbe : OutputProbe
@property(nonatomic) NSUInteger hold;
@property(nonatomic, strong) NSMutableArray *held;
@end
@implementation HoldingProbe
- (void)publish:(CVPixelBufferRef)pb ticks:(uint64_t)ticks sequence:(uint64_t)sequence drops:(uint64_t)drops
{
  [super publish:pb ticks:ticks sequence:sequence drops:drops];
  @synchronized (self) {
    [self.held addObject:(__bridge id)pb];
    while (self.held.count > self.hold) [self.held removeObjectAtIndex:0];
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
  const uint32_t sensors[] = {0, kPianoCameraFront, kPianoCameraRear};
  for (unsigned i = 0; i < 3; ++i) {
    PianoStreamSource *source = [[PianoStreamSource alloc] initWithWidth:widths[i] height:heights[i] sensor:sensors[i]];
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

static void invalidFrames(void)
{
  assert(![[PianoStreamSource alloc] initWithWidth:0 height:720 sensor:0]);
  assert(![[PianoStreamSource alloc] initWithWidth:1280 height:-1 sensor:0]);
  assert(![[PianoStreamSource alloc] initWithWidth:INT32_MAX height:INT32_MAX sensor:0]);
  assert(![[PianoStreamSource alloc] initWithWidth:816 height:612 sensor:kPianoCameraFront]);
  assert(![[PianoStreamSource alloc] initWithWidth:816 height:459 sensor:3]);
  const PianoCameraLiveFrame front = { .version = kPianoCameraLiveVersion, .sensor = kPianoCameraFront,
      .slot = kPianoCameraSlots - 1, .width = 3264, .height = 1836, .stride = 4080, .bayer = kPianoCameraBGGR, .black = 64,
      .bytes = 7490880, .sequence = 1, .sofTicks = 1 };
  const uint64_t end = (uint64_t)front.slot * kPianoCameraSlotBytes + front.bytes;
  assert(validLiveFrame(&front, kPianoCameraFront, end, 0));
  assert(!validLiveFrame(NULL, kPianoCameraFront, end, 0));
  assert(!validLiveFrame(&front, kPianoCameraRear, end, 0));
  assert(!validLiveFrame(&front, kPianoCameraFront, end - 1, 0));
  assert(!validLiveFrame(&front, kPianoCameraFront, end, 1));
  PianoCameraLiveFrame invalid = front;
  invalid.width = 6528; invalid.height = 4896; invalid.stride = 8160; invalid.bytes = 39951360;
  assert(!validLiveFrame(&invalid, kPianoCameraFront, UINT64_MAX, 0));
  invalid = front; invalid.height = 2448; invalid.bytes = 9987840;
  assert(!validLiveFrame(&invalid, kPianoCameraFront, UINT64_MAX, 0));
  invalid = front; invalid.slot = UINT32_MAX;
  assert(!validLiveFrame(&invalid, kPianoCameraFront, UINT64_MAX, 0));
  invalid = front; invalid.black = 1023;
  assert(!validLiveFrame(&invalid, kPianoCameraFront, end, 0));
}

@interface LifecycleProbe : PianoStreamSource
@property(nonatomic, strong) dispatch_semaphore_t opening, allowOpen, stopReading;
@property(nonatomic) BOOL failOpen, reportUnclean, readFinished;
@property(nonatomic) unsigned opens, stops, closes;
@end

@implementation LifecycleProbe
- (BOOL)openLiveAndReturnError:(NSError **)outError
{
  ++self.opens;
  self.readFinished = NO;
  if (self.opening) {
    dispatch_semaphore_signal(self.opening);
    assert(!dispatch_semaphore_wait(self.allowOpen, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
  }
  return self.failOpen ? ioResult(kIOReturnNoMemory, outError) : YES;
}
- (void)readLive
{
  assert(!dispatch_semaphore_wait(self.stopReading, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
  self.readFinished = YES;
}
- (IOReturn)stopLive:(PianoCameraLiveStatus *)state
{
  ++self.stops;
  state->state = kPianoCameraStopped;
  state->cleanOff = !self.reportUnclean;
  dispatch_semaphore_signal(self.stopReading);
  return kIOReturnSuccess;
}
- (void)closeLive
{
  assert(self.readFinished);
  ++self.closes;
}
@end

static void lifecycle(void)
{
  LifecycleProbe *source = [[LifecycleProbe alloc] initWithWidth:816 height:459 sensor:kPianoCameraFront];
  source.failOpen = YES;
  NSError *error = nil;
  assert(![source startStreamAndReturnError:&error] && error.code == kIOReturnNoMemory);
  assert([source stopStreamAndReturnError:NULL] && source.stops == 0);
  source.failOpen = NO;
  source.stopReading = dispatch_semaphore_create(0);
  source.opening = dispatch_semaphore_create(0);
  source.allowOpen = dispatch_semaphore_create(0);
  dispatch_semaphore_t stopping = dispatch_semaphore_create(0), stopped = dispatch_semaphore_create(0);
  dispatch_group_t group = dispatch_group_create();
  dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
  dispatch_group_async(group, queue, ^{ assert([source startStreamAndReturnError:NULL]); });
  assert(!dispatch_semaphore_wait(source.opening, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
  dispatch_group_async(group, queue, ^{
    dispatch_semaphore_signal(stopping);
    assert([source stopStreamAndReturnError:NULL]);
    dispatch_semaphore_signal(stopped);
  });
  assert(!dispatch_semaphore_wait(stopping, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
  assert(dispatch_semaphore_wait(stopped, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)));
  dispatch_semaphore_signal(source.allowOpen);
  assert(!dispatch_group_wait(group, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
  assert(source.opens == 2 && source.stops == 1 && source.closes == 1);

  source.opening = nil;
  for (unsigned round = 0; round < 3; ++round) {
    source.reportUnclean = round == 0;
    assert([source startStreamAndReturnError:NULL] && [source startStreamAndReturnError:NULL]);
    if (round == 0) assert(![source stopStreamAndReturnError:&error] && error.code == kIOReturnIOError);
    else assert([source stopStreamAndReturnError:NULL]);
    assert([source stopStreamAndReturnError:NULL]);
    assert(source.opens == round + 3 && source.stops == round + 2 && source.closes == round + 2);
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
  source.flags = [NSMutableArray array];
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
    // the two live format changes of the round, and nothing else, break the timeline
    unsigned flagged = 0;
    for (NSNumber *flag in source.flags) flagged += flag.unsignedIntValue == CMIOExtensionStreamDiscontinuityFlagSampleDropped;
    assert(source.flags.count == source.observations.count && flagged == 2);
    [source.observations removeAllObjects];
    [source.flags removeAllObjects];
  }
}

static void encoderHold(void)
{
  for (unsigned index = 0; index < 2; ++index) {
    HoldingProbe *source = [[HoldingProbe alloc] initWithWidth:1280 height:720 sensor:0];
    source.observations = [NSMutableArray array];
    source.flags = [NSMutableArray array];
    source.held = [NSMutableArray array];
    source.hold = 18;
    assert(set(source, @(index), CMTimeMake(1, index ? 30 : 15), CMTimeMake(1, index ? 30 : 15)));
    assert([source startStreamAndReturnError:NULL]);
    usleep(index ? 1200000 : 2000000);
    assert([source stopStreamAndReturnError:NULL]);
    NSUInteger sent = source.observations.count;
    printf("encoder hold index=%u: %lu frames delivered while the client held the last 18\n", index, (unsigned long)sent);
    assert(sent >= 26);   // 30 or 15 fps; with the old 4-buffer bound this stopped at 4
    for (NSNumber *flag in source.flags) assert(flag.unsignedIntValue == CMIOExtensionStreamDiscontinuityFlagNone);
    [source.held removeAllObjects];
  }
}

static void droppingTimeline(void)
{
  DroppingSource *source = [[DroppingSource alloc] initWithWidth:816 height:459 sensor:kPianoCameraFront];
  source.observations = [NSMutableArray array];
  source.flags = [NSMutableArray array];
  assert(set(source, @1, CMTimeMake(1, 30), CMTimeMake(1, 30)));
  assert([source startStreamAndReturnError:NULL]);
  usleep(1500000);
  assert([source stopStreamAndReturnError:NULL]);
  unsigned flagged = 0, firstAfterStall = 0;
  for (NSUInteger i = 0; i < source.flags.count; ++i) {
    if (source.flags[i].unsignedIntValue != CMIOExtensionStreamDiscontinuityFlagSampleDropped) continue;
    ++flagged;
    firstAfterStall = [source.observations[i][@"sequence"] unsignedIntValue];
  }
  // the kernel drop counter changed on every frame: no break for it; one break after the 450 ms stall, at its first frame
  printf("dropping source: %lu samples, %u discontinuity at sequence %u\n", (unsigned long)source.flags.count, flagged, firstAfterStall);
  assert(source.flags.count == source.observations.count && source.flags.count >= 20 && flagged == 1 && firstAfterStall == 7);
}

static void poolMemory(void)
{
  // the memory a client can pin through each bounded pool (IOSurface allocations, page rounded)
  const struct { int w, h; OSType type; const char *what; } pools[] = {
    {640, 480, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, "640x480 420v (index 1, the apps)"},
    {816, 459, kCVPixelFormatType_32BGRA, "816x459 BGRA (front native)"},
    {1020, 768, kCVPixelFormatType_32BGRA, "1020x768 BGRA (rear native)"},
  };
  for (unsigned p = 0; p < 3; ++p) {
    CVPixelBufferPoolRef pool = NULL;
    NSDictionary *attrs = @{(id)kCVPixelBufferWidthKey: @(pools[p].w), (id)kCVPixelBufferHeightKey: @(pools[p].h),
                            (id)kCVPixelBufferPixelFormatTypeKey: @(pools[p].type), (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
    assert(!CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)attrs, &pool));
    CVPixelBufferRef held[kPianoDeliveryBuffers];
    size_t bytes = 0;
    for (unsigned i = 0; i < kPianoDeliveryBuffers; ++i) {
      held[i] = borrowPixel(pool, kPianoDeliveryBuffers);
      assert(held[i]);
#ifdef PIANO_CAMERA_HOST_HEAP
      bytes += CVPixelBufferGetDataSize(held[i]);
#else
      bytes += IOSurfaceGetAllocSize(CVPixelBufferGetIOSurface(held[i]));
#endif
    }
    assert(!borrowPixel(pool, kPianoDeliveryBuffers));
    printf("pool bound %s: %u buffers, %.1f MB at most\n", pools[p].what, (unsigned)kPianoDeliveryBuffers, bytes / 1048576.0);
    for (unsigned i = 0; i < kPianoDeliveryBuffers; ++i) CVPixelBufferRelease(held[i]);
    CVPixelBufferPoolRelease(pool);
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
    invalidFrames();
    lifecycle();
    boundedPools();
    timer();
    repeatedAndStalled();
    encoderHold();
    droppingTimeline();
    poolMemory();
    puts("camera provider metadata/transitions/pattern/cancellation/repeats/stall/pool-bounds/encoder-hold/drop-timeline PASS");
  }
  return 0;
}
