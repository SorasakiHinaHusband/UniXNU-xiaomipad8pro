// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Host admission only: synthetic syntax is not a firmware decode fixture.
#include "../vt/PianoVideoCodec.h"
#include <assert.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
using Bytes = std::vector<uint8_t>;
namespace h = pianovideo::hevc;
namespace c = pianovideo::codec;
struct Writer {
  Bytes data; unsigned bit = 0;
  void put(uint32_t value, unsigned count) {
    for (unsigned i = count; i; --i) { if (!bit) data.push_back(0); data.back() |= ((value >> (i - 1)) & 1) << (7 - bit); bit = (bit + 1) & 7; }
  }
  void ue(uint32_t value) { unsigned bits = 0; for (uint32_t v = value + 1; v >>= 1;) ++bits; put(0, bits); put(value + 1, bits + 1); }
  void se(int value) { ue(value <= 0 ? uint32_t(-2 * value) : uint32_t(2 * value - 1)); }
  void end() { put(1, 1); while (bit) put(0, 1); }
  Bytes nal(uint32_t type, uint32_t temporal = 0) {
    Bytes out{uint8_t(type << 1), uint8_t(temporal + 1)}; unsigned zeros = 0;
    for (uint8_t value : data) { if (zeros >= 2 && value <= 3) { out.push_back(3); zeros = 0; } out.push_back(value); zeros = value ? 0 : zeros + 1; }
    return out;
  }
};
static void ptl(Writer &w, unsigned depth) {
  w.put(0, 2); w.put(0, 1); w.put(depth == 10 ? 2 : 1, 5); w.put(depth == 10 ? 0x20000000 : 0x40000000, 32);
  w.put(0x9000, 16); w.put(0, 32); w.put(120, 8);
}
static void ordering(Writer &w) { w.put(0, 1); w.ue(5); w.ue(2); w.ue(0); }
struct Fixture {
  Bytes vps, sps, pps, hvcc; unsigned depth; bool wpp;
  Fixture(unsigned bits = 8, bool wave = true, unsigned width = 1920, unsigned height = 1080) : depth(bits), wpp(wave) {
    Writer v; v.put(0, 4); v.put(3, 2); v.put(0, 6); v.put(0, 3); v.put(1, 1); v.put(0xffff, 16); ptl(v, depth);
    ordering(v); v.put(0, 6); v.ue(0); v.put(0, 1); v.put(0, 1); v.end(); vps = v.nal(32);
    Writer s; s.put(0, 4); s.put(0, 3); s.put(1, 1); ptl(s, depth); s.ue(0); s.ue(1); s.ue(width); s.ue(height); s.put(0, 1);
    s.ue(depth - 8); s.ue(depth - 8); s.ue(4); ordering(s); s.ue(0); s.ue(3); s.ue(0); s.ue(3); s.ue(0); s.ue(0);
    s.put(0, 1); s.put(1, 1); s.put(0, 1); s.put(0, 1); s.ue(1); s.ue(1); s.ue(0); s.ue(0); s.put(1, 1);
    s.put(0, 1); s.put(0, 1); s.put(1, 1); s.put(0, 1); s.put(0, 1); s.end(); sps = s.nal(33);
    Writer p; p.ue(0); p.ue(0); p.put(0, 1); p.put(0, 1); p.put(0, 3); p.put(0, 1); p.put(0, 1); p.ue(0); p.ue(0); p.se(0);
    p.put(0, 1); p.put(0, 1); p.put(0, 1); p.se(0); p.se(0); p.put(0, 1); p.put(0, 1); p.put(0, 1); p.put(0, 1);
    p.put(0, 1); p.put(wpp, 1); p.put(0, 1); p.put(0, 1); p.put(0, 1); p.put(0, 1); p.ue(0); p.put(0, 1); p.put(0, 1); p.end(); pps = p.nal(34);
    hvcc = {1, uint8_t(depth == 10 ? 2 : 1), uint8_t(depth == 10 ? 0x20 : 0x40), 0, 0, 0, 0x90, 0, 0, 0, 0, 0, 120,
      0xf0, 0, 0xfc, 0xfd, uint8_t(0xf8 + depth - 8), uint8_t(0xf8 + depth - 8), 0, 0, 0x0f, 3};
    for (const auto &n : {vps, sps, pps}) { hvcc.push_back(0x80 | (n[0] >> 1)); hvcc.push_back(0); hvcc.push_back(1); hvcc.push_back(n.size() >> 8); hvcc.push_back(n.size()); hvcc.insert(hvcc.end(), n.begin(), n.end()); }
  }
  Bytes slice(unsigned type = 19, unsigned kind = 2, unsigned temporal = 0) const {
    Writer w; w.put(1, 1); if (type >= 16) w.put(0, 1); w.ue(0); w.ue(kind);
    if (type != 19 && type != 20) { w.put(1, 8); w.put(1, 1); }
    if (kind != 2) { w.put(0, 1); if (kind == 0) w.put(0, 1); w.ue(0); }
    w.se(0); if (wpp) w.ue(0); w.end(); w.put(0x80, 8); return w.nal(type, temporal);
  }
};
static void append(Bytes &out, const Bytes &nal) {
  const uint32_t size = nal.size(); out.push_back(size >> 24); out.push_back(size >> 16); out.push_back(size >> 8); out.push_back(size); out.insert(out.end(), nal.begin(), nal.end());
}
static Bytes readFile(const char *path) { std::ifstream in(path, std::ios::binary); assert(in); return Bytes(std::istreambuf_iterator<char>(in), {}); }
static void unit() {
  for (unsigned depth : {8u, 10u}) for (bool wave : {false, true}) {
    Fixture f(depth, wave); c::Config cfg; assert(c::parseHevc(f.hvcc.data(), f.hvcc.size(), cfg));
    assert(cfg.valid() && cfg.isHevc() && cfg.depth == depth && cfg.s.width == 1920 && cfg.s.height == 1080 && cfg.s.maxDpb == 6 && cfg.s.maxReorder == 2);
    assert(cfg.hfiProfile() == (depth == 10 ? 2u : 0u) && cfg.hfiLevel() == 5 && !cfg.hfiTier());
    assert(cfg.pixelFormat() == (depth == 10 ? 0x78343230u : 0x34323076u) && cfg.format() == 0x68766331u);
    c::Config same; assert(c::parseHevc(f.hvcc.data(), f.hvcc.size(), same) && cfg.same(same));
    for (unsigned size = 0; size < f.hvcc.size(); ++size) { c::Config bad; assert(!c::parseHevc(f.hvcc.data(), size, bad) && !bad.valid()); }
    for (unsigned at : {0u, 13u, 15u, 16u, 17u, 18u, 21u, 22u, 23u}) {
      Bytes bad = f.hvcc; bad[at] ^= at == 23 ? 0x40 : at == 21 ? 1 : 0x80; c::Config out; assert(!c::parseHevc(bad.data(), bad.size(), out));
    }
    for (unsigned kind : {0u, 1u, 2u}) {
      Bytes au; append(au, f.slice(kind == 2 ? 19 : 1, kind)); c::Picture picture;
      assert(c::accessUnit(au.data(), au.size(), cfg, &picture) && picture.idr == (kind == 2));
      Bytes normalized(h::MaxAu); const uint32_t size = c::normalize(au.data(), au.size(), normalized.data(), normalized.size(), cfg, true);
      assert(size == au.size() + cfg.parameterBytes() && c::normalized(normalized.data(), size, cfg));
      assert(!c::normalize(au.data(), au.size(), normalized.data(), size - 1, cfg, true));
      Bytes bad = au; bad[4] |= 0x80; assert(!c::accessUnit(bad.data(), bad.size(), cfg));
      bad = au; bad[5] = 0; assert(!c::accessUnit(bad.data(), bad.size(), cfg));
      bad = au; bad[5] |= 8; assert(!c::accessUnit(bad.data(), bad.size(), cfg));
      bad = au; bad[5] = 2; assert(!c::accessUnit(bad.data(), bad.size(), cfg));
      bad = au; bad[6] &= 0x7f; assert(!c::accessUnit(bad.data(), bad.size(), cfg));
      bad = au; append(bad, f.slice()); assert(!c::accessUnit(bad.data(), bad.size(), cfg));
      bad = au; bad[3] = 255; assert(!c::accessUnit(bad.data(), bad.size(), cfg));
      bad = au; bad.push_back(0); assert(!c::accessUnit(bad.data(), bad.size(), cfg));
      bad = au; append(bad, Bytes{80, 1, 0x80}); assert(c::accessUnit(bad.data(), bad.size(), cfg)); // suffix SEI
      bad = au; append(bad, f.sps); assert(!c::accessUnit(bad.data(), bad.size(), cfg));
      bad.clear(); append(bad, Bytes{78, 1, 5, 1, 0, 0x80}); bad.insert(bad.end(), au.begin(), au.end()); assert(c::accessUnit(bad.data(), bad.size(), cfg)); // opaque prefix SEI
      bad = au; append(bad, Bytes{80, 1, 5, 10, 0, 0x80}); assert(!c::accessUnit(bad.data(), bad.size(), cfg));
    }
  }
  for (auto size : {std::pair<unsigned, unsigned>{3848, 2160}, {3840, 2184}, {3840, 2176}, {1920, 1081}}) {
    Fixture f(10, true, size.first, size.second); c::Config cfg; assert(!c::parseHevc(f.hvcc.data(), f.hvcc.size(), cfg));
  }
  Fixture f; c::Config cfg; assert(c::parseHevc(f.hvcc.data(), f.hvcc.size(), cfg));
  const uint8_t illegal[][7] = {{2,1,0,0,0,0x80,0x80},{2,1,0,0,1,0x80,0x80},{2,1,0,0,3,4,0x80},{2,1,0,0,3,0x80,0x80}};
  for (const auto &nal : illegal) assert(!h::ebsp(nal, sizeof(nal)));
  const uint8_t valid[] = {2,1,0,0,3,0,0x80}; assert(h::ebsp(valid, sizeof(valid)));
  const uint8_t overflow[] = {0,0,3,0,0,3,0,0,3,0}; h::Bits bits{overflow, sizeof(overflow)}; bits.ue(); assert(!bits.good);
  assert(h::level(183) == 11 && h::level(186) == UINT32_MAX);
  std::cout << "HEVC_UNIT_PASS main8=1 main10=1 WPP_B_RPS=1 malformed_rejected=1 config_bytes=" << sizeof(c::Config) << "\n";
}
static void corpus(const char *configPath, const char *samplesPath) {
  const Bytes hvcc = readFile(configPath), samples = readFile(samplesPath); c::Config cfg;
  assert(c::parseHevc(hvcc.data(), hvcc.size(), cfg));
  Bytes out(h::MaxAu); uint32_t count = 0, idrs = 0, maxAu = 0, tids = 0, types = 0; size_t at = 0;
  while (at < samples.size()) {
    assert(samples.size() - at >= 4); const uint32_t size = uint32_t(samples[at]) << 24 | uint32_t(samples[at + 1]) << 16 | uint32_t(samples[at + 2]) << 8 | samples[at + 3]; at += 4;
    assert(size <= samples.size() - at); c::Picture p;
    if (!c::accessUnit(samples.data() + at, size, cfg, &p)) { std::cerr << "rejected AU " << count << " bytes=" << size << "\n"; std::abort(); }
    const uint32_t written = c::normalize(samples.data() + at, size, out.data(), out.size(), cfg, true);
    assert(written == size + cfg.parameterBytes() && c::normalized(out.data(), written, cfg));
    const uint32_t without = c::normalize(samples.data() + at, size, out.data(), out.size(), cfg, false);
    assert(without == size && c::normalized(out.data(), without, cfg));
    size_t source = at; uint32_t dest = 0;
    while (source < at + size) {
      const uint32_t length = uint32_t(samples[source]) << 24 | uint32_t(samples[source + 1]) << 16 | uint32_t(samples[source + 2]) << 8 | samples[source + 3]; source += 4;
      assert(!out[dest] && !out[dest + 1] && !out[dest + 2] && out[dest + 3] == 1 && !memcmp(samples.data() + source, out.data() + dest + 4, length));
      source += length; dest += length + 4;
    }
    if (!count) assert(p.idr); ++count; if (p.idr) ++idrs; tids |= 1u << p.temporalId; types |= 1u << p.nalType;
    if (size > maxAu) maxAu = size; at += size;
  }
  std::cout << "HEVC_CORPUS_PASS samples=" << count << " idrs=" << idrs << " max_au=" << maxAu << " temporal_mask=" << tids << " nal_mask=" << types
    << " width=" << cfg.s.width << " height=" << cfg.s.height << " depth=" << cfg.depth << " profile=" << cfg.s.profile << " level=" << cfg.s.level
    << " tier=" << cfg.hfiTier() << " RPS=" << cfg.hevc.s.numRps << " WPP=" << cfg.hevc.p.wpp << " dpb=" << cfg.s.maxDpb << " reorder=" << cfg.s.maxReorder << "\n";
}
int main(int argc, char **argv) { unit(); if (argc == 3) corpus(argv[1], argv[2]); else assert(argc == 1); }
