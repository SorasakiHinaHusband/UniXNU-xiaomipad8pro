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
  if (t.pageCount != vsmmu::kSessionTablePages)
    return false;
  uint64_t *l2 = t.cpu + 512;
  for (unsigned i = 0; i < 32; ++i)
    l2[((kArenaBase >> 21) & 511u) + i] = vsmmu::tableDesc(t.pa + (4 + i) * 4096);
  for (unsigned i = 0; i < kArenaPages; ++i) {
    if (!layout.mapped(i))
      continue;
    if ((pages[i] & 4095) || pages[i] < kXnuDramBase || pages[i] >= kXnuDramEnd)
      return false;
    t.cpu[4 * 512 + i] = vsmmu::pageDesc(pages[i]);
  }
  for (unsigned i = 0; i < kArenaPages; ++i)
    if (vsmmu::walk(t, kArenaBase + 4096 * i) != (layout.mapped(i) ? pages[i] : 0))
      return false;
  return vsmmu::walk(t, kArenaBase + kArenaBytes) == 0;
}
} // namespace session
} // namespace pianovideo
