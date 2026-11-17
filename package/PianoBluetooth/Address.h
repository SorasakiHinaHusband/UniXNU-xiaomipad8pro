// SPDX-License-Identifier: BSD-2-Clause-Patent
// Stable local address following UniXNU-sheng's domain-separated chosen unique-chip-id hash (NOTICE.md).
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace pianobt {
inline bool addressValid(const uint8_t *address)
{
  if (!address || (address[0] & 1)) return false;
  bool zero = true, ones = true;
  for (unsigned i = 0; i < 6; ++i) { zero &= address[i] == 0; ones &= address[i] == 255; }
  return !zero && !ones;
}
inline bool deriveAddress(const uint8_t *seed, size_t bytes, uint8_t *address)
{
  if (!seed || bytes < 8 || !address) return false;
  uint64_t hash = 0xcbf29ce484222325ULL;
  static const char domain[] = "unixnu-bluetooth";
  for (size_t i = 0; i < sizeof domain - 1; ++i) hash = (hash ^ uint8_t(domain[i])) * 0x100000001b3ULL;
  bool nonzero = false;
  for (size_t i = 0; i < bytes; ++i) { nonzero |= seed[i] != 0; hash = (hash ^ seed[i]) * 0x100000001b3ULL; }
  if (!nonzero) return false;
  for (unsigned i = 0; i < 6; ++i) address[i] = uint8_t(hash >> (i * 8));
  address[0] = (address[0] & 0xfe) | 2;
  return true;
}
}  // namespace pianobt
