// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../../../package/NativeGopDisplay/GopShot.h"
#include "../Pixels.h"

static void check_pixel(uint32_t word, int packed10, const uint8_t expected[4])
{
    uint8_t pixel[4] = {word, word >> 8, word >> 16, word >> 24};
    gop_shot_rgbx(pixel, packed10);
    assert(memcmp(pixel, expected, sizeof pixel) == 0);
}

static void check_chunks(uint64_t stride)
{
    const size_t frame = GOP_SHOT_ROW_BYTES * GOP_SHOT_HEIGHT;
    const uint64_t end = (GOP_SHOT_HEIGHT - 1) * stride + GOP_SHOT_ROW_BYTES;
    const uint64_t base = 4096;
    assert(gop_shot_bounds(1, base + end, base, stride) == 0);
    assert(gop_shot_bounds(1, base + end - 1, base, stride) == EINVAL);
    size_t position = 0;
    while (position < frame) {
        uint64_t offset;
        const size_t count = gop_shot_chunk(position, stride, &offset);
        assert(count > 0 && count <= GOP_SHOT_CHUNK_BYTES);
        assert(count <= frame - position);
        assert(offset + count <= end);
        if (stride == GOP_SHOT_ROW_BYTES) {
            assert(offset == position);
        } else {
            assert(offset == (position / GOP_SHOT_ROW_BYTES) * stride);
            assert(count == GOP_SHOT_ROW_BYTES);
        }
        position += count;
    }
    uint64_t offset;
    assert(gop_shot_chunk(position, stride, &offset) == 0);
    assert(gop_shot_chunk(SIZE_MAX, stride, &offset) == 0);
}

int main(void)
{
    check_pixel(0xc0000000, 1, (uint8_t[]){0, 0, 0, 255});
    check_pixel(0x3fffffff, 1, (uint8_t[]){255, 255, 255, 255});
    check_pixel(1023u << 20, 1, (uint8_t[]){255, 0, 0, 255});
    check_pixel(1023u << 10, 1, (uint8_t[]){0, 255, 0, 255});
    check_pixel(1023u, 1, (uint8_t[]){0, 0, 255, 255});
    check_pixel((511u << 20) | (512u << 10) | 256u, 1, (uint8_t[]){127, 128, 64, 255});
    check_pixel(0x00123456, 0, (uint8_t[]){0x12, 0x34, 0x56, 0xff});
    assert(gop_shot_srgb8(2) == 0 && gop_shot_srgb8(3) == 1);
    assert(gop_shot_srgb8(1020) == 254 && gop_shot_srgb8(1021) == 255);

    check_chunks(GOP_SHOT_ROW_BYTES);
    check_chunks(GOP_SHOT_ROW_BYTES + 64);
    assert(gop_shot_bounds(1, UINT64_MAX, 0, GOP_SHOT_ROW_BYTES - 1) == EINVAL);
    assert(gop_shot_bounds(1, UINT64_MAX, 0, UINT64_MAX) == EINVAL);
    assert(gop_shot_bounds(1, 1, UINT64_MAX, GOP_SHOT_ROW_BYTES) == EINVAL);
    assert(gop_shot_bounds(1, UINT64_MAX, UINT64_MAX - 1, GOP_SHOT_ROW_BYTES) == EINVAL);

    assert(gop_shot_access(0, 1, 0) == ENOENT);
    assert(gop_shot_access(1, 0, 0) == EPERM);
    assert(gop_shot_access(1, 1, 1) == EPERM);
    assert(gop_shot_access(1, 1, 0) == 0);
    assert(gop_shot_bounds(0, 0, 0, GOP_SHOT_ROW_BYTES) == ENXIO);
    puts("gop-shot host tests passed");
    return 0;
}
