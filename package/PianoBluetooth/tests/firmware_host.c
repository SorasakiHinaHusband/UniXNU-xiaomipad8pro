/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. All rights reserved. */
/* Exercise the real firmware adapter and shared FAT reader over a fake UFS LUN.
 * The payload is generated test data, never controller firmware. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "BtFirmware.h"
#include "FatVolume.h"

static uint8_t *disk;
static size_t disk_size, alloc_calls, fail_alloc, live_allocs, reads, escaped;
static int ready = 1, fail_read, bad_a;
void *bt_fw_alloc(size_t size) {
    if (++alloc_calls == fail_alloc) return NULL;
    void *p = calloc(1, size);
    if (p) ++live_allocs;
    return p;
}
void bt_fw_free(void *p) { if (p) { assert(live_allocs); --live_allocs; free(p); } }
int native_ufs_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks) {
    if (!ready) return 0;
    assert(!strcmp(name, "bluetooth_a") || !strcmp(name, "bluetooth_b"));
    *lun = 4; *first = !strcmp(name, "bluetooth_a") ? 56710 : 239036; *blocks = disk_size / 4096;
    return 1;
}
int native_ufs_part_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst) {
    assert(lun == 4 && blocks && blocks <= 256);
    uint64_t first = lba >= 239036 ? 239036 : 56710;
    uint64_t at = (lba - first) * 4096, n = (uint64_t)blocks * 4096;
    ++reads;
    if (at > disk_size || n > disk_size - at) { ++escaped; return 0; }
    if (fail_read) return 0;
    memcpy(dst, disk + at, (size_t)n);
    if (bad_a && first == 56710 && !at) memset(dst, 0, (size_t)n);
    return 1;
}
static int image_read(void *ctx, uint64_t at, void *out, size_t n) {
    (void)ctx;
    if (at > disk_size || n > disk_size - at) { ++escaped; return EIO; }
    memcpy(out, disk + at, n); return 0;
}
static void put16(size_t at, uint16_t x) { disk[at] = x; disk[at+1] = x >> 8; }
static void put32(size_t at, uint32_t x) { put16(at, x); put16(at+2, x >> 16); }
static void entry(size_t at, const char *name, int directory, uint16_t cluster, uint32_t size) {
    memcpy(disk + at, name, 11); disk[at+11] = directory ? 16 : 32; put16(at+26, cluster); put32(at+28, size);
}
static void fixture(void) {
    piano_bt_firmware_close(); assert(!live_allocs);
    memset(disk, 0, 64u*1024*1024);
    disk_size = 8u*1024*1024; alloc_calls = fail_alloc = reads = escaped = 0;
    ready = 1; fail_read = bad_a = 0;
    put16(11, 4096); disk[13] = 4; put16(14, 1); disk[16] = 2;
    put16(17, 512); put16(19, 16384); put16(22, 3); disk[510] = 0x55; disk[511] = 0xaa;
    put16(4096, 0xfff8); put16(4098, 0xffff); put16(4100, 0xffff);
    put16(4102, 4); put16(4104, 0xffff);
    entry(28672, "IMAGE      ", 1, 2, 0);
    entry(45056, "TEST    BIN", 0, 3, 20000);
    for (size_t i=0; i<20000; ++i) disk[61440+i] = (uint8_t)(i*7+3);
}
int main(void) {
    disk = calloc(1, 64u*1024*1024); assert(disk);
    FatVolume v; FatFile f; const char *part = NULL; uint32_t size; uint8_t out[20000];
    fixture();
    assert(fat_mount(&v, disk_size, image_read, NULL) == FatNotFat);
    assert(!fat_mount_prefix(&v, disk_size, image_read, NULL));
    assert(!v.fat32 && v.sector_bytes == 4096 && v.cluster_bytes == 16384 && v.clusters == 509 && v.size == disk_size);
    assert(!fat_open(&v, "image/test.bin", &f));
    assert(!fat_read(&v, &f, 0, out, sizeof out));
    for (size_t i=0;i<sizeof out;++i) assert(out[i] == (uint8_t)(i*7+3));
    assert(!piano_bt_firmware_open(&part) && !strcmp(part, "bluetooth_a"));
    size_t allocations = alloc_calls;
    assert(!piano_bt_firmware_open(&part) && alloc_calls == allocations);
    assert(!piano_bt_firmware_stat("test.bin", &size) && size == sizeof out);
    memset(out, 0, sizeof out);
    assert(!piano_bt_firmware_read("test.bin", 37, out, 19312));
    for (size_t i=0;i<19312;++i) assert(out[i] == (uint8_t)((i+37)*7+3));
    assert(piano_bt_firmware_stat("absent.bin", &size) == PianoBtFwNotFound);
    assert(piano_bt_firmware_read("test.bin", UINT64_MAX, out, 1) == PianoBtFwRange);
    assert(piano_bt_firmware_read("test.bin", 19999, out, 2) == PianoBtFwRange);
    assert(!piano_bt_firmware_read("test.bin", 20000, out, 0));
    assert(!escaped && reads); piano_bt_firmware_close(); assert(!live_allocs);
    assert(piano_bt_firmware_stat("test.bin", &size) == PianoBtFwNoPartition);
    fixture(); ready = 0; assert(piano_bt_firmware_open(NULL) == PianoBtFwNotReady); assert(!live_allocs);
    for (size_t i=1;i<=2;++i) { fixture(); fail_alloc=i; assert(piano_bt_firmware_open(NULL) == PianoBtFwNoMemory); assert(!live_allocs); }
    fixture(); fail_read=1; assert(piano_bt_firmware_open(NULL) == PianoBtFwReadError); assert(!live_allocs && !escaped);
    fixture(); disk[510]=0; assert(piano_bt_firmware_open(NULL) == PianoBtFwNotFat); assert(!live_allocs);
    fixture(); bad_a=1; assert(!piano_bt_firmware_open(&part) && !strcmp(part, "bluetooth_b"));
    /* A chain or directory can name a declared cluster outside the physical prefix. */
    fixture(); put16(4102, 600); assert(!fat_mount_prefix(&v, disk_size, image_read, NULL));
    assert(!fat_open(&v, "image/test.bin", &f)); assert(fat_read(&v, &f, 0, out, sizeof out) == FatDamaged); assert(!escaped);
    fixture(); put16(45056+26, 600); assert(!fat_mount_prefix(&v, disk_size, image_read, NULL));
    assert(!fat_open(&v, "image/test.bin", &f)); assert(fat_read(&v, &f, 0, out, 1) == FatDamaged); assert(!escaped);
    fixture(); put16(4102, 3); put32(45056+28, 64u*1024*1024); assert(!fat_mount_prefix(&v, disk_size, image_read, NULL));
    assert(!fat_open(&v, "image/test.bin", &f)); assert(fat_read(&v, &f, disk_size, out, 1) == FatDamaged); assert(!escaped);
    fixture(); assert(fat_mount_prefix(&v, 45056, image_read, NULL) == FatNotFat);
    assert(fat_mount_prefix(&v, 45056+16383, image_read, NULL) == FatNotFat);
    fixture(); put16(22, 3000); put32(32, 100000); put16(19, 0);
    assert(fat_mount_prefix(&v, disk_size, image_read, NULL) == FatNotFat);
    /* Strict FAT16 and FAT32 mounts keep the old geometry and rejection rules. */
    fixture(); disk_size=64u*1024*1024;
    assert(!fat_mount(&v, disk_size, image_read, NULL) && !v.fat32 && v.clusters==4093);
    fixture(); disk_size=70000u*512; put16(11,512); disk[13]=1; put16(14,32); put16(17,0);
    put16(19,0); put32(32,70000); put16(22,0); put32(36,600); put32(44,2);
    assert(!fat_mount(&v,disk_size,image_read,NULL) && v.fat32 && v.clusters==68768);
    assert(!fat_mount_prefix(&v,disk_size,image_read,NULL) && v.fat32 && v.clusters==68768);
    piano_bt_firmware_close(); assert(!live_allocs && !escaped); free(disk);
    puts("PASS firmware adapter: prefix geometry, strict FAT16/32, real UFS path/cache, fallback, allocation/read errors, corruption and physical bounds");
    return 0;
}
