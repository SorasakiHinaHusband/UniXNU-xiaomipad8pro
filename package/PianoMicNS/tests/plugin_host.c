// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// The built bundle as the HAL's driver service takes it (27.3 com.apple.audio.Core-Audio-Driver-Service.helper: SecStaticCodeCreateWithPath
// + SecStaticCodeCheckValidity, then CFBundleCreate, CFPlugInFindFactoriesForPlugInTypeInPlugIn(kAudioServerPlugInTypeUUID),
// CFPlugInInstanceCreate, QueryInterface for the driver interface, Initialize), then the questions the audio server asks a new
// device: the plug-in's device list and UID translation, the device's name, UID, transport, streams by scope, latency, rate, time
// stamp period, the stream's direction, terminal and format, an unknown property refused; StartIO without PianoAudio's tap service
// (this Mac has none) refused with kAudioHardwareNotRunningError. Run by tooling/k27/build-piano-micns.sh on the signed bundle.
//   plugin_host BUNDLE
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do {++checks; if (!(c)) {fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1);}} while (0)

static OSStatus hostChanged(AudioServerPlugInHostRef h, AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress *a)
{(void)h; (void)o; (void)n; (void)a; return 0;}
static OSStatus hostCopy(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef *d) {(void)h; (void)k; *d = NULL; return 0;}
static OSStatus hostWrite(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef d) {(void)h; (void)k; (void)d; return 0;}
static OSStatus hostDelete(AudioServerPlugInHostRef h, CFStringRef k) {(void)h; (void)k; return 0;}
static OSStatus hostConfig(AudioServerPlugInHostRef h, AudioObjectID o, UInt64 a, void *i) {(void)h; (void)o; (void)a; (void)i; return 0;}
static const AudioServerPlugInHostInterface kHost = {hostChanged, hostCopy, hostWrite, hostDelete, hostConfig};

static AudioServerPlugInDriverRef drv;
static OSStatus get(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope scope, void *out, UInt32 size, UInt32 *got,
                    const void *q, UInt32 qs)
{
    const AudioObjectPropertyAddress a = {s, scope, kAudioObjectPropertyElementMain};
    return (*drv)->GetPropertyData(drv, o, 0, &a, qs, q, size, got, out);
}
static UInt32 u32(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope scope)
{
    UInt32 v = 0xdeadbeef, got = 0;
    CHECK(get(o, s, scope, &v, sizeof v, &got, NULL, 0) == 0 && got == sizeof v);
    return v;
}
static int str_is(AudioObjectID o, AudioObjectPropertySelector s, const char *want)
{
    CFStringRef v = NULL;
    UInt32 got = 0;
    if (get(o, s, kAudioObjectPropertyScopeGlobal, &v, sizeof v, &got, NULL, 0) || !v) return 0;
    char buf[256];
    return CFStringGetCString(v, buf, sizeof buf, kCFStringEncodingUTF8) && !strcmp(buf, want);
}

int main(int argc, char **argv)
{
    if (argc != 2) {fprintf(stderr, "usage: plugin_host BUNDLE\n"); return 2;}
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)argv[1], (CFIndex)strlen(argv[1]), true);
    SecStaticCodeRef code = NULL;
    CHECK(SecStaticCodeCreateWithPath(url, kSecCSDefaultFlags, &code) == errSecSuccess);
    CHECK(SecStaticCodeCheckValidity(code, kSecCSDefaultFlags, NULL) == errSecSuccess);   // the driver service refuses an invalid one
    CFRelease(code);
    CFPlugInRef plugin = CFPlugInCreate(NULL, url);
    CHECK(plugin != NULL);
    CFBundleRef bundle = CFPlugInGetBundle(plugin);
    CFDictionaryRef info = CFBundleGetInfoDictionary(bundle);
    CFArrayRef clients = CFDictionaryGetValue(info, CFSTR("AudioServerPlugIn_IOKitUserClients"));
    CHECK(clients && CFArrayGetCount(clients) == 1 &&
          CFEqual(CFArrayGetValueAtIndex(clients, 0), CFSTR("PianoMicTapUserClient")));   // the sandbox extension's class
    CFArrayRef factories = CFPlugInFindFactoriesForPlugInTypeInPlugIn(kAudioServerPlugInTypeUUID, plugin);
    CHECK(factories && CFArrayGetCount(factories) == 1);
    CFUUIDRef factory = CFArrayGetValueAtIndex(factories, 0);
    void *iunknown = CFPlugInInstanceCreate(NULL, factory, kAudioServerPlugInTypeUUID);
    CHECK(iunknown != NULL);
    IUnknownVTbl **unk = iunknown;
    void *driver = NULL;
    CHECK((*unk)->QueryInterface(unk, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), &driver) == S_OK && driver);
    drv = driver;
    const AudioServerPlugInHostInterface *host = &kHost;
    CHECK((*drv)->Initialize(drv, (AudioServerPlugInHostRef)host) == 0);
    // the plug-in object
    AudioObjectID list[4] = {0};
    UInt32 got = 0;
    CHECK(get(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, list, sizeof list, &got, NULL, 0) == 0);
    CHECK(got == sizeof(AudioObjectID) && list[0] == 2);
    CFStringRef uid = CFSTR("PianoMicNS");
    AudioObjectID dev = 0;
    CHECK(get(kAudioObjectPlugInObject, kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, &dev, sizeof dev, &got,
              &uid, sizeof uid) == 0 && dev == 2);
    CHECK(u32(kAudioObjectPlugInObject, kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal) == kAudioPlugInClassID);
    // the device
    CHECK(str_is(2, kAudioObjectPropertyName, "Mic (noise reduced)") && str_is(2, kAudioDevicePropertyDeviceUID, "PianoMicNS"));
    CHECK(u32(2, kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal) == kAudioDeviceClassID);
    CHECK(u32(2, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal) == kAudioDeviceTransportTypeVirtual);
    CHECK(u32(2, kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioObjectPropertyScopeInput) == 1);
    CHECK(u32(2, kAudioDevicePropertyDeviceCanBeDefaultSystemDevice, kAudioObjectPropertyScopeGlobal) == 0);
    CHECK(u32(2, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput) == 3360);
    CHECK(u32(2, kAudioDevicePropertyZeroTimeStampPeriod, kAudioObjectPropertyScopeGlobal) == 16384);
    CHECK(u32(2, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal) == 0);
    Float64 rate = 0;
    CHECK(get(2, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &rate, sizeof rate, &got, NULL, 0) == 0 && rate == 48000.0);
    CHECK(get(2, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, list, sizeof list, &got, NULL, 0) == 0 &&
          got == sizeof(AudioObjectID) && list[0] == 3);
    CHECK(get(2, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, list, sizeof list, &got, NULL, 0) == 0 && got == 0);
    CHECK(get(2, kAudioObjectPropertyControlList, kAudioObjectPropertyScopeGlobal, list, sizeof list, &got, NULL, 0) == 0 && got == 0);
    const AudioObjectPropertyAddress bogus = {'zzzz', kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CHECK(!(*drv)->HasProperty(drv, 2, 0, &bogus));
    // the stream
    CHECK(u32(3, kAudioStreamPropertyDirection, kAudioObjectPropertyScopeGlobal) == 1);
    CHECK(u32(3, kAudioStreamPropertyTerminalType, kAudioObjectPropertyScopeGlobal) == kAudioStreamTerminalTypeMicrophone);
    AudioStreamBasicDescription f;
    CHECK(get(3, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, &f, sizeof f, &got, NULL, 0) == 0);
    CHECK(f.mSampleRate == 48000.0 && f.mChannelsPerFrame == 1 && f.mBitsPerChannel == 32 && (f.mFormatFlags & kAudioFormatFlagIsFloat));
    Boolean settable = false;
    const AudioObjectPropertyAddress fa = {kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CHECK((*drv)->IsPropertySettable(drv, 3, 0, &fa, &settable) == 0 && settable);
    f.mChannelsPerFrame = 2;
    CHECK((*drv)->SetPropertyData(drv, 3, 0, &fa, 0, NULL, sizeof f, &f) == kAudioDeviceUnsupportedFormatError);
    // IO: only ReadInput; StartIO needs PianoAudio's tap, which this Mac does not have
    Boolean will = false, inplace = false;
    CHECK((*drv)->WillDoIOOperation(drv, 2, 0, kAudioServerPlugInIOOperationReadInput, &will, &inplace) == 0 && will && inplace);
    CHECK((*drv)->WillDoIOOperation(drv, 2, 0, kAudioServerPlugInIOOperationWriteMix, &will, &inplace) == 0 && !will);
    CHECK((*drv)->StartIO(drv, 2, 1) == kAudioHardwareNotRunningError);
    CHECK(u32(2, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal) == 0);
    printf("PASS plugin: %u checks (signature valid, factory, interface, properties, StartIO without a tap refused)\n", checks);
    return 0;
}
