// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifi80211: PianoWifiEthernet as a Wi-Fi interface for airportd (docs/drivers/wifi-menu.md section 5, milestone M1), behind
// piano-wifi-apple80211=1 (default off: PianoWifiEthernet, the Ethernet-like path, stays the default and the control).
// Included once, at the end of PianoWifiEthernet.cpp: it derives from PianoWifiEthernet (declared there) and changes nothing of
// it — the data path, the link, the en0 hand-over and the scan list's copy are PianoWifiEthernet's own.
//
// What it adds, all of it public KPI or the families' own exported functions (no Apple class is subclassed, nothing is added to
// the collection):
//   - the identity airportd's enumeration and SystemConfiguration look for (private/host-checks/20261114-io80211/REPORT.md section
//     4): the controller's class chain carries the name IO80211Controller (a class of ours by that name, between PianoWifiEthernet
//     and this one: IO80211Family is not in our native collection, so the name is free; IOKit matching and IOObjectConformsTo
//     compare class names), the interface carries IO80211InterfaceRole = "Infrastructure", and its ifnet the Wi-Fi subfamily
//     (BSD type 6 / IFT_ETHER, family Ethernet, subfamily 3) through IONetworkInterface::setInterfaceSubType, IONetworkingFamily's
//     exported setter (not in the SDK header; 27.3 disassembly: it stores the value for attachToDataLinkLayer, which puts it in
//     ifnet_init_eparams.subfamily, only before the interface is attached; docs/drivers/wifi-menu.md section 5.1);
//   - the apple80211 requests: an interface filter (iflt_attach, the BSD interface-filter KPI) whose ioctl callback answers
//     SIOCSA80211 / SIOCGA80211 before the interface's own handler (WifiApple80211.h); every other ioctl passes on, its first
//     occurrence logged;
//   - the apple80211 kernel events (power changed, scan done, link down) with kev_post_msg, as UniXNU-sheng's WifiNet.cpp does;
//   - the controller registered (registerService) once the filter answers, so that launchd's IOKit match for airportd
//     (IOProviderClass IO80211Controller) can fire only when the requests are answered.
// One log line per distinct request (number, direction, length, result), a summary every 60 s while anything changed. Never a
// network's name or address in the log.
#pragma once
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/copyio.h>   // copyin / copyout of the requesting task's buffers
#include <net/if_media.h>     // IFM_IEEE80211 / IFM_AUTO: the SIOCGIFMEDIA answer of sheng bit 0x20
#include <net/kpi_interfacefilter.h>
#include <IOKit/IOUserClient.h>
#include <sys/kern_event.h>
#include <sys/proc.h>     // proc_selfname, proc_selfpid: who asked for a scan
#include <sys/sockio.h>   // SIOCGIFMEDIA
#include <sys/sysctl.h>
#include "WifiApple80211.h"

// The KPI's private poster (Unsupported.exports): kev_msg_post refuses Apple's vendor code, which the apple80211 events carry.
extern "C" int piano_kev_post_msg(struct kev_msg *) __asm__("_kev_post_msg");
// IONetworkInterface::setInterfaceSubType(unsigned int), a non-virtual member exported by the 27.3 IONetworkingFamily and absent
// from the SDK's header: called with the interface as its object (x0) and the subfamily (w1); true when it took the value
// (the interface not yet attached to the network stack).
extern "C" bool piano_netif_set_subtype(IONetworkInterface *netif, unsigned int subfamily)
    __asm__("__ZN18IONetworkInterface19setInterfaceSubTypeEj");
#ifndef EJUSTRETURN
#define EJUSTRETURN (-2)   // sys/errno.h's kernel-private value: handled, the caller returns 0 and nothing after runs
#endif

// The class name airportd's launch match and SystemConfiguration's Wi-Fi test look for. Nothing but the name.
class IO80211Controller : public PianoWifiEthernet {
  OSDeclareDefaultStructors(IO80211Controller)
};
OSDefineMetaClassAndStructors(IO80211Controller, PianoWifiEthernet)

class PianoWifi80211 : public IO80211Controller {
  OSDeclareDefaultStructors(PianoWifi80211)

  using State = pianowifi::a80211::State;
  using Seen = pianowifi::a80211::Seen;
  static constexpr unsigned kAttachTickMs = 200, kAttachTicks = 900;   // the ifnet waited for up to 180 s
  static constexpr unsigned kScanDoneMs = 2000;   // a scan answered from the list completes after a short scan's time (sheng:
                                                  // answered at once, the daemon asks again straight away)
  static constexpr unsigned kReportMs = 60000;
  static constexpr uint32_t kScanLinesMax = 400;   // two lines a scan: about 200 scans, the first half hour of a boot

  IOLock *fLock = nullptr;
  State *fState = nullptr;          // data allocations (no pointer in them): the state, the scratch buffer, the log's table
  uint8_t *fScratch = nullptr;
  Seen *fSeen = nullptr;
  IONetworkInterface *fNetif = nullptr;   // PianoWifiEthernet's interface (owned there), seen in configureInterface
  IOService *fWifi = nullptr;             // PianoWifi, whose registry property piano-wifi-scan is the scan list
  interface_filter_t fFilter = nullptr;
  volatile bool fFilterGone = false, fStopping = false;
  thread_call_t fAttachCall = nullptr, fScanDoneCall = nullptr, fReportCall = nullptr;
  char fIfName[16] = {};
  unsigned fAttachTries = 0;
  bool fSubtypeTaken = false, fRegistered = false, fSysctl = false;
  // counters (under fLock)
  uint64_t fRequests = 0, fUnknown = 0, fEvents = 0, fEventErrors = 0, fListRefreshes = 0, fOtherIoctls = 0;
  uint64_t fReported = ~0ull;
  uint32_t fEventCodesLogged = 0;
  volatile uint32_t fCallsArmed = 0, fCallsInside = 0;   // thread calls armed / inside their callback (review F4)
  uint32_t fUcMode = 0;            // kUc*: the user-client switches (docs/drivers/wifi-menu.md section 7)
  uint64_t fUcOpens = 0, fUcRefused = 0, fUcSelectors = 0;   // newUserClient calls answered / refused, selectors served
  uint32_t fUcClients = 0;         // clients alive now
  uint32_t fScanLines = 0;   // the scans logged one by one (with the asking process), at most kScanLinesMax
  uint64_t fScanSerial = 0;  // every SCAN_REQ gets one (ADOPTION.md 6.1 item 3): the 15 ms CoreWLAN scan is found by it
  uint32_t fScanRecords = 0; // records served since the serial's scan; printed at its list end
  uint32_t fScanDonePending = 0; // one completion is required for every accepted scan request
  bool fScanDoneArmed = false;   // guarded by fLock; the delayed callback drains fScanDonePending
  unsigned long fOtherSeen[16] = {};
  unsigned fOtherCount = 0;

  static errno_t filterIoctl(void *cookie, ifnet_t, protocol_family_t, unsigned long cmd, void *arg);
  static void filterDetached(void *cookie, ifnet_t);
  static void attachTick(thread_call_param_t self, thread_call_param_t);
  static void scanDoneTick(thread_call_param_t self, thread_call_param_t);
  static void reportTick(thread_call_param_t self, thread_call_param_t);
  static int userIn(void *, uint64_t from, void *to, uint32_t length) { return copyin(from, to, length); }
  static int userOut(void *, const void *from, uint64_t to, uint32_t length) { return copyout(from, to, length); }
  // The user client's copies (sheng bit 0x8): the data of a client's request may ride inside the structure itself — kernel
  // memory — where copyout faults (their macOS-27.3 EFAULT fix); a copy that faults and whose target lies inside the
  // structure is answered with memcpy. The context is a UcIo (below), its `sheng` a snapshot taken before the request's lock.
  static int ucIn(void *ctx, uint64_t from, void *to, uint32_t length);
  static int ucOut(void *ctx, const void *from, uint64_t to, uint32_t length);

  void attachStep();
  errno_t apple80211(bool set, pianowifi::a80211::Req &rq, const pianowifi::a80211::UserIo *io = nullptr);
  void refreshScanLocked();
  void postEvent(uint32_t code, const void *payload = nullptr, uint32_t length = 0);
  void scanStarted();
  void noteOtherIoctl(unsigned long cmd);
  void report(const char *why);
  void eventPayload(uint32_t code, uint8_t *buffer, uint32_t capacity, uint32_t &length);
  void noteIdentity(const char *why);
  void setOverride(bool on, bool onInterface);
  void linkChanged(bool up) override;
  errno_t mediaIoctl(struct ifmediareq *m);
  void applyMedium();
  void applyGrant(bool on);

  // Armed only while the driver runs, and counted: stop() waits for every call to be out of its callback (review F4).
  void arm(thread_call_t call, unsigned ms)
  {
    if (fStopping) return;
    uint64_t deadline = 0;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    __atomic_fetch_add(&fCallsArmed, 1u, __ATOMIC_ACQ_REL);
    thread_call_enter_delayed(call, deadline);
  }
  // A callback's own frame: it holds the count while it runs, so stop() cannot free under it.
  struct Tick {
    PianoWifi80211 *self;
    bool run;
    explicit Tick(PianoWifi80211 *s) : self(s), run(!s->fStopping)
    {
      __atomic_fetch_add(&self->fCallsInside, 1u, __ATOMIC_ACQ_REL);
      __atomic_fetch_sub(&self->fCallsArmed, 1u, __ATOMIC_ACQ_REL);
    }
    ~Tick() { __atomic_fetch_sub(&self->fCallsInside, 1u, __ATOMIC_ACQ_REL); }
  };
  void quiesceCalls();

 protected:
  bool configureInterface(IONetworkInterface *netif) override;
  void free() override;

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  uint32_t acceptMode();
  uint32_t shengMode();
  static int sysctlAccept(struct sysctl_oid *, void *, int, struct sysctl_req *req);
  static int sysctlSheng(struct sysctl_oid *, void *, int, struct sysctl_req *req);
  void setAcceptMode(uint32_t mode, const char *why);
  void setShengMode(uint32_t mode, const char *why);
  bool start(IOService *provider) override;
  void stop(IOService *provider) override;
  // the user client's side (PianoWifi80211Uc.h)
  IOReturn newUserClient(task_t owningTask, void *security, UInt32 type, OSDictionary *properties, IOUserClient **handler) override;
  IOReturn ucRequest(IOExternalMethodArguments *args, bool large, uint32_t &answered);
  uint32_t ucMode();
  void noteUserClient(const char *what, task_t task, uint32_t type, uint64_t calls);
  void noteSelector(uint32_t selector, uint32_t inBytes, uint32_t outBytes, uint32_t scalarIn, uint32_t scalarOut,
                    uint32_t answered, IOReturn result, uint64_t calls);
  static int sysctlUc(struct sysctl_oid *, void *, int, struct sysctl_req *req);
  void setUcMode(uint32_t mode, const char *why);
};

// (A subclass of IOEthernetInterface named IO80211Interface was tried for wm2-diag's Rank 2 and cannot be built: the 27.3
// IONetworkingFamily does not export IONetworkInterface's reserved slot 10, so such a subclass's vtable does not link. The
// interface's identity is given by properties instead — IOClassNameOverride below, which is what 27.3's CoreWiFi reads anyway.)

#include "PianoWifi80211Uc.h"

OSDefineMetaClassAndStructors(PianoWifi80211, IO80211Controller)

namespace {
const pianowifi::a80211::Identity kIdentity = {"PianoWifi 1.0 (apple80211 M1)", "Qualcomm WCN7860", "PianoWifi"};

void note80211(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note80211(const char *fmt, ...)
{
  char line[300];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoWifi80211: %s\n", line);
}
// machdep.piano_wifi_a80211 (root to write): the accept mode, read and set at run time. Filled in at run time rather than by
// SYSCTL_OID's static initializer: its parent and handler are signed pointers on arm64e (PianoWifi.cpp's pattern).
// The object behind it is held by a retain of its own and by a gate (review 2026-11-14 F1): the handler takes the gate, which
// the unregister closes and then waits to be empty, so a handler in flight can never meet a freed driver.
IOSimpleLock *gA80211Gate;             // guards the two words below
PianoWifi80211 *gA80211;               // retained while set
unsigned gA80211Busy;                  // handlers inside the gate
struct sysctl_oid gA80211Oid, gA80211UcOid, gA80211ShengOid;

// The driver for a sysctl handler, retained for the call (nullptr once the unregister has closed the gate).
PianoWifi80211 *a80211Enter()
{
  if (!gA80211Gate) return nullptr;
  const IOInterruptState state = IOSimpleLockLockDisableInterrupt(gA80211Gate);
  PianoWifi80211 *self = gA80211;
  if (self) ++gA80211Busy;
  IOSimpleLockUnlockEnableInterrupt(gA80211Gate, state);
  return self;
}
void a80211Leave()
{
  const IOInterruptState state = IOSimpleLockLockDisableInterrupt(gA80211Gate);
  if (gA80211Busy) --gA80211Busy;
  IOSimpleLockUnlockEnableInterrupt(gA80211Gate, state);
}
}  // namespace

SYSCTL_DECL(_machdep);

int PianoWifi80211::sysctlAccept(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
  PianoWifi80211 *self = a80211Enter();
  struct Leave {
    PianoWifi80211 *held;
    ~Leave()
    {
      if (held) a80211Leave();
    }
  } leave{self};
  int value = self ? int(self->acceptMode()) : -1;
  int err = SYSCTL_OUT(req, &value, sizeof value);
  if (err || !req->newptr) return err;
  int incoming = 0;
  err = SYSCTL_IN(req, &incoming, sizeof incoming);
  if (err) return err;
  if (!self) return ENODEV;
  self->setAcceptMode(uint32_t(incoming), "machdep.piano_wifi_a80211");
  return 0;
}

namespace {
bool registerAcceptSysctl(PianoWifi80211 *self)
{
  gA80211Gate = IOSimpleLockAlloc();
  if (!gA80211Gate) return false;
  self->retain();   // given back by unregisterAcceptSysctl
  gA80211 = self;
  gA80211Busy = 0;
  bzero(&gA80211Oid, sizeof gA80211Oid);
  gA80211Oid.oid_parent = &sysctl__machdep_children;
  gA80211Oid.oid_number = OID_AUTO;
  gA80211Oid.oid_kind = int(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2);
  gA80211Oid.oid_name = "piano_wifi_a80211";
  gA80211Oid.oid_handler = PianoWifi80211::sysctlAccept;
  gA80211Oid.oid_fmt = "I";
  gA80211Oid.oid_descr = "PianoWifi80211's accept mode for apple80211 requests without a layout (docs/drivers/wifi-menu.md 6)";
  gA80211Oid.oid_version = SYSCTL_OID_VERSION;
  sysctl_register_oid(&gA80211Oid);
  bzero(&gA80211UcOid, sizeof gA80211UcOid);
  gA80211UcOid.oid_parent = &sysctl__machdep_children;
  gA80211UcOid.oid_number = OID_AUTO;
  gA80211UcOid.oid_kind = int(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2);
  gA80211UcOid.oid_name = "piano_wifi_a80211_uc";
  gA80211UcOid.oid_handler = PianoWifi80211::sysctlUc;
  gA80211UcOid.oid_fmt = "I";
  gA80211UcOid.oid_descr = "PianoWifi80211's user-client switches (docs/drivers/wifi-menu.md section 7)";
  gA80211UcOid.oid_version = SYSCTL_OID_VERSION;
  sysctl_register_oid(&gA80211UcOid);
  bzero(&gA80211ShengOid, sizeof gA80211ShengOid);
  gA80211ShengOid.oid_parent = &sysctl__machdep_children;
  gA80211ShengOid.oid_number = OID_AUTO;
  gA80211ShengOid.oid_kind = int(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2);
  gA80211ShengOid.oid_name = "piano_wifi_sheng";
  gA80211ShengOid.oid_handler = PianoWifi80211::sysctlSheng;
  gA80211ShengOid.oid_fmt = "I";
  gA80211ShengOid.oid_descr = "PianoWifi80211's UniXNU-sheng binding bits (docs/drivers/wifi-menu.md section 9)";
  gA80211ShengOid.oid_version = SYSCTL_OID_VERSION;
  sysctl_register_oid(&gA80211ShengOid);
  return true;
}
// The gate closed, every handler inside it waited for, then the retain given back: after this the driver may be freed.
void unregisterAcceptSysctl()
{
  sysctl_unregister_oid(&gA80211Oid);
  sysctl_unregister_oid(&gA80211UcOid);
  sysctl_unregister_oid(&gA80211ShengOid);
  if (!gA80211Gate) return;
  const IOInterruptState state = IOSimpleLockLockDisableInterrupt(gA80211Gate);
  PianoWifi80211 *self = gA80211;
  gA80211 = nullptr;
  IOSimpleLockUnlockEnableInterrupt(gA80211Gate, state);
  for (unsigned waited = 0; waited < 500; ++waited) {
    const IOInterruptState inner = IOSimpleLockLockDisableInterrupt(gA80211Gate);
    const unsigned busy = gA80211Busy;
    IOSimpleLockUnlockEnableInterrupt(gA80211Gate, inner);
    if (!busy) break;
    IOSleep(10);
  }
  IOSimpleLockFree(gA80211Gate);
  gA80211Gate = nullptr;
  if (self) self->release();
}
}  // namespace

// Only with piano-wifi-apple80211=1; otherwise PianoWifiEthernet (the same match category, a lower score) starts instead.
IOService *PianoWifi80211::probe(IOService *provider, SInt32 *score)
{
  uint32_t on = 0;
  if (!PE_parse_boot_argn("piano-wifi-apple80211", &on, sizeof on) || !on) return nullptr;
  return IO80211Controller::probe(provider, score);
}

// Inside PianoWifiEthernet::start's attachInterface, before the interface is registered (and so before the network stack
// attaches it): the subfamily and the role, which the network stack and airportd read once.
bool PianoWifi80211::configureInterface(IONetworkInterface *netif)
{
  if (!IO80211Controller::configureInterface(netif)) return false;
  fNetif = netif;
  fSubtypeTaken = piano_netif_set_subtype(netif, 3 /* IFNET_SUBFAMILY_WIFI */);
  netif->setProperty("IO80211InterfaceRole", "Infrastructure");
  // PianoWifiEthernet registers the interface before PianoWifi80211::start
  // can return. Install the grant while the interface is still configured so
  // an immediate airportd open sees it.
  if (fState && (fState->sheng & pianowifi::a80211::kShengGrant)) applyGrant(true);
  // The production form of ADOPTION.md P0: the override is on both services before this interface registers, so CoreWiFi cannot
  // see the child before the controller's identity is there (the sysctl's late set is the A/B of the same thing).
  if (fUcMode & pianowifi::a80211::kUcOverride) setOverride(true, (fUcMode & pianowifi::a80211::kUcOverrideIf) != 0);
  note80211("the interface is a Wi-Fi one: subfamily 3 %s, IO80211InterfaceRole Infrastructure",
            fSubtypeTaken ? "taken" : "REFUSED (the interface was attached already)");
  return true;
}

bool PianoWifi80211::start(IOService *provider)
{
  using namespace pianowifi::a80211;
  fLock = IOLockAlloc();
  fState = static_cast<State *>(IOMallocZeroData(sizeof(State)));
  fScratch = static_cast<uint8_t *>(IOMallocZeroData(kScratchBytes));
  fSeen = static_cast<Seen *>(IOMallocZeroData(sizeof(Seen)));
  fAttachCall = thread_call_allocate(&PianoWifi80211::attachTick, this);
  fScanDoneCall = thread_call_allocate(&PianoWifi80211::scanDoneTick, this);
  fReportCall = thread_call_allocate(&PianoWifi80211::reportTick, this);
  if (!fLock || !fState || !fScratch || !fSeen || !fAttachCall || !fScanDoneCall || !fReportCall) {
    note80211("no memory for the apple80211 state: not started");
    return false;
  }
  reset(*fState);
  uint32_t accept = 0;
  if (PE_parse_boot_argn("piano-wifi-apple80211-accept", &accept, sizeof accept)) fState->accept = accept;
  // The UniXNU-sheng binding bits (docs/drivers/wifi-menu.md section 9): what their hardware-verified macOS-27.3 en0 binding
  // does that ours does not, each its own A/B on this one front end. The medium bit was already applied where the dictionary
  // is published (PianoWifiEthernet::start read the same argument); the grant rides on the filter's attach below.
  uint32_t sheng = 0;
  if (PE_parse_boot_argn("piano-wifi-sheng", &sheng, sizeof sheng)) fState->sheng = sheng & pianowifi::a80211::kShengAll;
  // The production form of ADOPTION.md P0, confirmed on the tablet (wm3b, 2026-11-14: with the override airportd created its
  // CWFApple80211 and its SystemConfiguration objects for en2; without it, "Did NOT find matching WiFi interface"): the override
  // on the controller by default, set in configureInterface before the interface registers. piano-wifi-apple80211-uc=0 is the
  // control without it.
  uint32_t uc = pianowifi::a80211::kUcOverride;
  if (PE_parse_boot_argn("piano-wifi-apple80211-uc", &uc, sizeof uc)) uc &= 0x3f;
  fUcMode = uc;
  if (!IO80211Controller::start(provider)) return false;   // PianoWifiEthernet: the interface, its data path, the link down
  IOEthernetAddress mac = {};
  getHardwareAddress(&mac);
  for (unsigned i = 0; i < 6; ++i) fState->mac[i] = mac.bytes[i];
  if (OSDictionary *matching = IOService::serviceMatching("PianoWifi")) {
    fWifi = IOService::copyMatchingService(matching);
    matching->release();
  }
  IOLockLock(fLock);
  refreshScanLocked();
  const uint32_t networks = fState->scan.count;
  IOLockUnlock(fLock);
  setProperty("piano-wifi-apple80211", true);
  note80211("started (piano-wifi-apple80211=1): %u network(s) in PianoWifi's scan list%s; waiting for the BSD interface",
            unsigned(networks), fWifi ? "" : " (PianoWifi not found: no scan list)");
  fSysctl = registerAcceptSysctl(this);
  note80211("user-client mode %#x (machdep.piano_wifi_a80211_uc changes it at run time: 0x1 a client offered, 0x2 the controller "
            "carries IOInterfaceName, 0x4 event payloads, 0x8 the interface's class IO80211Interface (boot only), 0x10 the event "
            "pipe)", unsigned(fUcMode));
  note80211("accept mode %#x (machdep.piano_wifi_a80211 changes it at run time: 0x1 INFRA_ENUMERATED set, 0x2 any other set, "
            "0x4 any other get zeroed, 0x8 VIRTUAL_IF_ROLE answered)", unsigned(fState->accept));
  note80211("sheng mode %#x (machdep.piano_wifi_sheng changes it at run time: 0x1 the 802.11 medium selected, 0x2 SSID/BSSID "
            "events beside link ones, 0x4 card caps bit 46, 0x8 kernel-buffer copies for user-client requests, 0x10 "
            "NativeSandboxGrantProcess=airportd, 0x20 SIOCGIFMEDIA answered in the filter)", unsigned(fState->sheng));
  arm(fAttachCall, kAttachTickMs);
  return true;
}

// Every 200 ms until the network stack has attached the interface (its ifnet exists once it has a BSD name): then the filter,
// the first events and the controller's registration.
void PianoWifi80211::attachStep()
{
  if (fStopping) return;
  ifnet_t ifp = fNetif ? fNetif->getIfnet() : nullptr;
  if (!ifp) {
    if (++fAttachTries < kAttachTicks) arm(fAttachCall, kAttachTickMs);
    else note80211("no BSD interface after %u s: the apple80211 requests are not answered", kAttachTicks * kAttachTickMs / 1000);
    return;
  }
  snprintf(fIfName, sizeof fIfName, "%s%u", ifnet_name(ifp), unsigned(ifnet_unit(ifp)));
  // IO80211.framework finds the interface's service by the property IOInterfaceName (any class) and reads the role there; the
  // family sets it with the name, this only makes sure of it.
  OSObject *existingName = fNetif->copyProperty("IOInterfaceName");
  const bool named = existingName != nullptr;
  OSSafeReleaseNULL(existingName);
  if (!named) fNetif->setProperty("IOInterfaceName", fIfName);
  iff_filter filter = {};
  filter.iff_cookie = this;
  filter.iff_name = "com.unixnu.driver.PianoWifi80211";
  filter.iff_protocol = 0;   // every protocol's ioctls
  filter.iff_ioctl = &PianoWifi80211::filterIoctl;
  filter.iff_detached = &PianoWifi80211::filterDetached;
  retain();   // the filter's reference, given back in filterDetached
  const errno_t e = iflt_attach(ifp, &filter, &fFilter);
  if (e) {
    fFilter = nullptr;
    release();
    note80211("%s: the interface filter was refused (%d): the apple80211 requests are not answered", fIfName, e);
    return;
  }
  note80211("%s: apple80211 requests answered from now on (after %u ms); the BSD interface reads type %u, family %u, subfamily %u; "
            "IOInterfaceName %s", fIfName, fAttachTries * kAttachTickMs, unsigned(ifnet_type(ifp)), unsigned(ifnet_family(ifp)),
            unsigned(ifnet_subfamily(ifp)), named ? "the family's" : "set here");
  postEvent(pianowifi::a80211::kEvPowerChanged);
  pianowifi::a80211::LinkEvent down = {};
  down.down = 1;
  postEvent(pianowifi::a80211::kEvLinkChanged, &down, sizeof down);
  // sheng bit 0x2: their post_link_down sends SSID_CHANGED and BSSID_CHANGED beside every LINK_CHANGED (WifiNet.cpp:195-197) —
  // the two codes the framework's state machine waits for beside the link's own.
  if (shengMode() & pianowifi::a80211::kShengEvents) {
    postEvent(pianowifi::a80211::kEvSsidChanged);
    postEvent(pianowifi::a80211::kEvBssidChanged);
  }
  if (fUcMode & pianowifi::a80211::kUcControllerName) {
    // The lookup user space makes by IOInterfaceName takes any class: with these the controller can be what it finds and opens.
    setProperty("IOInterfaceName", fIfName);
    setProperty("IO80211InterfaceRole", "Infrastructure");
    setProperty("IOInterfaceNamePrefix", "en");
    note80211("%s: the controller carries IOInterfaceName and the Infrastructure role too (user-client mode 0x2)", fIfName);
  }
  setProperty("piano-wifi-apple80211-answering", fIfName);   // the BSD name; the airportd starter waits for this key
  noteIdentity("at the filter's attach");
  registerService();
  fRegistered = true;
  note80211("%s: the controller is registered (class chain PianoWifi80211 < IO80211Controller < PianoWifiEthernet): airportd's "
            "IOKit match may fire", fIfName);
  arm(fReportCall, 5000);   // the first summary soon (who matched the controller), then every minute
}

void PianoWifi80211::attachTick(thread_call_param_t p, thread_call_param_t)
{
  auto *self = static_cast<PianoWifi80211 *>(p);
  Tick tick(self);
  if (tick.run) self->attachStep();
}

errno_t PianoWifi80211::filterIoctl(void *cookie, ifnet_t, protocol_family_t, unsigned long cmd, void *arg)
{
  auto *self = static_cast<PianoWifi80211 *>(cookie);
  if (!self || !arg) return EOPNOTSUPP;
  if (cmd == pianowifi::a80211::kSiocSA80211 || cmd == pianowifi::a80211::kSiocGA80211)
    return self->apple80211(cmd == pianowifi::a80211::kSiocSA80211, *static_cast<pianowifi::a80211::Req *>(arg));
  // sheng bit 0x20: SIOCGIFMEDIA answered in the filter outright, their if_ioctl form (WifiNet.cpp:712-719), for the case the
  // family's own answer (bit 0x1's selected medium) lags or never carries the 802.11 type.
  if (cmd == SIOCGIFMEDIA && (self->shengMode() & pianowifi::a80211::kShengMediaFilter)) {
    const errno_t e = self->mediaIoctl(static_cast<struct ifmediareq *>(arg));
    static unsigned said;
    if (said++ < 8) note80211("%s: SIOCGIFMEDIA answered in the filter (%d): IEEE 802.11 auto (sheng 0x20)", self->fIfName, e);
    return e ? e : EJUSTRETURN;
  }
  self->noteOtherIoctl(cmd);
  return EOPNOTSUPP;   // not ours: the protocol and the interface handle it as before
}

// The stack has freed the filter (our detach, or the interface's own teardown): nothing may be detached after this.
void PianoWifi80211::filterDetached(void *cookie, ifnet_t)
{
  auto *self = static_cast<PianoWifi80211 *>(cookie);
  self->fFilter = nullptr;
  self->fFilterGone = true;
  self->release();
}

// The user client's copy pair's context (sheng bit 0x8, ucRequest below): the two structure buffers the client call carries —
// kernel memory — and the sheng snapshot taken before the request, so ucIn/ucOut can fall back to memcpy for a copy whose
// address lies inside one of them (WifiApple80211.h shengKernelBuffer; their macOS-27.3 EFAULT fix).
struct UcIo {
  uint8_t *inBase = nullptr;    // the client's structure buffers: kernel memory, written by the memcpy fallback
  uint32_t inBytes = 0;
  uint8_t *outBase = nullptr;
  uint32_t outBytes = 0;
  uint32_t sheng = 0;
  bool inlineData = false;       // the sheng user-client request carries its data in the input structure
};

int PianoWifi80211::ucIn(void *ctx, uint64_t from, void *to, uint32_t length)
{
  auto *uc = static_cast<UcIo *>(ctx);
  uint32_t offset = 0;
  if (uc->inlineData && pianowifi::a80211::shengKernelBuffer(from, length, uc->inBase, uc->inBytes, offset)) {
    memcpy(to, uc->inBase + offset, length);
    return 0;
  }
  const int e = copyin(from, to, length);
  if (!e || !(uc->sheng & pianowifi::a80211::kShengUcKernel)) return e;
  if (!pianowifi::a80211::shengKernelBuffer(from, length, uc->inBase, uc->inBytes, offset)) return e;
  memcpy(to, uc->inBase + offset, length);
  return 0;
}

int PianoWifi80211::ucOut(void *ctx, const void *from, uint64_t to, uint32_t length)
{
  auto *uc = static_cast<UcIo *>(ctx);
  uint32_t offset = 0;
  if (uc->inlineData && pianowifi::a80211::shengKernelBuffer(to, length, uc->inBase, uc->inBytes, offset)) {
    memcpy(uc->inBase + offset, from, length);
    return 0;
  }
  const int e = copyout(from, to, length);
  if (!e || !(uc->sheng & pianowifi::a80211::kShengUcKernel)) return e;
  if (pianowifi::a80211::shengKernelBuffer(to, length, uc->outBase, uc->outBytes, offset)) {
    memcpy(uc->outBase + offset, from, length);
    return 0;
  }
  if (pianowifi::a80211::shengKernelBuffer(to, length, uc->inBase, uc->inBytes, offset)) {
    memcpy(uc->inBase + offset, from, length);
    return 0;
  }
  return e;
}

// One request, in the requesting thread (airportd's ioctl, or the user client's selector 0 through ucRequest): the answer under
// the lock, its log line and its effects after. `ioOverride` is the user client's copy pair (a request whose data rides inside
// the client's own structure); without it the plain copyin/copyout of the ioctl path.
// F3 (review 2026-11-14, accepted): the user copies happen under fLock, because the state and the one scratch buffer are shared.
// A paged-out user buffer therefore stalls other apple80211 requests, the summary line and the accept sysctl — never the data
// path, the link or PianoWifi's ladder, which share no lock with this. The buffers are airportd's own, small and touched again
// and again; a separate per-request buffer would be the fix if that ever shows up on the tablet.
errno_t PianoWifi80211::apple80211(bool set, pianowifi::a80211::Req &rq, const pianowifi::a80211::UserIo *ioOverride)
{
  using namespace pianowifi::a80211;
  const int32_t type = rq.type, value = rq.value;
  const uint32_t length = rq.length;
  const UserIo plain = {nullptr, &PianoWifi80211::userIn, &PianoWifi80211::userOut};
  const UserIo &io = ioOverride ? *ioOverride : plain;
  // F6 (review 2026-11-14): the request carries the interface's name; one naming another interface is refused, even though the
  // BSD layer routed it to this ifnet (the filter sees whatever the caller sent).
  if (rq.name[0] && strncmp(rq.name, fIfName, sizeof rq.name) != 0) {
    char named[17] = {};
    memcpy(named, rq.name, sizeof rq.name);
    static unsigned said;
    if (said++ < 8) note80211("%s: a request of type %d names %.16s, not this interface: ENXIO", fIfName, int(type), named);
    return ENXIO;
  }
  const bool scanSet = set && (type == kScanReq || type == kScanReqMultiple);
  uint32_t directed = 0, scanType = 0xffffffffu;
  IOLockLock(fLock);
  if (scanSet) {
    refreshScanLocked();
    if (type == kScanReq) scanReqInfo(rq, io, directed, scanType);
  }
  const Answer a = handle(*fState, kIdentity, set, rq, io, fScratch);
  const bool first = fSeen->first(type, set, length, a.error);
  ++fRequests;
  if (a.error == kErrUnsupported) ++fUnknown;
  const uint32_t networks = fState->scan.count, cursor = fState->cursor;
  const bool power = fState->power;
  IOLockUnlock(fLock);
  if (first) {
    char extra[64] = "";
    if (scanSet) snprintf(extra, sizeof extra, "; %u network(s)%s", unsigned(networks), directed ? ", a directed scan" : "");
    else if (type == kScanResult && !set) snprintf(extra, sizeof extra, "; cursor %u of %u", unsigned(cursor), unsigned(networks));
    note80211("%s: %s %d %s len %u val %d -> %d (wrote %u, len %u, val %d)%s%s", fIfName, set ? "set" : "get", int(type),
              requestName(type), unsigned(length), int(value), a.error, unsigned(a.wrote), unsigned(rq.length), int(rq.value), extra,
              a.accepted ? " [accept mode]" : "");
  }
  if (a.effects & kDoPowerChanged) {
    note80211("%s: POWER set %s by airportd (M1: scans %s; the chip stays up)", fIfName, power ? "on" : "off",
              power ? "answered again" : "refused with EPWROFF");
    postEvent(kEvPowerChanged);
  }
  // Every scan request and every list end, with a serial and the asking process (ADOPTION.md 6.1 item 3): that is how a
  // client's scan through airportd — CoreWLAN's, which came back empty in 15 ms — is told from airportd's own scheduled ones.
  if (scanSet || (!set && type == kScanResult)) {
    uint64_t serial = 0;
    uint32_t records = 0;
    bool end = false;
    IOLockLock(fLock);
    if (scanSet) {
      serial = ++fScanSerial;
      fScanRecords = 0;
    } else {
      serial = fScanSerial;
      if (a.error == kErrEndOfResults) {
        end = true;
        records = fScanRecords;
      } else if (!a.error) {
        ++fScanRecords;
      }
    }
    IOLockUnlock(fLock);
    if ((scanSet || end) && __atomic_fetch_add(&fScanLines, 1u, __ATOMIC_RELAXED) < kScanLinesMax) {
      char proc[32] = "?";
      proc_selfname(proc, sizeof proc);
      if (scanSet)
        note80211("%s: scan %llu asked by %s[%d]: %s len %u -> %d, scan type %s (%u network(s) in the list%s)", fIfName, serial,
                  proc, proc_selfpid(), requestName(type), unsigned(length), a.error,
                  scanType == 0xffffffffu ? "?" : (scanType == kScanTypeCached ? "3 (the held results)" : "other"),
                  unsigned(networks), directed ? ", a directed scan" : "");
      else
        note80211("%s: scan %llu read to its end by %s[%d]: %u record(s) served, the list end answered %d", fIfName, serial, proc,
                  proc_selfpid(), unsigned(records), a.error);
    }
  }
  if (a.effects & kDoScanStarted) scanStarted();
  return a.error ? a.error : EJUSTRETURN;   // EJUSTRETURN: answered, nothing after this filter runs (ifnet_ioctl makes it 0)
}

// PianoWifi's scan list (the registry property of level 6: PianoWifi.cpp publishScan) into the compact snapshot.
void PianoWifi80211::refreshScanLocked()
{
  using namespace pianowifi::a80211;
  ScanSnapshot &snap = fState->scan;
  snap.count = 0;
  OSObject *property = fWifi ? fWifi->copyProperty("piano-wifi-scan") : nullptr;
  OSArray *list = OSDynamicCast(OSArray, property);
  for (unsigned i = 0; list && i < list->getCount() && snap.count < pianowifi::kScanEntriesMax; ++i) {
    OSDictionary *d = OSDynamicCast(OSDictionary, list->getObject(i));
    if (!d) continue;
    Bss &b = snap.bss[snap.count];
    bzero(&b, sizeof b);
    OSString *bssid = OSDynamicCast(OSString, d->getObject("bssid"));
    if (!bssid || !parseBssid(bssid->getCStringNoCopy(), b.bssid)) continue;
    if (OSData *raw = OSDynamicCast(OSData, d->getObject("ssid-raw"))) {
      b.ssidLength = uint8_t(raw->getLength() <= pianowifi::kSsidMax ? raw->getLength() : pianowifi::kSsidMax);
      memcpy(b.ssid, raw->getBytesNoCopy(), b.ssidLength);
    } else if (OSString *ssid = OSDynamicCast(OSString, d->getObject("ssid"))) {
      b.ssidLength = uint8_t(ssid->getLength() <= pianowifi::kSsidMax ? ssid->getLength() : pianowifi::kSsidMax);
      memcpy(b.ssid, ssid->getCStringNoCopy(), b.ssidLength);
    }
    auto number = [d](const char *key) -> uint32_t {
      OSNumber *n = OSDynamicCast(OSNumber, d->getObject(key));
      return n ? n->unsigned32BitValue() : 0;
    };
    b.channel = uint16_t(number("channel"));
    b.freq = number("freq");
    b.rssi = int32_t(number("rssi"));
    b.capability = uint16_t(number("capability"));
    b.beaconInterval = uint16_t(number("beacon-interval"));
    if (OSData *rates = OSDynamicCast(OSData, d->getObject("rates"))) {
      b.rateCount = uint8_t(rates->getLength() <= sizeof b.rates ? rates->getLength() : sizeof b.rates);
      memcpy(b.rates, rates->getBytesNoCopy(), b.rateCount);
    }
    if (OSData *rsn = OSDynamicCast(OSData, d->getObject("rsn"))) {
      b.rsnLength = uint8_t(rsn->getLength() <= sizeof b.rsn ? rsn->getLength() : 0);
      memcpy(b.rsn, rsn->getBytesNoCopy(), b.rsnLength);
    }
    b.hidden = d->getObject("hidden") == kOSBooleanTrue;
    ++snap.count;
  }
  OSSafeReleaseNULL(property);
  ++fListRefreshes;
}

// kUcEventPayloads (wm2-diag Rank 3): the payload a 27.3 reader expects for the events we post without one. POWER_CHANGED: the
// power structure (the same {version, radios, state} the POWER get answers). SCAN_DONE: a status word (0: the scan is done).
// Both are our own reading of the layouts the get answers carry; nothing was copied.
void PianoWifi80211::eventPayload(uint32_t code, uint8_t *buffer, uint32_t capacity, uint32_t &length)
{
  using namespace pianowifi::a80211;
  length = 0;
  if (!(ucMode() & kUcEventPayloads) || !buffer) return;
  if (code == kEvPowerChanged && capacity >= sizeof(Power)) {
    Power p;
    bzero(&p, sizeof p);
    p.version = 1;
    p.radios = 1;
    IOLockLock(fLock);
    p.state[0] = fState->power ? 1 : 0;
    IOLockUnlock(fLock);
    memcpy(buffer, &p, sizeof p);
    length = sizeof p;
  } else if (code == kEvScanDone && capacity >= 8) {
    const uint32_t done[2] = {1, 0};   // {version, status 0: the scan finished}
    memcpy(buffer, done, sizeof done);
    length = sizeof done;
  }
}

// Vendor Apple, class 6 (IEEE 802.11), subclass 1 (apple80211), the interface's name (16 bytes) and the payload.
void PianoWifi80211::postEvent(uint32_t code, const void *payload, uint32_t length)
{
  char name[16] = {};
  memcpy(name, fIfName, sizeof name);
  uint8_t made[32];
  if (!payload || !length) {
    uint32_t madeLength = 0;
    eventPayload(code, made, sizeof made, madeLength);
    if (madeLength) {
      payload = made;
      length = madeLength;
    }
  }
  struct kev_msg m;
  bzero(&m, sizeof m);
  m.vendor_code = KEV_VENDOR_APPLE;
  m.kev_class = pianowifi::a80211::kEventClass;
  m.kev_subclass = pianowifi::a80211::kEventSubclass;
  m.event_code = code;
  m.dv[0].data_length = sizeof name;
  m.dv[0].data_ptr = name;
  if (payload && length) {
    m.dv[1].data_length = length;
    m.dv[1].data_ptr = const_cast<void *>(payload);
  }
  const int e = piano_kev_post_msg(&m);
  IOLockLock(fLock);
  ++fEvents;
  if (e) ++fEventErrors;
  const bool first = code < 32 && !(fEventCodesLogged & (1u << code));
  if (code < 32) fEventCodesLogged |= 1u << code;
  IOLockUnlock(fLock);
  if (first || e) note80211("%s: event %u (%u payload bytes) posted -> %d", fIfName, unsigned(code), unsigned(length), e);
}

// One completion per accepted scan: an earlier one still waiting is posted at once (thread_call_cancel is true only for a call
// that had not started; one that runs posts its own), this one's after kScanDoneMs.
void PianoWifi80211::scanStarted()
{
  bool schedule = false;
  IOLockLock(fLock);
  ++fScanDonePending;
  if (!fScanDoneArmed) {
    fScanDoneArmed = true;
    schedule = true;
  }
  IOLockUnlock(fLock);
  if (schedule) arm(fScanDoneCall, kScanDoneMs);
}

void PianoWifi80211::scanDoneTick(thread_call_param_t p, thread_call_param_t)
{
  auto *self = static_cast<PianoWifi80211 *>(p);
  Tick tick(self);
  if (!tick.run) return;
  uint32_t pending = 0;
  IOLockLock(self->fLock);
  pending = self->fScanDonePending;
  self->fScanDonePending = 0;
  self->fScanDoneArmed = false;
  IOLockUnlock(self->fLock);
  while (pending--) self->postEvent(pianowifi::a80211::kEvScanDone);
}

void PianoWifi80211::noteOtherIoctl(unsigned long cmd)
{
  IOLockLock(fLock);
  ++fOtherIoctls;
  bool first = true;
  for (unsigned i = 0; i < fOtherCount; ++i) first = first && fOtherSeen[i] != cmd;
  if (first && fOtherCount < sizeof fOtherSeen / sizeof fOtherSeen[0]) fOtherSeen[fOtherCount++] = cmd;
  else first = false;
  IOLockUnlock(fLock);
  if (first)
    note80211("%s: ioctl %#lx (group '%c', number %lu, %lu bytes) passed on", fIfName, cmd, char((cmd >> 8) & 0xff), cmd & 0xff,
              (cmd >> 16) & 0x1fff);
}

uint32_t PianoWifi80211::ucMode()
{
  IOLockLock(fLock);
  const uint32_t mode = fUcMode;
  IOLockUnlock(fLock);
  return mode;
}

// The discovery identity 27.3's CoreWiFi reads, on one line (ADOPTION.md 6.1 item 2): the controller's override, the
// interface's class and its names.
void PianoWifi80211::noteIdentity(const char *why)
{
  char override[48] = "(absent)", ifClass[48] = "?", role[32] = "(absent)", name[20] = "(absent)", prefix[12] = "(absent)";
  int unit = -1;
  auto text = [](IORegistryEntry *where, const char *key, char *out, size_t size) {
    if (!where) return;
    OSObject *o = where->copyProperty(key);
    if (OSString *str = OSDynamicCast(OSString, o)) strlcpy(out, str->getCStringNoCopy(), size);
    OSSafeReleaseNULL(o);
  };
  text(this, "IOClassNameOverride", override, sizeof override);
  if (fNetif) {
    strlcpy(ifClass, fNetif->getMetaClass() ? fNetif->getMetaClass()->getClassName() : "?", sizeof ifClass);
    text(fNetif, "IO80211InterfaceRole", role, sizeof role);
    text(fNetif, "IOInterfaceName", name, sizeof name);
    text(fNetif, "IOInterfaceNamePrefix", prefix, sizeof prefix);
    OSObject *o = fNetif->copyProperty("IOInterfaceUnit");
    if (OSNumber *n = OSDynamicCast(OSNumber, o)) unit = int(n->unsigned32BitValue());
    OSSafeReleaseNULL(o);
  }
  char ifOverride[48] = "(absent)";
  if (fNetif) text(fNetif, "IOClassNameOverride", ifOverride, sizeof ifOverride);
  note80211("identity (%s): controller class PianoWifi80211 < IO80211Controller, IOClassNameOverride %s; interface class %s, "
            "IOClassNameOverride %s, IO80211InterfaceRole %s, IOInterfaceName %s, IOInterfaceNamePrefix %s, IOInterfaceUnit %d; "
            "BSD %s", why, override, ifClass, ifOverride, role, name, prefix, unit, fIfName[0] ? fIfName : "(none yet)");
}

// IOClassNameOverride = "IO80211Controller" (ADOPTION.md P0): the one discovery input of 27.3's CoreWiFi that we lack. CoreWiFi
// reads registry properties (it imports no IOObjectConformsTo), so the class-name chain that satisfies launchd and
// SystemConfiguration says nothing to it.
void PianoWifi80211::setOverride(bool on, bool onInterface)
{
  if (on) setProperty("IOClassNameOverride", "IO80211Controller");
  else removeProperty("IOClassNameOverride");
  if (fNetif) {
    if (onInterface) fNetif->setProperty("IOClassNameOverride", "IO80211Controller");
    else fNetif->removeProperty("IOClassNameOverride");
  }
}

// The sheng path posts the link event together with the SSID/BSSID state
// events whenever the association changes.  The initial attach has its own
// down event above; this hook covers later link callbacks from PianoWifi's
// Ethernet personality.
void PianoWifi80211::linkChanged(bool up)
{
  if (!(shengMode() & pianowifi::a80211::kShengEvents)) return;
  pianowifi::a80211::LinkEvent event = {};
  event.down = up ? 0 : 1;
  if (up) {
    postEvent(pianowifi::a80211::kEvSsidChanged);
    postEvent(pianowifi::a80211::kEvBssidChanged);
    postEvent(pianowifi::a80211::kEvLinkChanged, &event, sizeof event);
  } else {
    postEvent(pianowifi::a80211::kEvLinkChanged, &event, sizeof event);
    postEvent(pianowifi::a80211::kEvSsidChanged);
    postEvent(pianowifi::a80211::kEvBssidChanged);
  }
}

// sheng bit 0x20's answer, their if_ioctl form word for word (WifiNet.cpp:712-719): "the framework lists wireless interfaces
// by this media type". ifm_name is the caller's; the five answer words are ours. ifm_count 1 with no list written is their
// answer too (the reader takes the current word, not the list).
errno_t PianoWifi80211::mediaIoctl(struct ifmediareq *m)
{
  if (!m) return EINVAL;
  m->ifm_current = IFM_IEEE80211 | IFM_AUTO;
  m->ifm_mask = 0;
  m->ifm_status = IFM_AVALID | (fLinkUp ? IFM_ACTIVE : 0);
  m->ifm_active = IFM_IEEE80211 | IFM_AUTO;
  m->ifm_count = 1;
  return 0;
}

// sheng bit 0x1: the 802.11 medium is selected while the bit is on, the Ethernet one otherwise (both stay published — the
// dictionary was built with them in PianoWifiEthernet::start). The family answers SIOCGIFMEDIA from the selection, so the
// re-select announces the link status again with the chosen medium, as a link change does.
void PianoWifi80211::applyMedium()
{
  const bool on11 = (shengMode() & pianowifi::a80211::kShengMedium) != 0 && fMedium11 != nullptr;
  IONetworkMedium *chosen = on11 ? fMedium11 : fMedium;
  if (!chosen || !fMedium11 || on11 == fMedium11On) return;
  fMedium11On = on11;
  setSelectedMedium(chosen);
  setLinkStatus(fLinkUp ? (kIONetworkLinkValid | kIONetworkLinkActive) : kIONetworkLinkValid, chosen, fLinkUp ? kLinkSpeed : 0);
  note80211("%s: the selected medium is %s (the family's SIOCGIFMEDIA answer follows it)", fIfName,
            on11 ? "IEEE 802.11 auto" : "Ethernet auto full duplex");
}

// sheng bit 0x10: their NativeSandboxGrantProcess on the interface (ShengEthernet.cpp:283), the kernel-side grant that stands
// in for the sandbox's verdict when the named process opens the service's user client. Ours is exactly "airportd": our kernel's
// native_sandbox_grant (the k27 IOUserClient patch) compares the whole property with one process name, so their
// "airportd,sharingd,wifivelocityd" would never match here. The controller carries it too — it is the service our user client is
// offered on, and what a lookup by IOInterfaceName (user-client mode 0x2) finds.
void PianoWifi80211::applyGrant(bool on)
{
  if (on) {
    setProperty("NativeSandboxGrantProcess", "airportd");
    if (fNetif) fNetif->setProperty("NativeSandboxGrantProcess", "airportd");
  } else {
    removeProperty("NativeSandboxGrantProcess");
    if (fNetif) fNetif->removeProperty("NativeSandboxGrantProcess");
  }
  note80211("%s: NativeSandboxGrantProcess %s (the kernel's native_sandbox_grant stands in for the sandbox verdict at the open)",
            fIfName, on ? "= airportd" : "removed");
}

void PianoWifi80211::setUcMode(uint32_t mode, const char *why)
{
  IOLockLock(fLock);
  const uint32_t was = fUcMode;
  fUcMode = mode & 0x3f;
  IOLockUnlock(fLock);
  using namespace pianowifi::a80211;
  if ((fUcMode & (kUcOverride | kUcOverrideIf)) != (was & (kUcOverride | kUcOverrideIf)))
    setOverride((fUcMode & kUcOverride) != 0, (fUcMode & kUcOverrideIf) != 0);
  // kUcControllerName takes effect at once; the interface's class is decided when the interface is made (a boot argument only).
  const bool name = (fUcMode & pianowifi::a80211::kUcControllerName) != 0;
  if (name != ((was & pianowifi::a80211::kUcControllerName) != 0)) {
    if (name) {
      setProperty("IOInterfaceName", fIfName);
      setProperty("IO80211InterfaceRole", "Infrastructure");
      setProperty("IOInterfaceNamePrefix", "en");
    } else {
      removeProperty("IOInterfaceName");
    }
  }
  note80211("%s: stage mode %#x (was %#x, %s): 0x1 a user client offered, 0x2 the controller carries IOInterfaceName%s, "
            "0x4 event payloads, 0x8 IOClassNameOverride on the controller, 0x10 the event pipe, 0x20 the override on the "
            "interface too", fIfName, unsigned(fUcMode), unsigned(was), why, name ? " (set now)" : "");
  noteIdentity(why);
}

// Every open logged, whoever asks and whatever it asks for: that is the question wm3 answers.
IOReturn PianoWifi80211::newUserClient(task_t owningTask, void *security, UInt32 type, OSDictionary *properties,
                                       IOUserClient **handler)
{
  const uint32_t mode = ucMode();
  if (!(mode & pianowifi::a80211::kUcOffer)) {
    IOLockLock(fLock);
    ++fUcRefused;
    IOLockUnlock(fLock);
    noteUserClient("refused (no client offered: mode 0x1 is off)", owningTask, uint32_t(type), 0);
    return IO80211Controller::newUserClient(owningTask, security, type, properties, handler);
  }
  PianoWifi80211Uc *client = OSTypeAlloc(PianoWifi80211Uc);
  if (!client || !client->initWithOwner(this, owningTask, uint32_t(type))) {
    OSSafeReleaseNULL(client);
    IOLockLock(fLock);
    ++fUcRefused;
    IOLockUnlock(fLock);
    noteUserClient("refused (no memory)", owningTask, uint32_t(type), 0);
    return kIOReturnNoMemory;
  }
  if (!client->attach(this) || !client->start(this)) {
    client->detach(this);
    client->release();
    IOLockLock(fLock);
    ++fUcRefused;
    IOLockUnlock(fLock);
    noteUserClient("refused (the client did not start)", owningTask, uint32_t(type), 0);
    return kIOReturnError;
  }
  IOLockLock(fLock);
  ++fUcOpens;
  ++fUcClients;
  IOLockUnlock(fLock);
  noteUserClient("opened", owningTask, uint32_t(type), 0);
  *handler = client;
  return kIOReturnSuccess;
}

// An apple80211 request through the client: the five fields of the structure read, the request answered by the same handler the
// ioctls use, and the request's own length and value written back into the caller's structure output.
IOReturn PianoWifi80211::ucRequest(IOExternalMethodArguments *args, bool large, uint32_t &answered)
{
  using namespace pianowifi::a80211;
  const uint32_t want = large ? kUcLargeBytes : kUcSmallBytes;
  const uint32_t offset = large ? kUcLargeBytes - kUcSmallBytes : 0;
  const uint8_t *in = static_cast<const uint8_t *>(args->structureInput);
  if (!in || args->structureInputSize < want) return kIOReturnBadArgument;
  // Keep the field reads below inside the structure supplied by IOUserClient.
  // The selector-6 size/offset pair is a host-derived ABI fact and must not be
  // trusted until the 40-byte request fits; a stale large-layout constant must
  // fail closed instead of becoming an out-of-bounds kernel read.
  if (offset > args->structureInputSize || kUcNameAt > args->structureInputSize - offset ||
      sizeof(Req) > args->structureInputSize - offset - kUcNameAt) return kIOReturnBadArgument;
  uint32_t cmd = 0, length = 0, type = 0;
  int32_t val = 0;
  uint64_t data = 0;
  memcpy(&cmd, in + kUcCmdAt, 4);
  memcpy(&length, in + kUcLenAt, 4);
  memcpy(&data, in + kUcDataAt, 8);
  memcpy(&type, in + offset + kUcTypeAt, 4);
  memcpy(&val, in + offset + kUcValAt, 4);
  if (cmd != kUcGetCmd && cmd != kUcSetCmd) return kIOReturnBadArgument;
  Req rq;
  bzero(&rq, sizeof rq);
  memcpy(rq.name, in + offset + kUcNameAt, sizeof rq.name);
  rq.name[sizeof rq.name - 1] = 0;
  rq.type = int32_t(type);
  rq.value = val;
  rq.length = length;
  rq.data = data;
  // The client's copies (sheng bit 0x8): the request's data may ride inside the structure the client passed, kernel memory the
  // kext holds, where copyin/copyout fault (their macOS-27.3 EFAULT fix is memcpy); ucIn/ucOut fall back to that only for a
  // copy whose address lies inside one of the two structure buffers, and only while the bit is on.
  UcIo ucio;
  ucio.inBase = const_cast<uint8_t *>(in);   // the memcpy fallback writes into it (the kext owns the copy)
  ucio.inBytes = args->structureInputSize;
  ucio.outBase = static_cast<uint8_t *>(args->structureOutput);
  ucio.outBytes = args->structureOutputSize;
  ucio.sheng = shengMode();
  if ((ucio.sheng & kShengUcKernel) && args->structureInputSize > kUcDataAt) {
    // Sheng's interface client marks this condition in apple80211req and
    // points req_data at its inline data area. Use the same explicit kernel
    // buffer path here instead of probing copyout and guessing from EFAULT.
    ucio.inlineData = true;
    ucio.inBase = const_cast<uint8_t *>(in + kUcDataAt);
    ucio.inBytes = args->structureInputSize - kUcDataAt;
    rq.data = reinterpret_cast<uint64_t>(ucio.inBase);
  }
  const UserIo uio = {&ucio, &PianoWifi80211::ucIn, &PianoWifi80211::ucOut};
  const errno_t e = apple80211(cmd == kUcSetCmd, rq, &uio);
  // The answer's own length and value go back to the caller, as the ioctl's struct would carry them.
  uint8_t *out = static_cast<uint8_t *>(args->structureOutput);
  if (out && args->structureOutputSize >= want) {
    memcpy(out, in, want);
    memcpy(out + kUcLenAt, &rq.length, 4);
    memcpy(out + offset + kUcValAt, &rq.value, 4);
    answered = want;
  } else if (out && args->structureOutputSize >= 8) {
    memcpy(out, &rq.length, 4);
    memcpy(out + 4, &rq.value, 4);
    answered = 8;
  }
  IOLockLock(fLock);
  ++fUcSelectors;
  IOLockUnlock(fLock);
  if (e == EJUSTRETURN || e == 0) return kIOReturnSuccess;
  // The driver's own errno reaches user space here (which the ioctl path cannot do): 5 ends a scan's result list.
  return unsigned(e) & 0x3fff ? IOReturn(sub_iokit_common | (unsigned(e) & 0x3fff)) : kIOReturnError;
}

void PianoWifi80211::noteUserClient(const char *what, task_t task, uint32_t type, uint64_t calls)
{
  char name[32] = "?";
  int pid = -1;
  if (task) {
    proc_t p = (proc_t)get_bsdtask_info(task);
    if (p) {
      pid = proc_pid(p);
      proc_name(pid, name, sizeof name);
    }
  }
  note80211("%s: a user client was %s: type %u, task %s[%d], %llu call(s) so far (opens %llu, refused %llu, alive %u)", fIfName,
            what, unsigned(type), name, pid, calls, fUcOpens, fUcRefused, unsigned(fUcClients));
}

void PianoWifi80211::noteSelector(uint32_t selector, uint32_t inBytes, uint32_t outBytes, uint32_t scalarIn, uint32_t scalarOut,
                                  uint32_t answered, IOReturn result, uint64_t calls)
{
  note80211("%s: selector %u: struct in %u out %u, scalars in %u out %u -> %#x (wrote %u), call %llu", fIfName,
            unsigned(selector), unsigned(inBytes), unsigned(outBytes), unsigned(scalarIn), unsigned(scalarOut),
            unsigned(answered), unsigned(result), calls);
}

int PianoWifi80211::sysctlUc(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
  PianoWifi80211 *self = a80211Enter();
  struct Leave {
    PianoWifi80211 *held;
    ~Leave()
    {
      if (held) a80211Leave();
    }
  } leave{self};
  int value = self ? int(self->ucMode()) : -1;
  int err = SYSCTL_OUT(req, &value, sizeof value);
  if (err || !req->newptr) return err;
  int incoming = 0;
  err = SYSCTL_IN(req, &incoming, sizeof incoming);
  if (err) return err;
  if (!self) return ENODEV;
  self->setUcMode(uint32_t(incoming), "machdep.piano_wifi_a80211_uc");
  return 0;
}

uint32_t PianoWifi80211::acceptMode()
{
  IOLockLock(fLock);
  const uint32_t mode = fState->accept;
  IOLockUnlock(fLock);
  return mode;
}

// A new accept mode: the kind table starts again, so the log shows every kind airportd asks from now on with the new answers.
void PianoWifi80211::setAcceptMode(uint32_t mode, const char *why)
{
  IOLockLock(fLock);
  const uint32_t was = fState->accept;
  fState->accept = mode & 0xf;
  fSeen->clear();
  IOLockUnlock(fLock);
  note80211("%s: accept mode %#x (was %#x, %s): every request kind is logged again from here", fIfName, unsigned(mode & 0xf),
            unsigned(was), why);
}

uint32_t PianoWifi80211::shengMode()
{
  IOLockLock(fLock);
  const uint32_t mode = fState->sheng;
  IOLockUnlock(fLock);
  return mode;
}

// A new set of the UniXNU-sheng binding bits (docs/drivers/wifi-menu.md section 9): each bit its own A/B, the ones with a state
// beyond the answers applied at once — the medium re-selected (0x1), the grant property set or removed (0x10). The kind table
// starts again so every request is re-logged with the new answers (bit 0x4 changes CARD_CAPABILITIES).
void PianoWifi80211::setShengMode(uint32_t mode, const char *why)
{
  using namespace pianowifi::a80211;
  const uint32_t masked = mode & kShengAll;
  IOLockLock(fLock);
  const uint32_t was = fState->sheng;
  fState->sheng = masked;
  fSeen->clear();
  IOLockUnlock(fLock);
  if (((masked ^ was) & kShengMedium) && fMedium11) applyMedium();
  if ((masked ^ was) & kShengGrant) applyGrant((masked & kShengGrant) != 0);
  note80211("%s: sheng mode %#x (was %#x, %s): 0x1 the 802.11 medium selected, 0x2 SSID/BSSID events beside link ones, 0x4 card "
            "caps bit 46, 0x8 kernel-buffer copies for user-client requests, 0x10 NativeSandboxGrantProcess, 0x20 SIOCGIFMEDIA "
            "in the filter; every request kind is logged again from here",
            fIfName, unsigned(masked), unsigned(was), why);
}

int PianoWifi80211::sysctlSheng(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
  PianoWifi80211 *self = a80211Enter();
  struct Leave {
    PianoWifi80211 *held;
    ~Leave()
    {
      if (held) a80211Leave();
    }
  } leave{self};
  int value = self ? int(self->shengMode()) : -1;
  int err = SYSCTL_OUT(req, &value, sizeof value);
  if (err || !req->newptr) return err;
  int incoming = 0;
  err = SYSCTL_IN(req, &incoming, sizeof incoming);
  if (err) return err;
  if (!self) return ENODEV;
  self->setShengMode(uint32_t(incoming), "machdep.piano_wifi_sheng");
  return 0;
}

void PianoWifi80211::report(const char *why)
{
  IOLockLock(fLock);
  const uint64_t requests = fRequests, unknown = fUnknown, events = fEvents, eventErrors = fEventErrors, other = fOtherIoctls;
  const uint32_t kinds = fSeen->count, overflow = fSeen->overflow, scans = fState->scanRequests, results = fState->resultsServed;
  const uint32_t ends = fState->listEnds, networks = fState->scan.count, powerSets = fState->powerSets;
  const bool power = fState->power;
  const uint32_t accept = fState->accept, accepted = fState->acceptedUnknown, ucMode = fUcMode, ucClients = fUcClients;
  const uint64_t ucOpens = fUcOpens, ucRefused = fUcRefused, ucSelectors = fUcSelectors, serial = fScanSerial;
  const uint64_t stamp = requests + events + other;
  const bool changed = stamp != fReported;
  fReported = stamp;
  IOLockUnlock(fLock);
  if (!changed) return;
  unsigned clients = 0;
  char names[96] = "";
  if (OSIterator *it = getClientIterator()) {
    while (OSObject *o = it->getNextObject()) {
      const size_t at = strlen(names);
      if (clients++ < 3 && at + 2 < sizeof names)
        snprintf(names + at, sizeof names - at, "%s%s", at ? " " : "", o->getMetaClass()->getClassName());
    }
    it->release();
  }
  // The client line goes first and alone: in wm2 four of six summaries were cut by interleaving and this tail was lost.
  note80211("%s (%s): the controller's clients: %u (%s); user clients: %llu opened, %llu refused, %u alive, %llu selector call(s); "
            "modes accept %#x uc %#x", fIfName, why, clients, names, ucOpens, ucRefused, unsigned(ucClients), ucSelectors,
            unsigned(accept), unsigned(ucMode));
  note80211("%s (%s): %llu request(s) of %u kind(s)%s, %llu unknown, %u answered by the accept mode; %u scan(s) (serial %llu), %u "
            "result(s) served, %u list end(s), %u network(s); power %s (%u set(s)); %llu event(s), %llu refused; %llu other "
            "ioctl(s)",
            fIfName, why, requests, unsigned(kinds), overflow ? " (the kind table is full)" : "", unknown, unsigned(accepted),
            unsigned(scans), serial, unsigned(results), unsigned(ends), unsigned(networks), power ? "on" : "off",
            unsigned(powerSets), events, eventErrors, other);
}

void PianoWifi80211::reportTick(thread_call_param_t p, thread_call_param_t)
{
  auto *self = static_cast<PianoWifi80211 *>(p);
  Tick tick(self);
  if (!tick.run) return;
  self->report("summary");
  self->arm(self->fReportCall, kReportMs);
}

// Every thread call out of its callback and none armed: fStopping is already set, so no callback arms another (review F4).
void PianoWifi80211::quiesceCalls()
{
  if (fAttachCall) thread_call_cancel_wait(fAttachCall);
  if (fScanDoneCall) thread_call_cancel_wait(fScanDoneCall);
  if (fReportCall) thread_call_cancel_wait(fReportCall);
  for (unsigned waited = 0; waited < 500; ++waited) {
    if (!__atomic_load_n(&fCallsArmed, __ATOMIC_ACQUIRE) && !__atomic_load_n(&fCallsInside, __ATOMIC_ACQUIRE)) return;
    if (fAttachCall) thread_call_cancel_wait(fAttachCall);
    if (fScanDoneCall) thread_call_cancel_wait(fScanDoneCall);
    if (fReportCall) thread_call_cancel_wait(fReportCall);
    IOSleep(10);
  }
  note80211("a thread call was still armed or running after 5 s at the stop");
}

void PianoWifi80211::stop(IOService *provider)
{
  fStopping = true;
  if (fSysctl) unregisterAcceptSysctl();
  fSysctl = false;
  quiesceCalls();
  // Only a filter the stack has not freed itself may be detached (review F2): fFilterGone / fFilter are set by the detached
  // callback, which the stack calls for its own teardown too.
  interface_filter_t filter = fFilter;
  if (filter && !fFilterGone) {
    fFilter = nullptr;
    iflt_detach(filter);   // the detached callback gives the filter's reference back
    for (unsigned i = 0; i < 200 && !fFilterGone; ++i) IOSleep(10);
    if (!fFilterGone) note80211("the interface filter did not report its detach within 2 s");
  }
  if (fRegistered) report("at the stop");
  OSSafeReleaseNULL(fWifi);
  IO80211Controller::stop(provider);
}

void PianoWifi80211::free()
{
  if (fAttachCall) thread_call_free(fAttachCall);
  if (fScanDoneCall) thread_call_free(fScanDoneCall);
  if (fReportCall) thread_call_free(fReportCall);
  fAttachCall = fScanDoneCall = fReportCall = nullptr;
  if (fState) IOFreeData(fState, sizeof(State));
  if (fScratch) IOFreeData(fScratch, pianowifi::a80211::kScratchBytes);
  if (fSeen) IOFreeData(fSeen, sizeof(Seen));
  fState = nullptr;
  fScratch = nullptr;
  fSeen = nullptr;
  if (fLock) IOLockFree(fLock);
  fLock = nullptr;
  IO80211Controller::free();
}
