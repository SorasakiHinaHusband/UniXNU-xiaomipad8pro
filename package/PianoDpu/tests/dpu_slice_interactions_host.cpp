// SPDX-License-Identifier: BSD-2-Clause-Patent
// Startup policy and serialized M2/M3/M4 transactions on the timed DPU model.
// Build: c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I.. dpu_slice_interactions_host.cpp -o /tmp/dpu_slice_interactions_host && /tmp/dpu_slice_interactions_host
#include "DpuClockRead.h"
#include "DpuFlip.h"
#include "DpuModeSwitch.h"
#include "DpuNightShift.h"
#include "DpuSliceArgs.h"
#include "DpuSnapshot.h"
#include "dpu_model.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace pianodpu;
using namespace pianodpu::slices;

namespace {

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::exit(1); } } while (0)

struct BootArgs {
  std::map<std::string, uint32_t> numbers;
  const char *refresh = nullptr;

  uint32_t number(const char *name, uint32_t fallback) const
  {
    const auto it = numbers.find(name);
    return it == numbers.end() ? fallback : it->second;
  }

  SliceArgs slices() const
  {
    return readStartupArgs(
        [&](const char *name, uint32_t *value, size_t size) {
          CHECK(size == sizeof *value);
          const auto it = numbers.find(name);
          if (it == numbers.end()) return false;
          *value = it->second;
          return true;
        },
        [&](const char *name, char *value, size_t size) {
          if (!refresh || std::strcmp(name, "piano-dpu-refresh") != 0) return false;
          CHECK(std::strlen(refresh) < size);
          std::strcpy(value, refresh);
          return true;
        });
  }

  bool cleanSkip(uint32_t flipLevel) const
  {
    return flip::readCleanSkip(flipLevel, [&](const char *name, uint32_t fallback) { return number(name, fallback); });
  }
};

void testStartup()
{
  const struct {
    bool panel, pcc, modes;
    unsigned ahbVote;
  } cases[] = {
    {false, false, false, 0},
    {true,  false, false, 1},
    {false, true,  false, 0},
    {false, false, true,  1},
    {true,  true,  false, 1},
    {true,  false, true,  1},
    {false, true,  true,  1},
    {true,  true,  true,  1},
  };
  for (unsigned level : {1u, 2u, 3u}) {
    for (const auto &row : cases) {
      BootArgs boot{{{"piano-dpu", level}}};
      if (row.panel) boot.numbers["piano-dpu-panel"] = 1;
      if (row.pcc) boot.numbers["piano-dpu-pcc"] = 1;
      if (row.modes) boot.numbers["piano-dpu-modes"] = 1;
      for (int explicitVote : {-1, 0, 1, 2}) {
        if (explicitVote >= 0) boot.numbers["piano-dpu-ahb-vote"] = unsigned(explicitVote);
        const auto args = boot.slices();
        CHECK(args.level == level && args.refresh == kRefreshOff && !args.nightShift);
        CHECK(args.panel == row.panel && args.panelTrace == row.panel && args.pcc == row.pcc);
        CHECK(args.modeHz == (row.modes ? 60u : 0u) && args.modesEnabled() == row.modes);
        CHECK(args.wanted() == (row.panel || row.pcc || row.modes));
        const unsigned expectedVote = explicitVote >= 0 ? unsigned(explicitVote) : level >= 2 ? row.ahbVote : 0;
        CHECK(args.ahbVote == expectedVote);
        CHECK(!boot.cleanSkip(1) && !boot.cleanSkip(4));
      }
    }
  }

  BootArgs boot{{{"piano-dpu", 2}, {"piano-dpu-pcc", 1}, {"native-gop-display-night-shift", 1}}};
  auto args = boot.slices();
  CHECK(args.nightShift && args.wanted() && args.pcc == 0 && args.ahbVote == 1);
  boot.numbers["native-gop-display-night-shift"] = 2;
  args = boot.slices();
  CHECK(!args.nightShift && args.pcc == 1 && args.ahbVote == 0);

  boot = BootArgs{{{"piano-dpu", 2}, {"piano-dpu-modes", 1}}};
  boot.refresh = "plan";
  args = boot.slices();
  CHECK(args.modeHz == 60 && !args.modesEnabled() && args.refresh == kRefreshPlan && args.ahbVote == 0);
  boot.refresh = nullptr;
  boot.numbers["piano-dpu-refresh-db"] = 0;
  args = boot.slices();
  CHECK(!args.modesEnabled() && !args.wanted() && args.ahbVote == 0);
  boot.numbers.erase("piano-dpu-refresh-db");
  for (unsigned hz : {0u, 60u, 90u, 120u, 144u, UINT32_MAX}) {
    boot.numbers["piano-dpu-modes"] = hz;
    args = boot.slices();
    const bool qualified = hz == 120 || hz == 144;
    CHECK(args.modeHz == (qualified ? hz : 0));
    CHECK(args.modesEnabled() == qualified && args.ahbVote == unsigned(qualified));
  }

  boot = BootArgs{{{"piano-dpu", 2}, {"piano-dpu-clean-skip", 1}}};
  for (unsigned level : {1u, 2u, 3u, 4u}) CHECK(boot.cleanSkip(level) == (level == 4));
  for (unsigned invalid : {0u, 2u, UINT32_MAX}) {
    boot.numbers["piano-dpu-clean-skip"] = invalid;
    CHECK(!boot.cleanSkip(4));
  }
}

struct Output {
  std::string text;

  void line(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    char row[480];
    va_list args;
    va_start(args, fmt);
    const int len = std::vsnprintf(row, sizeof row, fmt, args);
    va_end(args);
    CHECK(len >= 0 && size_t(len) < sizeof row);
    text.append(row, size_t(len)).append("\n");
  }
};

struct Capture {
  snapshot::Snapshot state;
  std::vector<uint64_t> loads;
};

Capture takeReadOnlySnapshot(model::Dpu &io, bool hwGatedAhb, bool clockRead = true)
{
  const auto stores = io.stores.size(), refused = io.refused.size(), votes = io.bandwidthVotes.size();
  Capture capture;
  const auto load = [&](uint64_t pa) {
    capture.loads.push_back(pa);
    return io.rd(pa);
  };
  snapshot::take(capture.state, load, hwGatedAhb, true);
  CHECK(capture.state.loads == capture.loads.size());
  Output out;
  snapshot::format(capture.state, 1, io.nowUs(), out);
  CHECK(out.text.find("loads, 0 stores") != std::string::npos);
  if (clockRead) {
    const auto clocks = clkread::capture(capture.state.path.clocks, load);
    clkread::formatReadback(clocks, true, out, [](Output &) { CHECK(false); });
    CHECK(out.text.find("clkread_mdp status=") != std::string::npos);
    CHECK(out.text.find("clkread_rpmh status=unavailable (ACTIVE transport requires stores)") != std::string::npos);
  }
  CHECK(io.stores.size() == stores && io.refused.size() == refused && io.bandwidthVotes.size() == votes);
  return capture;
}

void testSnapshotGates()
{
  constexpr uint32_t off = 1u << 31;
  const struct {
    uint64_t pa;
    uint32_t value;
    bool hwGatedAhb, readable;
    int unclockedLink;
  } cases[] = {
    {kGcc + kGccDispAhb, 1, false, true, -1},
    {kGcc + kGccDispAhb, off | 3, true, true, -1},
    {kGcc + kGccDispAhb, off | 3, false, false, -1},
    {kGcc + kGccDispAhb, off, true, false, -1},
    {kDispCc + kCcMdssGdsc, 0, true, false, -1},
    {kDispCc + kCcAhb, off, true, false, -1},
    {kDispCc + kCcMdp, off, true, false, -1},
    {kDispCc + kCcByte0, off, true, true, 0},
    {kDispCc + kCcPclk0, off, true, true, 0},
    {kDispCc + kCcByte1, off, true, true, 1},
    {kDispCc + kCcPclk1, off, true, true, 1},
  };
  for (bool parked : {false, true}) {
    for (const auto &row : cases) {
      auto io = model::modeSwitchFixture();
      for (unsigned l = 0; l < 2; ++l) {
        io.regs[kDispCc + (l ? kCcByte1 : kCcByte0)] = 1;
        io.regs[kDispCc + (l ? kCcPclk1 : kCcPclk0)] = 1;
      }
      if (parked) {
        io.regs[ctlReg(0, kCtlIntfActive)] = kDsiIntfSlave;
        io.regs[ctlReg(0, kCtlIntfMaster)] = 0;
        io.regs[intfReg(1, kIntfEn)] = 0;
        io.intf[1].on = false;
        io.latchedMaster = 0;
      }
      io.regs[row.pa] = row.value;
      for (bool clockRead : {false, true}) {
        const auto capture = takeReadOnlySnapshot(io, row.hwGatedAhb, clockRead);
        const auto &s = capture.state;
        CHECK(s.readable == row.readable);
        const bool routed = row.readable && row.unclockedLink < 0;
        CHECK(s.path.ok == (routed && !parked));
        if (routed) {
          CHECK(s.path.ctl == 0 && s.path.intfActive == (parked ? kDsiIntfSlave : kIntfFlushDsi));
          CHECK(s.mode.hz == 120);
        }
        for (unsigned l = 0; l < 2; ++l) {
          const bool readable = row.readable && int(l) != row.unclockedLink;
          CHECK(s.dsi[l].read == readable && s.m5Link[l].read == readable);
          if (!readable) {
            for (uint64_t pa : capture.loads) {
              CHECK(pa < kDsi[l] || pa >= kDsi[l] + kDsiBytes);
              CHECK(pa < kDsiPhy[l] || pa >= kDsiPhy[l] + kDsiPhyBytes);
              CHECK(pa < kDsiPll[l] || pa >= kDsiPll[l] + kDsiPllBytes);
            }
          }
        }
        if (!row.readable)
          for (uint64_t pa : capture.loads) CHECK(pa < kMdss || pa >= kMdp + kMdpBytes);
      }
    }
  }
  // Clock readback alone still allows the ACTIVE read, so the suppression check is observable.
  unsigned rpmhReads = 0;
  Output out;
  clkread::formatReadback(clkread::Clocks{}, false, out, [&](Output &) { ++rpmhReads; });
  CHECK(rpmhReads == 1 && out.text.find("clkread_rpmh status=unavailable") == std::string::npos);
}

void checkTiming(const snapshot::Snapshot &s, unsigned hz, unsigned htotal, unsigned vtotal)
{
  // The fixture's inherited start (0x0013b6c1) is on line 2243 of 2310.
  constexpr unsigned fetchLead = 67;
  const auto *target = modes::modeByHz(hz);
  CHECK(target);
  CHECK(s.path.ok && s.mode.hz == hz && s.ctlFlush == 0);
  for (unsigned l = 0; l < 2; ++l) {
    const auto plan = modes::plan(modes::geometry(s.live[l]), *target);
    CHECK(plan.hz == hz && plan.htotal == htotal && plan.vtotal == vtotal && plan.vfp == vtotal - 2242);
    CHECK(plan.pixelClockHz == 159667200 && plan.period == htotal * vtotal);
    CHECK(plan.progFetch == (vtotal - fetchLead) * htotal + 1);
    CHECK(plan.progFetch > plan.dispVEnd && plan.progFetch < plan.period);
    CHECK(s.dsi[l].total == ((vtotal - 1) << 16 | (htotal - 1)));
    CHECK(!modes::firstDifference(plan, s.live[l]));
  }
}

void testDailyMode()
{
  const BootArgs boot{{{"piano-dpu", 2}}};
  const auto args = boot.slices();
  CHECK(!args.wanted() && !args.panel && !args.nightShift && !boot.cleanSkip(4));
  auto io = model::modeSwitchFixture();
  CHECK(!modes::switchTiming(io, 60, {}).why);  // Model the 60 Hz scanout already in use.
  const auto before = io.stores.size();
  const auto unexpected = [] { CHECK(false); };
  for (unsigned hz : {60u, 90u, 120u, 144u}) {
    const auto result = modes::switchTo(io, hz, {.fixedHz = args.modeHz}, unexpected, unexpected);
    CHECK(result.why && !result.timingTouched);
  }
  checkTiming(takeReadOnlySnapshot(io, false).state, 60, 576, 4620);
  CHECK(io.stores.size() == before && io.dcs.empty());
}

void testInterleavedSlices()
{
  const BootArgs boot{{{"piano-dpu", 2}, {"piano-dpu-clean-skip", 1},
                       {"piano-dpu-modes", 1}, {"native-gop-display-night-shift", 1}}};
  const auto args = boot.slices();
  CHECK(args.nightShift && args.modesEnabled() && args.ahbVote == 1 && !args.panel);

  auto io = model::modeSwitchFixture();
  io.resetPccOnTimingChange = true;
  const uint32_t baseDiagonal[2][3] = {{0x8000, 0x6000, 0x4000}, {0x6000, 0x8000, 0x8000}};
  for (unsigned d = 0; d < 2; ++d) {
    io.pccShadow[d][0] = io.pccActive[d][0] = 1;
    for (unsigned c = 0; c < 3; ++c) {
      const unsigned word = kPccCoef[c][c] / sizeof(uint32_t);
      io.pccShadow[d][word] = io.pccActive[d][word] = baseDiagonal[d][c];
    }
  }
  const bool hwGatedAhb = args.ahbVote == 1;
  pcc::PanelPcc base;
  CHECK(pcc::capturePanelPcc(io, readPath(io, hwGatedAhb), base));
  auto last = base;
  pcc::NightShift nightShift;
  const uint64_t off[9] = {1ull << 32, 0, 0, 0, 1ull << 31, 0, 0, 0, 1ull << 31};
  const uint64_t warm[9] = {1ull << 32, 0, 0, 0, 1ull << 30, 0, 0, 0, 1ull << 29};
  CHECK(nightShift.setMatrix(off) == pcc::NightShift::Input::accepted && nightShift.neutral());
  const auto applyColor = [&](bool replay) {
    return pcc::updateNightShift(io, readPath(io, hwGatedAhb), pcc::nightShiftPrograms(base, nightShift), last, replay);
  };
  CHECK(!applyColor(false).attempted && io.stores.empty());

  struct FlipHardware {} hw;
  flip::Flipper flipper(hw);
  flipper.cfg.level = 4;
  flipper.cfg.cleanSkip = boot.cleanSkip(flipper.cfg.level);
  CHECK(flipper.cfg.cleanSkip);
  using Action = flip::CleanEpoch::Action;
  unsigned cleans = 0, barriers = 0;
  const auto prepare = [&](unsigned slot) {
    const auto verifiedWc = [] { return flip::cleanEligibility(flip::kSurfaceWriteCombined, {4, 0, 0}); };
    return flipper.cleanEpochs[slot].prepare(verifiedWc(), [&](bool whole) {
      CHECK(whole);
      ++cleans;
      return true;
    }, verifiedWc, [&] { ++barriers; }).action;
  };
  CHECK(prepare(0) == Action::initialized && prepare(1) == Action::initialized);

  const struct {
    unsigned hz, htotal, vtotal;
    bool warm;
    unsigned correctionDivisor;
  } steps[] = {
    {60,  576, 4620, true,  1},
    {90,  768, 2310, false, 1},
    {144, 480, 2310, true,  2},
    {120, 576, 2310, false, 1},
  };
  constexpr unsigned rounds = 8;
  unsigned invalidations = 0, replays = 0, transitions = 0;
  for (unsigned round = 0; round < rounds; ++round) {
    for (const auto &step : steps) {
      CHECK(nightShift.setMatrix(step.warm ? warm : off) == pcc::NightShift::Input::accepted);
      nightShift.setCorrection(pcc::kOne / step.correctionDivisor);
      CHECK(nightShift.neutral() == !step.warm);
      const auto update = applyColor(false);
      CHECK(update.attempted && update.result.ok);
      const auto before = io.stores.size();
      CHECK(!applyColor(false).attempted && io.stores.size() == before);
      CHECK(prepare(0) == Action::skipped && prepare(1) == Action::skipped);

      // The timed model exercises all four plans. Production fixes the requested rate at boot.
      const modes::Options modelOptions{.hwGatedAhb = hwGatedAhb, .allowUnqualified = true};
      const auto unexpected = [] { CHECK(false); };
      const auto time = io.ns;
      for (unsigned bad : {0u, 1u, 59u, 61u, 89u, 91u, 119u, 121u, 143u, 145u, UINT32_MAX}) {
        CHECK(!modes::modeByHz(bad));
        const auto refused = modes::switchTo(io, bad, modelOptions, unexpected, unexpected);
        CHECK(refused.why && !refused.timingTouched && !refused.reverted);
      }
      CHECK(io.stores.size() == before && io.ns == time);

      ++transitions;
      const auto switched = modes::switchTo(io, step.hz, modelOptions, [&] {
        ++invalidations;
        flipper.invalidateCleanEpochs();
      }, [&] {
        ++replays;
        CHECK(invalidations == transitions && replays == transitions);
        CHECK(!flipper.cleanEpochs[0].initialized && !flipper.cleanEpochs[1].initialized);
        CHECK(!io.pccActive[0][0] && !io.pccActive[1][0]);
        CHECK(applyColor(true).result.ok);
      });
      if (switched.why) std::fprintf(stderr, "switch %u -> %u Hz: %s\n", switched.from, step.hz, switched.why);
      CHECK(!switched.why && switched.timingTouched && !switched.reverted && switched.rate.lockstep);
      CHECK(switched.rate.milliHz >= step.hz * 980 && switched.rate.milliHz <= step.hz * 1020);

      const auto s = takeReadOnlySnapshot(io, hwGatedAhb).state;
      checkTiming(s, step.hz, step.htotal, step.vtotal);
      for (unsigned d = 0; d < 2; ++d) {
        CHECK(io.pccActive[d][0] == 1 && s.dspp[d].pcc[0] == 1);
        for (unsigned o = 0; o < 3; ++o) {
          for (unsigned i = 0; i < 3; ++i) {
            const unsigned index = kPccCoef[o][i] / sizeof(uint32_t);
            const uint32_t expected = o != i ? 0 : baseDiagonal[d][o] / ((step.warm ? 1u << o : 1u) * step.correctionDivisor);
            CHECK(io.pccActive[d][index] == expected && s.dspp[d].pcc[index] == expected);
          }
        }
      }
      CHECK(prepare(0) == Action::initialized && prepare(1) == Action::initialized);
    }
  }
  CHECK(transitions == rounds * 4 && replays == transitions && invalidations == transitions);
  CHECK(cleans == 2 * (transitions + 1) && barriers == 2 * transitions);
  CHECK(io.glitches == 0 && io.desync == 0 && io.commandsInFrame == 0 && !io.modeUnderruns());
  CHECK(io.refused.empty() && io.irqDepth == 0);
}

}  // namespace

int main()
{
  testStartup();
  testSnapshotGates();
  testDailyMode();
  testInterleavedSlices();
  std::puts("PianoDpu startup and slice interactions: all passed");
}
