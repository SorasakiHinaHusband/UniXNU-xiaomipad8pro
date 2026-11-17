/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. All rights reserved. */
/* Fixed, hash-checked Data views for the native Bluetooth interoperability test.
 * bindfs takes a source C string, not struct union_args (the pinned XNU
 * bindfs_mount copies user_data with copyinstr). Requires bindfs-allow. */
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define ROOT "/private/var/piano/bluetooth-path1"
#define COUNT 4
static const char *const relative[COUNT] = {
    "bin/bluetoothd", "bin/BlueTool", "lib/QcaBTType0.dylib", "bin/PianoBluetooth.runtime"
};
static unsigned char expected[COUNT][CC_SHA256_DIGEST_LENGTH];

static int fail(const char *step)
{
    fprintf(stderr, "piano-bt-path1 mount: %s failed (%d)\n", step, errno ? errno : EINVAL);
    return 1;
}
static bool protected_stat(const struct stat *s, bool directory)
{
    return s->st_uid == 0 && !(s->st_mode & 0022) &&
        (directory ? S_ISDIR(s->st_mode) : S_ISREG(s->st_mode));
}
static bool directory_ok(const char *path)
{
    struct stat s;
    return !lstat(path, &s) && protected_stat(&s, true);
}
static int checked_open(const char *path, bool protected)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat s;
    if (fd >= 0 && (!fstat(fd, &s)) && S_ISREG(s.st_mode) && (!protected || protected_stat(&s, false))) return fd;
    if (fd >= 0) close(fd);
    errno = EPERM;
    return -1;
}
static bool digest(const char *path, unsigned char result[CC_SHA256_DIGEST_LENGTH], bool protected)
{
    int fd = checked_open(path, protected);
    if (fd < 0) return false;
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    unsigned char buffer[32768];
    ssize_t n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) CC_SHA256_Update(&c, buffer, (CC_LONG)n);
    int error = n < 0 ? errno : 0;
    close(fd);
    if (error) { errno = error; return false; }
    CC_SHA256_Final(result, &c);
    return true;
}
static bool manifest(void)
{
    int fd = checked_open(ROOT "/runtime.sha256", true);
    if (fd < 0) return false;
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return false; }
    bool seen[COUNT] = {false};
    bool ok = true;
    char line[256], hex[65], name[128], extra;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%64[0-9a-f] %127s %c", hex, name, &extra) != 2 || strlen(hex) != 64) { ok = false; break; }
        unsigned index;
        for (index = 0; index < COUNT && strcmp(name, relative[index]); ++index) {}
        if (index == COUNT || seen[index]) { ok = false; break; }
        seen[index] = true;
        for (unsigned k = 0; k < CC_SHA256_DIGEST_LENGTH; ++k) {
            unsigned value;
            if (sscanf(hex + 2 * k, "%2x", &value) != 1) { ok = false; break; }
            expected[index][k] = (unsigned char)value;
        }
    }
    if (ferror(f)) ok = false;
    fclose(f);
    for (unsigned k = 0; k < COUNT; ++k) ok = ok && seen[k];
    if (!ok) errno = EINVAL;
    return ok;
}
static bool file_matches(const char *prefix, unsigned index)
{
    char path[256];
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    if (snprintf(path, sizeof(path), "%s/%s", prefix, relative[index]) >= (int)sizeof(path)) return false;
    return digest(path, hash, true) && !memcmp(hash, expected[index], sizeof(hash));
}
static bool view_matches(bool library)
{
    const char *path = library ? "/usr/lib/QcaBTType0.dylib" : "/usr/sbin/PianoBluetooth.runtime";
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    return digest(path, hash, true) && !memcmp(hash, expected[library ? 2 : 3], sizeof(hash));
}
static bool mounted_files_match(void)
{
    const char *const paths[COUNT] = {"/usr/sbin/bluetoothd", "/usr/sbin/BlueTool", "/usr/lib/QcaBTType0.dylib", "/usr/sbin/PianoBluetooth.runtime"};
    for (unsigned k = 0; k < COUNT; ++k) {
        unsigned char hash[CC_SHA256_DIGEST_LENGTH];
        if (!digest(paths[k], hash, true) || memcmp(hash, expected[k], sizeof(hash))) return false;
    }
    return true;
}
static bool overlay(bool library)
{
    if (view_matches(library)) return true;
    const char *point = library ? "/usr/lib" : "/usr/sbin";
    const char *source = library ? ROOT "/lib" : ROOT "/bin";
    struct statfs fs;
    if (statfs(point, &fs)) return false;
    /* Do not stack this test's view on another overlay's mount. */
    if (!strcmp(fs.f_mntonname, point)) { errno = EBUSY; return false; }
    return !mount("bindfs", point, MNT_RDONLY | MNT_UNION, (void *)source) && view_matches(library);
}
int main(int argc, const char *argv[])
{
    (void)argv;
    if (argc != 1 || geteuid() != 0) { errno = EPERM; return fail("root-only fixed invocation"); }
    if (!directory_ok(ROOT) || !directory_ok(ROOT "/bin") || !directory_ok(ROOT "/lib") || !manifest()) return fail("protected runtime manifest");
    for (unsigned k = 0; k < COUNT; ++k) if (!file_matches(ROOT, k)) return fail("source SHA256");
    unsigned char lower[2][CC_SHA256_DIGEST_LENGTH], after[CC_SHA256_DIGEST_LENGTH];
    const char *const sentinel[2] = {"/usr/lib/dyld", "/usr/sbin/notifyd"};
    for (unsigned k = 0; k < 2; ++k) if (!digest(sentinel[k], lower[k], false)) return fail("lower view before mount");
    if (!overlay(true)) return fail("library union");
    if (!overlay(false)) return fail("executable union");
    if (!mounted_files_match()) return fail("mounted SHA256");
    for (unsigned k = 0; k < 2; ++k)
        if (!digest(sentinel[k], after, false) || memcmp(after, lower[k], sizeof(after))) return fail("sealed lower visibility");
    puts("piano-bt-path1 mount: library=verified executables=verified lower=verified");
    return 0;
}
