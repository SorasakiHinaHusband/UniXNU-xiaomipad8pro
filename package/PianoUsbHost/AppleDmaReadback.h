// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang. The readable xHCI DMA pointers only.
// xHCI 5.4.5, Table 5-24: CRCR[63:6] always reads as zero, even after
// successful command DMA. Keep its raw status for diagnostics, never use it
// as an address. This does not establish Apple's command-ring DMA completion.
#pragma once
#include "XhciHost.h"
#include "SmmuIdentity.h"

namespace pianousbhost {
struct AppleDmaReadback {
  uint64_t dcbaap, crcr, erstba, erdp;
  const char *failure() const
  {
    if (!identityRange(dcbaap & ~0x3full, 64)) return "apple-dma-dcbaap";
    if (!identityRange(erstba & ~0x3full, 16)) return "apple-dma-erstba";
    if (!identityRange(erdp & ~0xfull, 16)) return "apple-dma-erdp";
    return nullptr;
  }
};
template <class Hal> AppleDmaReadback appleDmaReadback(Hal &h, const Xhci &x)
{
  const XhciAt at = xhciAt(x);
  return {rd64(h, at.op + kOpDcbaap), rd64(h, at.op + kOpCrcr),
          rd64(h, at.rt + kRtIr0 + kIrErstba), rd64(h, at.rt + kRtIr0 + kIrErdp)};
}
}  // namespace pianousbhost
