// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>
#import <CommonCrypto/CommonDigest.h>
#import "Devices.h"

@interface FrameSink : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic) NSUInteger frames, invalid, dropped;
@property(nonatomic) size_t expectedWidth, expectedHeight;
@property(nonatomic) BOOL moved, scene, saved;
@property(nonatomic) uint64_t firstFingerprint;
@property(nonatomic) CMTime lastPTS;
@property(nonatomic, copy) NSString *file;
@end

@implementation FrameSink
- (void)captureOutput:(AVCaptureOutput *)output didOutputSampleBuffer:(CMSampleBufferRef)sample
       fromConnection:(AVCaptureConnection *)connection
{
    CVPixelBufferRef pixel = CMSampleBufferGetImageBuffer(sample);
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
    if (!pixel || CVPixelBufferGetPixelFormatType(pixel) != kCVPixelFormatType_32BGRA || !CMTIME_IS_NUMERIC(pts) ||
        (_frames && CMTimeCompare(pts, _lastPTS) <= 0) || CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly)) {
        ++_invalid;
        return;
    }
    size_t width = CVPixelBufferGetWidth(pixel), height = CVPixelBufferGetHeight(pixel);
    size_t stride = CVPixelBufferGetBytesPerRow(pixel);
    const uint8_t *base = CVPixelBufferGetBaseAddress(pixel);
    if (!base || width != _expectedWidth || height != _expectedHeight || stride < width * 4) {
        CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
        ++_invalid;
        return;
    }
    unsigned lo = 255, hi = 0;
    uint64_t fingerprint = 14695981039346656037ULL;
    for (size_t y = 0; y < height; y += 8) {
        for (size_t x = 0; x < width; x += 8) {
            const uint8_t *p = base + y * stride + x * 4;
            unsigned level = (p[0] + 2 * p[1] + p[2]) / 4;
            lo = MIN(lo, level);
            hi = MAX(hi, level);
            fingerprint = (fingerprint ^ level) * 1099511628211ULL;
        }
    }
    CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
    _lastPTS = pts;
    if (++_frames == 1) {
        _firstFingerprint = fingerprint;
        printf("first-frame %zux%zu stride=%zu FourCC=BGRA pts=%.9f sequence=%lu luminance=%u..%u\n",
               width, height, stride, CMTimeGetSeconds(pts), (unsigned long)_frames, lo, hi);
    }
    _moved |= fingerprint != _firstFingerprint;
    BOOL scene = hi >= 16 && hi - lo >= 8;
    _scene |= scene;
    if (_frames == 1 && _file) {
        CIImage *image = [CIImage imageWithCVPixelBuffer:pixel];
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        NSError *error = nil;
        _saved = [[CIContext contextWithOptions:@{kCIContextUseSoftwareRenderer: @YES}]
            writePNGRepresentationOfImage:image toURL:[NSURL fileURLWithPath:_file]
            format:kCIFormatRGBA8 colorSpace:srgb options:@{} error:&error];
        CGColorSpaceRelease(srgb);
        if (!_saved) printf("PNG error domain=%s code=%ld\n", error.domain.UTF8String, (long)error.code);
        else {
            NSData *data = [NSData dataWithContentsOfFile:_file];
            if (!data.length) { _saved = NO; return; }
            unsigned char digest[CC_SHA256_DIGEST_LENGTH];
            CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
            printf("saved sequence=%lu sha256=", (unsigned long)_frames);
            for (unsigned i = 0; i < sizeof digest; ++i) printf("%02x", digest[i]);
            puts("");
        }
    }
}
- (void)captureOutput:(AVCaptureOutput *)output didDropSampleBuffer:(CMSampleBufferRef)sample
       fromConnection:(AVCaptureConnection *)connection
{
    ++_dropped;
}
@end

// Discovery-only diagnostics use this compiled AVFoundation client: JavaScript's
// framework import need not expose the Tundra class aliases on a native boot.
static void inventory(void)
{
    // QuickTime still queries the legacy type; keep that diagnostic comparison.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSArray *types = @[AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal,
                       AVCaptureDeviceTypeExternalUnknown];
#pragma clang diagnostic pop
    for (AVCaptureDeviceType type in types) {
        NSArray *devices = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:@[type]
                            mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified].devices;
        printf("discovery type=%s count=%lu\n", type.UTF8String, (unsigned long)devices.count);
        for (AVCaptureDevice *device in devices) {
            if (![device.uniqueID.uppercaseString hasPrefix:@"6A1F2A8E-4C2B-4E8F-9B57-50494E4F000"]) continue;
            printf("device name=%s type=%s model=%s connected=%d suspended=%d inUse=%d formats=%lu\n",
                   device.localizedName.UTF8String, device.deviceType.UTF8String, device.modelID.UTF8String,
                   device.connected, device.suspended, device.inUseByAnotherApplication, (unsigned long)device.formats.count);
            for (AVCaptureDeviceFormat *format in device.formats) {
                CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
                for (AVFrameRateRange *range in format.videoSupportedFrameRateRanges)
                    printf("inventory format %s %dx%d FourCC=%08x fps=%.2f..%.2f\n", device.localizedName.UTF8String,
                           size.width, size.height, CMFormatDescriptionGetMediaSubType(format.formatDescription),
                           range.minFrameRate, range.maxFrameRate);
            }
        }
    }
    puts("CAMERA_INVENTORY_COMPLETE");
}

static BOOL capture(AVCaptureDevice *device, int seconds, NSString *file)
{
    if (!device) return NO;
    AVCaptureDeviceFormat *native = nil;
    for (AVCaptureDeviceFormat *format in device.formats) {
        CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
        FourCharCode type = CMFormatDescriptionGetMediaSubType(format.formatDescription);
        for (AVFrameRateRange *range in format.videoSupportedFrameRateRanges) {
            printf("format %s %dx%d FourCC=%08x fps=%.2f..%.2f\n", device.localizedName.UTF8String,
                   size.width, size.height, type, range.minFrameRate, range.maxFrameRate);
            if (!native && type == kCVPixelFormatType_32BGRA && range.minFrameRate <= 15 && range.maxFrameRate >= 15) native = format;
        }
    }
    NSError *error = nil;
    AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
    AVCaptureSession *session = [AVCaptureSession new];
    AVCaptureVideoDataOutput *output = [AVCaptureVideoDataOutput new];
    output.alwaysDiscardsLateVideoFrames = YES;
    output.videoSettings = @{ (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA) };
    FrameSink *sink = [FrameSink new];
    sink.file = file;
    dispatch_queue_t queue = dispatch_queue_create("com.unixnu.PianoCameraCapture.frames", DISPATCH_QUEUE_SERIAL);
    [output setSampleBufferDelegate:sink queue:queue];
    if (!native || !input || ![session canAddInput:input] || ![session canAddOutput:output]) {
        printf("capture configuration failed for %s (%ld)\n", device.localizedName.UTF8String, (long)error.code);
        return NO;
    }
    [session beginConfiguration];
    [session addInput:input];
    [session addOutput:output];
    // The macOS SDK selects the device format through activeFormat.
    if (![device lockForConfiguration:&error]) {
        [session commitConfiguration];
        printf("format lock failed code=%ld\n", (long)error.code);
        return NO;
    }
    device.activeFormat = native;
    device.activeVideoMinFrameDuration = CMTimeMake(1, 15);
    device.activeVideoMaxFrameDuration = CMTimeMake(1, 15);
    [device unlockForConfiguration];
    [session commitConfiguration];
    CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(device.activeFormat.formatDescription);
    printf("capture begin %s %dx%d duration=%.9f\n", device.localizedName.UTF8String, size.width, size.height,
           CMTimeGetSeconds(device.activeVideoMinFrameDuration));
    sink.expectedWidth = size.width;
    sink.expectedHeight = size.height;
    [session startRunning];
    if (session.isRunning) {
        NSDate *until = [NSDate dateWithTimeIntervalSinceNow:10];
        __block BOOL received = NO;
        while (!received && [until timeIntervalSinceNow] > 0) {
            dispatch_sync(queue, ^{ received = sink.frames > 0; });
            if (!received) [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        }
        if (received) [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
    }
    [session stopRunning];
    [output setSampleBufferDelegate:nil queue:NULL];
    __block BOOL ok;
    dispatch_sync(queue, ^{
        ok = !sink.invalid && (file ? sink.frames >= 60 && sink.saved && sink.scene : sink.frames >= 12 && sink.moved);
        printf("capture end %s valid=%lu invalid=%lu dropped=%lu fps=%.2f moved=%d scene=%d png=%d result=%s\n",
               device.localizedName.UTF8String, (unsigned long)sink.frames, (unsigned long)sink.invalid,
               (unsigned long)sink.dropped, (double)sink.frames / seconds, sink.moved, sink.scene, sink.saved, ok ? "PASS" : "FAIL");
    });
    return ok;
}

int main(int argc, const char **argv)
{
    @autoreleasepool {
        if (argc != 2 || geteuid() == 0) return 2;
        if (!strcmp(argv[1], "--inventory")) { inventory(); return 0; }
        NSString *directory = [NSString stringWithUTF8String:argv[1]];
        NSString *log = [directory stringByAppendingPathComponent:@"capture.txt"];
        if (!freopen(log.fileSystemRepresentation, "w", stdout)) return 2;
        setvbuf(stdout, NULL, _IOLBF, 0);
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        printf("capture GUI client started uid=%u\n", geteuid());
        AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
        printf("Camera authorization initial=%ld\n", (long)status);
        if (status == AVAuthorizationStatusNotDetermined) {
            __block BOOL answered = NO, granted = NO;
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL value) {
                dispatch_async(dispatch_get_main_queue(), ^{ granted = value; answered = YES; });
            }];
            NSDate *until = [NSDate dateWithTimeIntervalSinceNow:100];
            while (!answered && [until timeIntervalSinceNow] > 0)
                [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.2]];
            printf("Camera authorization answered=%d granted=%d\n", answered, granted);
            if (!granted) { puts("CLIENT FAIL authorization"); return 1; }
        } else if (status != AVAuthorizationStatusAuthorized) {
            puts("CLIENT FAIL authorization");
            return 1;
        }
        NSArray *types = @[AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal];
        NSArray *devices = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                          mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified].devices;
        NSDictionary *names = @{PIANO_PATTERN_ID: PIANO_PATTERN_NAME, PIANO_FRONT_ID: PIANO_FRONT_NAME,
                                PIANO_REAR_ID: PIANO_REAR_NAME};
        NSMutableDictionary *piano = [NSMutableDictionary new];
        for (AVCaptureDevice *device in devices) {
            NSString *idKey = device.uniqueID.uppercaseString;
            if (![device.localizedName isEqualToString:names[idKey]]) continue;
            piano[idKey] = device;
            printf("discovered %s %s\n", device.localizedName.UTF8String, idKey.UTF8String);
        }
        // The synthetic control is optional when the kernel disables patterns.
        const BOOL hasPattern = piano[PIANO_PATTERN_ID] != nil;
        BOOL pattern = !hasPattern || capture(piano[PIANO_PATTERN_ID], 2, nil);
        BOOL front = capture(piano[PIANO_FRONT_ID], 5, [directory stringByAppendingPathComponent:@"front.png"]);
        BOOL rear = capture(piano[PIANO_REAR_ID], 5, [directory stringByAppendingPathComponent:@"rear.png"]);
        printf("CLIENT %s pattern=%s front=%d rear=%d\n", pattern && front && rear ? "PASS" : "FAIL",
               hasPattern ? (pattern ? "PASS" : "FAIL") : "DISABLED", front, rear);
        return pattern && front && rear ? 0 : 1;
    }
}
