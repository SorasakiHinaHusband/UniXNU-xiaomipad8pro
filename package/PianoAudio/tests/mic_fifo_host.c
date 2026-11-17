/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. */
/* Transport-boundary replay, not an invented DSP/READ refill clock. Including
 * production C lets the test drive the actual static capture_period boundary.
 * Graph/wire/lifecycle remain covered independently by live_host. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../AudioLive.c"
#include "mic_fifo_run1.h"

static unsigned checks;
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);} ++checks;} while (0)
#define N(a) (sizeof(a)/sizeof((a)[0]))
static AudioLive live;
static AudioLiveShared shared;
static int16_t output[AUDIO_LIVE_RING*2], input[AUDIO_LIVE_MIC_RING];
static uint8_t dma[AUDIO_DMA_BYTES];

/* Spread the measured count/byte increments inside each observed one-second
 * window, then bunch at a bounded host poll. No fine timing was captured. */
static double arrival(unsigned k, unsigned *frames)
{
    unsigned prev_count=0, prev_frames=0, prev_ms=0;
    *frames=k==1 ? 144 : 480;
    for (unsigned i=0; i<N(mic_fifo_run1); ++i) {
        const struct MicFifoObservation *b=&mic_fifo_run1[i];
        if (k<=b->returns) {
            /* first-read-ms+1=29 is measured. Spreading that startup gap
             * over the first second would invent a slow producer clock. */
            if (!i) return 28.0+(k-1u)*(b->ms-28.0)/(b->returns-1u);
            if (i && k==b->returns) {
                unsigned deficit=(b->returns-prev_count)*480u-(b->frames-prev_frames);
                if (deficit) *frames=480u-deficit;
            }
            return prev_ms+(double)(k-prev_count)*(b->ms-prev_ms)/(b->returns-prev_count);
        }
        prev_count=b->returns;prev_frames=b->frames;prev_ms=b->ms;
    }
    return prev_ms+(k-prev_count)*10.0;
}

static unsigned missing_events(void)
{
    unsigned events=0,last=0;
    for (unsigned i=0;i<N(mic_fifo_run1);++i) {
        const struct MicFifoObservation *b=&mic_fifo_run1[i];
        if (b->missing!=last) {++events;last=b->missing;}
        CHECK(b->drops>=b->repeats); /* saved accounting, not a replay target */
    }
    CHECK(last==667);
    return events;
}

static void capacity_boundary(void)
{
    memset(&live,0,sizeof live);
    audio_live_shared_init(&shared,output,input,AUDIO_Q15_ONE);
    live.sh=&shared;live.ops.dma=dma;
    ArAnswer a={0};a.data_bytes=480*4;
    /* Above the old mean-relative cutoff, but the actual return fits. */
    shared.in.head=AUDIO_LIVE_MIC_RING-481u;
    capture_period(&live,&a,200);
    CHECK(!live.mic_full && !shared.in.overruns && audio_ring_fill(&shared.in)==AUDIO_LIVE_MIC_RING-1u);
    /* Real capacity exhaustion still discards/counts the whole return. */
    shared.want_in=1;
    capture_period(&live,&a,210);
    CHECK(live.mic_full==1 && live.mic_full_frames==480 && !shared.in.overruns);
}

static void replay(unsigned batch, unsigned phase, int ppm)
{
    memset(&live,0,sizeof live);
    audio_live_shared_init(&shared,output,input,AUDIO_Q15_ONE);
    audio_live_running(&shared,0,1);
    shared.capturing=1;
    live.sh=&shared;live.ops.dma=dma;
    /* The same two physical channel values make real delivered/repeated
     * samples testable; mute/gain are separately tested by live_host. */
    for (unsigned i=0;i<AUDIO_PERIOD_FRAMES;++i) {
        dma[AUDIO_DMA_MIC+4*i]=0x40;dma[AUDIO_DMA_MIC+4*i+1]=0x1f;
        dma[AUDIO_DMA_MIC+4*i+2]=0xa0;dma[AUDIO_DMA_MIC+4*i+3]=0x0f;
    }
    unsigned k=1,frames=0,max_batch=0,peak_fill=0;double due=arrival(k,&frames),next_read=0;
    uint64_t replay_frames=0;
    for (unsigned ms=0;ms<60000;++ms) {
        if ((ms+phase)%batch==0) {
            unsigned count=0;
            while (due<=ms) {
                ArAnswer a={0};a.data_bytes=frames*4;
                capture_period(&live,&a,ms);
                if (audio_ring_fill(&shared.in)>peak_fill) peak_fill=audio_ring_fill(&shared.in);
                replay_frames+=frames;count++;
                for (unsigned i=0;i<N(mic_fifo_run1);++i)
                    if (k==mic_fifo_run1[i].returns) CHECK(replay_frames==mic_fifo_run1[i].frames);
                due=arrival(++k,&frames);
            }
            if (count>max_batch) max_batch=count;
        }
        if (ms>=next_read) {
            int16_t samples[240];
            const unsigned n=audio_live_capture(&shared,samples,240);
            if (shared.input_hal.missing && shared.input_hal.missing==240u-n)
                fprintf(stderr,"first deficit: ms=%u count=%u frames=%llu avg=%d target=%d fill=%u\n",
                        ms,k-1,(unsigned long long)replay_frames,live.mic_drift.avg/64,
                        live.mic_drift.setpoint/64,audio_ring_fill(&shared.in));
            if (n) {CHECK(samples[0]==0 || samples[0]==6000);CHECK(samples[n-1]==0 || samples[n-1]==6000);}
            next_read+=5.0/(1.0+ppm/1000000.0);
        }
    }
    CHECK(max_batch<=AUDIO_LIVE_MIC_DEPTH);
    fprintf(stderr,"mic FIFO replay: batch=%u phase=%u consumer-ppm=%d missing=%llu overrun=%llu full=%llu drop=%u repeat=%u peak=%u\n",
            batch,phase,ppm,(unsigned long long)shared.in.underruns,(unsigned long long)shared.in.overruns,
            (unsigned long long)live.mic_full_frames,live.mic_drops,live.mic_repeats,peak_fill);
    CHECK(shared.input_primed && live.mic_frames==replay_frames);
    CHECK(!shared.in.underruns && !shared.in.overruns && !live.mic_full_frames);
    CHECK(shared.input_hal.requested==shared.input_hal.delivered+shared.input_hal.startup+shared.input_hal.missing);
}

int main(void)
{
    CHECK(missing_events()==5);
    capacity_boundary();
    const unsigned batches[]={5,10,20,25,35};
    for (unsigned i=0;i<N(batches);++i)
        for (unsigned phase=0;phase<batches[i];++phase)
            for (int ppm=-200;ppm<=200;ppm+=200) replay(batches[i],phase,ppm);
    CHECK(AUDIO_LIVE_MIC_TARGET-AUDIO_LIVE_SLIP_BAND-AUDIO_LIVE_MIC_BATCH_BIAS>=AUDIO_LIVE_MIC_RESERVE);
    CHECK(AUDIO_LIVE_MIC_TARGET+4u*AUDIO_PERIOD_FRAMES+1u<=AUDIO_LIVE_MIC_RING);
    CHECK(AUDIO_LIVE_MIC_TARGET+AUDIO_LIVE_MIC_DEPTH*AUDIO_PERIOD_FRAMES+
          AUDIO_LIVE_SLIP_BAND+PIANO_AUDIO_LEGACY_PERIOD_FRAMES<=AUDIO_LIVE_MIC_RING);
    CHECK(AUDIO_LIVE_INPUT_LATENCY+PIANO_AUDIO_ENGINE_LATENCY_FRAMES==2640u);
    printf("PASS mic FIFO: %u checks; recorded counts/bytes, 285 phase/rate cases\n",checks);
    return 0;
}
