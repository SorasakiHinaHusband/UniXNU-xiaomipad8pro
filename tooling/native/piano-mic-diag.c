/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Copyright (c) 2026 Leo Zhang.
 * Explicit root-only PianoAudio engineering diagnostic, not a HAL recorder.
 * Uses the published mic-delivery1 ABI; never requests or changes TCC access.
 * Build: clang -Wall -Wextra -Werror -O2 piano-mic-diag.c -framework IOKit
 *        -framework CoreFoundation -o piano-mic-diag
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../package/PianoAudio/AudioTiming.h"

enum { RATE=48000, RING=7680, BLOCK=480, LAG=3840 };
typedef struct { uint64_t sample, host, seed, reserved, end; } Status;
static volatile sig_atomic_t interrupted;
static void stop_signal(int sig) { (void)sig; interrupted=1; }
static uint64_t load64(const uint64_t *p) { return __atomic_load_n(p,__ATOMIC_ACQUIRE); }
static mach_timebase_info_data_t tb;
static uint64_t ns(uint64_t ticks) { return (uint64_t)((__uint128_t)ticks*tb.numer/tb.denom); }

/* The engine publishes a completed wrap, not a per-period producer cursor.
 * Read 80 ms behind its clock, keep 20 ms of overwrite headroom, and reject
 * stale/moved anchors. Also compare every block again 2 ms later, before
 * it can be overwritten. Kernel late/FIFO diagnostics are a separate gate:
 * clock interpolation alone is NOT a proof that the producer met deadlines. */
static int readable(uint64_t clock, uint64_t anchor, uint64_t next)
{
    if (anchor>clock || clock-anchor>RING+960) return -1;
    if (clock<next+BLOCK+LAG) return 0;
    if (clock-next>RING-960) return -1;
    return 1;
}
static int selftest(void)
{
    int16_t ring[RING], block[BLOCK];
    uint64_t next=0, producer=0;
    for (uint64_t clock=0;clock<=5u*RATE+LAG;clock+=96) {
        /* A real 5-ms producer, with deliberately nonuniform reader polls. */
        uint64_t complete=clock/240*240;
        for (;producer<complete;++producer) ring[producer%RING]=(int16_t)(producer*13+7);
        while (readable(clock,complete/RING*RING,next)==1) {
            piano_audio_copy_from_ring(block,ring,RING,(uint32_t)(next%RING),BLOCK,1);
            for (unsigned j=0;j<BLOCK;++j) if (block[j]!=(int16_t)((next+j)*13+7)) return 1;
            next+=BLOCK;
        }
    }
    if (next!=5u*RATE || readable(20000,0,0)!=-1 || readable(1000,2000,0)!=-1 || readable(2000,0,0)!=0) return 1;
    puts("SELFTEST_PASS chronological 5s PCM, ring wraps, stale anchor, overrun, future anchor, early read");
    return 0;
}
static CFTypeRef prop(io_service_t s,CFStringRef key)
{ return IORegistryEntryCreateCFProperty(s,key,kCFAllocatorDefault,0); }
static int number(io_service_t s,CFStringRef key,int *v)
{
    CFTypeRef p=prop(s,key); int ok=p && CFGetTypeID(p)==CFNumberGetTypeID() && CFNumberGetValue(p,kCFNumberIntType,v);
    if (p) CFRelease(p);
    return ok;
}
static int equal(io_service_t s,CFStringRef key,CFStringRef value)
{ CFTypeRef p=prop(s,key); int ok=p && CFEqual(p,value); if (p) CFRelease(p); return ok; }
static int boolean(io_service_t s,CFStringRef key,int *v)
{
    CFTypeRef p=prop(s,key); int ok=p && CFGetTypeID(p)==CFBooleanGetTypeID();
    if (ok) *v=CFBooleanGetValue(p);
    if (p) CFRelease(p);
    return ok;
}
static io_service_t find_mic(void)
{
    io_iterator_t it=0; io_service_t found=0,s; unsigned matches=0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("PianoAudioDevice"),&it)) return 0;
    while ((s=IOIteratorNext(it))) {
        if (equal(s,CFSTR("device UID"),CFSTR("Mic")) &&
            equal(s,CFSTR("piano-audio-input-contract"),CFSTR("mic-delivery1:inpt/main:mono:db-linear:pre-HAL"))) {
            ++matches; if (!found) {found=s; continue;}
        }
        IOObjectRelease(s);
    }
    IOObjectRelease(it);
    if (matches!=1) { if (found) IOObjectRelease(found); return 0; }
    return found;
}
static int control(io_connect_t c,io_service_t s,int level,int mute)
{
    uint64_t args[2]={1,(uint64_t)level};
    kern_return_t a=IOConnectCallScalarMethod(c,2,args,2,NULL,NULL);
    args[0]=2; args[1]=(uint64_t)mute;
    kern_return_t b=IOConnectCallScalarMethod(c,2,args,2,NULL,NULL);
    int l=-1,m=-1;
    int ok=number(s,CFSTR("piano-audio-input-level"),&l) && boolean(s,CFSTR("piano-audio-input-mute"),&m);
    printf("CONTROL level=%d mute=%d level_rc=0x%x mute_rc=0x%x read_level=%d read_mute=%d %s\n",
           level,mute,a,b,l,m,!a&&!b&&ok&&l==level&&m==mute?"PASS":"FAIL");
    return a||b||!ok||l!=level||m!=mute;
}
static void le16(unsigned char *p,uint16_t v) {p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);}
static void le32(unsigned char *p,uint32_t v) {for (unsigned i=0;i<4;++i)p[i]=(unsigned char)(v>>(i*8));}
static int wav_header(FILE *f,uint32_t bytes)
{
    unsigned char h[44]={0};
    memcpy(h,"RIFF",4); le32(h+4,bytes+36); memcpy(h+8,"WAVEfmt ",8); le32(h+16,16);
    le16(h+20,1);le16(h+22,1);le32(h+24,RATE);le32(h+28,RATE*2);le16(h+32,2);le16(h+34,16);
    memcpy(h+36,"data",4);le32(h+40,bytes);
    return fseek(f,0,SEEK_SET) || fwrite(h,1,sizeof h,f)!=sizeof h;
}
static int capture(io_connect_t c,io_service_t service,const char *prefix,unsigned seconds)
{
    char wav[1024],ready[1024],stop[1024],done[1024];
    if (snprintf(wav,sizeof wav,"%s.wav",prefix)>=(int)sizeof wav ||
        snprintf(ready,sizeof ready,"%s.ready",prefix)>=(int)sizeof ready ||
        snprintf(stop,sizeof stop,"%s.stop",prefix)>=(int)sizeof stop ||
        snprintf(done,sizeof done,"%s.done",prefix)>=(int)sizeof done) return 1;
    int running=1,frames=0;
    if (!boolean(service,CFSTR("is running"),&running) || running ||
        !number(service,CFSTR("io buffer frame size"),&frames) || frames!=RING ||
        access(ready,F_OK)==0 || access(stop,F_OK)==0 || access(done,F_OK)==0) {
        puts("HARNESS_FAIL nonidle engine, ring ABI, or reused prefix"); return 1;
    }
    FILE *f=fopen(wav,"wbx"); if (!f) {perror("exclusive wav");return 1;}
    int bad=wav_header(f,0),started=0;
    mach_vm_address_t sa=0,ra=0; mach_vm_size_t sl=0,rl=0;
    uint64_t next=0,seed=0,previous_clock=0,blocks=0,anchors=0,last_anchor=0;
    uint64_t start=mach_absolute_time(),origin=0;
    if (!bad && IOConnectMapMemory64(c,0,mach_task_self(),&sa,&sl,kIOMapAnywhere|kIOMapReadOnly)) bad=1;
    if (!bad && IOConnectMapMemory64(c,0x10000001,mach_task_self(),&ra,&rl,kIOMapAnywhere|kIOMapReadOnly)) bad=1;
    if (!bad && (sl<sizeof(Status) || rl<RING*2)) bad=1;
    if (!bad) {bad=IOConnectCallScalarMethod(c,0,NULL,0,NULL,NULL)!=0;started=!bad;}
    const Status *s=(const Status *)(uintptr_t)sa;
    const int16_t *ring=(const int16_t *)(uintptr_t)ra;
    int16_t block[BLOCK],again[BLOCK];
    if (!bad) {seed=load64(&s->seed);origin=load64(&s->host);if (!seed||!origin||load64(&s->sample))bad=1;}
    int announced=0,stopped=0;
    while (!bad && !interrupted && ns(mach_absolute_time()-start)<(uint64_t)seconds*1000000000) {
        if (access(stop,F_OK)==0) {stopped=1;break;}
        uint64_t sample=load64(&s->sample),host=load64(&s->host),now=mach_absolute_time();
        if (host>now || sample%RING || load64(&s->seed)!=seed) {bad=1;break;}
        uint64_t clock=ns(now-origin)*RATE/1000000000;
        /* host is stored before sample; a reader may straddle that update.
         * Skip a torn pair rather than misclassifying it as PCM corruption. */
        uint64_t expected=ns(host-origin)*RATE/1000000000;
        if (expected+2<sample || expected>sample+2) {usleep(1000);continue;}
        if (sample!=last_anchor) {++anchors;last_anchor=sample;}
        if (previous_clock && clock-previous_clock>2400) {bad=1;break;}
        previous_clock=clock;
        int state=readable(clock,sample,next);
        if (state<0) {bad=1;break;}
        if (state==1) {
            piano_audio_copy_from_ring(block,ring,RING,(uint32_t)(next%RING),BLOCK,1);
            usleep(2000);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            uint64_t check=ns(mach_absolute_time()-origin)*RATE/1000000000;
            if (check-next>RING-960 || load64(&s->seed)!=seed) {bad=1;break;}
            piano_audio_copy_from_ring(again,ring,RING,(uint32_t)(next%RING),BLOCK,1);
            if (memcmp(block,again,sizeof block) || fwrite(block,1,sizeof block,f)!=sizeof block) {bad=1;break;}
            next+=BLOCK;++blocks;
            if (!announced && next>=2u*RATE) {
                FILE *r=fopen(ready,"wx");if (!r) {bad=1;break;}
                fprintf(r,"READY seed=%llu frames=%llu root=1 direct-driver=1\n",(unsigned long long)seed,(unsigned long long)next);
                if (fclose(r)) {bad=1;break;} announced=1;
            }
        } else usleep(1000);
    }
    if (!stopped || interrupted || !announced) bad=1; /* timeout is not a requested stop */
    kern_return_t sr=started?IOConnectCallScalarMethod(c,1,NULL,0,NULL,NULL):KERN_SUCCESS;
    if (sr) bad=1;
    kern_return_t ru=ra?IOConnectUnmapMemory64(c,0x10000001,mach_task_self(),ra):KERN_SUCCESS;
    kern_return_t su=sa?IOConnectUnmapMemory64(c,0,mach_task_self(),sa):KERN_SUCCESS;
    if (ru||su||wav_header(f,(uint32_t)(next*2))) bad=1;
    if (fclose(f)) bad=1;
    printf("CAPTURE %s seed=%llu frames=%llu blocks=%llu anchors=%llu stop_rc=0x%x unmap_ring=0x%x unmap_status=0x%x\n",
        bad?"FAIL":"PASS",(unsigned long long)seed,(unsigned long long)next,(unsigned long long)blocks,(unsigned long long)anchors,sr,ru,su);
    if (!bad) {FILE *d=fopen(done,"wx");if (!d)return 1;fprintf(d,"CAPTURE_DONE frames=%llu seed=%llu\n",(unsigned long long)next,(unsigned long long)seed);if(fclose(d))return 1;}
    return bad;
}
int main(int argc,char **argv)
{
    if (argc==2 && !strcmp(argv[1],"--selftest")) return selftest();
    int ctl=argc==4&&!strcmp(argv[1],"--control"),rec=argc==4&&!strcmp(argv[1],"--capture");
    if ((!ctl&&!rec)||geteuid()!=0) {fputs("usage (root): --control LEVEL MUTE | --capture UNIQUE_PREFIX MAX_SECONDS; --selftest needs no device\n",stderr);return 2;}
    umask(0077);
    char *end=NULL;unsigned seconds=0;int level=0,mute=0;
    if (ctl) {
        long v=strtol(argv[2],&end,10);if (*end||v<0||v>100)return 2;level=(int)v;
        v=strtol(argv[3],&end,10);if (*end||v<0||v>1)return 2;mute=(int)v;
    } else {long v=strtol(argv[3],&end,10);if (*end||v<5||v>180)return 2;seconds=(unsigned)v;}
    io_service_t s=find_mic();io_connect_t c=0;
    if (!s) {puts("HARNESS_FAIL unique Mic/mic-delivery1 contract missing");return 2;}
    kern_return_t kr=IOServiceOpen(s,mach_task_self(),0,&c);
    if (kr) {printf("HARNESS_FAIL IOServiceOpen 0x%x\n",kr);IOObjectRelease(s);return 2;}
    signal(SIGINT,stop_signal);signal(SIGTERM,stop_signal);signal(SIGHUP,stop_signal);
    mach_timebase_info(&tb);
    int rc=ctl?control(c,s,level,mute):capture(c,s,argv[2],seconds);
    kr=IOServiceClose(c);if (kr)rc=1;IOObjectRelease(s);
    printf("CONNECTION_CLOSED rc=0x%x result=%d\n",kr,rc);
    return rc;
}
