/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Replay the sparse wake-3 ledger's known FIFO boundaries through the real
 * refill code. It does not invent timings for unrecorded successful refills or
 * model DSP hardware lead. live_host separately exercises the entire FSM. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../AudioLive.c"

static unsigned checks;
#define CHECK(x) do {++checks; if (!(x)) {fprintf(stderr,"prime:%d: %s\n",__LINE__,#x); exit(1);}} while (0)
typedef struct {unsigned seq, epoch, mode, out, before, after, want, got, inflight;} Recorded;
#include "prime_ledger.h"
static AudioLiveShared shared;
static AudioLive live;
static AudioLedger events;
static int16_t out[AUDIO_LIVE_RING*2], in[AUDIO_LIVE_MIC_RING], pcm[AUDIO_LIVE_RING*2];
static uint8_t dma[AUDIO_DMA_BYTES];

static void init(void)
{
    memset(&live,0,sizeof live); memset(&events,0,sizeof events);
    audio_live_shared_init(&shared,out,in,AUDIO_Q15_ONE);
    live.sh=&shared; live.ledger=&events; live.cfg.level=9;
    live.ops.dma=dma; live.ops.dma_bytes=sizeof dma;
    live.spk_mode=SpkQuiet;
    CHECK(!audio_chain_init(&live.chain,4));
    for(unsigned n=0;n<AUDIO_LIVE_RING*2;++n) pcm[n]=(n&1)?-1000:1000;
    audio_live_running(&shared,1,0); shared.accepting=1;
    live.ledger_epoch=shared.epoch;
}
static void fill(unsigned n)
{
    unsigned f=audio_ring_fill(&shared.out);
    if(f>n) CHECK(audio_ring_skip(&shared.out,f-n)==f-n);
    else CHECK(audio_ring_write(&shared.out,pcm,n-f)==n-f);
    CHECK(audio_ring_fill(&shared.out)==n);
}
static void boundaries(void)
{
    init();
    shared.have_sound=1; shared.have_pcm=1; shared.sound_at=123;
    shared.out.head=shared.out.tail=UINT32_MAX-100;
    fill(720);
    audio_live_running(&shared,0,0); audio_live_running(&shared,1,0);
    CHECK(!shared.have_sound && !shared.have_pcm && !shared.sound_at);
    const uint32_t boundary=shared.output_head;
    int wake=0;
    CHECK(audio_live_play(&shared,pcm,240,200,&wake)==240);
    AudioDspGuard guard={0}; guard.state=AdgWait;
    live.guard=&guard;
    (void)audio_live_tick(&live,0,201);
    CHECK(shared.out.tail==boundary && audio_ring_fill(&shared.out)==240);
    CHECK(shared.have_pcm && !live.spk_primed); /* current PCM survived owner noticing StartIO late */
    init(); shared.have_sound=1; /* stale history and silent DONE cannot prime */
    fill(AUDIO_LIVE_START_FRAMES); CHECK(!spk_has_prefill(&live));
    fill(0);
    for(unsigned n=1;n<=12;++n) {
        CHECK(audio_live_play(&shared,pcm,240,n*5,&wake)==240);
        fill_period(&live,0,4); /* silent transport keeps running without consuming prefill */
        CHECK(audio_ring_fill(&shared.out)==n*240);
        CHECK(spk_has_prefill(&live)==(n==12));
    }
    CHECK(!live.spk_drift.samples && !live.underruns);
    ++shared.epoch; CHECK(!spk_has_prefill(&live)); /* StartIO during an owner tick cannot use its old arm */
}
static void replay(void)
{
    unsigned epoch=0, credit=0, old_u=0, old_missing=0, tails=0, tail_frames=0, cold=0, warm=0;
    for(unsigned n=0;n<sizeof recorded/sizeof recorded[0];++n) {
        const Recorded *r=&recorded[n];
        if(r->epoch!=epoch) {init(); epoch=r->epoch; credit=0;}
        if(r->out && r->before>2400) {
            ++cold; fill(r->before); shared.have_pcm=1;
            if(r->before<2880) {CHECK(!spk_has_prefill(&live)); fill(2880);}
            CHECK(spk_has_prefill(&live));
            credit=audio_ring_fill(&shared.out)-r->before;
            live.prime_left=AUDIO_LIVE_DEPTH;
        } else if(r->out && !credit) {
            /* Three warm starts each have 1680 producer frames by the old
             * +39 ms short read and 2880 by the old +62/63 ms short read.
             * Their earlier successful reads are included by conservation. */
            ++warm; fill(1680); shared.have_pcm=1;
            CHECK(!spk_has_prefill(&live)); fill(2880); CHECK(spk_has_prefill(&live));
            live.prime_left=4;
            for(unsigned b=3;b;--b) take_period(&live,b);
            CHECK(audio_ring_fill(&shared.out)==1440 && !live.underruns);
            ++n; /* the pair is one deferred audible start, not two new epochs */
            CHECK(n<sizeof recorded/sizeof recorded[0] && recorded[n].epoch==epoch);
            old_u+=2; old_missing+=480;
            continue;
        }
        live.spk_mode=r->mode; live.spk_primed=1; shared.want_out=r->out;
        live.pump.in_flight=r->inflight;
        const unsigned u=live.underruns;
        if(r->out) {
            fill(r->before+credit);
            take_period(&live,AUDIO_LIVE_DEPTH-r->inflight);
            CHECK(live.underruns==u && !live.skipped);
            credit=audio_ring_fill(&shared.out)-r->after;
            old_u+=r->got<r->want; old_missing+=r->want-r->got;
        } else {
            fill(r->before); live.prime_left=0; drift_reset(&live.spk_drift);
            const uint64_t missing=shared.out.underruns;
            take_period(&live,AUDIO_LIVE_DEPTH-r->inflight);
            CHECK(live.underruns==u+1);
            CHECK(shared.out.underruns==r->want-r->got+missing);
            ++tails; tail_frames+=r->want-r->got;
        }
    }
    CHECK(cold==4 && warm==3 && old_u==18 && old_missing==5280);
    CHECK(tails==13 && tail_frames==6000);
    printf("PASS wake-3 sparse replay: cold=%u warm=%u active old=%u/%u corrected=0; raw tails=%u/%u preserved\n",cold,warm,old_u,old_missing,tails,tail_frames);
}
static void stale(void)
{
    for(unsigned b=1;b<=4;++b) {
        init(); fill(4096); live.spk_primed=1; live.spk_mode=SpkPlaying;
        for(unsigned left=b;left;--left) take_period(&live,left);
        CHECK(audio_ring_fill(&shared.out)==960 && !live.underruns && live.skipped==1);
        init(); fill(4096); live.prime_left=4;
        for(unsigned left=4;left;--left) take_period(&live,left);
        CHECK(audio_ring_fill(&shared.out)==960 && !live.underruns && !live.spk_drift.samples);
    }
}
int main(void) {boundaries(); replay(); stale(); printf("PASS prime: %u checks\n",checks); return 0;}
