// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
#pragma once
#include "PianoLlccIsland.h"

namespace pianollcc {

struct AuditRead {
  Block view;
  uint32_t offset;
  uint32_t mask;
  uint32_t expected;

  constexpr uint64_t physical() const { return kBlockPhys[view] + offset; }
  constexpr bool accepts(uint32_t word) const { return word != UINT32_MAX && (word & mask) == expected; }
};

// Register facts from third-party yzddmr6/linux-sm8750-piano at
// 1c21cb9895d69c1c5e83ceae68952fedf522644a. After the two probe reads,
// follow the v6 control-word order for SCID12. The boot's flag bits are data.
constexpr AuditRead kAuditReads[] = {
  {kOr,       kHwInfo,  0xff000000u, kVersion6},
  {kChannel0, kStatus0, 0xf0000000u, 4u << 28},
  {kOr, 0x42080, 0, 0},  // WRS_EN
  {kOr, 0x42088, 0, 0},  // WRS_CACHEABLE_EN
  {kOr, 0x42008, 0, 0},  // ALGO_STALE_EN
  {kOr, 0x42010, 0, 0},  // ALGO_STALE_CAP_EN
  {kOr, 0x42018, 0, 0},  // ALGO_MRU0
  {kOr, 0x42020, 0, 0},  // ALGO_MRU1
  {kOr, 0x42028, 0, 0},  // ALGO_ALLOC0
  {kOr, 0x42030, 0, 0},  // ALGO_ALLOC1
  {kOr, 0x42038, 0, 0},  // ALGO_ALLOC2
  {kOr, 0x42040, 0, 0},  // ALGO_ALLOC3
};
constexpr unsigned kAuditCount = sizeof kAuditReads / sizeof kAuditReads[0];
static_assert(kAuditCount == 12, "two metadata reads and ten SCID12 control reads");

}  // namespace pianollcc
