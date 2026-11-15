// SPDX-License-Identifier: BSD-2-Clause-Patent
// What macOS is shown as the battery (PianoBattery.cpp, piano-power-ps=1), from what level 1 reads: the two bq27z561 packs
// (Gauge.h, combined as PackReading) and the charger PMIC's Type-C state (ChargerPmic.h). Pure: no I/O, host-tested.
//
// The property set is the IOPMPowerSource one (xnu iokit/IOKit/pwr_mgt/IOPMPowerSource.h) plus what a Mac's battery shows next to
// it (names only): CurrentCapacity is a percentage with MaxCapacity 100, as on current Macs, and the charge in mAh goes to
// AppleRawCurrentCapacity / AppleRawMaxCapacity / NominalChargeCapacity; DesignCapacity mAh; Voltage mV; Amperage and
// InstantAmperage mA (negative while discharging); Temperature in hundredths of a degree Celsius; CycleCount; TimeRemaining,
// AvgTimeToEmpty and AvgTimeToFull in minutes, 65535 when there is nothing to compute (no current, or not discharging/charging).
//
// The low-battery levels are never set from here (AtWarnLevel/AtCriticalLevel stay false): macOS's own warnings follow the
// percentage. piano-power-ps-floor=N keeps the percentage shown away from N and below while on battery (0, the default: the true
// value). See docs/drivers/charging.md §5d for what macOS does at low charge on this tablet.
#ifndef PIANO_POWER_BATTERY_MAP_H
#define PIANO_POWER_BATTERY_MAP_H
#include <stdint.h>
#include "Gauge.h"
#include "ChargerPmic.h"

namespace pianopower {

constexpr unsigned kTimeUnknown = 65535;

struct BatteryValues {
  bool known = false;          // a reading to show (until the first one, nothing is published)
  bool implausible = false;    // a reading was there but its capacities are not believable: not published either
  bool installed = false, external = false, chargeCapable = false, charging = false, full = false;
  bool clamped = false;        // the percentage shown was raised to the floor
  unsigned percent = 0, maxCapacity = 100;
  unsigned rawMah = 0, fullMah = 0, designMah = 0;
  unsigned voltageMv = 0, cycles = 0;
  int amperageMa = 0, instantMa = 0;
  int temperatureCenti = 0;    // 0.01 degC
  bool temperatureValid = false;
  unsigned timeRemaining = kTimeUnknown, timeToEmpty = kTimeUnknown, timeToFull = kTimeUnknown;  // minutes
  uint32_t updateSec = 0;      // the calendar time of this reading (0: unknown, not published; "UpdateTime" on a Mac)
  // the adapter, when a source is seen on the Type-C port (its Rp current, ChargerPmic.h; the firmware's contract at level 2)
  bool adapter = false;
  bool adapterPd = false;      // the numbers come from a PD contract, not just a Type-C Rp
  unsigned adapterMv = 0, adapterMa = 0, adapterWatts = 0;
  uint32_t adapterFamily = 0;  // kIOPSFamilyCode value: 0xe0004009 USB Type-C (UniXNU-sheng ShengBattmgr.c's table)
};

constexpr uint32_t kFamilyTypeC = 0xe0004009u;

// The charger firmware's own view of the source (level 2, PianoMca's McaBusVoltage/McaType/ICL and the PDO list); null at level 1
// and whenever the firmware has not answered yet. What macOS shows as the adapter comes from here when it is available.
struct AdapterReal {
  bool ok = false;      // a reading this cycle
  bool pd = false;      // the firmware's type is a PD one (a contract, not just a Type-C Rp)
  int32_t mv = 0;       // the bus voltage the firmware reports
  int32_t ma = 0;       // the offer in force: the matching fixed PDO's mA, else the input limit asked
};

inline unsigned minutes(long mah, long ma)
{
  if (mah <= 0 || ma <= 0) return kTimeUnknown;
  const long m = mah * 60 / ma;
  return m >= long(kTimeUnknown) ? kTimeUnknown - 1 : unsigned(m);
}

// packs: the two readings (either may be missing); pack: their combination; port: null when the Type-C block is not readable;
// floorPct: piano-power-ps-floor; real: the charger firmware's view of the source (null at level 1); nowSec: the reading's
// calendar time (0: unknown).
inline BatteryValues mapBattery(const GaugeReading *packs, unsigned n, const PackReading &pack, const PortState *port,
                                unsigned floorPct, const AdapterReal *real = nullptr, uint32_t nowSec = 0, bool guard = true)
{
  BatteryValues v;
  if (!pack.ok) return v;
  // A reading with implausible capacities is not published at all, not even as an update: powerd anchors the percentage it
  // shows on the values it sees first (2026-11-15: pmset said 12 % for a whole boot while the registry tracked 95 -> 90 %),
  // so a garbage FullChargeCapacity must never reach the registry. Both packs together: 2 x bq27z561, design 9200 mAh.
  // A negative remaining would show as 0 % (macOS's low-battery notices), so it is held out too.
  if (guard && (pack.fullMah < 2000 || pack.fullMah > 20000 || pack.remainingMah <= 0 ||
                pack.remainingMah > pack.fullMah + pack.fullMah / 20)) {
    v.implausible = true;
    return v;
  }
  v.known = true;
  v.installed = true;
  v.updateSec = nowSec;
  // On external power: a source's Rp on the Type-C port, else (no port view) a battery that takes charge.
  v.external = port && port->ok ? port->sourceSeen : pack.charging;
  v.chargeCapable = v.external;
  v.charging = v.external && pack.charging;
  v.full = v.external && (pack.full || pack.socPct >= 100);
  v.percent = unsigned(pack.socPct > 100 ? 100 : pack.socPct < 0 ? 0 : pack.socPct);
  if (!v.external && floorPct && v.percent < floorPct) {
    v.percent = floorPct > 100 ? 100 : floorPct;
    v.clamped = true;
  }
  v.rawMah = unsigned(pack.remainingMah > 0 ? pack.remainingMah : 0);
  v.fullMah = unsigned(pack.fullMah > 0 ? pack.fullMah : 0);
  v.voltageMv = unsigned(pack.voltageMv > 0 ? pack.voltageMv : 0);
  v.instantMa = pack.currentMa;
  long average = 0, design = 0;
  unsigned cycles = 0;
  for (unsigned i = 0; i < n; ++i) {
    if (!packs[i].ok) continue;
    average += packs[i].averageMa;
    design += packs[i].designMah;
    if (unsigned(packs[i].cycles) > cycles) cycles = unsigned(packs[i].cycles);
  }
  v.amperageMa = int(average);
  v.designMah = design > 0 ? unsigned(design) : v.fullMah;
  v.cycles = cycles;
  v.temperatureCenti = pack.tempDeciC * 10;
  v.temperatureValid = pack.tempDeciC > -400 && pack.tempDeciC < 900;
  if (pack.discharging) v.timeToEmpty = minutes(v.rawMah, -average > 0 ? -average : -long(pack.currentMa));
  if (pack.charging && v.fullMah > v.rawMah) v.timeToFull = minutes(long(v.fullMah) - long(v.rawMah), average > 0 ? average : pack.currentMa);
  v.timeRemaining = v.charging ? v.timeToFull : !v.external ? v.timeToEmpty : kTimeUnknown;
  if (v.external && port && port->ok) {
    v.adapter = true;
    v.adapterFamily = kFamilyTypeC;
    if (real && real->ok && real->mv >= 4000 && real->mv <= 22000 && real->ma > 0) {
      // The firmware's own numbers: the bus it reports (a PD contract at its settled voltage, or 5 V) and the offer in force
      v.adapterMv = unsigned(real->mv);
      v.adapterMa = unsigned(real->ma > 5000 ? 5000 : real->ma);
      v.adapterPd = real->pd;
    } else {
      v.adapterMv = 5000;
      v.adapterMa = rpMilliamps(port->rpBits);
      v.adapterPd = false;
    }
    v.adapterWatts = (v.adapterMv * v.adapterMa + 500000) / 1000000;
  }
  return v;
}

}  // namespace pianopower
#endif
