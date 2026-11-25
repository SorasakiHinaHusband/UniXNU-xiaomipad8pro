// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoDpu B1e (DpuPanel.h) on the timed register model (dpu_model.h):
//   - the DCS short packet and its FIFO words; one command on both links: the stores in order, low power, sent in the vertical
//     blanking (by the hardware's block or, without it, triggered in the front porch), every word restored as found; the
//     refusals (not video mode, the FIFO in use, no software trigger, a command that is not a sleep command, a busy DMA);
//   - counter setup from disabled, ten cycles with advancing counters despite zero COUNT_EN, and the logged refusal when stuck;
//   - y12n's slave timeout and recovery with both possible enable readbacks, the trace rows, and poll timing with tracing on/off;
//   - display sleep: 0x28, >= 20 ms, 0x10, >= 100 ms, INTF2 stopped before INTF1, both stopped, the panel halves asleep; wake:
//     the INTF flush, INTF1 and INTF2 started together and in phase, counting in lockstep, 0x11, >= 120 ms, 0x29; 50 cycles;
//   - the failures: a DMA that never finishes (the link left alone, nothing after it), an engine that does not stop (the panel
//     brought back), an engine that does not start; DCS only; high speed;
//   - nothing written outside the guard's registers: never TLMM (the panel's reset and supply pins).
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. dpu_panel_host.cpp -o /tmp/dpu_panel_host && /tmp/dpu_panel_host
#include "DpuPanel.h"
#include "dpu_model.h"
#include "../../NativeGopDisplay/GopCompose.h"
#include "../../NativeGopDisplay/GopPanelPower.h"
#include <cstdio>
#include <cstring>

using namespace pianodpu;
using namespace pianodpu::panel;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static bool clean(const model::Dpu &m)
{
  return m.refused.empty() && !m.anyStoreIn(kTlmm, 0x200000) && !m.anyStoreIn(kDsiPhy[0], 0x1000) && !m.anyStoreIn(kDsiPhy[1], 0x1000) &&
         !m.anyStoreIn(kMdp + 0x4000, 0x8000) && !m.anyStoreIn(kMdp + 0x24000, 0xc000) && !m.anyStoreIn(kMdp + kLmBase, 0x8000);
}

static void testPacket()
{
  const Packet p = dcsShort(kDcsDisplayOff);
  CHECK(p.words[0] == 0x80050028u && p.words[1] == 0 && p.count == 2 && p.bytes == 4);
  CHECK(dcsShort(kDcsSleepOut).words[0] == 0x80050011u);
  CHECK(dcsAllowed(0x28) && dcsAllowed(0x10) && dcsAllowed(0x11) && dcsAllowed(0x29) && !dcsAllowed(0x51) && !dcsAllowed(0x01));
}

static void testSend()
{
  model::Dpu m;
  m.sleepMs(3);
  uint32_t found[2][4];
  for (unsigned l = 0; l < 2; ++l) {
    m.regs[dsiReg(l, kDsiAckErr)] = 0x10u << l;
    found[l][0] = m.rd(dsiReg(l, kDsiCtrl));
    found[l][1] = m.rd(dsiReg(l, kDsiCmdDmaCtrl));
    found[l][2] = m.rd(dsiReg(l, kDsiDmaCtrl1));
    found[l][3] = m.rd(dsiReg(l, kDsiDmaLength));
  }
  const size_t before = m.stores.size();
  Sent s = sendDcs(m, kDcsDisplayOff, Options());
  CHECK(s.ok && s.done[0] && s.done[1] && !s.waitedBlanking && s.refused == 0);
  CHECK(m.dcs.size() == 2 && m.dcs[0].command == 0x28 && m.dcs[1].command == 0x28 && m.dcs[0].lowPower && m.dcs[1].lowPower);
  CHECK(m.commandsInFrame == 0 && !m.half[0].displayOn && !m.half[1].displayOn);
  CHECK(s.ackBeforeMask == 3 && s.ackAfterMask == 3);
  for (unsigned l = 0; l < 2; ++l) {   // every word as found, the FIFO free, the done bit clear
    CHECK(s.before[l].ack == (0x10u << l) && s.ackAfter[l] == s.before[l].ack);
    CHECK(m.rd(dsiReg(l, kDsiCtrl)) == found[l][0] && m.rd(dsiReg(l, kDsiCmdDmaCtrl)) == found[l][1]);
    CHECK(m.rd(dsiReg(l, kDsiDmaCtrl1)) == found[l][2] && m.rd(dsiReg(l, kDsiDmaLength)) == found[l][3]);
    CHECK(m.rd(dsiReg(l, kDsiTpgCtrl)) == 0 && (m.rd(dsiReg(l, kDsiIntrCtrl)) & 1u) == 0 && (m.rd(dsiReg(l, kDsiIntrCtrl)) & 2u));
  }
  // DSI0's preparation in order, DSI1's, then the two triggers back to back
  const uint64_t prep[] = {kDsiIntrCtrl, kDsiCtrl, kDsiTpgCtrl, kDsiTpgFifo, kDsiTpgFifo, kDsiCmdDmaCtrl, kDsiDmaCtrl1, kDsiDmaLength};
  for (unsigned l = 0; l < 2; ++l)
    for (unsigned k = 0; k < 8; ++k) CHECK(m.stores[before + 8 * l + k].pa == dsiReg(l, uint32_t(prep[k])));
  CHECK(m.stores[before + 1].v == (found[0][0] | 4u) && m.stores[before + 2].v == 0x30006u && m.stores[before + 3].v == 0x80050028u &&
        m.stores[before + 4].v == 0 && m.stores[before + 5].v == ((found[0][1] & ~0xc4000000u) | 0x14000000u) &&
        m.stores[before + 6].v == (found[0][2] & ~2u) && m.stores[before + 7].v == 4);
  CHECK(m.stores[before + 16].pa == dsiReg(0, kDsiTrigDma) && m.stores[before + 17].pa == dsiReg(1, kDsiTrigDma));
  CHECK(m.stores[before + 17].ns - m.stores[before + 16].ns < 1000);
  CHECK(m.storesTo(dsiReg(0, 0x1ec)) == 2 && m.firstStore(dsiReg(0, 0x1ec), 1) >= 0);   // PeripheralIO's FIFO reset pulse
  CHECK(clean(m));
  // high speed: the low-power bit clear
  Options hs;
  hs.lowPower = false;
  CHECK(sendDcs(m, kDcsDisplayOn, hs).ok && !m.dcs.back().lowPower && m.half[0].displayOn);
  // no hardware block within the frame: triggered in the front porch
  model::Dpu n;
  n.blockDma = false;
  n.sleepMs(3);
  s = sendDcs(n, kDcsDisplayOff, Options());
  CHECK(s.ok && s.waitedBlanking && n.commandsInFrame == 0 && n.dcs.size() == 2);
  CHECK(n.stores[n.firstStore(dsiReg(0, kDsiTrigDma), 1)].line >= kVActiveEnd);
}

static void testRefusals()
{
  {
    model::Dpu m;
    m.regs[dsiReg(1, kDsiCtrl)] = 0x1f5;   // DSI1 in command mode
    m.sleepMs(2);
    const Sent s = sendDcs(m, kDcsDisplayOff, Options());
    CHECK(!s.ok && std::strstr(s.why, "video mode") && m.stores.empty());
  }
  {
    model::Dpu m;
    m.regs[dsiReg(0, kDsiTpgCtrl)] = 0x30006;   // the FIFO in use
    CHECK(!sendDcs(m, kDcsDisplayOff, Options()).ok && m.stores.empty());
  }
  {
    model::Dpu m;
    CHECK(!sendDcs(m, 0x51, Options()).ok && m.stores.empty());   // brightness: not this slice's
  }
  {
    model::Dpu m;
    m.dsi[0].dmaPending = true;   // a DMA of someone else's that never ends
    m.dsi[0].dmaAt = UINT64_MAX;
    const Sent s = sendDcs(m, kDcsDisplayOff, Options());
    CHECK(!s.ok && std::strstr(s.why, "busy") && m.stores.empty());
  }
  {
    model::Dpu m;
    m.blockDma = false;
    m.regs[intfReg(1, kIntfHsyncCtl)] = 0;  // no usable porch geometry
    const Result r = panelOff(m, readPath(m), Options());
    CHECK(!r.ok && r.safeAwake && m.dcs.empty() && m.storesTo(dsiReg(0, kDsiTrigDma)) == 0 &&
          m.rd(dsiReg(0, kDsiTpgCtrl)) == 0);
    CHECK(!r.dcs[0].triggerAttempted && r.dcs[0].dmaDoneMask() == 0 && r.dcs[0].ackAfterMask == 3);
  }
}

static void testSleepWake()
{
  model::Dpu m;
  m.blockDma = false;
  constexpr uint32_t config = 0x80800000u;
  for (unsigned i = 1; i <= 2; ++i) {
    m.regs[intfReg(i, kIntfCountEn)] = 0;
    m.intf[i].counting = false;
    m.regs[intfReg(i, kIntfConfig)] = config;
    m.intf[i].fetch = m.regs[intfReg(i, kIntfProgFetch)] = (2290 + i) * model::kHtotal;
  }
  m.sleepMs(3);
  LivePath p = readPath(m);
  CHECK(p.ok && p.link[0].countEn == 0 && p.link[1].countEn == 0);
  const CounterInit init = initCounters(m, p);
  CHECK(init.enabled && init.moving && m.stores.size() == 2);
  CHECK(m.firstStore(intfReg(1, kIntfCountEn), 3) == 0 && m.firstStore(intfReg(2, kIntfCountEn), 3) == 1);
  Result off = panelOff(m, p, Options());
  CHECK(off.ok && off.enginesStopped && off.step == 5 && off.dcs[0].waitedBlanking);
  const model::Dcs *d28 = m.firstDcs(0x28, 0), *d10 = m.firstDcs(0x10, 0), *e28 = m.firstDcs(0x28, 1), *e10 = m.firstDcs(0x10, 1);
  CHECK(d28 && d10 && e28 && e10);
  if (d28 && d10) CHECK(d10->ns - d28->ns >= 20000000ull);
  const int stop2 = m.firstStore(intfReg(2, kIntfEn), 0), stop1 = m.firstStore(intfReg(1, kIntfEn), 0);
  CHECK(stop2 >= 0 && stop1 > stop2);                                  // the slave first
  for (unsigned i = 1; i <= 2; ++i) {
    const int disable = m.firstStore(intfReg(i, kIntfConfig), config & ~kIntfConfigProgFetch);
    CHECK(disable > stop1);
    CHECK(m.rd(intfReg(i, kIntfConfig)) == (config & ~kIntfConfigProgFetch));
    m.regs[intfReg(i, kIntfProgFetch)] = 0;   // prove the wake uses the saved registers
  }
  if (d10 && stop2 >= 0) CHECK(m.stores[size_t(stop2)].ns - d10->ns >= 100000000ull);   // 100 ms after sleep-in
  CHECK(!m.intf[1].on && !m.intf[2].on && m.half[0].sleeping && m.half[1].sleeping && !m.half[0].displayOn);
  const uint32_t f1 = m.intf[1].frames, f2 = m.intf[2].frames;
  m.sleepMs(1000);   // asleep: no frames, no fetch
  CHECK(m.intf[1].frames == f1 && m.intf[2].frames == f2 && m.rd(intfReg(1, kIntfStatus)) == 0);
  const size_t before = m.stores.size();
  Result on = panelOn(m, p, Options(), off.enginesStopped, off.fetch);
  CHECK(on.ok && on.engines.ok && on.engines.rate.lockstep && on.engines.latchUs >= 0);
  for (unsigned i = 1; i <= 2; ++i) {
    CHECK(m.stores[before + 2 * (i - 1)].pa == intfReg(i, kIntfProgFetch));
    CHECK(m.stores[before + 2 * (i - 1) + 1].pa == intfReg(i, kIntfConfig));
    CHECK(m.rd(intfReg(i, kIntfConfig)) == config && m.rd(intfReg(i, kIntfProgFetch)) == (2290 + i) * model::kHtotal);
  }
  CHECK(m.stores[before + 4].pa == ctlReg(0, kCtlIntfFlush) && m.stores[before + 5].pa == ctlReg(0, kCtlFlush));
  CHECK(m.stores[before + 6].pa == intfReg(1, kIntfEn) && m.stores[before + 7].pa == intfReg(2, kIntfEn));
  CHECK(m.stores[before + 7].ns - m.stores[before + 6].ns < 1000 && m.desync == 0);
  CHECK(m.intf[1].on && m.intf[2].on && m.intf[1].frames > f1 && m.intf[1].frames - f1 == m.intf[2].frames - f2);
  const model::Dcs *d11 = m.firstDcs(0x11, 0), *d29 = nullptr;
  for (const model::Dcs &x : m.dcs)
    if (x.command == 0x29 && x.link == 1 && d11 && x.ns > d11->ns) d29 = &x;
  CHECK(d11 && d29);
  if (d11 && d29) CHECK(d29->ns - d11->ns >= 120000000ull);
  CHECK(!m.half[0].sleeping && !m.half[1].sleeping && m.half[0].displayOn && m.half[1].displayOn);
  CHECK(m.glitches == 0 && clean(m));
  // 50 cycles
  using namespace gopcompose;
  PanelPower power;
  Slot slot;
  Busy busy;
  Job shown;
  unsigned ok = 0;
  for (unsigned k = 0; k < 50; ++k) {
    m.sleepMs(3 * k % 17);
    Job flip;
    flip.serial = 2 * k + 1;
    flip.flip = true;
    flip.kind = Job::kFull;
    CHECK(offer(slot, flip, nullptr) == Offer::Taken && takeJob(slot, busy, &shown));
    CHECK(power.request(false) && !power.canPresent());
    uint64_t done = 0;
    CHECK(finishJob(busy, &done, flip.serial) && done == flip.serial);  // drain the serving flip before DCS
    bool want;
    CHECK(power.begin(want) && !want);
    const Result sleep = panelOff(m, p, Options());
    ok += sleep.ok;
    power.finish(false, sleep.ok);
    flip.serial++;
    const Admission held = admitSwap(slot, busy, flip, LayerChange(), kDeferAlways);
    CHECK(held.hadJob && held.offer == Offer::Taken && held.waitSerial == flip.serial);
    const uint32_t asleepFrame = m.intf[1].frames;
    m.sleepMs(500);
    CHECK(!power.canPresent() && slot.busy && m.intf[1].frames == asleepFrame);
    CHECK(power.request(true) && power.begin(want) && want && !power.canPresent());
    const Result wake = panelOn(m, p, Options(), sleep.enginesStopped, sleep.fetch);
    ok += wake.ok;
    power.finish(true, wake.ok);
    CHECK(power.wakeComplete && power.canPresent() && takeJob(slot, busy, &shown) && shown.serial == flip.serial && shown.flip);
    CHECK(finishJob(busy, &done, flip.serial) && done == flip.serial);
  }
  CHECK(ok == 100 && m.desync == 0 && m.glitches == 0 && m.commandsInFrame == 0 && m.half[0].displayOn && clean(m));
}

static void testZeroCounterReadback()
{
  model::Dpu m;
  m.blockDma = false;
  for (unsigned i = 1; i <= 2; ++i) {
    m.regs[intfReg(i, kIntfCountEn)] = 0;
    m.countEnableIgnored[i - 1] = true;
    m.regs[intfReg(i, kIntfConfig)] = kIntfConfigProgFetch;
    m.intf[i].fetch = m.regs[intfReg(i, kIntfProgFetch)] = 2290 * model::kHtotal;   // the merged model (M3) keeps the fetch per INTF
  }
  LivePath p = readPath(m);
  const CounterInit init = initCounters(m, p);
  CHECK(p.ok && !init.enabled && init.moving && m.stores.size() == 2);
  CHECK(p.link[0].countEn == 0 && p.link[1].countEn == 0);
  for (unsigned k = 0; k < 10; ++k) {
    const Result off = panelOff(m, p, Options());
    CHECK(off.ok && off.enginesStopped && off.dcs[0].waitedBlanking);
    const Frames asleep = readFrames(m);
    m.sleepMs(100);
    const Frames after = readFrames(m);
    for (unsigned i = 1; i <= 2; ++i) {
      CHECK(m.rd(intfReg(i, kIntfEn)) == 0 && m.rd(intfReg(i, kIntfStatus)) == 0 &&
            !(m.rd(intfReg(i, kIntfConfig)) & kIntfConfigProgFetch));
      CHECK(after.f[i - 1] == asleep.f[i - 1]);
    }
    const Result on = panelOn(m, p, Options(), off.enginesStopped, off.fetch);
    CHECK(on.ok && on.engines.rate.lockstep);
    for (unsigned i = 1; i <= 2; ++i) {
      CHECK(m.rd(intfReg(i, kIntfEn)) == 1 && m.rd(intfReg(i, kIntfStatus)) == 1 &&
            (m.rd(intfReg(i, kIntfConfig)) & kIntfConfigProgFetch));
      CHECK(m.rd(intfReg(i, kIntfFrameCount)) > asleep.f[i - 1] && m.rd(intfReg(i, kIntfCountEn)) == 0);
    }
  }
  CHECK(m.dcs.size() == 80 && m.desync == 0 && m.glitches == 0 && m.commandsInFrame == 0 && clean(m));
}

static void testUnavailablePanel()
{
  model::Dpu m;
  // The second timing engine runs, but its counters do not advance even with the enable bits set.
  m.intf[2].counting = false;
  LivePath p = readPath(m);
  const CounterInit init = initCounters(m, p);
  CHECK(p.ok && init.enabled && !init.moving && m.stores.empty());
  RequestGate gate;
  gate.ready = init.moving;
  unsigned logs = 0;
  const char *message = nullptr;
  auto log = [&](const char *line) { ++logs; message = line; };
  if (gate.admit(false, log)) panelOff(m, p, Options());
  CHECK(logs == 1 && message && std::strstr(message, "sleep refused"));
  CHECK(m.dcs.empty() && m.stores.empty() && m.intf[1].on && m.intf[2].on);
  CHECK(!gate.admit(true, log) && logs == 2 && std::strstr(message, "wake refused"));
  gate.wakePending = true;   // a dispatched transition may still need a recovery wake after sleep was disabled
  CHECK(gate.admit(true, log) && logs == 2);
  CHECK(!gate.admit(false, log) && logs == 3);
}

struct TraceIo : model::Dpu {
  uint64_t sampleStartNs = 0, maxSampleNs = 0, maxPollOverrunNs = 0;
  std::vector<uint64_t> slavePollNs;
  bool slaveStop = false;

  bool wr(uint64_t pa, uint32_t value)
  {
    if (pa == intfReg(2, kIntfEn) && value == 0) slaveStop = true;
    return Dpu::wr(pa, value);
  }

  uint32_t rd(uint64_t pa)
  {
    const uint32_t value = Dpu::rd(pa);
    if (slaveStop && pa == intfReg(2, kIntfStatus) && slavePollNs.size() < 41) slavePollNs.push_back(ns);
    return value;
  }

  uint64_t pollDeadline(uint32_t us)
  {
    sampleStartNs = ns;
    return Dpu::pollDeadline(us);
  }

  void pollWait(uint64_t deadline, bool sleep)
  {
    if (ns - sampleStartNs > maxSampleNs) maxSampleNs = ns - sampleStartNs;
    if (ns > deadline && ns - deadline > maxPollOverrunNs) maxPollOverrunNs = ns - deadline;
    Dpu::pollWait(deadline, sleep);
  }
};

static Result replayY12n(TraceIo &m, bool dropped, TraceRing *ring)
{
  m.blockDma = false;
  m.engineStuck[1] = !dropped;
  m.dropStore = intfReg(2, kIntfEn);
  m.dropCount = dropped ? 1 : 0;
  for (unsigned i = 1; i <= 2; ++i) {
    m.regs[intfReg(i, kIntfCountEn)] = 0;
    m.countEnableIgnored[i - 1] = true;
    m.regs[intfReg(i, kIntfConfig)] = kIntfConfigProgFetch;
    m.intf[i].fetch = m.regs[intfReg(i, kIntfProgFetch)] = 2290 * model::kHtotal;
  }
  LivePath p = readPath(m);
  const CounterInit init = initCounters(m, p);
  CHECK(p.ok && !init.enabled && init.moving);
  RequestGate gate;
  gate.ready = init.moving;
  unsigned sleepRefusals = 0;
  auto log = [&](const char *msg) { if (std::strstr(msg, "sleep refused")) ++sleepRefusals; };
  const auto offAdmitted = [&](bool blank, bool touchParked) { return blank && touchParked && gate.admit(false, log); };
  CHECK(!offAdmitted(false, false) && m.dcs.empty() && (!ring || !ring->size()));  // pre-first-on guard
  CHECK(offAdmitted(true, true));   // first runtime off, with dual blank and touch park confirmed
  const Result r = panelOff(m, p, Options(), ring);
  CHECK(!r.ok && r.step == 3 && r.restored && !r.enginesStopped && !r.safeAwake);
  CHECK(std::strcmp(r.why, "INTF2's timing engine did not stop within 40 ms") == 0);
  CHECK(r.dcs[0].command == 0x28 && r.dcs[1].command == 0x10);
  for (const Sent &s : r.dcs) {
    CHECK(s.ok && s.waitedBlanking && s.dmaDoneMask() == 3 && s.doneUs > 0 && s.doneUs < 2000);
    CHECK(s.ackBeforeMask == 3 && s.ackAfterMask == 3 && !s.before[0].ack && !s.before[1].ack && !s.ackAfter[0] && !s.ackAfter[1]);
    CHECK((s.intrPoll[0] & kIntDmaDone) && (s.intrPoll[1] & kIntDmaDone));
  }
  CHECK(m.firstStore(intfReg(1, kIntfEn), 0) < 0);
  CHECK((m.firstStore(intfReg(2, kIntfEn), 1) >= 0) == !dropped);
  CHECK(m.dcs.size() == 8 && m.firstDcs(0x11, 1) && m.firstDcs(0x29, 1));
  CHECK(m.intf[1].on && m.intf[2].on && m.half[0].displayOn && m.half[1].displayOn && clean(m));
  gate.ready = r.ok;
  gate.wakePending = true;
  CHECK(gate.admit(true, log) && r.restored);   // worker's recovery wake has no new hardware sequence
  gate.wakePending = false;
  const unsigned rows = ring ? ring->total : 0;
  const size_t stores = m.stores.size();
  for (unsigned i = 0; i < 10; ++i) CHECK(!offAdmitted(true, true));
  CHECK(sleepRefusals == 10 && m.stores.size() == stores && m.dcs.size() == 8 && (!ring || ring->total == rows));
  return r;
}

static void testY12nTrace()
{
  uint64_t maxSampleNs = 0, maxPollShiftNs = 0;
  for (bool dropped : {false, true}) {
    TraceRing ring;
    TraceIo logged, baseline;
    const Result plain = replayY12n(baseline, dropped, nullptr);
    const Result traced = replayY12n(logged, dropped, &ring);
    CHECK(traced.step == plain.step && traced.engines.status[1] == plain.engines.status[1]);
    CHECK(logged.stores.size() == baseline.stores.size());
    for (size_t i = 0; i < logged.stores.size() && i < baseline.stores.size(); ++i) {
      CHECK(logged.stores[i].pa == baseline.stores[i].pa && logged.stores[i].v == baseline.stores[i].v);
    }
    CHECK(logged.slavePollNs.size() == 41 && logged.slavePollNs.size() == baseline.slavePollNs.size());
    for (size_t i = 1; i < logged.slavePollNs.size() && i < baseline.slavePollNs.size(); ++i) {
      const uint64_t withTrace = logged.slavePollNs[i] - logged.slavePollNs[0];
      const uint64_t withoutTrace = baseline.slavePollNs[i] - baseline.slavePollNs[0];
      CHECK(withTrace == withoutTrace);
      const uint64_t shift = withTrace > withoutTrace ? withTrace - withoutTrace : withoutTrace - withTrace;
      if (shift > maxPollShiftNs) maxPollShiftNs = shift;
    }
    if (logged.maxSampleNs > maxSampleNs) maxSampleNs = logged.maxSampleNs;
    CHECK(logged.maxPollOverrunNs == 0);

    CHECK(ring.size() == ring.total && ring.size() > 41);
    CHECK(ring.at(0).boundary && ring.at(0).step == TraceStep::displayOff);
    const TraceSample &last = ring.at(ring.size() - 1);
    CHECK(last.boundary && last.step == TraceStep::end && last.link[1].en == 1 && last.link[1].status == 1);
    const TraceSample *firstStopPoll = nullptr, *lastStopPoll = nullptr;
    unsigned stopPolls = 0;
    bool undo = false, flushed = false, latched = false, verified = false;
    for (unsigned i = 0; i < ring.size(); ++i) {
      const TraceSample &s = ring.at(i);
      CHECK(s.top == 0xf0000000u && s.active == 0x6);
      CHECK(!i || s.us >= ring.at(i - 1).us);
      CHECK(s.step != TraceStep::stopMaster);
      if (s.step == TraceStep::stopSlave) {
        CHECK(s.link[0].en == 1 && s.link[0].status == 1 && s.link[1].status == 1 && !s.flush && !s.intfFlush);
        CHECK(s.link[1].en == (s.boundary || dropped ? 1u : 0u));
        if (s.boundary) continue;
        ++stopPolls;
        if (!firstStopPoll) firstStopPoll = &s;
        lastStopPoll = &s;
      }
      if (s.step == TraceStep::undo) undo = s.boundary && s.link[1].en == unsigned(dropped) && s.link[1].status == 1;
      if (s.step == TraceStep::flush && (s.flush & kFlushIntf)) flushed |= s.intfFlush == kIntfFlushDsi;
      if (s.step == TraceStep::flush && s.flush == 0) latched = true;
      if (s.step == TraceStep::rate && !s.boundary) verified = true;
    }
    CHECK(undo && flushed && latched && verified && stopPolls == 41);
    if (firstStopPoll && lastStopPoll) {
      CHECK(lastStopPoll->us - firstStopPoll->us >= 40000);
      for (unsigned i = 0; i < 2; ++i) {
        CHECK(lastStopPoll->link[i].frame > firstStopPoll->link[i].frame);
        CHECK(lastStopPoll->link[i].line != firstStopPoll->link[i].line);
      }
    }
  }
  TraceIo io;
  TraceRing ring;
  const LivePath p = readPath(io);
  const Result off = panelOff(io, p, Options(), &ring);
  CHECK(off.ok && ring.at(ring.size() - 1).link[1].status == 0);
  ring.reset();
  const Result on = panelOn(io, p, Options(), off.enginesStopped, off.fetch, &ring);
  CHECK(on.ok && on.engines.rate.lockstep && ring.at(0).step == TraceStep::fetchOn);
  CHECK(ring.at(ring.size() - 1).link[1].status == 1 && io.desync == 0);
  CHECK(io.maxPollOverrunNs == 0);
  ring.reset();
  for (unsigned i = 0; i < TraceRing::kCapacity + 5; ++i) ring.next().us = i;
  CHECK(ring.size() == TraceRing::kCapacity && ring.at(0).us == 5 && ring.at(ring.size() - 1).us == TraceRing::kCapacity + 4);
  ring.reset();
  CHECK(!ring.size());
  CHECK(maxSampleNs > 0 && maxSampleNs < kLineNs && maxPollShiftNs < kLineNs / 10);
  std::printf("M5 trace model: max in-wait sample %llu ns, added stop poll drift %llu ns (line %u ns)\n",
              (unsigned long long)maxSampleNs, (unsigned long long)maxPollShiftNs, kLineNs);
}

static void testFailures()
{
  {   // display-off never finishes on DSI1: stop there, DSI1's words left to its engine, nothing more
    model::Dpu m;
    m.dmaHang[1] = true;
    m.sleepMs(3);
    const LivePath p = readPath(m);
    const Result r = panelOff(m, p, Options());
    CHECK(!r.ok && !r.safeAwake && r.step == 1 && std::strstr(r.why, "DSI1"));
    CHECK(r.dcs[0].dmaDoneMask() == 1 && r.dcs[0].ackBeforeMask == 3 && r.dcs[0].ackAfterMask == 1);
    CHECK(m.rd(dsiReg(1, kDsiTpgCtrl)) == 0x30006u && m.rd(dsiReg(0, kDsiTpgCtrl)) == 0);
    CHECK(m.storesTo(intfReg(1, kIntfEn)) == 0 && m.storesTo(intfReg(2, kIntfEn)) == 0 && m.intf[1].on);
  }
  {   // sleep-in never finishes on DSI0: no engine store, no undo on a link the engine still owns
    model::Dpu m;
    m.dmaBudget[0] = 1;
    m.sleepMs(3);
    const Result r = panelOff(m, readPath(m), Options());
    CHECK(!r.ok && r.step == 2 && !r.restored && m.storesTo(intfReg(2, kIntfEn)) == 0);
  }
  {   // INTF2 does not stop: brought back (EN = 1 again), sleep-out, display-on
    model::Dpu m;
    m.engineStuck[1] = true;
    m.sleepMs(3);
    const Result r = panelOff(m, readPath(m), Options());
    CHECK(!r.ok && r.step == 3 && r.restored && std::strstr(r.why, "INTF2"));
    CHECK(m.half[0].displayOn && !m.half[0].sleeping && m.rd(intfReg(2, kIntfEn)) == 1 && m.intf[1].on && m.intf[2].on);
    CHECK(m.firstStore(intfReg(1, kIntfEn), 0) < 0);   // the master was never stopped
  }
  {   // INTF1 does not stop: INTF2 (stopped) started again at INTF1's frame start, in phase; the panel back
    TraceIo m;
    TraceRing ring;
    m.engineStuck[0] = true;
    m.sleepMs(3);
    const Result r = panelOff(m, readPath(m), Options(), &ring);
    CHECK(!r.ok && r.step == 4 && r.restored && m.desync == 0 && m.intf[1].on && m.intf[2].on && m.half[1].displayOn);
    const TraceSample *lastAlign = nullptr;
    for (unsigned i = 0; i < ring.size(); ++i)
      if (ring.at(i).step == TraceStep::align) lastAlign = &ring.at(i);
    CHECK(lastAlign && !lastAlign->boundary && lastAlign->link[0].line <= 2 && lastAlign->link[1].status == 0);
    CHECK(m.maxPollOverrunNs == 0);
  }
  {   // the engines do not start at the wake: reported, no DCS sent into a stopped link
    model::Dpu m;
    m.sleepMs(3);
    const LivePath p = readPath(m);
    const Result off = panelOff(m, p, Options());
    CHECK(off.ok);
    m.engineDead[0] = m.engineDead[1] = true;
    const size_t dcs = m.dcs.size();
    const Result r = panelOn(m, p, Options(), off.enginesStopped, off.fetch);
    CHECK(!r.ok && r.step == 1 && std::strstr(r.why, "running") && m.dcs.size() == dcs);
  }
  {   // both engines count but the INTF flush never latched
    model::Dpu m;
    const LivePath p = readPath(m);
    const Result off = panelOff(m, p, Options());
    CHECK(off.ok);
    m.flushStuck = true;
    const size_t dcs = m.dcs.size();
    const Result wake = panelOn(m, p, Options(), off.enginesStopped, off.fetch);
    CHECK(!wake.ok && std::strstr(wake.why, "flush") && m.dcs.size() == dcs);
  }
  {   // DCS only: the engines keep running
    model::Dpu m;
    m.sleepMs(3);
    const LivePath p = readPath(m);
    Options o;
    o.engines = false;
    const Result off = panelOff(m, p, o);
    CHECK(off.ok && !off.enginesStopped && m.intf[1].on && m.half[0].sleeping && m.storesTo(intfReg(1, kIntfEn)) == 0);
    const Result on = panelOn(m, p, o, off.enginesStopped, off.fetch);
    CHECK(on.ok && m.half[0].displayOn && !m.half[1].sleeping && m.storesTo(intfReg(2, kIntfEn)) == 0);
  }
}

static void testHeldOnFailedOrSupersededWake()
{
  using namespace gopcompose;
  PanelPower power;
  Slot slot;
  Job frame;
  frame.kind = Job::kFull;
  frame.flip = true;
  frame.serial = 42;
  CHECK(offer(slot, frame, nullptr) == Offer::Taken);
  bool on;
  CHECK(power.request(false) && power.begin(on) && !on);
  power.finish(false, true);
  CHECK(power.request(true) && power.begin(on) && on);
  power.finish(true, false);
  CHECK(!power.canPresent() && !power.wakeComplete && slot.job.serial == 42);
  CHECK(power.request(false) && power.request(true) && power.begin(on) && on);
  CHECK(power.request(false));   // a new sleep arrived while the wake was in progress
  power.finish(true, true);
  CHECK(!power.canPresent() && !power.wakeComplete && slot.job.serial == 42);
  CHECK(power.begin(on) && !on);
  power.finish(false, false);    // a clean refusal of the hardware sleep
  CHECK(power.request(true) && power.begin(on) && on);
  power.finish(true, true);
  Busy busy;
  Job resumed;
  CHECK(power.canPresent() && power.wakeComplete && takeJob(slot, busy, &resumed) && resumed.serial == 42 && resumed.flip);
}

int main()
{
  testPacket();
  testSend();
  testRefusals();
  testSleepWake();
  testZeroCounterReadback();
  testUnavailablePanel();
  testY12nTrace();
  testFailures();
  testHeldOnFailedOrSupersededWake();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("PianoDpu panel host tests: all passed\n");
  return 0;
}
