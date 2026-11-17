// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// V5a qualification contract, deliberately narrower than general AVC decoding.
#pragma once
#include "PianoVideoVTWire.h"
#include "PianoVideoNative.h"
#include "../VideoFixture.h"
namespace pianovideo { namespace vtcontract {
constexpr uint32_t kMaxAuBytes = 1u << 20;
// Bound snapshots independently of the native AU limit. Rejected frames still
// need completion; a dfrm error reply alone does not retire a VT frame.
constexpr uint32_t kMaxMessageBytes = 4u << 20;
constexpr uint32_t kMaxSurfaceBytes = 4u << 20; // bounded task wiring for the tiny qualification pool
constexpr uint32_t kParameterBytes = 35; // start4+SPS22+start4+PPS5
inline bool atoms(const uint8_t *p, uint32_t n) {
  const uint8_t *at = p, *end = p + n;
  vtwire::Atom a;
  unsigned count = 0;
  while (at != end) if (++count > 128 || !vtwire::next(at, end, a)) return false;
  return true;
}
inline bool unique(const uint8_t *p, uint32_t n, uint32_t type, vtwire::Atom &result) {
  if (!atoms(p, n)) return false;
  const uint8_t *at = p, *end = p + n;
  vtwire::Atom a; bool found = false;
  while (vtwire::next(at, end, a)) if (a.type == type) {
    if (found) return false;
    found = true; result = a;
  }
  return found;
}
inline bool message(const uint8_t *p, uint32_t n) {
  if (!p || n < vtwire::HeaderBytes || n > kMaxMessageBytes || !atoms(p, n)) return false;
  const uint8_t *at = p + vtwire::HeaderBytes, *end = p + n;
  vtwire::Atom a;
  while (vtwire::next(at, end, a)) {
    if (a.type != vtwire::cc("para") || a.size < 8) return false;
    const uint8_t *q = p + vtwire::HeaderBytes;
    vtwire::Atom b;
    while (q < at - a.size - 8 && vtwire::next(q, end, b))
      if (vtwire::be32(b.body) == vtwire::be32(a.body)) return false;
  }
  return true;
}
// Read only the bounded slice-header prefix needed for the supported progressive
// Baseline SPS (POC type2). Emulation prevention is skipped without an extra copy.
struct Bits {
  const uint8_t *p; uint32_t n, byte = 0, bit = 0, zeros = 0; bool good = true;
  uint32_t read(unsigned count) {
    uint32_t v = 0;
    for (unsigned i = 0; i < count; ++i) {
      if (!bit && byte < n && zeros >= 2 && p[byte] == 3) { ++byte; zeros = 0; }
      if (byte >= n) { good = false; return 0; }
      v = (v << 1) | ((p[byte] >> (7-bit)) & 1);
      if (++bit == 8) { zeros = p[byte] ? 0 : zeros + 1; ++byte; bit = 0; }
    }
    return v;
  }
  uint32_t ue() {
    unsigned leading = 0;
    while (good && !read(1)) if (++leading > 20) { good = false; return 0; }
    return good ? ((1u << leading) - 1 + read(leading)) : 0;
  }
};
inline bool idr(const uint8_t *p, uint32_t n) {
  if (!p || n < 4 || n > kMaxAuBytes - kParameterBytes - 4 || p[0] != 0x65) return false;
  Bits b{p + 1, n - 1};
  const uint32_t first = b.ue(), slice = b.ue(), pps = b.ue();
  const uint32_t frame = b.read(4), id = b.ue();
  // No P/B slices or multiple slices/pictures; one IDR starts the only AU.
  return b.good && first == 0 && (slice == 2 || slice == 7) && pps == 0 && frame == 0 && id <= 65535;
}
inline bool sample(const uint8_t *p, uint32_t n) {
  return p && n >= 8 && n <= kMaxAuBytes - kParameterBytes && vtwire::be32(p) == n - 4 && idr(p + 4, n - 4);
}
inline bool normalizedAu(const uint8_t *p, uint32_t n) {
  if (!p || n < kParameterBytes + 8 || n > kMaxAuBytes || memcmp(p, session::kAccessUnit, kParameterBytes)) return false;
  const uint8_t start[4] = {0, 0, 0, 1};
  if (memcmp(p + kParameterBytes, start, 4) || !idr(p + kParameterBytes + 4, n - kParameterBytes - 4)) return false;
  // Another Annex-B start code is a second slice/AU or unsupported NAL. Reject
  // forbidden raw 00 00 {00,01,02}; emulation-prevented fixture passes.
  for (uint32_t i = kParameterBytes + 5; i + 2 < n; ++i)
    if (!p[i] && !p[i+1] && p[i+2] < 3) return false;
  return true;
}
inline uint32_t normalize(const uint8_t *p, uint32_t n, uint8_t *out, uint32_t cap) {
  if (!sample(p, n) || !out || cap < n + kParameterBytes) return 0;
  memcpy(out, session::kAccessUnit, kParameterBytes);
  vtwire::put32(out + kParameterBytes, 1);
  memcpy(out + kParameterBytes + 4, p + 4, n - 4);
  const uint32_t bytes = n + kParameterBytes;
  return normalizedAu(out, bytes) ? bytes : 0;
}
struct InputReport {
  int64_t token = 0;
  bool hasToken = false;
  uint32_t parameterType = 0, bytes = 0;
  const uint8_t *data = nullptr;
  const char *decision = "frame-parameter";
};
inline bool inputRequest(const uint8_t *m, uint32_t n, InputReport &r) {
  r = InputReport{};
  if (!message(m,n) || !(r.hasToken = vtwire::paramS64(m,n,vtwire::cc(",frm"),r.token))) return false;
  vtwire::Param p; vtwire::Atom sbuf,sdat;
  r.decision = "sample-parameter";
  if (!vtwire::param(m,n,vtwire::cc(",sbf"),p)) return false;
  r.parameterType = p.type;
  if (p.type != vtwire::cc("=sbf")) return false;
  r.decision = "sbuf-layout";
  if (!unique(p.value,p.size,vtwire::cc("sbuf"),sbuf)) return false;
  r.decision = "sdat-layout";
  if (!unique(sbuf.body,sbuf.size,vtwire::cc("sdat"),sdat)) return false;
  r.data = sdat.body; r.bytes = sdat.size; r.decision = "sample-not-qualified";
  if (!sample(r.data,r.bytes)) return false;
  r.decision = "accept"; return true;
}
struct FormatReport {
  uint32_t parameterType = 0, bytes = 0, declared = 0, codec = 0, width = 0, height = 0;
  uint32_t extensionBytes = 0, atomBytes = 0, avcBytes = 0;
  bool terminalZero = false;
  const char *decision = "missing-fds";
};
// QuickTime ImageDescription extensions alone may end in a zero u32. The
// native CM bridge includes it in the declared size. Recognize it only at an
// atom boundary and at the exact end; message/sample atoms remain strict.
inline bool imageExtensions(const uint8_t *p, uint32_t n, uint32_t &atomBytes, bool &terminalZero) {
  atomBytes = 0; terminalZero = false;
  if (!p) return false;
  const uint8_t *at = p, *end = p + n;
  vtwire::Atom a;
  unsigned count = 0;
  while (at != end) {
    if (end - at == 4 && !vtwire::be32(at)) { terminalZero = true; atomBytes = n - 4; return true; }
    if (++count > 128 || !vtwire::next(at, end, a)) return false;
  }
  atomBytes = n;
  return true;
}
inline bool format(const vtwire::ImageDescription &d, FormatReport *report = nullptr) {
  using namespace vtwire;
  FormatReport local;
  FormatReport &r = report ? *report : local;
  r.codec = d.codec; r.width = d.width; r.height = d.height; r.extensionBytes = d.extensionsSize;
  r.avcBytes = 0;
  r.decision = "extension-layout";
  if (!imageExtensions(d.extensions, d.extensionsSize, r.atomBytes, r.terminalZero)) return false;
  r.decision = "codec";
  if (d.codec != cc("avc1")) return false;
  r.decision = "dimensions";
  if (d.width != 320 || d.height != 240) return false;
  r.decision = "avcC-missing-or-duplicate";
  Atom avc;
  if (!unique(d.extensions, r.atomBytes, cc("avcC"), avc)) return false;
  r.avcBytes = avc.size;
  r.decision = "avcC-configuration";
  if (avc.size != 38) return false;
  const uint8_t *p = avc.body;
  // avcC version, Baseline compatibility/level, NAL length4, one SPS22 and PPS5.
  if (p[0] != 1 || p[1] != 66 || p[2] != 0xc0 || p[3] != 13 || p[4] != 0xff || p[5] != 0xe1 || be16(p+6) != 22 ||
      memcmp(p+8, session::kAccessUnit+4, 22) || p[30] != 1 || be16(p+31) != 5 || memcmp(p+33, session::kAccessUnit+30, 5)) return false;
  Atom protectedAtom;
  r.decision = "protected-or-hevc";
  if (find(d.extensions, r.atomBytes, cc("sinf"), protectedAtom) || find(d.extensions, r.atomBytes, cc("hvcC"), protectedAtom)) return false;
  r.decision = "accept";
  return true;
}
// Shared by dsta/dcan and by the real CoreMedia serializer host test.
inline bool formatRequest(const uint8_t *m, uint32_t n, FormatReport &r) {
  using namespace vtwire;
  r = FormatReport{};
  r.decision = "message-layout";
  if (!message(m, n)) return false;
  Param p;
  r.decision = "missing-fds";
  if (!param(m, n, cc(",fds"), p)) return false;
  r.parameterType = p.type; r.bytes = p.size;
  if (p.size >= 4) r.declared = be32(p.value);
  r.decision = "fds-type";
  if (p.type != cc("=fds")) return false;
  ImageDescription d;
  r.decision = "image-description";
  if (!imageDescription(p.value, p.size, d)) return false;
  r.codec = d.codec; r.width = d.width; r.height = d.height; r.extensionBytes = d.extensionsSize;
  r.decision = "declared-size";
  if (r.declared != p.size) return false;
  return format(d, &r);
}
inline bool extent(uint64_t offset, uint64_t stride, uint64_t rows, uint64_t width, uint64_t size) {
  return rows && width && stride >= width && offset <= size && width <= size-offset && rows-1 <= (size-offset-width)/stride;
}
inline bool layout(const native::Layout &l, uint64_t length) {
  return l.width == 320 && l.height == 240 && l.stride == 384 && l.scanlines == 256 && l.yOffset == 0 && l.uvOffset == 98304 &&
    l.bytes <= length && l.bytes >= 144256 && l.bitdepth == 8 && l.format == vtwire::cc("420v") &&
    extent(l.yOffset,l.stride,l.height,l.width,l.bytes) && extent(l.uvOffset,l.stride,l.height/2,l.width,l.bytes);
}
struct Plane { uint64_t width = 0, height = 0, offset = 0, stride = 0; uint32_t nonlinear = 0; };
struct Surface {
  uint64_t base = 0, size = 0; uint32_t id = 0, format = 0, planes = 0; Plane plane[2];
};
inline bool surface(const Surface &s) {
  return s.base && s.size && s.size <= kMaxSurfaceBytes && s.base <= UINT64_MAX-s.size && s.format == vtwire::cc("420v") && s.planes == 2 &&
    s.plane[0].width == 320 && s.plane[0].height == 240 && s.plane[1].width == 160 && s.plane[1].height == 120 &&
    !s.plane[0].nonlinear && !s.plane[1].nonlinear &&
    extent(s.plane[0].offset,s.plane[0].stride,240,320,s.size) && extent(s.plane[1].offset,s.plane[1].stride,120,320,s.size) &&
    s.plane[1].offset >= s.plane[0].offset + 239*s.plane[0].stride + 320;
}
// Native 27.3 IOSurface lookup lock result. Plane+0x6c is a byte:
// IOSurfaceClientGetBytesPerRowOfPlane 0x1876b7978 uses ldrb, as does
// GetBaseAddressOfPlane 0x1876b7c2c. Neighbouring bytes are not this flag.
inline bool surfaceResult(const uint8_t *b, uint32_t n, uint32_t id, Surface &v) {
  if (!b || n != 0xc68) return false;
  auto u64 = [&](uint32_t at) { uint64_t x; memcpy(&x,b+at,8); return x; };
  auto u32 = [&](uint32_t at) { uint32_t x; memcpy(&x,b+at,4); return x; };
  v.base=u64(0); v.id=u32(0x18); v.size=u64(0x20); v.format=u32(0x48); v.planes=u32(0x58);
  for (unsigned p=0;p<2;++p) {
    const uint32_t at=0x88+p*0x98;
    v.plane[p]={u64(at),u64(at+8),u64(at+0x18),u64(at+0x20),b[at+0x6c]};
  }
  return v.id==id && surface(v);
}
// All calls under the session lock. Invalid input does not consume the one-AU
// budget. Cancellation suppresses success, settles an accepted frame once.
struct RejectedFrame { int64_t token = 0; int error = 0; };
// Independent of the single accepted AU. A full queue fails the transport, so
// the guest's send-error path completes its frame without losing a token.
struct RejectedFrames {
  static constexpr unsigned Capacity = 8;
  RejectedFrame frames[Capacity];
  unsigned head = 0, count = 0;
  bool push(int64_t token, int error) {
    if (!error || count == Capacity) return false;
    frames[(head + count) % Capacity] = {token,error}; ++count; return true;
  }
  bool pop(RejectedFrame &frame) {
    if (!count) return false;
    frame = frames[head]; head = (head + 1) % Capacity; --count; return true;
  }
};
struct State {
  bool started = false, accepted = false, completed = false, terminal = false, cancelled = false;
  bool accept() { if (!started || accepted || terminal || cancelled) return false; accepted = true; return true; }
  bool complete() { if (!accepted || completed) return false; completed = true; return true; }
  bool success() const { return accepted && !completed && !cancelled; }
  // VT's WaitForAsynchronousFrames calls FinishDelayedFrames even after a
  // rejected sample. No native input means nothing to drain or terminate.
  void finish() { if (accepted) terminal = true; }
  void cancel() { cancelled = terminal = true; }
};
} }
