// SPDX-License-Identifier: BSD-2-Clause-Patent
// D0 EDID 1.4 and CTA-861 timing model. No allocation or I/O.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "DpPort.h"

namespace pianodp {

constexpr size_t kEdidBlockBytes = 128, kEdidMaxBlocks = 4;
constexpr size_t kEdidMaxBytes = kEdidBlockBytes * kEdidMaxBlocks;
constexpr uint32_t kDpRbrKhz = kRateRbr * 100, kDpHbrKhz = kRateHbr * 100, kDpHbr2Khz = kRateHbr2 * 100;

struct DpLink {
  uint32_t rateKhz = 0;  // link symbol clock, one symbol per ten wire bits per lane
  unsigned lanes = 0;
};

constexpr bool validDpLaneCount(unsigned lanes) { return lanes == 1 || lanes == 2 || lanes == 4; }
constexpr bool validDpLink(DpLink link)
{
  return validDpLaneCount(link.lanes) &&
         (link.rateKhz == kDpRbrKhz || link.rateKhz == kDpHbrKhz || link.rateKhz == kDpHbr2Khz);
}

struct DpTiming {
  uint32_t pixelKhz = 0;
  uint16_t hActive = 0, hBlank = 0, hSyncOffset = 0, hSyncWidth = 0;
  // For an interlaced DTD these are the on-wire EDID values per field.
  uint16_t vActive = 0, vBlank = 0, vSyncOffset = 0, vSyncWidth = 0;
  bool hSyncPositive = false, vSyncPositive = false;
  bool separateSync = true, interlaced = false;
};

constexpr bool validDpTiming(const DpTiming &timing)
{
  return timing.pixelKhz && timing.hActive && timing.vActive && timing.hBlank && timing.vBlank &&
         timing.hSyncWidth && timing.vSyncWidth &&
         uint32_t(timing.hActive) + timing.hBlank <= UINT16_MAX &&
         uint32_t(timing.vActive) + timing.vBlank <= UINT16_MAX &&
         uint32_t(timing.hSyncOffset) + timing.hSyncWidth <= timing.hBlank &&
         uint32_t(timing.vSyncOffset) + timing.vSyncWidth <= timing.vBlank;
}

constexpr unsigned kDpRgb8BitsPerPixel = 24;

constexpr bool edidModeFits(const DpTiming &timing, DpLink link)
{
  return validDpTiming(timing) && validDpLink(link) &&
         modeFits(timing.pixelKhz, kDpRgb8BitsPerPixel, link.lanes, link.rateKhz / 100);
}

struct EdidMode {
  DpTiming timing;
  uint8_t vic = 0;  // zero for a DTD
};

struct EdidInfo {
  // Four base DTDs, up to six per extension, and one entry per supported VIC.
  static constexpr size_t kMaxModes = 4 + (kEdidMaxBlocks - 1) * 6 + 5;
  char manufacturer[4]{};
  uint16_t productCode = 0;
  uint16_t widthMm = 0, heightMm = 0;  // both zero when the base block gives only an aspect ratio or no size
  uint8_t extensionCount = 0;
  size_t modeCount = 0;
  EdidMode modes[kMaxModes]{};  // modes[0] is the required base preferred timing
  uint32_t vicBits[8]{};        // includes advertised SVDs outside the model's timing table
  uint64_t nativeVicBits = 0;   // only VICs 1–64 can carry the SVD native flag

  constexpr bool hasVic(uint8_t vic) const { return (vicBits[vic / 32] >> (vic % 32)) & 1; }
  constexpr bool nativeVic(uint8_t vic) const
  {
    return vic >= 1 && vic <= 64 && ((nativeVicBits >> (vic - 1)) & 1);
  }
};

enum class EdidStatus : uint8_t {
  Ok, InvalidSize, BadHeader, BadChecksum, UnsupportedVersion, BadManufacturer, BadTiming, BadExtension,
};

namespace edid_detail {

constexpr size_t kDescriptorBytes = 18;

inline bool allZero(const uint8_t *bytes, size_t length)
{
  for (size_t i = 0; i < length; ++i)
    if (bytes[i]) return false;
  return true;
}

inline uint16_t le16(const uint8_t *bytes) { return uint16_t(bytes[0] | (bytes[1] << 8)); }

inline bool isDisplayDescriptor(const uint8_t *descriptor)
{
  const uint8_t tag = descriptor[3];
  return !le16(descriptor) && !descriptor[2] && (tag <= 0x10 || tag >= 0xf7) &&
         (!descriptor[4] || tag == 0xfd);  // EDID 1.4 range limits may use byte 4.
}

inline EdidStatus appendDtd(EdidInfo &info, const uint8_t *dtd)
{
  const bool separateSync = (dtd[17] & 0x18) == 0x18;
  const DpTiming timing{
    uint32_t(le16(dtd)) * 10,
    uint16_t(dtd[2] | ((dtd[4] & 0xf0) << 4)), uint16_t(dtd[3] | ((dtd[4] & 0x0f) << 8)),
    uint16_t(dtd[8] | ((dtd[11] & 0xc0) << 2)), uint16_t(dtd[9] | ((dtd[11] & 0x30) << 4)),
    uint16_t(dtd[5] | ((dtd[7] & 0xf0) << 4)), uint16_t(dtd[6] | ((dtd[7] & 0x0f) << 8)),
    uint16_t((dtd[10] >> 4) | ((dtd[11] & 0x0c) << 2)), uint16_t((dtd[10] & 0x0f) | ((dtd[11] & 3) << 4)),
    separateSync && (dtd[17] & 2), separateSync && (dtd[17] & 4), separateSync, bool(dtd[17] & 0x80),
  };
  if (!validDpTiming(timing) || info.modeCount == EdidInfo::kMaxModes) return EdidStatus::BadTiming;
  info.modes[info.modeCount++] = {timing, 0};
  return EdidStatus::Ok;
}

// CTA-861 1080p60 and UHD 30/60 (16:9 and 64:27). The 533.25 MHz UHD and
// 241.5 MHz 1440p60 reduced-blanking modes are advertised via DTDs.
constexpr DpTiming vicTiming(uint8_t vic)
{
  switch (vic) {
    case 16: return {148500, 1920, 280, 88, 44, 1080, 45, 4, 5, true, true};
    case 95: case 105: return {297000, 3840, 560, 176, 88, 2160, 90, 8, 10, true, true};
    case 97: case 107: return {594000, 3840, 560, 176, 88, 2160, 90, 8, 10, true, true};
    default: return {};
  }
}

inline EdidStatus appendSvd(EdidInfo &info, uint8_t svd)
{
  const bool native = svd >= 129 && svd <= 192;
  const uint8_t vic = native ? uint8_t(svd & 0x7f) : svd;
  if (vic == 0 || vic == 128 || vic >= 254) return EdidStatus::BadExtension;
  if (native) info.nativeVicBits |= uint64_t{1} << (vic - 1);
  if (info.hasVic(vic)) return EdidStatus::Ok;
  info.vicBits[vic / 32] |= uint32_t{1} << (vic % 32);
  const DpTiming timing = vicTiming(vic);
  if (timing.pixelKhz) {
    if (info.modeCount == EdidInfo::kMaxModes) return EdidStatus::BadExtension;
    info.modes[info.modeCount++] = {timing, vic};
  }
  return EdidStatus::Ok;
}

inline EdidStatus parseCta(EdidInfo &info, const uint8_t *block)
{
  constexpr size_t end = kEdidBlockBytes - 1;  // checksum byte, excluded
  const unsigned revision = block[1];
  const size_t start = block[2];
  if (!revision || revision > 3 || (revision == 1 && block[3])) return EdidStatus::BadExtension;
  if (!start) return allZero(block + 4, end - 4) ? EdidStatus::Ok : EdidStatus::BadExtension;
  if (start < 4 || start > end) return EdidStatus::BadExtension;

  size_t offset = start;
  while (end - offset >= kDescriptorBytes && !allZero(block + offset, kDescriptorBytes)) {
    const uint8_t *descriptor = block + offset;
    if (!isDisplayDescriptor(descriptor)) {
      const EdidStatus status = appendDtd(info, descriptor);
      if (status != EdidStatus::Ok) return status;
    }
    offset += kDescriptorBytes;
  }
  if (!allZero(block + offset, end - offset)) return EdidStatus::BadExtension;

  // Revisions 1 and 2 reserve the area before the DTDs. Revision 3 frames it as data blocks.
  if (revision < 3) return EdidStatus::Ok;
  for (offset = 4; offset < start;) {
    const uint8_t header = block[offset++];
    if (!header) return allZero(block + offset, start - offset) ? EdidStatus::Ok : EdidStatus::BadExtension;
    const size_t length = header & 31;
    const unsigned tag = header >> 5;
    if (!length || length > start - offset || !tag) return EdidStatus::BadExtension;
    if (tag == 2) {
      for (size_t i = 0; i < length; ++i) {
        const EdidStatus status = appendSvd(info, block[offset + i]);
        if (status != EdidStatus::Ok) return status;
      }
    }
    offset += length;
  }
  return EdidStatus::Ok;
}

} // namespace edid_detail

// Expects exactly the declared blocks, including the base. Clears info on every failure.
inline EdidStatus parseEdid(const uint8_t *bytes, size_t length, EdidInfo &info)
{
  using namespace edid_detail;
  info = {};
  if (!bytes || length < kEdidBlockBytes || length > kEdidMaxBytes || length % kEdidBlockBytes)
    return EdidStatus::InvalidSize;
  if (size_t(bytes[126]) + 1 != length / kEdidBlockBytes) return EdidStatus::InvalidSize;

  constexpr uint8_t header[] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
  for (size_t i = 0; i < sizeof header; ++i)
    if (bytes[i] != header[i]) return EdidStatus::BadHeader;
  for (size_t offset = 0; offset < length; offset += kEdidBlockBytes) {
    unsigned sum = 0;
    for (size_t i = 0; i < kEdidBlockBytes; ++i) sum += bytes[offset + i];
    if (sum & 0xff) return EdidStatus::BadChecksum;
  }
  if (bytes[18] != 1 || bytes[19] != 4) return EdidStatus::UnsupportedVersion;

  EdidInfo parsed;
  const unsigned pnp = (unsigned(bytes[8]) << 8) | bytes[9];
  if (pnp & 0x8000) return EdidStatus::BadManufacturer;
  for (unsigned i = 0; i < 3; ++i) {
    const unsigned letter = (pnp >> (10 - 5 * i)) & 31;
    if (!letter || letter > 26) return EdidStatus::BadManufacturer;
    parsed.manufacturer[i] = char('A' + letter - 1);
  }
  parsed.productCode = le16(bytes + 10);
  if (bytes[21] && bytes[22]) {
    parsed.widthMm = uint16_t(bytes[21] * 10);
    parsed.heightMm = uint16_t(bytes[22] * 10);
  }
  parsed.extensionCount = bytes[126];
  for (size_t i = 0; i < 4; ++i) {
    const uint8_t *descriptor = bytes + 54 + i * kDescriptorBytes;
    if (i && isDisplayDescriptor(descriptor)) continue;
    const EdidStatus status = appendDtd(parsed, descriptor);
    if (status != EdidStatus::Ok) return status;
  }
  for (size_t offset = kEdidBlockBytes; offset < length; offset += kEdidBlockBytes) {
    if (!bytes[offset]) return EdidStatus::BadExtension;
    if (bytes[offset] != 2) continue;  // other checksummed extension types are opaque
    const EdidStatus status = parseCta(parsed, bytes + offset);
    if (status != EdidStatus::Ok) return status;
  }
  info = parsed;
  return EdidStatus::Ok;
}

// Prefer DTDs in EDID order, then supported CTA VICs in advertisement order.
inline const EdidMode *preferredFittingMode(const EdidInfo &info, DpLink link)
{
  if (info.modeCount > EdidInfo::kMaxModes) return nullptr;
  for (unsigned source = 0; source < 2; ++source) {
    for (size_t i = 0; i < info.modeCount; ++i) {
      const auto &mode = info.modes[i];
      if (unsigned(mode.vic != 0) == source && edidModeFits(mode.timing, link)) return &mode;
    }
  }
  return nullptr;
}

} // namespace pianodp
