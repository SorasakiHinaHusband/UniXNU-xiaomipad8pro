# Audio duplex and input wake analysis

2026-11-27; source baseline `lead/v131y12t`, `3fcf74f45`.
Host-side analysis only. No driver modification, image, tablet access or push.
This analysis covers daily-use Goals 6 and 7. Source references below refer to
the baseline, unless explicitly qualified as a reference revision.

## Evidence and scope

- The owner's installed macOS 27.3 report establishes an audible duplex problem;
  it does not identify the installed KC, runtime switches or a failure layer.
- The saved `aud-mic4` score and original console establish two distinct input
  runs: 469 late periods / 8293 us maximum, then 25 / 7374 us. Both have zero
  post-startup missing PCM. The often quoted 25 / 7.4 ms describes the restart,
  not the long capture. This was a RAM diagnostic run, not installed acceptance.
- The saved November 24 audio analysis reports an earlier duplex defect fixed
  since v13.1p, with signed sound age enabled on its observed installed y12c.
  Later silent installs skipped audible duplex acceptance. Historical acceptance
  cannot establish the behavior of the owner's current installed boot.
- Linux/Ubuntu v1.4.0 is available locally: mainline project
  `9f4b92899a7114c6af70a9ed64cd15a3f9b788f5`, whose `kernel/source.json` pins
  Linux `83f0f89bed14a0cb7d582a26e72bced0387870f4` (also kernel tag v1.4.0).
  These exact revisions, rather than the newer checkout HEADs, are the reference.
  GPL sources are read as behavioral specifications; no Linux implementation is
  copied. `unixnu.lock` does not pin this Ubuntu release; the older references in
  `docs/drivers/audio.md` and `package/PianoAudio/UPSTREAM` are distinguished.

## Decision

**HOLD a new fix image.** The historical duplex root cause is confirmed and
already fixed by default. A second, conditional speaker FIFO defect is proven
below in the optional 480-frame engine mode. The failing installed boot's mode
is unknown. Input wake delay is real, but the saved metric does not identify
its source. There is no evidence for a shared Speaker/Mic device-lock deadlock.

The leading classification is therefore:

| Symptom and condition | Finding | Confidence / next discriminator |
|---|---|---|
| Duplex dip with old unsigned sound age | False silence starts our own 20 ms fade; microphone completion processing widens the race window | Confirmed historical root cause, host reproduced and saved same-boot A/B confirmed; check active age policy and fades |
| Speaker short reads with engine mode 1 (480 frames) and a four-slot refill | Legacy trim destroys the refill reserve, even when enough PCM was available | Statically proven and reproduced with production C; check the failing engine's StartIO mode, ledger skips and missing frames |
| Stutter on current signed-age, mode-0 installed playback | Shared-worker stalls, late output production, graph fallback or a downstream problem remain possible | Unclosed; classify from synchronized counters before choosing any change |
| Input `late-periods` with intact PCM | Ordinary deadline/scheduler service, previous-pass work, or same-device lock delay; synchronous input logging is one source of previous-pass work | Leading host scheduling hypothesis; no measured attribution to a timer, lock holder or logging call |

## Source and evidence notation

Within the source tables, unqualified filenames are in `package/PianoAudio/`.
`Power/` means `package/PianoPower/`. `L/` means the pinned Linux source above,
under `sound/soc/qcom/qdsp6/` unless a longer path is given. These are function
and statement comparisons, not an assertion that two different audio stacks
have identical code. Reference links:
[Ubuntu v1.4.0](https://github.com/yzddmr6/xiaomipad-8pro-mainline/tree/9f4b92899a7114c6af70a9ed64cd15a3f9b788f5/device/audio),
[Linux qdsp6](https://github.com/yzddmr6/linux-sm8750-piano/tree/83f0f89bed14a0cb7d582a26e72bced0387870f4/sound/soc/qcom/qdsp6),
[Linux packet router](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14a0cb7d582a26e72bced0387870f4/drivers/soc/qcom/apr.c).

Evidence labels below identify saved runs and artifact basenames only; no
private location, PCM, calibration, credentials or raw console is committed.

| Evidence | What was actually read | Limit |
|---|---|---|
| `aud-mic4`, November 17 | `0-provenance.txt`, `SCORE-aud-mic4.md`, `MAIN-NOTE.md`, original `console.out` and its saved microphone window | KC provenance identifies commit `4ca0b595a2`; diagnostic root IOKit capture, not normal-app/installed acceptance |
| `aud-mic4` original console, lines 6220–6221 and 6777–6778 | Run 1: requested 3014400, delivered 2994240, startup 20160, missing 0, unavailable 0, fault 0; run 2: 177600 / 157440 / 20160 / 0 / 0 / 0 | 12560 and 740 engine batches respectively; 469/12560 = 3.73%, 25/740 = 3.38% late; WAV duration and engine request duration need not coincide |
| `aud-mic4` original console, lines 6455 and 6975 | READ health at release: 7749/7749 and 1839/1839 sent/returned, errors/late/alignment/origin/range all 0; short READs 2 and 3 | Release includes idle capture after the HAL stopped; these are not counts within just the recording window |
| November 21 duplex round-3 analysis | `DUPLEX-R3.md`: comparison of HAL nonzero periods, our DMA nonzero periods, and 20 ms acoustic fade envelopes | Historical analysis includes inferred timings; those estimates are not lock measurements |
| November 21 `duplex-age1` | Original `RESULT.md` and `verdict.txt`: signed B blocks 0/0/0 acoustic dip windows; old control 85/17/60; signed C and direct-Mic D also 0; no protection fallback | Same frozen RAM image, not current installed acceptance. The text “starved 18 lines” is a parser summary; the actual `z0` fields mean zero depth-0 observations, not 18 starvation events |
| November 24 audio review | `ANALYSIS.md`, `TABLET-CHECKLIST.md` | Reports signed age enabled on installed y12c, protection 3 and MIPLAY 1; later silent-install checks skipped audible duplex. The proposed silent probe is not present in this branch |

The original score called the long run about 12544 periods; the arithmetic above
uses the original engine request count. No claim is made that the late-wake
timestamps were recorded: only totals and maxima exist.

## End-to-end path and reference comparison

### Host devices, rings, engine service

| Step / event | Ours, exact source | Linux/Ubuntu v1.4.0 comparison | State |
|---|---|---|---|
| App -> CoreAudio -> VirtualAudio duplex route | `PianoAudioDevice.cpp:3` and `:11`: HALS_IOA2Device/Engine; separate UIDs Speaker and Mic, aggregated above the driver | ALSA/UCM routes MultiMedia1 to secondary TDM and MultiMedia3 to VA TX0 (`device/audio/README.md:100`) | `ours-OK` at the implemented ABI; `unknown` current app route and HAL callback behavior |
| User client mapping/start | `PianoAudioDevice.cpp:124`, `:329`, `:459`, `:622`: shared status page, ring type `0x10000000 + 1`; selectors 0/10 start and 1/11 stop | `L/q6apm-dai.c:514`, `:560`: substream runtime and DMA region; separate playback/capture runtime objects | `ours-OK` independent allocations |
| Device execution owner | `PianoAudioDevice.cpp:465`, `:511`: one `kernel_thread_start` per device; no IOAudioEngine superclass implementation, workloop, command gate or AudioDriverKit process | `L/q6apm-dai.c:240`: callback-driven PCM notifications; `L/q6apm.c:603`: graph callback | `ours-OK` ownership; `ours-missing` explicit audio deadline scheduling policy |
| Host format and cadence | `AudioTiming.h:10`: 48 kHz, stereo/mono S16; fixed HAL modulus 7680 frames, zero timestamp period 7680, safety 480, engine latency 240; kernel batches 240 or 480 | `L/q6apm-dai.c:201`, `:220`, `:547`: formats/rates constrained by PCM parameters; period/buffer step 480 frames | `ours-OK` fixed ring layout; HAL callback size is not the kernel batch size |
| Deadline/wake | `PianoAudioDevice.cpp:326`, `:532`: deadline = StartIO absolute time + frames/rate; unlock before `clock_delay_until`, lock again before sampling `woke` | DSP completion -> period elapsed, with no matching host 5 ms timer (`L/q6apm-dai.c:252`) | `unknown` source of installed wake latency |
| Speaker period | `PianoAudioDevice.cpp:549`: copy passed region, transport play, erase region; `AudioDsp.cpp:260` -> `AudioLive.c:200`: timestamp nonzero sound, write SPSC FIFO | ALSA producer `.ack` queues available complete periods, `L/q6apm-dai.c:426`; hardware position follows returned periods, `:647` | `ours-OK` SPSC; `unknown` current HAL write lead |
| Mic period | `PianoAudioDevice.cpp:542` -> `AudioDsp.cpp:267` -> `AudioLive.c:227`: wait for actual FIFO reserve, read mono, digital gain/mute and stats, fill short return with zero, write mapped HAL ring | `L/q6apm-dai.c:255`: READ_DONE notifies ALSA and requeues a READ; no separate mono FIFO/consumer timer | `ours-OK` accounting and mute; input logging is within the engine's device lock |
| Position publication | `PianoAudioDevice.cpp:557`: advance `done` by one batch, update timestamp on 7680-frame wrap with barriers; no rebase or frame skip on late wake | `L/q6apm.c:643`, `:670`, `L/q6apm-dai.c:653`: returned token -> atomic hardware pointer modulo periods | `ours-OK` catch-up preserves samples; `unknown` whether a late mapped-ring write missed an app read deadline |
| Start/stop and controls | `PianoAudioDevice.cpp:136`, `:424`, `:492`; `AudioLive.c:176`: combined direction intent, epoch boundary; controls publish atomically | ALSA trigger/start/stop and DAPM, `L/q6apm-dai.c:447`; independent substream state | `ours-OK` direction independence; StartIO/StopIO are not per-period GPR waits |

The implemented Apple ABI is documented in `docs/drivers/audio.md:92` and the
source comments. This job did not redisassemble Apple's 27.3 binaries. Their
current callback scheduling, VirtualAudio aggregation and app processing remain
`unknown` beyond those documented contracts; source comments are not new binary
or installed-system evidence.

### Shared worker, DSP, endpoints, protection

| Step / resource transition | Ours, exact source | Linux/Ubuntu v1.4.0 comparison | State |
|---|---|---|---|
| Worker entry and clock snapshot | `PianoAudio.cpp:459` -> `AudioDsp.cpp:565`; loop captures `now` at `:613`, reads health, drains inbox at `:633`, then ticks at `:637` | GPR RX workqueue `drivers/soc/qcom/apr.c:315`, `:619`; DAPM/codec work is separate | `ours-contended` one worker services both pumps and synchronous local actions; duration unknown |
| DSP receive boundary | `AudioDsp.cpp:44`: PianoPower callback copies only, no PCM processing; `AudioDspLink.c:64`: 16 x 512-byte SPSC inbox; **receive does not wake the audio worker** | `apr.c:161`: RPMSG callback copies/enqueues; `:185` schedules RX work, which calls graph callbacks outside router locks at `:299` | `ours-OK` callback isolation; `ours-missing` completion-triggered worker wake |
| Ticking/ordering | `AudioLive.c:1784`: speaker, mic, hold; `:1795`: running returns 5 ms, start/stop 2 ms, both down 2 s; `AudioDsp.cpp:219` sleeps relative to work completion | `apr.c:315` dispatches received packets; `L/q6apm-dai.c:255` requeues capture in callback | `ours-contended` work + sleep cadence; no bound on processing/scheduler overhead |
| APM/PRM sequencers | `AudioLive.c:393`, `:401`; one outstanding control command across speaker/mic/monitor/hold plans, answer limit 2000 ms; local steps execute synchronously through `AudioSequence.c:42` | `L/q6apm.c:44` APM mutex/wait; `L/q6prm.c:58` separate PRM mutex/wait; `L/audioreach.c:611` holds command mutex over a reply wait up to 5 s | `ours-OK` serialized control correctness, not a 2 s PCM mutex hold |
| Speaker graph startup | `AudioSequence.c:189`: bit clock, device open/log/MFC/TDM config, stream open/format/decoder/converter/log, map, prepare x2, START device/stream, amps muted | `L/q6apm-dai.c:343`, `:379`, `:401`, `:412`; TDM machine `sound/soc/qcom/sc8280xp.c:155` | `ours-OK` ordered commands; local amplifier start can stall active capture |
| Playback data pump | `AudioLive.c:823`, `:897`, `:1126`: FIFO -> drift/trim -> stereo-to-quad -> DC/gain/fade/ceiling -> DMA stores/publish -> WRITE V2; eight slots, four outstanding | `L/q6apm.c:515`: per-graph ring index mutex released before GPR async send; producer `.ack` queues periods | `ours-OK` default burst reserve; `ours-missing` equivalent reserve in optional mode 1 |
| Mic graph startup | `AudioSequence.c:233`, `:287`, `:302`: two PRM votes, TX-core mclk, device/stream opens/config/map/prepares, local VA on, START x2 | Linux VA driver runtime PM, `sound/soc/codecs/lpass-va-macro.c:1390`, `:1647`; capture starts in trigger, `L/q6apm-dai.c:458` | `ours-OK` reversible ownership; `ours-contended` VA startup sleeps on audio worker |
| Mic hardware -> graph | `AudioVa.c:65`: clocks/FS broadcast, DMIC mux and DEC0/DEC1, 48 kHz/div4, gain; `PianoAudioReach.c:188`, `:197`, `:299`: VA TX0 -> LOG -> MFC -> converter -> encoder -> RD_SHMEM | Ubuntu `device/audio/README.md:58`, `:100`: same VA endpoint and legacy dual-mic route; Android TX3 is a distinct reference, not this Ubuntu topology | `ours-OK` endpoint/format; chosen current route `unknown` until read |
| READ data and completion | `AudioLive.c:1535`, `:1822`: independent capture pump; validate origin/token/handle/slot/range/alignment, copy two channels, mix legacy or select DEC0, zero the first 100 ms, feed FIFO/tap (`:1426`) | `L/q6apm.c:554`, `:657`: per-graph READ and callback; start prequeues runtime periods (`L/q6apm-dai.c:464`) | `ours-OK` wire/range checks; WC read cost `unknown` |
| Packet send to shared GLINK | `AudioDsp.cpp:161` -> `Power/PianoAdsp.cpp:1323`, `:1002`: per-channel 16-message software queue, global client mutex around copy/count, fair channel rounds; GLINK send outside that lock | `apr.c:126`: per-service spinlock + `rpmsg_trysend`; `drivers/rpmsg/qcom_glink_native.c:355`: shared TX lock, no wait with lock held | `ours-contended` physical FIFO/remote intents shared with charger/sensors, no measured saturation |
| Hardware notification | `Power/PianoAdsp.cpp:379`, `:402`, `:909`: IPCC doorbells polled; 20 ms steady poll, 1 ms while recent control replies pending (`:80`, `:265`); sends wake owner | Linux mailbox/GLINK RX -> `qcom_glink_native_rx` (`qcom_glink_native.c:1215`) -> RPMSG -> GPR workqueue | `ours-missing` audio completion IRQ dispatch; polling is not by itself proof of underrun |
| SMMU / memory | `AudioBoard.cpp:266`, `:302`: exact stream bypass setup and write-combined low-DRAM slice; `PianoAudioReach.c:399` encodes SID low nibble above bit 31 | `L/q6apm-dai.c:563`: physical DMA address with SID above bit 31; separate mapped graph fragments (`L/q6apm.c:323`) | `ours-OK` nonoverlap/address bounds; actual installed mapping and cache/firmware behavior `unknown` |
| Protection graph | `AudioSpkProt.c:682`, `:759`: endpoint-owned-clock variant omits CPU PRM bclk; open speaker/SP_RX + Re25, optional MIPLAY, stream, open/config/prepare/start V/I then prepare/start speaker/MIPLAY/stream | Ubuntu `device/audio/README.md:41`: no V/I protection graph, caps gain; not evidence that shared capture hardware cannot coexist with protection | `ours-OK` explicit graph/control-link plan; DSP internals `unknown` |
| Protection running | `AudioLive.c:665`: battery voltage, live GET, optional DAP pregain every 2 s; parameter slot separate from PCM; `:642` parses/logs after response | No corresponding monitor in Ubuntu reference | `ours-contended` same control channel/worker, but asynchronous answer wait does not stop PCM pumps |
| Amplifier monitoring / control | `AudioLive.c:1144` refills first, then one gain write or one status-register read; `AudioAmps.c:130`, `Fs19xx.c:304`; four amplifiers share bit-banged I2C | Linux FS19xx per-amp mutex and delayed startup work, `sound/soc/codecs/fs19xx.c:418`, `:493`; independent of PCM RX callback | `ours-OK` sliced polling; `unknown` actual per-register elapsed time |
| Stop/failure/PM | `AudioLive.c:973`, `:1008`, `:1559`, `:1891`: fade/drain, mute, reverse undo, then clock release; quiet/fatal/link health can stop both; sleep retains uncertain DMA | Linux ALSA/DAPM teardown, FS19xx mute/shutdown while bclk runs (`fs19xx.c:513`) | `ours-OK` safety ordering; `ours-contended` synchronous shutdown can delay the other direction |

Control packets do **not** impose the one-command restriction on READ/WRITE
buffers: the pump loops at `AudioLive.c:1126` and `:1553` do not call
`others_busy()`. READ_DONE processing also precedes control-answer dispatch
(`:1813`, `:1822`, `:1840`). Linux is not entirely lock-free either: endpoint
configuration can use `graph->lock` through `L/audioreach.c:657`, shared with
that graph's data bookkeeping. Its usual setup occurs before PCM runs. Both
stacks share a physical transport; neither fact proves a duplex deadlock.

## Lock, buffer, cadence and clock inventory

`ours-OK` means the named source-level invariant holds, not live acceptance.
`ours-contended` means competing work shares that owner/resource; it does not
claim a measured mutex wait. `ours-missing` names a concrete absent invariant
or observation. Unmeasured implementation/firmware behavior is `unknown`.

### Locks and owners (complete explicit PianoAudio lock set)

| Resource | Hold/work / relationship | Classification |
|---|---|---|
| Speaker `PianoAudioDevice::lock` | Allocated separately at `PianoAudioDevice.cpp:333`; held across output ring copy/play/erase/status/producer trace (`:535` to `:572`), released during deadline wait | `ours-OK` direction isolation; hold time `unknown` |
| Mic `PianoAudioDevice::lock` | Different allocation; held across FIFO capture, PCM stats, periodic `micHalReport`, ring copy/status; never waits for a GPR reply | `ours-OK` separation; periodic logging cost `unknown` |
| `gTransportLock` | `PianoAudioDevice.cpp:136`: StartIO/StopIO combined intent only. Encloses `dspRunning`, its logs, atomics and `liveWake`; not taken by normal play/capture | `ours-OK`; no steady-period shared-lock explanation |
| `gWakeLock` | `AudioDsp.cpp:210`: flag/wakeup or condition wait; `IOLockSleepDeadline` releases it while sleeping; no DSP/bus operation under it | `ours-OK`; allocation-once, short critical section by source, elapsed duration `unknown` |
| `gMicRecordLock` | `AudioDsp.cpp:458`: diagnostic window allocation/arming/readout; producer publishes ready with atomic state, does not take this lock | `ours-OK` off period path; diagnostic CPU copy adds worker cost |
| `fLock`, command gate, workloop | No such audio serialization object in this package; engine instances are IOService subclasses with their own explicit locks | `ours-OK` absence of the proposed common gate; internal IOKit/registry/IOLog locks `unknown` |
| PianoPower `gClientLock` | `Power/PianoAdsp.cpp:182`, `:994`, `:1328`, `:1391`: all clients' registration/send queue/crash health copy. No PCM conversion, reply wait or GLINK send under ordinary enqueue/dequeue hold; optional sensor trace can format logs inside enqueue | `ours-contended`; measured wait/hold absent, hence latency `unknown` |
| GLINK ring, intents, GPR/APM/PRM state | One PianoPower owner for GLINK, one audio owner for plans/pumps. No additional host audio GPR mutex. APM/PRM services and ADSP memory bus shared in firmware | `ours-OK` software owner; transport arbitration `ours-contended`; firmware locks `unknown` |
| TCSR SMEM mutex / SCM mutex | `Power/PianoAdsp.cpp:433`, `:666`: allocation/secure bring-up, not a per-period audio resource | `ours-OK` absent from steady PCM path |
| TLMM/I2C, VA/LPI, SMMU configuration | One audio worker; no per-period shared mutex. SMMU/preset/firmware and FAT/NativeUfs reads are startup-only | `ours-OK` ownership; synchronous local operations `ours-contended` |

Source lock order on start/stop: device lock -> transport lock -> wake lock.
Steady output may take device -> wake only for first sound while down. Steady
input takes only its device lock in our code. Worker sleep takes wake only;
the PM reader barrier takes device locks without transport/wake held. No
source-level reverse edge closes a steady-state cycle. Hidden IOLog/IORegistry
holds and scheduling delay cannot be assigned a microsecond bound from this
graph. `Power/PianoAdsp.cpp:960` explicitly invokes receive callbacks outside
the client lock.

### All PCM/control buffers and timers on this path

| Resource | Exact geometry / owner | Classification |
|---|---|---|
| HAL status pages and Speaker/Mic rings | One status page + 7680-frame ring each; 30720 output bytes, 15360 input bytes; HAL and respective engine share, not DSP DMA | `ours-OK` fixed modulus and barriers; HAL producer/read margin `unknown` |
| Engine stack period | 480 x 2 S16 capacity = 1920 bytes, thread-local; batch240/480; no allocation per pass (`PianoAudioDevice.cpp:517`) | `ours-OK` |
| Speaker FIFO | 4096 stereo frames = 16384 bytes = 85.33 ms capacity; target 960, default prime/burst budget 2880 (`AudioLive.h:52`, `:73`) | `ours-OK` mode 0; `ours-missing` burst reserve in mode 1 |
| Mic FIFO | 8192 mono frames = 16384 bytes = 170.67 ms capacity; target 1920, priming requires target + consumer batch (`AudioLive.c:239`) | `ours-OK` reserve/accounting, measured simplex missing 0 |
| Speaker DSP slots | Eight x 3840 = 30720 bytes; four x 480 frames = 40 ms outstanding, not six (the six-depth constant in `AudioSession.h:33` is for earlier level tests) | `ours-OK` bounded ownership; true DSP queued audio time `unknown` |
| Mic DSP slots | Eight x 1920 = 15360 bytes, four 10 ms READs outstanding | `ours-OK`; pending READs are empty buffer promises, not captured PCM |
| DMA parameter region | Speaker periods `[0,0x7800)`; protection `[0x8000,0x10000)`; mic `[0x10000,0x13c00)`; total CPU map 0x20000 bytes (`AudioSession.h:30`, `AudioSpkProt.c:686`) | `ours-OK` disjoint. Protection maps first 64 KiB; mic map rounds 15360 to 16384 bytes; no overlap |
| Cache ordering and DMA request descriptors | `AudioBoard.cpp:300`, `:310`; WC mapping and `dsb sy` publish; READ/WRITE packets carry address/handle/size, not a CPU LPASS descriptor chain | `ours-OK` store-before-WRITE and packet bounds; DSP cache behavior and measured WC read cost `unknown` |
| GPR packets / inbox / send queue | 512-byte packet cap; 16 inbox entries, 20 offered RX intents; audio send queue 16 messages; eight total PCM requests can be outstanding plus a control command | `ours-OK` bounded geometry; queue/intent/FIFO service `ours-contended`; current drops `unknown` |
| Protection OOB blocks/live slot | Private-file opens/config and live slot laid out/aligned with bounds checks (`AudioSpkProt.c:714`); monitor writes only its reserved slot | `ours-OK`; no PCM or calibration value is exposed by this analysis |
| Mic tap and diagnostic window | Tap 16384 mono frames / 341.33 ms (`AudioTap.h:28`), atomic publish; diagnostic raw window is bounded and optional (`AudioLive.c:1435`) | `ours-OK` buffer isolation; extra completion processing `ours-contended` |
| Speaker ledger | 128 events, atomic slot publication (`AudioLedger.h:12`); formatting on sysctl thread (`AudioDsp.cpp:527`), no PCM | `ours-OK`; overwritten rows/producer sentinel must be respected |
| Host engine deadline timer | Independent 5/10 ms absolute grids; max/wake metric per device; generic APPS timer interrupt scheduling below driver | `unknown` service latency; `ours-missing` separate wait-return, lock-wait and body timing |
| Worker sleep / condition event | 5 ms while any stream runs; 2 ms only when no running path and a plan starts/stops; 2000 ms idle with first-sound/start wake | `ours-OK` wake intent; completion wake absent (`ours-missing`), processing/sleep can contend |
| Amplifier status / gain cadence | Config default status period 500 ms (`AudioLive.h:57`); one of 24 fields per worker pass; initial unmute schedules first poll using 100 ms constant (`AudioLive.c:1071`); four gain writes across four passes | `ours-OK` sliced; elapsed poll length `unknown` |
| Protection monitor / timeout | Every 2000 ms; SET battery / optional pregain / GET live sequential control commands; 2000 ms answer timeout; pump late guard 250 ms | `ours-OK` bounds, `ours-contended` worker/transport; late guard is not an underrun deadline |
| Mic settle / idle / PM | 100 ms DSP-start settle; idle default 3000 ms or boot override; PM 250 ms engine barrier and 8 s drain (`PianoAudioDevice.cpp:190`, `AudioDsp.cpp:689`) | `ours-OK` explicit transitions; cannot explain sustained steady-state wakes without a PM/idle event |
| IPCC and ADSP endpoint DMA IRQs | APPS doorbells polled; FS19xx interrupt GPIOs surveyed, not registered as audio IRQ event sources; TDM/codec DMA IRQs run inside ADSP | APPS completion/fault IRQ delivery `ours-missing`; DSP IRQ scheduling `unknown` |

`AudioSafety.c:215` through `:258` uses SPSC head/tail acquire/release. The
producer owns head, consumer owns tail; ring reset advances tail to current
head. No ring mutex is shared by the engine threads. The three rings (HAL,
host FIFO, DSP slots) are different latency/ownership boundaries and cannot be
substituted for one another in buffer arithmetic.

### Clock and firmware resources

| Clock/resource | Source / relation | Classification |
|---|---|---|
| Host `mach_absolute_time` and 48 kHz model | `PianoAudioDevice.cpp:326`, `:532`; each device gets its own StartIO anchor, common monotonic clock | `ours-OK` arithmetic; not phase locked to ADSP |
| Speaker TDM frame/bit clock | 48000 x 4 slots x 32 bits = 6.144 MHz; passthrough PRM SEC_TDM_IBIT 0x202 (`AudioSequence.c:190`, `PianoAudioReach.h`) | `ours-OK` rate; actual hardware discontinuity `unknown` |
| FS19xx PLL, DAC and V/I return | PLL configured for 6.144 MHz (`Fs19xx.c:262`); speaker lane 1, feedback lane 0 in protected graph | `ours-OK` configuration; shared LPASS/TDM hardware capacity/internal clock gating `unknown` |
| VA TX-core MCLK | PRM 0x30c at 19.2 MHz plus macro and digital-codec votes (`AudioSequence.c:233`); VA internal 9.6 MHz / 4 = 2.4 MHz DMIC clock, 48 kHz decimation (`AudioVa.c:65`) | `ours-OK` distinct requested clock IDs; physical PLL coupling `unknown` |
| Clock ownership under protection | Request 3 = protection + endpoint-owned bit clock (`AudioSpkProt.h:228`, `AudioSpkProt.c:759`); other variants can use CPU-owned/inverted clock | `ours-OK` selectable ownership; active variant must be read, not assumed from ready files |
| Duplex clock hold | `AudioLive.c:471`: 0 default, 1 votes, 3 votes+MCLK held before first path; mic skips held prefix; release only both down | `ours-OK` single vote owner; historical hold experiments did not remove false fades |
| Host/DSP drift | `AudioLive.c:81`, `:878`, `:1480`: 1/64 smoothing, 240-frame deadband, at most one-frame correction per DSP period | `ours-OK` correction accounting; at most 100 frames/s = 2083 ppm correction authority; rate estimates `unknown` |
| SPF/APM graph and data ports | APM 1, PRM 2, data 0x10000000 by default; independent graph instance IDs, map handles, playback token base 0x4000/capture 0x5000 (`AudioDspLink.c:89`, `AudioSequence.c:349`) | `ours-OK` demultiplexing; firmware graph scheduler/locks `unknown` |
| V/I graph and MIPLAY | Shared ADSP CPU/memory and TDM interface; V/I is an internal feedback source, not the user Mic; MIPLAY adds DSP processing | `ours-contended` firmware resources, no measured DSP load/MPPS/underrun counter |
| DSP power / charger / transport | PianoPower owns boot/health/quiet, audio only votes plan-owned clocks; guard checks generations/quiet (`AudioDspLink.c:20`) | `ours-OK` ownership; current quiet/fatal/restart occurrence `unknown` |

Ancillary source boundaries: `AudioPcm.c` is the null transport, inactive for
level-10 DSP service; `AudioControl*.h`/`AudioLevelDictionary.h` translate HAL
controls; `AudioCapture.h` computes input gain/statistics; `AudioSurvey.h`,
`AudioHw.h`, `AudioPins.h`, `SmmuStream.h` describe startup/survey; `AudioPreset`,
`AudioSha256`, `FatVolume`, the shared firmware loader and `Fs19xx` preset parser
run before live streaming. They introduce no additional explicit steady-state
audio lock, timer or PCM queue. `AudioSession.c` hands the mapped hardware to
`AudioLive` rather than running a concurrent second pump. Host tests cover
these boundaries; none is evidence of installed acoustic acceptance.

## Root causes and latency arithmetic

### Historical duplex: our own false-silence fade

The exact race is `AudioDsp.cpp:613` -> health/inbox drain `:633` ->
`audio_live_tick(now)` `:637`, concurrent with `AudioLive.c:208` publishing
`sound_at` from the output engine's **newer** clock read. For worker `now=1000`
and producer `at=1001`, unsigned age is 4294967295 ms, exceeding even the
600000 ms maximum idle setting. In `SpkPlaying`, false `active=0` starts
`audio_chain_fade(-1)` (`AudioLive.c:1081`). `AudioSafety.c:74` applies a
960-frame linear fade: 960/48000 = 20 ms. A later fresh tick reverses the fade
or restarts from zero. Queue depth and software short-read counters can remain
healthy because the driver itself fades valid PCM before writing DMA.

This is a timestamp-consistency defect, not a lock timeout. Microphone
READ_DONE processing in the inbox drain adds byte loads, mixing/statistics,
optional raw/tap copying and FIFO work, increasing the interval in which a
producer can publish a timestamp newer than the worker's snapshot. No measured
WC bandwidth or drain duration is available. Atomics alone do not make the
worker's earlier clock read current.

The signed comparison at `AudioLive.c:727` now regards this difference as -1
and therefore fresh; the same helper covers `SpkQuiet` idle ending (`:1042`).
The bounded idle interval is below the signed half-range. `AudioLive.c:1716`
enables it by default; `PianoAudio.cpp:120` and `AudioDsp.cpp:419` retain the
old policy only as an explicit control. `tests/live_host.c:2284` drives the
actual state machine with an ahead producer: old policy fades, signed policy
does not; ordinary silence/restart tests still pass.

The saved `duplex-age1` original result supplies hardware corroboration:
0/0/0 versus 85/17/60 acoustic dip windows on the same RAM boot. Thus the old
root cause is closed. It is **not** a newly missing fix in `lead/v131y12t`.
If installed age is 1 and there are no false-silence fades, it cannot explain
that window's stutter. The November 24 report alone does not identify the
owner's current failing boot.

### Conditional current defect: mode480 discards a needed burst reserve

`AudioLive.c:534` deliberately retains the older trim/prime policy when the
engine produces 480-frame batches. `take_period()` (`:862`) computes:

| Policy | Threshold before trim | Frames retained on trim | Demand for four free DSP slots |
|---|---:|---:|---:|
| Default mode 0, batch240 | max(2880, 960 + pending x 480) | 960 + pending x 480 | 1920 |
| Optional mode 1, batch480 | 960 + 3 x 480 = 2400 | 960 | 1920 |

With 2880 actual frames and four free slots, mode 1 first skips 1920 frames to
retain 960. Two reads consume that 960; the next two need 480 each and get 0.
Result: one skip, two underrun events, **960 missing frames / 20 ms** before the
DC/fade stage. Mode 0 retains all 2880 and ends with the 960-frame reserve.
There need be no HAL lateness, GPR error, DSP timeout, or physical clock fault.
Even without a trim, mode 1's 960-frame initial prefill is smaller than a
four-slot refill; it lacks the same worst-burst invariant (`:535`, `:1048`).

The added `tests/duplex_schedule_host.c` calls the **production static
`take_period()`**, using the same post-unmute `prime_left` selection. It
deterministically reproduces 0 versus 960 missing frames for these two modes.
This is an exact source-level defect under a supplied four-slot completion
schedule, not a simulated measurement of the tablet's completion schedule.
Duplex may increase batching through shared work, but batching is not unique
to duplex. The installed engine mode and matching short-read/skip ledger
events are required to associate this defect with the complaint.

If those observations match, the candidate single policy change is to use the
existing epoch/burst reserve policy for batch480 as well. It is deliberately
**not implemented** here. A different defect should not be bundled with a
timer-policy change or with reapplying the historical age fix.

### Input wakes: what is proven and what is not

For batch B, `PianoAudioDevice.cpp:532` computes
`D = startTime + (done+B)/48000`, waits without the device lock, then samples
`W` after reacquiring it. Reported lateness is `max(0,W-D)`; `late-periods`
increments only when this exceeds B/48000, not whenever it exceeds zero.
The measurement excludes the current capture body but can include delay from
the preceding body when the next deadline is already overdue.

For a fast body, a pass 8293 us late on a 5000 us grid is followed by a pass
3293 us late, then the engine resumes its absolute grid. `done` still advances
by 240 each pass. No sample is skipped merely because the first pass was late.
The early global `piano_audio_dev_late` counter (`:533`) checks **before** the
wait; it misses a wait that returns late and is also updated by both engines
with non-atomic read/modify/write. It is not an authoritative substitute for
the per-engine stop line (`:582`).

The added model injects 7374 and 8293 us at three isolated deadlines. It uses
the real `audio_live_capture` and SPSC implementation, with exact sequential
sample checks and accounting. For both single and four-period producer bursts:

- batch240 reports three late periods and zero missing/overrun frames;
- batch480 reports **zero** late periods for the same delay magnitude, because
  its threshold is now 10000 us; its maximum lateness is unchanged;
- delaying wait return, lock acquisition, or the previous body can produce
  identical per-engine maxima/counts and delivered samples;
- strict threshold and catch-up cases at 5000/5001/10000/10001 us behave as
  expected, with no lost PCM in this bounded schedule.

This explains the apparent contradiction in `aud-mic4` without claiming to
reproduce its unrecorded wake history. Increasing the batch to make the counter
zero would not establish a scheduling fix and also encounters the output
reserve defect above.

The input engine is created with plain `kernel_thread_start`, with no audio
time constraint/priority setup (`PianoAudioDevice.cpp:485`, `:513`). Its body
does synchronous IOLog through `dspCapture`/`micHalReport` once a second and
on control changes (`AudioDsp.cpp:189`, `:267`), while holding the device lock.
This can delay a subsequent iteration and compete for shared kernel logging
resources. However, 469 late passes over 62.8 s is about 7.47/s; isolated
one-second reports followed by fast catch-up cannot by themselves explain
all those events with a maximum below 10 ms. Scheduler/timer service and other
work remain necessary alternatives, rather than attributing everything to
logging.

The readable k27 XNU build-input source adds useful bounds, but its exact
correspondence to the installed KC and historical `aud-mic4` KC is `unknown`:

| XNU source | Observed implementation / implication |
|---|---|
| `osfmk/kern/thread.c:1951` | `kernel_thread_start` selects default priority (-1); the audio driver supplies no deadline reservation |
| `osfmk/kern/clock.c:1555`, `:1584` | Past deadline returns immediately; otherwise short/atomic-context waits may spin, normal waits use `assert_wait_deadline` and block |
| `osfmk/kern/sched_prim.c:1317` | Deadline wait uses SYS_NORMAL timer urgency; “no explicit leeway” does not promise an audio-critical timer |
| `osfmk/kern/timer_call.c:1859`, `:1920` | Normal system timers may receive coalescing slop, and wakeup still needs scheduler service |
| `osfmk/arm/arm_timer.c:309`, `:314` | Kernel timer defaults shift 3, maximum 1 ms. A fresh 5 ms wait implies at most min(5/8,1) = 0.625 ms base slop before the idle heuristic; **not sufficient alone** to explain >5 ms lateness |

There is no justified static claim of a 7.4 ms lock hold, a bad timer divisor,
or an ADSP late READ in this evidence. A scheduler/interrupt delay, same-device
holder, or extended previous-pass body must be separated at the missing timing
boundaries. Missing a HAL read deadline could still matter even with FIFO
continuity; the simplex saved PCM is the evidence that it did not cause a gap
in that particular capture.

### Shared-worker interference and buffer margins

| Budget / hold | Arithmetic and implication |
|---|---|
| HAL ring capacity | 7680/48000 = 160 ms, but this is capacity/modulus, not a guaranteed 160 ms producer lead |
| Speaker published latency | (240 engine + 4 x 480 DSP + 960 FIFO)/48000 = 65 ms; 480-frame safety offset = another separately published 10 ms; downstream graph latency is not measured by these constants |
| Mic published latency | (240 engine + 480 transport + 1920 FIFO)/48000 = 55 ms, safety another separately published 10 ms; not a scheduler allowance |
| Mic center/trough math | Completion pre-write bias = (4-1) x 480/2 = 720; center 960 reserve + 240 deadband + 720 bias = 1920. Center minus bias/deadband leaves 960 frames / 20 ms nominal trough; pending READ slots do not add available PCM |
| Isolated observed wake magnitude | 8.293 ms x 48 frames/ms = 398.064 frames, below 960-frame nominal reserve. This is consistent with zero loss, not a guarantee for arbitrary phases/bursts/work stalls |
| Worker revisit | Relative sleep adds 5 ms after health/drain/tick/property work, plus scheduler delay. A nominal 20 ms doorbell poll can batch roughly two 10 ms periods; longer service can batch four. There is no measured worst case |
| Queue warning versus audible gap | Four requests represent at most 40 ms of supplied PCM, while `AUDIO_LATE_MS=250` is a safety stop threshold. A 41–249 ms service pause can be audible without a late-period stop. Conversely DONE notifications can batch after data was consumed: observed host `in_flight` is not the hardware FIFO fill |
| Normal sliced amplifier status | A read is 5 I2C bytes including both addresses, 45 clock pulses. Default half=5 us gives 450 us bit delays + about 89 us start/stop/bus-free/mux = 539 us ideal minimum including mux. MMIO, barriers, preemption and stretch add time. One register per pass avoids the former whole-group block; no upper bound is measured |
| Old unsliced status group | 24 x approximately 0.539 ms = 12.94 ms ideal, already above a 5 ms revisit before OS overhead. Current code slices it; don't diagnose the already-removed 24-read block as current |
| VA local startup | Two decimators each wait 1 + optional 1 + 1 + 6 ms (`AudioVa.c:18`): **16–18 ms mandatory requested sleeps** on the audio worker, plus MMIO/mapping/scheduler time. With only one 10 ms speaker slot effectively left, this can exhaust lead; with four slots it need not |
| Speaker local startup while Mic runs | Four amplifiers x 10 ms sleep (`Fs19xx.c:280`, `AudioAmps.c:168`) = at least 40 ms plus synchronous I2C on the shared worker. Capture callbacks reach the inbox but cannot be processed by that worker during the local action |
| Speaker shutdown while Mic runs | Four x 35 ms (`AudioAmps.c:217`, `Fs19xx.c:239`) = at least 140 ms plus writes; longer than the mic's nominal 20 ms usable trough and 40 ms posted READ budget. This is a concrete exposure at speaker idle/fallback, not proof it occurred during steady music |
| Protection monitor | SET/GET waits are asynchronous; there is no 2 s lock hold. Battery IORegistry lookup, live parsing/logging and one status register do run synchronously. Their maximum cost and correlation with acoustic dips are unmeasured |

These **local action durations are worker occupancy, not device-lock hold
times**. No static upper bound exists for a preemptible sleep, I2C stretch or
kernel logging. In the new model, lock delay is an explicitly injected
alternative, not a fabricated measurement. The saved history's short capture
loss at speaker start is consistent with the startup exposure, but is not
evidence for recurring steady playback drops.

The Linux reference has independent APM/PRM locks and a per-graph data mutex,
yet shares its GPR RX workqueue and GLINK TX FIFO. Its FS19xx startup waits run
on delayed codec work, not on the GPR RX callback worker. Its recommended
capture period is 1920 stereo S16 frames (7680 bytes, 40 ms), four periods /
7680 frames (160 ms), satisfying the driver's 6144-byte minimum and 480-frame
step (`device/audio/README.md:169`, `L/q6apm-dai.c:29`, `:547`). These are a
supported recommendation, **not a recorded live Ubuntu duplex hw_params**.
Our 1920-byte / 10 ms READs are smaller; successful recorded READ returns prove
their acceptance on this firmware, not a shared Linux minimum constraint.

## Exact remaining observations (specified, not executed)

This job never connects to, boots, records on, or changes the tablet. A future
authorized observer can read the following on the **already installed failing
boot**, without installing an image or writing a control. ENOENT is an absent
observer, not a zero counter. Knob readback is not proof of a currently running
engine's settings: timing is latched at StartIO and graph requests at startup.

| Read / saved event | Required interpretation / hypothesis it settles |
|---|---|
| Installed release/KC provenance from the existing installation record and boot console; `sw_vers`, `sysctl kern.boottime kern.bootargs` | Distinguish installed boot from RAM and identify whether signed-age/burst-reserve code is actually in that KC. Keep raw boot args private |
| `sysctl machdep.piano_audio_timing machdep.piano_audio_age machdep.piano_audio_duplex_hold machdep.piano_audio_spk machdep.piano_audio_miplay machdep.piano_audio_gain` | Read only, no `-w`. Record requests and exact time. Timing 0/1 alone does not identify an already-running engine |
| `ioreg -r -c PianoAudioDevice -l`, `ioreg -r -c PianoAudio -l` | Device UIDs/transport, fixed ring and latencies, live engine/path states, readiness; sanitized fields only go in tracked summaries |
| Boot/current-window `HAL ring: output/input ... mode ... period ... seed ...`, `IO window start`, graph startup and mic-route lines | Prove actual mode240 versus mode480, current epoch/seed, protection/MIPLAY and route applied, start/stop order and held clocks. A ready file or requested knob is not an active graph |
| Full timestamped console/log window around the owner's existing playback/capture start, stutter and stop; `duplex-obs1` lines | Exact min-depth, depth-0 ticks, WRITE gap, queue-full, sound-ahead, fades/as-silent. If age=1 and false fades=0, exclude the historical age trigger for that window |
| `IO window stop` and `HAL ring read: output`; `HAL wake: output` | Compare nonzero upstream HAL periods to downstream DMA sound, active underruns/skips, FIFO drops, tick and slice maxima. Don't compare cumulative boot totals or count ordinary fade/stop tails as active loss |
| `sysctl machdep.piano_audio_ledger` promptly around a stutter, before overwrite | Retain header written/overwritten/capacity and every CSV column; AlRefill event 1 (`AudioLedger.h:9`) carries want/got/missing, mode, fill, skips, slips, producer lateness, HAL writeEnd and completion batch. For mode480 look for pre-trim fill >2400, skip, then shortage in a four-slot refill. A full reconstruction may need more than one row; unchanged cumulative skips are distinct from a new trim |
| `sysctl machdep.piano_audio_spk_state`, console `PianoSpeakerProtV1`, `PianoMiplayV1`, `STOPPED`, `duplex-hold1`, amp/gain lines | Active/requested graph, fallbacks, live-read progress, periods refused/late and amplifier status failures; distinguish worker shutdown/fallback gap from steady starvation |
| `mic HAL`, `mic HAL stop`, `mic frames`, `mic health`, `HAL wake: input`, `mic release` for the same run/seed | Requested = delivered + startup + missing + unavailable; mute is a subset of delivered. Post-prime missing/full/overrun/short READs, PCM totals and wire error counters must be separate from late host wake totals and intentional clock slips |
| Existing passive `sysctl machdep.piano_glink_fifo machdep.piano_glink_owner machdep.piano_smem_state` when present | FIFO indices/debt, remote/local intent availability, queue ownership and doorbell poll progress; preserve concurrent/unavailable/truncated markers. No extra doorbell ACK/read-clear, GPR GET or new tracing switch |
| Existing `PianoPower` quiet/guard/link-generation/crash and slow-callback lines in the same window | Separate charger quiet, transport restart or another client blocking DSP owner from audio-internal faults. Do not restart ADSP to investigate |
| App's current input route, raw Mic versus noise-reduced tap, VoiceProcessingIO use | VPIO/NS digital gating is a separate path; no assumption that an acoustic pause is a PCM transport gap |

Ledger fields use the worker's millisecond mach-time domain; `hal_host` is mach
ticks and requires the timebase before comparison. `producer_late_us=4294967295`
means unavailable concurrent context, not multi-second delay. Missing ledger
rows and overwritten events cannot prove zero loss. State sysctls are snapshots
of a changing owner, not transactional measurements. No existing exported
steady-state audio inbox-drop total was found; do not invent a
`machdep.piano_audio_inbox` or treat absence of a timeout as zero packet loss.

No new playback, control A/B, diagnostic recording, silent protection probe or
DSP profiling command is authorized or performed by this job. Passive reads
can classify a naturally occurring/owner-reported window. Acoustic proof still
needs a synchronized, owner-authorized listening/recording session; silent
transport acceptance alone cannot establish audible continuity.

### What would close each remaining gate

| Finding in a failing installed window | Decision before any implementation |
|---|---|
| Old policy enabled, ahead stamps and false fades while input is up; no upstream loss | Verify deployment/runtime selection of the already-existing signed-age fix. No new driver algorithm is justified |
| Actual mode480 plus four-slot completion/trim/short-read signature | Associate the exact production-code reproducer with live failure; choose ONE common burst-reserve policy for both engine batch sizes |
| Mode0, late producer then speaker FIFO shortages | Trace host output engine wait/body/lock and HAL write lead; a GPR clock-vote fix does not address this layer |
| FIFO supplied, no false fade/fallback, excessive WRITE gap or queue-full | Separate audio-worker occupancy from enqueue-to-publish/intent delay using existing ledger and passive transport state |
| Healthy supplied PCM and timely transport but acoustic dip | Downstream protection/MIPLAY/TDM/DSP clock/load remains unknown; Linux topology alone cannot close it |
| Input late wakes only, actual PCM still complete | Keep functional continuity separate from scheduling acceptance; do not widen a counter threshold as a “fix” |

The last case needs finer timing than the current exports provide. If existing
scheduler traces are available, inspect engine thread IDs and deadline timer
arm/expiry, runnable-to-running delay, lock acquisition and service-body
durations. Required boundaries are `D`, time before/after wait, before/after
device-lock acquisition, capture start/end and unlock; retain per-event
histograms/correlation, not only maxima. Expected signatures:

- Late wait return with long runnable delay: scheduling/service contention.
- Timer expiry itself late: APPS timer/IRQ service (then verify exact KC timer
  implementation and any coalescing/interrupt masking).
- Prompt wait return, late lock acquisition: identify that **device's** holder
  and hold interval; separate Mic and Speaker locks.
- Next pass already overdue after slow capture/logging: previous-body cost.

Those boundaries are **ours-missing** in the installed driver. Passive reads
may narrow the cause, but cannot retroactively recover them from the saved
469/25 totals. If existing traces do not contain them, an observation design
must be reviewed separately; this analysis does not authorize a speculative
priority, timer, DMA-cache or buffer-size fix image.

## Host validation and reproduction

All 19 existing PianoAudio executables compiled with the build script's
`-Wall -Wextra -Werror` C11/C++20 recipes and passed: audio, level dictionary,
GPR, AudioReach, sequence, protection, FS19xx, amps, PCM, controls, timing,
DSP link, session, live, prime, mic FIFO, VA, tap and preset. The preset FAT
image case ran with mtools (22 checks, no skip). The build script's host-only
portion was extracted and run; execution stopped before firmware staging,
kext compilation or KC/image operations. In particular:

- `live_host`: 995293 checks, including old/signed sound-age reproduction,
  duplex plans/backpressure, protection/MIPLAY, startup and sleep.
- `mic_fifo_host`: 6845646 checks over the saved count/byte replay and 285
  phase/rate cases. This is a bounded model, not measured wake timestamps.
- `prime_host`: 231 checks, sparse-ledger cold/warm startup and active/tail
  accounting.
- Added `duplex_schedule_host`: 1871588 checks, including actual mode480
  four-slot reserve failure and contiguous real-FIFO input under synthetic
  late waits/locks/previous-body stalls.
- Protection extraction (21), Dolby extraction (41) and shared firmware-loader
  host self-tests also passed on synthetic inputs.

Reproduce the new model from the repository root; it includes `AudioLive.c`
directly to drive the static refill boundary, as the existing mic FIFO test
does, so do not link a second copy of that file:

```sh
cc -std=c11 -Wall -Wextra -Werror -I package/PianoAudio \
  package/PianoAudio/tests/duplex_schedule_host.c \
  package/PianoAudio/AudioSpkProt.c package/PianoAudio/AudioSession.c \
  package/PianoAudio/AudioAmps.c package/PianoAudio/Fs19xx.c \
  package/PianoAudio/AudioSha256.c package/PianoAudio/AudioSequence.c \
  package/PianoAudio/PianoAudioReach.c package/PianoAudio/PianoGpr.c \
  package/PianoAudio/AudioSafety.c package/PianoAudio/AudioDspLink.c \
  -o /tmp/duplex_schedule_host
/tmp/duplex_schedule_host
python3 scripts/ci/privacy-scan.py --diff 3fcf74f4
```

Expected reserve-model output: mode 0 skipped 0 / short-reads 0 / missing 0;
mode 1 skipped 1 / short-reads 2 / missing 960. That passing assertion records
the existing optional-mode defect, not a fix or a hardware stutter waveform.
Privacy scan: **0 hits, 0 refused files**. Only this analysis and the offline
test are changed; driver/build/firmware/image files remain untouched.

## Fix-image gate

Path: IOAudio2 Speaker/Mic mapped rings -> independent host engine threads ->
AudioLive FIFOs -> shared worker/inbox -> GPR/GLINK -> AudioReach playback and
capture graphs -> TDM/FS19xx and VA/DMIC; protection adds the V/I graph and monitor.

Checks: `ours-OK`: independent device locks/rings, SPSC FIFO publication,
nonoverlapping DMA slots/parameters, GPR token/handle validation, asynchronous
PCM pumps, default signed age and mode240 burst reserve. `ours-missing`:
mode480 burst reserve, completion-triggered worker wake and separate engine
wait/lock/body observations. `ours-contended`: synchronous shared-worker local
actions, physical GLINK/ADSP resources and client enqueue lock. `unknown`:
installed failing KC and active engine mode/policy, current stutter signature,
actual lock/service latencies, HAL/firmware scheduling and downstream clocks.
Existing host tests and the added production-boundary model pass; privacy
scan against `3fcf74f4` returns 0 hits, 0 refused files.

Root cause: Historical unsigned timestamp race caused false-silence fades;
the signed-age correction is already in this baseline and saved same-boot A/B
confirmed it. Optional mode480 still has a statically proven trim/burst deficit
(2880 -> 960 before 1920-frame demand, two short reads / 960 missing frames),
but its involvement in the installed complaint is unconfirmed. Input host wake
lateness is measured after wait plus lock acquisition; scheduling, previous
body and lock delay remain indistinguishable from existing totals. Saved
simplex PCM has missing 0 and DSP late/errors 0. No new causal claim about the
installed system is closed by those historical counters.

ONE change: HOLD. No driver edit or fix image. First obtain the specified
read-only installed provenance, latched engine mode, age/fade, refill ledger,
transport and matching input health/wake observations. If mode480 and its
four-slot trim signature match, the candidate ONE change is to apply the
existing epoch/burst reserve policy to both batch sizes, with this reproducer
changed to demand zero active shortage. Otherwise reclassify from the actual
failure layer; do not reapply the age fix or combine speculative timer,
priority, clock-vote and buffer changes.
