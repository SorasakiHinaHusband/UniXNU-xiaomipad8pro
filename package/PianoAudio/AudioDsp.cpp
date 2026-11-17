// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoAudio level 2: the DSP link over PianoPower's client contract (package/include/PianoAdspContract.h, docs/drivers/charging.md
// section 9). The channel "adsp_apps" is registered (the DSP opens it: we answer; 20 receive intents of 512 bytes, the DT's
// qcom,intents), PianoPower's health is watched through AudioDspLink.h's guard, and one question goes to the DSP's graph manager:
// is the framework ready (APM_CMD_GET_SPF_STATE from the port set's APM port: Ubuntu's port 1 by default, piano-audio-ports=N for
// another, piano-audio-ports-try=1 to walk the others while nobody answers). Nothing else is sent at this level.
// Levels 4-7 (piano_audio_levels, AudioSession.h) then run on the same channel, inbox and guard, with the hardware of AudioBoard.cpp;
// from piano-audio=8 levels 8-10 (AudioLive.h) follow for the rest of the boot on what levels 5 and 6 set up, fed by the macOS
// devices through the dsp transport defined here (AudioPcm.h). PianoAudio never starts, stops or restarts the DSP and votes nothing:
// PianoPower owns it (piano-power>=2 in the same boot).
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <PianoAdspContract.h>
#include "AudioLog.h"
#include "AudioBoard.h"
#include "AudioDspLink.h"
#include "AudioLive.h"
#include "AudioPcm.h"
#include "AudioPower.h"
#include <sys/sysctl.h>
SYSCTL_DECL(_machdep);
#include "AudioSession.h"

extern "C" {
// Post-mortem: the link's state (AudioLink AlWaiting..AlOff) | the guard's reason << 8 | queries << 16.
volatile uint32_t piano_audio_dsp_word = 0;
// Post-mortem: levels 4-7: the session's phase | the results of levels 4..7 << 8 (two bits each: AudioLevel*).
volatile uint32_t piano_audio_levels_word = 0;
// Post-mortem: levels 8-10: the speaker path's state | the mic path's << 4 | the speaker's mode << 8 | its starts << 16.
volatile uint32_t piano_audio_live_word = 0;
}

namespace {

AudioInbox gInbox;
AudioLink gLink;
PianoAdspChannel *gChannel;
int gChannelUp;

void onState(void *, int up) {__atomic_store_n(&gChannelUp, up ? 1 : 0, __ATOMIC_RELEASE);}
// On PianoPower's DSP thread: copy and return (the contract's rule 2).
void onReceive(void *, const uint8_t *data, uint32_t length) {audio_inbox_put(&gInbox, data, length);}
const PianoAdspClient kClient = {onState, onReceive};

uint32_t nowMs()
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return uint32_t(ns / 1000000);
}

void observe(AdgObservation &o, const PianoAdspHealth &h)
{
    o.contract_ok = h.version == PIANO_ADSP_HEALTH_VERSION;
    o.dsp_up = h.dsp_up != 0;
    o.fatal = h.fatal != 0;
    o.fatal_reports = h.fatal_reports;
    o.link_generation = h.link_generation;
    o.charger_pd = h.charger_pd;
    o.charger_generation = h.charger_generation;
    o.quiet = h.quiet;
    o.crash = h.crash;
    o.channel_up = __atomic_load_n(&gChannelUp, __ATOMIC_ACQUIRE);
}

}  // namespace

// Runs on PianoAudio's thread until the exchange ends (ready, gave up, off) or wait_s pass; the summary for the log and ioreg.
extern "C" int piano_audio_dsp_link(uint32_t wait_s, uint32_t settle_ms, int ports, int try_ports, char *summary, size_t size)
{
    audio_inbox_init(&gInbox);
    audio_link_init_ports(&gLink, ports, try_ports ? (1u << AUDIO_PORT_SETS) - 1u : 0u, 10, settle_ms);
    const AudioPorts *set = audio_ports(gLink.set);
    piano_audio_log("dsp: GPR ports: set %d (%s: %s): APM from %#x, PRM from %#x, data from %#x%s", gLink.set, set->name, set->source,
                    set->apm, set->prm, set->data, try_ports ? "; the others are tried while nobody answers" : "");
    int lastSet = gLink.set;
    gChannel = piano_adsp_channel("adsp_apps", AUDIO_INBOX_BYTES, 20, 0, &kClient, nullptr);
    if (!gChannel) {
        snprintf(summary, size, "adsp_apps refused by PianoPower (name taken or slots full)");
        piano_audio_log("dsp: %s", summary);
        return -1;
    }
    piano_audio_log("dsp: channel adsp_apps registered (20 x %u-byte intents; the DSP opens it, we answer)", AUDIO_INBOX_BYTES);
    const uint32_t start = nowMs();
    int lastGuard = -1, lastChannel = -1;
    unsigned lastQueries = 0;
    uint8_t q[GPR_HEADER_BYTES], in[AUDIO_INBOX_BYTES];
    PianoAdspHealth h;
    AdgObservation o;
    while (gLink.state != AlReady && gLink.state != AlGaveUp && gLink.state != AlOff) {
        const uint32_t now = nowMs();
        if (now - start >= wait_s * 1000) break;
        bzero(&h, sizeof h);
        piano_adsp_health(&h);
        observe(o, h);
        for (uint32_t n; (n = audio_inbox_get(&gInbox, in, sizeof in)) != 0;) audio_link_receive(&gLink, in, n, now);
        const uint32_t n = audio_link_tick(&gLink, &o, now, q);
        if (n && piano_adsp_send(gChannel, q, n)) audio_link_unsent(&gLink);   // the queue is full: asked again at the next tick
        if (o.channel_up != lastChannel) {
            piano_audio_log("dsp: channel adsp_apps %s (PianoPower stage %d, dsp_up %u, charger_pd %#x gen %u, link gen %u, quiet %#x)",
                            o.channel_up ? "up" : "not up", h.stage, h.dsp_up, h.charger_pd, h.charger_generation, h.link_generation, h.quiet);
            lastChannel = o.channel_up;
        }
        if (gLink.guard.state != lastGuard) {
            piano_audio_log("dsp: guard %s%s%s", gLink.guard.state == AdgRun ? "run" : gLink.guard.state == AdgWait ? "wait" : "OFF",
                            gLink.guard.state == AdgOff ? ": " : "", gLink.guard.state == AdgOff ? adg_reason(gLink.guard.reason) : "");
            lastGuard = gLink.guard.state;
        }
        if (gLink.set != lastSet) {
            const AudioPorts *now = audio_ports(gLink.set);
            piano_audio_log("dsp: nobody answered from port %#x: GPR ports set %d (%s: %s) now: APM %#x, PRM %#x, data %#x",
                            audio_ports(lastSet)->apm, gLink.set, now->name, now->source, now->apm, now->prm, now->data);
            lastSet = gLink.set;
            lastQueries = 0;
        }
        if (gLink.spf.queries != lastQueries) {
            piano_audio_log("dsp: APM_CMD_GET_SPF_STATE %u sent from port %#x (so far: %u answers, last state %u, %u refused, %u timeouts)",
                            gLink.spf.queries, gLink.spf.port, gLink.spf.answers, gLink.spf.last_status, gLink.spf.errors, gLink.spf.timeouts);
            lastQueries = gLink.spf.queries;
        }
        piano_audio_dsp_word = uint32_t(gLink.state) | uint32_t(gLink.guard.reason) << 8 | gLink.spf.queries << 16;
        IOSleep(20);
    }
    piano_audio_dsp_word = uint32_t(gLink.state) | uint32_t(gLink.guard.reason) << 8 | gLink.spf.queries << 16;
    if (gLink.state == AlReady)
        snprintf(summary, size, "APM ready (status 1) after %u queries from port %#x (set %d, %s), %u ms after the first; inbox dropped %u",
                 gLink.spf.queries, gLink.spf.port, gLink.set, audio_ports(gLink.set)->name, gLink.ready_at - gLink.first_sent,
                 gInbox.dropped + gInbox.too_long);
    else if (gLink.state == AlOff)
        snprintf(summary, size, "stopped for the rest of the boot: %s (crash \"%s\")", adg_reason(gLink.guard.reason), h.crash);
    else if (gLink.state == AlGaveUp)
        snprintf(summary, size, "APM not ready after %u queries from port %#x (last status %u, refused %u, timeouts %u; sets tried %#x)",
                 gLink.spf.queries, gLink.spf.port, gLink.spf.last_status, gLink.spf.errors, gLink.spf.timeouts, gLink.tried);
    else
        snprintf(summary, size, "no answer within %u s (channel %s, guard %s, %u queries)", wait_s, o.channel_up ? "up" : "not up",
                 gLink.guard.state == AdgRun ? "run" : "wait", gLink.spf.queries);
    piano_audio_log("dsp: %s", summary);
    return gLink.state == AlReady ? 0 : -1;
}

// ------------------------------------------------------------------------------------------------------- levels 4-7
namespace {

AudioSession gSession;

void sessionLog(void *, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sessionLog(void *, const char *fmt, ...)
{
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    IOLog("PianoAudio: %s\n", line);
}

int sessionSend(void *, const uint8_t *packet, uint32_t length)
{
    return gChannel && piano_adsp_send(gChannel, packet, length) == 0 ? 0 : -1;
}

uint32_t levelsWord(const AudioSession &s)
{
    uint32_t w = uint32_t(s.phase) & 0xffu;
    for (unsigned l = 4; l <= 7; ++l) w |= (uint32_t(s.result[l]) & 3u) << (8 + 2 * (l - 4));
    return w;
}

// ------------------------------------------------------------------------------------------------ levels 8-10
// The dsp transport (AudioPcm.h): the devices' engine threads hand their periods to AudioLive.h's shared state; the first sound
// while the speaker path is down, and every start or stop of a device, wakes the live thread (which otherwise sleeps 2 s).
AudioLiveShared *gShared;
AudioLive *gLive;
IOLock *gWakeLock;
bool gWake;
uint32_t gIoSound, gIoUnderruns, gIoSkipped, gIoPolls;
uint64_t gIoDropped;
bool gIoWarm;
uint32_t gTickMaxUs, gSliceMaxUs;
uint32_t gPmRequest, gPmAck, gPmSleep, gPmGeneration;
int gPmResult, gPmFailed;


uint32_t elapsedUs(uint64_t start)
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    return uint32_t(ns / 1000u);
}

void liveWake()
{
    if (!gWakeLock) return;
    IOLockLock(gWakeLock);
    gWake = true;
    IOLockWakeup(gWakeLock, &gWake, true);
    IOLockUnlock(gWakeLock);
}

void liveSleep(uint32_t ms)
{
    IOLockLock(gWakeLock);
    if (!gWake) {
        uint64_t deadline;
        clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
        IOLockSleepDeadline(gWakeLock, &gWake, deadline, THREAD_UNINT);
    }
    gWake = false;
    IOLockUnlock(gWakeLock);
}

void dspRunning(void *, int output, int input)
{
    const bool wasOutput = __atomic_load_n(&gShared->want_out, __ATOMIC_ACQUIRE) != 0;
    if (gLive && output && !wasOutput) {
        gIoSound = gLive->sound_periods;
        gIoUnderruns = gLive->underruns;
        gIoSkipped = gLive->skipped;
        gIoPolls = gLive->status_polls;
        gIoDropped = gShared->dropped;
        gIoWarm = gLive->spk_state == LiveRunning && __atomic_load_n(&gShared->accepting, __ATOMIC_ACQUIRE);
        gTickMaxUs = gSliceMaxUs = 0;
        piano_audio_log("live: IO window start: warm %u; monitor-slice1:refill-first", unsigned(gIoWarm));
    } else if (gLive && !output && wasOutput) {
        piano_audio_log("live: IO window stop: warm %u sound %u underruns %u skips %u fifo-dropped %llu polls %u tick-max-us %u slice-tick-max-us %u",
                        unsigned(gIoWarm), gLive->sound_periods - gIoSound, gLive->underruns - gIoUnderruns,
                        gLive->skipped - gIoSkipped, (unsigned long long)(gShared->dropped - gIoDropped),
                        gLive->status_polls - gIoPolls, gTickMaxUs, gSliceMaxUs);
    }
    __atomic_store_n(&gShared->boundary_ms,nowMs(),__ATOMIC_RELEASE);
    audio_live_running(gShared, output, input);
    liveWake();
}
uint32_t dspPlay(void *, const int16_t *stereo, uint32_t frames)
{
    int wake = 0;
    const uint32_t n = audio_live_play(gShared, stereo, frames, nowMs(), &wake);
    if (wake) liveWake();
    return n;
}
uint32_t dspCapture(void *, int16_t *mono, uint32_t frames) {return audio_live_capture(gShared, mono, frames);}
void dspControl(void *, int volume, int mute)
{
    audio_live_control(gShared, volume, mute);
    liveWake();   // an unmute while sound plays brings the speaker path up again
}
const PianoPcmTransport kDsp = {"dsp", nullptr, AUDIO_LIVE_OUTPUT_LATENCY, AUDIO_LIVE_INPUT_LATENCY, dspRunning, dspPlay, dspCapture,
                                dspControl};

void liveSummary(const AudioLive &l, char *line, size_t size)
{
    snprintf(line, size, "level %u: speaker %s (%u starts, %u periods, %u with sound, %u underruns, %u slips), mic %s (%u starts, %u periods); "
             "frames offered %llu, taken %llu, dropped %llu", l.cfg.level, audio_live_state(l.spk_state), l.spk_starts, l.periods,
             l.sound_periods, l.underruns, l.drops + l.repeats, audio_live_state(l.mic_state), l.mic_starts, l.mic_periods,
             (unsigned long long)l.sh->offered, (unsigned long long)l.sh->taken, (unsigned long long)l.sh->dropped);
}

uint32_t liveWord(const AudioLive &l)
{
    return (uint32_t(l.spk_state) & 0xfu) | (uint32_t(l.mic_state) & 0xfu) << 4 | (uint32_t(l.spk_mode) & 0xffu) << 8 | l.spk_starts << 16;
}

struct sysctl_oid gLedgerOid;
int ledgerRead(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    if (req->newptr) return EPERM;
    if (!gLive) return ENXIO;
    constexpr size_t cap=100000;
    char *buf=static_cast<char *>(IOMallocData(cap));
    if (!buf) return ENOMEM;
    const AudioLedger *l=gLive->ledger;
    if (!l) {IOFreeData(buf,cap); return ENXIO;}
    const uint64_t n=__atomic_load_n(&l->written,__ATOMIC_ACQUIRE), first=n>AUDIO_LEDGER_CAP?n-AUDIO_LEDGER_CAP:0;
    size_t used=size_t(snprintf(buf,cap,"speaker-ledger1 written=%llu overwritten=%llu capacity=128\n"
        "seq,event,ms,epoch,mode,want_out,fill_before,fill_after,want,got,missing,producer_ms,producer_frames,producer_late_us,sent,returned,inflight,done_batch,monitor_start_ms,monitor_end_ms,monitor_field,skips,drops,repeats,slip,hal_sample,hal_host,hal_writeEnd,missing_total,boundary_ms,sleep_gen,state,U\n",
        (unsigned long long)n,(unsigned long long)first));
    int err=0;
    for (uint64_t i=first;i<n;++i) {
        uint64_t w[AUDIO_LEDGER_WORDS];
        if (!audio_ledger_get(l,i,w)) {err=EAGAIN; break;}
        used+=size_t(snprintf(buf+used,cap-used,"%llu",(unsigned long long)i));
        for (unsigned k=0;k<AUDIO_LEDGER_WORDS;++k)
            used+=size_t(snprintf(buf+used,cap-used,",%llu",(unsigned long long)w[k]));
        used+=size_t(snprintf(buf+used,cap-used,"\n"));
    }
    if (!err) err=SYSCTL_OUT(req,buf,used+1);
    IOFreeData(buf,cap);
    return err;
}
void registerLedger()
{
    bzero(&gLedgerOid,sizeof gLedgerOid);
    gLedgerOid.oid_parent=&sysctl__machdep_children; gLedgerOid.oid_number=OID_AUTO;
    gLedgerOid.oid_kind=int(CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_OID2);
    gLedgerOid.oid_name="piano_audio_ledger"; gLedgerOid.oid_handler=ledgerRead;
    gLedgerOid.oid_fmt="A"; gLedgerOid.oid_descr="Bounded speaker refill event ledger (no PCM); sleep-contract1";
    gLedgerOid.oid_version=SYSCTL_OID_VERSION;
    sysctl_register_oid(&gLedgerOid);
}

// For the rest of the boot (until PianoPower's health stops it): the live paths on the session's hardware.
void liveRun(const PianoAudioLevelArgs *a)
{
    AudioLiveConfig lc;
    audio_live_defaults(&lc, a->level);
    if (a->liveGain) lc.gain = a->liveGain;
    if (a->liveCeilingDb) lc.ceiling = audio_ceiling_db(a->liveCeilingDb);
    if (a->liveIdleMs) lc.idle_ms = a->liveIdleMs;
    gLive = IOMallocType(AudioLive);
    if (!gLive || audio_live_init(gLive, &lc, &gSession, gShared)) {
        piano_audio_log("live: levels 8-10 not run: %s", gLive ? "the session did not hand the hardware over" : "no memory");
        if (gLive) IOFreeType(gLive, AudioLive);
        gLive = nullptr;
        return;
    }
    gLive->ledger=static_cast<AudioLedger *>(IOMallocData(sizeof(AudioLedger)));
    if (gLive->ledger) {bzero(gLive->ledger,sizeof(AudioLedger)); registerLedger();}
    else {piano_audio_log("sleep-contract1: ledger allocation failed; readiness gate held"); return;}
    piano_audio_power_ready();
    uint32_t handled=0;
    uint8_t in[AUDIO_INBOX_BYTES];
    PianoAdspHealth h;
    AdgObservation o;
    char line[300] = "", last[300] = "";
    uint32_t lastWord = ~0u;
    while (!gLive->finished) {
        const uint32_t now = nowMs();
        bzero(&h,sizeof h); piano_adsp_health(&h); observe(o,h);
        const uint32_t request=__atomic_load_n(&gPmRequest,__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&gPmFailed,__ATOMIC_ACQUIRE)) gLive->sleep_failed=1;
        if (request!=handled && !gLive->sleep_failed) {
            if (__atomic_load_n(&gPmSleep,__ATOMIC_ACQUIRE)) {
                if (!gLive->sleeping) {
                    if (piano_audio_devices_power(1)) gLive->sleep_failed=1;
                    else audio_live_sleep(gLive,++gPmGeneration);
                    piano_audio_log("sleep-contract1: drain gen %u periods %u U %u",gPmGeneration,gLive->periods,gLive->underruns);
                }
            } else if (gLive->parked && adg_observe(gLive->guard,&o,now)==AdgRun) {
                if (audio_live_rearm(gLive) || piano_audio_devices_power(0)) gLive->sleep_failed=1;
                else {
                    piano_audio_log("sleep-contract1: on-ready gen %u periods %u U %u",gPmGeneration,gLive->periods,gLive->underruns);
                    __atomic_store_n(&gPmResult,0,__ATOMIC_RELEASE);
                    __atomic_store_n(&gPmAck,request,__ATOMIC_RELEASE); handled=request;
                }
            } else if (!gLive->parked || gLive->guard->state==AdgOff) gLive->sleep_failed=1;
        }
        for (uint32_t n; (n = audio_inbox_get(&gInbox, in, sizeof in)) != 0;) audio_live_receive(gLive, in, n, now);
        const uint32_t slices = gLive->status_slices;
        const bool playing = gLive->unmuted;
        const uint64_t tickStart = mach_absolute_time();
        const uint32_t d = audio_live_tick(gLive, &o, now);
        const uint32_t us = elapsedUs(tickStart);
        if (playing && us > gTickMaxUs) gTickMaxUs = us;
        if (slices != gLive->status_slices && us > gSliceMaxUs) gSliceMaxUs = us;
        if (request!=handled && __atomic_load_n(&gPmSleep,__ATOMIC_ACQUIRE)) {
            const int parked=audio_live_parked(gLive);
            if (parked!=0) {
                piano_audio_log("sleep-contract1: %s gen %u periods %u U %u",parked==1?"off-ready":"off-unsafe",gPmGeneration,gLive->periods,gLive->underruns);
                __atomic_store_n(&gPmResult,parked==1?0:-1,__ATOMIC_RELEASE);
                __atomic_store_n(&gPmAck,request,__ATOMIC_RELEASE); handled=request;
            }
        }
        const uint32_t word = liveWord(*gLive);
        piano_audio_live_word = word;
        if (word != lastWord) {   // the registry follows the paths' states (not every period)
            liveSummary(*gLive, line, sizeof line);
            if (strcmp(line, last)) piano_audio_set_property("piano-audio-live", line);
            strlcpy(last, line, sizeof last);
            lastWord = word;
        }
        if (!d) break;
        liveSleep(gLive->sleeping && d>5 ? 5 : d);
    }
    liveSummary(*gLive, line, sizeof line);
    piano_audio_set_property("piano-audio-live", line);
    piano_audio_log("live: %s", line);
}

}  // namespace

extern "C" void piano_audio_producer_trace(uint32_t frames, uint32_t late, uint64_t sample, uint64_t host, uint64_t end)
{
    if (!gShared) return;
    __atomic_add_fetch(&gShared->producer_seq,1u,__ATOMIC_SEQ_CST);
    __atomic_store_n(&gShared->producer_frames,frames,__ATOMIC_RELEASE);
    __atomic_store_n(&gShared->producer_late_us,late,__ATOMIC_RELEASE);
    __atomic_store_n(&gShared->hal_sample,sample,__ATOMIC_RELEASE);
    __atomic_store_n(&gShared->hal_host,host,__ATOMIC_RELEASE);
    __atomic_store_n(&gShared->hal_write_end,end,__ATOMIC_RELEASE);
    __atomic_store_n(&gShared->producer_ms,nowMs(),__ATOMIC_RELEASE);
    __atomic_add_fetch(&gShared->producer_seq,1u,__ATOMIC_SEQ_CST);
}

extern "C" int piano_audio_power_request(int sleep)
{
    if (!gLive || __atomic_load_n(&gPmFailed,__ATOMIC_ACQUIRE)) return -1;
    __atomic_store_n(&gPmSleep,sleep?1u:0u,__ATOMIC_RELEASE);
    const uint32_t n=__atomic_add_fetch(&gPmRequest,1u,__ATOMIC_ACQ_REL);
    liveWake();
    const uint32_t start=nowMs();
    while (nowMs()-start<8000) {
        if (__atomic_load_n(&gPmAck,__ATOMIC_ACQUIRE)==n) return __atomic_load_n(&gPmResult,__ATOMIC_ACQUIRE);
        IOSleep(10);
    }
    __atomic_store_n(&gPmFailed,1,__ATOMIC_RELEASE);
    liveWake();
    return -1;
}

extern "C" const PianoPcmTransport *piano_pcm_dsp(void) {return gShared ? &kDsp : nullptr;}

extern "C" int piano_audio_live_prepare(uint32_t volume_percent)
{
    if (gShared) return 0;
    gWakeLock = IOLockAlloc();
    AudioLiveShared *sh = gWakeLock ? IOMallocType(AudioLiveShared) : nullptr;
    int16_t *out = sh ? static_cast<int16_t *>(IOMallocData(AUDIO_LIVE_RING_BYTES)) : nullptr;
    int16_t *in = out ? static_cast<int16_t *>(IOMallocData(AUDIO_LIVE_MIC_RING_BYTES)) : nullptr;
    if (!in) {   // kept for the boot otherwise (the devices' engines may run at any time)
        if (out) IOFreeData(out, AUDIO_LIVE_RING_BYTES);
        if (sh) IOFreeType(sh, AudioLiveShared);
        return -1;
    }
    bzero(out, AUDIO_LIVE_RING_BYTES);
    bzero(in, AUDIO_LIVE_MIC_RING_BYTES);
    audio_live_shared_init(sh, out, in, audio_volume_q15(volume_percent > 100 ? 100 : int(volume_percent)));
    gShared = sh;
    return 0;
}

int piano_audio_levels(const PianoAudioLevelArgs *a, char *summary, size_t size)
{
    AudioSessionConfig c;
    audio_session_defaults(&c);
    c.level = a->level > 7 ? 7 : a->level;
    c.keep = a->level >= 8 && gShared;
    c.dsp = a->dsp && gChannel && gLink.state == AlReady;
    c.ports = gLink.set >= 0 ? gLink.set : AudioPortsUbuntu;
    c.lpass_read = int(a->lpassRead);
    c.preset_any = int(a->presetAny);
    c.mic = int(a->mic);
    c.silence_ms = a->silenceMs;
    c.tone = a->tone == 1 ? 1 : 0;
    c.tone_mask = a->toneMask;
    c.tone_ms = a->toneMs;
    c.gap_ms = a->gapMs;
    c.tone_gain = a->toneGain;
    AudioSessionOps ops = {};
    ops.clock_ms = [](void *) -> uint32_t {return nowMs();};
    ops.log = sessionLog;
    ops.send = sessionSend;
    const pianoaudio::BoardArgs b = {a->level, a->i2cSwap, a->i2cHalfUs, a->smmu, a->micGain};
    pianoaudio::boardSetup(ops, b);
    audio_session_init(&gSession, &c, &ops, c.dsp ? &gLink.guard : nullptr);
    // A budget well beyond what the levels need (level 6's silence and level 7's walk are seconds; every command is answered or
    // times out in 2 s): past it the session is stopped, with its undo commands first, then without.
    const uint32_t start = nowMs(), budget = 180000u + (c.silence_ms < AUDIO_SILENCE_MAX_MS ? c.silence_ms : AUDIO_SILENCE_MAX_MS);
    uint32_t aborted = 0;
    uint8_t in[AUDIO_INBOX_BYTES];
    PianoAdspHealth h;
    AdgObservation o;
    while (!gSession.finished) {
        const uint32_t now = nowMs();
        if (c.dsp) {
            bzero(&h, sizeof h);
            piano_adsp_health(&h);
            observe(o, h);
            for (uint32_t n; (n = audio_inbox_get(&gInbox, in, sizeof in)) != 0;) audio_session_receive(&gSession, in, n, now);
        }
        const uint32_t d = audio_session_tick(&gSession, c.dsp ? &o : nullptr, now);
        piano_audio_levels_word = levelsWord(gSession);
        if (!d) break;
        if (!aborted && now - start > budget) {
            aborted = now;
            audio_session_abort(&gSession, 0, now);
        } else if (aborted && now - aborted > 30000u) {
            audio_session_abort(&gSession, 1, now);
        }
        IOSleep(d);
    }
    piano_audio_levels_word = levelsWord(gSession);
    char board[400];
    pianoaudio::boardSummary(board, sizeof board);
    size_t n = 0;
    int failed = 0;
    for (unsigned l = 4; l <= c.level && l <= 7 && n < size; ++l) {
        n += size_t(snprintf(summary + n, size - n, "%s%u %s", l > 4 ? ", " : "", l, audio_level_result(gSession.result[l])));
        failed |= gSession.result[l] == AudioLevelFailed;
    }
    piano_audio_log("levels: %s; %s", summary, board);
    if (a->level >= 8) {
        if (gSession.kept) {
            piano_audio_set_property("piano-audio-levels", summary);
            piano_audio_log("levels 8-%u: the macOS devices on the DSP (the hardware of levels 5 and 6 kept)", a->level);
            liveRun(a);
        } else {
            piano_audio_log("levels 8-%u: not run: %s", a->level, !gShared ? "no dsp transport" : "levels 4, 5 and 6 must pass in the same boot");
        }
        if (n < size) n += size_t(snprintf(summary + n, size - n, ", 8-%u %s", a->level, gSession.kept ? "ran" : "not run"));
        failed |= !gSession.kept;
    }
    pianoaudio::boardTeardown();
    return failed ? -1 : 0;
}
