// SPDX-License-Identifier: MIT
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreAudio/CoreAudio.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../../package/include/PianoBootArgs.h"

enum { OK = 0, Error = 1, Usage = 2, NoPermission = 3, Silence = 4, ToneMissing = 5 };
static const double kMinInputDBFS = -85.0;
static const double kMinToneDBFS = -55.0;
static const double kMinToneSNR = 12.0;

@interface AudioCompletion : NSObject <AVAudioRecorderDelegate, AVAudioPlayerDelegate>
@property (atomic) BOOL finished;
@property (atomic) BOOL succeeded;
@end

@implementation AudioCompletion
- (void)audioRecorderDidFinishRecording:(AVAudioRecorder *)recorder successfully:(BOOL)success
{
    (void)recorder;
    self.succeeded = success;
    self.finished = YES;
}
- (void)audioRecorderEncodeErrorDidOccur:(AVAudioRecorder *)recorder error:(NSError *)error
{
    (void)recorder;
    fprintf(stderr, "encode: %s\n", error.localizedDescription.UTF8String ?: "unknown error");
    self.finished = YES;
}
- (void)audioPlayerDidFinishPlaying:(AVAudioPlayer *)player successfully:(BOOL)success
{
    (void)player;
    self.succeeded = success;
    self.finished = YES;
}
@end

static void pump(void)
{
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
}

static int result(int code)
{
    const char *names[] = {"ok", "error", "usage", "no_permission", "silence", "tone_missing"};
    printf("result=%s\n", names[code]);
    return code;
}

static AVAuthorizationStatus mic_status(void)
{
    return [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
}

static int report_tcc(void)
{
    AVAuthorizationStatus status = mic_status();
    const char *name = "unknown";
    switch (status) {
        case AVAuthorizationStatusNotDetermined: name = "not_determined"; break;
        case AVAuthorizationStatusRestricted: name = "restricted"; break;
        case AVAuthorizationStatusDenied: name = "denied"; break;
        case AVAuthorizationStatusAuthorized: name = "authorized"; break;
    }
    printf("mic_status=%ld\nmicrophone_authorization=%s\n", (long)status, name);
    return status == AVAuthorizationStatusAuthorized ? OK : NoPermission;
}

static AudioDeviceID default_device(AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address = {
        selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &device) != noErr)
        return kAudioObjectUnknown;
    return device;
}

static NSString *device_name(AudioDeviceID device)
{
    if (device == kAudioObjectUnknown)
        return nil;
    AudioObjectPropertyAddress address = {
        kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    CFStringRef name = NULL;
    UInt32 size = sizeof(name);
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &name) != noErr || !name)
        return nil;
    NSString *deviceName = CFBridgingRelease(name);
    return [[deviceName componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]
            componentsJoinedByString:@" "];
}

static BOOL report_input(AudioDeviceID device)
{
    NSString *deviceName = device_name(device);
    if (deviceName.length == 0)
        return NO;
    printf("input_device=%s\n", deviceName.UTF8String);

    // Report HAL input streams' client formats alongside the encoded file format.
    AudioObjectPropertyAddress address = {
        kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain
    };
    UInt32 size;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, NULL, &size) != noErr ||
        size < sizeof(AudioStreamID))
        return NO;
    NSMutableData *streams = [NSMutableData dataWithLength:size];
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, streams.mutableBytes) != noErr ||
        size < sizeof(AudioStreamID))
        return NO;
    const AudioStreamID *ids = streams.bytes;
    address.mSelector = kAudioStreamPropertyVirtualFormat;
    address.mScope = kAudioObjectPropertyScopeGlobal;
    for (size_t i = 0; i < size / sizeof(*ids); ++i) {
        AudioStreamBasicDescription format;
        UInt32 bytes = sizeof(format);
        if (AudioObjectGetPropertyData(ids[i], &address, 0, NULL, &bytes, &format) != noErr ||
            !isfinite(format.mSampleRate) || format.mSampleRate <= 0 || format.mChannelsPerFrame == 0)
            return NO;
        printf("input_format=%.0f Hz %u ch %u bit id=0x%08x flags=0x%x\n",
               format.mSampleRate, (unsigned)format.mChannelsPerFrame, (unsigned)format.mBitsPerChannel,
               (unsigned)format.mFormatID, (unsigned)format.mFormatFlags);
    }
    return YES;
}

static double power_db(double power)
{
    return 10.0 * log10(fmax(power, 1e-20));
}

typedef struct {
    int frequency;
    double snr;
    double power;
} Tone;

static Tone find_tone(const float *samples, size_t count, double rate, double mean, double acPower)
{
    Tone best = { .snr = -200.0 };
    for (int hz = 980; hz <= 1020; hz += 5) {
        double coefficient = 2.0 * cos(2.0 * M_PI * hz / rate);
        double previous = 0.0, older = 0.0;
        for (size_t i = 0; i < count; ++i) {
            double state = samples[i] - mean + coefficient * previous - older;
            older = previous;
            previous = state;
        }
        double power = 2.0 * (previous * previous + older * older - coefficient * previous * older) /
                       ((double)count * count);
        if (power > best.power) {
            best = (Tone){ hz, power_db(power) - power_db(acPower - power), power };
        }
    }
    return best;
}

static int analyze_file(NSURL *url, BOOL expectTone, long seconds)
{
    NSError *error = nil;
    AVAudioFile *file = [[AVAudioFile alloc] initForReading:url commonFormat:AVAudioPCMFormatFloat32
                                              interleaved:NO error:&error];
    if (!file) {
        fprintf(stderr, "decode: %s\n", error.localizedDescription.UTF8String ?: "unknown error");
        return Error;
    }
    AVAudioFormat *format = file.processingFormat;
    double rate = format.sampleRate;
    AVAudioFramePosition frames = file.length;
    if (format.channelCount != 1 || !isfinite(rate) || rate < 4000 || rate > 192000 || frames <= 0) {
        fprintf(stderr, "decode: expected non-empty mono PCM at 4–192 kHz\n");
        return Error;
    }
    double duration = frames / rate;
    printf("recording_format=%.0f Hz %u ch id=0x%08x\nduration_s=%.3f\n",
           file.fileFormat.sampleRate, (unsigned)file.fileFormat.channelCount,
           (unsigned)file.fileFormat.streamDescription->mFormatID, duration);
    if (seconds && fabs(duration - seconds) > 0.25) {
        fprintf(stderr, "recording duration differs from request by more than 0.25 s\n");
        return Error;
    }

    AVAudioFrameCount second = (AVAudioFrameCount)llround(rate);
    size_t window = second / 5;
    AVAudioPCMBuffer *buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:format frameCapacity:second];
    if (!buffer)
        return Error;
    BOOL hasInput = NO, detected = NO;
    unsigned consecutive = 0;
    Tone best = { .snr = -200.0 }, previous = best;
    // Best qualifying window, or the strongest candidate for diagnosis on a failed tone run.
    Tone candidate = best;
    for (AVAudioFramePosition offset = 0; offset < frames; offset += second) {
        AVAudioFrameCount count = (AVAudioFrameCount)MIN((AVAudioFramePosition)second, frames - offset);
        if (![file readIntoBuffer:buffer frameCount:count error:&error] || buffer.frameLength != count) {
            fprintf(stderr, "decode: incomplete PCM read%s%s\n", error ? ": " : "",
                    error.localizedDescription.UTF8String ?: "");
            return Error;
        }
        const float *samples = buffer.floatChannelData[0];
        double sumSquares = 0.0, peak = 0.0;
        for (size_t i = 0; i < count; ++i) {
            sumSquares += (double)samples[i] * samples[i];
            peak = fmax(peak, fabs(samples[i]));
        }
        printf("second=%lld rms_dbfs=%.2f peak_dbfs=%.2f\n", (long long)(offset / second),
               power_db(sumSquares / count), power_db(peak * peak));

        for (size_t start = 0; start + window <= count; start += window) {
            double sum = 0.0, squares = 0.0;
            for (size_t i = start; i < start + window; ++i) {
                sum += samples[i];
                squares += (double)samples[i] * samples[i];
            }
            double mean = sum / window, acPower = squares / window - mean * mean;
            hasInput |= power_db(acPower) >= kMinInputDBFS;
            if (!expectTone)
                continue;
            Tone tone = find_tone(samples + start, window, rate, mean, acPower);
            if (tone.power > candidate.power)
                candidate = tone;
            if (power_db(tone.power) >= kMinToneDBFS && tone.snr >= kMinToneSNR) {
                if (++consecutive >= 2) {
                    detected = YES;
                    Tone stronger = tone.snr >= previous.snr ? tone : previous;
                    if (stronger.snr > best.snr)
                        best = stronger;
                }
                previous = tone;
            } else {
                consecutive = 0;
            }
        }
    }

    if (expectTone) {
        Tone tone = detected ? best : candidate;
        printf("tone_detected=%s\n", detected ? "yes" : "no");
        if (tone.frequency)
            printf("%s=%d tone_snr_db=%.2f tone_dbfs=%.2f\n",
                   detected ? "tone_frequency_hz" : "tone_candidate_hz",
                   tone.frequency, tone.snr, power_db(tone.power));
    }
    if (!hasInput)
        return Silence;
    return expectTone && !detected ? ToneMissing : OK;
}

static int analyze(NSURL *url, BOOL expectTone, long seconds)
{
    @try {
        return analyze_file(url, expectTone, seconds);
    } @catch (NSException *exception) {
        fprintf(stderr, "decode: %s\n", exception.reason.UTF8String ?: "audio framework exception");
        return Error;
    }
}

static BOOL play_recording(NSURL *url)
{
    NSError *error = nil;
    AVAudioPlayer *player = [[AVAudioPlayer alloc] initWithContentsOfURL:url error:&error];
    AudioCompletion *completion = [AudioCompletion new];
    player.delegate = completion;
    if (![player play]) {
        fprintf(stderr, "playback: %s\n", error.localizedDescription.UTF8String ?: "could not start player");
        return NO;
    }
    double deadline = NSProcessInfo.processInfo.systemUptime + player.duration + 5;
    while (!completion.finished && NSProcessInfo.processInfo.systemUptime < deadline)
        pump();
    [player stop];
    printf("playback_complete=%s\n", completion.succeeded ? "yes" : "no");
    return completion.succeeded;
}

static BOOL unattended_enabled(void)
{
    char args[PIANO_BOOT_ARGS_MAX];
    pianoBootArgs(args);
    unsigned count = 0, enabled = 0;
    char *state = NULL;
    for (char *word = strtok_r(args, " \t", &state);
         word; word = strtok_r(NULL, " \t", &state)) {
        if (strcmp(word, "piano-mic-unattended") == 0 ||
            strncmp(word, "piano-mic-unattended=", sizeof("piano-mic-unattended=") - 1) == 0)
            ++count;
        if (strcmp(word, "piano-mic-unattended=1") == 0)
            ++enabled;
    }
    return count == 1 && enabled == 1;
}

static int replay(NSURL *url)
{
    if (!unattended_enabled()) {
        fprintf(stderr, "replay: piano-mic-unattended=1 required\n");
        return Error;
    }
    AudioDeviceID output = default_device(kAudioHardwarePropertyDefaultOutputDevice);
    NSString *name = device_name(output);
    printf("output_device=%s\n", name.UTF8String ?: "unknown");
    if (![name isEqualToString:@"Speaker"]) {
        fprintf(stderr, "replay: select the tablet Speaker as the default output\n");
        return Error;
    }
    if (!play_recording(url))
        return Error;
    if (default_device(kAudioHardwarePropertyDefaultOutputDevice) != output) {
        fprintf(stderr, "replay: default output changed during playback\n");
        return Error;
    }
    return OK;
}

static int record_to(NSURL *url, long seconds, BOOL tone)
{
    NSDictionary *settings = @{
        AVFormatIDKey: @(kAudioFormatLinearPCM), AVSampleRateKey: @48000.0,
        AVNumberOfChannelsKey: @1, AVLinearPCMBitDepthKey: @16,
        AVLinearPCMIsFloatKey: @NO, AVLinearPCMIsBigEndianKey: @NO
    };
    NSError *error = nil;
    AVAudioRecorder *recorder = [[AVAudioRecorder alloc] initWithURL:url settings:settings error:&error];
    if (!recorder || ![recorder prepareToRecord]) {
        fprintf(stderr, "record: %s\n", error.localizedDescription.UTF8String ?: "prepareToRecord failed");
        return Error;
    }

    AVAudioPlayer *player = nil;
    AudioCompletion *played = nil;
    if (tone) {
        NSURL *stimulus = [NSBundle.mainBundle URLForResource:@"mic-check-tone" withExtension:@"wav"];
        if (stimulus)
            player = [[AVAudioPlayer alloc] initWithContentsOfURL:stimulus error:&error];
        if (!player || ![player prepareToPlay]) {
            fprintf(stderr, "stimulus: %s\n", error.localizedDescription.UTF8String ?: "bundled WAV unavailable");
            return Error;
        }
        played = [AudioCompletion new];
        player.delegate = played;
    }

    AudioCompletion *recorded = [AudioCompletion new];
    recorder.delegate = recorded;
    if (![recorder recordForDuration:seconds]) {
        fprintf(stderr, "record: recordForDuration failed\n");
        return Error;
    }
    if (tone && ![player play]) {
        [recorder stop];
        fprintf(stderr, "stimulus: could not start player\n");
        return Error;
    }
    double deadline = NSProcessInfo.processInfo.systemUptime + seconds + 5;
    while (!recorded.finished && NSProcessInfo.processInfo.systemUptime < deadline)
        pump();
    [recorder stop];
    [player stop];
    if (!recorded.succeeded) {
        fprintf(stderr, "record: recorder failed or timed out\n");
        return Error;
    }
    if (tone) {
        printf("stimulus_complete=%s\n", played.succeeded ? "yes" : "no");
        if (!played.succeeded)
            return Error;
    }
    return OK;
}

static int capture(NSURL *output, long seconds, BOOL tone, BOOL playback)
{
    if (report_tcc() != OK)
        return NoPermission;
    AudioDeviceID device = default_device(kAudioHardwarePropertyDefaultInputDevice);
    if (!report_input(device)) {
        fprintf(stderr, "record: no named default input with a readable stream format\n");
        return Error;
    }
    AudioDeviceID speaker = kAudioObjectUnknown;
    if (tone || playback) {
        speaker = default_device(kAudioHardwarePropertyDefaultOutputDevice);
        NSString *name = device_name(speaker);
        if (name.length == 0) {
            fprintf(stderr, "record: no named default output\n");
            return Error;
        }
        printf("output_device=%s\n", name.UTF8String);
    }

    // The recorder owns a fresh path; publish the finalized file without replacing any prior result.
    struct stat existing;
    if (lstat(output.fileSystemRepresentation, &existing) == 0 || errno != ENOENT) {
        fprintf(stderr, "record: output must be a new path\n");
        return Error;
    }
    NSURL *templateURL = [output.URLByDeletingLastPathComponent URLByAppendingPathComponent:@".mic-check.XXXXXXXX"];
    char *scratch = strdup(templateURL.fileSystemRepresentation);
    if (!scratch || !mkdtemp(scratch)) {
        perror("record: mkdtemp");
        free(scratch);
        return Error;
    }
    NSURL *folder = [NSURL fileURLWithFileSystemRepresentation:scratch isDirectory:YES relativeToURL:nil];
    NSURL *temp = [folder URLByAppendingPathComponent:@"recording.m4a"];
    int status = record_to(temp, seconds, tone);
    if (status == OK) {
        if (link(temp.fileSystemRepresentation, output.fileSystemRepresentation) != 0) {
            perror("record: publish");
            status = Error;
        } else {
            printf("recording_published=yes\n");
            status = analyze(output, tone, seconds);
            if (default_device(kAudioHardwarePropertyDefaultInputDevice) != device) {
                fprintf(stderr, "record: default input changed during capture\n");
                status = Error;
            }
            if (speaker != kAudioObjectUnknown && default_device(kAudioHardwarePropertyDefaultOutputDevice) != speaker) {
                fprintf(stderr, "record: default output changed during capture\n");
                status = Error;
            }
            if (playback && status == OK && !play_recording(output))
                status = Error;
        }
    }
    unlink(temp.fileSystemRepresentation);
    rmdir(scratch);
    free(scratch);
    return status;
}

static int run(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--tcc") == 0)
        return report_tcc();
    if (argc == 2 && strcmp(argv[1], "--request-access") == 0) {
        if (mic_status() == AVAuthorizationStatusNotDetermined) {
            [NSApplication sharedApplication];
            dispatch_semaphore_t answered = dispatch_semaphore_create(0);
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
                (void)granted;
                dispatch_semaphore_signal(answered);
            }];
            double deadline = NSProcessInfo.processInfo.systemUptime + 60;
            while (dispatch_semaphore_wait(answered, DISPATCH_TIME_NOW) != 0 &&
                   NSProcessInfo.processInfo.systemUptime < deadline)
                pump();
        }
        return report_tcc();
    }
    if (argc >= 3 && argc <= 4 && strcmp(argv[1], "--analyze") == 0 &&
        (argc == 3 || strcmp(argv[3], "--tone") == 0)) {
        NSString *path = [NSString stringWithUTF8String:argv[2]];
        if (path.length)
            return analyze([NSURL fileURLWithPath:path], argc == 4, 0);
    }
    if (argc == 3 && strcmp(argv[1], "--replay") == 0) {
        NSString *path = [NSString stringWithUTF8String:argv[2]];
        if ([path.pathExtension.lowercaseString isEqualToString:@"m4a"])
            return replay([NSURL fileURLWithPath:path]);
    }
    if (argc >= 3 && argc <= 5) {
        char *end = NULL;
        long seconds = strtol(argv[1], &end, 10);
        NSString *path = [NSString stringWithUTF8String:argv[2]];
        if (end != argv[1] && *end == '\0' && seconds >= 1 && seconds <= 300 &&
            [path.pathExtension.lowercaseString isEqualToString:@"m4a"]) {
            BOOL tone = NO, playback = NO, valid = YES;
            for (int i = 3; i < argc; ++i) {
                if (!tone && strcmp(argv[i], "--tone") == 0)
                    tone = YES;
                else if (!playback && strcmp(argv[i], "--playback") == 0)
                    playback = YES;
                else
                    valid = NO;
            }
            if (valid && (!tone || seconds >= 3))
                return capture([NSURL fileURLWithPath:path], seconds, tone, playback);
        }
    }
    fprintf(stderr, "usage: piano-mic-check --tcc | --request-access | --analyze FILE [--tone]\n"
                    "       piano-mic-check --replay FILE.m4a (piano-mic-unattended=1)\n"
                    "       piano-mic-check SECONDS OUTPUT.m4a [--tone] [--playback]\n"
                    "       SECONDS is 1–300 (at least 3 with --tone)\n");
    return argc == 2 && strcmp(argv[1], "--help") == 0 ? OK : Usage;
}

int main(int argc, char **argv)
{
    umask(077);
    setvbuf(stdout, NULL, _IOLBF, 0);
    @autoreleasepool {
        return result(run(argc, argv));
    }
}
