// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Geometry vectors include padding-heavy small pictures and the cropped 1080
// allocation. Check capacities, the two scratch generations and DMA guard gaps.
#include <cstdio>
#include <cstdlib>
#include "VideoBuffers.h"
using namespace pianovideo::session;
static unsigned failures;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %u: %s\n", __LINE__, #x); ++failures; } } while (0)
struct Vector {
  uint32_t w, h, stride, rows, uv, raw, dpb, bin, comv4, comv16, non, line;
};
static constexpr Vector vectors[] = {
  {160,96,256,96,24576,36864,49152,5652480,35328,139776,249856,347904},
  {320,240,384,256,98304,147456,155648,5652480,172544,688640,557056,685824},
  {1280,720,1280,736,942080,1413120,1445888,5652480,1946112,7782912,1217536,2689280},
  {1920,1088,1920,1088,2088960,3133440,3153920,12533760,4311552,17244672,1217536,4054272},
};
static void verifyLayout(const Layout &l) {
  CHECK(l.bytes < kArenaBytes);
  uint32_t previousEnd = 0;
  for (unsigned i = 0; i < l.used; ++i) {
    const auto &b = l.buf[i];
    CHECK(b.capacity && b.size == b.capacity && !(b.off & 16383));
    CHECK(b.off >= previousEnd);
    CHECK(l.mapped(b.off / 4096));
    CHECK(l.mapped((b.off + b.capacity - 1) / 4096));
    const uint32_t roundedEnd = b.off + ((b.capacity + 16383) & ~16383u);
    CHECK(!l.mapped(roundedEnd / 4096));
    CHECK(!l.mapped((roundedEnd + 16383) / 4096));
    previousEnd = roundedEnd + 16384;
  }
  CHECK(previousEnd == l.bytes);
  CHECK(!l.mapped(l.bytes / 4096));
}
int main() {
  for (const auto &v : vectors) {
    Layout l;
    CHECK(l.init(v.w, v.h));
    CHECK(l.general && l.maxN() == 16 && l.scratchCount() == 4 && l.used == 43);
    CHECK(l.geometry.width == v.w && l.geometry.height == v.h);
    CHECK(l.geometry.stride == v.stride && l.geometry.scanlines == v.rows);
    CHECK(l.uvOffset() == v.uv && l.rawBytes() == v.raw && l.dpbBytes() == v.dpb);
    CHECK(l.scratchSize(0,4) == v.bin && l.scratchSize(1,4) == v.comv4);
    CHECK(l.scratchSize(1,16) == v.comv16 && l.scratchSize(2,4) == v.non && l.scratchSize(3,4) == v.line);
    CHECK(!l.scratchSize(1,0) && !l.scratchSize(1,17) && !l.scratchSize(4,4));
    CHECK(l.find(Raw,15) && l.find(Dpb,15) && !l.find(Raw,16) && !l.find(Partial,0));
    for (unsigned i = 0; i < 4; ++i) {
      const Buffer *a = l.find(scratchType[i],0), *b = l.find(scratchType[i],1);
      CHECK(a && b && a->capacity == l.scratchSize(i,16) && b->capacity == a->capacity);
      CHECK(a && b && a->off != b->off);
    }
    verifyLayout(l);
    if (v.w == 1920) CHECK(l.bytes == 186449920); // 177.8125 MiB, including every guard
  }
  Layout legacy;
  CHECK(legacy.init() && !legacy.general && legacy.used == 141 && legacy.bytes == 56623104);
  CHECK(legacy.maxN() == 64 && legacy.scratchCount() == 5 && legacy.find(Partial,1));
  CHECK(legacy.rawBytes() == 147456 && legacy.dpbBytes() == 155648 && legacy.uvOffset() == 98304);
  verifyLayout(legacy);
  const uint32_t invalid[][2] = {{0,96},{160,0},{161,96},{160,95},{1922,1088},{1920,1090},{UINT32_MAX,UINT32_MAX}};
  for (const auto &v : invalid) {
    CHECK(!legacy.init(v[0],v[1]));
    CHECK(!legacy.general && !legacy.used && !legacy.bytes && !legacy.geometry.width);
  }
  CHECK(legacy.init(1920,1088) && legacy.init() && !legacy.general && legacy.bytes == 56623104);
  std::printf("geometry: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
