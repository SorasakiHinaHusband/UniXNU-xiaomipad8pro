/* SPDX-License-Identifier: MIT */
#ifndef A830_GPU_TIER_H
#define A830_GPU_TIER_H
#include <stddef.h>
#include <stdint.h>
#if defined(KERNEL)
#include <libkern/libkern.h>   /* snprintf */
#else
#include <stdio.h>
#endif

/* RAM16G R1, the region1 GPU tier (UniXNU-piano private/host-checks/20261125-ram16g/ANALYSIS.md "Fix-image gate").
 *
 * DarwinBoot (firmware/patches/darwinboot/0002) reserves DRAM region1's conventional runs below XNU's window (SM8750 16 GiB:
 * 874 MiB at 0x880000000.., XNU never manages them) and publishes them in /chosen piano-extra-ram: u32 version 1, u32 count,
 * then 8 pairs u64 base, u64 size, little endian. With boot argument piano-gpu-tier=1 (default 0: the tier is never built
 * and every GPU buffer is backed by XNU pages as before), PianoA830 backs paged GPU buffers from these runs first, in 2 MiB
 * slabs, and falls back to XNU pages when the tier cannot hold a request.
 *
 * Pure and allocation-free (one fixed bitmap): the kernel side supplies the zeroing (bzero_phys on each slab span) and the
 * per-run self-test probe (write and read back a pattern at a physical address through its CPU view), and holds its own lock
 * around alloc/release. Every physical span it returns is whole slabs inside one published run. */
namespace a830tier {

constexpr uint64_t kSlab = 2ull << 20;
constexpr unsigned kMaxRuns = 8;
constexpr unsigned kPropBytes = 8 + 16 * kMaxRuns;
constexpr unsigned kMaxSlabs = 1024;                   /* 2 GiB of slabs: region1's runs are below 1 GiB */
constexpr uint64_t kRegionLo = 0x880000000ull;         /* region1: the bank below the window's 0x8c0000000 */
constexpr uint64_t kRegionHi = 0x8c0000000ull;

struct Run { uint64_t base; uint64_t size; };
struct Span { uint64_t pa; uint64_t bytes; };

enum class Parse { ok, absent, badLength, badVersion, badCount, unaligned, outOfRegion, overlap, tooBig };

static inline const char *parseName(Parse p) {
  switch (p) {
    case Parse::ok: return "ok"; case Parse::absent: return "absent"; case Parse::badLength: return "bad length";
    case Parse::badVersion: return "bad version"; case Parse::badCount: return "bad count"; case Parse::unaligned: return "unaligned";
    case Parse::outOfRegion: return "outside region1"; case Parse::overlap: return "overlap"; case Parse::tooBig: return "too big";
  }
  return "?";
}

static inline uint64_t le64(const uint8_t *p) {
  uint64_t v = 0; for (int i = 7; i >= 0; i--) { v = (v << 8) | p[i]; } return v;
}

/* The /chosen piano-extra-ram bytes (nullptr: absent) -> runs. Refuses anything DarwinBoot would never write. A count of 0
 * (DarwinBoot found nothing, or every run was refused) is ok with n = 0. */
static inline Parse parseProperty(const uint8_t *p, size_t len, Run out[kMaxRuns], unsigned &n) {
  n = 0;
  if (!p) { return Parse::absent; }
  if (len != kPropBytes) { return Parse::badLength; }
  uint32_t version = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
  uint32_t count = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
  if (version != 1) { return Parse::badVersion; }
  if (count > kMaxRuns) { return Parse::badCount; }
  uint64_t slabs = 0;
  for (unsigned i = 0; i < count; i++) {
    Run r = {le64(p + 8 + 16 * i), le64(p + 16 + 16 * i)};
    if (!r.size || (r.base | r.size) & (kSlab - 1)) { return Parse::unaligned; }
    if (r.base < kRegionLo || r.size > kRegionHi - r.base) { return Parse::outOfRegion; }
    if (i && r.base < out[i - 1].base + out[i - 1].size) { return Parse::overlap; }
    slabs += r.size / kSlab;
    if (slabs > kMaxSlabs) { return Parse::tooBig; }
    out[i] = r;
  }
  for (unsigned i = count; i < kMaxRuns; i++) {           /* unused pairs are zero */
    if (le64(p + 8 + 16 * i) || le64(p + 16 + 16 * i)) { return Parse::badCount; }
  }
  n = count;
  return Parse::ok;
}

/* The off switch: on only for the token piano-gpu-tier=1 in the boot arguments (the last one wins); default off. */
static inline bool bootArgOn(const char *args) {
  static const char key[] = "piano-gpu-tier=";
  bool on = false;
  if (!args) { return false; }
  for (const char *s = args; *s; ) {
    while (*s == ' ') { s++; }
    const char *t = s; while (*t && *t != ' ') { t++; }
    size_t k = sizeof key - 1;
    if ((size_t)(t - s) == k + 1 && __builtin_memcmp(s, key, k) == 0) { on = s[k] == '1'; }
    else if ((size_t)(t - s) > k && __builtin_memcmp(s, key, k) == 0) { on = false; }
    s = t;
  }
  return on;
}

/* Tier spans as the BO's piece list (NativeAdrenoPhysicalSpan {offset, physical, bytes}: the scattered-BO form poolTakeMany
 * fills in NativeAdrenoMemory.cpp): offsets are the running sum inside the BO. */
template <class Piece> static inline void asPieces(const Span *s, unsigned n, Piece *out) {
  uint64_t off = 0;
  for (unsigned i = 0; i < n; i++) { out[i].offset = off; out[i].physical = s[i].pa; out[i].bytes = s[i].bytes; off += s[i].bytes; }
}

class Tier {
 public:
  using ZeroFn = void (*)(uint64_t pa, uint64_t bytes, void *ctx);
  using ProbeFn = bool (*)(uint64_t pa, void *ctx);

  /* Takes the runs and self-tests each (probe of its first and last slab); a failed run is kept out (all its slabs used).
   * lines receives one line per run: "PianoA830: gpu tier: run I 0xBASE+N MiB self-test ok|FAIL". */
  bool init(const Run *runs, unsigned n, ZeroFn zero, ProbeFn probe, void *ctx, void (*line)(const char *, void *)) {
    *this = Tier();
    zero_ = zero; probe_ = probe; ctx_ = ctx;
    if (!zero || !probe || n > kMaxRuns) { return false; }
    for (unsigned i = 0; i < n; i++) {
      unsigned s = (unsigned)(runs[i].size / kSlab);
      if (slabs_ + s > kMaxSlabs) { return false; }
      runs_[i] = runs[i]; first_[i] = slabs_; slabs_ += s; nRuns_ = i + 1;
      bool ok = probe(runs[i].base, ctx) && probe(runs[i].base + runs[i].size - kSlab, ctx);
      ok_[i] = ok;
      if (!ok) { for (unsigned k = first_[i]; k < slabs_; k++) { set(k); } }
      else { total_ += runs[i].size; }
      if (line) {
        char b[112];
        snprintf(b, sizeof b, "PianoA830: gpu tier: run %u 0x%llx+%llu MiB self-test %s", i, (unsigned long long)runs[i].base,
                 (unsigned long long)(runs[i].size >> 20), ok ? "ok" : "FAIL");
        line(b, ctx);
      }
    }
    free_ = total_;
    return true;
  }

  /* bytes rounded up to slabs, as up to maxSpans spans (each contiguous inside one run, lowest first); every span zeroed.
   * Returns the span count, 0 when the tier cannot hold it whole (nothing taken: the caller uses XNU pages). */
  unsigned alloc(uint64_t bytes, Span *out, unsigned maxSpans) {
    if (!bytes || !maxSpans || !nRuns_) { return 0; }
    uint64_t need = (bytes + kSlab - 1) / kSlab;
    if (need * kSlab > free_) { return 0; }
    unsigned n = 0; uint64_t got = 0;
    for (unsigned r = 0; r < nRuns_ && got < need; r++) {
      unsigned end = first_[r] + (unsigned)(runs_[r].size / kSlab);
      for (unsigned k = first_[r]; k < end && got < need; ) {
        if (isSet(k)) { k++; continue; }
        unsigned j = k; while (j < end && !isSet(j) && got + (j - k) < need) { j++; }
        if (n == maxSpans) { undo(out, n); return 0; }
        out[n].pa = runs_[r].base + (uint64_t)(k - first_[r]) * kSlab; out[n].bytes = (uint64_t)(j - k) * kSlab;
        for (unsigned m = k; m < j; m++) { set(m); }
        got += j - k; n++; k = j;
      }
    }
    if (got < need) { undo(out, n); return 0; }
    for (unsigned i = 0; i < n; i++) { zero_(out[i].pa, out[i].bytes, ctx_); }
    free_ -= need * kSlab;
    return n;
  }

  /* Gives spans back; refuses (and changes nothing) a span that is not whole tier slabs all in use. */
  bool release(const Span *s, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
      unsigned k; uint64_t c;
      if (!locate(s[i], k, c)) { return false; }
      for (uint64_t m = 0; m < c; m++) { if (!isSet(k + (unsigned)m)) { return false; } }
    }
    for (unsigned i = 0; i < n; i++) {
      unsigned k; uint64_t c; locate(s[i], k, c);
      for (uint64_t m = 0; m < c; m++) { clear(k + (unsigned)m); }
      free_ += c * kSlab;
    }
    return true;
  }

  bool owns(uint64_t pa) const {
    for (unsigned r = 0; r < nRuns_; r++) { if (ok_[r] && pa >= runs_[r].base && pa - runs_[r].base < runs_[r].size) { return true; } }
    return false;
  }
  uint64_t totalBytes() const { return total_; }
  uint64_t freeBytes() const { return free_; }
  unsigned runs() const { return nRuns_; }

 private:
  bool locate(const Span &s, unsigned &k, uint64_t &c) const {
    if (!s.bytes || (s.pa | s.bytes) & (kSlab - 1)) { return false; }
    for (unsigned r = 0; r < nRuns_; r++) {
      if (ok_[r] && s.pa >= runs_[r].base && s.pa - runs_[r].base < runs_[r].size && s.bytes <= runs_[r].base + runs_[r].size - s.pa) {
        k = first_[r] + (unsigned)((s.pa - runs_[r].base) / kSlab); c = s.bytes / kSlab; return true;
      }
    }
    return false;
  }
  void undo(const Span *s, unsigned n) {
    for (unsigned i = 0; i < n; i++) { unsigned k; uint64_t c; if (locate(s[i], k, c)) { for (uint64_t m = 0; m < c; m++) { clear(k + (unsigned)m); } } }
  }
  bool isSet(unsigned k) const { return bits_[k / 64] >> (k % 64) & 1; }
  void set(unsigned k) { bits_[k / 64] |= 1ull << (k % 64); }
  void clear(unsigned k) { bits_[k / 64] &= ~(1ull << (k % 64)); }

  Run runs_[kMaxRuns] = {};
  unsigned first_[kMaxRuns] = {};
  bool ok_[kMaxRuns] = {};
  unsigned nRuns_ = 0, slabs_ = 0;
  uint64_t bits_[kMaxSlabs / 64] = {};
  uint64_t total_ = 0, free_ = 0;
  ZeroFn zero_ = nullptr; ProbeFn probe_ = nullptr; void *ctx_ = nullptr;
};

}  // namespace a830tier
#endif
