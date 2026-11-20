// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of levels 8-10 (AudioLive.c) in simulated time, after the real session (AudioSession.c, levels 4-6 with keep) handed
// the hardware over, against models of the DSP (PRM clocks and votes, the APM's graphs, two maps, the speaker's shared memory
// endpoint playing one 10 ms buffer after another on its own clock, the capture endpoint filling one), the four amplifiers on
// their bus and reset lines, the VA macro's DMIC path, and both macOS devices' engine threads (5 ms periods on the host clock):
//   - the commands of each plan in order, never two plan commands in flight at once (over both plans), from the port set's ports;
//   - speaker safety at every amplifier write and every buffer the DSP plays: no amplifier powered up but while the bit clock and
//     both speaker graphs run, none unmuted before four periods of the stream came back, no unmuted gain above the level's cap
//     (160 at level 8, 231 at 9), no gain above 231 ever, no sample above the level's ceiling, no slot changed while the DSP has
//     it, (in the clean runs) no sound played while an amplifier is muted (the fades come first), the amplifiers down before a
//     speaker graph stops and held in reset at the end;
//   - a device that plays only silence never brings anything up; sound brings the path up within a few hundred ms; silence, a
//     stopped device and the master mute take it down after idle_ms; a sound within the hold keeps it up;
//   - the clocks: the device 200 ppm fast or slow against the DSP: single frames slip, no underrun, the ring never overflows;
//   - the master volume ramps (no step above AUDIO_VOLUME_STEP a period); level 9's ceiling and gain;
//   - a refused period, an amplifier whose status changes, a plan step refused or unanswered: that path off for the boot; late
//     periods: down and back, the third time off; quiet: down and back; a fatal report: no command any more, everything held;
//   - the Mic (level 10): the capture plan with the VA macro's DMIC path between the prepares and the starts (and undone after the
//     stops), the two channels mixed to mono after the first 100 ms, both paths at once.
//   cc -std=c11 -Wall -Wextra -Werror live_host.c ../AudioLive.c ../AudioSession.c ../AudioAmps.c ../Fs19xx.c ../AudioSha256.c \
//      ../AudioSequence.c ../PianoAudioReach.c ../PianoGpr.c ../AudioSafety.c ../AudioDspLink.c -lm && ./a.out
#include <math.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../AudioLive.h"
#include "../PianoGpr.h"
#include "synthetic_fsm.h"

static unsigned checks;
static void dump(void);
static void engine(void);
static void deliver(int to_live);
static unsigned g_status_read_us, g_status_fail_at, g_loaded_reads;
#define CHECK(x) do {if (!(x)) {dump(); fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}

#define DMA_PHYS 0x9da00000ull
#define H_SPK 0x55u
#define H_MIC 0x77u
#define MAX_PERIODS 20000

struct Answer {uint64_t due_us; uint32_t n; int read_slot, plan; uint8_t b[96];};
struct Amp {uint8_t address; int present, released; uint16_t reg[256]; uint16_t inject;};
struct World {
    uint64_t now_us;
    // ---- the DSP
    struct Answer q[1024];
    unsigned nq;
    char cmd[1024][40];
    unsigned ncmd, outstanding;
    const char *fail_match;           // the first command (after `fail_after` commands) whose line is this: refused or, with drop, lost
    unsigned fail_after;
    int fail_seen, drop;
    uint32_t fail_status;
    unsigned writes, write_dones, reads, read_dones;
    int write_error_at, drop_writes_from, drop_writes_until;   // WRITEs by count: refused with status 5; not answered in [from, until)
    double dsp_ppm;                   // the DSP's clock against the host's
    uint64_t play_until_us, cap_until_us;
    unsigned queue_gaps, max_monitor_reads;
    int clock_on, va_votes, va_mclk, dev_started, str_started, mic_started, cap_started, mic_prepared, cap_prepared;
    int map_spk, map_mic;
    // the speaker-protection pair (AudioSpkProt.h)
    int vi_open, vi_started, prot_dev_open, re25_sets, oob_cfgs;
    unsigned vbat_sets, live_gets;
    uint32_t vbat_uv;
    unsigned stream_dones;
    uint8_t slot_copy[AUDIO_SPK_SLOTS][AUDIO_SPK_PERIOD_BYTES];
    int slot_busy[AUDIO_SPK_SLOTS];
    int capture_short;
    uint64_t cap_bytes;
    uint64_t cap_k;                   // the capture signal's sample counter
    // ---- what was played
    uint64_t played_periods, sound_periods, first_sound_played_us;
    int32_t peak, period_peak[MAX_PERIODS];
    uint32_t period_at[MAX_PERIODS];
    unsigned nperiods;
    // ---- the amplifiers, the pins, the VA macro
    struct Amp amp[4];
    unsigned power_ups, unmutes, transfers, gain_cap;
    int pins_on, va_on, va_calls;
    // ---- the devices' engines
    int out_running, in_running, sound_on, strict;
    int16_t amplitude;
    double phase, engine_ppm;
    double next_out_us, next_in_us;
    int hal_replay;
    uint32_t hal_mode, hal_io, hal_next, hal_read;
    double hal_origin_us;
    uint32_t steady_periods, steady_sound;
    uint64_t first_sound_offered_us;
    int wake;
    unsigned wakes;
    uint64_t mic_frames, mic_ok, mic_bad;
    int16_t mic_peak;
    // ---- PianoPower's health
    AdgObservation o;
    int off;
    unsigned sends_after_off, send_calls;
    int queue_full_every;
    // ---- what went wrong
    unsigned violations;
    char violation[200];
    char lines[8000][240];
    unsigned nlines;
};
static struct World w;
static int16_t hal_ring[PIANO_AUDIO_RING_FRAMES * 2u];
static AudioLiveShared sh;
static int16_t g_out_ring[AUDIO_LIVE_RING * 2], g_in_ring[AUDIO_LIVE_MIC_RING];
static AudioSession session;
static AudioLive live;
static AudioLedger live_ledger;
static AudioDspGuard guard;
static uint8_t preset[4096];
static uint8_t g_dma[AUDIO_DMA_BYTES];   // the audio slice the kernel maps
static unsigned preset_bytes;
static uint32_t g_t0;                // the live paths' start (ms)
static double g_ppm;                 // the next run's device clock against the DSP's
static const char *g_fail;           // the next run's failing live command (refused, or lost with g_drop)
static int g_drop, g_queue_full;
static int g_mic_clock;
static uint64_t g_mic_missing;
static unsigned g_wakes;
static int g_mark;
// speaker protection: the synthetic private files the session's ops carry, and the gauge's voltage
static uint8_t g_spk_file[8192], g_spk_re25[16];
static uint32_t g_spk_bytes, g_re25_bytes = 16, g_re25_q12[4] = {27000, 27500, 28000, 28500};
static int g_spk, g_spk_files, g_live_header_bad;
static uint32_t g_battery_uv = 4012000;
static uint32_t now_ms(void) {return (uint32_t)(w.now_us / 1000u);}

static void violate(const char *what)
{
    if (!w.violations++) snprintf(w.violation, sizeof w.violation, "%s (at %u ms)", what, now_ms());
}
static void dump(void)
{
    for (uint64_t n = live_ledger.written > AUDIO_LEDGER_CAP ? live_ledger.written - AUDIO_LEDGER_CAP : 0; n < live_ledger.written; ++n) {
        uint64_t e[AUDIO_LEDGER_WORDS];
        if (audio_ledger_get(&live_ledger, n, e))
            fprintf(stderr, "  ledger %llu event %llu ms %llu epoch %llu mode %llu want-out %llu fill %llu->%llu want/got %llu/%llu skips %llu\n",
                (unsigned long long)n, (unsigned long long)e[0], (unsigned long long)e[1], (unsigned long long)e[2], (unsigned long long)e[3],
                (unsigned long long)e[4], (unsigned long long)e[5], (unsigned long long)e[6], (unsigned long long)e[7], (unsigned long long)e[8], (unsigned long long)e[20]);
    }
    const unsigned from = w.nlines > 60 ? w.nlines - 60 : 0;
    for (unsigned i = from; i < w.nlines; ++i) fprintf(stderr, "  | %s\n", w.lines[i]);
    const unsigned c0 = w.ncmd > 60 ? w.ncmd - 60 : 0;
    for (unsigned i = c0; i < w.ncmd; ++i) fprintf(stderr, "  cmd %u: %s\n", i, w.cmd[i]);
    if (w.violations) fprintf(stderr, "  first violation: %s (%u in all)\n", w.violation, w.violations);
}
static void log_line(void *ctx, const char *fmt, ...)
{
    (void)ctx;
    va_list ap;
    va_start(ap, fmt);
    if (w.nlines < 8000) vsnprintf(w.lines[w.nlines++], sizeof w.lines[0], fmt, ap);
    va_end(ap);
    if (getenv("LIVE_LOG")) fprintf(stderr, "[%6u] %s\n", now_ms(), w.nlines ? w.lines[w.nlines - 1] : "");
}
static int logged(const char *needle)
{
    for (unsigned i = 0; i < w.nlines; ++i) if (strstr(w.lines[i], needle)) return 1;
    return 0;
}
static unsigned count_cmd(const char *line, unsigned from)
{
    unsigned n = 0;
    for (unsigned i = from; i < w.ncmd; ++i) n += !strcmp(w.cmd[i], line);
    return n;
}
static void note_cmd(const char *line) {if (w.ncmd < 1024) snprintf(w.cmd[w.ncmd++], sizeof w.cmd[0], "%s", line);}

// ------------------------------------------------------------------------------------------------------- the amplifiers
static int any_powered(void)
{
    for (int i = 0; i < 4; ++i) if (w.amp[i].released && !(w.amp[i].reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN)) return 1;
    return 0;
}
static int all_unmuted(void)
{
    for (int i = 0; i < 4; ++i)
        if (!w.amp[i].released || (w.amp[i].reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN) || (w.amp[i].reg[FS19XX_REG_DACCTRL] & FS19XX_DACCTRL_MUTE_BIT))
            return 0;
    return 1;
}
static void amp_defaults(struct Amp *a)
{
    memset(a->reg, 0, sizeof a->reg);
    a->reg[FS19XX_REG_DEVID] = FS19XX_DEVID; a->reg[FS19XX_REG_REVID] = 0x00b1; a->reg[FS19XX_REG_CHIPINI] = 0x0003;
    a->reg[FS19XX_REG_PWRCTRL] = FS19XX_PWRCTRL_PWDN; a->reg[FS19XX_REG_VOLCTRL] = 0xffc0;
    a->reg[FS19XX_REG_DACCTRL] = FS19XX_DACCTRL_MUTE; a->reg[FS19XX_REG_I2SCTRL] = 0x0003;
}
static void amp_derive(struct Amp *a)
{
    const int on = !(a->reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN) && a->reg[FS19XX_REG_SYSCTRL] == FS19XX_SYSCTRL_ON;
    const int clocked = w.clock_on && w.dev_started;
    a->reg[FS19XX_REG_DIGSTAT] = on && clocked && a->reg[FS19XX_REG_DACCTRL] == FS19XX_DACCTRL_UNMUTE ? FS19XX_DIGSTAT_DACRUN : 0;
    a->reg[FS19XX_REG_STATUS] = (uint16_t)((clocked ? 0x0040 : 0) | (on && clocked ? 0x0002 : 0) | a->inject);
}
static struct Amp *amp_at(uint8_t address)
{
    for (int i = 0; i < 4; ++i) if (w.amp[i].address == address && w.amp[i].present && w.amp[i].released) return &w.amp[i];
    return 0;
}
static int transfer(void *ctx, uint8_t address, const uint8_t *out, uint32_t out_len, uint8_t *in, uint32_t in_len)
{
    (void)ctx;
    w.transfers++;
    w.now_us += 300;
    if (g_status_read_us && in_len == 2 && live.spk_state == LiveRunning && live.unmuted) {
        /* Engine and DSP clocks continue while the owner is on the bus. There
         * is no refill here: completed periods stay free until tick returns. */
        for (unsigned us = 0; us < g_status_read_us; us += 500) {
            w.now_us += 500;
            engine();
            deliver(1);
        }
        if (++g_loaded_reads == g_status_fail_at) return -1;
    }
    struct Amp *a = amp_at(address);
    if (!a) return -1;
    amp_derive(a);
    if (out_len == 3 && !in_len) {
        const uint8_t reg = out[0];
        const uint16_t v = (uint16_t)(out[1] << 8 | out[2]);
        const int unmuted = !(a->reg[FS19XX_REG_DACCTRL] & FS19XX_DACCTRL_MUTE_BIT);
        if (reg == FS19XX_REG_VOLCTRL && (v >> 8) > FS19XX_GAIN_CAP) violate("an amplifier gain above 231");
        if (reg == FS19XX_REG_VOLCTRL && unmuted && (v >> 8) > w.gain_cap) violate("an unmuted amplifier's gain above the level's cap");
        if (reg == FS19XX_REG_DACCTRL && !(v & FS19XX_DACCTRL_MUTE_BIT) && (a->reg[FS19XX_REG_VOLCTRL] >> 8) > w.gain_cap)
            violate("an amplifier unmuted at a gain above the level's cap");
        if (reg == FS19XX_REG_PWRCTRL && !(v & (FS19XX_PWRCTRL_PWDN | FS19XX_PWRCTRL_I2CR))) {
            w.power_ups++;
            if (!w.clock_on || !w.dev_started || !w.str_started) violate("an amplifier powered up without the bit clock and both graphs");
            if (w.prot_dev_open && !w.vi_started) violate("an amplifier powered up on the protected graph without the V/I graph");
        }
        if (reg == FS19XX_REG_DACCTRL && !(v & FS19XX_DACCTRL_MUTE_BIT)) {
            w.unmutes++;
            if (w.stream_dones < AUDIO_UNMUTE_PERIODS) violate("an amplifier unmuted before four periods of the stream came back");
            if (a->reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN) violate("an amplifier unmuted while powered down");
        }
        if (reg == FS19XX_REG_PWRCTRL && (v & FS19XX_PWRCTRL_I2CR)) {amp_defaults(a); return -1;}
        a->reg[reg] = v;
        amp_derive(a);
        return 0;
    }
    if (out_len == 1 && in_len == 2) {
        in[0] = (uint8_t)(a->reg[out[0]] >> 8); in[1] = (uint8_t)a->reg[out[0]];
        return 0;
    }
    return -1;
}
static void sleep_us(void *ctx, uint32_t us) {(void)ctx; w.now_us += us;}
static const Fs19xxBus bus = {0, transfer, sleep_us};
static int reset_line(void *owner, unsigned slot, int release)
{
    (void)owner;
    if (slot >= 4) return -1;
    struct Amp *a = &w.amp[slot];
    if (release && !a->released) amp_defaults(a);
    if (!release) amp_defaults(a);
    a->released = release;
    return 0;
}

// ------------------------------------------------------------------------------------------------------------- the DSP
static struct Answer *queue(uint64_t due_us, const GprPacket *r, const uint32_t *words, unsigned n)
{
    CHECK(w.nq < 1024);
    struct Answer *a = &w.q[w.nq++];
    a->due_us = due_us;
    a->read_slot = -1;
    a->plan = 0;
    a->n = gpr_encode(a->b, sizeof a->b, r, words, 4 * n);
    CHECK(a->n);
    return a;
}

// An out-of-band payload: the command header names an address in the slice (the region mapped at DMA_PHYS).
static const uint8_t *oob_payload(const GprPacket *p)
{
    const uint8_t *d = p->payload;
    if (p->payload_bytes != 16 || !le32(d + 8) || (p->opcode != AR_OP_GRAPH_OPEN && p->opcode != AR_OP_SET_CFG && p->opcode != AR_OP_GET_CFG))
        return 0;
    const uint64_t a = le32(d) | (uint64_t)le32(d + 4) << 32;
    if (a >> 32 != 1 || (uint32_t)a < DMA_PHYS + AUDIO_DMA_PARAM || (uint32_t)a + le32(d + 12) > DMA_PHYS + AUDIO_DMA_PARAM + AUDIO_DMA_PARAM_BYTES ||
        a % 32 || le32(d + 8) != H_SPK) {
        violate("an out-of-band payload outside the parameter area or another map");
        return 0;
    }
    return g_dma + ((uint32_t)a - DMA_PHYS);
}

static void describe(const GprPacket *p, char *s, unsigned size)
{
    const uint8_t *d = p->payload;
    const uint8_t *o = oob_payload(p);
    if (o) {
        if (p->opcode == AR_OP_GRAPH_OPEN) snprintf(s, size, "OPEN %#x", le32(o + 20));
        else if (p->opcode == AR_OP_SET_CFG) snprintf(s, size, "CFG %#x %#x oob", le32(o), le32(o + 4));
        else snprintf(s, size, "GET %#x %#x", le32(o), le32(o + 4));
        return;
    }
    if (p->opcode == AR_OP_SET_CFG && le32(d + 16) == SPK_IID_SP_RX) {snprintf(s, size, "CFG 0x4046 %#x", le32(d + 20)); return;}
    switch (p->opcode) {
    case AR_OP_PRM_REQUEST:
        if (le32(d + 20) == 0x08001032u) snprintf(s, size, "PRM VOTE %u", le32(d + 32));
        else snprintf(s, size, "PRM REQ %#x %u", le32(d + 36), le32(d + 40));
        break;
    case AR_OP_PRM_RELEASE:
        if (le32(d + 20) == 0x08001032u) snprintf(s, size, "PRM UNVOTE %u", le32(d + 32));
        else snprintf(s, size, "PRM REL %#x", le32(d + 36));
        break;
    case AR_OP_GRAPH_OPEN: snprintf(s, size, "OPEN %#x", le32(d + 36)); break;
    case AR_OP_GRAPH_PREPARE: snprintf(s, size, "PREPARE %#x", le32(d + 36)); break;
    case AR_OP_GRAPH_START: snprintf(s, size, "START %#x", le32(d + 36)); break;
    case AR_OP_GRAPH_STOP: snprintf(s, size, "STOP %#x", le32(d + 36)); break;
    case AR_OP_GRAPH_CLOSE: snprintf(s, size, "CLOSE %#x", le32(d + 36)); break;
    case AR_OP_SET_CFG: snprintf(s, size, "CFG %#x", le32(d + 16)); break;
    case AR_OP_MEM_MAP: snprintf(s, size, "MAP %#x%08x", le32(d + 12), le32(d + 8)); break;
    case AR_OP_MEM_UNMAP: snprintf(s, size, "UNMAP %#x", le32(d)); break;
    default: snprintf(s, size, "OP %#x", p->opcode);
    }
}

static uint64_t dsp_period_us(void) {return (uint64_t)(10000.0 / (1.0 + w.dsp_ppm * 1e-6) + 0.5);}

static void dsp_write(const GprPacket *p)
{
    const uint8_t *d = p->payload;
    w.writes++;
    if (p->src_port != session.ports->data || p->dst_port != AR_IID_PLAY_SHMEM) violate("a WRITE from the wrong port");
    if (!w.str_started || !w.dev_started || !w.clock_on) violate("a WRITE without the running graphs");
    const uint64_t address = le32(d) | (uint64_t)le32(d + 4) << 32;
    const uint64_t offset = (address & 0xffffffffu) - DMA_PHYS;
    const unsigned slot = (unsigned)(offset / AUDIO_SPK_PERIOD_BYTES);
    if (address >> 32 != 1 || offset % AUDIO_SPK_PERIOD_BYTES || slot >= AUDIO_SPK_SLOTS || le32(d + 8) != H_SPK || le32(d + 12) != AUDIO_SPK_PERIOD_BYTES) {
        violate("a WRITE of another buffer");
        return;
    }
    if (w.slot_busy[slot]) violate("a WRITE of a slot the DSP still has");
    w.slot_busy[slot] = 1;
    memcpy(w.slot_copy[slot], g_dma + offset, AUDIO_SPK_PERIOD_BYTES);
    const int k = (int)w.writes;
    if (k >= w.drop_writes_from && k < w.drop_writes_until) {w.slot_busy[slot] = 0; return;}   // never played, never answered
    GprPacket r = {.dst_domain = GPR_DOMAIN_APPS, .src_domain = GPR_DOMAIN_ADSP, .src_port = p->dst_port, .dst_port = p->src_port,
                   .token = p->token, .opcode = AR_OP_WRITE_DONE};
    const uint32_t words[4] = {le32(d), le32(d + 4), le32(d + 8), k == w.write_error_at ? 5u : 0u};
    if (g_status_read_us && live.unmuted && w.out_running && w.play_until_us < w.now_us) ++w.queue_gaps;
    uint64_t at = w.play_until_us > w.now_us ? w.play_until_us : w.now_us;
    at += dsp_period_us();
    w.play_until_us = at;
    struct Answer *a = queue(at, &r, words, 4);
    a->read_slot = -2 - (int)slot;   // a speaker slot to "play" at delivery
}

// The DSP plays a speaker slot when its WRITE_DONE is due.
static void play_slot(unsigned slot)
{
    w.slot_busy[slot] = 0;
    const int16_t *pcm = (const int16_t *)(const void *)w.slot_copy[slot];
    // a slot the kernel changed while the DSP had it, but for zeros (a stop for trouble zeroes the buffer at once)
    const uint8_t *now = g_dma + slot * AUDIO_SPK_PERIOD_BYTES;
    if (memcmp(w.slot_copy[slot], now, AUDIO_SPK_PERIOD_BYTES)) {
        int zeros = 1;
        for (unsigned i = 0; i < AUDIO_SPK_PERIOD_BYTES; ++i) zeros &= !now[i];
        if (!zeros) violate("a slot changed while the DSP had it");
    }
    int32_t peak = 0;
    for (uint32_t i = 0; i < AUDIO_PERIOD_FRAMES * 4; ++i) {
        const int v = abs(pcm[i]);
        if (v > peak) peak = v;
    }
    if (peak > live.cfg.ceiling) violate("a sample above the level's ceiling");
    if (peak > w.peak) w.peak = peak;
    if (w.nperiods < MAX_PERIODS) {w.period_at[w.nperiods] = now_ms() - g_t0; w.period_peak[w.nperiods++] = peak;}
    w.played_periods++;
    if (peak) {
        w.sound_periods++;
        if (!w.first_sound_played_us) w.first_sound_played_us = w.now_us;
        if (w.strict && !all_unmuted()) violate("sound played while an amplifier is muted (a fade was cut)");
    }
}

static void dsp_read(const GprPacket *p)
{
    const uint8_t *d = p->payload;
    w.reads++;
    if (p->payload_bytes != 32) violate("a READ with the wrong V2 payload size");
    if (p->src_port != session.ports->data || p->dst_port != AR_IID_CAP_SHMEM) violate("a READ from the wrong port");
    if (!w.mic_started || !w.cap_started || !w.va_mclk || w.va_votes != 2) violate("a READ without the running capture graphs");
    if (!w.va_on) violate("a READ without the VA macro's DMIC path");
    const uint64_t address = le32(d) | (uint64_t)le32(d + 4) << 32;
    const uint64_t offset = (address & 0xffffffffu) - DMA_PHYS - AUDIO_DMA_MIC;
    const unsigned slot = (unsigned)(offset / AUDIO_MIC_PERIOD_BYTES);
    if (address >> 32 != 1 || offset % AUDIO_MIC_PERIOD_BYTES || slot >= AUDIO_MIC_SLOTS || le32(d + 8) != H_MIC || le32(d + 12) != AUDIO_MIC_PERIOD_BYTES) {
        violate("a READ of another buffer");
        return;
    }
    GprPacket r = {.dst_domain = GPR_DOMAIN_APPS, .src_domain = GPR_DOMAIN_ADSP, .src_port = p->dst_port, .dst_port = p->src_port,
                   .token = p->token, .opcode = AR_OP_READ_DONE};
    const uint32_t short_bytes[] = {1920, 1916, 960, 0, 4, 576};
    const uint32_t bytes = g_mic_clock && w.reads == 1 ? 576 : w.capture_short ? short_bytes[w.reads % 6] : AUDIO_MIC_PERIOD_BYTES;
    const uint32_t words[14] = {0, le32(d), le32(d + 4), H_MIC, bytes,
                                261034941u + 10000u * w.reads, 1, 0x80000000u, 1, 0, 0, 0, 0, 0};
    uint64_t at = w.cap_until_us > w.now_us ? w.cap_until_us : w.now_us;
    at += g_mic_clock && w.reads == 1 ? 30000 : dsp_period_us();
    w.cap_until_us = at;
    struct Answer *a = queue(at, &r, words, 14);
    a->read_slot = (int)slot;
}

// The DSP's capture: a square wave of +-8000 on DEC0 and +-4000 on DEC1 (96 samples a cycle): the mono mix is +-6000.
static int g_route_fixture;
static void fill_capture(unsigned slot, uint32_t bytes, uint32_t offset)
{
    uint8_t *base = g_dma + AUDIO_DMA_MIC + slot * AUDIO_MIC_PERIOD_BYTES;
    memset(base, 0x55, AUDIO_MIC_PERIOD_BYTES); /* stale padding must not become PCM */
    int16_t *pcm = (int16_t *)(void *)(base + offset);
    w.cap_bytes += bytes;
    for (uint32_t i = 0; i < bytes / 4; ++i, ++w.cap_k) {
        const int hi = w.cap_k % 96 < 48;
        pcm[2 * i] = g_route_fixture ? (hi ? INT16_MAX : INT16_MIN) : (int16_t)(hi ? 8000 : -8000);
        pcm[2 * i + 1] = (int16_t)(hi ? 4000 : -4000);
    }
}

static int dsp_send(void *ctx, const uint8_t *b, uint32_t n)
{
    (void)ctx;
    w.send_calls++;
    if (w.queue_full_every && w.send_calls % (unsigned)w.queue_full_every == 0) return -1;
    if (w.off) w.sends_after_off++;
    GprPacket p;
    CHECK(gpr_decode(b, n, &p) == GprOk && p.size == n && n <= GPR_MAX_PACKET);
    if (p.opcode == AR_OP_WRITE) {dsp_write(&p); return 0;}
    if (p.opcode == AR_OP_READ) {dsp_read(&p); return 0;}
    char line[40];
    describe(&p, line, sizeof line);
    note_cmd(line);
    if (w.outstanding) violate("two DSP commands in flight at once");
    const int prm = p.opcode == AR_OP_PRM_REQUEST || p.opcode == AR_OP_PRM_RELEASE;
    if (p.src_port != (prm ? session.ports->prm : session.ports->apm)) violate("a command from the wrong port");
    uint32_t status = 0;
    if (w.fail_match && !w.fail_seen && w.ncmd > w.fail_after && !strcmp(line, w.fail_match)) {
        w.fail_seen = 1;
        if (w.drop) return 0;   // lost: no answer will come
        status = w.fail_status;
    }
    if (!status) {
        if (!strcmp(line, "PRM REQ 0x202 6144000")) w.clock_on = 1;
        if (!strcmp(line, "PRM REL 0x202")) {if (w.dev_started || w.str_started) violate("the bit clock released under running graphs"); w.clock_on = 0;}
        if (!strncmp(line, "PRM VOTE", 8)) w.va_votes++;
        if (!strncmp(line, "PRM UNVOTE", 10)) w.va_votes--;
        if (!strcmp(line, "PRM REQ 0x30c 19200000")) w.va_mclk = 1;
        if (!strcmp(line, "PRM REL 0x30c")) {if (w.va_on) violate("the VA mclk released under the DMIC path"); w.va_mclk = 0;}
        if (!strcmp(line, "START 0x4005")) {if (!w.clock_on) violate("the device graph started without the bit clock"); w.dev_started = 1;}
        // the protection pair (round 2, Android's order): the speaker graph (SP_RX, SP_TX's control peer) opened before the V/I
        // graph; V/I started before the speaker graph and stopped after it
        if (!strcmp(line, "OPEN 0xb00000d5")) {if (!w.prot_dev_open) violate("the V/I graph opened before the speaker graph"); w.vi_open = 1;}
        if (!strcmp(line, "START 0xb00000d5")) {if (!w.clock_on || !w.vi_open) violate("the V/I graph started without the bit clock"); w.vi_started = 1;}
        if (!strcmp(line, "OPEN 0xb0000002")) w.prot_dev_open = 1;
        if (!strcmp(line, "START 0xb0000002")) {
            if (!w.clock_on || !w.vi_started) violate("the protected speaker graph started without the clock or the V/I graph");
            if (!w.re25_sets) violate("the protected speaker graph started without Re25");
            w.dev_started = 1;
        }
        if (!strcmp(line, "STOP 0xb0000002")) {if (any_powered()) violate("a speaker graph stopped with an amplifier powered up"); w.dev_started = 0;}
        if (!strcmp(line, "STOP 0xb00000d5")) {if (w.dev_started) violate("the V/I graph stopped under the running speaker graph"); w.vi_started = 0;}
        if (!strcmp(line, "CLOSE 0xb0000002")) {if (w.vi_open) violate("the speaker graph closed before the V/I graph"); w.prot_dev_open = 0;}
        if (!strcmp(line, "CLOSE 0xb00000d5")) {if (w.vi_started) violate("the V/I graph closed while running"); w.vi_open = 0;}
        if (!strcmp(line, "CFG 0x4046 0x10001fa7")) {
            w.re25_sets++;
            if (le32(p.payload + 24) != 36 || le32(p.payload + 32) != 0x00040000u) violate("a Re25 that is not 36 bytes for four channels");
        }
        if (!strcmp(line, "CFG 0x4046 0x10001fb5")) {
            w.vbat_sets++; w.vbat_uv = le32(p.payload + 36);
            if (!w.dev_started || le32(p.payload + 32) != 1) violate("a battery voltage outside the running protected graph");
        }
        if (strstr(line, " oob") && p.opcode == AR_OP_SET_CFG) w.oob_cfgs++;
        if (!strcmp(line, "START 0x4001")) {w.str_started = 1; w.stream_dones = 0;}
        if (!strcmp(line, "STOP 0x4001") || !strcmp(line, "STOP 0x4005")) {
            if (any_powered()) violate("a speaker graph stopped with an amplifier powered up");
            if (line[10] == '1') w.str_started = 0; else w.dev_started = 0;
        }
        if (!strcmp(line, "PREPARE 0x4008")) w.mic_prepared = 1;
        if (!strcmp(line, "PREPARE 0x4003")) w.cap_prepared = 1;
        if (!strcmp(line, "START 0x4008")) w.mic_started = 1;
        if (!strcmp(line, "START 0x4003")) w.cap_started = 1;
        if (!strcmp(line, "STOP 0x4008")) w.mic_started = 0;
        if (!strcmp(line, "STOP 0x4003")) w.cap_started = 0;
        if (!strcmp(line, "CLOSE 0x4008")) {if (w.va_on) violate("the mic graph closed under the DMIC path"); w.mic_prepared = 0;}
        if (!strcmp(line, "CLOSE 0x4003")) w.cap_prepared = 0;
    }
    GprPacket r = {.dst_domain = GPR_DOMAIN_APPS, .src_domain = GPR_DOMAIN_ADSP, .src_port = p.dst_port, .dst_port = p.src_port,
                   .token = p.token};
    const uint64_t due = w.now_us + 1000;
    w.outstanding = 1;
    if (p.opcode == AR_OP_MEM_MAP && !status) {
        r.opcode = AR_OP_MEM_MAP_DONE;
        const uint32_t h = le32(p.payload + 8) == (uint32_t)DMA_PHYS ? H_SPK : H_MIC;
        if (h == H_SPK) w.map_spk++; else w.map_mic++;
        queue(due, &r, &h, 1)->plan = 1;
        return 0;
    }
    if (p.opcode == AR_OP_MEM_UNMAP && !status) {if (le32(p.payload) == H_SPK) w.map_spk--; else w.map_mic--;}
    if (p.opcode == AR_OP_GET_CFG && !status) {
        // the live data: the header kept, four channels {Re (Re25 + 2 %), temperature, 0, f0 Q8, Q, 0}
        uint8_t *o = (uint8_t *)(uintptr_t)oob_payload(&p);
        if (!o || !w.dev_started) violate("a live-data GET outside the running protected graph");
        else {
            if (le32(o) != SPK_IID_SP_RX || le32(o + 4) != SPK_PID_LIVE || le32(o + 8) != SPK_LIVE_BYTES) violate("a live-data GET of another header");
            for (unsigned c = 0; c < 4; ++c) {
                const uint32_t words[6] = {g_re25_q12[c] * 102u / 100u, 31u + c, 0, (800u + 10u * c) * 256u, 3u + c, 0};
                for (unsigned k = 0; k < 6; ++k) for (unsigned b = 0; b < 4; ++b) o[16 + 24 * c + 4 * k + b] = (uint8_t)(words[k] >> (8 * b));
            }
            if (g_live_header_bad) o[4] ^= 1;
        }
        w.live_gets++;
        r.opcode = AR_OP_RSP_GET_CFG;
        const uint32_t ok = 0;
        queue(due, &r, &ok, 1)->plan = 1;
        return 0;
    }
    r.opcode = p.opcode == AR_OP_PRM_REQUEST ? AR_OP_PRM_REQUEST_DONE : p.opcode == AR_OP_PRM_RELEASE ? AR_OP_PRM_RELEASE_DONE : GPR_OP_BASIC_RESULT;
    const uint32_t words[2] = {p.opcode, status};
    queue(due, &r, words, 2)->plan = 1;
    return 0;
}

// ---------------------------------------------------------------------------------------------------- the owner's side
static void lpass_read(void *ctx) {(void)ctx;}
static uint32_t battery_uv(void *ctx) {(void)ctx; return g_battery_uv;}
static int tdm_pins(void *ctx, int on) {(void)ctx; w.pins_on = on; return 0;}
static int smmu(void *ctx) {(void)ctx; return 0;}
static void publish(void *ctx) {(void)ctx;}
static int g_va_fail_off;
static int va_mic(void *ctx, int on)
{
    (void)ctx;
    w.va_calls++;
    note_cmd(on ? "VA ON" : "VA OFF");
    if (w.va_votes != 2 || !w.va_mclk) violate("the VA macro touched without both votes and the mclk");
    if (on && (!w.mic_prepared || !w.cap_prepared || w.mic_started || w.cap_started)) violate("the DMIC path on outside prepared, not started graphs");
    if (!on && (w.mic_started || w.cap_started)) violate("the DMIC path off under running capture graphs");
    w.va_on = on;
    return !on && g_va_fail_off ? -1 : 0;
}

// ------------------------------------------------------------------------------------------------------ the engines
static void engine(void)
{
    const PianoAudioTiming *tp = piano_audio_timing_profile(w.hal_mode);
    const uint32_t period = w.hal_replay ? tp->periodFrames : 240u;
    const double out_period = (double)period * 1000000.0 / 48000.0 / (1.0 + w.engine_ppm * 1e-6);
    if (w.hal_replay && w.out_running) {
        const uint32_t now_frame = (uint32_t)((w.now_us - w.hal_origin_us) * 48000.0 / 1000000.0);
        while (w.hal_next <= now_frame) {
            const uint32_t first = w.hal_next + tp->safetyOffset + w.hal_io;
            for (uint32_t i = 0; i < w.hal_io; ++i) {
                const int16_t v = w.sound_on ? (int16_t)lrint(w.amplitude * sin(2 * M_PI * 1000.0 * (first+i) / 48000.0)) : 0;
                const uint32_t at = (first + i) % tp->ioBufferFrames;
                hal_ring[2u*at] = v;
                hal_ring[2u*at+1u] = (int16_t)-v;
            }
            w.hal_next += w.hal_io;
        }
    }
    if (w.out_running && w.now_us >= (uint64_t)w.next_out_us) {
        int16_t buf[PIANO_AUDIO_PERIOD_FRAMES * 2];
        for (unsigned i = 0; i < period; ++i) {
            const int16_t v = w.sound_on ? (int16_t)lrint(w.amplitude * sin(w.phase)) : 0;
            w.phase += 2 * M_PI * 1000.0 / 48000.0;
            buf[2 * i] = v;
            buf[2 * i + 1] = (int16_t)-v;
        }
        if (w.hal_replay) {
            piano_audio_copy_from_ring(buf, hal_ring, tp->ringFrames, w.hal_read, period, 2);
            piano_audio_zero_ring(hal_ring, tp->ringFrames, w.hal_read, period, 2);
            w.hal_read += period;
        }
        if (w.sound_on && w.amplitude && !w.first_sound_offered_us) w.first_sound_offered_us = w.now_us;
        int wake = 0;
        CHECK(audio_live_play(&sh, buf, period, now_ms(), &wake) == period);
        if (wake) {
            w.wake = 1;
            w.wakes++;
            if (getenv("LIVE_WAKES")) fprintf(stderr, "wake at %u ms (speaker %s)\n", now_ms() - g_t0, audio_live_state(live.spk_state));
        }
        w.next_out_us += out_period;
    }
    if (w.in_running && w.now_us >= (uint64_t)w.next_in_us) {
        int16_t mono[240];
        memset(mono, 0x55, sizeof mono);
        const uint32_t n = audio_live_capture(&sh, mono, 240);
        CHECK(n <= 240);
        for (uint32_t i = 0; i < n; ++i) {
            if (mono[i] == 6000 || mono[i] == -6000) w.mic_ok++;
            else if (mono[i]) w.mic_bad++;
            if (abs(mono[i]) > w.mic_peak) w.mic_peak = (int16_t)abs(mono[i]);
        }
        w.mic_frames += n;
        w.next_in_us += 5000.0;
    }
}
static void device(int out, int in)
{
    if (out && !w.out_running) {
        w.next_out_us = (double)w.now_us;
        if (w.hal_replay) {
            memset(hal_ring, 0, sizeof hal_ring);
            w.hal_origin_us = (double)w.now_us;
            w.hal_read = w.hal_next = 0;
            w.next_out_us += piano_audio_timing_profile(w.hal_mode)->periodFrames * 1000000.0 / 48000.0;
        }
    }
    if (in && !w.in_running) w.next_in_us = (double)w.now_us;
    w.out_running = out;
    w.in_running = in;
    audio_live_running(&sh, out, in);
    w.wake = 1;
}

// ---------------------------------------------------------------------------------------------------- the runs
struct Scenario {
    uint32_t level;
    unsigned gain;
    unsigned ceiling_db;              // 0: the level's default
    uint32_t idle_ms, end_ms;
    int ports, strict;
    void (*hook)(uint32_t t);         // ms since the live paths started
};

static void deliver(int to_live)
{
    for (unsigned i = 0; i < w.nq;) {
        /* Capture completions are drained by a polling host, not immediately
         * at the DSP timestamp. Change poll phase after startup; equal sample
         * clocks must not turn this into a permanent 100-frames/s drop. */
        if (to_live && g_mic_clock && w.q[i].read_slot >= 0) {
            const unsigned poll = now_ms() - g_t0 < 1500 ? 5000 : 25000;
            if (now_ms() % (poll / 1000u)) {++i; continue;}
        }
        if (w.q[i].due_us <= w.now_us) {
            struct Answer a = w.q[i];
            memmove(&w.q[i], &w.q[i + 1], (w.nq - i - 1) * sizeof w.q[0]);
            w.nq--;
            if (a.plan) w.outstanding = 0;
            if (a.n >= 24 && le32(a.b + 20) == AR_OP_WRITE_DONE) {w.write_dones++; if (w.str_started) w.stream_dones++;}
            if (a.read_slot <= -2) play_slot((unsigned)(-2 - a.read_slot));
            if (a.read_slot >= 0) {fill_capture((unsigned)a.read_slot, le32(a.b + 40), 0); w.read_dones++;}
            if (to_live) audio_live_receive(&live, a.b, a.n, now_ms());
            else audio_session_receive(&session, a.b, a.n, now_ms());
            continue;
        }
        ++i;
    }
}

static void (*g_setup)(AudioLiveConfig *);
static int pm_expect_unsafe;

static void run(const struct Scenario *sc)
{
    memset(&w, 0, sizeof w);
    const uint8_t addresses[4] = AUDIO_AMP_ADDRESSES;
    for (int i = 0; i < 4; ++i) {w.amp[i].address = addresses[i]; w.amp[i].present = 1; amp_defaults(&w.amp[i]);}
    w.write_error_at = -1;
    w.drop_writes_from = w.drop_writes_until = -1;
    w.gain_cap = AUDIO_AMPS_TEST_GAIN;   // the session's levels 6 never power up anyway
    w.strict = sc->strict;
    w.o = (AdgObservation){1, 1, 0, 0, 3, ADG_CHARGER_UP, 1, 0, "", 1};
    adg_init(&guard, 0);
    CHECK(adg_observe(&guard, &w.o, 0) == AdgRun);
    // ---- levels 4-6 with keep: the session hands the hardware over
    AudioSessionConfig c;
    audio_session_defaults(&c);
    c.level = 6; c.dsp = 1; c.ports = sc->ports; c.preset_any = 1; c.mic = 1; c.silence_ms = 300; c.keep = 1;
    AudioSessionOps ops = {0};
    ops.log = log_line; ops.send = dsp_send; ops.lpass_read = lpass_read;
    ops.amps = (AudioAmpsIo){0, &bus, reset_line, 0};
    ops.preset = preset; ops.preset_bytes = preset_bytes;
    ops.tdm_pins = tdm_pins; ops.smmu = smmu;
    uint8_t *dma = g_dma;
    memset(g_dma, 0, sizeof g_dma);
    ops.dma = dma; ops.dma_phys = DMA_PHYS; ops.dma_bytes = sizeof g_dma; ops.publish = publish;
    ops.va_mic = va_mic;
    if (g_spk_files) {
        ops.spk_params = g_spk_file; ops.spk_params_bytes = g_spk_bytes;
        ops.spk_re25 = g_spk_re25; ops.spk_re25_bytes = g_re25_bytes;
    } else {
        ops.spk_why = "not staged (test)";
    }
    ops.battery_uv = battery_uv;
    audio_session_init(&session, &c, &ops, &guard);
    uint64_t next = 0;
    while (!session.finished && w.now_us < 60000000ull) {
        deliver(0);
        if (w.now_us >= next) {
            const uint32_t d = audio_session_tick(&session, &w.o, now_ms());
            next = w.now_us + (d ? d : 1) * 1000u;
        }
        w.now_us += 1000;
    }
    CHECK(session.finished && session.kept && !w.violations);
    CHECK(session.result[4] == AudioLevelPassed && session.result[5] == AudioLevelPassed && session.result[6] == AudioLevelPassed);
    for (int i = 0; i < 4; ++i) CHECK(w.amp[i].released && (w.amp[i].reg[FS19XX_REG_PWRCTRL] & FS19XX_PWRCTRL_PWDN));
    CHECK(w.pins_on && !w.clock_on && !w.va_votes && !w.va_mclk && !w.map_spk && !w.map_mic && w.nq == 0);
    // ---- levels 8-10
    w.gain_cap = sc->level >= 9 ? FS19XX_GAIN_CAP : AUDIO_AMPS_TEST_GAIN;
    w.engine_ppm = g_ppm;
    w.queue_full_every = g_queue_full;
    w.fail_match = g_fail; w.drop = g_drop; w.fail_status = 1; w.fail_after = w.ncmd;
    AudioLiveConfig lc;
    audio_live_defaults(&lc, sc->level);
    if (sc->gain) lc.gain = sc->gain;
    if (sc->ceiling_db) lc.ceiling = audio_ceiling_db(sc->ceiling_db);
    if (sc->idle_ms) lc.idle_ms = sc->idle_ms;
    static AudioLiveSpk spk_state;
    memset(&spk_state, 0, sizeof spk_state);
    lc.spk_prot = g_spk;
    lc.spk_state = &spk_state;
    if (g_setup) g_setup(&lc);
    audio_live_shared_init(&sh, g_out_ring, g_in_ring, AUDIO_Q15_ONE);
    CHECK(audio_live_init(&live, &lc, &session, &sh) == 0);
    memset(&live_ledger,0,sizeof live_ledger); live.ledger=&live_ledger;
    w.write_dones = w.stream_dones = 0;
    w.writes = 0;
    g_t0 = now_ms();
    next = 0;
    const uint64_t end = w.now_us + (uint64_t)sc->end_ms * 1000u;
    while (!live.finished && w.now_us < end) {
        deliver(1);
        engine();
        if (sc->hook) sc->hook(now_ms() - g_t0);
        if (getenv("LIVE_FILL") && w.now_us % 2000000u == 0)
            fprintf(stderr, "t %u ms: fill %u avg %d drops %u repeats %u played %llu\n", now_ms() - g_t0, audio_ring_fill(&sh.out),
                    live.spk_drift.avg / 64, live.drops, live.repeats, (unsigned long long)w.played_periods);
        if (w.now_us >= next || w.wake) {
            w.wake = 0;
            const unsigned reads = g_loaded_reads;
            const uint32_t d = audio_live_tick(&live, &w.o, now_ms());
            if (g_loaded_reads - reads > w.max_monitor_reads) w.max_monitor_reads = g_loaded_reads - reads;
            next = w.now_us + (d ? d : 1) * 1000u;
        }
        w.now_us += 500;
    }
    if (pm_expect_unsafe==2) {CHECK(live.sleep_safe && !any_powered() && w.map_spk==1); return;}
    if (pm_expect_unsafe) {
        CHECK(live.sleep_failed && audio_live_parked(&live)==-1);
        CHECK(audio_live_rearm(&live)==-1 && w.map_spk==1 && !live.finished);
        CHECK(live.splan.in_flight && live.splan.seq.state==ArSeqWaiting);
        CHECK(!w.violations && !any_powered());
        return; // reset the host world between tests; production retains this backing
    }
    // ---- the owner's stop, then the end state
    if (!live.finished) {
        w.strict = 0;   // the owner's stop powers the amplifiers down at once, whatever plays
        audio_live_abort(&live, 0, now_ms());
        const uint64_t stop = w.now_us + 60000000ull;
        while (!live.finished && w.now_us < stop) {
            deliver(1);
            if (w.now_us >= next) {
                const uint32_t d = audio_live_tick(&live, &w.o, now_ms());
                next = w.now_us + (d ? d : 1) * 1000u;
            }
            w.now_us += 500;
        }
    }
    CHECK(live.finished);
    for (int i = 0; i < 4; ++i) CHECK(!w.amp[i].released);
    CHECK(!w.pins_on && !w.va_on);
    for (unsigned i = 0; i < sizeof g_dma; ++i) if (g_dma[i]) {CHECK(!"the buffer is not zeroed at the end"); break;}
    if (!w.off) CHECK(!w.clock_on && !w.va_votes && !w.va_mclk && !w.map_spk && !w.map_mic && !w.dev_started && !w.str_started);
    if (!w.off) CHECK(!w.vi_open && !w.vi_started && !w.prot_dev_open);
    g_setup = 0;
}

// The live paths' own commands (after the session's).
static unsigned live_from(void)
{
    for (unsigned i = 0; i < w.ncmd; ++i) if (!strcmp(w.cmd[i], "CLOSE 0x4008")) return i + 4;   // level 6's mic plan ends: mclk, votes
    return 0;
}
static const char *kSpkUp[] = {"PRM REQ 0x202 6144000", "OPEN 0x4005", "CFG 0x6050", "CFG 0x6051", "CFG 0x6052", "OPEN 0x4001", "CFG 0x6001",
                               "CFG 0x6002", "CFG 0x6004", "MAP 0x19da00000", "PREPARE 0x4005", "PREPARE 0x4001", "START 0x4005", "START 0x4001"};
static const char *kSpkDown[] = {"STOP 0x4001", "STOP 0x4005", "UNMAP 0x55", "CLOSE 0x4001", "CLOSE 0x4005", "PRM REL 0x202"};
static const char *kMicUp[] = {"PRM VOTE 1", "PRM VOTE 2", "PRM REQ 0x30c 19200000", "OPEN 0x4008", "CFG 0x6080", "CFG 0x6081", "OPEN 0x4003",
                               "CFG 0x6021", "CFG 0x6020", "MAP 0x19da10000", "PREPARE 0x4008", "PREPARE 0x4003", "VA ON", "START 0x4008",
                               "START 0x4003"};
static const char *kMicDown[] = {"STOP 0x4003", "STOP 0x4008", "VA OFF", "UNMAP 0x77", "CLOSE 0x4003", "CLOSE 0x4008", "PRM REL 0x30c",
                                 "PRM UNVOTE 2", "PRM UNVOTE 1"};
#define N(a) (sizeof a / sizeof a[0])
static unsigned expect(unsigned at, const char *const *want, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        if (at + i >= w.ncmd || strcmp(w.cmd[at + i], want[i])) {
            fprintf(stderr, "command %u: got \"%s\", want \"%s\"\n", at + i, at + i < w.ncmd ? w.cmd[at + i] : "(none)", want[i]);
            CHECK(0);
        }
    }
    return at + n;
}
static unsigned power_ups_after_session(void) {return w.power_ups;}   // the session's levels power nothing up

// ------------------------------------------------------------------------------------------------------------- the tests
// A device that runs from 500 ms, sound (full scale) from 1000 to 3000 ms, stopped at 9000.
static uint32_t replay_mode, replay_io;
static uint32_t replay_begin_periods, replay_begin_sound;
static void hal_burst(uint32_t t)
{
    if (t == 500) {
        w.hal_replay = 1; w.hal_mode = replay_mode; w.hal_io = replay_io;
        device(1, 0);
    }
    w.amplitude = 16000;
    w.sound_on = t >= 500 && t < 5000;
    if (t == 1500) { replay_begin_periods = (uint32_t)w.played_periods; replay_begin_sound = (uint32_t)w.sound_periods; }
    if (t == 4500) {
        w.steady_periods = (uint32_t)w.played_periods - replay_begin_periods;
        w.steady_sound = (uint32_t)w.sound_periods - replay_begin_sound;
    }
    if (t == 5500 && w.out_running) device(0, 0);
}
static void hal_transport_replay(void)
{
    const uint32_t callbacks[] = {64, 128, 256, 512};
    for (replay_mode = 0; replay_mode < 2; ++replay_mode) {
        for (unsigned i = 0; i < sizeof callbacks / sizeof callbacks[0]; ++i) {
            replay_io = callbacks[i];
            const struct Scenario sc = {9, 192, 1, 0, 9500, AudioPortsUbuntu, 1, hal_burst};
            run(&sc);
            CHECK(!w.violations && w.steady_periods >= 298 && w.steady_periods <= 302);
            CHECK(w.steady_sound == w.steady_periods);
            CHECK(live.drops == 0 && live.repeats == 0);
            printf("HAL -> AudioLive -> DSP replay: mode=%u callback=%u steady sound=%u/%u drops=%u repeats=%u\n",
                   replay_mode, replay_io, w.steady_sound, w.steady_periods, live.drops, live.repeats);
        }
    }
}

static void burst(uint32_t t)
{
    if (t == 500 && !w.out_running) device(1, 0);
    w.amplitude = 32767;
    w.sound_on = t >= 1000 && t < 3000;
    if (t == 9000 && w.out_running) device(0, 0);
}
static void testLevel8(int ports)
{
    const struct Scenario sc = {8, 0, 0, 0, 12000, ports, 1, burst};
    run(&sc);
    CHECK(!w.violations);
    unsigned at = expect(live_from(), kSpkUp, N(kSpkUp));
    at = expect(at, kSpkDown, N(kSpkDown));
    CHECK(at == w.ncmd && live.spk_starts == 1 && power_ups_after_session() == 4 && w.unmutes == 4);
    // up within 300 ms of the first sound; about 2 s of sound played at the level 8 ceiling (full scale lands on it)
    CHECK(w.first_sound_played_us > w.first_sound_offered_us && w.first_sound_played_us - w.first_sound_offered_us < 300000);
    CHECK(w.sound_periods > 170 && w.sound_periods < 205 && w.peak <= AUDIO_CEILING_TEST && w.peak > AUDIO_CEILING_TEST * 97 / 100);
    CHECK(live.underruns <= 1 && live.skipped <= 1 && live.drops == 0 && live.repeats == 0);   // equal clocks: nothing slips
    // down 3 s after the sound ended (no sound for idle_ms), the amplifiers powered down then, not held (the path comes back)
    CHECK(logged("live: speaker: no sound for 3000 ms") && logged("the amplifiers unmuted, the sound fades in"));
    CHECK(logged("live: level 8: the Speaker on the DSP: playing gain 160"));
}

// Two bursts 5 s apart (two starts) and two 1 s apart (one: the path stays up through the gap).
static void bursts(uint32_t t)
{
    if (t == 200 && !w.out_running) device(1, 0);
    w.amplitude = 16000;
    w.sound_on = (t >= 500 && t < 1500) || (t >= 7000 && t < 8000) || (t >= 9000 && t < 9500);
}
static void testRestart(void)
{
    const struct Scenario sc = {8, 0, 0, 0, 16000, AudioPortsUbuntu, 1, bursts};
    run(&sc);
    CHECK(!w.violations && live.spk_starts == 2 && count_cmd("START 0x4001", live_from()) == 2 && count_cmd("STOP 0x4001", live_from()) == 2);
    CHECK(w.power_ups == 8 && w.unmutes == 8);
}

// The device stops while sound plays: a fade, the amplifiers muted after it played, the hold; the device back within the hold:
// unmuted again on the same plan.
static void stop_and_back(uint32_t t)
{
    if (t == 100 && !w.out_running) device(1, 0);
    if (t == 2000 && w.out_running) device(0, 0);
    if (t == 3500 && !w.out_running) device(1, 0);
    if (t == 5000 && w.out_running) device(0, 0);
    w.amplitude = 20000;
    w.sound_on = 1;
}
static void testDeviceStopHold(void)
{
    const struct Scenario sc = {8, 0, 0, 0, 12000, AudioPortsUbuntu, 1, stop_and_back};
    run(&sc);
    CHECK(!w.violations && live.spk_starts == 1 && w.power_ups == 4 && w.unmutes == 8);
    CHECK(logged("the device stopped: faded to silence, the amplifiers muted"));
    // the fade: the periods' peaks fall over 20 ms, not at once
    unsigned falls = 0;
    for (unsigned i = 1; i < w.nperiods; ++i) if (w.period_peak[i - 1] > 1000 && w.period_peak[i] > 0 && w.period_peak[i] < w.period_peak[i - 1] / 2) falls++;
    CHECK(falls >= 1);
}

// A device that plays only silence brings nothing up.
static void silent(uint32_t t)
{
    if (t == 100 && !w.out_running) device(1, 1);
    w.sound_on = 0;
}
static void testSilentDevice(void)
{
    const struct Scenario sc = {8, 0, 0, 0, 20000, AudioPortsUbuntu, 1, silent};
    run(&sc);
    CHECK(!w.violations && live.spk_starts == 0 && w.ncmd == live_from() && w.writes == 0 && w.power_ups == 0);
}

// Level 9: gain 192, the ceiling -1 dBFS; the master volume halved and back, then muted and unmuted.
static void volume(uint32_t t)
{
    if (t == 100 && !w.out_running) device(1, 0);
    w.amplitude = 32767;
    w.sound_on = 1;
    if (t == 2000) {audio_live_control(&sh, 50, -1); w.wake = 1;}
    if (t == 3000) {audio_live_control(&sh, 100, -1); w.wake = 1;}
    if (t == 4000) {audio_live_control(&sh, -1, 1); w.wake = 1;}
    if (t == 7600 && g_mark == 0) {g_mark = 1; g_wakes = w.wakes;}            // the path is down (muted at 4 s, 3 s of hold)
    if (t == 8990 && g_mark == 1) {g_mark = 2; g_wakes = w.wakes - g_wakes;}  // (the hook runs twice a millisecond)
    if (t == 9000) {audio_live_control(&sh, -1, 0); w.wake = 1;}
}
static void testLevel9(void)
{
    g_mark = 0;
    const struct Scenario sc = {9, 192, 1, 0, 12000, AudioPortsUbuntu, 1, volume};
    run(&sc);
    CHECK(!w.violations && live.cfg.ceiling == AUDIO_CEILING_MAX && w.peak <= AUDIO_CEILING_MAX && w.peak > AUDIO_CEILING_MAX * 97 / 100);
    CHECK(logged("gain 192 (-23.6 dB)") && logged("muted: faded to silence"));
    CHECK(live.spk_starts == 2);   // muted at 4 s: down 3 s later; unmuted at 9 s: up again
    if (g_wakes > 1) fprintf(stderr, "wakes while muted and down: %u\n", g_wakes);
    CHECK(g_wakes <= 1);           // sound while muted and down wakes the live thread at most once, not every period
    // the ramp (1.9-3.9 s: the two volume changes, no fade): from one period to the next the level moves by at most the step
    unsigned big = 0;
    for (unsigned i = 1; i < w.nperiods; ++i) {
        if (w.period_at[i] < 1900 || w.period_at[i] > 3900) continue;
        const int32_t a = w.period_peak[i - 1], b = w.period_peak[i];
        if (a > 1000 && b > 1000 && abs(a - b) > (int32_t)((int64_t)AUDIO_CEILING_MAX * AUDIO_VOLUME_STEP / AUDIO_Q15_ONE) + 200) big++;
    }
    CHECK(big == 0);
    // half slider volume follows the shared -36 dB squared curve: about 35.5%
    // of the ceiling (the old -25 dB expectation belonged to the rejected
    // -50 dB/half-dB curve).
    int seen = 0;
    for (unsigned i = 0; i < w.nperiods; ++i) if (w.period_peak[i] > AUDIO_CEILING_MAX * 32 / 100 && w.period_peak[i] < AUDIO_CEILING_MAX * 38 / 100) seen = 1;
    CHECK(seen);
}

// Level 8 never takes level 9's gain or ceiling, whatever is asked.
static void testLevel8Caps(void)
{
    const struct Scenario sc = {8, 231, 1, 0, 5000, AudioPortsUbuntu, 1, burst};
    run(&sc);
    CHECK(!w.violations && live.cfg.gain == AUDIO_AMPS_TEST_GAIN && live.cfg.ceiling == AUDIO_CEILING_TEST && w.peak <= AUDIO_CEILING_TEST);
}

static void steady(uint32_t t);
static void gain_switch(uint32_t t)
{
    steady(t);
    if (t == 2000 && !g_mark) {
        g_mark = 1;
        CHECK(live.unmuted && audio_live_gain_select(&sh, 192) == 0);
        CHECK(audio_live_gain_select(&sh, 232) == -1 && audio_live_gain_select(&sh, 255) == -1 &&
              audio_live_gain_select(&sh, UINT32_MAX) == -1 && sh.amp_gain_requested == 192);
    }
    if (t == 3000 && g_mark == 1) {
        g_mark = 2;
        CHECK(live.cfg.gain == 192 && !live.gain_changing && live.unmuted);
        CHECK(audio_live_gain_select(&sh, 231) == 0);
    }
}

static void testGainSwitch(void)
{
    g_mark = 0;
    const struct Scenario sc = {9, 0, 1, 0, 7000, AudioPortsUbuntu, 1, gain_switch};
    run(&sc);
    CHECK(g_mark == 2 && !w.violations && live.cfg.gain == 231 && !live.gain_changing);
    CHECK(live.spk_starts == 1 && live.underruns == 0 && live.amp_bad == 0);
    CHECK(logged("applied gain 192 cap 231") && logged("applied gain 231 cap 231"));
    CHECK(logged("requested 192 gain 192 VOLCTRL 0xc0c0") && logged("requested 231 gain 231 VOLCTRL 0xe7c0"));
    CHECK(live.gain_reports > 0 && live.gain_reports <= 8);
}

// The clocks: the device 200 ppm fast, then slow, for 60 s of sound.
static void steady(uint32_t t)
{
    if (t == 100 && !w.out_running) device(1, 0);
    w.amplitude = 12000;
    w.sound_on = 1;
}
static void testDrift(double ppm)
{
    const struct Scenario sc = {8, 0, 0, 0, 62000, AudioPortsUbuntu, 1, steady};
    g_ppm = ppm;
    run(&sc);
    g_ppm = 0;
    CHECK(!w.violations && live.spk_starts == 1);
    // the drift less the band the average may wander before the first slip
    const double expect_slips = fabs(ppm) * 1e-6 * 48000.0 * 60.0 - AUDIO_LIVE_SLIP_BAND;
    const uint32_t slips = ppm > 0 ? live.drops : live.repeats;
    if (getenv("LIVE_DRIFT"))
        fprintf(stderr, "drift %+.0f ppm: offered %llu taken %llu dropped %llu, played %llu periods, drops %u repeats %u underruns %u skipped %u\n",
                ppm, (unsigned long long)sh.offered, (unsigned long long)sh.taken, (unsigned long long)sh.dropped,
                (unsigned long long)w.played_periods, live.drops, live.repeats, live.underruns, live.skipped);
    CHECK(slips > expect_slips * 0.6 && slips < expect_slips * 1.4 + 20 && (ppm > 0 ? live.repeats : live.drops) == 0);
    CHECK(live.underruns <= 1 && live.skipped <= 1);
}

// ---- faults
static int g_kind;
static uint32_t g_at;
static void fault(uint32_t t)
{
    steady(t);
    if (t == 1500 && g_kind == 1) w.write_error_at = (int)w.writes + 3;                        // a refused period
    if (t == 1500 && g_kind == 2) w.amp[1].inject = 0x0100;                                     // OTPDS on one amplifier
    if (t == 1500 && g_kind == 3) {w.drop_writes_from = (int)w.writes + 1; w.drop_writes_until = w.drop_writes_from + 4;}   // late
    if (t == 1500 && g_kind == 4) w.o.quiet = 1;
    if (t == 2500 && g_kind == 4) w.o.quiet = 0;
    if (t == 1500 && g_kind == 5) {w.o.fatal = 1; w.o.fatal_reports = 1; w.off = 1;}
    if (g_kind == 6 && (t == 1500 || t == 4000 || t == 6500)) {w.drop_writes_from = (int)w.writes + 1; w.drop_writes_until = w.drop_writes_from + 4;}
    (void)g_at;
}
static void testFaults(void)
{
    // a refused period: the amplifiers down at once, the plan undone, no restart in this boot
    g_kind = 1;
    struct Scenario sc = {8, 0, 0, 0, 8000, AudioPortsUbuntu, 0, fault};
    run(&sc);
    CHECK(!w.violations && live.spk_state == LiveOff && live.spk_starts == 1 && logged("WRITE_DONE refused") && logged("OFF for this boot"));
    CHECK(!any_powered());
    // an amplifier whose status changes while playing
    g_kind = 2;
    run(&sc);
    CHECK(!w.violations && live.spk_state == LiveOff && logged("flagged by the group check") && logged("stopped by amplifier status"));
    // late periods: down, back with the sound
    g_kind = 3;
    run(&sc);
    CHECK(!w.violations && live.spk_starts == 2 && live.late_stops == 1 && logged("STOPPED: late period"));
    // quiet: down, no new command while it lasts, back after
    g_kind = 4;
    run(&sc);
    CHECK(!w.violations && live.spk_starts == 2 && logged("STOPPED: PianoPower asked for quiet") && logged("quiet over"));
    // a fatal report: no command any more, everything held in reset, finished
    g_kind = 5;
    run(&sc);
    CHECK(!w.violations && w.sends_after_off == 0 && logged("guard OFF"));
    // three late stops: off
    g_kind = 6;
    sc.end_ms = 12000;
    run(&sc);
    CHECK(!w.violations && live.late_stops == 3 && live.spk_state == LiveOff && logged("3 late stops"));
    g_kind = 0;
}

// A plan step refused or unanswered: the speaker path off, what was done undone, nothing powered up unless the plan was up.
static void testPlanFailures(void)
{
    for (unsigned k = 0; k < N(kSpkUp); ++k)
        for (int drop = 0; drop < 2; ++drop) {
            g_kind = 0;
            const struct Scenario sc = {8, 0, 0, 0, 6000, AudioPortsUbuntu, 0, steady};
            g_fail = kSpkUp[k];
            g_drop = drop;
            run(&sc);
            CHECK(!w.violations && live.spk_state == LiveOff && w.power_ups == 0);
            CHECK(logged("the speaker path is OFF for this boot"));
        }
    g_fail = 0;
    g_drop = 0;
}

// ---- level 10: the Mic, alone and with the speaker
static void mic_only(uint32_t t)
{
    if (t == 100 && !w.in_running) device(0, 1);
    if (t == 3000 && w.in_running) device(0, 0);
}
static void both(uint32_t t)
{
    if (t == 100) device(1, 1);
    w.amplitude = 10000;
    w.sound_on = t >= 100 && t < 4000;
    if (t == 4000) device(0, 0);
}
static void testMic(void)
{
    struct Scenario sc = {10, 0, 0, 0, 10000, AudioPortsUbuntu, 1, mic_only};
    run(&sc);
    CHECK(!w.violations && live.mic_starts == 1 && live.spk_starts == 0);
    unsigned at = expect(live_from(), kMicUp, N(kMicUp));
    at = expect(at, kMicDown, N(kMicDown));
    CHECK(at == w.ncmd && w.va_calls == 2);
    // 2.9 s of capture after the first 100 ms: the mono mix of the two channels, nothing else
    CHECK(w.mic_ok > 48000u * 25 / 10 && w.mic_bad == 0 && w.mic_peak == 6000);
    // both at once: one command at a time over both plans (the model flags two), each path's commands complete
    sc = (struct Scenario){10, 0, 0, 0, 10000, AudioPortsUbuntu, 1, both};
    run(&sc);
    CHECK(!w.violations && live.mic_starts == 1 && live.spk_starts == 1 && w.sound_periods > 300 && w.mic_ok > 48000u * 3);
    CHECK(count_cmd("VA ON", 0) == 1 && count_cmd("VA OFF", 0) == 1 && count_cmd("START 0x4003", 0) == 1 && count_cmd("STOP 0x4003", 0) == 1);
    // sheng's and SunUEFI's ports
    sc.ports = AudioPortsSheng;
    run(&sc);
    CHECK(!w.violations);
    sc.ports = AudioPortsSun;
    run(&sc);
    CHECK(!w.violations);
    // level 9 leaves the Mic silent (capture() gives nothing), no capture plan
    sc = (struct Scenario){9, 0, 0, 0, 5000, AudioPortsUbuntu, 1, mic_only};
    run(&sc);
    CHECK(!w.violations && live.mic_starts == 0 && w.mic_frames == 0);
}

static AudioMicWindow route_window;
static unsigned route_applies;

static void route_apply(int route, uint32_t generation)
{
    CHECK(!w.va_on && !w.va_votes && !w.va_mclk && !w.map_mic && !w.in_running);
    CHECK(generation == ++route_applies);
    CHECK(route == (generation == 1 ? 4 : -1));
}

static void route_config(AudioLiveConfig *c)
{
    c->mic_route = 4;
    c->mic_route_apply = route_apply;
}

static void mic_route_controls(uint32_t t)
{
    const AudioMicRouteStatus s = audio_live_mic_route_status(&sh);
    if (t == 100 && !w.out_running) {
        CHECK(s.requested == 4 && s.applied == 4 && s.generation == 1 && s.stopped && !s.diag_applied);
        CHECK(audio_live_mic_request(&sh, 0, 3) == EINVAL && audio_live_mic_request(&sh, 1, -1) == EINVAL);
        CHECK(audio_live_mic_record(&live, 1, &route_window) == EPERM && !audio_live_mic_window(&live));
        w.sound_on = 1;
        w.amplitude = 10000;
        device(1, 1);
        /* HAL started, even before the owner sends its first graph command. */
        CHECK(audio_live_mic_request(&sh, 0, -1) == EBUSY);
    }
    if (t == 500 && w.in_running) {
        CHECK(!s.stopped && live.mic_state == LiveRunning && w.va_on);
        CHECK(audio_live_mic_request(&sh, 0, -1) == EBUSY && audio_live_mic_request(&sh, 1, 1) == EBUSY);
        CHECK(audio_live_mic_record(&live, 1, &route_window) == EBUSY);
        CHECK(live.mic_pcm[2].clipped > 0 && live.mic_pcm[2].clipped == live.mic_pcm[0].clipped);
        CHECK(!live.mic_pcm[1].clipped && sh.input_hal.pcm.clipped > 0);
        CHECK(live.mic_settle && !audio_live_mic_window(&live));  /* opt-in even with an active recording */
        device(1, 0);
        CHECK(audio_live_mic_request(&sh, 0, -1) == EBUSY);
    }
    if (t == 4000 && s.stopped && s.requested == 4) {
        CHECK(live.mic_state == LiveDown && !w.va_on && !w.va_mclk && !w.va_votes && !w.map_mic);
        CHECK(live.spk_state == LiveRunning && w.sound_periods > 100);
        CHECK(audio_live_mic_request(&sh, 0, -1) == 0 && audio_live_mic_request(&sh, 1, 1) == 0);
        const AudioMicRouteStatus queued = audio_live_mic_route_status(&sh);
        CHECK(queued.requested == -1 && queued.applied == 4 && queued.diag_requested && !queued.diag_applied);
        CHECK(queued.generation == 1 && audio_live_mic_record(&live, 1, &route_window) == EBUSY);
        w.wake = 1;
    }
    if (t == 4100 && !w.in_running) {
        CHECK(s.stopped && s.generation == 2 && s.applied == -1 && s.diag_applied);
        CHECK(audio_live_mic_record(&live, 1, &route_window) == 0 && !audio_live_mic_window(&live));
        CHECK(audio_live_mic_route_status(&sh).armed);
        device(1, 1);
    }
    if (t == 12500 && w.in_running) {
        CHECK(live.mic_state == LiveRunning && live.spk_state == LiveRunning && w.va_on);
        CHECK(live.mic_pcm[2].peak == 18384 && !live.mic_pcm[2].clipped);
        CHECK(live.mic_pcm[0].clipped && !sh.input_hal.pcm.clipped);
        const AudioMicWindow *raw = audio_live_mic_window(&live);
        CHECK(raw == &route_window && raw->magic == AUDIO_MIC_DIAG_MAGIC);
        CHECK(raw->generation == 2 && raw->run == 2 && raw->route == -1);
        CHECK(raw->periods <= AUDIO_MIC_DIAG_PERIODS && raw->frames <= AUDIO_MIC_DIAG_FRAMES);
        CHECK(raw->frames > 7 * 48000u && raw->end_ms - raw->start_ms <= AUDIO_MIC_DIAG_MS + 10);
        CHECK(raw->period[0].arrival_ms - raw->start_ms >= AUDIO_LIVE_MIC_SETTLE_MS);
        CHECK(raw->pcm[1] == 4000 || raw->pcm[1] == -4000);
        CHECK(raw->pcm[0] == INT16_MAX || raw->pcm[0] == INT16_MIN);
        device(0, 0);
    }
    if (t == 16000 && s.recorded) {
        CHECK(s.stopped && audio_live_mic_record(&live, 0, &route_window) == 0);
        CHECK(!audio_live_mic_window(&live));
    }
}

static void testMicRoutes(void)
{
    const struct Scenario sc = {10, 160, 20, 0, 16500, AudioPortsUbuntu, 1, mic_route_controls};
    g_route_fixture = 1;
    g_setup = route_config;
    route_applies = 0;
    run(&sc);
    g_route_fixture = 0;
    CHECK(live.mic_starts == 2 && live.spk_starts == 1 && w.va_calls == 4 && !w.violations);
    CHECK(route_applies == 2 && logged("mic release: run 2 down 1 va 0"));
}

static void mic_route_start_race(uint32_t t)
{
    if (t == 100 && !w.in_running) {
        CHECK(audio_live_mic_request(&sh, 0, 4) == 0);
        device(0, 1);   // StartIO wins before the worker observes the request
    }
    if (t == 500 && w.in_running) {
        const AudioMicRouteStatus s = audio_live_mic_route_status(&sh);
        CHECK(s.requested == 4 && s.applied == -1 && s.generation == 1 && !s.stopped);
        CHECK(w.va_on && live.mic_state == LiveRunning && w.mic_peak == 6000);
        device(0, 0);
    }
    if (t == 4000) w.wake = 1;
    if (t == 4100) {
        const AudioMicRouteStatus s = audio_live_mic_route_status(&sh);
        if (g_va_fail_off) {
            CHECK(!s.stopped && s.applied == -1 && s.generation == 1 && live.mic_state == LiveOff);
            CHECK(audio_live_mic_request(&sh, 0, -1) == EBUSY && live.mplan.seq.undo_errors == 1);
        } else {
            CHECK(s.stopped && s.applied == 4 && s.generation == 2 && live.mic_state == LiveDown);
        }
    }
}

static void testMicRoutePendingAndFailedRelease(void)
{
    const struct Scenario sc = {10, 160, 20, 0, 4500, AudioPortsUbuntu, 1, mic_route_start_race};
    for (g_va_fail_off = 0; g_va_fail_off < 2; ++g_va_fail_off) {
        run(&sc);
        CHECK(!w.violations && w.va_calls == 2 && logged("mic-route1: start request 4 applied -1 gen 1 run 1"));
    }
    g_va_fail_off = 0;
}

// A full queue every third send: everything still in order, nothing lost.
static void testQueueFull(void)
{
    g_queue_full = 3;
    const struct Scenario sc = {10, 0, 0, 0, 8000, AudioPortsUbuntu, 1, both};
    run(&sc);
    g_queue_full = 0;
    CHECK(!w.violations && live.queue_full > 0 && live.spk_starts == 1 && live.mic_starts == 1 && w.sound_periods > 300);
}

static void testLoadedMonitor(void)
{
    g_status_read_us = 4000;   /* explicit load, not an assumed hardware measurement */
    g_loaded_reads = 0;
    const struct Scenario sc = {9, 192, 1, 0, 6500, AudioPortsUbuntu, 1, burst};
    run(&sc);
    printf("loaded monitor: reads=%u polls=%u max-reads/tick=%u DSP-gaps=%u underruns=%u skips=%u\n",
           g_loaded_reads, live.status_polls, w.max_monitor_reads, w.queue_gaps, live.underruns, live.skipped);
    CHECK(!w.violations && live.status_polls >= 3 && w.max_monitor_reads == 1);
    CHECK(w.queue_gaps == 0 && live.underruns == 0);
    /* A deliberate backlog trim may now remove the larger startup lead when
     * this load catches up. It must retain this whole burst plus the reserve. */
    uint32_t skips = 0;
    for (uint64_t n = 0; n < live_ledger.written; ++n) {
        uint64_t e[AUDIO_LEDGER_WORDS];
        CHECK(audio_ledger_get(&live_ledger, n, e));
        if (e[20] == skips) continue;
        CHECK(e[0] == AlRefill && e[20] == ++skips && e[7] == e[8]);
        CHECK(e[6] >= AUDIO_LIVE_TARGET + (AUDIO_LIVE_DEPTH - e[15] - 1u) * AUDIO_PERIOD_FRAMES);
    }
    CHECK(skips == live.skipped);
    /* A partial group must not be published; bus errors still stop the path. */
    g_loaded_reads = 0;
    g_status_fail_at = 11;
    struct Scenario fault = sc;
    fault.strict = 0;   /* a safety fault mutes immediately, without waiting for the fade */
    run(&fault);
    CHECK(!w.violations && live.spk_state == LiveOff && live.status_poll.failed && live.status_polls == 0);
    CHECK(logged("a status read failed") && logged("the speaker path is OFF for this boot"));
    g_status_read_us = g_status_fail_at = g_loaded_reads = 0;
}

static unsigned pm_cycle, pm_stage, pm_command, pm_park_at;
static void sleep_cycles(uint32_t t)
{
    if (!w.out_running) device(1, 1);
    w.sound_on=1; w.amplitude=12000;
    w.o.quiet=((t>=1500 && t<1700) || (t>=4000 && t<4200) || (t>=6500 && t<6700)) ? 1 : 0;
    if (!pm_stage && t>=1500+pm_cycle*2500) {
        pm_command=w.ncmd; pm_stage=1;
        audio_live_control(&sh,60,0);
        audio_live_sleep(&live,pm_cycle+1);
        audio_live_sleep(&live,99); /* repeated off does not change generation */
        CHECK(live.sleep_generation==pm_cycle+1);
    }
    if (pm_stage==1 && live.parked) {
        CHECK(audio_live_parked(&live)==1);
        CHECK(!w.clock_on && !w.map_spk && !w.map_mic && !w.va_on && !w.va_votes && !w.va_mclk);
        CHECK(!any_powered() && !w.pins_on && !live.pump.in_flight && !live.cap.in_flight);
        CHECK(sh.want_out && sh.want_in && sh.volume_q15==audio_volume_q15(60) && !sh.muted);
        unsigned si=0,mi=0;
        for (unsigned j=pm_command;j<w.ncmd;++j) {
            if(si<N(kSpkDown) && !strcmp(w.cmd[j],kSpkDown[si])) ++si;
            if(mi<N(kMicDown) && !strcmp(w.cmd[j],kMicDown[mi])) ++mi;
        }
        CHECK(si==N(kSpkDown) && mi==N(kMicDown));
        pm_stage=2; pm_park_at=t;
    }
    if (pm_stage==2 && t-pm_park_at>=100) {
        const unsigned sends=w.send_calls;
        CHECK(audio_live_rearm(&live)==0);
        CHECK(audio_live_rearm(&live)==-1);
        CHECK(w.send_calls==sends && !audio_ring_fill(&sh.out) && !audio_ring_fill(&sh.in));
        CHECK(!sh.have_sound && !sh.sound && sh.volume_q15==audio_volume_q15(60));
        CHECK(!live.finished && !live.off);
        pm_stage=3;
    }
    if (pm_stage==3 && live.spk_state==LiveRunning && live.mic_state==LiveRunning) {
        ++pm_cycle; pm_stage=pm_cycle==3 ? 4 : 0;
    }
}
static void testSleepCycles(void)
{
    pm_cycle=pm_stage=0;
    const struct Scenario sc={10,160,6,0,10000,AudioPortsUbuntu,0,sleep_cycles};
    run(&sc);
    CHECK(pm_cycle==3 && !w.violations && !live.sleep_failed);
    uint64_t words[AUDIO_LEDGER_WORDS], n=live.ledger->written;
    CHECK(n>0);
    CHECK(audio_ledger_get(live.ledger,n-1,words));
    CHECK(!audio_ledger_get(live.ledger,n,words));
}
static int pm_safe_seen;
static void fail_active_sleep(uint32_t t)
{
    if (!w.out_running) device(1,0);
    w.sound_on=1; w.amplitude=10000;
    if (!pm_safe_seen && t>=1500 && live.unmuted) {
        const unsigned commands=w.ncmd; const int map=w.map_spk;
        audio_live_sleep_fail(&live);
        audio_live_sleep_fail(&live);
        CHECK(!sh.accepting && !sh.capturing && !live.unmuted && !any_powered());
        CHECK(w.ncmd==commands && w.map_spk==map && map==1);
        CHECK(audio_live_parked(&live)==-1 && audio_live_rearm(&live)==-1);
        pm_safe_seen=1;
        live.finished=1; // host-only fixture cleanup must not undo retained ownership
    }
}
static void testFailSafeMute(void)
{
    const struct Scenario sc={9,160,6,0,2500,AudioPortsUbuntu,0,fail_active_sleep};
    pm_safe_seen=0; pm_expect_unsafe=2; run(&sc); pm_expect_unsafe=0;
    CHECK(pm_safe_seen);
}

static int pm_map_seen;
static void sleep_pending_map(uint32_t t)
{
    (void)t;
    if (!w.out_running) device(1,0);
    w.sound_on=1; w.amplitude=10000;
    if (!pm_map_seen && live.splan.in_flight && live.splan.seq.pending_opcode==AR_OP_MEM_MAP) {
        pm_map_seen=1;
        CHECK(live.splan.seq.state==ArSeqWaiting && w.map_spk==1);
        audio_live_sleep(&live,7);
        CHECK(live.splan.seq.state==ArSeqWaiting); // pending MAP is NOT aborted
        if (pm_expect_unsafe) w.nq=0; // DSP mapped; the answer is lost
    }
    if (live.parked) {
        CHECK(pm_map_seen && live.splan.seq.handle==H_SPK && w.map_spk==0);
        CHECK(count_cmd("UNMAP 0x55",live_from())==1);
    }
}
static void testSleepPendingMap(void)
{
    const struct Scenario sc={9,160,6,0,5000,AudioPortsUbuntu,0,sleep_pending_map};
    pm_map_seen=0; run(&sc); CHECK(pm_map_seen && live.parked);
    pm_map_seen=0; pm_expect_unsafe=1; run(&sc);
    CHECK(pm_map_seen && !count_cmd("UNMAP 0x55",live_from()));
    pm_expect_unsafe=0;
}
static void testLedgerOverflow(void)
{
    AudioLedger l={0}; uint64_t a[AUDIO_LEDGER_WORDS]={0},b[AUDIO_LEDGER_WORDS];
    for(unsigned n=0;n<AUDIO_LEDGER_CAP+7;++n) {a[0]=n; audio_ledger_put(&l,a);}
    CHECK(l.written==AUDIO_LEDGER_CAP+7 && !audio_ledger_get(&l,0,b));
    CHECK(audio_ledger_get(&l,7,b) && b[0]==7);
    CHECK(audio_ledger_get(&l,l.written-1,b) && b[0]==l.written-1);
}

static void epoch_starts(uint32_t t)
{
    w.hal_replay=1; w.hal_mode=0; w.hal_io=512; w.amplitude=16000;
    if(t==100 || t==2200 || t==2250 || t==4510) device(1,0);
    if(t==2000 || t==2230 || t==4500 || t==6500) device(0,0);
    w.sound_on=(t>=100 && t<2000) || (t>=2250 && t<4500) || (t>=4630 && t<6500);
    if(t==2225 || t==4625) CHECK(!live.unmuted); /* old sound/silent PCM cannot release the new epoch */
}
static void testEpochStarts(void)
{
    const struct Scenario sc={9,192,1,15000,7000,AudioPortsUbuntu,1,epoch_starts};
    run(&sc);
    CHECK(!w.violations && sh.epoch==4);
    unsigned primed=0, tail=0;
    for(uint64_t n=0;n<live_ledger.written;++n) {
        uint64_t e[AUDIO_LEDGER_WORDS]; CHECK(audio_ledger_get(&live_ledger,n,e));
        if(e[0]==AlPrime) {
            CHECK(e[2]!=2 && e[5]>=2880 && e[6]>=2880);
            ++primed;
        }
        if(e[0]==AlRefill && e[9]) {
            CHECK(!e[4] && (e[3]==SpkFading || e[3]==SpkMuting));
            ++tail;
        }
    }
    CHECK(primed==3 && tail>0); /* original raw U remains visible after StopIO */
    printf("PASS epoch starts: cold/warm/rapid restart, silent cancellation, active U=0; raw tail U=%u\n",tail);
}

static void testInputControlsAndWrap(void)
{
    const uint32_t callbacks[] = {64, 128, 256, 512, 1024};
    for (unsigned cb = 0; cb < N(callbacks); ++cb) {
        AudioLiveShared state;
        int16_t out[AUDIO_LIVE_RING * 2], fifo[AUDIO_LIVE_MIC_RING];
        int16_t input[1024], got[1024], copied[1024], hal[PIANO_AUDIO_RING_FRAMES];
        audio_live_shared_init(&state, out, fifo, 12345);
        CHECK(state.input_q15 == 32767 && !state.input_muted);
        audio_live_running(&state, 0, 1);
        CHECK(state.input_runs == 1);
        audio_live_running(&state, 1, 1);
        CHECK(state.input_runs == 1);
        CHECK(audio_live_capture(&state, got, 64) == 0 && state.input_hal.startup == 64);
        state.capturing = 1; state.input_primed = 1; /* controls test: already primed */
        const uint32_t count = callbacks[cb];
        for (uint32_t at = 0; at < 4 * PIANO_AUDIO_RING_FRAMES; at += count) {
            for (uint32_t i = 0; i < count; ++i) input[i] = (int16_t)((at + i) % 30001 - 15000);
            CHECK(audio_ring_write(&state.in, input, count) == count);
            CHECK(audio_live_capture(&state, got, count) == count);
            piano_audio_copy_to_ring(hal, PIANO_AUDIO_RING_FRAMES, at, got, count, 1);
            piano_audio_copy_from_ring(copied, hal, PIANO_AUDIO_RING_FRAMES, at, count, 1);
            CHECK(!memcmp(input, copied, count * sizeof *input));
        }
        CHECK(!state.in.underruns && !state.in.overruns);
        /* Level/mute take effect on data already queued, keep draining, and
         * preserve speaker state. Zero level is real silence, not a default. */
        const int levels[] = {100, 50, 0};
        for (unsigned j = 0; j < N(levels); ++j) {
            for (unsigned i = 0; i < 64; ++i) input[i] = i & 1 ? 32767 : -32768;
            CHECK(audio_ring_write(&state.in, input, 64) == 64);
            audio_live_input_control(&state, levels[j], 0);
            CHECK(audio_live_capture(&state, got, 64) == 64);
            for (unsigned i = 0; i < 64; ++i) CHECK(got[i] == (int16_t)((int32_t)input[i] * piano_audio_input_q15(levels[j]) / 32767));
        }
        CHECK(state.volume_q15 == 12345 && !state.muted);
        audio_live_input_control(&state, 50, 1);
        CHECK(audio_ring_write(&state.in, input, 64) == 64);
        CHECK(audio_live_capture(&state, got, 64) == 64 && !audio_ring_fill(&state.in));
        for (unsigned i = 0; i < 64; ++i) CHECK(!got[i]);
        CHECK(state.input_hal.muted == 64 && !state.input_hal.mute_nonzero);
        audio_live_input_control(&state, 101, 9);
        CHECK(state.input_q15 == piano_audio_input_q15(50) && state.input_muted == 1);
        audio_live_running(&state, 1, 0);
        audio_live_running(&state, 1, 1);
        CHECK(state.input_runs == 2 && state.input_q15 == piano_audio_input_q15(50) && state.input_muted == 1);
        CHECK(!state.input_hal.requested && !state.input_hal.pcm.frames);
        state.capturing = 0; state.input_fault = 1;
        CHECK(!audio_live_capture(&state, got, 64) && state.input_hal.unavailable == 64 && !state.input_hal.startup);
    }
    for (int p = 1; p <= 100; ++p) {
        const double db = 20 * log10(piano_audio_input_q15(p) / 32767.0);
        CHECK(fabs(db - (-36 + .36 * p)) < .015);
    }
}
static void short_mic(uint32_t t)
{
    w.capture_short = 1;
    if (t == 100) device(0, 1);
    if (t == 3100) device(0, 0);
    /* Wait past idle and all release replies before a genuinely new plan. */
    if (t == 7000 && !w.in_running) {
        CHECK(live.mic_state == LiveDown && !w.va_on && !w.va_votes && !w.map_mic);
        CHECK(live.cap.returned_bytes == live.mic_frames * 4 && live.cap.returned_bytes == w.cap_bytes);
        CHECK(live.cap.short_returns && live.cap.zero_returns && !live.cap.errors);
        CHECK(live.mic_pcm[0].peak == 8000 && live.mic_pcm[1].peak == 4000 && live.mic_pcm[2].peak == 6000);
        CHECK(sh.input_hal.missing > 0); /* real short-period deficit is measured */
        w.cap_bytes = 0;
        device(0, 1);
    }
    if (t == 10000) device(0, 0);
}
static void testMicShortRestart(void)
{
    const struct Scenario sc = {10, 0, 0, 0, 14500, AudioPortsUbuntu, 1, short_mic};
    run(&sc);
    CHECK(!w.violations && live.mic_starts == 2 && sh.input_runs == 2 && w.mic_bad == 0);
    CHECK(live.cap.returned_bytes == w.cap_bytes && live.cap.returned_bytes == live.mic_frames * 4);
    CHECK(!live.cap.errors && count_cmd("VA ON", 0) == 2 && count_cmd("VA OFF", 0) == 2);
    CHECK(logged("mic release: run 2 down 1 va 0"));
}

static void mic_clock(uint32_t t)
{
    if (t == 100) device(0, 1);
    if (t == 4000) g_mic_missing = sh.in.underruns;
    if (t == 59000 && w.in_running) {
        fprintf(stderr, "mic clock: ppm=%+.0f missing=%llu->%llu drops=%u repeats=%u fill=%u avg=%d target=%d\n",
                w.dsp_ppm, (unsigned long long)g_mic_missing, (unsigned long long)sh.in.underruns,
                live.mic_drops, live.mic_repeats, audio_ring_fill(&sh.in), live.mic_drift.avg/64, live.mic_drift.setpoint/64);
        CHECK(sh.in.underruns == g_mic_missing && !sh.in.overruns && !live.mic_full_frames);
        device(0, 0);
    }
    w.dsp_ppm = g_ppm;
}
static void testMicClock(void)
{
    const struct Scenario sc = {10, 0, 0, 3000, 63000, AudioPortsUbuntu, 1, mic_clock};
    g_mic_clock = 1;
    const int ppms[] = {0, -200, 200};
    for (unsigned i = 0; i < N(ppms); ++i) {
        g_ppm = ppms[i];
        run(&sc);
        CHECK(!w.violations && live.mic_starts == 1 && live.cap.in_flight == 0 && live.cap.errors == 0);
    }
    g_ppm = 0; g_mic_clock = 0;
}


// ---- speaker protection (AudioSpkProt.h): SYNTHETIC private files (no ACDB byte, no calibration value of any unit)
static void spk_files(void)
{
    static const struct {uint32_t iid, pid, size, v;} e[] = {
        {0x40ee, 0x11111601, 4, 1}, {0x45c2, 0x0800106a, 4, 1}, {0x4587, 0x08001026, 4, 1}, {0x4023, 0x0800106a, 4, 2},
        {0x4046, 0x11111611, 4, 1}, {0x4046, 0x10001fc2, 8, 0x5a}, {0x4046, 0x10001fb0, 6180, 0xa5}, {0x418e, 0x080014fc, 12, 0x11},
    };
    uint32_t at = 16;
    const uint32_t head[4] = {SPK_PARAMS_MAGIC, 1, N(e), 0};
    memcpy(g_spk_file, head, sizeof head);
    for (unsigned i = 0; i < N(e); ++i) {
        const uint32_t h[3] = {e[i].iid, e[i].pid, e[i].size};
        memcpy(g_spk_file + at, h, sizeof h);
        at += 12;
        for (uint32_t b = 0; b < e[i].size; ++b) g_spk_file[at + b] = e[i].size == 4 ? (uint8_t)(b ? 0 : e[i].v) : (uint8_t)(e[i].v ^ b);
        at += (e[i].size + 3u) & ~3u;
    }
    g_spk_bytes = at;
    memcpy(g_spk_re25, g_re25_q12, 16);
    g_re25_bytes = 16;
    g_spk_files = 1;
}
static void spk_reset(void) {g_spk = g_spk_files = g_live_header_bad = 0; g_battery_uv = 4012000; g_fail = 0; g_drop = 0; g_kind = 0;}

static const char *kProtUp[] = {"PRM REQ 0x202 6144000", "MAP 0x19da00000", "OPEN 0xb0000002", "CFG 0x4023", "CFG 0x4023",
                                "CFG 0x4046 0x10001fb0 oob", "CFG 0x4046 0x10001fa7", "OPEN 0x4001", "CFG 0x6001", "CFG 0x6002", "CFG 0x6004",
                                "OPEN 0xb00000d5", "CFG 0x45c2", "CFG 0x40ee", "PREPARE 0xb00000d5", "START 0xb00000d5",
                                "PREPARE 0xb0000002", "PREPARE 0x4001", "START 0xb0000002", "START 0x4001"};
static const char *kProtDown[] = {"STOP 0x4001", "STOP 0xb0000002", "STOP 0xb00000d5", "CLOSE 0xb00000d5", "CLOSE 0x4001", "CLOSE 0xb0000002",
                                  "UNMAP 0x55", "PRM REL 0x202"};
static unsigned monitor_rounds(unsigned *at)
{
    unsigned rounds = 0;
    while (*at + 1 < w.ncmd && !strcmp(w.cmd[*at], "CFG 0x4046 0x10001fb5") && !strcmp(w.cmd[*at + 1], "GET 0x4046 0x10001fab")) {*at += 2; ++rounds;}
    return rounds;
}
static unsigned find_cmd(const char *line, unsigned from)
{
    for (unsigned i = from; i < w.ncmd; ++i) if (!strcmp(w.cmd[i], line)) return i;
    return w.ncmd;
}

static void testProtection(void)
{
    spk_reset(); spk_files(); g_spk = 1;
    const struct Scenario sc = {9, 231, 1, 0, 12000, AudioPortsUbuntu, 1, burst};
    run(&sc);
    CHECK(!w.violations && live.prot_ready && live.prot_starts == 1 && live.spk_starts == 1 && !live.prot_fallbacks);
    unsigned at = expect(live_from(), kProtUp, N(kProtUp));
    // the monitor while the stream ran (about 5 s): the battery voltage then the live data, every 2 s, the first at once
    const unsigned rounds = monitor_rounds(&at);
    CHECK(rounds >= 2 && rounds <= 4 && w.vbat_sets == rounds && w.live_gets == rounds && w.vbat_uv == 4012000 && live.live_reads == rounds);
    at = expect(at, kProtDown, N(kProtDown));
    CHECK(at == w.ncmd && w.re25_sets == 1 && w.oob_cfgs == 1);
    // the sound played as on the passthrough graph: gain 231 at most, the ceiling, the fades, the amplifiers' order
    CHECK(w.sound_periods > 170 && w.sound_periods < 205 && w.peak <= AUDIO_CEILING_MAX && power_ups_after_session() == 4 && w.unmutes == 4);
    CHECK(logged("PianoSpeakerProtV1: ready: 8 ACDB parameters (6220 bytes)") && logged("PianoSpeakerProtV1: start 1: the stock protection pair"));
    CHECK(logged("PianoSpeakerProtV1: up: SP_RX protects") && logged("PianoSpeakerProtV1 live ch0 slot 0 r0 6.723 ohm (102.0% of Re25) f0 800 Hz q 3 t 31"));
    CHECK(logged("PianoSpeakerProtV1 live ch3 slot 3 r0 7.097 ohm (102.0% of Re25) f0 830 Hz q 6 t 34") && logged("vbat 4012000 uV"));
    CHECK(live.live_data.f0[0] == 800u * 256u && live.live_data.re[3] == 28500u * 102u / 100u);
    CHECK(logged("live: plan speaker-protection: 21 steps") && !logged("live: plan speaker-protection monitor"));   // the monitor is quiet
    // level 10: the Mic alongside, one command at a time over the three plans (the model flags two in flight)
    const struct Scenario sc10 = {10, 0, 0, 0, 10000, AudioPortsUbuntu, 1, both};
    run(&sc10);
    CHECK(!w.violations && live.prot_starts == 1 && live.mic_starts == 1 && w.live_gets >= 1 && w.mic_ok > 48000u * 3);
    // sheng's and SunUEFI's ports
    struct Scenario ports = sc;
    ports.ports = AudioPortsSheng; run(&ports); CHECK(!w.violations && live.prot_starts == 1 && w.live_gets >= 2);
    ports.ports = AudioPortsSun; run(&ports); CHECK(!w.violations && live.prot_starts == 1 && w.live_gets >= 2);
    // no battery voltage from the gauge: none sent, the live data still read
    g_battery_uv = 0;
    run(&sc);
    CHECK(!w.violations && w.vbat_sets == 0 && w.live_gets >= 2 && logged("no battery voltage from PianoPower's gauge") && !live.prot_fallbacks);
    spk_reset();
}

// A protection plan step refused or unanswered: everything it did undone, then the passthrough plan in the same boot, which plays.
static void testProtPlanFallback(void)
{
    for (unsigned k = 0; k < N(kProtUp); ++k)
        for (int drop = 0; drop < 2; ++drop) {
            spk_reset(); spk_files(); g_spk = 1;
            g_fail = kProtUp[k]; g_drop = drop;
            const struct Scenario sc = {9, 0, 1, 0, 9000, AudioPortsUbuntu, 0, steady};
            run(&sc);
            CHECK(!w.violations && live.prot_starts == 1 && live.prot_fallbacks == 1 && live.prot_failed && !logged("the plan failed at"));
            CHECK(logged("PianoSpeakerProtV1: FALLBACK to passthrough") && logged("everything it did is undone"));
            const unsigned open = find_cmd("OPEN 0x4005", live_from());
            CHECK(open < w.ncmd && open > 0 && count_cmd("START 0xb0000002", live_from()) == (k >= N(kProtUp) - 2 ? 1u : 0u));
            expect(open - 1, kSpkUp, N(kSpkUp));
            CHECK(w.sound_periods > 300 && w.power_ups == 4 && live.spk_starts == 2);
        }
    spk_reset();
}

// While the pair runs: a monitor command refused or lost, live data under another header, a refused or late period: the pair
// undone, the passthrough graph back in the same boot; an amplifier fault stays what it was (off for the boot).
static void testProtRunningFallback(void)
{
    static const struct {const char *fail; int drop, kind, header;} cases[] = {
        {"GET 0x4046 0x10001fab", 0, 0, 0}, {"GET 0x4046 0x10001fab", 1, 0, 0}, {"CFG 0x4046 0x10001fb5", 0, 0, 0},
        {"CFG 0x4046 0x10001fb5", 1, 0, 0}, {0, 0, 0, 1}, {0, 0, 1, 0}, {0, 0, 3, 0},
    };
    for (unsigned i = 0; i < N(cases); ++i) {
        spk_reset(); spk_files(); g_spk = 1;
        g_fail = cases[i].fail; g_drop = cases[i].drop; g_kind = cases[i].kind; g_live_header_bad = cases[i].header;
        const struct Scenario sc = {9, 0, 1, 0, 9000, AudioPortsUbuntu, 0, cases[i].kind ? fault : steady};
        run(&sc);
        CHECK(!w.violations && live.prot_starts == 1 && live.prot_fallbacks == 1 && live.prot_failed && live.spk_starts == 2);
        CHECK(!logged("the speaker path is OFF for this boot") || logged("stopped by the owner"));   // only the owner's stop at the end
        CHECK(logged("PianoSpeakerProtV1: FALLBACK to passthrough") && logged("the pair is undone"));
        if (cases[i].fail || cases[i].header) CHECK(logged("PianoSpeakerProtV1: monitor:"));
        if (cases[i].header) CHECK(logged("live data under another header") && logged("the answer's header is not the live data's"));
        if (cases[i].kind == 1) CHECK(logged("WRITE_DONE refused") && logged("a refused period or command"));
        if (cases[i].kind == 3) CHECK(logged("a late period") && live.late_stops == 0);
        // the pair's down order, then the passthrough plan, which plays
        const unsigned open = find_cmd("OPEN 0x4005", live_from());
        CHECK(open < w.ncmd && count_cmd("CLOSE 0xb00000d5", live_from()) == 1 && find_cmd("CLOSE 0xb00000d5", live_from()) < open);
        expect(open - 1, kSpkUp, N(kSpkUp));
        CHECK(w.sound_periods > 250);
    }
    // an amplifier whose status changes on the pair: off for the boot as on the passthrough graph (a safety stop, not the DSP's)
    spk_reset(); spk_files(); g_spk = 1; g_kind = 2;
    const struct Scenario sc = {9, 0, 1, 0, 8000, AudioPortsUbuntu, 0, fault};
    run(&sc);
    CHECK(!w.violations && live.spk_state == LiveOff && !live.prot_fallbacks && logged("stopped by amplifier status") && !any_powered());
    spk_reset();
}

// The private files: missing or mismatched, the speaker plays through the passthrough graph from the start; the boot argument off.
static void testProtFiles(void)
{
    const struct Scenario sc = {9, 0, 1, 0, 6000, AudioPortsUbuntu, 1, burst};
    spk_reset(); g_spk = 1;   // asked for, no files
    run(&sc);
    CHECK(!w.violations && !live.prot_ready && !live.prot_starts && logged("PianoSpeakerProtV1: OFF: not staged (test); the speaker plays"));
    CHECK(logged("PianoSpeakerProtV1: asked for, but not staged (test): the passthrough graph"));
    expect(live_from(), kSpkUp, N(kSpkUp));
    spk_reset(); spk_files(); g_spk = 1; g_spk_file[0] ^= 1;   // not a PSPK file
    run(&sc);
    CHECK(!w.violations && !live.prot_ready && logged("OFF: spk-prot.bin: not a PSPK file") && count_cmd("OPEN 0x4005", live_from()) == 1);
    spk_reset(); spk_files(); g_spk = 1; g_re25_bytes = 15;
    run(&sc);
    CHECK(!w.violations && !live.prot_ready && logged("OFF: spk-re25.bin: missing or not 16 bytes") && count_cmd("OPEN 0x4005", live_from()) == 1);
    spk_reset(); spk_files(); g_spk = 1; g_spk_file[16 + 12 + 4 + 12] = 2;   // the V/I source's lane is not lane 0
    run(&sc);
    CHECK(!w.violations && !live.prot_ready && logged("format or lane that is not the driver's"));
    spk_reset(); spk_files();   // the files there, the boot argument off: ready, not used
    run(&sc);
    CHECK(!w.violations && live.prot_ready && !live.prot_starts && logged("off (piano-audio-spk=0") && count_cmd("OPEN 0x4005", live_from()) == 1);
    CHECK(!logged("PianoSpeakerProtV1: asked for"));
    CHECK(audio_live_spk_select(&sh, 2) == -1 && audio_live_spk_select(&sh, -1) == -1);
    spk_reset();
}

// The run-time switch while the speaker is down: on for the first start, off for the second; after a fallback a new request
// clears it.
static void spk_switch(uint32_t t)
{
    if (t == 200 && !w.out_running) device(1, 0);
    w.amplitude = 16000;
    w.sound_on = (t >= 500 && t < 1500) || (t >= 7000 && t < 8000);
    if (t == 100 && g_mark == 0) {g_mark = 1; CHECK(audio_live_spk_select(&sh, 1) == 0);}
    if (t == 6000 && g_mark == 1) {g_mark = 2; CHECK(live.spk_state == LiveDown && audio_live_spk_select(&sh, g_kind) == 0);}
}
static void testProtSwitch(void)
{
    spk_reset(); spk_files(); g_mark = 0; g_kind = 0;
    const struct Scenario sc = {9, 0, 1, 0, 12000, AudioPortsUbuntu, 1, spk_switch};
    run(&sc);
    CHECK(!w.violations && g_mark == 2 && live.spk_starts == 2 && live.prot_starts == 1);
    CHECK(count_cmd("START 0xb0000002", live_from()) == 1 && count_cmd("START 0x4005", live_from()) == 1);
    CHECK(find_cmd("START 0xb0000002", live_from()) < find_cmd("START 0x4005", live_from()));
    // a fallback at the first start (the speaker graph refused), then a new request: the second start is protected again
    spk_reset(); spk_files(); g_mark = 0; g_kind = 1; g_fail = "OPEN 0xb0000002";
    run(&sc);
    CHECK(!w.violations && g_mark == 2 && live.prot_fallbacks == 1 && live.prot_starts == 2 && !live.prot_failed);
    CHECK(count_cmd("START 0xb0000002", live_from()) == 1 && count_cmd("START 0x4005", live_from()) == 1 && logged("a new request: the fallback"));
    CHECK(find_cmd("START 0x4005", live_from()) < find_cmd("START 0xb0000002", live_from()));
    spk_reset();
}

// Sleep with the pair up: the monitor stops, the pair is undone in order, parked; after the wake the pair comes back.
static unsigned prot_stage;
static void prot_sleep(uint32_t t)
{
    if (!w.out_running) device(1, 0);
    w.sound_on = 1; w.amplitude = 12000;
    if (prot_stage == 0 && t >= 3000 && live.spk_state == LiveRunning) {prot_stage = 1; g_mark = (int)w.ncmd; audio_live_sleep(&live, 1);}
    if (prot_stage == 1 && live.parked) {
        CHECK(audio_live_parked(&live) == 1 && !w.vi_open && !w.vi_started && !w.prot_dev_open && !w.map_spk && !w.clock_on && !any_powered());
        unsigned at = (unsigned)g_mark;
        while (at < w.ncmd && strcmp(w.cmd[at], kProtDown[0])) ++at;   // a monitor command in flight may come first
        CHECK(expect(at, kProtDown, N(kProtDown)) == w.ncmd);
        CHECK(audio_live_rearm(&live) == 0);
        prot_stage = 2;
    }
    if (prot_stage == 2 && live.spk_state == LiveRunning && live.prot_active) prot_stage = 3;
}
static void testProtSleep(void)
{
    spk_reset(); spk_files(); g_spk = 1; prot_stage = 0;
    const struct Scenario sc = {9, 0, 1, 0, 10000, AudioPortsUbuntu, 0, prot_sleep};
    run(&sc);
    CHECK(!w.violations && prot_stage == 3 && live.prot_starts == 2 && !live.prot_fallbacks && !live.sleep_failed);
    spk_reset();
}

int main(void)
{
    preset_bytes = sfsm_build(preset, sizeof preset, 0);
    CHECK(preset_bytes > 100);
    hal_transport_replay();
    CHECK(audio_volume_q15(100) == AUDIO_Q15_ONE && audio_volume_q15(0) == 0 && audio_volume_q15(98) == 32713);
    CHECK(audio_ceiling_db(1) == AUDIO_CEILING_MAX && audio_ceiling_db(20) == 3277 && audio_ceiling_db(0) == AUDIO_CEILING_TEST);
    CHECK(audio_ceiling_db(6) == 16422 && audio_ceiling_db(41) == AUDIO_CEILING_TEST);
    testLevel8(AudioPortsUbuntu);
    testLevel8(AudioPortsSheng);
    testLevel8(AudioPortsSun);
    testRestart();
    testDeviceStopHold();
    testSilentDevice();
    testLevel9();
    testLevel8Caps();
    testGainSwitch();
    testDrift(200);
    testDrift(-200);
    testFaults();
    testPlanFailures();
    testMicClock();
    testMic();
    testMicRoutes();
    testMicRoutePendingAndFailedRelease();
    testInputControlsAndWrap();
    testMicShortRestart();
    testQueueFull();
    testLoadedMonitor();
    testSleepCycles();
    testSleepPendingMap();
    testFailSafeMute();
    testLedgerOverflow();
    testEpochStarts();
    testProtection();
    testProtPlanFallback();
    testProtRunningFallback();
    testProtFiles();
    testProtSwitch();
    testProtSleep();
    printf("PASS live: %u checks\n", checks);
    return 0;
}
