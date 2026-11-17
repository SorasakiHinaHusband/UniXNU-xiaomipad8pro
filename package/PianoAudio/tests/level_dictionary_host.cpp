// SPDX-License-Identifier: BSD-2-Clause-Patent
// Decode the driver's published fields using the 27.3 IOAudio2 CF contract.
#include <CoreFoundation/CoreFoundation.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "../AudioLevelDictionary.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static CFStringRef key(const char *s) { return CFStringCreateWithCString(nullptr, s, kCFStringEncodingUTF8); }
static void put(CFMutableDictionaryRef d, const char *s, CFTypeRef v) {
    CFStringRef k = key(s); CFDictionarySetValue(d, k, v); CFRelease(k);
}
static CFTypeRef get(CFDictionaryRef d, const char *s) {
    CFStringRef k = key(s); CFTypeRef v = CFDictionaryGetValue(d, k); CFRelease(k); return v;
}

// The publisher's small allocation/set/release interface, backed by real CF
// collections. Its number callback models OSNumber's integer serialization.
struct Array;
struct Dictionary {
    CFMutableDictionaryRef cf = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    static Dictionary *withCapacity(unsigned) { return new Dictionary; }
    void setObject(const char *, Array *);
    void release() { CFRelease(cf); delete this; }
};
struct Array {
    CFMutableArrayRef cf = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    static Array *withCapacity(unsigned) { return new Array; }
    void setObject(Dictionary *d) { CFArrayAppendValue(cf, d->cf); }
    void release() { CFRelease(cf); delete this; }
};
void Dictionary::setObject(const char *s, Array *a) { put(cf, s, a->cf); }
static void number(Dictionary *d, const char *s, uint64_t raw, unsigned bits) {
    int32_t n32 = int32_t(raw); int64_t n64; std::memcpy(&n64, &raw, sizeof n64);
    CFNumberRef n = CFNumberCreate(nullptr, bits == 32 ? kCFNumberSInt32Type : kCFNumberSInt64Type,
                                  bits == 32 ? static_cast<void *>(&n32) : static_cast<void *>(&n64));
    put(d->cf, s, n); CFRelease(n);
}
static int32_t raw32(CFDictionaryRef d, const char *s) {
    CFTypeRef v = get(d, s); CHECK(v && CFGetTypeID(v) == CFNumberGetTypeID());
    int32_t n = 0; CHECK(CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberSInt32Type, &n)); return n;
}
static double fixed64(CFDictionaryRef d, const char *s) {
    CFTypeRef v = get(d, s); CHECK(v && CFGetTypeID(v) == CFNumberGetTypeID());
    CHECK(!CFNumberIsFloatType(static_cast<CFNumberRef>(v)));
    int64_t n = 0; CHECK(CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberSInt64Type, &n));
    // GetFixed64: signed high word + unsigned low word / 2^32.
    return double(int32_t(uint64_t(n) >> 32)) + std::ldexp(double(uint32_t(n)), -32);
}
struct Curve { int32_t first, last; float dbFirst, dbLast; };
static bool decode(CFDictionaryRef d, Curve &curve) {
    CFTypeRef map = get(d, "range map");
    if (!map || CFGetTypeID(map) != CFArrayGetTypeID()) return false;
    auto a = static_cast<CFArrayRef>(map);
    if (CFArrayGetCount(a) != 1) return false;
    CFTypeRef entry = CFArrayGetValueAtIndex(a, 0);
    if (CFGetTypeID(entry) != CFDictionaryGetTypeID()) return false;
    auto range = static_cast<CFDictionaryRef>(entry);
    int32_t start = raw32(range, "start int value");
    uint32_t steps = uint32_t(raw32(range, "integer steps"));
    double dbStart = fixed64(range, "start db value"), dbStep = fixed64(range, "db per step");
    // SynchronizeWithDictionary computes endpoints in double, AddRange stores
    // them as Float32. Transfer function 0 normalizes only the raw interval.
    curve = {start, int32_t(start + steps), float(dbStart), float(dbStart + steps * dbStep)};
    return steps != 0;
}
static float scalar(const Curve &c, int32_t raw) {
    int32_t clamped = raw < c.first ? c.first : (raw > c.last ? c.last : raw);
    return float(clamped - c.first) / float(c.last - c.first);
}
static float decibels(const Curve &c, int32_t raw) {
    int32_t clamped = raw < c.first ? c.first : (raw > c.last ? c.last : raw);
    float slope = (c.dbLast - c.dbFirst) / float(c.last - c.first);
    return std::fma(float(clamped - c.first), slope, c.dbFirst);
}
int main() {
    for (uint32_t value : {0U, 25U, 60U, 100U}) {
        Dictionary *d = Dictionary::withCapacity(16);
        CHECK((piano_audio_level_dictionary<Dictionary, Array>(d, value, number)));
        number(d, "value", value, 32);
        // Round-trip a property list as IORegistry serialization does.
        CFDataRef bytes = CFPropertyListCreateData(nullptr, d->cf, kCFPropertyListXMLFormat_v1_0, 0, nullptr);
        CHECK(bytes);
        CFPropertyListRef plist = CFPropertyListCreateWithData(nullptr, bytes, kCFPropertyListImmutable, nullptr, nullptr);
        CHECK(plist); auto dict = static_cast<CFDictionaryRef>(plist);
        CHECK(raw32(dict, "minimum value") == 0 && raw32(dict, "maximum value") == 100);
        CHECK(raw32(dict, "transfer function") == 0);
        for (const char *s : {"left value", "center value", "right value", "value"}) CHECK(raw32(dict, s) == int32_t(value));
        CHECK(raw32(dict, "left channel") == 1 && raw32(dict, "right channel") == 2);
        CHECK(!get(dict, "start db value") && !get(dict, "integer steps"));
        Curve c; CHECK(decode(dict, c));
        CHECK(c.first == 0 && c.last == 100 && c.dbFirst == -36.0f && std::abs(c.dbLast) < 5e-6f && c.dbLast <= 0.0f);
        for (int raw = 0; raw <= 100; ++raw) {
            float s = scalar(c, raw), db = decibels(c, raw);
            CHECK(std::isfinite(s) && s >= 0 && s <= 1);
            CHECK(std::isfinite(db) && db >= -36 && db <= 0);
            CHECK(std::abs(s - float(raw) / 100) < 1e-6f);
        }
        std::printf("level dictionary raw=%u scalar=%.2f dB=%.8g\n", value, scalar(c, value), decibels(c, value));
        // Historical map type must fail this exact decoder, even with valid
        // root-level origin/step numbers. An empty curve divides 0 by 0.
        put(d->cf, "range map", kCFBooleanTrue);
        number(d, "start int value", 0, 32); number(d, "integer steps", 100, 32);
        CHECK(!decode(d->cf, c));
        CHECK(std::isnan(scalar({0, 0, 0, 0}, value)));
        CFRelease(plist); CFRelease(bytes); d->release();
    }
    uint32_t selector = 0x6376616c; float garbage;
    std::memcpy(&garbage, &selector, sizeof garbage);
    CHECK(double(garbage) == 4.5449190280517135e21);
    std::puts("audio level dictionary host: PASS (finite curve, all raw steps, old boolean map rejected)");
}
