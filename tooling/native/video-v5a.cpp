// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Bounded native VideoToolbox qualification: one caller-supplied Baseline AVC AU.
// --self-test does not touch VT's once-only decoder registry. --qualify requires
// the native V5a endpoint before this fresh process makes its first VT query.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <IOSurface/IOSurface.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "VideoFixture.h"

namespace {
using pianovideo::session::kAccessUnit;
using pianovideo::session::kGolden;
constexpr char kSampleSha[] = "07f70d032e843a48ce2560d7b8e886b41e83ba6b2c325393f3d97ed585238ec4";
constexpr char kNormalizedSha[] = "b3c7aab54e349a6e93254403bece100a3a219d32c7718adbe68302307b9c285b";
constexpr int64_t kPtsValue = 6001;
constexpr int32_t kPtsScale = 60000;
int failures;

bool result(const char *stage, bool pass, OSStatus status = 0) {
    std::printf("video-v5a: %s %s status=%d\n", pass ? "PASS" : "FAIL", stage, int(status));
    if (!pass) ++failures;
    return pass;
}
void timeout(int) {
    const char message[] = "video-v5a: FAIL process-timeout status=124\n";
    (void)!write(STDOUT_FILENO, message, sizeof(message) - 1);
    _exit(124);
}
void hexDigest(const uint8_t *digest, char out[65]) {
    for (unsigned i = 0; i < 32; ++i) std::snprintf(out + 2 * i, 3, "%02x", digest[i]);
    out[64] = 0;
}
void sha(const void *bytes, size_t length, char out[65]) {
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes, CC_LONG(length), digest);
    hexDigest(digest, out);
}

struct Nal { const uint8_t *bytes; size_t length; };
bool splitFixture(std::vector<Nal> &out) {
    const auto *bytes = kAccessUnit;
    const size_t length = sizeof kAccessUnit;
    auto start = [&](size_t i) -> size_t {
        if (i + 4 <= length && bytes[i] == 0 && bytes[i+1] == 0 && bytes[i+2] == 0 && bytes[i+3] == 1) return 4;
        if (i + 3 <= length && bytes[i] == 0 && bytes[i+1] == 0 && bytes[i+2] == 1) return 3;
        return 0;
    };
    size_t i = 0;
    while (i < length) {
        size_t prefix = start(i);
        if (!prefix) return false;
        size_t begin = i + prefix, end = begin;
        while (end < length && !start(end)) ++end;
        if (end == begin) return false;
        out.push_back({bytes + begin, end - begin});
        i = end;
    }
    return out.size() == 3 && (out[0].bytes[0] & 31) == 7 && (out[1].bytes[0] & 31) == 8 &&
        (out[2].bytes[0] & 31) == 5 && out[0].length == 22 && out[1].length == 5 && out[2].length == 248;
}
std::vector<uint8_t> sampleBytes(const Nal &nal) {
    std::vector<uint8_t> bytes(4 + nal.length);
    bytes[0] = uint8_t(nal.length >> 24); bytes[1] = uint8_t(nal.length >> 16);
    bytes[2] = uint8_t(nal.length >> 8); bytes[3] = uint8_t(nal.length);
    std::memcpy(bytes.data() + 4, nal.bytes, nal.length);
    return bytes;
}
bool fixtureTest(std::vector<Nal> &nals, std::vector<uint8_t> &sample) {
    if (!result("fixture-nals", splitFixture(nals))) return false;
    sample = sampleBytes(nals[2]);
    char digest[65];
    sha(sample.data(), sample.size(), digest);
    std::printf("video-v5a: sample bytes=%zu sha256=%s\n", sample.size(), digest);
    bool ok = result("sample-sha256", sample.size() == 252 && std::strcmp(digest, kSampleSha) == 0);
    std::vector<uint8_t> normalized;
    for (const auto &nal : nals) {
        normalized.insert(normalized.end(), {0, 0, 0, 1});
        normalized.insert(normalized.end(), nal.bytes, nal.bytes + nal.length);
    }
    sha(normalized.data(), normalized.size(), digest);
    std::printf("video-v5a: normalized bytes=%zu sha256=%s\n", normalized.size(), digest);
    return result("normalized-sha256", normalized.size() == 287 && std::strcmp(digest, kNormalizedSha) == 0) && ok;
}

// One digest implementation handles the real callback and a host check whose
// padding deliberately differs from the active picture.
bool activeDigest(const uint8_t *y, size_t yStride, const uint8_t *uv, size_t uvStride, char digest[65]) {
    if (!y || !uv || yStride < 320 || uvStride < 320) return false;
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    for (size_t row = 0; row < 240; ++row) CC_SHA256_Update(&ctx, y + row * yStride, 320);
    for (size_t row = 0; row < 120; ++row) CC_SHA256_Update(&ctx, uv + row * uvStride, 320);
    uint8_t hash[32];
    CC_SHA256_Final(hash, &ctx);
    hexDigest(hash, digest);
    return true;
}
int selfTest() {
    std::vector<Nal> nals;
    std::vector<uint8_t> sample;
    fixtureTest(nals, sample);
    std::vector<uint8_t> y(384 * 256, 0xa5), uv(448 * 128, 0x5a);
    for (unsigned r = 0; r < 240; ++r) std::memset(y.data() + r * 384, 64, 320);
    for (unsigned r = 0; r < 120; ++r)
        for (unsigned c = 0; c < 320; c += 2) { uv[r * 448 + c] = 96; uv[r * 448 + c + 1] = 160; }
    char digest[65];
    result("active-rows-with-padding", activeDigest(y.data(), 384, uv.data(), 448, digest) && std::strcmp(digest, kGolden) == 0);
    result("short-plane-rejected", !activeDigest(y.data(), 319, uv.data(), 448, digest));
    std::printf("video-v5a: %s self-test failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

struct Frame {
    uint64_t token;
    bool reject;
    const char *stage;
    unsigned callbacks = 0, success = 0, errors = 0;
};
struct Context {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    unsigned success = 0, errors = 0;
    bool golden = false;
};
void output(void *refcon, void *source, OSStatus status, VTDecodeInfoFlags flags,
            CVImageBufferRef image, CMTime pts, CMTime duration) {
    auto *ctx = static_cast<Context *>(refcon);
    auto *frame = static_cast<Frame *>(source);
    pthread_mutex_lock(&ctx->mutex);
    const uint64_t token = frame ? frame->token : 0;
    std::printf("video-v5a: callback stage=%s token=%llu ref=%p status=%d flags=%u pts=%lld/%d duration=%lld/%d image=%u\n",
        frame ? frame->stage : "unknown", (unsigned long long)token, source, int(status), unsigned(flags),
        (long long)pts.value, int(pts.timescale), (long long)duration.value, int(duration.timescale), image != nullptr);
    if (frame) ++frame->callbacks;
    if (status || !image) {
        ++ctx->errors;
        if (frame) ++frame->errors;
        if (!frame || !frame->reject) result("accepted-frame-callback", false, status ? status : -1);
    } else {
        ++ctx->success;
        if (frame) ++frame->success;
        bool layout = frame && !frame->reject && CMTimeCompare(pts, CMTimeMake(kPtsValue, kPtsScale)) == 0 &&
            CVPixelBufferGetPixelFormatType(image) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
            CVPixelBufferGetWidth(image) == 320 && CVPixelBufferGetHeight(image) == 240 &&
            CVPixelBufferIsPlanar(image) && CVPixelBufferGetPlaneCount(image) == 2 && CVPixelBufferGetIOSurface(image);
        if (layout) layout = CVPixelBufferGetWidthOfPlane(image, 0) == 320 && CVPixelBufferGetHeightOfPlane(image, 0) == 240 &&
            CVPixelBufferGetWidthOfPlane(image, 1) == 160 && CVPixelBufferGetHeightOfPlane(image, 1) == 120;
        std::printf("video-v5a: buffer format=%08x width=%zu height=%zu planes=%zu surface=%u\n",
            unsigned(CVPixelBufferGetPixelFormatType(image)), CVPixelBufferGetWidth(image), CVPixelBufferGetHeight(image),
            CVPixelBufferGetPlaneCount(image), CVPixelBufferGetIOSurface(image) ? IOSurfaceGetID(CVPixelBufferGetIOSurface(image)) : 0);
        result("iosurface-420v-active-layout", layout);
        CVReturn lock = CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
        result("pixelbuffer-lock", lock == kCVReturnSuccess, lock);
        char digest[65] = "unavailable";
        bool valid = false;
        if (layout && lock == kCVReturnSuccess) {
            valid = activeDigest(static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(image, 0)),
                CVPixelBufferGetBytesPerRowOfPlane(image, 0), static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(image, 1)),
                CVPixelBufferGetBytesPerRowOfPlane(image, 1), digest);
            std::printf("video-v5a: active bytes=115200 y-stride=%zu uv-stride=%zu sha256=%s token=%llu\n",
                CVPixelBufferGetBytesPerRowOfPlane(image, 0), CVPixelBufferGetBytesPerRowOfPlane(image, 1), digest, (unsigned long long)token);
        }
        if (lock == kCVReturnSuccess) result("pixelbuffer-unlock", CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess);
        ctx->golden = valid && std::strcmp(digest, kGolden) == 0;
        result("active-golden-sha256", ctx->golden);
    }
    pthread_cond_broadcast(&ctx->condition);
    pthread_mutex_unlock(&ctx->mutex);
}
CFStringRef privateKey(const char *symbol) {
    void *address = dlsym(RTLD_DEFAULT, symbol);
    return address ? *static_cast<const CFStringRef *>(address) : nullptr;
}
CMVideoFormatDescriptionRef avcFormat(const std::vector<Nal> &nals) {
    const uint8_t *sets[2] = {nals[0].bytes, nals[1].bytes};
    size_t lengths[2] = {nals[0].length, nals[1].length};
    CMVideoFormatDescriptionRef format = nullptr;
    OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, sets, lengths, 4, &format);
    result("format-avc", status == 0 && format, status);
    return format;
}
CMSampleBufferRef makeSample(CMVideoFormatDescriptionRef format, const std::vector<uint8_t> &bytes) {
    CMBlockBufferRef block = nullptr;
    CMSampleBufferRef sample = nullptr;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, bytes.size(), kCFAllocatorDefault,
        nullptr, 0, bytes.size(), 0, &block);
    if (!status) status = CMBlockBufferReplaceDataBytes(bytes.data(), block, 0, bytes.size());
    const size_t size = bytes.size();
    CMSampleTimingInfo timing = {CMTimeMake(1, 30), CMTimeMake(kPtsValue, kPtsScale), kCMTimeInvalid};
    if (!status) status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, format, 1, 1, &timing, 1, &size, &sample);
    if (block) CFRelease(block);
    if (status) result("make-sample", false, status);
    return sample;
}
VTDecompressionSessionRef createSession(CMVideoFormatDescriptionRef format, CFDictionaryRef specification, Context &ctx, const char *stage, bool wanted) {
    int32_t pixel = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixel);
    CFDictionaryRef surface = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *keys[] = {kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferIOSurfacePropertiesKey};
    const void *values[] = {number, surface};
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    VTDecompressionOutputCallbackRecord callback = {output, &ctx};
    VTDecompressionSessionRef session = nullptr;
    OSStatus status = VTDecompressionSessionCreate(kCFAllocatorDefault, format, specification, attrs, &callback, &session);
    result(stage, wanted ? status == 0 && session : status != 0 && !session, status);
    CFRelease(attrs); CFRelease(surface); CFRelease(number);
    return session;
}
void invalidate(VTDecompressionSessionRef &session, const char *stage) {
    if (!session) return;
    uint64_t begin = mach_absolute_time();
    VTDecompressionSessionInvalidate(session);
    CFRelease(session);
    session = nullptr;
    std::printf("video-v5a: invalidate stage=%s elapsed-ticks=%llu\n", stage, (unsigned long long)(mach_absolute_time() - begin));
    result(stage, true);
}
void rejectSample(VTDecompressionSessionRef session, CMVideoFormatDescriptionRef format, const std::vector<uint8_t> &bytes, Frame &frame) {
    CMSampleBufferRef sample = makeSample(format, bytes);
    if (!sample) return;
    VTDecodeInfoFlags info = 0;
    OSStatus status = VTDecompressionSessionDecodeFrame(session, sample, kVTDecodeFrame_EnableAsynchronousDecompression, &frame, &info);
    std::printf("video-v5a: submit stage=%s bytes=%zu token=%llu status=%d info=%u\n", frame.stage, bytes.size(),
        (unsigned long long)frame.token, int(status), unsigned(info));
    // A public VT implementation may report a codec rejection synchronously or
    // in its asynchronous callback. Either way, no successful picture is allowed.
    OSStatus wait = VTDecompressionSessionWaitForAsynchronousFrames(session);
    result(frame.stage, frame.success == 0 && (status != 0 || frame.errors != 0), status ? status : wait);
    CFRelease(sample);
}
int qualify() {
    std::printf("video-v5a: begin pid=%d stamp=%llu hardware-required=1 pv-required=1\n", getpid(), (unsigned long long)mach_absolute_time());
    std::vector<Nal> nals;
    std::vector<uint8_t> bytes;
    if (!fixtureTest(nals, bytes)) return 1;
    CFStringRef requirePV = privateKey("kVTVideoDecoderSpecification_RequireParavirtualizedDecoder");
    CFStringRef allowPV = privateKey("kVTVideoDecoderSpecification_AllowParavirtualizedDecoder");
    if (!result("private-pv-keys", requirePV && allowPV)) return 1;
    const void *keys[] = {kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder, requirePV, allowPV};
    const void *values[] = {kCFBooleanTrue, kCFBooleanTrue, kCFBooleanTrue};
    CFDictionaryRef specification = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 3,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CMVideoFormatDescriptionRef format = avcFormat(nals);
    if (!format) { CFRelease(specification); return 1; }
    Context ctx;
    VTDecompressionSessionRef before = createSession(format, specification, ctx, "create-before-input", true);
    invalidate(before, "invalidate-before-input");
    result("no-callback-before-input", ctx.success == 0 && ctx.errors == 0);
    VTDecompressionSessionRef session = createSession(format, specification, ctx, "create-hardware-pv", true);
    if (!session) { CFRelease(format); CFRelease(specification); return 1; }
    CFTypeRef hardware = nullptr;
    OSStatus status = VTSessionCopyProperty(session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
        kCFAllocatorDefault, &hardware);
    result("using-hardware", !status && hardware && CFEqual(hardware, kCFBooleanTrue), status);
    if (hardware) CFRelease(hardware);
    result("dcan-same", VTDecompressionSessionCanAcceptFormatDescription(session, format));
    CMVideoFormatDescriptionRef changed = nullptr, hevc = nullptr;
    status = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_H264, 640, 240,
        CMFormatDescriptionGetExtensions(format), &changed);
    result("make-changed-format", !status && changed, status);
    if (changed) result("dcan-changed-rejected", !VTDecompressionSessionCanAcceptFormatDescription(session, changed));
    status = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_HEVC, 320, 240, nullptr, &hevc);
    result("make-hevc-format", !status && hevc, status);
    if (hevc) result("dcan-hevc-rejected", !VTDecompressionSessionCanAcceptFormatDescription(session, hevc));
    VTDecompressionSessionRef second = createSession(format, specification, ctx, "second-session-rejected", false);
    invalidate(second, "invalidate-unexpected-second");
    uint64_t token = (mach_absolute_time() ^ (uint64_t(getpid()) << 32)) | 1;
    Frame malformed{token + 1, true, "malformed-rejected"};
    rejectSample(session, format, {0, 0, 2, 0, 0x65, 0x88}, malformed);
    std::vector<uint8_t> large(2 * 1024 * 1024, 0x88);
    const uint32_t length = uint32_t(large.size() - 4);
    large[0] = uint8_t(length >> 24); large[1] = uint8_t(length >> 16); large[2] = uint8_t(length >> 8); large[3] = uint8_t(length); large[4] = 0x65;
    Frame oversized{token + 2, true, "oversized-rejected"};
    rejectSample(session, format, large, oversized);
    Frame accepted{token, false, "accepted-au"};
    CMSampleBufferRef sample = makeSample(format, bytes);
    if (sample) {
        VTDecodeInfoFlags info = 0;
        status = VTDecompressionSessionDecodeFrame(session, sample, kVTDecodeFrame_EnableAsynchronousDecompression, &accepted, &info);
        std::printf("video-v5a: submit stage=accepted-au bytes=%zu token=%llu ref=%p pts=%lld/%d status=%d info=%u\n",
            bytes.size(), (unsigned long long)token, static_cast<void *>(&accepted), (long long)kPtsValue, int(kPtsScale), int(status), unsigned(info));
        result("decode-submit", !status, status);
        status = VTDecompressionSessionWaitForAsynchronousFrames(session);
        result("wait-accepted-frame", !status, status);
        result("exactly-one-golden-callback", accepted.callbacks == 1 && accepted.success == 1 && accepted.errors == 0 && ctx.success == 1 && ctx.golden);
        Frame repeated{token + 3, true, "second-au-rejected"};
        rejectSample(session, format, bytes, repeated);
        status = VTDecompressionSessionFinishDelayedFrames(session);
        result("finish-delayed", !status, status);
        status = VTDecompressionSessionWaitForAsynchronousFrames(session);
        result("wait-after-finish", !status, status);
        CFRelease(sample);
    }
    invalidate(session, "invalidate-after-output");
    result("no-extra-success-callback", ctx.success == 1);
    if (hevc) CFRelease(hevc);
    if (changed) CFRelease(changed);
    CFRelease(format); CFRelease(specification);
    pthread_cond_destroy(&ctx.condition); pthread_mutex_destroy(&ctx.mutex);
    std::printf("video-v5a: %s qualification failures=%d success-callbacks=%u error-callbacks=%u\n", failures ? "FAIL" : "PASS", failures, ctx.success, ctx.errors);
    return failures ? 1 : 0;
}
}
int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    signal(SIGALRM, timeout);
    alarm(90);
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) return selfTest();
    if (argc == 1 || (argc == 2 && std::strcmp(argv[1], "--qualify") == 0)) return qualify();
    std::fprintf(stderr, "usage: video-v5a [--qualify|--self-test]\n");
    return 2;
}
