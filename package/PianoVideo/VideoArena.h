// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang. The same V3 bank, with independent backed session pages and invalid guards.
#pragma once
#include "VideoOps.h"
#include "VideoBuffers.h"
#include "VideoSmmu.h"
namespace pianovideo
{
namespace session
{
inline bool buildArena(vsmmu::Tables &t, const Layout &layout, const uint64_t *pages)
{
  const uint32_t arenaBytes = layout.arenaBytes();
  const unsigned arenaPages = arenaBytes / 4096, tablePages = vsmmu::sessionTablePages(arenaBytes);
  if (!t.cpu || !pages || (t.pa & 4095) || !tablePages || t.pageCount != tablePages || layout.bytes > arenaBytes)
    return false;
  uint64_t *l2 = t.cpu + 512;
  static_assert(((kArenaBase >> 21) & 511u) + kArenaBytes / (2 * 1024 * 1024) <= 512, "arena fits one L2");
  static_assert(vsmmu::kSessionTablePages == 4 + kAvcArenaBytes / (2 * 1024 * 1024), "AVC arena L3 table coverage");
  static_assert(vsmmu::kMaxSessionTablePages == 4 + kArenaBytes / (2 * 1024 * 1024), "HEVC arena L3 table coverage");
  for (unsigned i = 0; i < arenaPages; ++i)
    if (layout.mapped(i) && ((pages[i] & 4095) || pages[i] < kXnuDramBase || pages[i] >= kXnuDramEnd))
      return false;
  for (unsigned i = 4 * 512; i < t.pageCount * 512; ++i) t.cpu[i] = 0;
  for (unsigned i = 0; i < arenaBytes / (2 * 1024 * 1024); ++i)
    l2[((kArenaBase >> 21) & 511u) + i] = vsmmu::tableDesc(t.pa + (4 + i) * 4096);
  for (unsigned i = 0; i < arenaPages; ++i) {
    if (!layout.mapped(i))
      continue;
    t.cpu[4 * 512 + i] = vsmmu::pageDesc(pages[i]);
  }
  for (unsigned i = 0; i < arenaPages; ++i)
    if (vsmmu::walk(t, kArenaBase + 4096 * i) != (layout.mapped(i) ? pages[i] : 0))
      return false;
  return vsmmu::walk(t, kArenaBase + arenaBytes) == 0;
}
} // namespace session
} // namespace pianovideo
