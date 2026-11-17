/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Copyright (c) 2026 Leo Zhang.
 * Read-only CoreAudio output/control and timing snapshot. */
#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioHardwareService.h>
#include <stdio.h>
#include <stdlib.h>

static OSStatus get(AudioObjectID id, AudioObjectPropertySelector key, AudioObjectPropertyScope scope,
                    AudioObjectPropertyElement element, UInt32 *size, void *out)
{
    const AudioObjectPropertyAddress a = {key, scope, element};
    return AudioObjectGetPropertyData(id, &a, 0, NULL, size, out);
}
static OSStatus u32(AudioObjectID id, AudioObjectPropertySelector key, UInt32 *out)
{
    UInt32 n = sizeof *out;
    return get(id, key, kAudioObjectPropertyScopeGlobal, 0, &n, out);
}
static void fourcc(UInt32 v, char out[5])
{
    for (int i = 0; i < 4; ++i) {
        const unsigned c = (v >> (24 - 8*i)) & 255;
        out[i] = c >= 32 && c <= 126 ? (char)c : '.';
    }
    out[4] = 0;
}
int main(int argc, char **argv)
{
    const char *phase = argc > 1 ? argv[1] : "snapshot";
    AudioDeviceID dev = 0;
    const OSStatus ds = u32(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, &dev);
    printf("default_output phase=%s status=%d device=%u\n", phase, (int)ds, (unsigned)dev);
    if (ds || !dev) return 1;
    const AudioObjectPropertyAddress a = {kAudioObjectPropertyControlList, kAudioObjectPropertyScopeGlobal, 0};
    UInt32 bytes = 0;
    OSStatus st = AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &bytes);
    AudioObjectID *ids = bytes ? calloc(1, bytes) : NULL;
    if (!st && bytes && !ids) return 2;
    if (!st && bytes) st = AudioObjectGetPropertyData(dev, &a, 0, NULL, &bytes, ids);
    const UInt32 count = st ? 0 : bytes / sizeof *ids;
    printf("control_list_read phase=%s status=%d count=%u\n", phase, (int)st, (unsigned)count);
    for (UInt32 i = 0; i < count; ++i) {
        UInt32 cls = 0, base = 0, scope = 0, element = 0;
        const OSStatus cs = u32(ids[i], kAudioObjectPropertyClass, &cls);
        const OSStatus bs = u32(ids[i], kAudioObjectPropertyBaseClass, &base);
        const OSStatus ss = u32(ids[i], kAudioControlPropertyScope, &scope);
        const OSStatus es = u32(ids[i], kAudioControlPropertyElement, &element);
        Float32 scalar = 0, db = 0;
        UInt32 n = sizeof scalar;
        const OSStatus vs = get(ids[i], kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, 0, &n, &scalar);
        n = sizeof db;
        const OSStatus dbs = get(ids[i], kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, 0, &n, &db);
        char c[5], b[5], sc[5]; fourcc(cls, c); fourcc(base, b); fourcc(scope, sc);
        printf("control phase=%s id=%u class=0x%08x(%s) base=0x%08x(%s) scope_status=%d scope=0x%08x(%s) element_status=%d element=%u class_status=%d base_status=%d scalar_status=%d scalar=%.9g db_status=%d db=%.9g\n",
               phase, (unsigned)ids[i], (unsigned)cls, c, (unsigned)base, b, (int)ss, (unsigned)scope, sc,
               (int)es, (unsigned)element, (int)cs, (int)bs, (int)vs, (double)scalar, (int)dbs, (double)db);
    }
    free(ids);
    for (UInt32 element = 0; element < 3; ++element) {
        Float32 v = 0; UInt32 n = sizeof v;
        st = get(dev, kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput, element, &n, &v);
        printf("device_volume phase=%s element=%u status=%d size=%u scalar=%.9g\n", phase, (unsigned)element, (int)st, (unsigned)n, (double)v);
    }
    Float32 vm = 0; UInt32 n = sizeof vm;
    const AudioObjectPropertyAddress va = {kAudioHardwareServiceDeviceProperty_VirtualMainVolume, kAudioObjectPropertyScopeOutput, 0};
    st = AudioHardwareServiceGetPropertyData(dev, &va, 0, NULL, &n, &vm);
    printf("virtual_main phase=%s status=%d size=%u scalar=%.9g\n", phase, (int)st, (unsigned)n, (double)vm);
    UInt32 cycle = 0, safety = 0; n = sizeof cycle;
    const OSStatus fs = get(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, 0, &n, &cycle);
    n = sizeof safety;
    const OSStatus so = get(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, 0, &n, &safety);
    printf("hal_timing phase=%s callback_status=%d callback_frames=%u safety_status=%d safety_frames=%u\n", phase, (int)fs, (unsigned)cycle, (int)so, (unsigned)safety);
    return 0;
}
