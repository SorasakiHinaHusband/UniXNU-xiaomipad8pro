/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. */
/* Atomic administrator-only PianoPower configuration. Never exposes MCA writes.
 * --save persists only daily configuration, never the bounded test inhibit.
 * --apply-saved waits for the service and reapplies it after a boot. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const keys[] = {"ChargeLimitEnabled", "ChargeLimitStop", "ChargeLimitResume", "ChargeMode",
                                  "ChargeTestInhibitSeconds", "ChargeConfigRevision"};
static const char *config = "/private/var/db/piano-charge/config";
static unsigned number(CFDictionaryRef d, const char *key) {
    unsigned n = 0;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef value = k ? CFDictionaryGetValue(d, k) : NULL;
    if (value && CFGetTypeID(value) == CFNumberGetTypeID()) CFNumberGetValue(value, kCFNumberIntType, &n);
    if (k) CFRelease(k);
    return n;
}
static CFDictionaryRef state(io_service_t s) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, CFSTR("PianoCharge"), NULL, 0);
    if (v && CFGetTypeID(v) != CFDictionaryGetTypeID()) {CFRelease(v); return NULL;}
    return (CFDictionaryRef)v;
}
static int parse(const char *s, unsigned *out) {
    char *end; errno = 0; unsigned long n = strtoul(s, &end, 10);
    if (!*s || *s == '-' || *end || errno || n > 0xffffffffu) return 0;
    *out = (unsigned)n; return 1;
}
static int persist(const unsigned v[6]) {
    if (v[4]) return 1;
    if (mkdir("/private/var/db/piano-charge", 0700) && errno != EEXIST) return 1;
    const char *tmp = "/private/var/db/piano-charge/config.new";
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return 1;
    char line[80]; int n = snprintf(line, sizeof line, "%u %u %u %u\n", v[0], v[1], v[2], v[3]);
    int rc = write(fd, line, (size_t)n) != n || fsync(fd);
    close(fd);
    return rc || rename(tmp, config);
}
int main(int argc, char **argv) {
    unsigned v[6] = {1,80,75,1,0,0};
    int save = argc > 1 && !strcmp(argv[1], "--save");
    int apply = argc == 2 && !strcmp(argv[1], "--apply-saved");
    int show = argc == 2 && !strcmp(argv[1], "--show");
    if (!show && geteuid()) {fputs("administrator required\n", stderr); return 1;}
    if (apply) {
        int fd = open(config, O_RDONLY | O_NOFOLLOW);
        if (fd < 0) return errno == ENOENT ? 0 : 1;
        struct stat st; char line[96] = {0};
        int bad = fstat(fd, &st) || st.st_uid || (st.st_mode & 022) || !S_ISREG(st.st_mode);
        ssize_t n = bad ? -1 : read(fd, line, sizeof line - 1); close(fd);
        char extra;
        if (n <= 0 || sscanf(line, "%u %u %u %u %c", &v[0], &v[1], &v[2], &v[3], &extra) != 4) return 1;
    } else if (!show) {
        int first = save ? 2 : 1, count = argc - first;
        if (count != 4 && count != 5) {
            fputs("usage: piano-charge-config [--save] enabled stop resume mode [inhibitSeconds]\n"
                  "       piano-charge-config --show | --apply-saved\n", stderr); return 2;
        }
        for (int i = 0; i < count; ++i) if (!parse(argv[first+i], &v[i])) return 2;
    }
    if (v[0] > 1 || v[1] < 50 || v[1] > 100 || v[2] < 40 || v[2] >= v[1] || v[3] > 2 || v[4] > 180 || (save && v[4])) {
        fputs("configuration range refused\n", stderr); return 2;
    }
    io_service_t service = 0;
    for (int i = 0; i < (apply ? 120 : 1); ++i) {
        service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PianoPower"));
        if (service) break;
        sleep(1);
    }
    if (!service) {fputs("PianoPower unavailable\n", stderr); return 1;}
    CFDictionaryRef d = state(service);
    if (!d) {IOObjectRelease(service); fputs("PianoCharge state unavailable\n", stderr); return 1;}
    v[5] = number(d, "ConfigRevision");
    if (show) {
        CFDataRef xml = CFPropertyListCreateData(NULL, d, kCFPropertyListXMLFormat_v1_0, 0, NULL);
        if (xml) {fwrite(CFDataGetBytePtr(xml), 1, (size_t)CFDataGetLength(xml), stdout); CFRelease(xml);}
        CFRelease(d); IOObjectRelease(service); return xml ? 0 : 1;
    }
    CFRelease(d);
    CFMutableDictionaryRef request = CFDictionaryCreateMutable(NULL, 6, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!request) {IOObjectRelease(service); return 1;}
    for (unsigned i = 0; i < 6; ++i) {
        CFStringRef k = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v[i]);
        if (!k || !n) {if(k)CFRelease(k);if(n)CFRelease(n);CFRelease(request);IOObjectRelease(service);return 1;}
        CFDictionarySetValue(request, k, n); CFRelease(k); CFRelease(n);
    }
    kern_return_t kr = IORegistryEntrySetCFProperties(service, request); CFRelease(request);
    if (kr) {fprintf(stderr, "transaction refused code=0x%x\n", kr); IOObjectRelease(service); return 1;}
    int rc = 1;
    for (unsigned i = 0; i < 100; ++i) {
        usleep(100000); d = state(service);
        if (!d) continue;
        unsigned revision = number(d, "ConfigRevision"), error = number(d, "ConfigError");
        CFRelease(d);
        if (revision == v[5]+1 && !error) {
            printf("config accepted revision=%u enabled=%u stop=%u resume=%u mode=%u inhibit=%u\n", revision,v[0],v[1],v[2],v[3],v[4]);
            rc = save ? persist(v) : 0; break;
        }
    }
    IOObjectRelease(service);
    if (rc) fputs("configuration not confirmed; inspect readback\n", stderr);
    return rc;
}
