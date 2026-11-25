/* SPDX-License-Identifier: MIT */
/* The region1 GPU tier (A830GpuTier.h, RAM16G R1; boot argument piano-gpu-tier, default off).
 *  1. /chosen piano-extra-ram parsing: the 16 GiB SM8750 layout (three runs, 874 MiB), absent, every refusal.
 *  2. The off switch: only piano-gpu-tier=1 turns it on; the last token wins; prefixes and other values stay off.
 *  3. The slab allocator: whole 2 MiB slabs, lowest first, spans inside one run, every span zeroed, an all-or-nothing refusal
 *     (span limit, too big), release refuses foreign / partial / double frees, exhaustion and refill over 2000 rounds.
 *  4. The per-run self-test: a run whose probe fails is kept out (its line says FAIL) and never handed out.
 * Build: clang++ -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined test_a830gputier.cpp -o /tmp/test_a830gputier && /tmp/test_a830gputier
 * host-test: */
#include "../A830GpuTier.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace a830tier;
static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static std::vector<uint8_t> prop(std::vector<Run> rs, uint32_t version = 1, uint32_t count = ~0u) {
  std::vector<uint8_t> b(kPropBytes, 0);
  b[0] = (uint8_t)version; b[1] = (uint8_t)(version >> 8);
  uint32_t c = count == ~0u ? (uint32_t)rs.size() : count; b[4] = (uint8_t)c;
  for (size_t i = 0; i < rs.size() && i < kMaxRuns; i++) { put64(&b[8 + 16 * i], rs[i].base); put64(&b[16 + 16 * i], rs[i].size); }
  return b;
}
struct Ctx { std::vector<Span> zeroed; std::vector<uint64_t> badProbe; std::vector<std::string> lines; };
static void zero(uint64_t pa, uint64_t bytes, void *c) { ((Ctx *)c)->zeroed.push_back({pa, bytes}); }
static bool probe(uint64_t pa, void *c) { for (auto b : ((Ctx *)c)->badProbe) if (b == pa) return false; return true; }
static void line(const char *l, void *c) { ((Ctx *)c)->lines.push_back(l); }
static const uint64_t MiB = 1ull << 20;
int main() {
  /* 1 */
  std::vector<Run> sm = {{0x880000000ull, 512 * MiB}, {0x8a0400000ull, 256 * MiB}, {0x8b0400000ull, 106 * MiB}};
  Run r[kMaxRuns]; unsigned n = 9;
  auto p = prop(sm);
  CHECK(parseProperty(p.data(), p.size(), r, n) == Parse::ok && n == 3 && r[2].size == 106 * MiB);
  CHECK(parseProperty(nullptr, 0, r, n) == Parse::absent && n == 0);
  CHECK(parseProperty(p.data(), p.size() - 1, r, n) == Parse::badLength);
  auto v2 = prop(sm, 2); CHECK(parseProperty(v2.data(), v2.size(), r, n) == Parse::badVersion);
  auto c9 = prop(sm, 1, 9); CHECK(parseProperty(c9.data(), c9.size(), r, n) == Parse::badCount);
  auto c2 = prop(sm, 1, 2); CHECK(parseProperty(c2.data(), c2.size(), r, n) == Parse::badCount);   /* a third pair left over */
  auto ua = prop({{0x880001000ull, 2 * MiB}}); CHECK(parseProperty(ua.data(), ua.size(), r, n) == Parse::unaligned);
  auto z = prop({{0x880000000ull, 0}}); CHECK(parseProperty(z.data(), z.size(), r, n) == Parse::unaligned);
  auto lo = prop({{0x80000000ull, 2 * MiB}}); CHECK(parseProperty(lo.data(), lo.size(), r, n) == Parse::outOfRegion);
  auto hi = prop({{0x8bfe00000ull, 4 * MiB}}); CHECK(parseProperty(hi.data(), hi.size(), r, n) == Parse::outOfRegion);
  auto ov = prop({{0x880000000ull, 4 * MiB}, {0x880200000ull, 2 * MiB}}); CHECK(parseProperty(ov.data(), ov.size(), r, n) == Parse::overlap);
  auto none = prop({}); CHECK(parseProperty(none.data(), none.size(), r, n) == Parse::ok && n == 0);
  /* 2 */
  CHECK(!bootArgOn(nullptr)); CHECK(!bootArgOn("")); CHECK(!bootArgOn("piano-dpu=2 piano-gpu-user-vm=1"));
  CHECK(bootArgOn("piano-dpu=2 piano-gpu-tier=1")); CHECK(!bootArgOn("piano-gpu-tier=0"));
  CHECK(!bootArgOn("piano-gpu-tier=1 piano-gpu-tier=0")); CHECK(bootArgOn("piano-gpu-tier=0 piano-gpu-tier=1"));
  CHECK(!bootArgOn("piano-gpu-tier=11")); CHECK(!bootArgOn("piano-gpu-tiers=1")); CHECK(!bootArgOn("xpiano-gpu-tier=1"));
  /* 3 */
  Ctx ctx; Tier t;
  CHECK(t.init(sm.data(), 3, zero, probe, &ctx, line));
  CHECK(t.totalBytes() == 874 * MiB && t.freeBytes() == 874 * MiB && ctx.lines.size() == 3);
  CHECK(ctx.lines[0] == "PianoA830: gpu tier: run 0 0x880000000+512 MiB self-test ok");
  Span s[8];
  CHECK(t.alloc(1, s, 8) == 1 && s[0].pa == 0x880000000ull && s[0].bytes == 2 * MiB && ctx.zeroed.size() == 1);
  CHECK(t.freeBytes() == 872 * MiB);
  Span big[8];
  unsigned bn = t.alloc(600 * MiB, big, 8);                     /* rest of run 0 (510 MiB) + 90 MiB of run 1 */
  CHECK(bn == 2 && big[0].pa == 0x880200000ull && big[0].bytes == 510 * MiB && big[1].pa == 0x8a0400000ull && big[1].bytes == 90 * MiB);
  struct Piece { uint64_t offset, physical, bytes; } pc[2];
  asPieces(big, bn, pc);
  CHECK(pc[0].offset == 0 && pc[0].physical == 0x880200000ull && pc[1].offset == 510 * MiB && pc[1].physical == 0x8a0400000ull && pc[1].bytes == 90 * MiB);
  CHECK(t.owns(0x8a0400000ull) && !t.owns(0x8c0000000ull) && !t.owns(0x87fe00000ull));
  Span one[1]; size_t z0 = ctx.zeroed.size(); uint64_t f0 = t.freeBytes();
  CHECK(t.alloc(300 * MiB, one, 1) == 0 && t.freeBytes() == f0 && ctx.zeroed.size() == z0);   /* needs 2 spans: refused whole */
  CHECK(t.alloc(t.freeBytes() + 1, s + 1, 7) == 0 && t.freeBytes() == f0);                     /* too big: nothing taken */
  Span foreign = {0x8c0000000ull, 2 * MiB}, part = {0x880100000ull, 2 * MiB};
  CHECK(!t.release(&foreign, 1)); CHECK(!t.release(&part, 1));
  CHECK(t.release(big, bn) && !t.release(big, bn));             /* double free refused, nothing changed */
  CHECK(t.release(s, 1) && t.freeBytes() == 874 * MiB);
  srand(7); std::vector<std::vector<Span>> held; uint64_t heldBytes = 0;
  for (int round = 0; round < 2000; round++) {
    if (held.empty() || (rand() % 3)) {
      uint64_t want = (uint64_t)(1 + rand() % 64) * MiB; std::vector<Span> v(8);
      unsigned k = t.alloc(want, v.data(), 8);
      if (k) { v.resize(k); uint64_t b = 0; for (auto &x : v) b += x.bytes; CHECK(b == (want + kSlab - 1) / kSlab * kSlab); heldBytes += b; held.push_back(v); }
    } else {
      size_t i = (size_t)rand() % held.size(); uint64_t b = 0; for (auto &x : held[i]) b += x.bytes;
      CHECK(t.release(held[i].data(), (unsigned)held[i].size())); heldBytes -= b; held.erase(held.begin() + (long)i);
    }
    CHECK(t.freeBytes() + heldBytes == 874 * MiB);
  }
  for (auto &v : held) CHECK(t.release(v.data(), (unsigned)v.size()));
  CHECK(t.freeBytes() == 874 * MiB);
  /* 4 */
  Ctx bad; bad.badProbe.push_back(0x8a0400000ull + 256 * MiB - kSlab); Tier u;
  CHECK(u.init(sm.data(), 3, zero, probe, &bad, line));
  CHECK(bad.lines[1] == "PianoA830: gpu tier: run 1 0x8a0400000+256 MiB self-test FAIL" && u.totalBytes() == 618 * MiB);
  Span w[8]; unsigned wn = u.alloc(600 * MiB, w, 8);
  CHECK(wn == 2 && w[0].pa == 0x880000000ull && w[1].pa == 0x8b0400000ull && !u.owns(0x8a0400000ull));
  Tier e; CHECK(!e.init(sm.data(), 3, nullptr, probe, &ctx, nullptr)); CHECK(e.alloc(2 * MiB, w, 8) == 0);
  if (fails) { std::fprintf(stderr, "%d of %d checks failed\n", fails, checks); return 1; }
  std::printf("PASS: gpu tier: piano-extra-ram parsing (SM8750 874 MiB, 11 refusals), the piano-gpu-tier off switch (10 cases), "
              "2 MiB slab spans with zeroing and all-or-nothing refusals, the BO piece list, 2000 alloc/release rounds, a failed run's self-test kept out\n");
  return 0;
}
