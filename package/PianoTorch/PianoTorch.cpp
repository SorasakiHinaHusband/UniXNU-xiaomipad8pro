// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoTorch: piano's rear torch (the PM8550 flash LED module at SID 1 0xee00, both factory-wired channels together) for macOS 27,
// native at EL1. Design, facts, the control path and the test plan: docs/drivers/torch.md. Facts and sources: FlashHw.h.
//   piano-torch=1  a read-only survey, logged as "PianoTorch: ..." lines: the SPMI arbiter, the flash module's APID and its owner
//                  EE, identity, enables, per-channel current/strobe/timer, clamp, thermal thresholds, status bytes, raw bytes.
//                  The only store is the SPMI observer channel's read command (package/PianoPower/SpmiObserver.h).
//   piano-torch=2  the survey, then torch control: writes only to that peripheral, only the ten registers of TorchCore.h's
//                  allowlist, each read back; a time limit (piano-torch-timeout), the module's own fault and over-temperature
//                  bits, the SoC (PianoThermal) and battery (PianoPower) temperatures; off on system sleep and shutdown. Control:
//                  an administrator's IORegistry property (tooling/native/piano-torch) or another driver's callPlatformFunction
//                  (PianoTorchClient.h); piano-torch-test=S lights it once for S seconds at boot (the attended first test).
// Without piano-torch the driver declines in probe() and maps nothing.
// Other arguments: piano-torch-ma (default 50, the lamp current of an on without its own), piano-torch-max-ma (default 100,
// hard cap 400; the factory range is opt-in), piano-torch-timeout=S (default 300, 10..1800), piano-torch-soc-max=C (default 85),
// piano-torch-batt-max=C (default 45), piano-torch-delay=S (default 0), piano-torch-probe-spare=1 to add the old channel-B
// mapping window, piano-torch-flash=1 for one bounded timed FLASH pulse after the torch window, and piano-torch-ladder=1 for
// the guarded 100/200/300/400 mA ten-second ladder.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOUserClient.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <PianoPowerContract.h>
#include "TorchSpmi.h"
#include "PianoTorchClient.h"

using namespace pianotorch;
using pianopower::Window;

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last stage reached, negative when it failed there.
volatile int32_t piano_torch_stage = 0;
}

namespace {

struct Args {
  uint32_t level, ma, maxMa, timeout, test, delay, socMax, battMax, probe, probeSpare, flash, ladder;
};
Args gArgs;

uint32_t bootArg(const char *name, uint32_t fallback)
{
  uint32_t v = fallback;
  if (!PE_parse_boot_argn(name, &v, sizeof v)) v = fallback;
  return v;
}
uint32_t clampArg(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

void readArgs()
{
  gArgs.level = bootArg("piano-torch", 0);
  gArgs.maxMa = clampArg(bootArg("piano-torch-max-ma", kUbuntuTorchMaxMa), kTorchMinMa, kHardMaxMa);
  gArgs.ma = clampArg(bootArg("piano-torch-ma", 50), kTorchMinMa, gArgs.maxMa);
  gArgs.timeout = clampArg(bootArg("piano-torch-timeout", 300), 10, 1800);
  gArgs.test = clampArg(bootArg("piano-torch-test", 0), 0, 60);
  gArgs.delay = clampArg(bootArg("piano-torch-delay", 0), 0, 600);
  gArgs.socMax = clampArg(bootArg("piano-torch-soc-max", 85), 40, 105);
  gArgs.battMax = clampArg(bootArg("piano-torch-batt-max", 45), 30, 60);
  gArgs.probe = clampArg(bootArg("piano-torch-probe", 0), 0, 1);
  gArgs.probeSpare = clampArg(bootArg("piano-torch-probe-spare", 0), 0, 1);
  gArgs.flash = clampArg(bootArg("piano-torch-flash", 0), 0, 1);
  gArgs.ladder = clampArg(bootArg("piano-torch-ladder", 0), 0, 1);
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoTorch: %s\n", line);
}

void stage(int32_t value, const char *what)
{
  piano_torch_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at stage" : "stage", value < 0 ? -value : value, what);
}

uint64_t nowMs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000;
}

inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]" ::"r"(v), "r"(a) : "memory"); }

// ------------------------------------------------------------------------------------------------------------ SPMI windows
struct Region {
  IOMemoryMap *map;
  uintptr_t va;
};
Region gCore, gCnfg, gObsv, gChnl;

bool mapRegion(Region &r, uint64_t pa, uint64_t bytes)
{
  if (r.va) return true;
  const uint64_t base = pa & ~0xfffull, length = ((pa + bytes + 0xfff) & ~0xfffull) - base;
  IODeviceMemory *m = IODeviceMemory::withRange(base, length);
  r.map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  r.va = r.map ? uintptr_t(r.map->getVirtualAddress()) + uintptr_t(pa - base) : 0;
  return r.va != 0;
}
void unmapRegion(Region &r)
{
  if (r.map) r.map->release();
  r = {};
}

uintptr_t windowBase(Window w) { return w == Window::core ? gCore.va : w == Window::cnfg ? gCnfg.va : gObsv.va; }
uint32_t ioRd(void *, Window w, uint32_t offset) { return rd32(windowBase(w) + offset); }
// The observer channel's command word (a read request). No PMIC register is written through it.
void ioWr(void *, Window w, uint32_t offset, uint32_t value) { wr32(windowBase(w) + offset, value); }
void ioDelay(void *, unsigned us) { IODelay(us); }
const pianopower::Io gIo = {nullptr, ioRd, ioWr, ioDelay};

// The flash APID's owner channel (level 2): the one page mapped, the only PMIC write path of this driver.
uint32_t chRd(void *, uint32_t offset) { return rd32(gChnl.va + offset); }
void chWr(void *, uint32_t offset, uint32_t value) { wr32(gChnl.va + offset, value); }
const ChnlIo gChnlIo = {nullptr, chRd, chWr, ioDelay};

SpmiPort gPort;
pianopower::Arbiter gArbiter;
pianopower::Apid gApid;
Survey gSurvey;

// ------------------------------------------------------------------------------------------------------ the control state
struct Request {
  bool pending;
  unsigned ma, seconds;
  const char *source;
};
IOLock *gLock;
Request gReq;
Torch gTorch;          // under gLock
bool gReady;           // level 2 control is up (under gLock)
char gState[128] = "starting";

// The temperatures other drivers publish, looked up every 10 s until found (they never unload).
IOService *gThermal, *gBattery;
uint64_t gNextLookupMs;

IOService *lookup(const char *name)
{
  OSDictionary *matching = IOService::serviceMatching(name);
  IOService *s = matching ? IOService::copyMatchingService(matching) : nullptr;
  OSSafeReleaseNULL(matching);
  if (s) note("found %s (its temperature is part of the guard)", name);
  return s;
}

Temperatures readTemperatures(uint64_t now)
{
  if ((!gThermal || !gBattery) && now >= gNextLookupMs) {
    gNextLookupMs = now + 10000;
    if (!gThermal) gThermal = lookup("PianoThermal");
    if (!gBattery) gBattery = lookup("AppleSmartBattery");
  }
  Temperatures t;
  if (gThermal) {
    OSObject *o = gThermal->copyProperty("TemperaturesDeciC");   // retained: PianoThermal replaces the dictionary each update
    if (OSDictionary *d = OSDynamicCast(OSDictionary, o)) {
      if (OSCollectionIterator *it = OSCollectionIterator::withCollection(d)) {
        while (OSSymbol *key = OSDynamicCast(OSSymbol, it->getNextObject())) {
          OSNumber *n = OSDynamicCast(OSNumber, d->getObject(key));
          if (!n) continue;
          const int32_t deci = int32_t(n->unsigned32BitValue());
          if (!t.socValid || deci > t.socDeci) t.socDeci = deci;
          t.socValid = true;
        }
        it->release();
      }
    }
    OSSafeReleaseNULL(o);
    // B6's PMIC ADC publishes the flash thermistor separately from the TSENS hottest-per-controller map.
    o = gThermal->copyProperty("BoardTemperaturesDeciC");
    if (OSDictionary *d = OSDynamicCast(OSDictionary, o)) {
      if (OSNumber *n = OSDynamicCast(OSNumber, d->getObject("flash"))) {
        t.flashDeci = int32_t(n->unsigned32BitValue());
        t.flashValid = true;
      }
    }
    OSSafeReleaseNULL(o);
  }
  if (gBattery) {
    OSObject *o = gBattery->copyProperty("Temperature");
    if (OSNumber *n = OSDynamicCast(OSNumber, o)) {
      t.batteryCenti = int32_t(n->unsigned32BitValue());
      t.batteryValid = true;
    }
    OSSafeReleaseNULL(o);
  }
  return t;
}

void temperatureText(const Temperatures &t, char *flash, char *soc, char *battery, unsigned size)
{
  if (t.flashValid) snprintf(flash, size, "%d.%dC", t.flashDeci / 10, t.flashDeci % 10);
  else strlcpy(flash, "none", size);
  if (t.socValid) snprintf(soc, size, "%d.%dC", t.socDeci / 10, t.socDeci % 10);
  else strlcpy(soc, "none", size);
  if (t.batteryValid) snprintf(battery, size, "%d.%02dC", t.batteryCenti / 100, t.batteryCenti % 100);
  else strlcpy(battery, "none", size);
}

// ------------------------------------------------------------------------------------------------------------ the report
void reportSurvey(const Survey &s)
{
  note("flash: revision %#04x type %#04x subtype %#04x: %s%s", s.revision, s.type, s.subtype,
       !s.flash ? "NOT a flash LED module (type 0x18 expected); nothing more read" : subtypeName(s.subtype),
       s.flash && !s.fourChannel ? " (not decoded: this driver knows the four-channel layout only)" : "");
  if (!s.flash) return;
  for (unsigned row = 0; row < 256; row += 16) {
    char line[64];   // "raw xx:" and 16 bytes of " xx"
    int at = snprintf(line, sizeof line, "raw %02x:", row);
    for (unsigned i = row; i < row + 16; ++i)
      at += s.valid[i] ? snprintf(line + at, sizeof line - at, " %02x", s.reg[i]) : snprintf(line + at, sizeof line - at, " --");
    note("%s", line);
  }
  if (!s.fourChannel) return;
  const uint8_t clamp = s.reg[kRegTorchClamp] & kClampMask;
  note("flash: module %s (%02x), channels on %02x, 5 mA steps %02x, torch clamp %u (%u mA a channel), otst thresholds %u/%u",
       s.moduleEn() ? "ENABLED" : "off", s.reg[kRegModuleEn], s.chanEn(), s.reg[kRegIres] & 0x0f, clamp, (clamp + 1u) * 5u,
       s.reg[kRegThermThrsh1] & kThrshMask, s.reg[kRegThermThrsh2] & kThrshMask);
  for (unsigned ch = 0; ch < kChannels4ch; ++ch) {
    const ChannelView v = s.channel(ch);
    const unsigned ua = v.ua();
    note("flash ch%u%s: itarget %u (%u.%u mA in %s steps) strobe %02x (%s) timer %02x (%s)%s", ch,
         (kPianoChannelMask & (1u << ch)) ? " (piano's lamp)" : " (not wired on piano)", v.itarget, ua / 1000, ua % 1000 / 100,
         v.torchSteps ? "5 mA" : "12.5 mA", v.strobe, strobeName(v.strobe), v.timer,
         (v.timer & kTimerEnable) ? "safety timer on" : "safety timer off", v.on ? ", ON" : "");
  }
  const Faults f = s.faults();
  note("flash status1 %02x status2 %02x status3 %02x rt %02x: %s%s%s%s%s%s%s", s.reg[kRegStatus1], s.reg[kRegStatus2],
       s.reg[kRegStatus3], s.reg[kRegIntRtSts], f.any() ? "" : "no fault on piano's channels", f.shortCircuit ? "SHORT " : "",
       f.open ? "OPEN " : "", f.timeout ? "TIMER-EXPIRED " : "", f.thermal ? "OVER-TEMPERATURE " : "",
       f.current ? "CURRENT-LIMIT " : "", f.voltage ? "LOW-SUPPLY" : "");
}

// The probe of a no-light failure (piano-torch-probe=1): a full dump before the first window and after the last, and between
// them only the registers that changed (a diff against the previous dump), so the log stays readable. The probe runs once on
// this driver's own thread; gProbeLast is its private state.
Survey gProbeLast;
void probeSnapFull(const char *when)
{
  gProbeLast = survey(portBus(gPort));
  note("probe dump %s (full):", when);
  reportSurvey(gProbeLast);
}
void probeSnapDiff(const char *label)
{
  const Survey now = survey(portBus(gPort));
  unsigned changed = 0;
  for (unsigned i = 0; i < 256; ++i)
    if (now.valid[i] && gProbeLast.valid[i] && now.reg[i] != gProbeLast.reg[i]) ++changed;
  if (!changed)
    note("probe diff %s: no register changed", label);
  else {
    char line[230];
    int at = snprintf(line, sizeof line, "probe diff %s: %u changed:", label, changed);
    unsigned printed = 0;
    for (unsigned i = 0; i < 256; ++i) {
      if (!now.valid[i] || !gProbeLast.valid[i] || now.reg[i] == gProbeLast.reg[i]) continue;
      if (at >= int(sizeof line) - 14) {
        at += snprintf(line + at, sizeof line - at, " +%u more", changed - printed);
        break;
      }
      at += snprintf(line + at, sizeof line - at, " %02x:%02x->%02x", uint8_t(i), gProbeLast.reg[i], now.reg[i]);
      ++printed;
    }
    note("%s", line);
  }
  gProbeLast = now;
}

void formatStatus1Channels(uint8_t status1, uint8_t mask, char *out, unsigned size)
{
  unsigned at = 0;
  for (unsigned ch = 0; ch < kChannels4ch; ++ch) {
    if (!(mask & (1u << ch))) continue;
    const bool shorted = status1 & (1u << (2 * ch));
    const bool open = status1 & (1u << (2 * ch + 1));
    if (at) at += unsigned(snprintf(out + at, at < size ? size - at : 0, " "));
    at += unsigned(snprintf(out + at, at < size ? size - at : 0, "ch%u[short=%u open=%u]", ch, shorted, open));
    if (at >= size) {
      out[size - 1] = 0;
      return;
    }
  }
  if (!at) snprintf(out, size, "none");
}

void summary(const Survey &s, const char *refusal, char *out, unsigned size)
{
  if (!s.flash) {
    snprintf(out, size, "no flash module at 0x1ee (APID %d, type %02x)", gApid.apid, s.type);
    return;
  }
  snprintf(out, size, "%s rev %02x %s; APID %d owner EE %u; ch0/ch1 itarget %u/%u ires %02x clamp %u; status %02x %02x %02x; %s",
           s.fourChannel ? "4ch" : "other", s.revision, s.lit() ? "LIT" : s.moduleEn() ? "module-on" : "off", gApid.apid, gApid.owner,
           s.reg[kRegItarget0] & kItargetMask, s.reg[kRegItarget0 + 1] & kItargetMask, s.reg[kRegIres] & 0x0f,
           s.reg[kRegTorchClamp] & kClampMask, s.reg[kRegStatus1], s.reg[kRegStatus2], s.reg[kRegStatus3],
           refusal ? "not writable" : "writable");
}

void logOutcome(const char *what, const Torch::Outcome &o, unsigned askedMa)
{
  const StepResult &r = o.steps;
  if (o.reason != Reason::none && o.reason != Reason::user && !r.writes) {
    note("refused (%s): %s; nothing written%s", what, reasonName(o.reason), o.lit ? ", the torch stays as it was" : "");
    return;
  }
  if (o.lit) {
    note("on: %u mA (2 x %u mA, itarget %u, clamp %u) for %s; %u writes, %u read back; status %02x %02x %02x rt %02x, enables %02x %02x",
         gTorch.ma(), gTorch.ma() / 2, gTorch.itarget(), clampForMa(gTorch.maxMa()), what, r.writes, r.readbacks, o.live.status1,
         o.live.status2, o.live.status3, o.live.rt, o.live.moduleEn, o.live.chanEn);
    if (askedMa && askedMa != gTorch.ma()) note("on: %u mA asked, %u mA set (the limit is %u mA, 5 mA steps a channel)", askedMa, gTorch.ma(), gTorch.maxMa());
    return;
  }
  note("off (%s): %s; %u writes, %u read back%s", what, reasonName(o.reason), r.writes, r.readbacks, r.ok ? "" : "; A STEP FAILED");
  if (!r.ok && r.why)
    note("failed step: register %#04x wanted %02x read %02x: %s (refused %u)", r.failedReg, r.failedWant, r.failedGot, r.why, r.refused);
}

void publishState(IOService *self)
{
  char state[128];
  bool lit;
  unsigned ma;
  IOLockLock(gLock);
  strlcpy(state, gState, sizeof state);
  lit = gTorch.lit();
  ma = gTorch.ma();
  IOLockUnlock(gLock);
  self->setProperty(kPropertyState, state);
  self->setProperty(kPropertyLit, lit);
  self->setProperty(kPropertyMa, ma, 32);
}

void queue(unsigned ma, unsigned seconds, const char *source)
{
  IOLockLock(gLock);
  gReq.pending = true;
  gReq.ma = ma;
  gReq.seconds = seconds;
  gReq.source = source ? source : "?";
  IOLockWakeup(gLock, &gReq, false);
  IOLockUnlock(gLock);
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------ the service
class PianoTorch : public IOService {
  OSDeclareDefaultStructors(PianoTorch)
  void run();
  void serve();
  void ladder();
  void probe();
  void probeWindow(uint8_t mask, const char *label);
  Reason probePollPhase(uint8_t mask, const char *phase, unsigned durationMs, bool enforceGuard);
  void probeFlashPulse();
  void offNow(Reason why, const char *what);
  static void thread(void *self, wait_result_t);

 public:
  using IOService::callPlatformFunction;
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn setProperties(OSObject *properties) override;
  IOReturn callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4) override;
  IOReturn setPowerState(unsigned long state, IOService *device) override;
  void systemWillShutdown(IOOptionBits specifier) override;
};

OSDefineMetaClassAndStructors(PianoTorch, IOService)

IOService *PianoTorch::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoTorch::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-torch=%u ma=%u max=%u timeout=%u s test=%u s soc-max=%u C batt-max=%u C delay=%u probe=%u spare=%u flash=%u ladder=%u (PM8550 SID %u flash %#x, PPID %#x)",
       gArgs.level, gArgs.ma, gArgs.maxMa, gArgs.timeout, gArgs.test, gArgs.socMax, gArgs.battMax, gArgs.delay, gArgs.probe,
       gArgs.probeSpare, gArgs.flash, gArgs.ladder, kSid, kBase, kPpid);
  if (gArgs.level > 2) {
    note("piano-torch=%u: only levels 1 and 2 exist (docs/drivers/torch.md section 8); running 2", gArgs.level);
    gArgs.level = 2;
  }
  if (gArgs.level == 1) note("level 1: loads only, nothing written (the SPMI observer's read command is the only store)");
  setProperty("piano-torch", gArgs.level, 32);
  gLock = IOLockAlloc();
  thread_t t = nullptr;
  if (!gLock || kernel_thread_start(&PianoTorch::thread, this, &t) != KERN_SUCCESS) {
    stage(-1, "no lock or no thread; nothing done");
    return true;
  }
  thread_deallocate(t);
  if (gArgs.level >= 2) pianopower::join(this);   // setPowerState(kOff) turns the torch off before a system sleep
  registerService();                               // tooling/native/piano-torch and other drivers find it by class
  return true;
}

void PianoTorch::thread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-torch");
  static_cast<PianoTorch *>(self)->run();
}

void PianoTorch::run()
{
  if (gArgs.delay) {
    note("waiting %u s", gArgs.delay);
    IOSleep(gArgs.delay * 1000);
  }
  stage(1, "SPMI: the arbiter and the flash module's APID (read only)");
  using namespace pianopower;
  if (!mapRegion(gCore, kSpmiCore, kSpmiCoreBytes) || !mapRegion(gCnfg, kSpmiCnfg, kSpmiCnfgBytes) ||
      !mapRegion(gObsv, kSpmiObsv, kSpmiObsvBytes)) {
    stage(-1, "the SPMI arbiter windows do not map");
    return;
  }
  gArbiter = spmiArbiter(gIo);
  if (!gArbiter.v7) {
    note("spmi: arbiter version %#x, %u APIDs: not the v7 layout", gArbiter.version, gArbiter.count);
    stage(-1, "not the SPMI arbiter v7; nothing read");
    return;
  }
  gApid = spmiFindApid(gIo, gArbiter, kPpid);
  note("spmi: arbiter %#x, %u APIDs; flash PPID %#x: APID %d, owner EE %u%s", gArbiter.version, gArbiter.count, kPpid, gApid.apid,
       gApid.owner, gApid.apid < 0 ? " (NOT in the map)" : gApid.owner == 0 ? " (this processor: writes possible)" : " (another EE: our writes would be refused)");
  if (gApid.apid < 0) {
    stage(-1, "the flash module is not in the APID map");
    return;
  }
  gPort.io = &gIo;
  gPort.apid = gApid.apid;

  stage(2, "the flash module's registers over the observer channel (read only)");
  gSurvey = survey(portBus(gPort));
  reportSurvey(gSurvey);
  const char *refusal = writeRefusal(gSurvey, gApid.apid, gApid.owner);
  char line[200];
  summary(gSurvey, refusal, line, sizeof line);
  setProperty(kPropertySurvey, line);
  note("summary: %s", line);
  {   // the SPMI lock every kext shares (PianoSpmi.h): this kext's transactions and waits, and every kext's
    char lockLine[360];
    pianospmi::format(lockLine, sizeof lockLine);
    note("%s", lockLine);
  }

  if (gArgs.level < 2) {
    stage(3, "done");
    note("done: %u observer reads (%u failed), 0 PMIC writes", gPort.reads, gPort.readFailures);
    IOLockLock(gLock);
    snprintf(gState, sizeof gState, "unavailable: read-only survey (piano-torch=1)");
    IOLockUnlock(gLock);
    publishState(this);
    unmapRegion(gCore);
    unmapRegion(gCnfg);
    unmapRegion(gObsv);
    return;
  }

  stage(3, "level 2: torch control (writes to the flash module only, each read back)");
  if (refusal || !mapRegion(gChnl, kSpmiChnls + uint64_t(kChnlStride) * uint32_t(gApid.apid), 0x20)) {
    note("level 2 refused: %s; the survey above is all this boot does", refusal ? refusal : "the APID's write channel does not map");
    IOLockLock(gLock);
    snprintf(gState, sizeof gState, "unavailable: %s", refusal ? refusal : "no write channel");
    IOLockUnlock(gLock);
    publishState(this);
    stage(-3, "no control");
    return;
  }
  gPort.chnl = &gChnlIo;
  GuardConfig g;
  g.defaultS = gArgs.timeout;
  g.socMaxDeci = int32_t(gArgs.socMax) * 10;
  g.batteryMaxCenti = int32_t(gArgs.battMax) * 100;
  IOLockLock(gLock);
  gTorch.configure(portBus(gPort), g, gArgs.maxMa);
  gTorch.controller().seed(gSurvey);
  gReady = true;
  const bool foundLit = gSurvey.lit();
  Torch::Outcome o;
  if (foundLit) o = gTorch.turnOff(nowMs(), Reason::bootFoundOn);
  gTorch.describe(nowMs(), gState, sizeof gState);
  IOLockUnlock(gLock);
  note("level 2: torch control on channels 1+2 at %u mA (at most %u), auto-off after %u s, off at SoC >= %u C or battery >= %u C; "
       "writes only to PPID %#x, %u allowlisted registers",
       gArgs.ma, gArgs.maxMa, gArgs.timeout, gArgs.socMax, gArgs.battMax, kPpid, unsigned(sizeof kWritable / sizeof kWritable[0]));
  if (foundLit) logOutcome("it was lit when the driver started", o, 0);
  publishState(this);
  if (gArgs.ladder) {
    ladder();
  } else if (gArgs.probe && gArgs.test) {
    probe();   // replaces the single boot test: two windows (the factory's pair and the spare pair), dumps and polls
  } else if (gArgs.test) {
    note("test: on at %u mA for %u s (piano-torch-test)", gArgs.ma, gArgs.test);
    queue(gArgs.ma, gArgs.test, "the boot test");
  }
  serve();
}

// The T4 probe (piano-torch-probe=1 with piano-torch-test=S): one 15-second lit window by default — channels 1+2 (the factory's
// own torch pair) at piano-torch-ma — with STATUS1's per-channel short/open bits and INT_RT_STS (0x10) polled every 500 ms
// before, during and after it, and a full 256-byte register dump before and after. The old spare-pair mapping window remains
// available with piano-torch-probe-spare=1. A bounded timed FLASH pulse can follow the torch window with piano-torch-flash=1.
void PianoTorch::probe()
{
  note("probe: piano-torch-probe=1: %u mA (Ubuntu 2 x 50 mA cap) for %u s, status1/INT_RT_STS polls every 500 ms before/during/after",
       gArgs.ma, gArgs.test);
  probeSnapFull("1: before torch window");
  probeWindow(kPianoChannelMask, "A: channels 1+2 (the factory's torch pair)");
  probeSnapDiff("2: after torch window A");
  if (gArgs.probeSpare) {
    probeWindow(kOtherChannelMask, "B: channels 3+4 (the spare pair)");
    probeSnapDiff("3: after spare window B");
  }
  if (gArgs.flash) probeFlashPulse();
  probeSnapFull(gArgs.probeSpare ? "4: after all windows" : "3: after torch window");
}

void PianoTorch::probeWindow(uint8_t mask, const char *label)
{
  const unsigned ma = clampMa(gArgs.ma, gArgs.maxMa);
  const uint8_t itarget = itargetForMa(ma);
  note("probe window %s: on at %u mA (itarget %u, clamp %u) for %u s", label, ma, itarget, clampForMa(gArgs.maxMa), gArgs.test);
  const uint64_t start = nowMs();
  Reason bad = Reason::none;
  StepResult steps;
  const Temperatures t0 = readTemperatures(start);
  probePollPhase(mask, "before", 1000, false);
  IOLockLock(gLock);
  if (mask == kPianoChannelMask) {   // the real control path, exactly as a user's on
    const Torch::Outcome o = gTorch.request(start, ma, gArgs.test, t0);
    steps = o.steps;
    bad = o.reason;
    if (o.lit) gTorch.setTestMode(true);
    gTorch.describe(start, gState, sizeof gState);
  } else {                           // the spare pair: the controller directly, this boot's diagnostic
    steps = gTorch.controller().on(itarget, clampForMa(gArgs.maxMa), mask);
    if (!steps.ok) bad = Reason::writeFailed;
    snprintf(gState, sizeof gState, "probe window B %s", steps.ok ? "on" : "off (a write failed)");
  }
  IOLockUnlock(gLock);
  note("probe window %s: %s; %u writes, %u read back%s", label, bad == Reason::none ? "on" : reasonName(bad), steps.writes,
       steps.readbacks, steps.ok ? "" : "; A STEP FAILED");
  if (!steps.ok) {
    if (!steps.why) note("probe window %s: no step detail (refused %u)", label, steps.refused);
    else note("failed step: register %#04x wanted %02x read %02x: %s", steps.failedReg, steps.failedWant, steps.failedGot, steps.why);
  }
  if (bad != Reason::none) {   // request() ran its own off; a raw window's remains
    if (mask != kPianoChannelMask) {
      IOLockLock(gLock);
      const StepResult undo = gTorch.controller().off(mask);
      IOLockUnlock(gLock);
      note("probe window %s: off after the refusal; %u writes, %u read back", label, undo.writes, undo.readbacks);
    }
    probePollPhase(mask, "after", 1000, false);
    publishState(this);
    return;
  }

  // While lit: the module's own words for why a lit register state makes no light (open channel, vreg not ready), which only
  // appear once the channels are on — sampled twice a second, logged every sample.
  bad = probePollPhase(mask, "during", gArgs.test * 1000, true);
  probeSnapDiff(label);   // any register the module itself changed while lit shows here
  IOLockLock(gLock);
  if (mask == kPianoChannelMask) {
    const Torch::Outcome o = gTorch.turnOff(nowMs(), Reason::test);
    gTorch.describe(nowMs(), gState, sizeof gState);
    IOLockUnlock(gLock);
    note("probe window %s: off (%s); %u writes, %u read back", label, reasonName(o.reason), o.steps.writes, o.steps.readbacks);
  } else {
    const StepResult r = gTorch.controller().off(mask);
    snprintf(gState, sizeof gState, "off (the probe)");
    IOLockUnlock(gLock);
    note("probe window %s: off (the probe); %u writes, %u read back", label, r.writes, r.readbacks);
  }
  const Reason after = probePollPhase(mask, "after", 1000, false);
  if (bad == Reason::none) bad = after;
  if (bad != Reason::none) note("probe window %s: ENDED EARLY by the guard: %s", label, reasonName(bad));
  publishState(this);
}

Reason PianoTorch::probePollPhase(uint8_t mask, const char *phase, unsigned durationMs, bool enforceGuard)
{
  unsigned unreadable = 0;
  Reason bad = Reason::none;
  const uint64_t phaseStart = nowMs();
  for (unsigned elapsed = 0; elapsed <= durationMs; elapsed += 500) {
    if (elapsed) IOSleep(500);
    const uint64_t now = nowMs();
    const Temperatures t = readTemperatures(now);
    IOLockLock(gLock);
    const Controller::Live l = gTorch.controller().live();
    const Reason verdict = enforceGuard ? judge(l, unreadable, t, gTorch.config(), mask) : Reason::none;
    IOLockUnlock(gLock);
    char faults[80];
    char channels[128];
    formatFaults(decodeFaults(l.status1, l.status2, l.status3, mask), faults, sizeof faults);
    formatStatus1Channels(l.status1, mask, channels, sizeof channels);
    if (!l.ok) {
      ++unreadable;
      note("probe %s: t+%llu ms: STATUS1/INT_RT_STS 0x10 UNREADABLE (%u consecutive): %s", phase,
           (unsigned long long)(now - phaseStart), unreadable, channels);
    } else {
      unreadable = 0;
      note("probe %s: t+%llu ms: STATUS1=%02x %s INT_RT_STS(0x10)=%02x status2=%02x status3=%02x enables=%02x/%02x faults=%s",
           phase, (unsigned long long)(now - phaseStart), l.status1, channels, l.rt, l.status2, l.status3, l.moduleEn, l.chanEn, faults);
    }
    if (verdict != Reason::none) {
      bad = verdict;
      break;
    }
  }
  return bad;
}

void PianoTorch::probeFlashPulse()
{
  // 50 mA total (2 x 25 mA, 12.5 mA FLASH steps) for 80 ms. The safety timer
  // is the visibility-friendly part of Ubuntu's photo path; off() still runs
  // afterward to cover a timer or V4L2-owner edge case.
  constexpr uint8_t kFlashItarget = 1;
  constexpr uint8_t kFlashTimerTicks = 8;
  note("probe FLASH pulse: timed 80 ms at 50 mA total (2 x 25 mA, 12.5 mA steps); V4L2 close trap covered by explicit off");
  IOLockLock(gLock);
  const StepResult pulse = gTorch.controller().flashPulse(kFlashItarget, kFlashTimerTicks, kPianoChannelMask);
  IOLockUnlock(gLock);
  note("probe FLASH pulse: on; %u writes, %u read back%s", pulse.writes, pulse.readbacks, pulse.ok ? "" : "; A STEP FAILED");
  IOSleep(120);
  IOLockLock(gLock);
  const StepResult off = gTorch.controller().off(kPianoChannelMask);
  IOLockUnlock(gLock);
  note("probe FLASH pulse: explicit off after timer; %u writes, %u read back%s", off.writes, off.readbacks,
       off.ok ? "" : "; A STEP FAILED");
}

// Torch5: one guarded boot answers the useful-current question without four reboots. Each level is lit for ten seconds and
// followed by ten seconds dark. The PMIC status/RT byte and all three temperatures are sampled every 500 ms. A fault, an
// unreadable status, or an existing SoC/battery guard ends the ladder and leaves the module explicitly off.
void PianoTorch::ladder()
{
  static const unsigned levels[] = {100, 200, 300, 400};
  note("ladder: 100/200/300/400 mA total (2 channels), 10 s lit + 10 s dark, status/fault and temperatures every 500 ms; cap %u mA",
       gArgs.maxMa);
  for (unsigned step = 0; step < sizeof(levels) / sizeof(levels[0]); ++step) {
    const unsigned ma = levels[step];
    const uint64_t start = nowMs();
    const Temperatures t0 = readTemperatures(start);
    IOLockLock(gLock);
    Torch::Outcome on = gTorch.request(start, ma, 10, t0);
    gTorch.setTestMode(false);
    gTorch.describe(start, gState, sizeof gState);
    IOLockUnlock(gLock);
    note("ladder step %u: request %u mA (2 x %u mA)", step + 1, ma, ma / 2);
    logOutcome("ladder lit", on, ma);
    publishState(this);
    if (!on.lit) {
      note("ladder stopped at %u mA: %s", ma, reasonName(on.reason));
      return;
    }

    for (unsigned elapsed = 0; elapsed < 10000; elapsed += 500) {
      if (elapsed) IOSleep(500);
      const uint64_t now = nowMs();
      const Temperatures t = readTemperatures(now);
      IOLockLock(gLock);
      const Torch::Outcome poll = gTorch.poll(now, t);
      const Controller::Live live = poll.live;
      gTorch.describe(now, gState, sizeof gState);
      IOLockUnlock(gLock);
      char faults[80], channels[128];
      char flashTemp[24], socTemp[24], batteryTemp[24];
      formatFaults(decodeFaults(live.status1, live.status2, live.status3, kPianoChannelMask), faults, sizeof faults);
      formatStatus1Channels(live.status1, kPianoChannelMask, channels, sizeof channels);
      temperatureText(t, flashTemp, socTemp, batteryTemp, sizeof flashTemp);
      note("ladder poll: step=%u ma=%u phase=lit t+%u ms STATUS1=%02x %s INT_RT_STS(0x10)=%02x status2=%02x status3=%02x enables=%02x/%02x faults=%s temp flash=%s SoC=%s battery=%s",
           step + 1, ma, elapsed, live.status1, channels, live.rt, live.status2, live.status3, live.moduleEn, live.chanEn, faults,
           flashTemp, socTemp, batteryTemp);
      if (!poll.lit) {
        note("ladder stopped at %u mA lit window: %s", ma, reasonName(poll.reason));
        return;
      }
    }

    IOLockLock(gLock);
    const Torch::Outcome off = gTorch.turnOff(nowMs(), Reason::test);
    gTorch.describe(nowMs(), gState, sizeof gState);
    IOLockUnlock(gLock);
    note("ladder step %u: dark; %u writes, %u read back%s", step + 1, off.steps.writes, off.steps.readbacks,
         off.steps.ok ? "" : "; A STEP FAILED");
    publishState(this);
    if (!off.steps.ok) {
      note("ladder stopped after %u mA: off write failed", ma);
      return;
    }

    unsigned darkUnreadable = 0;
    for (unsigned elapsed = 0; elapsed < 10000; elapsed += 500) {
      if (elapsed) IOSleep(500);
      const uint64_t now = nowMs();
      const Temperatures t = readTemperatures(now);
      IOLockLock(gLock);
      const Controller::Live live = gTorch.controller().live();
      IOLockUnlock(gLock);
      if (!live.ok) ++darkUnreadable;
      else darkUnreadable = 0;
      char faults[80], channels[128];
      char flashTemp[24], socTemp[24], batteryTemp[24];
      formatFaults(decodeFaults(live.status1, live.status2, live.status3, kPianoChannelMask), faults, sizeof faults);
      formatStatus1Channels(live.status1, kPianoChannelMask, channels, sizeof channels);
      temperatureText(t, flashTemp, socTemp, batteryTemp, sizeof flashTemp);
      note("ladder poll: step=%u ma=%u phase=dark t+%u ms STATUS1=%02x %s INT_RT_STS(0x10)=%02x status2=%02x status3=%02x enables=%02x/%02x faults=%s temp flash=%s SoC=%s battery=%s",
           step + 1, ma, elapsed, live.status1, channels, live.rt, live.status2, live.status3, live.moduleEn, live.chanEn, faults,
           flashTemp, socTemp, batteryTemp);
      const Faults f = decodeFaults(live.status1, live.status2, live.status3, kPianoChannelMask);
      const bool hot = (t.socValid && t.socDeci >= gTorch.config().socMaxDeci) ||
                       (t.batteryValid && t.batteryCenti >= gTorch.config().batteryMaxCenti);
      if (f.any() || darkUnreadable >= 2 || hot) {
        note("ladder stopped during dark after %u mA: %s%s%s", ma, f.any() ? "fault" : "", darkUnreadable >= 2 ? " unreadable" : "",
             hot ? " temperature guard" : "");
        return;
      }
    }
    note("ladder step %u complete: %u mA lit 10 s, dark 10 s", step + 1, ma);
  }
  note("ladder complete: all four levels passed without a fault or guard");
}

// The control loop: requests as they come; while lit, a poll every second (the time limit, the guard).
void PianoTorch::serve()
{
  uint64_t lastLogMs = 0;
  uint8_t lastStatus[3] = {0, 0, 0};
  for (;;) {
    IOLockLock(gLock);
    if (!gReq.pending) {
      uint64_t deadline = 0;
      clock_interval_to_deadline(gTorch.lit() ? 1000 : 60000, kMillisecondScale, &deadline);
      IOLockSleepDeadline(gLock, &gReq, deadline, THREAD_UNINT);
    }
    const Request r = gReq;
    gReq.pending = false;
    const bool lit = gTorch.lit();
    IOLockUnlock(gLock);
    if (!r.pending && !lit) continue;

    const uint64_t now = nowMs();
    const Temperatures t = readTemperatures(now);
    IOLockLock(gLock);
    Torch::Outcome o;
    bool acted = false, polled = false;
    if (r.pending) {
      const bool test = r.source && !strcmp(r.source, "the boot test");
      o = gTorch.request(now, r.ma, r.seconds, t);
      if (o.lit) gTorch.setTestMode(test);   // the boot test's end is logged as such; any later request is the user's
      acted = true;
    } else if (lit) {
      o = gTorch.poll(now, t);
      polled = true;
    }
    gTorch.describe(now, gState, sizeof gState);
    IOLockUnlock(gLock);

    if (acted) {
      char what[96];
      snprintf(what, sizeof what, "%s, %u mA %u s", r.source, r.ma, r.seconds);
      logOutcome(what, o, r.ma);
      if (t.socValid || t.batteryValid)
        note("temperatures: SoC %d.%d C%s, battery %d.%02d C%s", t.socDeci / 10, t.socDeci % 10, t.socValid ? "" : " (none)",
             t.batteryCenti / 100, t.batteryCenti % 100, t.batteryValid ? "" : " (none)");
      lastLogMs = now;
      lastStatus[0] = o.live.status1;
      lastStatus[1] = o.live.status2;
      lastStatus[2] = o.live.status3;
    } else if (polled) {
      if (!o.lit) {
        logOutcome("guard", o, 0);
      } else if (now - lastLogMs >= 30000 || o.live.status1 != lastStatus[0] || o.live.status2 != lastStatus[1] ||
                 o.live.status3 != lastStatus[2]) {
        note("lit: %s; status %02x %02x %02x rt %02x enables %02x %02x; SoC %d.%d C battery %d.%02d C", gState, o.live.status1,
             o.live.status2, o.live.status3, o.live.rt, o.live.moduleEn, o.live.chanEn, t.socValid ? t.socDeci / 10 : 0,
             t.socValid ? t.socDeci % 10 : 0, t.batteryValid ? t.batteryCenti / 100 : 0, t.batteryValid ? t.batteryCenti % 100 : 0);
        lastLogMs = now;
        lastStatus[0] = o.live.status1;
        lastStatus[1] = o.live.status2;
        lastStatus[2] = o.live.status3;
      }
    }
    publishState(this);
  }
}

// Synchronous off from a power transition (sleep, shutdown): a few SPMI transactions, well under a millisecond.
void PianoTorch::offNow(Reason why, const char *what)
{
  if (!gLock) return;
  IOLockLock(gLock);
  const bool act = gReady && gTorch.lit();
  Torch::Outcome o;
  if (act) {
    o = gTorch.turnOff(nowMs(), why);
    gTorch.describe(nowMs(), gState, sizeof gState);
  }
  IOLockUnlock(gLock);
  if (act) {
    logOutcome(what, o, 0);
    publishState(this);
  }
}

IOReturn PianoTorch::setPowerState(unsigned long state, IOService *)
{
  if (state == pianopower::kOff) offNow(Reason::sleep, "system sleep");
  return pianopower::kAckNow;
}

void PianoTorch::systemWillShutdown(IOOptionBits specifier)
{
  offNow(Reason::shutdown, "shutdown or restart");
  IOService::systemWillShutdown(specifier);
}

// An administrator's request (IORegistryEntrySetCFProperties): "piano-torch-set" = mA (0 off) or a boolean, "piano-torch-seconds"
// optional. Queued for the thread; the result is in "piano-torch-state".
IOReturn PianoTorch::setProperties(OSObject *properties)
{
  OSDictionary *d = OSDynamicCast(OSDictionary, properties);
  if (!d) return kIOReturnBadArgument;
  OSObject *set = d->getObject(kPropertySet);
  if (!set) return kIOReturnUnsupported;
  if (gArgs.level < 2 || !gLock) return kIOReturnUnsupported;
  if (IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess) return kIOReturnNotPrivileged;
  unsigned ma;
  if (OSNumber *n = OSDynamicCast(OSNumber, set)) ma = n->unsigned32BitValue();
  else if (OSBoolean *b = OSDynamicCast(OSBoolean, set)) ma = b->isTrue() ? gArgs.ma : 0;
  else return kIOReturnBadArgument;
  OSNumber *seconds = OSDynamicCast(OSNumber, d->getObject(kPropertySeconds));
  IOLockLock(gLock);
  const bool ready = gReady;
  IOLockUnlock(gLock);
  if (!ready) return kIOReturnNotReady;
  queue(ma, seconds ? seconds->unsigned32BitValue() : 0, "setProperties");
  return kIOReturnSuccess;
}

IOReturn PianoTorch::callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4)
{
  const bool set = name && name->isEqualTo(kFunctionSet), toggle = name && name->isEqualTo(kFunctionToggle),
             status = name && name->isEqualTo(kFunctionStatus);
  if (!set && !toggle && !status) return IOService::callPlatformFunction(name, wait, p1, p2, p3, p4);
  if (!gLock) return kIOReturnNotReady;
  if (status) {
    char *out = static_cast<char *>(p1);
    const size_t size = size_t(uintptr_t(p2));
    if (!out || !size) return kIOReturnBadArgument;
    IOLockLock(gLock);
    strlcpy(out, gState, size);
    IOLockUnlock(gLock);
    return kIOReturnSuccess;
  }
  IOLockLock(gLock);
  const bool ready = gReady, lit = gTorch.lit() || (gReq.pending && gReq.ma);
  IOLockUnlock(gLock);
  if (!ready) return kIOReturnNotReady;
  if (set) queue(unsigned(uintptr_t(p1)), unsigned(uintptr_t(p2)), p3 ? static_cast<const char *>(p3) : "a driver");
  else queue(lit ? 0 : gArgs.ma, 0, p1 ? static_cast<const char *>(p1) : "a driver");
  return kIOReturnSuccess;
}
