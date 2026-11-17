# Contract-bounded buck charging

The `piano-power=2 piano-power-policy=1` path now observes UCSI through the existing
charger GLINK client. A source advertisement and a voltage-request ACK do not
prove a contract. A current, connected PD sink snapshot must include the selected
raw source PDO and decoded RDO. Fixed RDO operating current uses 10 mA units;
PPS uses 20 mV and 50 mA. Malformed, stale or failed reads revoke authorization.
This implementation is host-tested; PD/PPS and runtime health actuator behavior
still need tablet verification.

The observer polls status every five seconds and on port events, including a
connected non-PD SDP sink. Such a sample has a fresh status sequence and age,
with PD contract validity and budget remaining zero. CONTROL readback is only
diagnostic: completion follows serialized CCI command/ACK indications, even when
CONTROL reads zero or an older value. Transport timeouts still quarantine the
unsequenced channel until relink. Status diagnostics distinguish the built-in
observer from the legacy probe, and include stage, CCI and quarantine state.

The first tablet run booted and charged on SDP, but the extra CONTROL-echo check
prevented status completion and voided its actuator precondition. One later gauge
report also contained an invalid all-ones voltage. Gauge voltage/error/failed
command are now exposed per pack. The follow-up spec waits within a shared
75-second budget for a newer valid dual-gauge report; it retains rejected
attempts and never treats a failed read or an older valid report as fresh.

The first buck profile caps HV input at 1600 mA and battery current at 1500 mA,
requires the existing 4400 mV float setting, and reads SC8541 CTRL2 to confirm the
pump is off. It never enables the pump. Missing pump state prevents source voltage
changes. Successful board configuration, fresh dual gauges, temperature, dry LPD,
clear ISC/SOA and the current contract are required for new HV. Voltage changes
wait for reduced-current ACKs and an ICL readback; the new RDO and measured bus
must settle before current can rise. An expected RDO re-read has a bounded
15-second window at ICL 100/FCC 500 mA; a failed read withdraws that exception.
An idle or active UCSI notification suspends authorization until CCI separates a
command completion from a connector event. A healthy existing HV session waits
at ICL 100/FCC 500 for at most five seconds, without renewing the snapshot age or
extending the deadline under notification storms. A pending 5 V restore remains
an obligation across cable and mode changes until its ACK and readback agree.
The existing protected 5 V SDP/Rp ramp remains available without a PD contract.

Boot with `piano-power-health=1 piano-power-hv=1` for the 80/75 vote and fixed 9 V
preference. Mode 2 explicitly selects a 9 V PPS APDO. Firmware safety gates remain
in force. No PD PHY, role swap, second GLINK owner or separate ADSP lifecycle is
introduced. Protocol state and short `buck-*` lines are published alongside the
`PianoCharge` IORegistry dictionary. `Seq` advances with decisions; connection
`Generation` changes on actual invalidation/connection changes; `HealthGeneration`
and `ConfigRevision` advance with complete admin transactions.

Administrators can use the staged `piano-charge-config` command:

```
sudo piano-charge-config 1 80 75 1
piano-charge-config --show
sudo piano-charge-config --save 1 80 75 1
```

The four values are enabled, stop, resume and mode (0=5 V, 1=fixed, 2=PPS).
The kernel validates the entire revision-checked transaction and applies it on its
worker. Stop is 50–100 and resume is 40–stop−1. Disabling health clears only the SOC
vote; thermal and firmware stops remain. Missing gauges retain an established
hold and repeated failures stop controlled charging. Optional saved daily values
are reapplied by a local launchd job; test thresholds are never saved.

For an actuator test when real SOC is below the legal threshold range, a fifth
argument requests an explicit admin inhibit for at most 180 seconds. It uses the
same real stop/resume actuator, keeps SOC genuine and expires automatically. It
proves the actuator/UI chain; it does not prove a natural 80→75 battery cycle.
Restore with `sudo piano-charge-config 1 80 75 1 0` in unconditional cleanup.
The portable regressions include exact 79/80/76/75 hysteresis, leak detection,
firmware disable, atomic rejection, RDO units, generation and transition barriers:
`bash package/PianoPower/tests/run-charge-host.sh`.
